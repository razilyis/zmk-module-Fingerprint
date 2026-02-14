#include "touchpass.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(touchpass_rpc, CONFIG_ZMK_LOG_LEVEL);

#ifdef CONFIG_ZMK_TOUCHPASS_SERIAL_RPC

static void rpc_thread(void *p1, void *p2, void *p3) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

  if (!device_is_ready(dev)) {
    LOG_ERR("CDC ACM device not ready for RPC");
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

K_THREAD_DEFINE(tp_rpc_tid, 4096, rpc_thread, NULL, NULL, NULL, 5, 0, 2000);

#endif
