# Werkspresets

## Überblick

Die Firmware stellt **8 statische Werkspresets** zur Verfügung, die die früheren
mdaEPiano-Programme (Default, Bright, Mellow, Autopan, Tremolo) ablösen.
Jedes Preset besteht aus:

- einem **Instrument** (Rd I, Rd II, Wr, Clv, Pno, CP),
- **12 Engine-Parametern** sowie
- der kompletten **FX-Ketten-Einstellung** (Drive → Trem/Wah → Cho/Pha → Delay → Reverb).

Klangliche Orientierung: _velvetkeys_-Preset-Sammlung
(github.com/Michi71/velvetkeys). Bewusste Abweichung vom Original-Yamaha
Reface CP, das keine Presets besitzt.

---

## Preset-Auswahl

### (a) Menü-gesteuert

1. **Selector lang drücken** → MENU öffnen.
2. Eintrag **Presets** anwählen.
3. Encoder blättert durch P0…P7.
4. Preset wird **sofort angewendet**.

### (b) MIDI Program Change

Ein **Program Change 0–7** wählt das entsprechende Preset P0…P7; Werte ≥ 8
werden ignoriert. Bei Preset-Wahl im Menü sendet die Firmware den passenden
Program Change auf dem TX-Kanal.

> **Hinweis:** Das Original-Reface-CP implementiert Program Change gar nicht —
> dies ist die einzige Preset-bedingte MIDI-Abweichung. **CC80 (TYPE) behält
> die Original-Bedeutung** und wählt wie beim Yamaha-Gerät das Instrument
> (6 Zonen); die Instrumentwahl am VOICE-Screen sendet weiterhin CC80.
> Program Change ist nicht an die MIDI-Control-Einstellung gebunden, nur an
> den Empfangskanal-Filter.

---

## Werkspresettabelle

| #  | Name          | Instrument | Effekte                                           | Charakter                              |
|----|---------------|------------|---------------------------------------------------|----------------------------------------|
| 0  | Rd I Classic  | Rd I       | Reverb 15%                                        | neutrales Rhodes Mark I                |
| 1  | Rd II Chorus  | Rd II      | Chorus 45%/30%, Reverb 20%, Treble +10%            | klassisches Suitcase mit Analog-Chorus |
| 2  | Phaser Rd     | Rd II      | Phaser 60%/35%, Reverb 20%                         | deutlicher Phaser-Sweep                |
| 3  | Wurli Trem    | Wr         | Tremolo 55%/50%, Drive 20%, Reverb 15%            | Wurlitzer 200A mit Röhren-Anzerrung    |
| 4  | Funky Clv     | Clv        | Touch-Wah 70%/50%, Reverb 10%                     | funkiges Clavinet                      |
| 5  | Piano Hall    | Pno        | Reverb 45%, Release +10%                          | akustisches Piano im Saal             |
| 6  | CP Delay      | CP         | Analog-Delay 35%/45%, Drive 15%, Reverb 20%       | CP80 mit Bandecho-Charakter           |
| 7  | Space Rd      | Rd I       | Chorus 50%/30%, Digital-Delay 30%/50%, Reverb 40%, Release +10% | ambienter Kombi-Effekt |

---

## Technische Details

Die Presettabelle ist statisch in **`src/presets.cpp`** definiert
(Struct `CpPreset`, Deklaration in `include/presets.h`).

Die Anwendung erfolgt **atomar auf Core 0** über den Befehl
`IPC_CMD_PROGRAM → preset_apply()`. Dieser Aufruf setzt:

- das Instrument,
- die 12 Engine-Parameter,
- alle FX-Modi und -Werte.

Nach dem Laden greift die **normale Settings-Persistenz**: Der resultierende
Panel-Zustand wird gespeichert, **nicht** der Preset-Index. Ein erneutes
Einschalten liefert also den zuletzt aktiven Klang, unabhängig vom Ursprung.

### Neutralisierte Engine-Parameter

Die Engine-Parameter **4 (Modulation)**, **5 (LFO Rate)** und
**11 (Overdrive)** werden von den Presets neutralisiert, da die FX-Kette
autoritativ ist und diese Werte sonst doppelt beeinflussen würde.
Diese Parameter sind im **V.PARAMS-Screen** ausgeblendet.
