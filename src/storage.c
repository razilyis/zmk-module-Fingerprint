#include "touchpass.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(touchpass_storage, CONFIG_ZMK_LOG_LEVEL);

#define TP_MNT_POINT "/tp"

/* LittleFS mount (manual — do NOT use CONFIG_FS_LITTLEFS_FMP_DEV) */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(tp_lfs_data);

static struct fs_mount_t tp_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &tp_lfs_data,
    .storage_dev = (void *)FIXED_PARTITION_ID(touchpass_partition),
    .mnt_point = TP_MNT_POINT,
};

static bool storage_mounted;

int touchpass_storage_init(void) {
    int rc = fs_mount(&tp_mnt);
    if (rc != 0) {
        LOG_ERR("TouchPass LittleFS mount failed: %d", rc);
        return rc;
    }
    storage_mounted = true;
    LOG_INF("TouchPass storage mounted at %s", TP_MNT_POINT);
    return 0;
}

/* JSON descriptors for finger_data_t */
static const struct json_obj_descr finger_descr[] = {
    JSON_OBJ_DESCR_PRIM(finger_data_t, name, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(finger_data_t, password, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(finger_data_t, press_enter, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM(finger_data_t, finger_id, JSON_TOK_NUMBER),
};

int touchpass_get_finger(uint16_t id, finger_data_t *data) {
    if (!storage_mounted) {
        return -ENODEV;
    }

    char path[48];
    snprintf(path, sizeof(path), TP_MNT_POINT "/f%d.json", id);

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, path, FS_O_READ) != 0) {
        return -ENOENT;
    }

    char buf[256];
    ssize_t len = fs_read(&file, buf, sizeof(buf) - 1);
    fs_close(&file);

    if (len <= 0) {
        return -EIO;
    }
    buf[len] = '\0';

    return json_obj_parse(buf, len, finger_descr, ARRAY_SIZE(finger_descr), data);
}

int touchpass_save_finger(uint16_t id, const finger_data_t *data) {
    if (!storage_mounted) {
        return -ENODEV;
    }

    char path[48];
    snprintf(path, sizeof(path), TP_MNT_POINT "/f%d.json", id);

    char buf[256];
    int ret = json_obj_encode_buf(finger_descr, ARRAY_SIZE(finger_descr),
                                  data, buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }

    size_t len = strlen(buf);
    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, path, FS_O_CREATE | FS_O_WRITE) != 0) {
        return -EIO;
    }
    fs_write(&file, buf, len);
    fs_close(&file);

    return 0;
}

int touchpass_delete_finger(uint16_t id) {
    if (!storage_mounted) {
        return -ENODEV;
    }

    char path[48];
    snprintf(path, sizeof(path), TP_MNT_POINT "/f%d.json", id);
    return fs_unlink(path);
}
