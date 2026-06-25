#include <stdio.h>
#include "pico/binary_info.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"   // __wfi
#include "project_config.h"
#include "ipc.h"
#include "midi_input_usb.h"
#include "audio_subsystem.h"
#include "pico_hw.h"

#if __has_include("bsp/board_api.h")
#include "bsp/board_api.h"
#else
#include "bsp/board.h"
#endif

#include "u8g2.h"

#include "encoder.h"
#include "push_button.h"
#include "pico_userinterface.h"
#include "pico_cp_ui.h"

#include "arduino_compat.h"
#include "mdaEPiano.h"
#include "cp_audio.h"

#define USE_DIN_MIDI 0
#define DEBUG_MIDI 0

// Set to 0 if you want to play notes via USB MIDI
#define PLAY_RANDOM_NOTES 1

#ifdef __cplusplus
extern "C"
{
#endif

// ---------------------------------------------------------------------------
// Globals
//   ep / cp_fx are owned and processed exclusively by Core 0 (audio master).
//   enc / bt / u8g2 / usbmidi are owned and serviced exclusively by Core 1.
//   Core 1 -> Core 0 communication goes through the SIO FIFO (see ipc.h).
// ---------------------------------------------------------------------------
Encoder enc(pio1, 0, {PIN_ENC_A, PIN_ENC_B});
PushButton bt(PIN_ENC_BTN, 50);

// Create the Audio Buffer
audio_buffer_pool_t *ap;

// Create the Oled screen
u8g2_t u8g2;

mdaEPiano ep(96);

// Reface CP effect chain (post-processes the engine output)
RefaceCpChain cp_fx;

MIDIInputUSB usbmidi;

#define logoWidth 32
#define logoHeight 32

const unsigned char logo [] = //http://javl.github.io/image2cpp/ "Invert image colors" and "Swap bits in byte"
{
	0xfe, 0xff, 0xff, 0xff, 0x06, 0x00, 0x00, 0xc0, 0x03, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0xc0,
	0x03, 0x00, 0x00, 0xc0, 0x03, 0x00, 0x00, 0xf0, 0x03, 0x00, 0x00, 0xbc, 0x03, 0x00, 0xf0, 0x9f,
	0x03, 0xf0, 0xff, 0x83, 0x83, 0xff, 0xc7, 0x83, 0xe3, 0xc7, 0xc7, 0x83, 0xf3, 0xc7, 0xc7, 0x83,
	0x9f, 0xc7, 0xc7, 0x83, 0x8f, 0xc7, 0xc7, 0x83, 0x87, 0xc7, 0xc7, 0x83, 0x87, 0xc7, 0xc7, 0x83,
	0x87, 0xc7, 0xc7, 0x83, 0x83, 0xc7, 0xc7, 0x83, 0x83, 0xc7, 0xc7, 0x83, 0x83, 0xc7, 0xc7, 0x83,
	0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81,
	0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81, 0x03, 0x03, 0x81, 0x81,
	0x03, 0x03, 0x81, 0x81, 0x06, 0x03, 0x81, 0xc1, 0xfe, 0xff, 0xff, 0xff, 0xf8, 0xff, 0xff, 0x3f
};

void core1_main(void);

// ===========================================================================
// MIDI callbacks (run on Core 1) -- now forward events to Core 0 via the FIFO
// ===========================================================================
void note_on_callback(uint8_t note, uint8_t level, uint8_t channel) {
    if (level > 0) {
        ipc_send_note_on(note, level);
        gpio_put(PIN_LED, 1);
    } else {
        ipc_send_note_off(note);
        gpio_put(PIN_LED, 0);
    }
}

void note_off_callback(uint8_t note, uint8_t level, uint8_t channel) {
    ipc_send_note_off(note);
    gpio_put(PIN_LED, 0);
}

void cc_callback(uint8_t cc, uint8_t value, uint8_t channel) {
    if (cc == 1) {                                  // mod wheel -> FX tremolo depth
        ipc_send_fx_param(FX_TW_DEPTH, (float)value / 127.0f);
    } else {
        ipc_send_cc(cc, value);
    }
}

// ===========================================================================
// ipc_apply (runs on Core 0) -- apply one FIFO packet to the engine / FX
// ===========================================================================
static void ipc_apply(uint32_t pkt) {
    switch (ipc_type(pkt)) {
        case IPC_CMD_NOTE_ON:
            ep.noteOn(ipc_d1(pkt), ipc_d2(pkt));
            break;
        case IPC_CMD_NOTE_OFF:
            ep.noteOff(ipc_d1(pkt));
            break;
        case IPC_CMD_CC:
            ep.processMidiController(ipc_d1(pkt), (uint8_t)ipc_d2(pkt));
            break;
        case IPC_CMD_FX_PARAM: {
            float v = ipc_u16_to_f(ipc_d2(pkt));
            switch (ipc_d1(pkt)) {
                case FX_DRIVE: cp_fx.setDrive(v); break;
                case FX_TW_DEPTH: cp_fx.setTremWahDepth(v); break;
                case FX_TW_RATE: cp_fx.setTremWahRate(v); break;
                case FX_CP_DEPTH: cp_fx.setChoPhaDepth(v); break;
                case FX_CP_SPEED: cp_fx.setChoPhaSpeed(v); break;
                case FX_DLY_DEPTH: cp_fx.setDelayDepth(v); break;
                case FX_DLY_TIME: cp_fx.setDelayTime(v); break;
                case FX_REVERB: cp_fx.setReverbDepth(v); break;
                case FX_VOLUME: cp_fx.setVolume(v); break;
            }
        } break;
        case IPC_CMD_FX_MODE: {
            int m = (int)ipc_d2(pkt);
            switch (ipc_d1(pkt)) {
                case FXM_TW_MODE: cp_fx.setTremWahMode(m); break;
                case FXM_CP_MODE: cp_fx.setChoPhaMode(m); break;
                case FXM_DLY_MODE: cp_fx.setDelayMode(m); break;
            }
        } break;
        case IPC_CMD_VOICE_PARAM:
            ep.setParameter(ipc_d1(pkt), ipc_u16_to_f(ipc_d2(pkt)));
            break;
        case IPC_CMD_PROGRAM:
            ep.setProgram(ipc_d1(pkt));
            break;
        case IPC_CMD_INSTRUMENT:
            ep.setInstrument(ipc_d1(pkt));
            cp_fx.setVoiceType(ep.getCurrentInstrument());
            break;
    }
}

// ===========================================================================
// Non-blocking random-note generator (Core 1 demo, replaces play_task)
// ===========================================================================
void play_random_notes_step(void) {
    static int phase = 0;
    static absolute_time_t next;
    static uint8_t x = 0;
    static uint8_t d = 0;
    static bool init = false;

    if (!init) {
        init = true;
        phase = 0;
        next = get_absolute_time();
    }

    if (!time_reached(next)) {
        return;
    }

    switch (phase) {
        case 0:
            x = rand() % 11;
            d = 64 + rand() % 63;
            ipc_send_note_on((uint8_t)(48 + x), (uint8_t)(90 + d));
            next = make_timeout_time_ms(100);
            phase = 1;
            break;
        case 1:
            ipc_send_note_on((uint8_t)(52 + x), (uint8_t)(90 + d));
            next = make_timeout_time_ms(100);
            phase = 2;
            break;
        case 2:
            ipc_send_note_on((uint8_t)(55 + x), (uint8_t)(90 + d));
            next = make_timeout_time_ms(100);
            phase = 3;
            break;
        case 3:
            ipc_send_note_on((uint8_t)(60 + x), (uint8_t)(90 + d));
            next = make_timeout_time_ms(2000);
            phase = 4;
            break;
        case 4:
            ipc_send_note_off((uint8_t)(48 + x));
            ipc_send_note_off((uint8_t)(52 + x));
            ipc_send_note_off((uint8_t)(55 + x));
            ipc_send_note_off((uint8_t)(60 + x));
            next = make_timeout_time_ms(2000);
            phase = 0;
            break;
        default:
            phase = 0;
            break;
    }
}

// ===========================================================================
// UI step (Core 1) -- one pass of the menu logic
// ===========================================================================
static void gui_step(void) {
    static bool cleared = false;
    if (!cleared) {
        u8g2_ClearDisplay(&u8g2);
        u8g2_SetDrawColor(&u8g2, 1);
        cleared = true;
    }
    int8_t res = pico_UserInterfaceProgramSelect(&u8g2, &enc, &bt, &ep);
    if (res > -1) {
        uint8_t menu = pico_UserInterfaceSelectionList(&u8g2, &enc, &bt, "MENU", 0, "Instrument\nVoice Params\nCP Effects\n<< BACK");
        if (menu == 1) {
            pico_UserInterfaceInstrumentSelect(&u8g2, &enc, &bt, &ep);
        } else if (menu == 2) {
            while (res > -1) {
                sleep_ms(500);
                res = pico_UserInterfaceParamSelect(&u8g2, &enc, &bt, &ep);
                if (res > -1) {
                    sleep_ms(500);
                    pico_UserInterfaceParamInput(&u8g2, &enc, &bt, &ep, res - 1);
                }
            }
        } else if (menu == 3) {
            pico_UserInterfaceCpEffects(&u8g2, &enc, &bt, &cp_fx);
        }
    }
    sleep_ms(500);
}

// ===========================================================================
// Core 1 periodic service -- also pumped from blocking UI wait-loops so USB
// (tud_task) keeps running while a menu is open.
// ===========================================================================
void ui_poll_usb(void) {
    tud_task();
    usbmidi.process();
#if PLAY_RANDOM_NOTES
    play_random_notes_step();
#endif
}

// Block (pumping USB) until the encoder button is released, so a single click
// is not consumed by several menu screens in a row (ReadButton is level-based).
void ui_wait_button_release(PushButton* bt) {
    absolute_time_t cap = make_timeout_time_ms(3000);
    while (bt->ReadButton() == PushButton::PRESSED && !time_reached(cap)) {
        ui_poll_usb();
        sleep_ms(1);
    }
}

// ===========================================================================
// Core 1 entry point: USB + DIN MIDI + encoder/button + OLED UI
// ===========================================================================
void core1_main(void) {
  board_init();
  tusb_init();
  bt.Init();
  enc.init();
  u8g2_Setup_sh1106_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8x8_byte_pico_hw_i2c, u8x8_gpio_and_delay_pico);
  u8g2_InitDisplay(&u8g2);
  u8g2_SetPowerSave(&u8g2, 0);
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, u8g2_font_8x13B_tf);
  u8g2_SetBitmapMode(&u8g2, false);
  u8g2_FirstPage(&u8g2);
  do {
      u8g2_DrawXBMP(&u8g2, 2, 15, logoWidth, logoHeight, logo);
      u8g2_DrawUTF8(&u8g2, 40, 25, PICO_PROGRAM_NAME);
      u8g2_DrawUTF8(&u8g2, 40, 40, PICO_PROGRAM_VERSION_STRING);
  } while (u8g2_NextPage(&u8g2));
  // keep USB enumeration alive during the splash screen
  absolute_time_t splash_end = make_timeout_time_ms(2000);
  while (!time_reached(splash_end)) {
      tud_task();
      sleep_ms(1);
  }
  usbmidi.setCCCallback(cc_callback);
  usbmidi.setNoteOnCallback(note_on_callback);
  usbmidi.setNoteOffCallback(note_off_callback);
  while (1) {
      ui_poll_usb();
      gui_step();
  }
}

// ===========================================================================
// Core 0 entry point: audio master
// ===========================================================================
int main(void) {
  pico_init();
  ep.setVolume(64);
  cp_fx.init((float)SAMPLING_RATE);
  cp_fx.setVoiceType(ep.getCurrentInstrument());
  cp_fx.setVolume(0.9f);
  cp_fx.setDrive(0.15f);
  cp_fx.setChoPhaMode(RefaceCpChain::CP_CHORUS);
  cp_fx.setChoPhaDepth(0.4f);
  cp_fx.setChoPhaSpeed(0.3f);
  cp_fx.setReverbDepth(0.25f);
  cp_fx.setTremWahMode(RefaceCpChain::TW_OFF);
  cp_fx.setDelayMode(RefaceCpChain::DLY_OFF);
  // Core 1 (USB/MIDI/UI) must launch BEFORE init_audio(): the SDK uses the SIO
  // FIFO for the core-launch handshake, and the audio DMA IRQ drains that same
  // FIFO (i2s_callback_func) -> it must not run during the launch handshake.
  multicore_launch_core1(core1_main);
  ap = init_audio();
  while (1) { __wfi(); }
  return 0;
}

// ===========================================================================
// I2S callback (Core 0, DMA-IRQ context): drain FIFO, then render one block
// ===========================================================================
void __not_in_flash_func(i2s_callback_func)() {
  while (multicore_fifo_rvalid()) { ipc_apply(multicore_fifo_pop_blocking()); }
  audio_buffer_t *buffer = take_audio_buffer(ap, false);
  if (buffer == NULL) { return; }
  int16_t l[buffer->max_sample_count];
  int16_t r[buffer->max_sample_count];
  int32_t *samples = (int32_t *)buffer->buffer->bytes;
  ep.process(&l[0], &r[0]);
  cp_process_block_i16(cp_fx, &l[0], &r[0], buffer->max_sample_count);
  for (uint i = 0; i < buffer->max_sample_count; i++) {
      samples[i * 2 + 0] = l[i] << 16;
      samples[i * 2 + 1] = r[i] << 16;
  }
  buffer->sample_count = buffer->max_sample_count;
  give_audio_buffer(ap, buffer);
  return;
}

#ifdef __cplusplus
}
#endif
