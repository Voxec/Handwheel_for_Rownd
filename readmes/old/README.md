# Pendant bundle

Wired movement pendant for the Rownd lathe — M5Dial (encoder + round screen) + 4-way
D-pad + RAPID/STOP, relayed to the lathe's cncjs by a small Pi-side bridge. **Motion
only**: jog, tool-zero, part-zero. Not Reset/Unlock, not tool-change.

## Case design status — operational mockup
The case here is an **operational mockup**: it works and lets you build and run the
pendant, but it is **not a final industrial design**. A proper design pass will happen
when there is time for it. **Other users' designs are highly appreciated** — remixes
and improvements are welcome.

## Contents (shareable)
- `cad/` — the case: `Rownd_pendant_case.stl` + `Rownd_pendant_cover.stl` (print) and
  the editable **Fusion 360 source** `Rownd_pendant_case.f3d` / `Rownd_pendant_cover.f3d`.
  Case ≈ 64 × 201 × 40 mm; cover ≈ 60 × 182 × 24 mm. Print in PETG / PETG-CF.
- `bom.csv` — parts list (qty, source, approx price — verify links/prices before order).
- `install.md` — build & install, step by step.
- `wiring.pdf` — interconnect / harness diagram + net table (Pi ↔ M5Dial ↔ PCF8574
  D-pad + native buttons). See the note under *Electronics* below.
- `make_wiring_pdf.py` — wiring-doc generator.

> The M5Dial houses in this case; no third-party (FluidDial) case geometry is needed
> or redistributed.

## Matching code
This is a software + hardware mod — reproduce both halves:
- **Pi-side bridge**: `bridge/` (this repo). `bridge.js` connects as a JWT peer
  client of the lathe's cncjs and relays pendant input; reads the cncjs secret from
  the device's own `.cncrc` at runtime (no secret committed).
- **Pendant firmware**: the FluidDial fork with the Rownd patch branch (jog model,
  safe-jog gating, USB-CDC routing, OTA dual-slot flash).

> TODO before publishing: pin the exact firmware repo/branch + bridge commit hashes
> here once the share scope (firmware-only vs full) is settled with the vendor.

> **Optional panel hook — not included.** `bridge.js` also carries a small proxy for an
> external body-panel daemon (work-light / door-bypass switches). That panel is a
> **private hardware build — no switches, wiring, or daemon are shared here** — so the
> hook stays dormant and can be turned off with `--no-panel`. It's a place to wire your
> own panel, nothing that ships ready to use.

## Electronics
There is **no custom PCB** — the pendant is entirely off-the-shelf modules joined by a
wire harness: an M5Dial (ESP32-S3), a PCF8574 I²C expander breakout, and six momentary
buttons. So the correct electrical deliverable is an **interconnect / harness diagram +
net table**, not a schematic-capture file (a KiCad schematic would imply a board to
fab, which there isn't). `wiring.pdf` carries: a system block diagram, the M5Dial Grove
pin reference (GPIO + colours), per-port wiring with build steps, and a full
**From / Wire / To / Function** net table plus a bench verification checklist.
- **I²C bus:** SDA = G15, SCL = G13. Grove I²C modules carry their own bus pull-ups; if
  the bus is marginal, add 4.7 kΩ SDA/SCL pull-ups to 3V3.
- **PCF8574:** address `0x20` (A0/A1/A2 → GND); breakout has a 100 nF decoupling cap —
  add one at the IC if you wire a bare chip. Button inputs are active-low on the
  ~100 µA internal pull-ups (add 10 kΩ externals if flaky).

## Design notes
- Buttons are field-removable via 2-pin Dupont disconnects.
- D-pad runs on a PCF8574 I²C expander (0x20) off Grove Port A; RAPID/STOP are native
  on Port B.
- A smooth mid-jog rapid uses feed-override-during-jog, which shipped in
  controller firmware **v4.2.3** (`$71 Jog/AllowFeedOverride`). The pendant side
  is implemented but **untested — not yet flashed or bench-verified.**
