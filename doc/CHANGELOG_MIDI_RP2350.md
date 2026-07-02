# Änderungsprotokoll: MIDI-Implementierung + RP2350-Optimierung

Datum: 2026-07-02 · Workflow: Architektur/Orchestrierung Claude, Codegenerierung `glm-5.2:cloud`
Build verifiziert: `cmake --build build -j4` → FLASH 26,32 % / RAM 36,44 % (Release, keine neuen Warnungen)

## 0. Nachtrag (nach Live-Test auf Hardware): neuer SYSTEM-Screen

Nach erfolgreichem Hardware-Test wurden zwei zusätzliche Bedienelemente ergänzt:

* **Pre-Gain** (`effects/reface_cp_chain.h`): neuer Parameter `_preGain` (0..1, Default 1.0),
  wird als allererste Stufe in `RefaceCpChain::process()` angewendet — **vor** Drive und
  der restlichen FX-Kette. Zweck: manche Effekte (insbesondere Drive/Wah bei hohem Pegel)
  neigen zum Übersteuern; mit Pre-Gain kann der Eingangspegel der FX-Kette unabhängig von
  der Engine-Lautstärke abgesenkt werden. Multiplikation wird bei Unity (≥0.9999) übersprungen
  (kein zusätzlicher Rechenaufwand im Normalfall). Neu: `FX_PRE_GAIN` in `include/ipc.h`,
  `ipc_apply`-Case in `main.cpp` (`cp_fx.setPreGain(v)`), lokal am Panel bedienbar
  (kein reface-CP-CC-Äquivalent, daher keine MIDI-TX/RX-Anbindung).
* **MIDI-Kanal-Anzeige/-Einstellung**: `RefaceMidi` bekam `getRxChannel()`/`setRxChannel()`
  (inline bzw. `src/midi_reface.cpp`). Da `RefaceMidi` und das Frontpanel-UI beide auf
  **Core 1** laufen, erfolgt der Zugriff direkt (kein IPC nötig) — anders als alle
  audiowirksamen Parameter, die über die Core0-FIFO laufen.
* **Neuer Screen `SCR_SYSTEM`** (`src/pico_frontpanel.cpp`, letzte Seite vor Zurück zu VOL/OCT):
  Param A = MIDI-Empfangskanal (1–16 oder „All“ für Omni, Reset-Taste → All),
  Param B = Pre-Gain in % (Reset-Taste → 100 %).

## 1. Neue Dateien

| Datei | Inhalt |
|---|---|
| `include/midi_reface.h` | Klasse `RefaceMidi`: reface-CP-kompatible MIDI-Protokollschicht (Deklaration, SYSTEM-/TG-Adress-Enums) |
| `src/midi_reface.cpp` | Implementierung: Kanalfilter, CC-Map, Channel-Mode-Messages, SysEx (Identity, Parameter Change/Request, Bulk Dump/Request inkl. Checksumme), Active Sensing (TX 200 ms / RX-Timeout 350 ms), Master Transpose/Tune, Soft-Pedal-Velocity, Panel-CC-TX |
| `doc/MIDI_IMPLEMENTATION.md` | Separates MIDI-Dokument (äquivalent Data List S. 11–14) |
| `doc/CHANGELOG_MIDI_RP2350.md` | dieses Dokument |

## 2. Geänderte Dateien

### `include/midi_input_usb.h` / `src/midi_input_usb.cpp` (Transport-Rewrite)
* Parser von festen 3-Byte-Reads auf **USB-MIDI-4-Byte-Event-Pakete** (`tud_midi_packet_read`, CIN-Dispatch) umgestellt — vorher gingen SysEx-Nachrichten und Pitch Bend verloren bzw. brachten den Stream aus dem Tritt.
* Neue Callbacks: Pitch Bend (14 bit), Realtime-Byte (0xF8–0xFF), komplette SysEx-Nachricht (64-Byte-Assembler, Overflow-sicher), Activity (für Active-Sensing-Überwachung).

### `include/ipc.h`
* Neu: `IPC_CMD_PITCH_BEND` (0x09), `FX_EXPRESSION`, `ipc_send_pitch_bend()`.

### `effects/reface_cp_chain.h`
* Neu: `setExpression()/getExpression()` (CC 11), multipliziert im Prozess-Loop mit dem Master-Volume (eine Multiplikation pro Sample zusätzlich, vorbereitet fürs Inlining).

### `include/mdaEPiano.h` / `src/mdaEPiano.cpp` (Engine)
* **Pitch Bend** (±2 Halbtöne): `VOICE.dbase` speichert das ungebendete Sample-Inkrement; `setPitchBend(0..16383)` skaliert `delta` aller aktiven Voices mit `exp2f(semis/12)`; `noteOn` startet neue Voices mit aktuellem Bend-Faktor.

### `src/main.cpp` (Integration, Core-Zuordnung unverändert)
* Globale Instanz `RefaceMidi refaceMidi` (Core 1).
* MIDI-Callbacks leiten an `refaceMidi.on*()` weiter; vier neue Callbacks (Pitch Bend, Realtime, SysEx, Activity) registriert; `refaceMidi.init(&ep, &cp_fx)` in `core1_main`.
* Oktav-Transposition (`apply_octave`) in `RefaceMidi::transposeNote()` überführt (kombiniert mit SysEx-Master-Transpose); CC-1-Sonderfall in die Protokollschicht verlagert.
* `ui_poll_usb()` ruft zusätzlich `refaceMidi.tick()` (Active Sensing).
* `ipc_apply` (Core 0, Audio-IRQ): neue Fälle `IPC_CMD_PITCH_BEND` → `ep.setPitchBend()` und `FX_EXPRESSION` → `cp_fx.setExpression()`.

### `src/pico_frontpanel.cpp`
* Panel-Änderungen senden jetzt zusätzlich die reface-CC (Wrapper `fp_send_fx_param/mode/instrument` = IPC + `RefaceMidi::tx*`; nur bei MIDI Control = on).

### `CMakeLists.txt`
* `src/midi_reface.cpp` ins Target aufgenommen.

## 3. RP2350-Optimierungen (Task 2)

Analyse-Befund: Der kritischste Pfad ist `mdaEPiano::noteOn()`, da er über `ipc_apply`
**im Audio-DMA-IRQ auf Core 0** läuft. Dort (und in `update()`/`getVolume()`) rechnete
die Engine mit **double**-Mathematik — der Cortex-M33 hat nur eine Single-Precision-FPU,
doubles laufen über die (langsamere) Software-/DCP-Emulation.

* `exp` → `expf` (8 Stellen, u. a. Decay/Release-Berechnung pro Note-On/Off, LFO-Rate, Treble-Filter),
  `pow` → `powf` (Velocity-Kurve), `sqrt` → `sqrtf`, `fabs` → `fabsf` + float-Literale;
  double-Casts `(double)note` entfernt. Numerik bleibt im float-Rahmen äquivalent.
* Nebenbefund behoben: `getVolume()` castete das `sqrt`-Ergebnis unnötig auf `uint8_t` innerhalb der float-Zuweisung.
* Bereits vorhandene Maßnahmen verifiziert und beibehalten: FX-Hot-Path in RAM (`CP_HOT`/
  `__not_in_flash_func`), int16-Delaylines, bare-metal Dual-Core mit FIFO-IPC,
  Release-Build (`-O3`, `-ffast-math`), Samples flash-resident (XIP).
* Neue MIDI-Schicht läuft vollständig auf **Core 1**; der Audio-IRQ sieht nur die
  bestehenden 32-bit-FIFO-Pakete → keine zusätzliche Latenz/Jitter im Audiopfad.
  SysEx-Puffer statisch (64 B), keine Heap-Allokation, kein printf im MIDI-Pfad.

## 4. Verhalten/Kompatibilität

* Bestehende Funktionen (UI, Presets, Instrumentwahl, FX-Bedienung) unverändert.
* Neu von außen steuerbar: alle reface-CP-CCs, SysEx-Parameter, Bulk Dump/Restore,
  Identity Reply, Active Sensing, Pitch Bend, Expression, Soft Pedal, Master Transpose/Tune,
  Empfangskanal (inkl. Omni on/off via CC 124/125).
* Abweichungen zum Original sind in `doc/MIDI_IMPLEMENTATION.md` §8 dokumentiert.

## 5. Offene Punkte / Empfehlungen

* Hardware-Test: Enumeration, CC-RX/TX mit DAW, Bulk Dump Round-Trip (z. B. MIDI-OX/SysEx Librarian).
* Optional: SYSTEM-Block in Flash persistieren (aktuell RAM-only, Defaults bei Boot).
* Optional: `_sys`-Einstellungen (RX-Kanal, MIDI Control) im OLED-SYSTEM-Menü anzeigen.
