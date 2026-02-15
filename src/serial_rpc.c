/*
 * TouchPass Serial RPC — JSON-RPC over USB CDC ACM
 *
 * Protocol (compatible with config.html / Web Serial API):
 *   → {"cmd":"xxx","params":{...},"id":N}\n
 *   ← {"ok":true,"status":"ok","data":{...},"id":N}\n
 *   ← {"ok":false,"status":"error","message":"...","id":N}\n
 */

#include "touchpass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>
#include <zmk/ble.h>

LOG_MODULE_REGISTER(touchpass_rpc, CONFIG_ZMK_LOG_LEVEL);

#ifdef CONFIG_ZMK_TOUCHPASS_SERIAL_RPC

/* ===== Configuration ===== */

#define RPC_BUF_SIZE 512
#define RPC_TX_BUF_SIZE 1024
#define FIRMWARE_VERSION "2.0.0-zmk"
#define PLATFORM_NAME "nRF52840 (ZMK)"

/* ===== State ===== */

static const struct device *rpc_dev;
static char rx_line[RPC_BUF_SIZE];
static uint16_t rx_pos;
static char tx_buf[RPC_TX_BUF_SIZE];

/* Finger detection latch (for get_detect) */
static bool finger_detected_pending;
static char last_status[64] = "Ready";

/* ===== UART I/O ===== */

static void rpc_write(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uart_poll_out(rpc_dev, data[i]);
  }
}

static void rpc_println(const char *str) {
  rpc_write(str, strlen(str));
  uart_poll_out(rpc_dev, '\n');
}

/* ===== Minimal JSON Helpers ===== */

/* Find a string value for "key":"value" in json. Returns pointer into json. */
static int json_find_string(const char *json, const char *key, char *out,
                            size_t out_size) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":\"", key);

  const char *p = strstr(json, search);
  if (!p)
    return -1;
  p += strlen(search);

  size_t i = 0;
  while (*p && *p != '"' && i < out_size - 1) {
    if (*p == '\\' && *(p + 1)) {
      p++; /* Skip escape */
    }
    out[i++] = *p++;
  }
  out[i] = '\0';
  return 0;
}

/* Find an integer value for "key":N in json */
static int json_find_int(const char *json, const char *key, int def) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":", key);

  const char *p = strstr(json, search);
  if (!p)
    return def;
  p += strlen(search);

  /* Skip whitespace */
  while (*p == ' ')
    p++;

  if (*p == '-' || (*p >= '0' && *p <= '9'))
    return atoi(p);

  return def;
}

/* Find a bool value for "key":true/false in json */
static bool json_find_bool(const char *json, const char *key, bool def) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":", key);

  const char *p = strstr(json, search);
  if (!p)
    return def;
  p += strlen(search);

  while (*p == ' ')
    p++;

  if (strncmp(p, "true", 4) == 0)
    return true;
  if (strncmp(p, "false", 5) == 0)
    return false;
  return def;
}

/* ===== Response Senders ===== */

static void rpc_send_ok(int id, const char *data_json) {
  int len = snprintf(tx_buf, sizeof(tx_buf),
                     "{\"ok\":true,\"status\":\"ok\",\"data\":%s", data_json);
  if (id >= 0) {
    len += snprintf(tx_buf + len, sizeof(tx_buf) - len, ",\"id\":%d", id);
  }
  snprintf(tx_buf + len, sizeof(tx_buf) - len, "}");
  rpc_println(tx_buf);
}

static void rpc_send_error(int id, const char *message) {
  int len =
      snprintf(tx_buf, sizeof(tx_buf),
               "{\"ok\":false,\"status\":\"error\",\"message\":\"%s\"", message);
  if (id >= 0) {
    len += snprintf(tx_buf + len, sizeof(tx_buf) - len, ",\"id\":%d", id);
  }
  snprintf(tx_buf + len, sizeof(tx_buf) - len, "}");
  rpc_println(tx_buf);
}

/* ===== Command Handlers ===== */

static void cmd_ping(const char *params, int id) {
  rpc_send_ok(id, "{\"ok\":true,\"pong\":true}");
}

static void cmd_get_status(const char *params, int id) {
  uint16_t count = 0, capacity = 200;
  bool enrolling =
      (touchpass_enroll_get_state() != ENROLL_IDLE &&
       touchpass_enroll_get_state() != ENROLL_DONE);

  /* Retry sensor handshake if not connected */
  if (!touchpass_is_sensor_ready()) {
    touchpass_check_sensor();
  }

  touchpass_get_template_count(&count);
  touchpass_get_library_size(&capacity);

  int len = snprintf(
      tx_buf, sizeof(tx_buf),
      "{\"ok\":true,\"sensor\":%s,\"count\":%d,\"capacity\":%d,"
      "\"last\":\"%s\",\"enrolling\":%s,\"ble_connected\":%s}",
      touchpass_is_sensor_ready() ? "true" : "false", count, capacity,
      last_status, enrolling ? "true" : "false",
      zmk_ble_active_profile_is_connected() ? "true" : "false");

  /* get_status returns data at top level (matches original) */
  if (id >= 0) {
    snprintf(tx_buf + len, sizeof(tx_buf) - len, "");
  }
  rpc_send_ok(id, tx_buf);
}

static void cmd_get_fingers(const char *params, int id) {
  /* We need to build a JSON array of fingers.
   * Use the index table from the sensor + NVS data. */
  char resp[RPC_TX_BUF_SIZE];
  int pos = 0;
  pos += snprintf(resp + pos, sizeof(resp) - pos, "{\"fingers\":[");

#ifdef CONFIG_NVS
  uint16_t lib_size = 200;
  touchpass_get_library_size(&lib_size);
  uint16_t total_pages = (lib_size + 255) / 256;
  uint8_t index_table[32];
  bool first = true;

  for (uint8_t page = 0; page < total_pages && pos < (int)sizeof(resp) - 128;
       page++) {
    if (touchpass_read_index_table(page, index_table) != 0)
      continue;
    for (int i = 0; i < 32 && pos < (int)sizeof(resp) - 128; i++) {
      uint8_t byte_val = index_table[i];
      for (int bit = 0; bit < 8; bit++) {
        if (byte_val & (1 << bit)) {
          uint16_t slot = page * 256 + i * 8 + bit;
          if (slot >= lib_size)
            break;
          finger_data_t data;
          if (touchpass_get_finger(slot, &data) == 0) {
            if (!first)
              resp[pos++] = ',';
            first = false;
            pos += snprintf(resp + pos, sizeof(resp) - pos,
                            "{\"id\":%d,\"name\":\"%s\",\"fingerId\":%d,"
                            "\"pressEnter\":%s}",
                            slot, data.name, data.finger_id,
                            data.press_enter ? "true" : "false");
          }
        }
      }
    }
  }
#endif

  snprintf(resp + pos, sizeof(resp) - pos, "]}");
  rpc_send_ok(id, resp);
}

static void cmd_get_finger(const char *params, int id) {
  int slot = json_find_int(params, "id", -1);
  if (slot < 0) {
    rpc_send_error(id, "Invalid ID");
    return;
  }

#ifdef CONFIG_NVS
  finger_data_t data;
  if (touchpass_get_finger(slot, &data) != 0) {
    rpc_send_error(id, "Finger not found");
    return;
  }

  snprintf(tx_buf, sizeof(tx_buf),
           "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"hasPassword\":%s,"
           "\"pressEnter\":%s,\"fingerId\":%d}",
           slot, data.name, (data.password[0] != '\0') ? "true" : "false",
           data.press_enter ? "true" : "false", data.finger_id);
  rpc_send_ok(id, tx_buf);
#else
  rpc_send_error(id, "Storage not available");
#endif
}

static void cmd_update_finger(const char *params, int id) {
#ifdef CONFIG_NVS
  int slot = json_find_int(params, "id", -1);
  if (slot < 0) {
    rpc_send_error(id, "Invalid ID");
    return;
  }

  finger_data_t data;
  if (touchpass_get_finger(slot, &data) != 0) {
    rpc_send_error(id, "Finger not found");
    return;
  }

  /* Update fields if present in params */
  char tmp[64];
  if (json_find_string(params, "name", tmp, sizeof(tmp)) == 0) {
    strncpy(data.name, tmp, sizeof(data.name) - 1);
    data.name[sizeof(data.name) - 1] = '\0';
  }
  if (json_find_string(params, "password", tmp, sizeof(tmp)) == 0) {
    strncpy(data.password, tmp, sizeof(data.password) - 1);
    data.password[sizeof(data.password) - 1] = '\0';
  }
  /* Check for pressEnter */
  const char *pe = strstr(params, "\"pressEnter\":");
  if (pe) {
    data.press_enter = json_find_bool(params, "pressEnter", data.press_enter);
  }
  /* Check for finger */
  const char *fi = strstr(params, "\"finger\":");
  if (fi) {
    data.finger_id = json_find_int(params, "finger", data.finger_id);
  }

  touchpass_save_finger(slot, &data);
  rpc_send_ok(id, "{\"ok\":true}");
#else
  rpc_send_error(id, "Storage not available");
#endif
}

static void cmd_delete_finger(const char *params, int id) {
  int slot = json_find_int(params, "id", -1);
  if (slot < 0) {
    rpc_send_error(id, "Invalid ID");
    return;
  }

  if (touchpass_delete_template(slot, 1) == 0) {
#ifdef CONFIG_NVS
    touchpass_delete_finger(slot);
#endif
    rpc_send_ok(id, "{\"ok\":true}");
  } else {
    rpc_send_error(id, "Delete failed");
  }
}

static void cmd_enroll_start(const char *params, int id) {
  char name[32] = "New Finger";
  char password[64] = "";
  bool press_enter = true;
  int finger = -1;

  json_find_string(params, "name", name, sizeof(name));
  json_find_string(params, "password", password, sizeof(password));
  press_enter = json_find_bool(params, "pressEnter", true);
  finger = json_find_int(params, "finger", -1);

  int rc = touchpass_enroll_start(name, password, press_enter, finger);
  if (rc == 0) {
    rpc_send_ok(id, "{\"ok\":true}");
  } else {
    rpc_send_error(id, rc == -ENOMEM ? "Library full" : "Enrollment failed");
  }
}

static void cmd_enroll_status(const char *params, int id) {
  enum enroll_state state = touchpass_enroll_get_state();
  int step = touchpass_enroll_get_step();
  bool done = touchpass_enroll_is_done();
  bool ok = touchpass_enroll_was_successful();
  const char *msg = touchpass_enroll_get_message();
  const char *name = touchpass_enroll_get_name();

  bool captured = false;
  if (state == ENROLL_LIFT_1 || state == ENROLL_LIFT_2 ||
      state == ENROLL_LIFT_3 || state == ENROLL_LIFT_4 ||
      state == ENROLL_LIFT_5 || state == ENROLL_MERGING ||
      state == ENROLL_DONE) {
    captured = true;
  }

  snprintf(tx_buf, sizeof(tx_buf),
           "{\"step\":%d,\"message\":\"%s\",\"done\":%s,\"captured\":%s,"
           "\"ok\":%s,\"name\":\"%s\",\"status\":\"%s\"}",
           step, msg, done ? "true" : "false", captured ? "true" : "false",
           ok ? "true" : "false", name, msg);
  rpc_send_ok(id, tx_buf);
}

static void cmd_enroll_cancel(const char *params, int id) {
  touchpass_enroll_cancel();
  rpc_send_ok(id, "{\"ok\":true}");
}

static void cmd_get_ble_status(const char *params, int id) {
  bool connected = zmk_ble_active_profile_is_connected();

  snprintf(tx_buf, sizeof(tx_buf),
           "{\"status\":\"%s\",\"connected\":%s,\"mode\":\"BLE\","
           "\"name\":\"TouchPass\"}",
           connected ? "connected" : "advertising",
           connected ? "true" : "false");
  rpc_send_ok(id, tx_buf);
}

static void cmd_get_keyboard_mode(const char *params, int id) {
  bool connected = zmk_ble_active_profile_is_connected();
  snprintf(tx_buf, sizeof(tx_buf),
           "{\"current\":\"BLE\",\"availableModes\":[\"BLE\"],"
           "\"saved\":\"BLE\",\"connected\":%s}",
           connected ? "true" : "false");
  rpc_send_ok(id, tx_buf);
}

static void cmd_set_keyboard_mode(const char *params, int id) {
  rpc_send_ok(id, "{\"ok\":true}");
}

static void cmd_get_system_info(const char *params, int id) {
  snprintf(tx_buf, sizeof(tx_buf),
           "{\"version\":\"%s\",\"platform\":\"%s\",\"chip\":\"%s\","
           "\"cpu_freq\":64,\"flash_size\":1024}",
           FIRMWARE_VERSION, PLATFORM_NAME, PLATFORM_NAME);
  rpc_send_ok(id, tx_buf);
}

static void cmd_diagnostics(const char *params, int id) {
  snprintf(tx_buf, sizeof(tx_buf),
           "{\"platform\":\"%s\",\"firmware\":\"%s\","
           "\"uart1\":{\"rxPin\":7,\"txPin\":6,\"baud\":57600},"
           "\"usb\":{\"hid\":true,\"serial\":true,\"mode\":\"ble\"},"
           "\"sensor\":{\"connected\":%s}}",
           PLATFORM_NAME, FIRMWARE_VERSION,
           touchpass_is_sensor_ready() ? "true" : "false");
  rpc_send_ok(id, tx_buf);
}

static void cmd_get_detect(const char *params, int id) {
  snprintf(tx_buf, sizeof(tx_buf),
           "{\"ok\":true,\"detected\":%s,\"status\":\"%s\"}",
           finger_detected_pending ? "true" : "false", last_status);
  finger_detected_pending = false;
  rpc_send_ok(id, tx_buf);
}

static void cmd_reboot(const char *params, int id) {
  rpc_send_ok(id, "{\"ok\":true}");
  k_sleep(K_MSEC(500));
  sys_reboot(SYS_REBOOT_COLD);
}

/* ===== Command Dispatch ===== */

struct rpc_command {
  const char *name;
  void (*handler)(const char *params, int id);
};

static const struct rpc_command commands[] = {
    {"ping", cmd_ping},
    {"get_status", cmd_get_status},
    {"get_fingers", cmd_get_fingers},
    {"get_finger", cmd_get_finger},
    {"update_finger", cmd_update_finger},
    {"delete_finger", cmd_delete_finger},
    {"enroll_start", cmd_enroll_start},
    {"enroll_status", cmd_enroll_status},
    {"enroll_cancel", cmd_enroll_cancel},
    {"get_ble_status", cmd_get_ble_status},
    {"get_keyboard_mode", cmd_get_keyboard_mode},
    {"set_keyboard_mode", cmd_set_keyboard_mode},
    {"get_system_info", cmd_get_system_info},
    {"diagnostics", cmd_diagnostics},
    {"get_detect", cmd_get_detect},
    {"reboot", cmd_reboot},
};

static void process_line(const char *line) {
  /* Extract "cmd" */
  char cmd[32];
  if (json_find_string(line, "cmd", cmd, sizeof(cmd)) != 0) {
    rpc_send_error(-1, "Missing cmd field");
    return;
  }

  int id = json_find_int(line, "id", -1);

  /* Dispatch */
  for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
    if (strcmp(cmd, commands[i].name) == 0) {
      commands[i].handler(line, id);
      return;
    }
  }

  rpc_send_error(id, "Unknown command");
}

/* ===== RPC Thread ===== */

static void rpc_thread(void *p1, void *p2, void *p3) {
  rpc_dev = DEVICE_DT_GET(DT_NODELABEL(usb_cdc_acm_uart));

  if (!device_is_ready(rpc_dev)) {
    LOG_ERR("CDC ACM device not ready for RPC");
    return;
  }

  LOG_INF("TouchPass RPC: waiting for USB connection...");

  /* Wait for DTR (host opened serial port) */
  uint32_t dtr = 0;
  while (!dtr) {
    uart_line_ctrl_get(rpc_dev, UART_LINE_CTRL_DTR, &dtr);
    k_sleep(K_MSEC(100));
  }

  LOG_INF("TouchPass RPC thread started");

  /* Try sensor handshake now (sensor should be ready after 2s+ boot delay) */
  if (!touchpass_is_sensor_ready()) {
    LOG_INF("Attempting sensor handshake...");
    touchpass_check_sensor();
  }

  rx_pos = 0;

  while (1) {
    /* Read available bytes */
    uint8_t b;
    while (uart_poll_in(rpc_dev, &b) == 0) {
      if (b == '\n' || b == '\r') {
        if (rx_pos > 0) {
          rx_line[rx_pos] = '\0';
          process_line(rx_line);
          rx_pos = 0;
        }
      } else if (rx_pos < sizeof(rx_line) - 1) {
        rx_line[rx_pos++] = b;
      } else {
        /* Buffer overflow, reset */
        rx_pos = 0;
        rpc_send_error(-1, "Buffer overflow");
      }
    }

    /* Process enrollment steps if active */
    enum enroll_state state = touchpass_enroll_get_state();
    if (state != ENROLL_IDLE && state != ENROLL_DONE) {
      touchpass_enroll_step();
    }

    /* Heartbeat ping (every 5s) to keep serial alive */
    static uint32_t last_heartbeat;
    if (k_uptime_get_32() - last_heartbeat > 5000) {
      rpc_println("{\"ok\":true,\"ping\":true}");
      last_heartbeat = k_uptime_get_32();
    }

    k_sleep(K_MSEC(50));
  }
}

K_THREAD_DEFINE(tp_rpc_tid, 4096, rpc_thread, NULL, NULL, NULL, 7, 0, 2000);

#endif /* CONFIG_ZMK_TOUCHPASS_SERIAL_RPC */
