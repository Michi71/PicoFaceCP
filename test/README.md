# test/ – macOS-Host-Test für die mdaEPiano-Engine

Spielt die **unveränderte Pico-mdaEPiano-Engine** (`src/mdaEPiano.cpp`) auf dem
Mac über **CoreAudio** und steuert sie per **MIDI** (PortMidi virtueller Input).

Damit lassen sich Engine + eigene Sample-Sets am Mac testen, bevor sie auf den
Pico gehen.

## Bauen & Starten

    ./test/build.sh
    ./test/mdaepiano_test

Abhängigkeiten (Homebrew): `portmidi` (CoreAudio ist Teil des Systems).

    brew install portmidi

## Steuerung

| Taste | Aktion |
|-------|--------|
| `+` / `-` | Programm (Preset) weiter/zurück |
| `1`…`5` | Programm direkt wählen |
| `i` | **Instrument umschalten** (alle 6: mda-Default, Clavinet, Piano, rhodes_suitcase, wurlitzer_200a, yamaha_cp80) |
| `s` | Sustain-Pedal (CC64) an/aus |
| `m` | Mod-Wheel (CC1) auf 127 |
| `q` | Quit |

**MIDI:** beim Start wird ein virtueller Eingang `mdaepiano` angelegt. Aus
einer DAW / einem MIDI-Tool diesen Port als Ausgang wählen. Empfangen werden
Note On/Off, Control Change (Sustain, Volume, ModWheel …) und Program Change.

## Funktionsweise

- `test/test.cpp` erzeugt die Engine (`mdaEPiano`), ruft `setSampleRate(44100)`
  + `setProgram(0)` auf und wandelt MIDI-Bytes in `noteOn`/`noteOff`/
  `processMidiController`/`setProgram` um.
- Die Engine liefert pro `process()`-Aufruf `I2S_BUFFER_WORDS` (16) Frames als
  `int16` Stereo (L/R getrennt). Der CoreAudio-Render-Callback hält einen
  **Ringpuffer**, damit beliebig große Audio-Blöcke ohne Sampleverlust gefüllt
  werden (16-er-Engine-Blöcke → Float-Stereo-Interleave).
- MIDI-Quelle: PortMidi virtueller Input.

## Engine-Anpassungen für den Host-Build (nur aktiv mit `-DMDA_HOST_BUILD`)

Damit die Pico-Engine auf dem Mac kompiliert/läuft, wurden zwei **guarded**
Minimaländerungen vorgenommen (Firmware-Build bleibt unberührt, da ohne
`MDA_HOST_BUILD` gebaut):

1. `include/mdaEPiano.h`: das Pico-`#include "audio_subsystem.h"` wird im
   Host-Build übersprungen; die nötigen Makros (`SAMPLES_PER_BUFFER` = 16,
   `PICO_AUDIO_I2S_BUFFERS_PER_CHANNEL` = 3) werden lokal definiert.
2. `include/mdaEPianoData.h`: `epianoData` wird host-seitig **schreibbar**
   (`int16_t` statt `const int16_t`), weil der Konstruktor die XFade-Loop-
   Überblendung in die Wellenformen schreibt (auf dem Pico liegen die Daten im
   RAM; `const`-Daten wären auf dem Mac nur-lesen → Bus-Error).

`test/build.sh` setzt `-DMDA_HOST_BUILD`, `-Iinclude`, `-I/opt/homebrew/include`
und linkt `-lportmidi -framework CoreAudio -framework AudioToolbox`.

## Neues Instrument eingebaut

Der Host-Test bindet **alle** vorhandenen Sample-Sets ein und startet
standardmaessig mit `Clavinet`. Mit `i` schaltet man zyklisch durch:

  0 = mda EPiano (Default)  1 = Clavinet  2 = Piano
  3 = rhodes_suitcase        4 = wurlitzer_200a  5 = yamaha_cp80

Damit das Umschalten zur Laufzeit knallfrei und use-after-free-sicher ist,
werden **alle Synth-Instanzen beim Start erzeugt** und leben; geschaltet wird
nur ein `std::atomic`-Zeiger (Audio-Thread) und ein Index (Hauptthread).

Die Engine wurde dafuer minimal generalisiert (alle Aenderungen wirken auch im
Firmware-Build, verhalten sich fuer mda aber identisch):
- `kgrp[34]` -> `kgrp[64]` + Member `nKeygroups`/`noteonStep` (mda: 33 / 3).
- neue Methode `loadInstrument(data, kgrpTable, nKeygroups, noteonStep)`:
  tauscht Wellenformen + Keygroup-Tabelle und beendet alte Stimmen.
- `noteOn`: `k += 3` -> `k += noteonStep` (mit Begrenzung durch `nKeygroups`).
  Fuer mda (33/3) bleibt die Keygroup-Suche exakt wie zuvor.
- Das XFade im Konstruktor laeuft weiterhin (nur auf den mda-Daten); neue
  Instrumente brauchen es nicht (Loop-Punkte sind via FindLoopPoints klickfrei).

`build.sh` setzt den Include-Pfad auf den `generated/`-Ordner.
