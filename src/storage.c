#include "touchpass.h"
#include <stdio.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(touchpass_storage, CONFIG_ZMK_LOG_LEVEL);

#define STORAGE_DIR CONFIG_ZMK_TOUCHPASS_STORAGE_PATH

/* JSON Descriptors (Zephyr's simple JSON) */
static const struct json_obj_descr finger_descr[] = {
    JSON_OBJ_DESCR_PRIM(finger_data_t, name, JSON_TYPE_STRING),
    JSON_OBJ_DESCR_PRIM(finger_data_t, password, JSON_TYPE_STRING),
    JSON_OBJ_DESCR_PRIM(finger_data_t, press_enter, JSON_TYPE_BOOL),
    JSON_OBJ_DESCR_PRIM(finger_data_t, finger_id, JSON_TYPE_INT),
};

static int ensure_dir(void) {
  struct fs_dirent entry;
  if (fs_stat(STORAGE_DIR, &entry) != 0) {
    return fs_mkdir(STORAGE_DIR);
  }
  return 0;
}

int touchpass_get_finger(uint16_t id, finger_data_t *data) {
  char path[64];
  snprintf(path, sizeof(path), "%s/f%d.json", STORAGE_DIR, id);

  struct fs_file_t file;
  fs_file_t_init(&file);
  if (fs_open(&file, path, FS_O_READ) != 0)
    return -ENOENT;

  char buf[256];
  ssize_t len = fs_read(&file, buf, sizeof(buf) - 1);
  fs_close(&file);

  if (len <= 0)
    return -EIO;
  buf[len] = '\0';

  return json_obj_parse(buf, len, finger_descr, ARRAY_SIZE(finger_descr), data);
}

int touchpass_save_finger(uint16_t id, const finger_data_t *data) {
  ensure_dir();
  char path[64];
  snprintf(path, sizeof(path), "%s/f%d.json", STORAGE_DIR, id);

  char buf[256];
  int len = json_obj_encode_buf(finger_descr, ARRAY_SIZE(finger_descr), data,
                                buf, sizeof(buf));
  if (len < 0)
    return len;

  struct fs_file_t file;
  fs_file_t_init(&file);
  if (fs_open(&file, path, FS_O_CREATE | FS_O_WRITE) != 0)
    return -EIO;
  fs_write(&file, buf, len);
  fs_close(&file);

  return 0;
}
/* More storage methods will follow */
