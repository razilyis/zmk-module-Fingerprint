#ifndef TOUCHPASS_H
#define TOUCHPASS_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdint.h>
#include <stdbool.h>

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

/* Colors */
#define FP_LED_RED 0x01
#define FP_LED_BLUE 0x02
#define FP_LED_PURPLE 0x03
#define FP_LED_GREEN 0x04

/* Data Structures */
typedef struct {
    char name[32];
    char password[64];
    bool press_enter;
    int finger_id;
} finger_data_t;

/* Public API */
int touchpass_init(void);
int touchpass_authenticate(finger_data_t *data);
int touchpass_enroll_start(const char *name, const char *password, bool press_enter, int finger_id);
int touchpass_enroll_step(void);
void touchpass_enroll_cancel(void);
int touchpass_delete_finger(uint16_t id);
int touchpass_get_finger(uint16_t id, finger_data_t *data);

#endif /* TOUCHPASS_H */
