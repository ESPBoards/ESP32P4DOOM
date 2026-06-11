#ifndef _DOOM_ESP_TOUCH_H_
#define _DOOM_ESP_TOUCH_H_

#include "esp_lcd_touch.h"
#include <stdint.h>

/**
 * @brief Initialize on-screen touch controls.
 *
 * Draws the control band (virtual thumbstick + FIRE/USE/ESC/ENT buttons) into the
 * bottom 180px of @p framebuffer and starts a task that polls the GT911 and feeds
 * DOOM key events via doomEsp_QueueKey(). Must be called after the framebuffer has
 * been cleared and the DOOM key queue created.
 *
 * @param touch       Initialized GT911 touch handle.
 * @param framebuffer The active 720x720 RGB565 DPI framebuffer.
 */
void doomEsp_TouchInit(esp_lcd_touch_handle_t touch, uint16_t *framebuffer);

#endif
