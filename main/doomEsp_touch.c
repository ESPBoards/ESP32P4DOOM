/**
 * @file doomEsp_touch.c
 * @brief On-screen touch controls for the DOOM port: left thumb = forward/back,
 *        right thumb = turn-left/turn-right + FIRE, with USE/ESC/ENT in the
 *        center. All icon buttons live in the bottom 180px band (the game view
 *        above is never touched).
 *
 * The GT911 (5-point multitouch) is polled ~60Hz; each touch point is hit-tested
 * against the buttons and press/release transitions are pushed into the same DOOM
 * key queue the USB keyboard used (so move + turn + fire work simultaneously).
 * PPA only writes the top 720x540, so the band is drawn once and only repainted
 * per-button for pressed feedback.
 */

#include "doomEsp_touch.h"
#include "doomEsp.h"
#include "doomkeys.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DOOM_TOUCH";

#define PANEL_W 720
#define PANEL_H 720
#define BAND_Y0 540

/* GT911 axis remap (flip if a tap lands on the wrong control). */
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0

#define C_BAND_BG 0x0000 // black

static uint16_t *s_fb;

/* ===================== Color helpers ===================== */

static inline uint16_t rgb565(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Brighten (delta>0) or darken (delta<0) a color by an equal amount per channel.
static uint16_t shade(uint16_t c, int delta) {
  int r = ((c >> 11) & 0x1F) << 3;
  int g = ((c >> 5) & 0x3F) << 2;
  int b = (c & 0x1F) << 3;
  return rgb565(r + delta, g + delta, b + delta);
}

/* ===================== Draw primitives ===================== */

static inline void put_px(int x, int y, uint16_t c) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  s_fb[y * PANEL_W + x] = c;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t c) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > PANEL_W) x1 = PANEL_W;
  if (y1 > PANEL_H) y1 = PANEL_H;
  for (int y = y0; y < y1; y++) {
    uint16_t *row = &s_fb[y * PANEL_W];
    for (int x = x0; x < x1; x++) row[x] = c;
  }
}

static void rect_outline(int x0, int y0, int x1, int y1, int t, uint16_t c) {
  fill_rect(x0, y0, x1, y0 + t, c);
  fill_rect(x0, y1 - t, x1, y1, c);
  fill_rect(x0, y0, x0 + t, y1, c);
  fill_rect(x1 - t, y0, x1, y1, c);
}

static void fill_circle(int cx, int cy, int r, uint16_t c) {
  for (int y = cy - r; y <= cy + r; y++) {
    if (y < 0 || y >= PANEL_H) continue;
    int dy = y - cy;
    for (int x = cx - r; x <= cx + r; x++) {
      int dx = x - cx;
      if (dx * dx + dy * dy <= r * r) put_px(x, y, c);
    }
  }
}

static void draw_ring(int cx, int cy, int r, int thick, uint16_t c) {
  int ro2 = r * r, ri2 = (r - thick) * (r - thick);
  for (int y = cy - r; y <= cy + r; y++) {
    int dy = y - cy;
    for (int x = cx - r; x <= cx + r; x++) {
      int dx = x - cx, d2 = dx * dx + dy * dy;
      if (d2 <= ro2 && d2 >= ri2) put_px(x, y, c);
    }
  }
}

static int imin3(int a, int b, int c) {
  int m = a < b ? a : b;
  return m < c ? m : c;
}
static int imax3(int a, int b, int c) {
  int m = a > b ? a : b;
  return m > c ? m : c;
}
static int edge(int ax, int ay, int bx, int by, int px, int py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                          uint16_t c) {
  int minx = imin3(x0, x1, x2), maxx = imax3(x0, x1, x2);
  int miny = imin3(y0, y1, y2), maxy = imax3(y0, y1, y2);
  for (int y = miny; y <= maxy; y++) {
    for (int x = minx; x <= maxx; x++) {
      int w0 = edge(x1, y1, x2, y2, x, y);
      int w1 = edge(x2, y2, x0, y0, x, y);
      int w2 = edge(x0, y0, x1, y1, x, y);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
        put_px(x, y, c);
    }
  }
}

// Thick line by stamping small discs along the segment.
static void thick_line(int x0, int y0, int x1, int y1, int t, uint16_t c) {
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps == 0) steps = 1;
  for (int i = 0; i <= steps; i++) {
    int x = x0 + dx * i / steps;
    int y = y0 + dy * i / steps;
    fill_circle(x, y, t, c);
  }
}

/* ===================== Icons ===================== */

typedef enum {
  IC_UP, IC_DOWN, IC_LEFT, IC_RIGHT, IC_FIRE, IC_USE, IC_ESC, IC_ENT
} icon_t;

static void draw_icon(icon_t ic, int cx, int cy, int s, uint16_t col) {
  switch (ic) {
  case IC_UP:
    fill_triangle(cx, cy - s, cx - s, cy + s, cx + s, cy + s, col);
    break;
  case IC_DOWN:
    fill_triangle(cx, cy + s, cx - s, cy - s, cx + s, cy - s, col);
    break;
  case IC_LEFT:
    fill_triangle(cx - s, cy, cx + s, cy - s, cx + s, cy + s, col);
    break;
  case IC_RIGHT:
    fill_triangle(cx + s, cy, cx - s, cy - s, cx - s, cy + s, col);
    break;
  case IC_FIRE: // crosshair: ring + cross + center dot
    draw_ring(cx, cy, s, 3, col);
    thick_line(cx - s - s / 3, cy, cx + s + s / 3, cy, 1, col);
    thick_line(cx, cy - s - s / 3, cx, cy + s + s / 3, 1, col);
    fill_circle(cx, cy, 3, col);
    break;
  case IC_USE: { // door: tall outlined panel + handle dot
    int w = s, h = (s * 7) / 5;
    rect_outline(cx - w, cy - h, cx + w, cy + h, 3, col);
    fill_circle(cx + w - 6, cy, 3, col);
    break;
  }
  case IC_ESC: // X
    thick_line(cx - s, cy - s, cx + s, cy + s, 3, col);
    thick_line(cx + s, cy - s, cx - s, cy + s, 3, col);
    break;
  case IC_ENT: // checkmark
    thick_line(cx - s, cy, cx - s / 3, cy + s, 3, col);
    thick_line(cx - s / 3, cy + s, cx + s, cy - s, 3, col);
    break;
  }
}

/* ===================== Buttons ===================== */

typedef enum { SH_CIRCLE, SH_RECT } shape_t;

typedef struct {
  shape_t shape;
  int cx, cy, hw, hh; // circle uses hw as radius (hh ignored)
  uint16_t color;
  icon_t icon;
  unsigned char key;
} button_t;

enum {
  B_FWD, B_BACK, B_LEFT, B_RIGHT, B_FIRE, B_USE, B_ESC, B_ENT, NUM_BTN
};

// Calm palette: every button shares one dark-slate body with a soft light-gray
// icon; only FIRE carries a red accent. Visual hierarchy comes from size, not
// from a rainbow of colors. The per-button `color` field is the ACCENT
// (icon + border), not the body.
#define C_FILL 0x2146    // dark slate body
#define C_FILL_HI 0x428C // body when pressed (lighter slate)
#define C_NEUTRAL 0xCE9B // soft light-gray accent (most buttons)
#define C_RED 0xE227     // red accent (FIRE only)

// Two thumb clusters: left = forward/back, right = turn-left/turn-right + FIRE;
// USE/ESC/ENT sit in the center. All within the bottom 180px band.
static const button_t BUTTONS[NUM_BTN] = {
  {SH_RECT, 90, 588, 64, 36, C_NEUTRAL, IC_UP, KEY_UPARROW},      // FWD (left thumb)
  {SH_RECT, 90, 682, 64, 32, C_NEUTRAL, IC_DOWN, KEY_DOWNARROW},  // BACK
  {SH_CIRCLE, 440, 632, 46, 0, C_NEUTRAL, IC_LEFT, KEY_LEFTARROW},  // TURN-L (right thumb)
  {SH_CIRCLE, 548, 632, 46, 0, C_NEUTRAL, IC_RIGHT, KEY_RIGHTARROW}, // TURN-R
  {SH_CIRCLE, 656, 624, 60, 0, C_RED, IC_FIRE, KEY_FIRE},         // FIRE (red accent)
  {SH_CIRCLE, 250, 590, 44, 0, C_NEUTRAL, IC_USE, KEY_USE},       // USE (center)
  {SH_CIRCLE, 220, 682, 30, 0, C_NEUTRAL, IC_ESC, KEY_ESCAPE},   // ESC
  {SH_CIRCLE, 300, 682, 30, 0, C_NEUTRAL, IC_ENT, KEY_ENTER},    // ENT
};

static int icon_size(const button_t *bt) {
  return bt->hw / 2; // half the radius / half-width reads well for all icons
}

static void draw_button(int b, int pressed) {
  const button_t *bt = &BUTTONS[b];
  uint16_t accent = bt->color; // icon + border (C_NEUTRAL, or C_RED for FIRE)
  uint16_t fill = pressed ? C_FILL_HI : C_FILL;
  // Dim border normally; the full accent "glows" the outline when pressed.
  uint16_t border = pressed ? accent : shade(accent, -90);
  if (bt->shape == SH_CIRCLE) {
    fill_circle(bt->cx, bt->cy, bt->hw, fill);
    draw_ring(bt->cx, bt->cy, bt->hw, 3, border);
  } else {
    fill_rect(bt->cx - bt->hw, bt->cy - bt->hh, bt->cx + bt->hw, bt->cy + bt->hh,
              fill);
    rect_outline(bt->cx - bt->hw, bt->cy - bt->hh, bt->cx + bt->hw,
                 bt->cy + bt->hh, 3, border);
  }
  draw_icon(bt->icon, bt->cx, bt->cy, icon_size(bt), accent);
}

static int hit(const button_t *bt, int px, int py) {
  int dx = px - bt->cx, dy = py - bt->cy;
  if (bt->shape == SH_CIRCLE) return dx * dx + dy * dy <= bt->hw * bt->hw;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  return adx <= bt->hw && ady <= bt->hh;
}

/* ===================== Touch task ===================== */

static void flush_band(void) {
  esp_cache_msync(&s_fb[BAND_Y0 * PANEL_W],
                  (size_t)(PANEL_H - BAND_Y0) * PANEL_W * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void remap_point(uint16_t *x, uint16_t *y) {
#if TOUCH_SWAP_XY
  uint16_t t = *x; *x = *y; *y = t;
#endif
#if TOUCH_MIRROR_X
  *x = (PANEL_W - 1) - *x;
#endif
#if TOUCH_MIRROR_Y
  *y = (PANEL_H - 1) - *y;
#endif
}

static void touch_task(void *arg) {
  esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)arg;
  int prev[NUM_BTN] = {0};

  while (1) {
    esp_lcd_touch_read_data(tp);
    uint16_t tx[5], ty[5];
    uint8_t cnt = 0;
    esp_lcd_touch_get_coordinates(tp, tx, ty, NULL, &cnt, 5);

    int cur[NUM_BTN] = {0};
    for (int i = 0; i < cnt; i++) {
      uint16_t px = tx[i], py = ty[i];
      remap_point(&px, &py);
      for (int b = 0; b < NUM_BTN; b++)
        if (hit(&BUTTONS[b], px, py)) cur[b] = 1;
    }

    int dirty = 0;
    for (int b = 0; b < NUM_BTN; b++) {
      if (cur[b] != prev[b]) {
        doomEsp_QueueKey(BUTTONS[b].key, cur[b]);
        draw_button(b, cur[b]);
        prev[b] = cur[b];
        dirty = 1;
      }
    }
    if (dirty) flush_band();
    vTaskDelay(pdMS_TO_TICKS(16)); // ~60 Hz
  }
}

/* ===================== Public init ===================== */

void doomEsp_TouchInit(esp_lcd_touch_handle_t touch, uint16_t *framebuffer) {
  s_fb = framebuffer;

  fill_rect(0, BAND_Y0, PANEL_W, PANEL_H, C_BAND_BG);
  for (int b = 0; b < NUM_BTN; b++) draw_button(b, 0);
  flush_band();

  xTaskCreatePinnedToCore(touch_task, "doom_touch", 4096, (void *)touch, 4, NULL,
                          1);
  ESP_LOGI(TAG, "Touch controls ready (move | turn + FIRE | USE/ESC/ENT).");
}
