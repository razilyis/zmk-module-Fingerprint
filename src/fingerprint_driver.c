#include "touchpass.h"
#include <string.h>
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
static bool sensor_ready;

/* ===== Enrollment State ===== */

static enum enroll_state enroll_state = ENROLL_IDLE;
static char enroll_name[32];
static char enroll_password[64];
static bool enroll_press_enter;
static int enroll_finger_id;
static int16_t enroll_slot = -1;
static bool enroll_success;
static const char *enroll_error = "";
static uint32_t enroll_timeout;

static const char *enroll_messages[] = {
    "Place finger on sensor", "Lift and place again", "Again, adjust slightly",
    "Now adjust your grip",   "Place again",          "One more time"};

/* ===== Low-level R502-A Communication ===== */

static int send_packet(uint8_t type, const uint8_t *data, uint16_t len) {
  if (!uart_dev)
    return -ENODEV;

  uint16_t packet_len = len + 2;
  uint16_t sum = type + (packet_len >> 8) + (packet_len & 0xFF);

  uart_poll_out(uart_dev, 0xEF);
  uart_poll_out(uart_dev, 0x01);
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
  uart_poll_out(uart_dev, 0xFF);
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

/* ===== Sensor Commands ===== */

static int capture_image(void) {
  uint8_t cmd[] = {CMD_GENIMG};
  send_packet(FP_CMD_PACKET, cmd, 1);
  if (receive_packet(1000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

static int generate_char(uint8_t slot) {
  uint8_t cmd[] = {CMD_IMG2TZ, slot};
  send_packet(FP_CMD_PACKET, cmd, 2);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

static int search_finger(uint16_t *match_id) {
  uint8_t cmd[] = {CMD_SEARCH, 0x01, 0x00, 0x00, 0x00, 0xC8};
  send_packet(FP_CMD_PACKET, cmd, 6);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  if (rx_buf[9] == 0x00) {
    *match_id = (rx_buf[10] << 8) | rx_buf[11];
    return 0;
  }
  return rx_buf[9];
}

static int create_template(void) {
  uint8_t cmd[] = {CMD_REGMODEL};
  send_packet(FP_CMD_PACKET, cmd, 1);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

static int store_template(uint8_t buf_id, uint16_t slot) {
  uint8_t cmd[] = {CMD_STORE, buf_id, (uint8_t)(slot >> 8),
                   (uint8_t)(slot & 0xFF)};
  send_packet(FP_CMD_PACKET, cmd, 4);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

/* ===== Public Sensor API ===== */

bool touchpass_is_sensor_ready(void) { return sensor_ready; }

int touchpass_get_template_count(uint16_t *count) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_TEMPLATENUM};
  send_packet(FP_CMD_PACKET, cmd, 1);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  if (rx_buf[9] != 0x00)
    return -EIO;
  *count = (rx_buf[10] << 8) | rx_buf[11];
  return 0;
}

int touchpass_get_library_size(uint16_t *size) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_READSYSPARA};
  send_packet(FP_CMD_PACKET, cmd, 1);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  if (rx_buf[9] != 0x00)
    return -EIO;
  *size = (rx_buf[14] << 8) | rx_buf[15];
  return 0;
}

int touchpass_read_index_table(uint8_t page, uint8_t *table) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_READINDEXTABLE, page};
  send_packet(FP_CMD_PACKET, cmd, 2);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  if (rx_buf[9] != 0x00)
    return -EIO;
  memcpy(table, &rx_buf[10], 32);
  return 0;
}

int touchpass_delete_template(uint16_t id, uint16_t count) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_DELETCHAR, (uint8_t)(id >> 8), (uint8_t)(id & 0xFF),
                   (uint8_t)(count >> 8), (uint8_t)(count & 0xFF)};
  send_packet(FP_CMD_PACKET, cmd, 5);
  if (receive_packet(2000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

int touchpass_set_led(uint8_t ctrl, uint8_t speed, uint8_t color,
                      uint8_t times) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_AURALEDCONFIG, ctrl, speed, color, times};
  send_packet(FP_CMD_PACKET, cmd, 5);
  if (receive_packet(1000) < 0)
    return -ETIMEDOUT;
  return rx_buf[9];
}

static int find_empty_slot(uint16_t lib_size) {
  uint16_t total_pages = (lib_size + 255) / 256;
  uint8_t table[32];

  for (uint8_t page = 0; page < total_pages; page++) {
    if (touchpass_read_index_table(page, table) != 0)
      continue;
    for (int i = 0; i < 32; i++) {
      for (int bit = 0; bit < 8; bit++) {
        if (!(table[i] & (1 << bit))) {
          uint16_t slot = page * 256 + i * 8 + bit;
          if (slot < lib_size)
            return slot;
        }
      }
    }
  }
  return -1;
}

/* ===== Authentication ===== */

int touchpass_authenticate(finger_data_t *data) {
  if (capture_image() != 0x00)
    return -EAGAIN;

  if (generate_char(1) != 0x00)
    return -EIO;

  uint16_t match_id;
  if (search_finger(&match_id) == 0) {
#ifdef CONFIG_NVS
    return touchpass_get_finger(match_id, data);
#else
    data->finger_id = match_id;
    snprintf(data->name, sizeof(data->name), "finger_%d", match_id);
    data->password[0] = '\0';
    data->press_enter = false;
    return 0;
#endif
  }
  return -EACCES;
}

/* ===== Enrollment ===== */

static bool enroll_capture_to_buffer(uint8_t buf_num) {
  if (capture_image() != 0x00)
    return false;

  int rc = generate_char(buf_num);
  if (rc != 0x00) {
    enroll_error = "Feature extraction failed";
    enroll_success = false;
    enroll_state = ENROLL_DONE;
    touchpass_set_led(LED_ON, 0, FP_LED_RED, 0);
    return false;
  }
  return true;
}

static bool is_finger_lifted(void) {
  return capture_image() != 0x00;
}

int touchpass_enroll_start(const char *name, const char *password,
                           bool press_enter, int finger_id) {
  if (!uart_dev)
    return -ENODEV;

  uint16_t lib_size = 200;
  touchpass_get_library_size(&lib_size);

#ifdef CONFIG_NVS
  for (uint16_t i = 0; i < lib_size; i++) {
    finger_data_t existing;
    if (touchpass_get_finger(i, &existing) == 0 &&
        existing.finger_id == finger_id) {
      touchpass_delete_template(i, 1);
      touchpass_delete_finger(i);
      break;
    }
  }
#endif

  int slot = find_empty_slot(lib_size);
  if (slot < 0)
    return -ENOMEM;

  strncpy(enroll_name, name, sizeof(enroll_name) - 1);
  enroll_name[sizeof(enroll_name) - 1] = '\0';
  strncpy(enroll_password, password, sizeof(enroll_password) - 1);
  enroll_password[sizeof(enroll_password) - 1] = '\0';
  enroll_press_enter = press_enter;
  enroll_finger_id = finger_id;
  enroll_slot = slot;
  enroll_success = false;
  enroll_error = "";
  enroll_timeout = k_uptime_get_32() + 60000;

  enroll_state = ENROLL_CAPTURE_1;
  touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);

  LOG_INF("Enrollment started: name=%s slot=%d finger=%d", enroll_name,
          enroll_slot, enroll_finger_id);
  return 0;
}

int touchpass_enroll_step(void) {
  if (enroll_state == ENROLL_IDLE || enroll_state == ENROLL_DONE)
    return 0;

  if (k_uptime_get_32() > enroll_timeout) {
    enroll_error = "Timeout";
    enroll_success = false;
    enroll_state = ENROLL_DONE;
    touchpass_set_led(LED_ON, 0, FP_LED_RED, 0);
    return -ETIMEDOUT;
  }

  switch (enroll_state) {
  case ENROLL_CAPTURE_1:
    if (enroll_capture_to_buffer(1)) {
      enroll_state = ENROLL_LIFT_1;
      touchpass_set_led(LED_FLASHING, 100, FP_LED_GREEN, 2);
    }
    break;
  case ENROLL_LIFT_1:
    if (is_finger_lifted()) {
      enroll_state = ENROLL_CAPTURE_2;
      touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_CAPTURE_2:
    if (enroll_capture_to_buffer(2)) {
      enroll_state = ENROLL_LIFT_2;
      touchpass_set_led(LED_FLASHING, 100, FP_LED_GREEN, 2);
    }
    break;
  case ENROLL_LIFT_2:
    if (is_finger_lifted()) {
      enroll_state = ENROLL_CAPTURE_3;
      touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_CAPTURE_3:
    if (enroll_capture_to_buffer(1)) {
      enroll_state = ENROLL_LIFT_3;
      touchpass_set_led(LED_FLASHING, 100, FP_LED_GREEN, 2);
    }
    break;
  case ENROLL_LIFT_3:
    if (is_finger_lifted()) {
      enroll_state = ENROLL_CAPTURE_4;
      touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_CAPTURE_4:
    if (enroll_capture_to_buffer(2)) {
      enroll_state = ENROLL_LIFT_4;
      touchpass_set_led(LED_FLASHING, 100, FP_LED_GREEN, 2);
    }
    break;
  case ENROLL_LIFT_4:
    if (is_finger_lifted()) {
      enroll_state = ENROLL_CAPTURE_5;
      touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_CAPTURE_5:
    if (enroll_capture_to_buffer(1)) {
      enroll_state = ENROLL_LIFT_5;
      touchpass_set_led(LED_FLASHING, 100, FP_LED_GREEN, 2);
    }
    break;
  case ENROLL_LIFT_5:
    if (is_finger_lifted()) {
      enroll_state = ENROLL_CAPTURE_6;
      touchpass_set_led(LED_BREATHING, 100, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_CAPTURE_6:
    if (enroll_capture_to_buffer(2)) {
      enroll_state = ENROLL_MERGING;
      touchpass_set_led(LED_BREATHING, 50, FP_LED_BLUE, 0);
    }
    break;
  case ENROLL_MERGING: {
    int rc = create_template();
    if (rc != 0x00) {
      enroll_error = "Template merge failed";
      enroll_success = false;
      enroll_state = ENROLL_DONE;
      touchpass_set_led(LED_ON, 0, FP_LED_RED, 0);
      return -EIO;
    }

    rc = store_template(1, enroll_slot);
    if (rc != 0x00) {
      enroll_error = "Store failed";
      enroll_success = false;
      enroll_state = ENROLL_DONE;
      touchpass_set_led(LED_ON, 0, FP_LED_RED, 0);
      return -EIO;
    }

#ifdef CONFIG_NVS
    finger_data_t data;
    strncpy(data.name, enroll_name, sizeof(data.name));
    strncpy(data.password, enroll_password, sizeof(data.password));
    data.press_enter = enroll_press_enter;
    data.finger_id = enroll_finger_id;
    touchpass_save_finger(enroll_slot, &data);
#endif

    enroll_success = true;
    enroll_state = ENROLL_DONE;
    touchpass_set_led(LED_ON, 0, FP_LED_GREEN, 0);
    LOG_INF("Enrollment complete: %s -> slot %d", enroll_name, enroll_slot);
  } break;
  default:
    break;
  }

  return 0;
}

void touchpass_enroll_cancel(void) {
  enroll_state = ENROLL_IDLE;
  enroll_slot = -1;
  touchpass_set_led(LED_OFF, 0, FP_LED_BLUE, 0);
  LOG_INF("Enrollment cancelled");
}

enum enroll_state touchpass_enroll_get_state(void) { return enroll_state; }

int touchpass_enroll_get_step(void) {
  switch (enroll_state) {
  case ENROLL_CAPTURE_1:
  case ENROLL_LIFT_1:
    return 1;
  case ENROLL_CAPTURE_2:
  case ENROLL_LIFT_2:
    return 2;
  case ENROLL_CAPTURE_3:
  case ENROLL_LIFT_3:
    return 3;
  case ENROLL_CAPTURE_4:
  case ENROLL_LIFT_4:
    return 4;
  case ENROLL_CAPTURE_5:
  case ENROLL_LIFT_5:
    return 5;
  case ENROLL_CAPTURE_6:
  case ENROLL_MERGING:
    return 6;
  default:
    return 0;
  }
}

bool touchpass_enroll_is_done(void) { return enroll_state == ENROLL_DONE; }

bool touchpass_enroll_was_successful(void) { return enroll_success; }

const char *touchpass_enroll_get_message(void) {
  int step = touchpass_enroll_get_step();
  if (step >= 1 && step <= 6)
    return enroll_messages[step - 1];
  if (enroll_state == ENROLL_MERGING)
    return "Creating template...";
  if (enroll_state == ENROLL_DONE)
    return enroll_success ? "Enrollment successful!" : enroll_error;
  return "";
}

const char *touchpass_enroll_get_name(void) { return enroll_name; }

/* ===== Polling Mode ===== */

#ifdef CONFIG_ZMK_TOUCHPASS_ALWAYS_ON
static void polling_thread(void *p1, void *p2, void *p3) {
  LOG_INF("TouchPass continuous polling thread started");
  while (1) {
    finger_data_t data;
    if (touchpass_authenticate(&data) == 0) {
      LOG_INF("Polling: Detected %s", data.name);
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
      k_sleep(K_MSEC(2000));
    }
    k_sleep(K_MSEC(200));
  }
}

K_THREAD_DEFINE(tp_polling_tid, 2048, polling_thread, NULL, NULL, NULL, 7, 0,
                0);
#endif

/* ===== Sensor Check (can be called any time) ===== */

int touchpass_check_sensor(void) {
  if (!uart_dev)
    return -ENODEV;
  uint8_t cmd[] = {CMD_HANDSHAKE};
  send_packet(FP_CMD_PACKET, cmd, 1);
  sensor_ready = (receive_packet(1000) >= 0 && rx_buf[9] == 0x00);
  if (sensor_ready) {
    LOG_INF("R502-A sensor connected");
  } else {
    LOG_WRN("R502-A sensor not responding");
  }
  return sensor_ready ? 0 : -EIO;
}

/* ===== Sensor Init Thread =====
 * Runs independently of RPC/USB — ensures sensor works in BLE-only mode.
 * Retries handshake every 5s until sensor responds. */

static void sensor_init_thread(void *p1, void *p2, void *p3) {
  /* R502-A needs ~1.5s to boot after power-on */
  k_sleep(K_MSEC(2000));

  for (int attempt = 0; attempt < 12; attempt++) {
    if (touchpass_check_sensor() == 0) {
      return;
    }
    k_sleep(K_MSEC(5000));
  }
  LOG_WRN("Sensor init thread: gave up after 60s");
}

K_THREAD_DEFINE(tp_sensor_init_tid, 1024, sensor_init_thread, NULL, NULL, NULL,
                8, 0, 0);

/* ===== Init ===== */

int touchpass_init(void) {
  uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
  if (!device_is_ready(uart_dev)) {
    LOG_ERR("UART device not ready");
    uart_dev = NULL;
    sensor_ready = false;
  }

#ifdef CONFIG_NVS
  int rc = touchpass_storage_init();
  if (rc != 0) {
    LOG_WRN("TouchPass storage init failed: %d (non-fatal)", rc);
  }
#endif

  LOG_INF("TouchPass driver initialized");
  return 0;
}

static int touchpass_pre_init(void) { return touchpass_init(); }

SYS_INIT(touchpass_pre_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
