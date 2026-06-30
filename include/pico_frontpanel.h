// pico_frontpanel.h
//
// "Virtual front panel" home screen for the Reface CP (RP2350 + SH1106 128x64).
//
// The display mimics the real instrument's front plate: a VOICE header line,
// five always-visible effect blocks (DRV / TRM / CHO / DLY / REV) and a
// context line at the bottom that shows the detailed parameters of the
// currently selected block.  One encoder edits values live, a short press
// steps to the next parameter, a long press steps back.  No sub-menus are
// opened for effect editing.
//
// The Presets / System main menu is reached by cycling the cursor past REV
// onto the MENU entry (top-right of the header) and pressing.

#pragma once

#include "u8g2.h"
#include "encoder.h"
#include "push_button.h"
#include "mdaEPiano.h"
#include "reface_cp_chain.h"

// Home screen.  Never returns (loops forever as the instrument UI).
void pico_UserInterfaceFrontPanel(u8g2_t* u8g2, Encoder* enc, PushButton* bt,
                                  mdaEPiano* ep, RefaceCpChain* fx);
