#ifndef __AUDIO_SUBSYSTEM_H__
#define __AUDIO_SUBSYSTEM_H__


#define SAMPLES_PER_BUFFER 32 // Samples / channel (16->32: halved IRQ rate to ~1378/s)

#define USE_AUDIO_I2S 1
#include "audio_i2s.h"
#include <stdio.h>

audio_buffer_pool_t *init_audio();

#endif // __AUDIO_SUBSYSTEM_H__