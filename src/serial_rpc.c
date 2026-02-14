#include "touchpass.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(touchpass_rpc, CONFIG_ZMK_LOG_LEVEL);

#ifdef CONFIG_ZMK_TOUCHPASS_SERIAL_RPC

static void rpc_thread(void *p1, void *p2, void *p3) {
  const struct device *dev = device_get_binding("CDC_ACM_0");
  if (!dev) {
    LOG_ERR("CDC_ACM_0 not found for RPC");
    return;
  }

  LOG_INF("TouchPass RPC thread started");

  while (1) {
    // Basic echo/command logic will go here
    // This will implement the JSON-RPC handling from the original
    // SerialCommandHandler
    k_sleep(K_MSEC(100));
  }
}

K_THREAD_DEFINE(tp_rpc_tid, 2048, rpc_thread, NULL, NULL, NULL, 5, 0, 0);

#endif
