# obc_mod — Jog Pendant v2 (preview)

> **Teaser.** This is the design direction for the next pendant — not a build kit yet.
> The current pendant (forked M5Dial + Pi-side bridge) is what ships in this package
> today; v2 is where it's heading.

## Why a v2

The M5Dial pendant works, but it's a touchscreen device doing a job that wants real,
tactile, fail-safe controls. v2 drops the touchscreen for proper machine-pendant
hardware, in one self-contained handheld on a single cable.

## The shape

A printed handheld with its own brain (an RP2040), talking to the lathe Pi over **one
USB cable** — power and data. Nothing inside the lathe is opened; the existing panel
stays as-is.

| Control | Part | Job |
|---|---|---|
| **Handwheel** | 100 P/R optical MPG | fine incremental jog |
| **Joystick** | 8-way microswitch (bat-top), octagonal gate | continuous traverse, coupled X+Z |
| **Axis** | 3-position bat-lever (X / off / Z) | which axis the wheel drives |
| **Step** | 4-position hard-detent selector | 0.001 / 0.01 / 0.1 / 1 mm per wheel detent |
| **Rapid** | momentary button | fast modifier for the joystick |
| **Display** | 2.0″ 240×320 IPS (ST7789) | in-hand DRO: X/Z, active axis, step |

No E-stop on the pendant (the machine's mushroom stays the real stop), no soft STOP,
no analog/pot controls (a switch fails *open* = safe; a pot drifts *toward* motion).

**Control split:** the **step selector sizes the handwheel only**; **Rapid modifies the
joystick only** — the two jog paths never cross.

## Status

Design converged; full BOM in `pendant/BOM_v1_2026-06-28.pdf`. A workshop build for
later in the year. Enclosure starting point: Timos Handrad (Printables #367064),
faceplate re-CAD'd for these parts.

*— obc_mod*
