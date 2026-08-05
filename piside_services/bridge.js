#!/usr/bin/env node
// rownd-pendant-bridge — Pi-side bridge between M5Dial pendant and Rownd's embedded cncjs.
//
// Architecture (verified 2026-04-25):
//   M5Dial /dev/ttyACM0 <-> bridge <-> http://127.0.0.1:<dynamic>/socket.io <-> cncjs <-> ESP32
//
// The bridge connects as a JWT-authenticated peer client of Rownd's own cncjs server,
// alongside the running Rownd Electron renderer. cncjs's command queue arbitrates writes
// from both clients. No XDG toggle, no serial contention.
//
// Today's mode: pendant interface is stubbed as stdin/stdout — type a Grbl command,
// get the controller response. When the M5Dial arrives, replace `attachPendantStub()`
// with a `serialport`-based reader/writer for /dev/ttyACM0.

'use strict';

const fs = require('fs');
const http = require('http');
const { execSync } = require('child_process');
const readline = require('readline');
const jwt = require('jsonwebtoken');
const io = require('socket.io-client');

// ── config ──────────────────────────────────────────────────────────────────
const CNCRC_PATH = process.env.CNCRC_PATH || '/home/rownd/.cncrc';
const TOKEN_TTL = '30d';
const RECONNECT_DELAY_MS = 3000;
const PORT_PROBE_TIMEOUT_MS = 2000;

const log = (...args) => console.error(`[${new Date().toISOString()}]`, ...args);

// ── auth ────────────────────────────────────────────────────────────────────
function readSecret() {
  let raw;
  try {
    raw = fs.readFileSync(CNCRC_PATH, 'utf8');
  } catch (e) {
    if (e.code === 'EACCES') {
      throw new Error(
        `Cannot read ${CNCRC_PATH} (EACCES). Bridge must run as user 'rownd' (parent dir is mode 700).`
      );
    }
    throw e;
  }
  const parsed = JSON.parse(raw);
  if (!parsed.secret) throw new Error(`No .secret key in ${CNCRC_PATH}`);
  return parsed.secret;
}

function mintToken(secret) {
  return jwt.sign({ id: '', name: 'm5dial-pendant' }, secret, { expiresIn: TOKEN_TTL });
}

// ── cncjs port discovery ────────────────────────────────────────────────────
// cncjs binds to a different localhost port on every Rownd-app startup.
// Strategy: list 127.0.0.1 listeners, try socket.io handshake on each, return first OK.
function listLocalPorts() {
  const out = execSync('ss -tln', { encoding: 'utf8' });
  const ports = new Set();
  for (const m of out.matchAll(/127\.0\.0\.1:(\d+)/g)) ports.add(m[1]);
  return [...ports];
}

async function probePort(port, token) {
  return new Promise((resolve) => {
    const sock = io(`http://127.0.0.1:${port}`, {
      query: `token=${token}`,
      reconnection: false,
      timeout: PORT_PROBE_TIMEOUT_MS,
      transports: ['websocket', 'polling'],
    });
    let settled = false;
    const finish = (ok) => {
      if (settled) return;
      settled = true;
      try { sock.close(); } catch (_) { /* ignore */ }
      resolve(ok);
    };
    sock.on('connect', () => finish(true));
    sock.on('connect_error', () => finish(false));
    setTimeout(() => finish(false), PORT_PROBE_TIMEOUT_MS + 500);
  });
}

async function findCncjsPort(token, excludePort) {
  // Filter our own panel port out so the bridge doesn't keep WebSocket-
  // handshaking against itself and spamming "panel: unknown cmd" in the log.
  const candidates = listLocalPorts().filter((p) => parseInt(p, 10) !== excludePort);
  log(`probing ${candidates.length} candidate ports: ${candidates.join(', ')}`);
  for (const p of candidates) {
    if (await probePort(p, token)) {
      log(`found cncjs on 127.0.0.1:${p}`);
      return p;
    }
  }
  throw new Error('no responding cncjs found on any local port');
}

// ── cncjs HTTP API client (for /api/watch/files) ────────────────────────────
function httpGetJson(port, token, path) {
  return new Promise((resolve, reject) => {
    const url = `http://127.0.0.1:${port}${path}${path.includes('?') ? '&' : '?'}token=${token}`;
    const req = http.get(url, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => {
        if (res.statusCode >= 400) {
          reject(new Error(`HTTP ${res.statusCode}: ${body.slice(0, 200)}`));
          return;
        }
        try { resolve(JSON.parse(body)); } catch (e) { reject(e); }
      });
    });
    req.on('error', reject);
    req.setTimeout(3000, () => { req.destroy(new Error('http timeout')); });
  });
}

// ── cncjs client ────────────────────────────────────────────────────────────
function connectCncjs(port, token, panelServer) {
  const sock = io(`http://127.0.0.1:${port}`, {
    query: `token=${token}`,
    reconnection: false,
    transports: ['websocket', 'polling'],
  });

  const state = {
    sock,
    httpPort: port,          // for /api/watch/files HTTP calls
    token,                   // JWT for HTTP + socket cmds
    serialPort: null,        // set when serialport:list / serialport:open seen
    controllerState: null,   // last <Idle|Run|Alarm|...> + axes
    workflowState: 'idle',   // cncjs sender workflow: idle|running|paused
    onPendantSend: null,     // write fn the pendant interface installs
    _panelServer: panelServer || null,
    _lastActiveState: null,
  };
  if (panelServer) panelServer.setState(state);

  // Re-query the port list until we see an active (inuse) serial port.
  // cncjs only sends serialport:open to the socket that opened the port —
  // the Rownd renderer — NOT to peer sockets like us, so the one-shot
  // 'list' at connect time misses the port whenever the renderer opens
  // ttyUSB0 after we connect (the normal boot ordering). Polling list is
  // the only ordering-proof way for a peer client to discover the port.
  // Re-emit 'list' until we see an active (inuse) serial port. cncjs only
  // sends serialport:open to the socket that opened the port — the Rownd
  // renderer — NOT to peer sockets like us, so the one-shot 'list' at connect
  // misses the port whenever the renderer opens ttyUSB0 after we connect (the
  // normal boot ordering). Polling is the only ordering-proof way for a peer
  // to discover it. This is what makes lights/pendant come up reliably after
  // a cold boot. (App relaunches that move cncjs to a new port are recovered
  // by restarting this service — the desktop "Relaunch Rownd UI" button and
  // the deploy procedure both do that.)
  let listPollTimer = null;
  const stopListPoll = () => {
    if (listPollTimer) { clearInterval(listPollTimer); listPollTimer = null; }
  };

  sock.on('connect', () => {
    log(`connected sid=${sock.id}`);
    sock.emit('list');
    stopListPoll();
    listPollTimer = setInterval(() => {
      if (state.serialPort) { stopListPoll(); return; }
      sock.emit('list');
    }, 2000);
  });

  sock.on('disconnect', (reason) => {
    log(`disconnected: ${reason}`);
    stopListPoll();
    state.serialPort = null; // force re-discovery on reconnect
  });
  sock.on('connect_error', (e) => log(`connect_error: ${e.message || e}`));

  sock.on('serialport:list', (ports) => {
    const open = (ports || []).find((p) => p.inuse);
    if (open && !state.serialPort) {
      state.serialPort = open.port;
      stopListPoll();
      log('serialport:list:', JSON.stringify(ports));
      log(`active serial port: ${state.serialPort}`);
      // Join the controller's broadcast list as a viewer/peer. cncjs only
      // emits serialport:read, controller:state, etc. to sockets that have
      // emitted 'open' on the port. Rownd UI already owns it; cncjs handles
      // a second `open` as "attach as additional viewer", returns the same
      // serialport:open event with inuse:true. Without this, our socket
      // gets serialport:list but nothing else.
      sock.emit('open', state.serialPort, { baudrate: 115200, controllerType: 'Grbl' });
      log(`emitted open(${state.serialPort}) — attaching as viewer/peer`);
      if (panelServer) panelServer.replayVolatile();
    }
  });

  sock.on('serialport:open', (info) => {
    log('serialport:open:', JSON.stringify(info));
    if (!info || !info.port) return;
    const wasUnknown = !state.serialPort;
    state.serialPort = info.port;
    // Race fix (2026-05-18): when the bridge connects to cncjs BEFORE the
    // Rownd renderer opens the serial, the initial serialport:list shows
    // every port as inuse:false and we miss the chance to attach as peer.
    // cncjs then broadcasts serialport:open to all clients once the
    // renderer's open call lands — at that point we emit our own open to
    // get serialport:read events flowing. Skip if we already attached on
    // serialport:list (state.serialPort was set there).
    if (wasUnknown) {
      sock.emit('open', state.serialPort, { baudrate: 115200, controllerType: 'Grbl' });
      log(`emitted open(${state.serialPort}) on late serialport:open — attaching as viewer/peer`);
      if (panelServer) panelServer.replayVolatile();
    }
  });

  sock.on('serialport:close', (info) => {
    log('serialport:close:', JSON.stringify(info));
  });

  sock.on('serialport:read', (data) => {
    // forward controller -> pendant
    log(`cncjs>pendant ${typeof data === 'string' ? data.length : '?'}B: ${JSON.stringify(data)}`);
    if (state.onPendantSend) state.onPendantSend(data);
  });

  sock.on('serialport:write', (data, ctx) => {
    if (ctx && ctx.source === 'm5dial') return; // skip our own echo
    log(`serialport:write [src=${ctx && ctx.source || '?'}]: ${JSON.stringify(data)}`);
  });

  sock.on('controller:state', (type, controllerState) => {
    state.controllerState = controllerState;
    // raw status frames come via serialport:read; this is the parsed object.
    // Surface the parser-state label (Idle/Run/Alarm/Hold/...) to the panel
    // daemon so its LED state machine can react.
    const active = controllerState && controllerState.status && controllerState.status.activeState;
    if (active && state._panelServer && active !== state._lastActiveState) {
      state._lastActiveState = active;
      state._panelServer.broadcastState(active);
    }
  });

  sock.on('feeder:status', (s) => {
    if (s.queue && s.queue.length) log(`feeder: ${s.queue.length} queued`);
  });

  sock.on('sender:status', () => { /* ignored unless running a job */ });

  // cncjs sender workflow — THE signal for "a streamed program is active".
  // Plain jogging leaves this 'idle' (jog is not a gcode:start job), so gating
  // pendant LINE input on this never blocks legitimate jogging — it only blocks
  // jog/cmd lines while a program streams, which otherwise steal an `ok` from
  // the sender's char-counting and stall it ("stopped and waited", 2026-06-29:
  // accidental dpad during automatic run). Signature has been unreliable
  // (sometimes (state), sometimes (type, state)) — pick the first run-y string.
  sock.on('workflow:state', (...args) => {
    const ws = args.find((a) => typeof a === 'string' && /^(idle|running|paused)$/.test(a));
    if (ws && ws !== state.workflowState) {
      state.workflowState = ws;
      log(`workflow:state -> ${ws}`);
    }
  });

  // Public API — pendant interface uses these
  state.sendRaw = (data) => {
    if (!state.serialPort) {
      log('cannot send — no active serial port yet');
      return false;
    }
    sock.emit('write', state.serialPort, data);
    return true;
  };

  state.sendLine = (line) => {
    if (!state.serialPort) {
      log('cannot send — no active serial port yet');
      return false;
    }
    sock.emit('writeln', state.serialPort, line);
    return true;
  };

  return state;
}

// ── out-of-band bridge protocol — `@`-prefix lines ──────────────────────────
// Pendant->bridge: any line starting with `@` is a bridge command, NOT
// forwarded to the controller. Used for the file-list / load / start path
// (cncjs `watchDirectory`). Bridge replies on the controller-echo channel
// with `@FILES <json>`, `@LOADED <name>`, or `@ERR <msg>` — pendant's
// GrblParserC ignores any line starting with `@` and routes to the bridge
// message handler instead.
//
// Why `@`: not a Grbl realtime byte (those are 0x18, 0x21, 0x3F, 0x7E,
// 0x85, 0x90..0x9F) and not a Grbl line-prefix (`$`, `<`, alpha, `#`).
// Controller never sees a line starting with `@`.
async function handleBridgeCommand(state, line, replyToPendant) {
  // line is the full `@CMD args` string with no trailing \n.
  log(`bridge cmd: ${line}`);
  const space = line.indexOf(' ');
  const cmd = (space < 0 ? line : line.slice(0, space)).toUpperCase();
  const args = space < 0 ? '' : line.slice(space + 1).trim();

  const reply = (s) => {
    if (replyToPendant) replyToPendant(s + '\n');
  };

  try {
    if (cmd === '@LIST') {
      const r = await httpGetJson(state.httpPort, state.token, '/api/watch/files?path=/');
      // Condense to {n,s,m} per file; filter to regular files only.
      const files = (r.files || [])
        .filter((f) => f.type === 'f')
        .map((f) => ({ n: f.name, s: f.size, m: f.mtime }));
      reply(`@FILES ${JSON.stringify(files)}`);
      return;
    }
    if (cmd === '@RST') {
      // Pendant reports esp_reset_reason() of its PREVIOUS boot on every
      // startup, so a silent crash (blank screen, no serial close) is
      // diagnosable from this log with zero added hardware. Decode the enum.
      const REASONS = ['UNKNOWN', 'POWERON', 'EXT', 'SW', 'PANIC', 'INT_WDT',
        'TASK_WDT', 'WDT', 'DEEPSLEEP', 'BROWNOUT', 'SDIO'];
      const n = parseInt(args, 10);
      log(`*** PENDANT RESET REASON: ${n} (${REASONS[n] || '?'}) ***`);
      return;
    }
    // Bridge commands that target the controller's command queue need
    // state.serialPort, which is set asynchronously after `serialport:list`
    // arrives post-connect. Wait briefly so a pendant that fires @LOAD
    // immediately on first connect doesn't see a spurious @ERR.
    const awaitSerialPort = async (ms = 1500) => {
      const deadline = Date.now() + ms;
      while (!state.serialPort && Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 50));
      }
      return !!state.serialPort;
    };

    if (cmd === '@LOAD') {
      if (!args) { reply('@ERR LOAD requires filename'); return; }
      if (!(await awaitSerialPort())) { reply('@ERR no active serial port'); return; }
      state.sock.emit('command', state.serialPort, 'watchdir:load', args);
      reply(`@LOADED ${args}`);
      return;
    }
    if (cmd === '@START') {
      if (!(await awaitSerialPort())) { reply('@ERR no active serial port'); return; }
      state.sock.emit('command', state.serialPort, 'gcode:start');
      reply('@OK START');
      return;
    }
    if (cmd === '@STOP') {
      if (!(await awaitSerialPort())) { reply('@ERR no active serial port'); return; }
      state.sock.emit('command', state.serialPort, 'gcode:stop', { force: true });
      reply('@OK STOP');
      return;
    }
    if (cmd === '@PAUSE') {
      if (!(await awaitSerialPort())) { reply('@ERR no active serial port'); return; }
      state.sock.emit('command', state.serialPort, 'gcode:pause');
      reply('@OK PAUSE');
      return;
    }
    reply(`@ERR unknown cmd ${cmd}`);
  } catch (e) {
    reply(`@ERR ${e.message || e}`);
  }
}

// Split an incoming pendant byte stream into (a) `@`-prefixed bridge command
// lines (handed to handleBridgeCommand) and (b) the residual byte sequence
// to forward to cncjs as-is.
//
// State machine: a line buffer that switches into "@-mode" the moment a `@`
// arrives at line-start (the line-start tracker is `atLineStart`). In
// @-mode, bytes are buffered until `\n`. Outside @-mode, bytes pass through
// to the forward queue verbatim — preserving realtime single-byte semantics
// (0x18 reset, 0x3F status, 0x90..0x9F overrides) and the existing line-
// terminated $J= / S<rpm> etc. behavior.
// Grbl/FluidNC realtime bytes — interpreted out-of-band, never line content:
//   0x18 reset, 0x21 (!) hold, 0x3F (?) status, 0x7E (~) start,
//   0x84-0x85 door/jogcancel, 0x90-0x9F overrides.
// They don't consume an `ok` from the streamer's ok-accounting, so they are
// always safe to forward — including mid-run.
function isRealtimeByte(ch) {
  const c = ch.charCodeAt(0);
  return c === 0x18 || c === 0x21 || c === 0x3F || c === 0x7E ||
         c === 0x84 || c === 0x85 || (c >= 0x90 && c <= 0x9F);
}

// Keep ONLY realtime single-byte commands, drop all line content. Used to gate
// pendant input during a streamed program run (see attachPendantSerial).
function realtimeOnly(str) {
  let out = '';
  for (let i = 0; i < str.length; i++) if (isRealtimeByte(str[i])) out += str[i];
  return out;
}

function makeBridgeLineSplitter() {
  // These don't break "at line start" — a `?` between two complete lines
  // shouldn't make the next byte mid-line.
  const isRealtime = isRealtimeByte;
  let inBridgeLine = false;
  let bridgeBuf = '';
  let atLineStart = true;
  return function split(chunkStr) {
    const forward = [];
    const bridgeLines = [];
    for (let i = 0; i < chunkStr.length; i++) {
      const ch = chunkStr[i];
      if (inBridgeLine) {
        if (ch === '\n') {
          bridgeLines.push(bridgeBuf);
          bridgeBuf = '';
          inBridgeLine = false;
          atLineStart = true;
        } else if (ch !== '\r') {
          bridgeBuf += ch;
        }
        continue;
      }
      if (atLineStart && ch === '@') {
        inBridgeLine = true;
        bridgeBuf = '@';
        atLineStart = false;
        continue;
      }
      forward.push(ch);
      // Realtime bytes don't constitute line content; preserve atLineStart.
      if (!isRealtime(ch)) atLineStart = (ch === '\n');
    }
    return { forward: forward.join(''), bridgeLines };
  };
}

// ── pendant interfaces ──────────────────────────────────────────────────────
// Three modes:
//   1. attachPendantSerial — full bridge, /dev/ttyACM* <-> cncjs (production)
//   2. attachPendantStub   — stdin replacement (manual testing, no real pendant)
//   3. passthroughMode     — log-only sanity check, no cncjs (serial dry-run)
//
// The pendant emits raw Grbl protocol bytes including single-byte realtime
// commands (0x18 reset, 0x3F status, 0x21/0x7E hold/start, 0x85 jog cancel,
// 0x90..0x9F overrides) intermixed with newline-terminated $J=, S<rpm>, etc.
// cncjs's `write` event accepts raw strings verbatim and forwards to the
// serial port — same wire format as the ESP32 expects, no transformation.
//
// Pendant lines starting with `@` are intercepted as bridge commands (file
// list, gcode load/start) and never reach cncjs.
function attachPendantSerial(state, devicePath, baudRate) {
  const { SerialPort } = require('serialport');
  const fs = require('fs');
  const splitter = makeBridgeLineSplitter();
  let dial = null;
  let reconnectTimer = null;

  // HANG WATCHDOG: the `close`-based fail-safe below only fires when the pendant
  // RESETS (USB drops → serial close). It does NOT cover a SILENT HANG — the
  // dial firmware freezes (blank screen) but FreeRTOS keeps the USB-CDC link
  // enumerated, so no `close` event fires and a running $J= coasts to the soft
  // limit with no operator control (observed 2026-06-29: blanked mid hard-jog).
  // The healthy pendant emits a `?` status poll every ~210 ms, so total byte
  // silence past PENDANT_SILENCE_MS means it has hung — cancel any active jog
  // from the Pi side. 0x85 is a no-op when idle and a no-op during a real
  // program run, so firing it on a false positive is harmless.
  const PENDANT_SILENCE_MS = 600;
  let lastPendantByte = Date.now();
  let hangWatchdog = null;
  let watchdogTripped = false;

  const replyToPendant = (s) => {
    try { if (dial && dial.isOpen) dial.write(s); } catch (e) { log(`pendant write fail: ${e.message}`); }
  };

  // AUTO-RECONNECT: when the pendant is unplugged the SerialPort emits 'close';
  // on replug udev re-creates the /dev/rownd-pendant symlink pointing at the new
  // ttyACMn. Poll for the symlink and reopen so the pendant reconnects on its
  // own after an unplug/replug.
  const scheduleReconnect = () => {
    if (reconnectTimer) return;
    reconnectTimer = setInterval(() => {
      if (fs.existsSync(devicePath)) {
        clearInterval(reconnectTimer); reconnectTimer = null;
        log(`pendant: ${devicePath} reappeared — reopening`);
        openPendant();
      }
    }, 1000);
  };

  function openPendant() {
    dial = new SerialPort({ path: devicePath, baudRate });
    dial.on('open', () => {
      log(`pendant serial: opened ${devicePath} @ ${baudRate}`);
      lastPendantByte = Date.now();
      watchdogTripped = false;
      if (!hangWatchdog) {
        hangWatchdog = setInterval(() => {
          if (!dial || !dial.isOpen) return;
          const silent = Date.now() - lastPendantByte;
          if (silent > PENDANT_SILENCE_MS && !watchdogTripped) {
            watchdogTripped = true;
            try {
              if (state.sendRaw('\x85'))
                log(`pendant SILENT ${silent}ms (hang, USB still open) — JogCancel (0x85) fail-safe`);
            } catch (e) { log(`hang-watchdog JogCancel failed: ${e.message}`); }
          }
        }, 200);
      }
    });
    dial.on('error', (e) => log(`pendant serial error: ${e.message}`));
    dial.on('close', () => {
      if (hangWatchdog) { clearInterval(hangWatchdog); hangWatchdog = null; }
      // FAIL-SAFE: the pendant owns the active jog. If it dies mid-jog
      // (USB-CDC stall / brownout / S3 reset) its onPoll deadman dies too,
      // so nothing cancels the running $J= — it coasts to the soft limit,
      // can throw error:15 → ALARM → spindle enable drops (chuck stops)
      // while the UI keeps the stale "turning" state, and the machine wedges
      // until a mushroom + reset. Cancel any active jog from the Pi side the
      // instant the serial drops. JogCancel (0x85) is a no-op when idle.
      try { if (state.sendRaw('\x85')) log('pendant closed — sent JogCancel (0x85) fail-safe'); }
      catch (e) { log(`fail-safe JogCancel failed: ${e.message}`); }
      log('pendant serial closed — waiting for replug');
      scheduleReconnect();
    });
    dial.on('data', (chunk) => {
    // Filter out XON (0x11). Pendant emits it as a flow-control kickstart
    // before status queries (legacy FluidNC habit), but Rownd's Grbl-ESP32
    // doesn't recognise XON — treats the byte as line content, emits
    // "error:1 (Expected command letter)" on every poll. Strip before
    // forwarding; everything else (line-terminated $J=, S<rpm>, single-byte
    // realtime 0x18/0x21/0x3F/0x7E/0x85/0x90-0x9F) goes through verbatim.
    // Heartbeat for the hang watchdog: any byte from the pendant means it's
    // alive (status polls arrive even when idle). Clear a tripped watchdog so a
    // recovered pendant resumes normal operation.
    lastPendantByte = Date.now();
    if (watchdogTripped) { watchdogTripped = false; log('pendant bytes resumed — hang-watchdog cleared'); }
    const filtered = chunk.toString('latin1').replace(/\x11/g, '');
    if (!filtered) return;
    // Split off any `@`-prefix bridge commands; everything else forwards.
    const { forward, bridgeLines } = splitter(filtered);
    for (const line of bridgeLines) {
      handleBridgeCommand(state, line, replyToPendant)
        .catch((e) => log(`bridge cmd ${line} failed: ${e.message}`));
    }
    let out = forward;
    // RUN LOCKOUT: while a program is streaming, a pendant LINE (e.g. an
    // accidental dpad $J=) steals an `ok` from cncjs's char-counting sender and
    // stalls the stream. Drop line content during running/paused; keep realtime
    // single-byte commands (hold/resume/reset/override/status/jogcancel).
    if (out && (state.workflowState === 'running' || state.workflowState === 'paused')) {
      const gated = realtimeOnly(out);
      if (gated.length !== out.length) {
        log(`run active (${state.workflowState}) — dropped ${out.length - gated.length}B pendant line content (jog/cmd lockout); kept ${gated.length}B realtime`);
      }
      out = gated;
    }
    if (out) {
      log(`pendant>cncjs ${out.length}B  hex=${[...Buffer.from(out, 'latin1')].map((b) => b.toString(16).padStart(2, '0')).join(' ')}`);
      const ok = state.sendRaw(out);
      if (!ok) log('  ↳ NOT FORWARDED (no active serial port)');
    }
    });
  }

  // Controller -> pendant echo path. cncjs's serialport:read fires with the
  // controller's reply ("ok", status frames, alarm decodes, etc.); we mirror
  // those to the pendant so its GrblParserC + scene state stay in sync.
  state.onPendantSend = (data) => {
    // cncjs's serialport:read strips trailing \r\n — emits just the line
    // content (e.g. "<Idle|MPos:...>" instead of "<Idle|...>\r\n"). The
    // pendant's GrblParserC requires \n to finalize a line and call
    // show_state(); without it, the parser line buffer fills but never
    // dispatches, state stays Disconnected, screen shows the WiFi-fallback
    // pill. Add the terminator back.
    try {
      if (dial && dial.isOpen) {
        dial.write(typeof data === 'string' ? data + '\n' : Buffer.concat([data, Buffer.from('\n')]));
      }
    } catch (e) {
      log(`pendant write fail: ${e.message}`);
    }
  };

  openPendant();
}

function attachPendantStub(state) {
  log('pendant stub: type a line + ENTER to send to controller. Ctrl-D to quit.');
  log('  examples: ?    $$    $G    M5    $J=G91 G21 X1 F100');
  log('  bridge cmds (out-of-band): @LIST    @LOAD foo.nc    @START    @STOP');
  const rl = readline.createInterface({ input: process.stdin });
  const replyToStub = (s) => process.stdout.write(s);
  rl.on('line', (line) => {
    line = line.trim();
    if (!line) return;
    if (line === 'quit' || line === 'exit') { state.sock.close(); process.exit(0); }
    if (line.startsWith('@')) {
      handleBridgeCommand(state, line, replyToStub)
        .catch((e) => log(`bridge cmd ${line} failed: ${e.message}`));
      return;
    }
    if (line.length === 1 && '?!~'.includes(line)) {
      // realtime single-byte commands — use raw write
      state.sendRaw(line);
    } else if (line === 'softreset') {
      state.sendRaw('\x18');
    } else {
      state.sendLine(line);
    }
  });
  rl.on('close', () => { state.sock.close(); process.exit(0); });
  state.onPendantSend = (data) => { /* stub: data already echoed by serialport:read handler */ };
}

// ── panel daemon proxy — localhost TCP, line-based ─────────────────────────
// Optional: an external panel daemon (NOT part of this pendant mod; disable with
// --no-panel) can own a body-mounted button/LED panel and talk to it over USB-CDC.
// If one is present, its cncjs traffic goes through this bridge:
// the daemon connects to a localhost TCP socket where the bridge broadcasts
// controller-state changes and accepts button events.
//
// Protocol (line-based ASCII, lines terminated by \n):
//   bridge -> client: STATE <activeState>      e.g. STATE Idle, STATE Alarm
//   client -> bridge: LIGHTS 0|1               cncjs writeln "$led/state=N"
//                     DOOR_BYPASS 0|1          cncjs writeln "$Door/Bypass=N"
//
// NOTE: Reset/Unlock are NOT handled here anymore. The renderer owns recovery
// and sends controller.command('reset'|'unlock') over cncjs directly; physical
// Reset/Unlock presses go Pico → panel daemon → renderer (phys_press IPC).
//
// The server is started once in main() and persists across cncjs reconnects.
// Each runOnce() iteration registers its fresh `state` via setState().
class PanelServer {
  constructor(port) {
    this.port = port;
    this.clients = new Set();
    this.state = null;
    this.lastActive = null;
    // Last volatile controller settings we were told to assert. On a cold
    // boot the daemon fires LIGHTS/DOOR_BYPASS (from the Pico's boot-time
    // SW LIGHTS / KEY position) BEFORE cncjs has opened the ESP32 serial
    // port, so sendLine() drops them silently and the switch's physical
    // position is lost until the operator toggles it. We stash them here and
    // replay via replayVolatile() the instant the serial port comes up.
    this._lastLights = null;       // 0 | 1 | null (never told)
    this._lastDoorBypass = null;   // 0 | 1 | null (never told)
  }
  start() {
    const net = require('net');
    const server = net.createServer((sock) => {
      this.clients.add(sock);
      log(`panel: client connected (${this.clients.size} total)`);
      if (this.lastActive) {
        try { sock.write(`STATE ${this.lastActive}\n`); } catch (_) { /* ignore */ }
      }
      let buf = '';
      sock.on('data', (chunk) => {
        buf += chunk.toString('utf8');
        let idx;
        while ((idx = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, idx).trim();
          buf = buf.slice(idx + 1);
          if (line) this.handleLine(line);
        }
      });
      sock.on('close', () => {
        this.clients.delete(sock);
        log(`panel: client disconnected (${this.clients.size} total)`);
      });
      sock.on('error', (e) => log(`panel: socket error ${e.message}`));
    });
    server.on('error', (e) => log(`panel server error: ${e.message}`));
    server.listen(this.port, '127.0.0.1', () => log(`panel: listening on 127.0.0.1:${this.port}`));
  }
  setState(state) { this.state = state; }
  broadcastState(active) {
    this.lastActive = active;
    this.broadcast(`STATE ${active}\n`);
  }
  broadcast(msg) {
    for (const c of this.clients) {
      try { c.write(msg); } catch (_) { /* ignore */ }
    }
  }
  handleLine(line) {
    log(`panel<: ${line}`);
    const space = line.indexOf(' ');
    const cmd = (space < 0 ? line : line.slice(0, space)).toUpperCase();
    const arg = space < 0 ? '' : line.slice(space + 1).trim();
    // Record the volatile setting BEFORE gating on cncjs state. At cold boot
    // the daemon fires LIGHTS/DOOR_BYPASS (from the switch's boot position)
    // before cncjs has opened the ESP32 serial port — if we early-returned
    // here the value would be lost and replayVolatile() would have nothing to
    // replay, leaving the lamp stuck at its NVS-persisted state regardless of
    // the switch. So stash first; sendLine (gated on serial) just becomes the
    // fast path when the port is already up.
    if (cmd === 'LIGHTS') {
      this._lastLights = arg === '1' ? 1 : 0;
      if (!this.state || !this.state.sendLine(`$led/state=${this._lastLights}`)) {
        log(`panel: LIGHTS ${this._lastLights} deferred (serial not ready) — will replay`);
      }
    } else if (cmd === 'DOOR_BYPASS') {
      // Custom Grbl_Esp32 obc_mod setting — masks
      // CONTROL_SAFETY_DOOR_PIN in firmware when On. Wired from the
      // body-panel KEY switch via the Pico → daemon → here path.
      this._lastDoorBypass = arg === '1' ? 1 : 0;
      if (!this.state || !this.state.sendLine(`$Door/Bypass=${this._lastDoorBypass}`)) {
        log(`panel: DOOR_BYPASS ${this._lastDoorBypass} deferred (serial not ready) — will replay`);
      }
    } else {
      log(`panel: unknown cmd ${cmd}`);
    }
  }
  // Re-send the last-known volatile settings once the cncjs serial port is
  // up. Called on the serialPort null->set transition in connectCncjs. This
  // closes the cold-boot race where the switch's boot position was dropped
  // because the port wasn't open yet (the lamp stayed off while the switch
  // sat at ON until a manual toggle).
  replayVolatile() {
    if (!this.state || !this.state.serialPort) return;
    if (this._lastLights !== null) {
      log(`panel: replay LIGHTS ${this._lastLights} (serial now ready)`);
      this.state.sendLine(`$led/state=${this._lastLights}`);
    }
    if (this._lastDoorBypass !== null) {
      log(`panel: replay DOOR_BYPASS ${this._lastDoorBypass} (serial now ready)`);
      this.state.sendLine(`$Door/Bypass=${this._lastDoorBypass}`);
    }
  }
}

// ── main ────────────────────────────────────────────────────────────────────
async function runOnce(secret, token, opts) {
  const port = await findCncjsPort(token, opts.panelPort);
  const state = connectCncjs(port, token, opts.panelServer);
  if (opts.pendantPath) {
    attachPendantSerial(state, opts.pendantPath, opts.baudRate);
  } else {
    attachPendantStub(state);
  }
  // Resolve when the socket disconnects so the outer loop can reconnect.
  return new Promise((resolve) => state.sock.on('disconnect', resolve));
}

// Passthrough — no cncjs. Just open the pendant serial port and timestamp
// every chunk it sends. Used for verifying the serial wrapper end-to-end on
// homepi (pendant USB plugged into homepi instead of the Rownd Pi) BEFORE
// touching the lathe. Pendant won't get status reports back, so it'll show
// "Disconnected" — that's expected; we're checking the bridge's input layer
// reads the wire cleanly.
async function passthroughMode(devicePath, baudRate) {
  const { SerialPort } = require('serialport');
  const dial = new SerialPort({ path: devicePath, baudRate });
  await new Promise((res, rej) => {
    dial.once('open', () => { log(`passthrough: opened ${devicePath} @ ${baudRate}`); res(); });
    dial.once('error', rej);
  });
  log('logging pendant traffic; nothing forwarded; Ctrl-C to stop.');
  dial.on('data', (chunk) => {
    const ts = new Date().toISOString().slice(11, 23);
    // Hex-print each chunk for clarity. Realtime bytes are single-byte;
    // line writes are usually multi-byte ASCII.
    const hex = [...chunk].map((b) => b.toString(16).padStart(2, '0')).join(' ');
    const ascii = chunk.toString('latin1').replace(/[^\x20-\x7e]/g, '·');
    process.stdout.write(`${ts}  ${hex}  |  ${ascii}\n`);
  });
  dial.on('error', (e) => log(`serial error: ${e.message}`));
  // Run forever
  return new Promise(() => {});
}

async function smokeTest(secret, token) {
  // One-shot: discover port, connect, listen 5s, exit. For health checks.
  const port = await findCncjsPort(token);
  log(`smoke: port=${port}`);
  const sock = io(`http://127.0.0.1:${port}`, {
    query: `token=${token}`,
    reconnection: false,
    transports: ['websocket', 'polling'],
  });
  await new Promise((res, rej) => {
    sock.on('connect', () => { log('smoke: connected'); res(); });
    sock.on('connect_error', (e) => rej(e));
    setTimeout(() => rej(new Error('connect timeout')), 5000);
  });
  sock.emit('list');
  await new Promise((r) => setTimeout(r, 1500));
  sock.close();
  log('smoke: PASS');
}

function parseArgs(argv) {
  const opts = { pendantPath: null, baudRate: 115200, smoke: false, passthrough: null, panelPort: 7990 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--smoke') opts.smoke = true;
    else if (a === '--pendant') opts.pendantPath = argv[++i];
    else if (a === '--passthrough') opts.passthrough = argv[++i];
    else if (a === '--baud') opts.baudRate = parseInt(argv[++i], 10);
    else if (a === '--panel-port') opts.panelPort = parseInt(argv[++i], 10);
    else if (a === '--no-panel') opts.panelPort = 0;
  }
  return opts;
}

function usage() {
  console.error(`Usage:
  bridge.js                          stub mode — stdin pendant + cncjs (manual)
  bridge.js --pendant /dev/ttyACM0   real pendant + cncjs (production)
  bridge.js --passthrough /dev/ttyACM0
                                     log pendant bytes only, no cncjs (dry run)
  bridge.js --smoke                  one-shot: discover port + connect, exit
  --baud <rate>                      override 115200`);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));

  if (opts.passthrough) {
    // Dry-run — no JWT, no cncjs. Just verify the serial wrapper sees the
    // pendant. Run on homepi with pendant plugged in there.
    await passthroughMode(opts.passthrough, opts.baudRate);
    return;
  }

  const secret = readSecret();
  const token = mintToken(secret);
  log(`secret prefix: ${secret.slice(0, 6)}…  token prefix: ${token.slice(0, 24)}…`);

  if (opts.smoke) {
    await smokeTest(secret, token);
    process.exit(0);
  }

  log(`pendant mode: ${opts.pendantPath ? `serial ${opts.pendantPath}` : 'stdin stub'}`);

  // Panel server is started once and survives cncjs reconnects. Each runOnce
  // iteration re-registers its fresh `state` object via panelServer.setState().
  opts.panelServer = null;
  if (opts.panelPort > 0) {
    opts.panelServer = new PanelServer(opts.panelPort);
    opts.panelServer.start();
  } else {
    log('panel: disabled (--no-panel or --panel-port 0)');
  }

  // Forever loop: connect, run, on disconnect → wait then retry.
  // Re-discover port each iteration (Rownd may have restarted with a new port).
  while (true) {
    try {
      await runOnce(secret, token, opts);
    } catch (e) {
      log(`run error: ${e.message}`);
    }
    log(`reconnecting in ${RECONNECT_DELAY_MS}ms`);
    await new Promise((r) => setTimeout(r, RECONNECT_DELAY_MS));
  }
}

process.on('SIGINT', () => process.exit(0));
process.on('SIGTERM', () => process.exit(0));
process.on('unhandledRejection', (e) => { log('unhandledRejection:', e); process.exit(1); });

main().catch((e) => { log('fatal:', e.message); process.exit(1); });
