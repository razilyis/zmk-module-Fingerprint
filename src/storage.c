#include "touchpass.h"
#include <string.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(touchpass_storage, CONFIG_ZMK_LOG_LEVEL);

/* Separate NVS instance on touchpass_partition (independent of ZMK settings
 * NVS) */
static struct nvs_fs tp_nvs;
static bool storage_ready;

int touchpass_storage_init(void) {
  tp_nvs.flash_device = FIXED_PARTITION_DEVICE(touchpass_partition);
  if (!device_is_ready(tp_nvs.flash_device)) {
    LOG_ERR("TouchPass flash device not ready");
    return -ENODEV;
  }

  tp_nvs.offset = FIXED_PARTITION_OFFSET(touchpass_partition);
  tp_nvs.sector_size = 4096; /* nRF52840 flash page = 4KB */
  tp_nvs.sector_count = 8;   /* 32KB / 4KB */

  int rc = nvs_mount(&tp_nvs);
  if (rc != 0) {
    LOG_ERR("TouchPass NVS mount failed: %d", rc);
    return rc;
  }

  storage_ready = true;
  LOG_INF("TouchPass storage ready (NVS on touchpass_partition)");
  return 0;
}

int touchpass_get_finger(uint16_t id, finger_data_t *data) {
  if (!storage_ready) {
    return -ENODEV;
  }

  ssize_t rc = nvs_read(&tp_nvs, id, data, sizeof(finger_data_t));
  if (rc <= 0) {
    return -ENOENT;
  }

  return 0;
}

int touchpass_save_finger(uint16_t id, const finger_data_t *data) {
  if (!storage_ready) {
    return -ENODEV;
  }

  ssize_t rc = nvs_write(&tp_nvs, id, data, sizeof(finger_data_t));
  if (rc < 0) {
    LOG_ERR("NVS write failed for finger %d: %zd", id, rc);
    return (int)rc;
  }

  return 0;
}

int touchpass_delete_finger(uint16_t id) {
  if (!storage_ready) {
    return -ENODEV;
  }

  return nvs_delete(&tp_nvs, id);
}
