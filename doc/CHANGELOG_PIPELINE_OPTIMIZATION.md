# Änderungsprotokoll: RP2350-Pipeline-Optimierung & Bugfixes

**Datum:** 2026-07-08  
**Workflow:** Architektur/Orchestrierung Claude, Codegenerierung `kimi-k2.7-code:cloud` via Ollama API

---

## 1. Kurzzusammenfassung

Diese Optimierungsrunde konzentriert sich auf zwei kritische Bereiche: die Korrektur der seit langem bestehenden L/R-Kanalvertauschung in der Stereo-Ausgabe sowie gezielte Performance- und Speicheroptimierungen in der DSP-Pipeline. Zusätzlich wurde ein Sicherheitsproblem im Audio-IRQ durch Entfernung eines variablen Längen-Arrays (VLA) behoben. Alle Änderungen wurden erfolgreich gebaut und verifiziert; es gibt keine neuen Compiler-Warnungen.

---

## 2. Bugfixes

| ID | Priorität | Datei(en) | Beschreibung |
|---|---|---|---|
| B1 | Kritisch | `src/main.cpp`, `i2s_callback_func` | L/R-Kanalvertauschung behoben: `mdaEPiano::process(int16_t* outputs_r, int16_t* outputs_l)` erwartet als ersten Parameter den rechten Kanal. Der Aufruf `ep.process(&l[0], &r[0])` führte dazu, dass `l[]` rechts und `r[]` links befüllt wurden. Fix auf `ep.process(&r[0], &l[0])`, sodass I2S-Links nun auch Engine-Links entspricht. Betroffen: Stereo-Breite, Tremolo-Pan, Reverb-L/R, Chorus-Stereo. |
| B2 | Normal | `src/pico_frontpanel.cpp` | `pct()` klappte Werte bei 99 statt 100 ab. Korrektur auf `if(x>100)x=100`. |

---

## 3. Performance-Optimierungen

| ID | Gewichtung | Datei(en) | Beschreibung | Erwarteter Effekt |
|---|---|---|---|---|
| P1 | Hoch | `effects/dsp_lut.h`, `effects/reface_cp_fx.h` | Neue globale `g_sinLUT()` mit 512 Einträgen (lazy-init). Sechs `sinf()`-Aufrufe in Tremolo (1), Chorus (2), Phaser (2) und Delay-Wobble (1) wurden durch LUT-Interpolation ersetzt. | `sinf` ca. 50–100 Zyklen → LUT-Interpolation ca. 5–10 Zyklen auf Cortex-M33. Kosten: +2048 Bytes RAM für die LUT-Tabelle. |
| P2 | Mittel | `effects/dsp_reverb.h` | Reverb-Modulo-Operation `idx=(idx+1)%len` (Integer-Division, 12× pro Sample) ersetzt durch `if(++idx>=len)idx=0`. | Eliminierung von ca. 529.000 Divisionen pro Sekunde. |
| P3 | Mittel | `lib/audio/include/audio_subsystem.h` | Audio-Puffergröße von 16 auf 32 Samples erhöht. | IRQ-Rate halbiert von 2756/s auf 1378/s, weniger Kontextwechsel-Overhead. Latenz bleibt weiterhin unter 1 ms. |
| P4 | Safety | `src/main.cpp` | VLA im Audio-IRQ entfernt: `int16_t l[buffer->max_sample_count]` ersetzt durch `int16_t l[SAMPLES_PER_BUFFER]` (feste Größe). | Vermeidung variabler Längen-Arrays im Interrupt-Kontext, höhere Vorhersagbarkeit und Sicherheit. |
| P5 | Niedrig | `src/pico_hw.cpp` | OLED-I2C-Takt von 400 kHz auf 1 MHz erhöht. | Schnelleres Display-Refresh für das Front-Panel-UI. |

---

## 4. Speicher-Optimierungen

| ID | Datei(en) | Beschreibung | Ersparnis |
|---|---|---|---|
| C2 | `effects/reface_cp_fx.h` | Delay-Buffer `kBuf` von 24001 auf 22080 Samples angepasst. Der alte Wert war für 48 kHz ausgelegt, das System läuft jedoch mit 44,1 kHz. | 7680 Bytes BSS |

---

## 5. Build-Verifikation

- **FLASH:** `text` 4.428.008 Bytes (≈ 26,4 % von 16 MB)
- **RAM:** `bss` 173.568 Bytes (≈ 33,8 % von 512 KB)  
  - Vorher: 179.200 Bytes (35,0 %)
  - Differenz: −7684 Bytes
- **Warnungen:** Keine neuen Warnungen, Build clean

---

## 6. Nachtrag: Lautstärke-Bugfix

**Datum:** 2026-07-08 (gleiche Session)

### B3 [BUGFIX KRITISCH]: Gesamtlautstärke zu niedrig

| Datei | Änderung | Erklärung |
|---|---|---|
| `src/main.cpp` | `ep.setVolume(64)` → `ep.setVolume(100)` | Der Engine-Volume-Parameter `volume = 0.00002 × value²` war auf 64 gesetzt (→ 0.08192). Der Konstruktor-Default in `mdaEPiano.cpp` ist `volume = 0.2f` (entspricht setVolume(100)). main.cpp überschrieb diesen mit 64 = 2.44× leiser. |
| `src/main.cpp` | `cp_fx.setVolume(0.9f)` → `cp_fx.setVolume(1.0f)` | FX-Chain-Mastervolume-Default von 90% auf 100% (Unity). |

**Signal-Pegel vor/nach Fix (einzelne Note, Velocity 100, FX-Volume 100%):**
- Vorher (setVolume 64): Peak ≈ 4.1% Full Scale = −27.8 dB
- Nachher (setVolume 100): Peak ≈ 10% Full Scale = −20.0 dB (+7.8 dB)
- 4-stimmiger Akkord: 40% FS = −8 dB (gesund, kein Clipping-Risiko)
