#include "doomEsp.h"
#include "doomEsp_touch.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "DOOM_ESP";

char doomEsp_savedir[32] = "/spiffs";

#define DOOM_W 320
#define DOOM_H 200

// Render DOOM's 320x200 into a top-aligned 4:3 region (720x540, scale 2.25/2.7,
// which reproduces DOOM's intended pixel aspect). The freed bottom 180px band
// (y 540..720) holds the on-screen touch controls (see doomEsp_touch.c).
#define DOOM_DEST_W 720
#define DOOM_DEST_H 540
#define DOOM_DEST_Y 0

// Buffers
static uint16_t *doom_rb565;
static uint16_t *global_frame_buffer;
static ppa_client_handle_t ppa_client;
static bsp_p4_handles_t g_bsp_handles;

// --- Optional on-screen FPS counter (set to 0 to disable) ---
#define DOOM_SHOW_FPS 1

#if DOOM_SHOW_FPS
// Minimal 5x7 font, digits 0-9.
static const uint8_t fps_font[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};

// Draw the FPS value top-left onto the RGB565 framebuffer (after the PPA blit).
static void draw_fps_overlay(uint16_t *fb, int fps) {
  const int scale = 3, ox = 10, oy = 10;
  char buf[6];
  int n = snprintf(buf, sizeof(buf), "%d", fps > 9999 ? 9999 : fps);
  int cell = (5 + 1) * scale; // 5px glyph + 1px gap
  int bw = n * cell + 6, bh = 7 * scale + 6;
  for (int y = oy - 3; y < oy - 3 + bh; y++) // dark backing box for legibility
    for (int x = ox - 3; x < ox - 3 + bw; x++)
      if (x >= 0 && x < LCD_H_RES && y >= 0 && y < LCD_V_RES)
        fb[y * LCD_H_RES + x] = 0x0000;
  for (int i = 0; i < n; i++) { // yellow digits
    const uint8_t *g = fps_font[buf[i] - '0'];
    for (int ry = 0; ry < 7; ry++)
      for (int rx = 0; rx < 5; rx++)
        if (g[ry] & (1 << (4 - rx)))
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++) {
              int x = ox + i * cell + rx * scale + sx, y = oy + ry * scale + sy;
              if (x >= 0 && x < LCD_H_RES && y >= 0 && y < LCD_V_RES)
                fb[y * LCD_H_RES + x] = 0xFFE0;
            }
  }
}
#endif

// Draw hook for doomgeneric
void p4_doom_draw_frame(const uint32_t *buffer) {
  // Directly copy the native RGB565 buffer requested from doomgeneric
  memcpy(doom_rb565, buffer, DOOM_W * DOOM_H * 2);

  esp_cache_msync(doom_rb565, DOOM_W * DOOM_H * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  float scale_x = (float)DOOM_DEST_W / (float)DOOM_W; // 2.25
  float scale_y = (float)DOOM_DEST_H / (float)DOOM_H; // 2.7

  ppa_srm_oper_config_t srm_config = {
      .in = {.buffer = doom_rb565,
             .pic_w = DOOM_W,
             .pic_h = DOOM_H,
             .block_w = DOOM_W,
             .block_h = DOOM_H,
             .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .out = {.buffer = global_frame_buffer,
              .buffer_size = LCD_H_RES * LCD_V_RES * 2,
              .pic_w = LCD_H_RES,
              .pic_h = LCD_V_RES,
              .block_offset_x = 0,
              .block_offset_y = DOOM_DEST_Y,
              .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
      .scale_x = scale_x,
      .scale_y = scale_y,
  };

  ppa_do_scale_rotate_mirror(ppa_client, &srm_config);

#if DOOM_SHOW_FPS
  static uint32_t fps_frames = 0;
  static uint64_t fps_last_us = 0;
  static int fps_value = 0;
  fps_frames++;
  uint64_t now_us = esp_timer_get_time();
  if (fps_last_us == 0)
    fps_last_us = now_us;
  if (now_us - fps_last_us >= 500000) { // refresh twice a second
    fps_value = (int)(fps_frames * 1000000ULL / (now_us - fps_last_us));
    fps_frames = 0;
    fps_last_us = now_us;
  }
  draw_fps_overlay(global_frame_buffer, fps_value);
#endif

  esp_cache_msync(global_frame_buffer, LCD_H_RES * LCD_V_RES * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

// Queue for keyboard events
typedef struct {
  int pressed;       // 1 if DOWN, 0 if UP
  unsigned char key; // DOOM key code
} doom_key_event_t;

static QueueHandle_t doom_key_queue;

static const uint8_t hid_to_doom[256] = {
    [0x04] = 'a',           [0x05] = 'b',           [0x06] = 'c',
    [0x07] = 'd',           [0x08] = 'e',           [0x09] = 'f',
    [0x0A] = 'g',           [0x0B] = 'h',           [0x0C] = 'i',
    [0x0D] = 'j',           [0x0E] = 'k',           [0x0F] = 'l',
    [0x10] = 'm',           [0x11] = 'n',           [0x12] = 'o',
    [0x13] = 'p',           [0x14] = 'q',           [0x15] = 'r',
    [0x16] = 's',           [0x17] = 't',           [0x18] = 'u',
    [0x19] = 'v',           [0x1A] = 'w',           [0x1B] = 'x',
    [0x1C] = 'y',           [0x1D] = 'z',           [0x1E] = '1',
    [0x1F] = '2',           [0x20] = '3',           [0x21] = '4',
    [0x22] = '5',           [0x23] = '6',           [0x24] = '7',
    [0x25] = '8',           [0x26] = '9',           [0x27] = '0',
    [0x28] = KEY_ENTER,     [0x29] = KEY_ESCAPE,    [0x2A] = KEY_BACKSPACE,
    [0x2B] = KEY_TAB,       [0x2C] = ' ',           [0x2D] = '-',
    [0x2E] = '=',           [0x2F] = '[',           [0x30] = ']',
    [0x31] = '\\',          [0x33] = ';',           [0x34] = '\'',
    [0x35] = '`',           [0x36] = ',',           [0x37] = '.',
    [0x38] = '/',           [0x3A] = KEY_F1,        [0x3B] = KEY_F2,
    [0x3C] = KEY_F3,        [0x3D] = KEY_F4,        [0x3E] = KEY_F5,
    [0x3F] = KEY_F6,        [0x40] = KEY_F7,        [0x41] = KEY_F8,
    [0x42] = KEY_F9,        [0x43] = KEY_F10,       [0x44] = KEY_F11,
    [0x45] = KEY_F12,       [0x48] = KEY_PAUSE,     [0x49] = KEY_INS,
    [0x4A] = KEY_HOME,      [0x4B] = KEY_PGUP,      [0x4C] = KEY_DEL,
    [0x4D] = KEY_END,       [0x4E] = KEY_PGDN,      [0x4F] = KEY_RIGHTARROW,
    [0x50] = KEY_LEFTARROW, [0x51] = KEY_DOWNARROW, [0x52] = KEY_UPARROW,
};

static void queue_doom_key(unsigned char key, int pressed) {
  if (!key)
    return;
  doom_key_event_t ev = {.pressed = pressed, .key = key};
  xQueueSend(doom_key_queue, &ev, 0);
}

// Public entry so other input sources (on-screen touch controls) can feed the
// same key queue the USB keyboard path uses.
void doomEsp_QueueKey(unsigned char key, int pressed) {
  queue_doom_key(key, pressed);
}

// USB HID Host Callback
static void hid_host_keyboard_report_callback(const uint8_t *const data,
                                              const int length) {
  hid_keyboard_input_report_boot_t *kb =
      (hid_keyboard_input_report_boot_t *)data;
  if (length < sizeof(hid_keyboard_input_report_boot_t))
    return;

  static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = {0};

  // Check released
  for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
    if (prev_keys[i]) {
      bool found = false;
      for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++)
        if (kb->key[j] == prev_keys[i])
          found = true;
      if (!found) {
        queue_doom_key(hid_to_doom[prev_keys[i]], 0);
        if (prev_keys[i] == 0x2C || prev_keys[i] == 0x08)
          queue_doom_key(KEY_USE, 0);
      }
    }
  }
  // Check pressed
  for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
    if (kb->key[i]) {
      bool found = false;
      for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++)
        if (prev_keys[j] == kb->key[i])
          found = true;
      if (!found) {
        queue_doom_key(hid_to_doom[kb->key[i]], 1);
        if (kb->key[i] == 0x2C || kb->key[i] == 0x08)
          queue_doom_key(KEY_USE, 1);
      }
    }
  }

  // Check modifiers
  static uint8_t old_mods = 0;
  uint8_t mods = kb->modifier.val;
  if ((mods & 0x11) && !(old_mods & 0x11)) {
    queue_doom_key(KEY_RCTRL, 1);
    queue_doom_key(KEY_FIRE, 1);
  }
  if (!(mods & 0x11) && (old_mods & 0x11)) {
    queue_doom_key(KEY_RCTRL, 0);
    queue_doom_key(KEY_FIRE, 0);
  }

  if ((mods & 0x22) && !(old_mods & 0x22))
    queue_doom_key(KEY_RSHIFT, 1);
  if (!(mods & 0x22) && (old_mods & 0x22))
    queue_doom_key(KEY_RSHIFT, 0);

  if ((mods & 0x44) && !(old_mods & 0x44))
    queue_doom_key(KEY_RALT, 1);
  if (!(mods & 0x44) && (old_mods & 0x44))
    queue_doom_key(KEY_RALT, 0);

  memcpy(prev_keys, kb->key, HID_KEYBOARD_KEY_MAX);
  old_mods = mods;
}

static void
hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                            const hid_host_interface_event_t event, void *arg) {
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    size_t data_length;
    uint8_t data[64];
    ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
        hid_device_handle, data, 64, &data_length));
    hid_host_dev_params_t dev_params;
    hid_host_device_get_params(hid_device_handle, &dev_params);
    if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
      hid_host_keyboard_report_callback(data, data_length);
    }
  } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    hid_host_device_close(hid_device_handle);
  }
}

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event,
                                     void *arg) {
  if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
    hid_host_dev_params_t dev_params;
    hid_host_device_get_params(hid_device_handle, &dev_params);
    if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
      const hid_host_device_config_t dev_config = {
          .callback = hid_host_interface_callback, .callback_arg = NULL};
      ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
      if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
        hid_class_request_set_protocol(hid_device_handle,
                                       HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(hid_device_handle, 0, 0);
      }
      ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
      ESP_LOGI(TAG, "USB Keyboard Configured and Ready!");
    }
  }
}

static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive(arg);
  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
  }
  vTaskDelete(NULL);
}

// Doom Hook
int p4_doom_get_key(int *pressed, unsigned char *key) {
  doom_key_event_t ev;
  // Only physical USB keyboard
  if (xQueueReceive(doom_key_queue, &ev, 0) == pdTRUE) {
    *pressed = ev.pressed;
    *key = ev.key;
    return 1;
  }
  return 0; // Empty
}

#include "doomEsp_sound.h"

void doomEsp_Start(bsp_p4_handles_t bsp_handles, uint16_t *frame_buffer) {
  ESP_LOGI(TAG, "Starting DOOM on ESP32-P4 with USB Keyboard support...");

  global_frame_buffer = frame_buffer;
  g_bsp_handles = bsp_handles;

  // Black out the whole panel once; the scaler only writes the 720x540 center.
  memset(global_frame_buffer, 0, LCD_H_RES * LCD_V_RES * 2);
  esp_cache_msync(global_frame_buffer, LCD_H_RES * LCD_V_RES * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  // 0. Keyboard Queue
  doom_key_queue = xQueueCreate(32, sizeof(doom_key_event_t));

  // 1. Initialize SPIFFS Subsystem
  esp_vfs_spiffs_conf_t spiffs_conf = {.base_path = "/spiffs",
                                       .partition_label = "spiffs",
                                       .max_files = 5,
                                       .format_if_mount_failed = false};
  if (esp_vfs_spiffs_register(&spiffs_conf) == ESP_OK) {
    ESP_LOGI(TAG, "SPIFFS mounted OK.");
  }

  // 2. Framebuffer Queue and Hardware PPA
  doom_rb565 = (uint16_t *)heap_caps_aligned_alloc(64, DOOM_W * DOOM_H * 2,
                                                   MALLOC_CAP_INTERNAL);

  ppa_client_config_t ppa_config = {.oper_type = PPA_OPERATION_SRM};
  ESP_ERROR_CHECK(ppa_register_client(&ppa_config, &ppa_client));

  // 3. Initialize USB HOST Hardware (HID)
  ESP_LOGI(TAG, "Initializing USB Host Driver...");
  BaseType_t task_created =
      xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                              xTaskGetCurrentTaskHandle(), 2, NULL, 0);
  assert(task_created == pdTRUE);
  ulTaskNotifyTake(false, 1000); // Wait for task to initialize usb_host

  const hid_host_driver_config_t hid_host_driver_config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = hid_host_device_callback,
      .callback_arg = NULL};
  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

  // Initialize Sound Engine
  doomEsp_SoundInit();

  // On-screen touch controls (thumbstick + FIRE/USE/ESC/ENT) in the bottom band.
  doomEsp_TouchInit(g_bsp_handles.touch_handle, global_frame_buffer);

  // 3.5 Initialize SD Card (optional)
  char *iwad_path = "/spiffs/doom1.wad"; // default IWAD
  char *pwad_path = NULL;                // optional PWAD

  if (bsp_sdcard_mount() == ESP_OK) {
    ESP_LOGI(TAG,
             "SD Card mounted successfully at /sdcard. Searching for WADs...");
    chdir("/sdcard");
    strcpy(doomEsp_savedir, "/sdcard");
    FILE *f;

    // Check for base IWAD first
    if ((f = fopen("/sdcard/doom2.wad", "rb"))) {
      iwad_path = "/sdcard/doom2.wad";
      fclose(f);
    } else if ((f = fopen("/sdcard/doom.wad", "rb"))) {
      iwad_path = "/sdcard/doom.wad";
      fclose(f);
    } else if ((f = fopen("/sdcard/doom1.wad", "rb"))) {
      iwad_path = "/sdcard/doom1.wad";
      fclose(f);
    } else {
      ESP_LOGW(TAG, "No base IWADs found on SD. Using internal Flash.");
    }

    // Check for Chiquito PWAD mod
    if ((f = fopen("/sdcard/chiquito.wad", "rb"))) {
      pwad_path = "/sdcard/chiquito.wad";
      ESP_LOGI(TAG, "Found PWAD: chiquito.wad");
      fclose(f);
    }
  } else {
    ESP_LOGW(
        TAG,
        "No SD card detected (or mount failed). Fallback to internal SPIFFS.");
    chdir("/spiffs");
    strcpy(doomEsp_savedir, "/spiffs");
  }

  ESP_LOGI(TAG, "Starting DOOM. IWAD: %s, PWAD: %s", iwad_path,
           pwad_path ? pwad_path : "None");

  // 4. Boot DOOM
  if (pwad_path) {
    char *doom_argv[] = {"doom",    "-iwad",    iwad_path, "-file",
                         pwad_path, "-gfxmode", "rgb565"};
    doomgeneric_Create(7, doom_argv);
  } else {
    char *doom_argv[] = {"doom", "-iwad", iwad_path, "-gfxmode", "rgb565"};
    doomgeneric_Create(5, doom_argv);
  }

  while (1) {
    doomgeneric_Tick();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
