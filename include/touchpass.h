#ifndef TOUCHPASS_H
#define TOUCHPASS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

/* R502-A Constants */
#define FP_HEADER 0xEF01
#define FP_DEFAULT_ADDR 0xFFFFFFFF
#define FP_CMD_PACKET 0x01
#define FP_ACK_PACKET 0x07

/* Command Codes */
#define CMD_HANDSHAKE 0x40
#define CMD_GENIMG 0x01
#define CMD_IMG2TZ 0x02
#define CMD_REGMODEL 0x05
#define CMD_STORE 0x06
#define CMD_DELETCHAR 0x0C
#define CMD_EMPTY 0x0D
#define CMD_SEARCH 0x04
#define CMD_READSYSPARA 0x0F
#define CMD_TEMPLATENUM 0x1D
#define CMD_READINDEXTABLE 0x1F
#define CMD_AURALEDCONFIG 0x35

/* LED Styles */
#define LED_OFF 0x00
#define LED_ON 0x01
#define LED_BREATHING 0x02
#define LED_BLINKING 0x03
#define LED_FLASHING 0x04

/* Colors */
#define FP_LED_RED 0x01
#define FP_LED_BLUE 0x02
#define FP_LED_PURPLE 0x03
#define FP_LED_GREEN 0x04

/* Enrollment States */
enum enroll_state {
  ENROLL_IDLE,
  ENROLL_START_REQUESTED,
  ENROLL_CAPTURE_1,
  ENROLL_LIFT_1,
  ENROLL_CAPTURE_2,
  ENROLL_LIFT_2,
  ENROLL_CAPTURE_3,
  ENROLL_LIFT_3,
  ENROLL_CAPTURE_4,
  ENROLL_LIFT_4,
  ENROLL_CAPTURE_5,
  ENROLL_LIFT_5,
  ENROLL_CAPTURE_6,
  ENROLL_MERGING,
  ENROLL_DONE
};

/* Data Structures */
typedef struct {
  char name[32];
  char password[64];
  bool press_enter;
  int finger_id;
} finger_data_t;

/* Driver Init & Authentication */
int touchpass_init(void);
int touchpass_authenticate(finger_data_t *data);
int touchpass_type_password(const finger_data_t *data);

/* Sensor Status */
int touchpass_check_sensor(void);
bool touchpass_is_sensor_ready(void);
int touchpass_get_template_count(uint16_t *count);
int touchpass_get_library_size(uint16_t *size);
int touchpass_read_index_table(uint8_t page, uint8_t *table);
int touchpass_delete_template(uint16_t id, uint16_t count);
int touchpass_set_led(uint8_t ctrl, uint8_t speed, uint8_t color, uint8_t times);

/* Enrollment API */
int touchpass_enroll_start(const char *name, const char *password,
                           bool press_enter, int finger_id);
int touchpass_enroll_step(void);
void touchpass_enroll_cancel(void);
enum enroll_state touchpass_enroll_get_state(void);
int touchpass_enroll_get_step(void);
bool touchpass_enroll_is_done(void);
bool touchpass_enroll_was_successful(void);
const char *touchpass_enroll_get_message(void);
const char *touchpass_enroll_get_name(void);

/* Detection polling (non-blocking, for RPC idle loop) */
int touchpass_poll_detection(void);

/* Storage API (available when CONFIG_NVS is enabled) */
int touchpass_storage_init(void);
int touchpass_get_finger(uint16_t id, finger_data_t *data);
int touchpass_save_finger(uint16_t id, const finger_data_t *data);
int touchpass_delete_finger(uint16_t id);
int touchpass_list_fingers(void (*cb)(uint16_t id, const finger_data_t *data,
                                     void *user_data),
                           void *user_data);

/* HID helper (US keyboard layout): convert ASCII -> usage + shift flag */
static inline bool ascii_to_hid_key(char c, uint8_t *usage, bool *shift) {
  if (!usage || !shift)
    return false;

  *shift = false;

  if (c >= 'a' && c <= 'z') {
    *usage = 0x04 + (c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    *usage = 0x04 + (c - 'A');
    *shift = true;
    return true;
  }
  if (c >= '1' && c <= '9') {
    *usage = 0x1E + (c - '1');
    return true;
  }
  if (c == '0') {
    *usage = 0x27;
    return true;
  }

  switch (c) {
  case ' ':
    *usage = 0x2C;
    return true;
  case '-':
    *usage = 0x2D;
    return true;
  case '_':
    *usage = 0x2D;
    *shift = true;
    return true;
  case '=':
    *usage = 0x2E;
    return true;
  case '+':
    *usage = 0x2E;
    *shift = true;
    return true;
  case '[':
    *usage = 0x2F;
    return true;
  case '{':
    *usage = 0x2F;
    *shift = true;
    return true;
  case ']':
    *usage = 0x30;
    return true;
  case '}':
    *usage = 0x30;
    *shift = true;
    return true;
  case '\\':
    *usage = 0x31;
    return true;
  case '|':
    *usage = 0x31;
    *shift = true;
    return true;
  case ';':
    *usage = 0x33;
    return true;
  case ':':
    *usage = 0x33;
    *shift = true;
    return true;
  case '\'':
    *usage = 0x34;
    return true;
  case '"':
    *usage = 0x34;
    *shift = true;
    return true;
  case '`':
    *usage = 0x35;
    return true;
  case '~':
    *usage = 0x35;
    *shift = true;
    return true;
  case ',':
    *usage = 0x36;
    return true;
  case '<':
    *usage = 0x36;
    *shift = true;
    return true;
  case '.':
    *usage = 0x37;
    return true;
  case '>':
    *usage = 0x37;
    *shift = true;
    return true;
  case '/':
    *usage = 0x38;
    return true;
  case '?':
    *usage = 0x38;
    *shift = true;
    return true;
  case '!':
    *usage = 0x1E;
    *shift = true;
    return true;
  case '@':
    *usage = 0x1F;
    *shift = true;
    return true;
  case '#':
    *usage = 0x20;
    *shift = true;
    return true;
  case '$':
    *usage = 0x21;
    *shift = true;
    return true;
  case '%':
    *usage = 0x22;
    *shift = true;
    return true;
  case '^':
    *usage = 0x23;
    *shift = true;
    return true;
  case '&':
    *usage = 0x24;
    *shift = true;
    return true;
  case '*':
    *usage = 0x25;
    *shift = true;
    return true;
  case '(':
    *usage = 0x26;
    *shift = true;
    return true;
  case ')':
    *usage = 0x27;
    *shift = true;
    return true;
  default:
    return false;
  }
}

#endif /* TOUCHPASS_H */
