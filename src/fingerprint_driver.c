#include "touchpass.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

LOG_MODULE_REGISTER(touchpass_driver, CONFIG_ZMK_LOG_LEVEL);

static const struct device *uart_dev;
static uint8_t rx_buf[512];
static uint16_t rx_idx = 0;

/* Helper: Send command to sensor */
static int send_packet(uint8_t type, const uint8_t *data, uint16_t len) {
  if (!uart_dev)
    return -ENODEV;

  uint16_t packet_len = len + 2; // Data + Checksum
  uint16_t sum = type + (packet_len >> 8) + (packet_len & 0xFF);

  // Header
  uart_poll_out(uart_dev, 0xEF);
  uart_poll_out(uart_dev, 0x01);
  // Address
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
  // Type & Len
  uart_poll_out(uart_dev, type);
  uart_poll_out(uart_dev, packet_len >> 8);
  uart_poll_out(uart_dev, packet_len & 0xFF);

  for (uint16_t i = 0; i < len; i++) {
    uart_poll_out(uart_dev, data[i]);
    sum += data[i];
  }

  uart_poll_out(uart_dev, sum >> 8);
  uart_poll_out(uart_dev, sum & 0xFF);

  return 0;
}

/* Helper: Wait for response */
static int receive_packet(uint32_t timeout_ms) {
  rx_idx = 0;
  uint32_t start = k_uptime_get_32();

  while (k_uptime_get_32() - start < timeout_ms) {
    uint8_t b;
    if (uart_poll_in(uart_dev, &b) == 0) {
      rx_buf[rx_idx++] = b;
      if (rx_idx >= 9) {
        uint16_t len = (rx_buf[7] << 8) | rx_buf[8];
        if (rx_idx >= 9 + len)
          return rx_idx;
      }
      if (rx_idx >= sizeof(rx_buf))
        break;
    } else {
      k_sleep(K_MSEC(1));
    }
  }
  return -ETIMEDOUT;
}

/* Helper: Character generation */
static int generate_char(uint8_t slot) {
  uint8_t cmd[] = {CMD_IMG2TZ, slot};
  send_packet(FP_CMD_PACKET, cmd, 2);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

/* Helper: Search fingerprint */
static int search_finger(uint16_t *match_id) {
  uint8_t cmd[] = {CMD_SEARCH, 0x01, 0x00,
                   0x00,       0x00, 0xC8}; // Search in first 200 slots
  send_packet(FP_CMD_PACKET, cmd, 6);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  if (rx_buf[9] == 0x00) {
    *match_id = (rx_buf[10] << 8) | rx_buf[11];
    return 0;
  }
  return rx_buf[9];
}

/* Authentication logic used by both trigger and polling */
int touchpass_authenticate(finger_data_t *data) {
  uint8_t cmd[] = {CMD_GENIMG};
  send_packet(FP_CMD_PACKET, cmd, 1);
  if (receive_packet(1000) < 0 || rx_buf[9] != 0x00)
    return -EAGAIN;

  if (generate_char(1) != 0x00)
    return -EIO;

  uint16_t match_id;
  if (search_finger(&match_id) == 0) {
    return touchpass_get_finger(match_id, data);
  }
  return -EACCES;
}

#ifdef CONFIG_ZMK_TOUCHPASS_ALWAYS_ON
static void polling_thread(void *p1, void *p2, void *p3) {
  LOG_INF("TouchPass continuous polling thread started");
  while (1) {
    finger_data_t data;
    if (touchpass_authenticate(&data) == 0) {
      LOG_INF("Polling: Detected %s", data.name);
      // Success: HID output handled here or via notify mechanism
      // For now, let's keep it consistent with the behavior's HID logic
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
      k_sleep(K_MSEC(2000)); // Cool down after successful match
    }
    k_sleep(K_MSEC(200)); // Poll at 5Hz
  }
}

K_THREAD_DEFINE(tp_polling_tid, 2048, polling_thread, NULL, NULL, NULL, 7, 0,
                0);
#endif

int touchpass_init(void) {
  uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
  if (!device_is_ready(uart_dev)) {
    LOG_ERR("UART device not ready");
    return -ENODEV;
  }

  // Initial handshake removed from SYS_INIT to prevent boot hang.
  // Handshake will be performed lazily on first access.
  LOG_INF("TouchPass driver initialized (Handshake deferred)");
  return 0;
}

static int touchpass_pre_init(void) { return touchpass_init(); }

SYS_INIT(touchpass_pre_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
