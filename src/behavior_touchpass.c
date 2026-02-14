#define DT_DRV_COMPAT zmk_behavior_touchpass
#include "touchpass.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

LOG_MODULE_DECLARE(touchpass_driver, CONFIG_ZMK_LOG_LEVEL);

static uint8_t ascii_to_hid_usage(char c) {
  if (c >= 'a' && c <= 'z')
    return 0x04 + (c - 'a');
  if (c >= 'A' && c <= 'Z')
    return 0x04 + (c - 'A');
  if (c >= '1' && c <= '9')
    return 0x1E + (c - '1');
  if (c == '0')
    return 0x27;
  if (c == ' ')
    return 0x2C;
  return 0;
}

/* Behavior binding implementation */
static int
behavior_tp_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
  LOG_INF("TouchPass behavior triggered");

  finger_data_t data;
  if (touchpass_authenticate(&data) == 0) {
    LOG_INF("Authentication successful for %s", data.name);

    // Type the password
    for (int i = 0; data.password[i] != '\0'; i++) {
      uint8_t usage = ascii_to_hid_usage(data.password[i]);
      if (usage) {
        zmk_hid_keyboard_press(usage);
        zmk_endpoints_send_report(HID_USAGE_KEY);
        zmk_hid_keyboard_release(usage);
        zmk_endpoints_send_report(HID_USAGE_KEY);
      }
      k_sleep(K_MSEC(10));
    }

    if (data.press_enter) {
      zmk_hid_keyboard_press(HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
      zmk_endpoints_send_report(HID_USAGE_KEY);
      zmk_hid_keyboard_release(HID_USAGE_KEY_KEYBOARD_RETURN_ENTER);
      zmk_endpoints_send_report(HID_USAGE_KEY);
    }
  } else {
    LOG_WRN("Authentication failed or timed out");
  }

  return 0;
}

static int
behavior_tp_keymap_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
  return 0;
}

static const struct behavior_driver_api behavior_tp_driver_api = {
    .binding_pressed = behavior_tp_keymap_binding_pressed,
    .binding_released = behavior_tp_keymap_binding_released,
};

static int behavior_tp_init(const struct device *dev) { return 0; }

DEVICE_DT_INST_DEFINE(0, behavior_tp_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &behavior_tp_driver_api);
