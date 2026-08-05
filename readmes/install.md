# Pendant — build & install

A wired movement pendant for the Rownd lathe: an M5Dial (rotary encoder + round
screen) plus a 4-way D-pad and two native buttons, talking to the lathe's cncjs over
a small Pi-side bridge. Scope is **motion only** — jog, tool-zero, part-zero. It does
not drive Reset/Unlock or tool-changes (those stay on the screen).

> Prices and links in `bom.csv` are approximate / TODO — verify before ordering.

## 1. Print the case
- Print `cad/Rownd_pendant_case.stl` + `cad/Rownd_pendant_cover.stl` in PETG / PETG-CF.
  The editable Fusion 360 source is alongside (`*.f3d`) if you want to tweak the fit.
- The M5Dial, PCF8574 and buttons house in the printed case — no third-party
  (FluidDial) case geometry is needed.
- This case is an **operational mockup** (see `README.md`); fit is functional, not a
  final industrial design.

## 2. Flash the M5Dial
- Firmware is the **FluidDial fork with the Rownd patches** (jog model, safe-jog
  gating, USB-CDC routing). Flash over USB.
- The M5Dial is OTA dual-slot — a single-slot flash rolls back on a cold boot. Flash
  `boot_app0.bin @ 0xe000` + `firmware.bin @ 0x10000` together, then power-cycle and
  confirm it comes up on the lean jog scene (not the old dial menu).

## 3. Wire it (see `wiring.pdf` for the full diagram)
- **Port A → PCF8574** (permanent internal I²C, address 0x20) via a Grove HY2.0-4P
  cable. This is the D-pad bus.
- **PCF8574 → 4 D-pad buttons** (Up / Down / Left / Right), each through a 2-pin
  Dupont disconnect so a button can be swapped in the field.
- **Port B → 2 native buttons** (RAPID, STOP) via a Grove cable, each on its own
  2-pin disconnect.
- All buttons are momentary NO; the second pin of each is GND.

## 4. Mount
- Seat the dial and buttons in the front shell, route the button leads to their
  disconnects, secure the cable with the strain-relief screw, close the case.

## 5. Connect & run
- USB-C from the lathe Pi to the M5Dial (power + serial).
- The Pi-side bridge (`bridge/bridge.js`, see the repo) connects as a JWT peer
  client of the lathe's cncjs and relays pendant input. Run it as the bridge
  service; it auto-finds the cncjs port.

## 6. Verify (hand near the STOP, machine able to move)
- Jog each D-pad direction: holds while pressed, stops on release.
- Hold RAPID + a direction: faster jog (note: a smooth mid-jog rapid needs the
  firmware feed-override change — see `../../share/ESP_FEATURE_REQUESTS.md`).
- Encoder detent = single small step.
- Confirm the second direction is ignored while one is held, and the dial press
  acts as a stop.

## Matching code (cherry-pick alongside this bundle)
- Pi-side bridge: `bridge/` in this repo.
- Pendant firmware: the FluidDial fork (Rownd patch branch).
- See `README.md` here for the exact commit pointers.
