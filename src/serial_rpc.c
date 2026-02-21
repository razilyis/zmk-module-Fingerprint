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
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/usb.h>

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
static char data_buf[RPC_BUF_SIZE];
static char rpc_frame_buf[RPC_TX_BUF_SIZE];

static char last_status[64] = "Ready";

/* Latch for cmd_get_detect: prevent re-authenticating while the same finger
 * stays on the sensor across consecutive get_detect calls. Reset on no-finger. */
static bool detect_latched;

/* Cached sensor info (refreshed periodically, not on every call) */
static uint16_t cached_count;
static uint16_t cached_capacity = 200;
static uint32_t last_sensor_refresh;
static uint32_t last_tx_time;

/* ===== UART I/O ===== */

static void rpc_write(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uart_poll_out(rpc_dev, data[i]);
  }
  last_tx_time = k_uptime_get_32();
}

static void rpc_println(const char *str) {
  rpc_write(str, strlen(str));
  uart_poll_out(rpc_dev, '\n');
}

/* ===== Minimal JSON Helpers ===== */

/* Escape a string for safe JSON embedding.
 * Handles: \ " \n \r \t and strips other control characters. */
static void json_escape_string(const char *in, char *out, size_t out_size) {
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j < out_size - 1; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '\\' || c == '"') {
      if (j + 2 >= out_size)
        break;
      out[j++] = '\\';
      out[j++] = c;
    } else if (c == '\n') {
      if (j + 2 >= out_size)
        break;
      out[j++] = '\\';
      out[j++] = 'n';
    } else if (c == '\r') {
      if (j + 2 >= out_size)
        break;
      out[j++] = '\\';
      out[j++] = 'r';
    } else if (c == '\t') {
      if (j + 2 >= out_size)
        break;
      out[j++] = '\\';
      out[j++] = 't';
    } else if (c >= 0x20) {
      out[j++] = c;
    }
    /* Skip other control characters (< 0x20) */
  }
  out[j] = '\0';
}

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
      p++;
    }
    out[i++] = *p++;
  }
  out[i] = '\0';
  return 0;
}

static int json_find_int(const char *json, const char *key, int def) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":", key);

  const char *p = strstr(json, search);
  if (!p)
    return def;
  p += strlen(search);

  while (*p == ' ')
    p++;

  if (*p == '-' || (*p >= '0' && *p <= '9'))
    return atoi(p);

  return def;
}

/* For request envelope fields (e.g. top-level "id"), use the last occurrence
 * so nested params objects do not override it. */
static int json_find_last_int(const char *json, const char *key, int def) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":", key);

  const char *last = NULL;
  const char *p = json;
  while ((p = strstr(p, search)) != NULL) {
    last = p + strlen(search);
    p += strlen(search);
  }

  if (!last)
    return def;

  while (*last == ' ')
    last++;

  if (*last == '-' || (*last >= '0' && *last <= '9'))
    return atoi(last);

  return def;
}

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

/* ===== Response Senders =====
 *
 * Handlers format data into data_buf, then pass it to these functions.
 * Senders use rpc_frame_buf to build the final JSON-RPC wrapper.
 */

static void rpc_send_ok(int id, const char *data_json) {
  int pos = 0;
  /* Trailing newline is standard, leading double newline was too aggressive */
  pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos,
                  "{\"ok\":true,\"status\":\"ok\",\"data\":%s", data_json);
  if (id >= 0) {
    pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos,
                    ",\"id\":%d", id);
  }
  pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos, "}\n");
  rpc_write(rpc_frame_buf, pos);
}

static void rpc_send_error(int id, const char *message) {
  int pos = 0;
  pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos,
                  "{\"ok\":false,\"status\":\"error\",\"message\":\"%s\"",
                  message);
  if (id >= 0) {
    pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos,
                    ",\"id\":%d", id);
  }
  pos += snprintf(rpc_frame_buf + pos, sizeof(rpc_frame_buf) - pos, "}\n");
  rpc_write(rpc_frame_buf, pos);
}

/* ===== Sensor Cache ===== */

static void refresh_sensor_cache(void) {
  if (!touchpass_is_sensor_ready())
    return;
  uint32_t now = k_uptime_get_32();
  if (now - last_sensor_refresh < 30000 && last_sensor_refresh != 0)
    return;
  touchpass_get_template_count(&cached_count);
  touchpass_get_library_size(&cached_capacity);
  last_sensor_refresh = now;
}

/* ===== Command Handlers ===== */

static void cmd_ping(const char *params, int id) {
  rpc_send_ok(id, "{\"pong\":true}");
}

static void cmd_get_status(const char *params, int id) {
  bool enrolling = (touchpass_enroll_get_state() != ENROLL_IDLE &&
                    touchpass_enroll_get_state() != ENROLL_DONE);

  bool ble_connected = zmk_ble_active_profile_is_connected();

  /* Use cached sensor values (refreshed every 30s in main loop) */
  snprintf(data_buf, sizeof(data_buf),
           "{\"ok\":true,\"sensor\":%s,\"count\":%d,\"capacity\":%d,"
           "\"last\":\"%s\",\"enrolling\":%s,\"ble_connected\":%s,"
           "\"ble_active_profile\":%d}",
           touchpass_is_sensor_ready() ? "true" : "false", cached_count,
           cached_capacity, last_status, enrolling ? "true" : "false",
           ble_connected ? "true" : "false", zmk_ble_active_profile_index());
  rpc_send_ok(id, data_buf);
}

static void cmd_get_fingers(const char *params, int id) {
  int pos = 0;
  pos += snprintf(data_buf + pos, sizeof(data_buf) - pos, "{\"fingers\":[");

#ifdef CONFIG_NVS
  if (touchpass_is_sensor_ready()) {
    uint8_t index_table[32];
    bool first = true;

    uint16_t total_pages = (cached_capacity + 255) / 256;
    for (uint8_t page = 0;
         page < total_pages && pos < (int)sizeof(data_buf) - 128; page++) {
      if (touchpass_read_index_table(page, index_table) != 0)
        continue;
      for (int i = 0; i < 32 && pos < (int)sizeof(data_buf) - 128; i++) {
        uint8_t byte_val = index_table[i];
        for (int bit = 0; bit < 8; bit++) {
          if (byte_val & (1 << bit)) {
            uint16_t slot = page * 256 + i * 8 + bit;
            if (slot >= cached_capacity)
              break;
            finger_data_t data;
            if (touchpass_get_finger(slot, &data) == 0) {
              if (!first)
                data_buf[pos++] = ',';
              first = false;
              char esc_name[40];
              json_escape_string(data.name, esc_name, sizeof(esc_name));
              pos += snprintf(data_buf + pos, sizeof(data_buf) - pos,
                              "{\"id\":%d,\"name\":\"%s\",\"fingerId\":%d,"
                              "\"pressEnter\":%s}",
                              slot, esc_name, data.finger_id,
                              data.press_enter ? "true" : "false");
            }
          }
        }
      }
    }
  }
#endif

  snprintf(data_buf + pos, sizeof(data_buf) - pos, "]}");
  rpc_send_ok(id, data_buf);
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

  snprintf(data_buf, sizeof(data_buf),
           "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"hasPassword\":%s,"
           "\"pressEnter\":%s,\"fingerId\":%d}",
           slot, data.name, (data.password[0] != '\0') ? "true" : "false",
           data.press_enter ? "true" : "false", data.finger_id);
  rpc_send_ok(id, data_buf);
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

  char tmp[64];
  if (json_find_string(params, "name", tmp, sizeof(tmp)) == 0) {
    strncpy(data.name, tmp, sizeof(data.name) - 1);
    data.name[sizeof(data.name) - 1] = '\0';
  }
  if (json_find_string(params, "password", tmp, sizeof(tmp)) == 0) {
    strncpy(data.password, tmp, sizeof(data.password) - 1);
    data.password[sizeof(data.password) - 1] = '\0';
  }
  if (strstr(params, "\"pressEnter\":")) {
    data.press_enter = json_find_bool(params, "pressEnter", data.press_enter);
  }
  if (strstr(params, "\"finger\":")) {
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

  char esc_msg[64];
  char esc_name[40];
  json_escape_string(msg, esc_msg, sizeof(esc_msg));
  json_escape_string(name, esc_name, sizeof(esc_name));

  snprintf(data_buf, sizeof(data_buf),
           "{\"step\":%d,\"message\":\"%s\",\"done\":%s,\"captured\":%s,"
           "\"ok\":%s,\"name\":\"%s\",\"status\":\"%s\"}",
           step, esc_msg, done ? "true" : "false", captured ? "true" : "false",
           ok ? "true" : "false", esc_name, esc_msg);
  rpc_send_ok(id, data_buf);
}

static void cmd_enroll_cancel(const char *params, int id) {
  touchpass_enroll_cancel();
  rpc_send_ok(id, "{\"ok\":true}");
}

static void cmd_get_ble_status(const char *params, int id) {
  bool connected = zmk_ble_active_profile_is_connected();

  snprintf(data_buf, sizeof(data_buf),
           "{\"status\":\"%s\",\"connected\":%s,\"mode\":\"BLE\","
           "\"name\":\"TouchPass\"}",
           connected ? "connected" : "advertising",
           connected ? "true" : "false");
  rpc_send_ok(id, data_buf);
}

static void cmd_get_keyboard_mode(const char *params, int id) {
  bool connected = zmk_ble_active_profile_is_connected();
  snprintf(data_buf, sizeof(data_buf),
           "{\"current\":\"BLE\",\"availableModes\":[\"BLE\"],"
           "\"saved\":\"BLE\",\"connected\":%s}",
           connected ? "true" : "false");
  rpc_send_ok(id, data_buf);
}

static void cmd_set_keyboard_mode(const char *params, int id) {
  rpc_send_ok(id, "{\"ok\":true}");
}

static void cmd_get_system_info(const char *params, int id) {
  snprintf(data_buf, sizeof(data_buf),
           "{\"version\":\"%s\",\"platform\":\"%s\",\"chip\":\"%s\","
           "\"cpu_freq\":64,\"flash_size\":1024}",
           FIRMWARE_VERSION, PLATFORM_NAME, PLATFORM_NAME);
  rpc_send_ok(id, data_buf);
}

static void cmd_diagnostics(const char *params, int id) {
  snprintf(data_buf, sizeof(data_buf),
           "{\"platform\":\"%s\",\"firmware\":\"%s\","
           "\"uart1\":{\"rxPin\":7,\"txPin\":6,\"baud\":57600},"
           "\"usb\":{\"hid\":true,\"serial\":true,\"mode\":\"ble\"},"
           "\"sensor\":{\"connected\":%s}}",
           PLATFORM_NAME, FIRMWARE_VERSION,
           touchpass_is_sensor_ready() ? "true" : "false");
  rpc_send_ok(id, data_buf);
}

static void cmd_get_detect(const char *params, int id) {
  int rc = touchpass_poll_detection();
  bool detected = false;
  bool matched = false;
  uint16_t score = 0;
  char finger_name[32] = "";

  if (rc == 0) {
    detected = true;
    /* Authenticate on first detection; latch until finger is lifted to avoid
     * repeated auth calls while the same finger stays on the sensor. */
    if (!detect_latched) {
      finger_data_t auth_data;
      if (touchpass_authenticate(&auth_data) == 0) {
        matched = true;
        score = touchpass_get_last_score();
        strncpy(finger_name, auth_data.name, sizeof(finger_name) - 1);
        finger_name[sizeof(finger_name) - 1] = '\0';
        snprintf(last_status, sizeof(last_status), "Matched: %s", auth_data.name);
      } else {
        snprintf(last_status, sizeof(last_status), "No Match");
      }
      detect_latched = true;
    } else {
      strncpy(last_status, "Finger Detected", sizeof(last_status) - 1);
      last_status[sizeof(last_status) - 1] = '\0';
    }
  } else {
    detect_latched = false;
    if (rc == -ENODATA) {
      strncpy(last_status, "No Finger", sizeof(last_status) - 1);
    } else if (rc == -EBUSY) {
      strncpy(last_status, "Sensor Busy", sizeof(last_status) - 1);
    } else if (rc == -ENODEV) {
      strncpy(last_status, "Sensor Not Ready", sizeof(last_status) - 1);
    } else if (rc == -ETIMEDOUT) {
      strncpy(last_status, "Sensor Timeout", sizeof(last_status) - 1);
    } else {
      strncpy(last_status, "Sensor Error", sizeof(last_status) - 1);
    }
    last_status[sizeof(last_status) - 1] = '\0';
  }

  char esc_finger[40];
  json_escape_string(finger_name, esc_finger, sizeof(esc_finger));

  snprintf(data_buf, sizeof(data_buf),
           "{\"detected\":%s,\"matched\":%s,\"finger\":\"%s\","
           "\"score\":%d,\"status\":\"%s\"}",
           detected ? "true" : "false", matched ? "true" : "false",
           esc_finger, score, last_status);
  rpc_send_ok(id, data_buf);
}

static void cmd_reboot(const char *params, int id) {
  rpc_send_ok(id, "{\"ok\":true}");
  k_sleep(K_MSEC(500));
  sys_reboot(SYS_REBOOT_COLD);
}

static void cmd_clear_bonds(const char *params, int id) {
  LOG_WRN("RPC: Clearing all BLE bonds...");
  zmk_ble_clear_all_bonds();
  rpc_send_ok(id, "{\"ok\":true,\"message\":\"Bonds cleared! Rebooting...\"}");
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
    {"clear_bonds", cmd_clear_bonds},
};

static void process_line(const char *line) {
  char cmd[32];
  if (json_find_string(line, "cmd", cmd, sizeof(cmd)) != 0) {
    rpc_send_error(-1, "Missing cmd field");
    return;
  }

  int id = json_find_last_int(line, "id", -1);

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

  /* Sensor handshake is handled by the sensor_init_thread in
   * fingerprint_driver.c — no need to duplicate here. */

  rx_pos = 0;
  last_tx_time = k_uptime_get_32();
  bool sensor_cache_warmup_pending = true;
  uint32_t sensor_cache_warmup_at = 0;
  uint32_t last_sensor_cache_tick = 0;
  bool dtr_was_high = true;

  while (1) {
    /* Detect USB CDC ACM disconnect/reconnect (DTR changes). */
    uart_line_ctrl_get(rpc_dev, UART_LINE_CTRL_DTR, &dtr);
    if (!dtr) {
      if (dtr_was_high) {
        dtr_was_high = false;
        rx_pos = 0;
        LOG_WRN("TouchPass RPC: host disconnected (DTR low)");
      }
      k_sleep(K_MSEC(100));
      continue;
    }
    if (!dtr_was_high) {
      dtr_was_high = true;
      rx_pos = 0;
      last_tx_time = k_uptime_get_32();
      sensor_cache_warmup_pending = true;
      sensor_cache_warmup_at = 0;
      LOG_INF("TouchPass RPC: host reconnected (DTR high)");
    }

    /* Read available bytes */
    uint8_t b;
    bool had_command = false;
    while (uart_poll_in(rpc_dev, &b) == 0) {
      if (b == '\n' || b == '\r') {
        if (rx_pos > 0) {
          rx_line[rx_pos] = '\0';
          process_line(rx_line);
          rx_pos = 0;
          had_command = true;
        }
      } else if (rx_pos < sizeof(rx_line) - 1) {
        rx_line[rx_pos++] = b;
      } else {
        rx_pos = 0;
        rpc_send_error(-1, "Buffer overflow");
      }
    }

    uint32_t now = k_uptime_get_32();
    if (had_command && sensor_cache_warmup_pending && sensor_cache_warmup_at == 0) {
      /* Delay initial cache warmup until after first valid command response.
       * This prevents startup blocking from starving early RPC commands. */
      sensor_cache_warmup_at = now + 200;
    }

    /* Heartbeat ping (every 10s) to keep serial alive.
     * Only send if not currently enrolling and no activity for 30s.
     * We use a longer threshold (30s) to avoid any collision with host
     * commands.
     */
    bool enrolling = (touchpass_enroll_get_state() != ENROLL_IDLE &&
                      touchpass_enroll_get_state() != ENROLL_DONE);

    if (!enrolling && (now - last_tx_time > 30000)) {
      rpc_println("{\"ok\":true,\"ping\":true}");
    }

    /* Warm up sensor cache only after first command has been handled. */
    if (!enrolling && sensor_cache_warmup_pending && sensor_cache_warmup_at != 0 &&
        ((int32_t)(now - sensor_cache_warmup_at) >= 0)) {
      refresh_sensor_cache();
      sensor_cache_warmup_pending = false;
      last_sensor_cache_tick = now;
    }

    /* Keep cache reasonably fresh while idle (refresh_sensor_cache() has 30s
     * internal throttling, so frequent calls here are cheap). */
    if (!enrolling && !sensor_cache_warmup_pending &&
        (now - last_sensor_cache_tick > 1000)) {
      refresh_sensor_cache();
      last_sensor_cache_tick = now;
    }

    k_sleep(K_MSEC(50));
  }
}

K_THREAD_DEFINE(tp_rpc_tid, 4096, rpc_thread, NULL, NULL, NULL, 7, 0, 2000);

#endif /* CONFIG_ZMK_TOUCHPASS_SERIAL_RPC */
