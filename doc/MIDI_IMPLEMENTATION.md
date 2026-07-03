# PicoFaceCP — MIDI Implementation

Äquivalent zur MIDI-Implementierung des **Yamaha reface CP**
(Quelle: *reface Data List*, `lchjs0-reface_en_dl_b0.pdf`, Seiten 11–14).
Implementiert in `include/midi_reface.h` / `src/midi_reface.cpp` (Protokollschicht, Core 1)
und `include/midi_input_usb.h` / `src/midi_input_usb.cpp` (USB-MIDI-Transport).

Version: 1.0 · Stand: 2026-07-02

---

## 1. Coverage

Diese Spezifikation beschreibt Senden und Empfang von MIDI-Daten des PicoFaceCP
über **USB MIDI** (TinyUSB Device, Cable 0). Ein DIN-MIDI-Port ist nicht bestückt.

## 2. Compliance

* MIDI 1.0 (über USB-MIDI-Class-Protokoll, 4-Byte-Event-Pakete)

## 3. Transmit / Receive Data

### 3-1 Channel Voice Messages

| Message | Status | RX | TX | Bemerkung |
|---|---|---|---|---|
| Note Off | `8nH` | ✓ | ✗ | Velocity ignoriert |
| Note On / Off | `9nH` (v=0 → Off) | ✓ | ✗ | v = 1–127; kein lokales Keyboard, daher kein TX |
| Control Change | `BnH` | ✓ | ✓ | Tabelle unten; TX nur Panel-CCs bei MIDI Control = on |
| Pitch Bend | `EnH` | ✓ | ✗ | ±2 Halbtöne, wirkt auf alle aktiven Voices |
| Program Change | `CnH` | ✓ | ✓ | **Abweichung:** PC 0–7 wählt Werkspreset (`doc/PRESETS.md`); Original hat kein PC |
| Aftertouch (Key/Ch) | `AnH`/`DnH` | ✗ | ✗ | |

Empfangskanal: SYSTEM-Parameter `MIDI receive channel` (1–16, All; Default **All**).
Sendekanal: SYSTEM-Parameter `MIDI transmit channel` (Default 1).

#### Empfangene Controller (immer aktiv)

| CC | Funktion | Umsetzung im PicoFaceCP |
|---|---|---|
| 1 | Modulation | FX-Tremolo-Depth (hörbar bei Trem/Wah-Modus ≠ Off) |
| 7 | Volume | Master-Volume der FX-Kette |
| 11 | Expression | Multiplikativ auf Master-Volume (`RefaceCpChain::setExpression`) |
| 64 | Sustain | Engine-Sustain (`mdaEPiano`, CC 0x40) |
| 66 | Sostenuto | wie Sustain (Näherung der Engine) |
| 67 | Soft Pedal | Note-On-Velocity × 0,75 solange gedrückt |

#### Empfangene/gesendete Controller (nur bei MIDI Control = on)

Identisch zur reface-CP-Tabelle (Data List S. 13):

| CC | Name | Wertebereiche (RX) | TX-Werte |
|---|---|---|---|
| 80 | TYPE | 0–21 Rd I · 22–42 Rd II · 43–64 Wr · 65–85 Clv · 86–106 Toy(=Pno) · 107–127 CP | 0 / 25 / 51 / 76 / 102 / 127 |
| 81 | DRIVE | 0–127 | 0–127 |
| 17 | TREMOLO/WAH SWITCH | 0–42 Off · 43–85 Tremolo · 86–127 Wah | 0 / 64 / 127 |
| 18 | TREMOLO/WAH DEPTH | 0–127 | 0–127 |
| 19 | TREMOLO/WAH RATE | 0–127 | 0–127 |
| 85 | CHORUS/PHASER SWITCH | 0–42 Off · 43–85 Chorus · 86–127 Phaser | 0 / 64 / 127 |
| 86 | CHORUS/PHASER DEPTH | 0–127 | 0–127 |
| 87 | CHORUS/PHASER SPEED | 0–127 | 0–127 |
| 88 | D.DELAY/A.DELAY SWITCH | 0–42 Off · 43–85 D.Delay · 86–127 A.Delay | 0 / 64 / 127 |
| 89 | DELAY DEPTH | 0–127 | 0–127 |
| 90 | DELAY TIME | 0–127 | 0–127 |
| 91 | REVERB DEPTH | 0–127 | 0–127 |

TX erfolgt bei Änderung am (virtuellen) Frontpanel (`pico_frontpanel.cpp` →
`fp_send_*`-Wrapper → `RefaceMidi::txFxParam/txFxMode/txInstrument`) bzw. bei
Preset-Wahl im Menü (`RefaceMidi::txProgram` → Program Change 0–7, siehe
Abweichung 7).

### 3-2 Channel Mode Messages (RX)

| CC | Funktion | Verhalten |
|---|---|---|
| 120 | All Sound Off | alle Voices sofort stumm (`resetVoices`) |
| 121 | Reset All Controllers | Pitch Bend center, Expression max, Sustain/Sostenuto/Soft off, Engine-Controller-Reset |
| 123 | All Notes Off | Release läuft aus; Sustain wird respektiert (`stopVoices`) |
| 124 | Omni Mode Off | wie All Notes Off; Empfangskanal := 1 |
| 125 | Omni Mode On | wie All Notes Off; Empfangskanal := All |
| 126 | Mono | wie All Sound Off; Polyphonie = 1 |
| 127 | Poly | wie All Sound Off; Polyphonie = max |

### 3-3 System Real Time Messages

**Active Sensing (`FEH`)**
* TX: alle 200 ms (`RefaceMidi::tick()`, Core-1-Loop).
* RX: Nach erstem `FEH` wird der Datenstrom überwacht; bleiben Daten länger als
  ~350 ms aus, werden alle Noten/Sounds abgeschaltet und die Überwachung beendet.

### 3-4 System Exclusive Messages

Yamaha-Header wie beim reface CP: **Yamaha ID `43H`, Group `7FH 1CH`, Model ID `04H`** (reface CP).
Die Device-Nummer `n` wird beim Empfang ignoriert; gesendet wird mit n=1 (`10H`) bzw. `00H` (Bulk).

#### 3-4-1 Identity Request / Reply (Universal Non-Realtime)

* Request (RX): `F0 7E 0n 06 01 F7`
* Reply (TX): `F0 7E 7F 06 02 43 00 41 52 06 00 00 00 7F F7` (identisch zum reface CP)

#### 3-4-2 Parameter Change (RX)

`F0 43 1n 7F 1C 04 <AddrHigh> <AddrMid> <AddrLow> <Data…> F7`
Bei Parametern mit Datengröße ≥ 2 (Master Tune) werden entsprechend viele Datenbytes übertragen.

#### 3-4-3 Bulk Dump (RX/TX)

`F0 43 0n 7F 1C <ByteCountHi> <ByteCountLo> 04 <Addr…> <Data…> <Checksum> F7`
* Byte Count = Model ID + Adresse + Daten (ohne Checksumme).
* Checksumme: Summe (Model ID + Adresse + Daten + Checksumme) ≡ 0 (untere 7 Bit).
* Ungültige Checksumme ⇒ Block wird verworfen.

#### 3-4-4 Dump Request (RX)

`F0 43 2n 7F 1C 04 <Addr…> F7` — Antwort:

| Request-Adresse | Antwort |
|---|---|
| `00 00 00` | SYSTEM Common (Byte Count 36) |
| `0E 0F 00` | Bulk Header (4) + TG Common (`30 00 00`, 20) + Bulk Footer (4) |
| `30 00 00` | TG Common einzeln (Erweiterung) |

#### 3-4-5 Parameter Request (RX)

`F0 43 3n 7F 1C 04 <Addr…> F7` — Antwort ist die entsprechende Parameter-Change-Message
mit aktuellem Wert (live aus Engine/FX-Kette gelesen).

---

## 4. MIDI Parameter Change Table (SYSTEM), Basisadresse `00 00 00`

| Addr | Size | Bereich | Parameter | Umsetzung |
|---|---|---|---|---|
| 00 | 1 | 00–0F | MIDI transmit channel | gespeichert, für CC/SysEx-TX |
| 01 | 1 | 00–0F, 10 | MIDI receive channel (1–16, All) | Kanalfilter |
| 02 | 4 | je Nibble | Master Tune (−102,4…+102,3 Cent, 0,1-Cent-Schritte, Center `0400H`) | auf Engine-Fine-Tune gemappt, **auf ±50 Cent begrenzt** (Engine-Bereich) |
| 06 | 1 | 00–01 | Local Control | gespeichert (kein lokales Keyboard vorhanden) |
| 07 | 1 | 34–4C | Master Transpose −12…+12 Halbtöne | auf Note-Ereignisse angewendet (zusätzlich zur UI-Oktave) |
| 0B | 1 | 00–01 | Sustain Pedal Select (FC3/FC4-5) | gespeichert (Half-Damper nicht unterstützt) |
| 0C | 1 | 00–01 | Auto Power-Off | gespeichert |
| 0D | 1 | 00–01 | Speaker Output | gespeichert |
| 0E | 1 | 00–01 | MIDI Control | schaltet CC 17–19/80/81/85–91 (RX+TX) |
| übrige | 1 | — | reserved | gespeichert |

Gesamtgröße Block: 32 Bytes (wie reface CP).

## 5. MIDI Parameter Change Table (Tone Generator), Basisadresse `30 00 00`

| Addr | Bereich | Parameter | Ziel |
|---|---|---|---|
| 00 | 00–7F | Volume | FX-Kette Master-Volume |
| 01 | — | reserved | — |
| 02 | 00–05 | Wave Type (Rd I, Rd II, Wr, Clv, Toy→Pno, CP) | `mdaEPiano::setInstrument` |
| 03 | 00–7F | Drive | FX Drive |
| 04 | 00–02 | Effect 1 Type (thru/Tremolo/Wah) | FX Trem/Wah-Modus |
| 05 | 00–7F | Effect 1 Depth | FX Trem/Wah Depth |
| 06 | 00–7F | Effect 1 Rate | FX Trem/Wah Rate |
| 07 | 00–02 | Effect 2 Type (thru/Chorus/Phaser) | FX Cho/Pha-Modus |
| 08 | 00–7F | Effect 2 Depth | FX Cho/Pha Depth |
| 09 | 00–7F | Effect 2 Speed | FX Cho/Pha Speed |
| 0A | 00–02 | Effect 3 Type (thru/D.Delay/A.Delay) | FX Delay-Modus |
| 0B | 00–7F | Effect 3 Depth | FX Delay Depth |
| 0C | 00–7F | Effect 3 Time | FX Delay Time |
| 0D | 00–7F | Reverb Depth | FX Reverb |
| 0E | 2 | reserved | — |

Gesamtgröße Block: 16 Bytes. Lesezugriffe (Parameter Request / Dump) liefern
Live-Werte aus `RefaceCpChain`-Gettern bzw. `mdaEPiano::getCurrentInstrument()`.

## 6. Bulk Dump Blocks

| Block | Beschreibung | Byte Count (dec/hex) | Top-Adresse |
|---|---|---|---|
| SYSTEM | Common | 36 / 24H | `00 00 00` |
| TG | Bulk Header | 4 / 04H | `0E 0F 00` |
| TG | Common | 20 / 14H | `30 00 00` |
| TG | Bulk Footer | 4 / 04H | `0F 0F 00` |

## 7. MIDI Implementation Chart

```
PicoFaceCP                    MIDI Implementation Chart        Version: 1.0
Model: PicoFaceCP (reface CP kompatibel)                       Datum: 2026-07-02

Function...        Transmitted        Recognized       Remarks
Basic   Default    1                  All (1-16)
Channel Changed    1-16 (SysEx)       1-16, All (SysEx)
Mode    Default    3                  1
        Messages   x                  1,3 (CC 124-127)
Note Number        x                  0-127            Transpose ±12 + Oktave ±24
Velocity  NoteOn   x                  o v=1-127
          NoteOff  x                  x
After Touch        x                  x
Pitch Bend         x                  o                ±2 Halbtöne
Control    1,7,11  x                  o                Mod->Trem-Depth, Vol, Expr
Change     64,66,67 x                 o                Sustain, Sostenuto, Soft
           17-19   o *1               o *1
           80,81   o *1               o *1
           85-91   o *1               o *1
Program Change     o (0-7)            o (0-7)          Werkspresets (Abweichung)
System Exclusive   o                  o                Param Change/Request,
                                                       Bulk Dump/Request, Identity
System Common      x                  x
System Real Time   o (FE)             o (FE)           Active Sensing 200ms/350ms
Aux: All Sound Off x                  o (120,126,127)
     Reset All Ctrl x                 o (121)
     Local On/Off  x                  o (SysEx, gespeichert)
     All Notes Off x                  o (123-125)
     Active Sense  o                  o
     Reset         x                  x

*1: Gesendet und erkannt nur bei MIDI Control = on (SYSTEM-Param 0E).
Mode 1: OMNI ON, POLY   Mode 3: OMNI OFF, POLY   o: Yes  x: No
```

## 8. Bewusste Abweichungen zum Original

1. **Kein Note-TX / kein lokales Keyboard** — PicoFaceCP ist ein Tongenerator;
   Local Control und Notensendung entfallen (Parameter wird akzeptiert/gespeichert).
2. **Master Tune** wird auf den Fine-Tune-Bereich der mdaEPiano-Engine begrenzt (±50 Cent statt ±102,4).
3. **CC 1 (Modulation)** steuert die FX-Tremolo-Depth (Designentscheidung der
   Parameter-Deduplication; das Original moduliert Pan/Volume abhängig vom Voice-Typ —
   klanglich äquivalent, da derselbe Tremolo-Block Pan- bzw. Volume-Modus je Voice-Typ wählt).
4. **Sostenuto (CC 66)** wird von der Engine wie Sustain behandelt (Näherung).
5. **Reset All Controllers** setzt die per Panel eingestellte Tremolo-Depth nicht zurück
   (nur die Spielcontroller: Pitch Bend, Expression, Pedale).
6. **Auto Power-Off / Speaker Output / Sustain Pedal Select** werden gespeichert und
   im Bulk Dump geführt, haben aber keine Hardware-Funktion.
7. **Program Change ist implementiert (Original: nicht vorhanden).** PC 0–7
   wählt eines der 8 statischen Werkspresets (Instrument + Engine-Params +
   FX-Kette, `doc/PRESETS.md`); Werte ≥ 8 werden ignoriert. TX bei Preset-Wahl
   im Menü. Nicht durch die MIDI-Control-Einstellung gegated (eigene
   Erweiterung, wie Noten nur durch den Empfangskanal gefiltert). CC 80 (TYPE)
   behält die Original-Bedeutung (Instrumentwahl in 6 Zonen).
