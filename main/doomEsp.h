#ifndef _DOOM_ESP_H_
#define _DOOM_ESP_H_

#include "bsp_p4_eval.h"
#include <stdint.h>

void doomEsp_Start(bsp_p4_handles_t bsp_handles, uint16_t *frame_buffer);

// Enqueue a DOOM key event (pressed=1 down, 0 up) from any input source.
void doomEsp_QueueKey(unsigned char key, int pressed);

#endif
