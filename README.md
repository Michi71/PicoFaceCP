# PicoFaceCP

**A Yamaha Reface CP emulation for the Raspberry Pi RP2350 (16 MB flash).**

PicoFaceCP turns an RP2350 board into a compact electric-piano module: the
[mda‑EPiano](https://sourceforge.net/projects/mda-vst/) sound engine drives six
classic electric‑piano voices, processed through a faithful re‑creation of the
**Reface CP** insert‑effect chain, and controlled from an SH1106 OLED and a
single rotary encoder. A macOS host build lets you develop and audition the
whole signal path on your desktop before flashing hardware.

---

## Features

- **6 voices** (mda‑EPiano sample engine, 96‑voice polyphony, 44.1 kHz, stereo):
  `Rd I` · `Rd II` · `Wr` · `Clv` · `Piano` · `CP`
- **Reface CP effect chain** — four insert effects in series plus drive & volume,
  with authentic 3‑position switching per block and voice‑type‑linked tremolo.
- **USB‑MIDI** input (note on/off, control change, program change).
- **Virtual front‑panel UI** (SH1106 + single encoder) — VOICE header, a
  scrolling list of effect blocks (DRV / TRM / CHO / DLY / REV / VOL / MENU) in a
  large bold font, and a context line showing the selected block's parameters;
  edits are applied live. Effect type (Off/Tremolo/Wah …) is switched with a
  long press, like the hardware toggle. Presets and system settings live in a
  separate main menu.
- **Header‑only, RP2350‑optimized DSP** — single‑precision float, no heap, the
  per‑sample hot path placed in RAM to avoid XIP‑cache jitter inside the audio IRQ.
- **macOS host demo** (CoreAudio + PortMidi) running the exact same effect code.

Current firmware footprint: **FLASH ≈ 5.6 %** (≈ 0.9 MB / 16 MB), **RAM ≈ 36 %**
(≈ 187 KB / 512 KB).

---

## Signal flow

```
              ┌────────┐   ┌──────────────┐   ┌───────────────┐   ┌─────────────────┐   ┌────────┐
MIDI ▶ Voice ▶│ DRIVE  │ ▶ │ TREMOLO / WAH│ ▶ │ CHORUS /PHASER│ ▶ │ D.DELAY/A.DELAY │ ▶ │ REVERB │ ▶ VOLUME ▶ I2S out
       engine └────────┘   └──────────────┘   └───────────────┘   └─────────────────┘   └────────┘
```

| Block | Switch positions | Parameters |
|-------|------------------|------------|
| **Drive** | — | amount |
| **1 · Tremolo / Wah** | Off / Tremolo / Wah | Depth, Rate |
| **2 · Chorus / Phaser** | Off / Chorus / Phaser | Depth, Speed |
| **3 · D.Delay / A.Delay** | Off / Digital / Analog | Depth, Time |
| **4 · Reverb** | — | Depth |
| **Volume** | — | output level |

The **tremolo** automatically follows the selected voice, exactly like the
hardware: auto‑pan for `Rd I` / `Rd II` / `CP`, amplitude modulation for
`Wr` / `Clv` / `Piano`. The reference for these effects is the official manual
(`doc/ZT92080_reface_En_OM_C0.pdf`, "reface CP" section).

---

## Hardware

Default target board: **SparkFun Pro Micro RP2350** (`PICO_BOARD` in
`CMakeLists.txt`; change it for your own board). Audio output uses an I2S DAC
(e.g. Waveshare Pico‑Audio).

| Function | GPIO | Notes |
|----------|------|-------|
| I2S DATA (DOUT) | 26 | `PICO_AUDIO_I2S_DATA_PIN` |
| I2S BCLK | 27 | `PICO_AUDIO_I2S_CLOCK_PIN_BASE` |
| I2S LRCLK (WS) | 28 | |
| OLED SDA | 2 | SH1106 128×64 over I2C |
| OLED SCL | 3 | |
| Encoder A | 20 | |
| Encoder B | 21 | |
| Encoder button | 13 | |
| Status LED | 25 | |
| DIN‑MIDI RX | 5 | optional |

Pins are defined in [`include/project_config.h`](include/project_config.h).

---

## Repository layout

```
effects/              Reface CP effect chain (header-only)
  dsp_fastmath.h        fast tanh/tan approximations
  dsp_lut.h             sine lookup table
  dsp_reverb.h          Schroeder stereo reverb
  reface_cp_fx.h        Tremolo / Chorus / Phaser / Delay primitives
  wahwah.h              standalone MK8 touch-wah
  reface_cp_chain.h     RefaceCpChain master class (setters + getters)
  cp_hot.h              CP_HOT() RAM-placement macro (Pico) / no-op (host)
  cp_audio.h            int16 <-> float block glue
  effect_chain.{h,cpp}  Rhodes MK8 reference chain (source of WahWah)
include/              engine, UI, board and config headers
src/                  main.cpp, mdaEPiano engine, USB-MIDI, OLED/encoder UI
  pico_frontpanel.{h,cpp}  virtual front‑panel UI (home screen + main menu)
test/                 macOS host demo (cp_test.cpp, build_cp.sh)
doc/                  Reface owner's manual (PDF)
lib/                  pico-sdk, pico-extras, FreeRTOS-Kernel (submodules), u8g2, ...
```

---

## Building the firmware

Requires the Arm GNU toolchain (`arm-none-eabi-gcc`), CMake ≥ 3.22 and Ninja
(or Make).

```bash
# 1. Clone with submodules (pico-sdk, pico-extras, FreeRTOS-Kernel)
git clone --recurse-submodules https://github.com/Michi71/PicoFaceCP.git
cd PicoFaceCP
# (if already cloned: git submodule update --init --recursive)

# 2. Configure & build
cmake -S . -B build -G Ninja
cmake --build build -j4
```

Flash `build/main.uf2` by holding BOOTSEL while plugging in the board and copying
the file to the `RPI-RP2` drive (or use `picotool load build/main.uf2`).

---

## macOS host demo

Build and run the full engine **plus the Reface CP effect chain** natively, with
audio through CoreAudio and MIDI through a virtual PortMidi input named
`mdaepiano`.

```bash
brew install portmidi
./test/build_cp.sh
./test/cp_test
```

Point any DAW / MIDI tool at the `mdaepiano` virtual port to play it.

> The original engine‑only demo (`test/test.cpp`, `./test/build.sh`,
> `test/mdaepiano_test`) is kept alongside it for reference.

---

## Controls

### On the device (OLED + encoder)
The home screen is a virtual front panel. The VOICE header stays fixed; the
effect blocks are a bold scroll list that follows the cursor (only the visible
rows are drawn, with up/down arrows when more exist):
```
VOICE Rd I
------------
DRV   40            ▲
TRM  T 25/60        
CHO  C 40/35        ▼
------------
Depth 25  Rate 60   <- context line of the selected block
```
- **Turn** the encoder to change the highlighted value (applied live).
- **Short press** → next parameter (cycles through all blocks, then `MENU`);
  the list scrolls so the selected block stays visible.
- **Long press** (≥ 0.5 s) on TRM / CHO / DLY → cycle that block's type
  (Off → Tremolo → Wah, Off → Chorus → Phaser, Off → Digital → Analog);
  the context line briefly shows the new mode. On the other entries a long
  press steps back to the previous parameter.
- On `MENU`, **short press** opens the main menu: `Presets` · `System` ·
  `<< BACK` (`System` = about / future settings). Master volume is the VOL
  block on the front panel.
Drive, Reverb and Volume are single‑knob blocks; Tremolo/Wah, Chorus/Phaser
and Delay expose Depth and Rate/Speed/Time plus a long‑press type switch.

### Host demo keyboard
| Key | Action |
|-----|--------|
| `+` / `-` | program up / down |
| `1`…`5` | select program directly |
| `i` | next voice (Rd I → Rd II → Wr → Clv → Piano → CP) |
| `t` | Tremolo/Wah: off → tremolo → wah |
| `c` | Chorus/Phaser: off → chorus → phaser |
| `d` | Delay: off → digital → analog |
| `r` | Reverb on/off |
| `s` | sustain pedal (CC64) |
| `m` | mod wheel (CC1) → 127 |
| `q` | quit |

---

## Design notes (RP2350)

- **Header‑only DSP, no dynamic allocation** — everything lives in fixed buffers
  and compiles for both the Cortex‑M33 firmware and the host build.
- **Hot path in RAM** — `cp_hot.h` maps `CP_HOT()` to the Pico SDK's
  `__not_in_flash_func`, so the per‑sample chain (`RefaceCpChain::process`, which
  the compiler inlines the whole chain into) runs from SRAM and avoids flash
  XIP‑cache stalls in the I2S interrupt; on the host the macro is a no‑op.
- **int16 delay line** — the 500 ms stereo delay stores `int16` samples, which is
  lossless relative to the 16‑bit engine source and roughly halves its RAM use.
- **Single‑precision float** throughout the audio path (the M33 has a hardware FP
  unit; `double` is avoided).

---

## Acknowledgements & license

- Sound engine: **mda‑EPiano** © Paul Kellett / David Robillard (GPL).
- Reface CP behaviour modelled from Yamaha's official owner's manual.
- DSP and UI code for the effect chain were developed with an LLM‑assisted
  workflow (architecture/review here, code generation via `glm‑5.2`).

Licensed under the **GNU General Public License v3** — see [`LICENSE`](LICENSE).
