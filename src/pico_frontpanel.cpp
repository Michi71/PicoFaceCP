// pico_frontpanel.cpp
//
// Virtual "front panel" UI for the Reface CP (RP2350 + SH1106 128x64 OLED,
// single rotary encoder + push button).
//
// Layout (128x64):
//
//   VOICE Rd I                     <- fixed header (8x13B), always visible
//   ------------
//   DRV   40                       <- scroll list of effect blocks (8x13B);
//   TRM  T 25/60                      only as many rows as fit are shown; the
//   ...                              selected block is always scrolled into view
//   ------------                   <- (up/down arrows on the right when more exist)
//   Depth 25  Rate 60              <- context line (6x10), active value inverted
//
// One encoder edits the highlighted value live (sent to Core 0 via the FIFO).
// A short press steps to the next parameter; a long press (>= 500 ms) on a
// Tremolo/Wah, Chorus/Phaser or Delay block cycles that block's 3-position
// mode (Off -> A -> B), like the hardware toggle.  On the other entries a long
// press steps back.  The MENU entry's short press opens Presets / System.

#include "pico_frontpanel.h"
#include "pico_userinterface.h"   // ui_poll_usb, ui_wait_button_release, selection list, program select
#include "pico/stdlib.h"
#include "ipc.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static inline float clamp01f(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Normalised 0..1 -> two-digit 00..99 (the top step is shown as 99 so the
// compact columns never overflow).
static int pct(float v) {
    int x = (int)(v * 100.0f + 0.5f);
    if (x < 0) x = 0;
    if (x > 99) x = 99;
    return x;
}

// 3-position mode -> single-letter column glyph.  '-' = off.
static char modeLetter(int m, char onA, char onB) {
    if (m == 1) return onA;
    if (m == 2) return onB;
    return '-';
}

static const char* kTwNames[3]  = { "Off", "Tremolo", "Wah" };
static const char* kCpNames[3]  = { "Off", "Chorus", "Phaser" };
static const char* kDlyNames[3] = { "Off", "Digital", "Analog" };

// Flat cursor table: (block, sub).  block: 0=VOICE,1=DRV,2=TRM,3=CHO,4=DLY,
// 5=REV,6=VOL,7=MENU.  sub 0=primary,1=depth,2=rate/speed/time.  The mode is
// NOT a cursor entry -- it is cycled with a long press on its block.
struct FpEntry { uint8_t blk; uint8_t sub; };
static const FpEntry kEntries[] = {
    { 0, 0 },  // 0  VOICE
    { 1, 0 },  // 1  DRV Drive
    { 2, 1 },  // 2  TRM Depth
    { 2, 2 },  // 3  TRM Rate
    { 3, 1 },  // 4  CHO Depth
    { 3, 2 },  // 5  CHO Speed
    { 4, 1 },  // 6  DLY Depth
    { 4, 2 },  // 7  DLY Time
    { 5, 0 },  // 8  REV Reverb
    { 6, 0 },  // 9  VOL Volume
    { 7, 0 },  // 10 MENU
};
static const int kNEntries = (int)(sizeof(kEntries) / sizeof(kEntries[0]));

// Scroll-list items are blocks 1..7 (DRV, TRM, CHO, DLY, REV, VOL, MENU).
#define LIST_FIRST_BLK 1
#define LIST_NITEMS    7

#define LONG_PRESS_MS   500
#define MODE_FLASH_MS   1200

// Draw a string inverted (filled background, foreground pixels cleared).
static void drawInvStr(u8g2_t* u, int x, int y, const char* s) {
    int w = u8g2_GetStrWidth(u, s);
    int asc = u8g2_GetAscent(u);
    int desc = u8g2_GetDescent(u);
    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, x, y - asc, w, asc - desc);
    u8g2_SetDrawColor(u, 0);
    u8g2_DrawStr(u, x, y, s);
    u8g2_SetDrawColor(u, 1);
}

// A run of text segments, any of which may be drawn inverted.
struct Seg { const char* text; bool inv; };

static void drawSegments(u8g2_t* u, int x, int y, const Seg* segs, int n) {
    for (int i = 0; i < n; i++) {
        if (segs[i].inv) drawInvStr(u, x, y, segs[i].text);
        else             u8g2_DrawStr(u, x, y, segs[i].text);
        x += u8g2_GetStrWidth(u, segs[i].text);
    }
}

// Fill `buf` with the scroll-list label for effect block `blk` (1..7).
static void blockLabel(char* buf, size_t n, int blk,
                        float drv, int twM, float twD, float twR,
                        int cpM, float cpD, float cpS,
                        int dlyM, float dlyD, float dlyT,
                        float rev, float vol) {
    switch (blk) {
        case 1: snprintf(buf, n, "DRV   %02d", pct(drv)); break;
        case 2: snprintf(buf, n, "TRM  %c %02d/%02d", modeLetter(twM, 'T', 'W'), pct(twD), pct(twR)); break;
        case 3: snprintf(buf, n, "CHO  %c %02d/%02d", modeLetter(cpM, 'C', 'P'), pct(cpD), pct(cpS)); break;
        case 4: snprintf(buf, n, "DLY  %c %02d/%02d", modeLetter(dlyM, 'D', 'A'), pct(dlyD), pct(dlyT)); break;
        case 5: snprintf(buf, n, "REV   %02d", pct(rev)); break;
        case 6: snprintf(buf, n, "VOL   %02d", pct(vol)); break;
        case 7: snprintf(buf, n, ">> Menu"); break;
        default: buf[0] = 0; break;
    }
}

// ---------------------------------------------------------------------------
// System menu (About only -- volume now lives on the front panel)
// ---------------------------------------------------------------------------

static void showAbout(u8g2_t* u8g2, Encoder* enc, PushButton* bt)
{
    char line1[32];
    char line2[40];
#ifdef PICO_PROGRAM_NAME
    snprintf(line1, sizeof(line1), "%s", PICO_PROGRAM_NAME);
#else
    snprintf(line1, sizeof(line1), "PicoFaceCP");
#endif
#ifdef PICO_PROGRAM_VERSION_STRING
    snprintf(line2, sizeof(line2), "%s", PICO_PROGRAM_VERSION_STRING);
#else
    snprintf(line2, sizeof(line2), "Reface CP emulator");
#endif

    for (;;) {
        u8g2_FirstPage(u8g2);
        do {
            u8g2_SetFont(u8g2, u8g2_font_8x13B_tf);
            u8g2_SetFontPosBaseline(u8g2);
            u8g2_DrawStr(u8g2, 0, 12, "ABOUT");
            u8g2_DrawHLine(u8g2, 0, 14, 128);
            int w = u8g2_GetStrWidth(u8g2, line1);
            u8g2_DrawStr(u8g2, (128 - w) / 2, 34, line1);
            w = u8g2_GetStrWidth(u8g2, line2);
            u8g2_DrawStr(u8g2, (128 - w) / 2, 50, line2);
            u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
            u8g2_DrawStr(u8g2, 0, 62, "press: back");
        } while (u8g2_NextPage(u8g2));

        for (;;) {
            ui_poll_usb();
            (void)enc->delta();
            if (bt->ReadButton() == PushButton::PRESSED) {
                ui_wait_button_release(bt);
                return;
            }
        }
    }
}

static void openSystem(u8g2_t* u8g2, Encoder* enc, PushButton* bt)
{
    for (;;) {
        uint8_t sel = pico_UserInterfaceSelectionList(u8g2, enc, bt, "SYSTEM", 0,
                        "About\n<< BACK");
        if (sel == 1) {
            showAbout(u8g2, enc, bt);
        } else {
            return;  // 2 = BACK, 255 = timeout
        }
    }
}

// ---------------------------------------------------------------------------
// Main menu: Presets + System
// ---------------------------------------------------------------------------

static void openMainMenu(u8g2_t* u8g2, Encoder* enc, PushButton* bt,
                         mdaEPiano* ep, RefaceCpChain* fx)
{
    (void)fx;
    for (;;) {
        uint8_t sel = pico_UserInterfaceSelectionList(u8g2, enc, bt, "MENU", 0,
                        "Presets\nSystem\n<< BACK");
        if (sel == 1) {
            pico_UserInterfaceProgramSelect(u8g2, enc, bt, ep);
        } else if (sel == 2) {
            openSystem(u8g2, enc, bt);
        } else {
            return;  // 3 = BACK, 255 = timeout
        }
    }
}

// ---------------------------------------------------------------------------
// Front panel home screen
// ---------------------------------------------------------------------------

void pico_UserInterfaceFrontPanel(u8g2_t* u8g2, Encoder* enc, PushButton* bt,
                                  mdaEPiano* ep, RefaceCpChain* fx)
{
    const int nInstr = (int)ep->getInstrumentCount();

    // Cache of the live FX state (mirrors Core 0's RefaceCpChain).
    int   instr = ep->getCurrentInstrument();
    float drv   = fx->getDrive();
    int   twM   = fx->getTremWahMode();
    float twD   = fx->getTremWahDepth();
    float twR   = fx->getTremWahRate();
    int   cpM   = fx->getChoPhaMode();
    float cpD   = fx->getChoPhaDepth();
    float cpS   = fx->getChoPhaSpeed();
    int   dlyM  = fx->getDelayMode();
    float dlyD  = fx->getDelayDepth();
    float dlyT  = fx->getDelayTime();
    float rev   = fx->getReverbDepth();
    float vol   = fx->getVolume();

    int      cursor         = 0;
    int      first          = 0;   // first visible scroll-list item (0-based)
    bool     btnHeld        = false;
    uint32_t pressT         = 0;
    uint32_t modeFlashUntil = 0;

    for (;;) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        const FpEntry  curE = kEntries[cursor];

        // ---- render ----
        u8g2_FirstPage(u8g2);
        do {
            u8g2_SetFontPosBaseline(u8g2);

            // -- header (8x13B) --
            u8g2_SetFont(u8g2, u8g2_font_8x13B_tf);
            const int hAsc  = u8g2_GetAscent(u8g2);
            const int hDesc = u8g2_GetDescent(u8g2);
            const int hdrY  = hAsc;
            const int sep1  = hAsc - hDesc;

            char hdr[24];
            snprintf(hdr, sizeof(hdr), "VOICE %s", ep->getInstrumentName(instr));
            if (curE.blk == 0) {
                u8g2_SetDrawColor(u8g2, 1);
                u8g2_DrawBox(u8g2, 0, 0, 128, hAsc - hDesc);
                u8g2_SetDrawColor(u8g2, 0);
            } else {
                u8g2_SetDrawColor(u8g2, 1);
            }
            u8g2_DrawStr(u8g2, 0, hdrY, hdr);
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawHLine(u8g2, 0, sep1, 128);

            // -- context line metrics (6x10) so we know the list area bottom --
            u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
            const int cAsc  = u8g2_GetAscent(u8g2);
            const int cDesc = u8g2_GetDescent(u8g2);
            const int cLh   = cAsc - cDesc;
            const int ctxTop  = 64 - cLh;
            const int ctxBase = ctxTop + cAsc;
            const int sep2    = ctxTop - 1;

            // -- scroll list (8x13B) --
            u8g2_SetFont(u8g2, u8g2_font_8x13B_tf);
            const int rAsc  = u8g2_GetAscent(u8g2);
            const int rDesc = u8g2_GetDescent(u8g2);
            const int rLh   = rAsc - rDesc;

            const int listTop    = sep1 + 1;
            const int listBottom = sep2 - 1;
            const int listAreaH  = listBottom - listTop + 1;
            int visible = listAreaH / rLh;
            if (visible < 1) visible = 1;
            if (visible > LIST_NITEMS) visible = LIST_NITEMS;
            int maxFirst = LIST_NITEMS - visible;
            if (maxFirst < 0) maxFirst = 0;
            if (first > maxFirst) first = maxFirst;
            if (first < 0) first = 0;
            // re-clamp selection into the (now known) window
            if (curE.blk >= LIST_FIRST_BLK) {
                const int sel = curE.blk - LIST_FIRST_BLK;
                if (sel < first) first = sel;
                else if (sel >= first + visible) first = sel - visible + 1;
                if (first > maxFirst) first = maxFirst;
                if (first < 0) first = 0;
            }

            const int firstRowTop = listTop + (listAreaH - visible * rLh) / 2;

            for (int r = 0; r < visible; r++) {
                const int item = first + r;            // 0..LIST_NITEMS-1
                const int blk  = LIST_FIRST_BLK + item;
                const int baseY = firstRowTop + rAsc + r * rLh;
                char line[24];
                blockLabel(line, sizeof(line), blk,
                           drv, twM, twD, twR, cpM, cpD, cpS,
                           dlyM, dlyD, dlyT, rev, vol);
                const bool inv = (curE.blk == blk);
                if (inv) {
                    u8g2_SetDrawColor(u8g2, 1);
                    u8g2_DrawBox(u8g2, 0, baseY - rAsc, 128, rLh);
                    u8g2_SetDrawColor(u8g2, 0);
                } else {
                    u8g2_SetDrawColor(u8g2, 1);
                }
                u8g2_DrawStr(u8g2, 1, baseY, line);
                u8g2_SetDrawColor(u8g2, 1);
            }

            // -- scroll arrows on the right edge --
            if (first > 0) {
                int y = firstRowTop + 1;
                u8g2_DrawTriangle(u8g2, 124, y, 120, y + 5, 127, y + 5);
            }
            if (first + visible < LIST_NITEMS) {
                int y = firstRowTop + visible * rLh - 2;
                u8g2_DrawTriangle(u8g2, 124, y, 120, y - 5, 127, y - 5);
            }

            // -- context line (6x10) --
            u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
            u8g2_DrawHLine(u8g2, 0, sep2, 128);

            Seg sg[6];
            int ns = 0;
            char aBuf[4], bBuf[4];
            const bool modeFlash = (now < modeFlashUntil) &&
                                   (curE.blk == 2 || curE.blk == 3 || curE.blk == 4);

            switch (curE.blk) {
                case 0: {  // VOICE
                    u8g2_DrawStr(u8g2, 0, ctxBase, "Voice ");
                    int x = u8g2_GetStrWidth(u8g2, "Voice ");
                    drawInvStr(u8g2, x, ctxBase, ep->getInstrumentName(instr));
                    break;
                }
                case 1: {  // DRV
                    snprintf(aBuf, sizeof(aBuf), "%02d", pct(drv));
                    sg[ns++] = { "Drive ", false };
                    sg[ns++] = { aBuf, true };
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 2: {  // TRM
                    if (modeFlash) {
                        sg[ns++] = { "Mode ", false };
                        sg[ns++] = { kTwNames[twM], true };
                    } else {
                        snprintf(aBuf, sizeof(aBuf), "%02d", pct(twD));
                        snprintf(bBuf, sizeof(bBuf), "%02d", pct(twR));
                        sg[ns++] = { "Depth ", false };
                        sg[ns++] = { aBuf, curE.sub == 1 };
                        sg[ns++] = { "  Rate ", false };
                        sg[ns++] = { bBuf, curE.sub == 2 };
                    }
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 3: {  // CHO
                    if (modeFlash) {
                        sg[ns++] = { "Mode ", false };
                        sg[ns++] = { kCpNames[cpM], true };
                    } else {
                        snprintf(aBuf, sizeof(aBuf), "%02d", pct(cpD));
                        snprintf(bBuf, sizeof(bBuf), "%02d", pct(cpS));
                        sg[ns++] = { "Depth ", false };
                        sg[ns++] = { aBuf, curE.sub == 1 };
                        sg[ns++] = { "  Speed ", false };
                        sg[ns++] = { bBuf, curE.sub == 2 };
                    }
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 4: {  // DLY
                    if (modeFlash) {
                        sg[ns++] = { "Mode ", false };
                        sg[ns++] = { kDlyNames[dlyM], true };
                    } else {
                        snprintf(aBuf, sizeof(aBuf), "%02d", pct(dlyD));
                        snprintf(bBuf, sizeof(bBuf), "%02d", pct(dlyT));
                        sg[ns++] = { "Depth ", false };
                        sg[ns++] = { aBuf, curE.sub == 1 };
                        sg[ns++] = { "  Time ", false };
                        sg[ns++] = { bBuf, curE.sub == 2 };
                    }
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 5: {  // REV
                    snprintf(aBuf, sizeof(aBuf), "%02d", pct(rev));
                    sg[ns++] = { "Reverb ", false };
                    sg[ns++] = { aBuf, true };
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 6: {  // VOL
                    snprintf(aBuf, sizeof(aBuf), "%02d", pct(vol));
                    sg[ns++] = { "Volume ", false };
                    sg[ns++] = { aBuf, true };
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
                case 7: {  // MENU
                    sg[ns++] = { ">> Open Menu", true };
                    drawSegments(u8g2, 0, ctxBase, sg, ns);
                    break;
                }
            }
            u8g2_SetDrawColor(u8g2, 1);
        } while (u8g2_NextPage(u8g2));

        // ---- input poll ----
        for (;;) {
            ui_poll_usb();
            const uint32_t now = to_ms_since_boot(get_absolute_time());

            // button edge + long-press detection (button is level-triggered)
            const bool p = (bt->ReadButton() == PushButton::PRESSED);
            if (p && !btnHeld) { btnHeld = true; pressT = now; }
            if (!p && btnHeld) {
                btnHeld = false;
                const uint32_t dur = now - pressT;
                if (dur >= LONG_PRESS_MS) {
                    // long press: cycle the block's mode (TRM/CHO/DLY), else go back
                    const uint8_t blk = kEntries[cursor].blk;
                    if (blk == 2) { twM = (twM + 1) % 3; ipc_send_fx_mode(FXM_TW_MODE, (uint8_t)twM); modeFlashUntil = now + MODE_FLASH_MS; }
                    else if (blk == 3) { cpM = (cpM + 1) % 3; ipc_send_fx_mode(FXM_CP_MODE, (uint8_t)cpM); modeFlashUntil = now + MODE_FLASH_MS; }
                    else if (blk == 4) { dlyM = (dlyM + 1) % 3; ipc_send_fx_mode(FXM_DLY_MODE, (uint8_t)dlyM); modeFlashUntil = now + MODE_FLASH_MS; }
                    else { cursor = (cursor == 0) ? kNEntries - 1 : cursor - 1; }
                } else {
                    // short press: next entry, or open menu
                    if (kEntries[cursor].blk == 7) {
                        openMainMenu(u8g2, enc, bt, ep, fx);
                        instr = ep->getCurrentInstrument();  // safety refresh
                    } else {
                        cursor = (cursor + 1) % kNEntries;
                    }
                }
                break;  // re-render
            }

            // encoder -> edit current value (live IPC)
            int32_t d = enc->delta();
            if (d != 0 && kEntries[cursor].blk != 7) {
                const int dir = (d > 0) ? 1 : -1;
                switch (cursor) {
                    case 0:  instr = (instr + dir + nInstr) % nInstr; ipc_send_instrument((uint8_t)instr); break;
                    case 1:  drv   = clamp01f(drv   + d * 0.02f); ipc_send_fx_param(FX_DRIVE, drv); break;
                    case 2:  twD   = clamp01f(twD   + d * 0.02f); ipc_send_fx_param(FX_TW_DEPTH, twD); break;
                    case 3:  twR   = clamp01f(twR   + d * 0.02f); ipc_send_fx_param(FX_TW_RATE, twR); break;
                    case 4:  cpD   = clamp01f(cpD   + d * 0.02f); ipc_send_fx_param(FX_CP_DEPTH, cpD); break;
                    case 5:  cpS   = clamp01f(cpS   + d * 0.02f); ipc_send_fx_param(FX_CP_SPEED, cpS); break;
                    case 6:  dlyD  = clamp01f(dlyD  + d * 0.02f); ipc_send_fx_param(FX_DLY_DEPTH, dlyD); break;
                    case 7:  dlyT  = clamp01f(dlyT  + d * 0.02f); ipc_send_fx_param(FX_DLY_TIME, dlyT); break;
                    case 8:  rev   = clamp01f(rev   + d * 0.02f); ipc_send_fx_param(FX_REVERB, rev); break;
                    case 9:  vol   = clamp01f(vol   + d * 0.02f); ipc_send_fx_param(FX_VOLUME, vol); break;
                    default: break;  // MENU: encoder is a no-op
                }
                break;  // re-render
            }
        }
    }
}
