#ifndef __PROJECT_CONFIG_H__
#define __PROJECT_CONFIG_H__

#define PIN_MIDI_RX 5

// Pimoroni Pico Audio
//#define PIN_I2S_DOUT  9
//#define PIN_I2S_BCK   10
//#define PIN_I2S_WS    11

// Waveshare Pico Audio
#define PIN_I2S_DOUT  26
#define PIN_I2S_BCK   27
#define PIN_I2S_WS    28

#define  PIN_LED	  25

#define PIN_OLED_SDA  2
#define PIN_OLED_SCL  3

//#define PIN_POT_0     28
#define PIN_POT_1     29

// Selector encoder
#define PIN_SEL_CLK   6
#define PIN_SEL_DT    7
#define PIN_SEL_SW    8

// Param A encoder
#define PIN_PA_CLK    10
#define PIN_PA_DT     11
#define PIN_PA_SW     14   // optional switch

// Param B encoder
#define PIN_PB_CLK    12
#define PIN_PB_DT     13
#define PIN_PB_SW     15   // optional switch

// QMI M0_TIMING value for the 444 MHz overclock (set in pico_init, and
// re-applied after every flash_range_erase/program because the SDK's boot2
// re-init clobbers it with a timing that is unstable at 444 MHz).
#define PICOFACE_QMI_M0_TIMING_OC 0x60007303u

#endif // __PROJECT_CONFIG_H__
