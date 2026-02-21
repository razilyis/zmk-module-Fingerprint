#define DT_DRV_COMPAT zmk_behavior_touchpass
#include "touchpass.h"
#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

LOG_MODULE_DECLARE(touchpass_driver, CONFIG_ZMK_LOG_LEVEL);

/* Behavior binding implementation */
static int
behavior_tp_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
  LOG_INF("TouchPass behavior triggered");

  finger_data_t data;
  if (touchpass_authenticate(&data) == 0) {
    LOG_INF("Authentication successful for %s", data.name);
    touchpass_type_password(&data);
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
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = behavior_tp_keymap_binding_pressed,
    .binding_released = behavior_tp_keymap_binding_released,
};

static int behavior_tp_init(const struct device *dev) { return 0; }

DEVICE_DT_INST_DEFINE(0, behavior_tp_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &behavior_tp_driver_api);
