/*
 * Settings storage, see badge_settings.h. The file is a CBOR map:
 *
 *   {"brightness": 0..100, "boot": "png" | "gif" | "lua", "name": tstr,
 *    "default_png": tstr, "default_gif": tstr, "default_lua": tstr}
 *
 * A default is a path below the root, "" or left out for the file in the
 * root (default.png and so on).
 *
 * Keys it does not know are skipped and missing ones keep their default, so
 * older and newer firmware can read each other's file.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "badge_settings.h"
#include "media_files.h"

LOG_MODULE_REGISTER(badge_settings);

#define DEFAULT_BRIGHTNESS 30
#define SAVE_DELAY K_SECONDS(5)
/* all keys and the longest values fit with room to spare */
#define FILE_MAX 320

BUILD_ASSERT(sizeof(MEDIA_ROOT "/") - 1 + BADGE_PATH_MAX < MEDIA_PATH_MAX,
	     "a default file's full path has to fit MEDIA_PATH_MAX");

static const char *const boot_dirs[BOOT_MODES] = {
	[BOOT_PNG] = "png",
	[BOOT_GIF] = "gif",
	[BOOT_LUA] = "lua",
};

static struct badge_settings settings = {
	.brightness = DEFAULT_BRIGHTNESS,
	.boot = BOOT_PNG,
};
K_MUTEX_DEFINE(settings_lock);

static void save(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(save_work, save);

const char *badge_boot_dir(uint8_t boot)
{
	return boot < BOOT_MODES ? boot_dirs[boot] : boot_dirs[BOOT_PNG];
}

int badge_boot_from_dir(const char *dir, size_t len)
{
	for (int i = 0; i < BOOT_MODES; i++) {
		if (len == strlen(boot_dirs[i]) && memcmp(dir, boot_dirs[i], len) == 0) {
			return i;
		}
	}
	return -EINVAL;
}

bool badge_name_valid(const char *name, size_t len)
{
	if (len > BADGE_NAME_MAX || (len && (name[0] == ' ' || name[len - 1] == ' '))) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (name[i] < 0x20 || name[i] > 0x7e || name[i] == '"' || name[i] == '\\') {
			return false;
		}
	}
	return true;
}

bool badge_path_valid(const char *path, size_t len)
{
	if (len > BADGE_PATH_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		bool part_start = i == 0 || path[i - 1] == '/';
		if ((uint8_t)path[i] < 0x20 || path[i] == 0x7f ||
		    (part_start && (path[i] == '/' || path[i] == '.'))) {
			return false;
		}
	}
	return len == 0 || path[len - 1] != '/';
}

static void copy_string(char *out, size_t size, const char *value, size_t len)
{
	len = MIN(len, size - 1);
	memcpy(out, value, len);
	out[len] = '\0';
}

static void save(struct k_work *work)
{
	uint8_t buf[FILE_MAX];
	struct badge_settings s;

	ARG_UNUSED(work);
	badge_settings_get(&s);

	ZCBOR_STATE_E(zse, 1, buf, sizeof(buf), 1);
	bool ok = zcbor_map_start_encode(zse, 6) &&
		  zcbor_tstr_put_lit(zse, "brightness") && zcbor_uint32_put(zse, s.brightness) &&
		  zcbor_tstr_put_lit(zse, "boot") && zcbor_tstr_put_term(zse, badge_boot_dir(s.boot), 4) &&
		  zcbor_tstr_put_lit(zse, "name") && zcbor_tstr_put_term(zse, s.name, sizeof(s.name)) &&
		  zcbor_tstr_put_lit(zse, "default_png") &&
		  zcbor_tstr_put_term(zse, s.defaults[BOOT_PNG], sizeof(s.defaults[0])) &&
		  zcbor_tstr_put_lit(zse, "default_gif") &&
		  zcbor_tstr_put_term(zse, s.defaults[BOOT_GIF], sizeof(s.defaults[0])) &&
		  zcbor_tstr_put_lit(zse, "default_lua") &&
		  zcbor_tstr_put_term(zse, s.defaults[BOOT_LUA], sizeof(s.defaults[0])) &&
		  zcbor_map_end_encode(zse, 6);
	if (!ok) {
		LOG_ERR("Could not encode the settings");
		return;
	}

	int rc = file_write_replace(SETTINGS_PATH, MEDIA_ROOT "/.settings.tmp", buf, zse->payload - buf);
	if (rc) {
		LOG_ERR("Could not save the settings: %d", rc);
	} else {
		LOG_INF("Settings saved");
	}
}

static void changed(void)
{
	k_work_reschedule(&save_work, SAVE_DELAY);
}

static ssize_t read_file(const char *path, void *buf, size_t len)
{
	struct fs_file_t file;

	fs_file_t_init(&file);
	int rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		return rc;
	}
	ssize_t got = fs_read(&file, buf, len);
	fs_close(&file);
	return got;
}

static int load_cbor(void)
{
	uint8_t buf[FILE_MAX];
	ssize_t len = read_file(SETTINGS_PATH, buf, sizeof(buf));
	uint32_t brightness = settings.brightness;
	struct zcbor_string boot = { 0 };
	struct zcbor_string name = { 0 };
	struct zcbor_string defaults[BOOT_MODES] = { 0 };
	size_t decoded;

	if (len < 0) {
		return len;
	}

	ZCBOR_STATE_D(zsd, 2, buf, len, 1, 0);
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("brightness", zcbor_uint32_decode, &brightness),
		ZCBOR_MAP_DECODE_KEY_DECODER("boot", zcbor_tstr_decode, &boot),
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &name),
		ZCBOR_MAP_DECODE_KEY_DECODER("default_png", zcbor_tstr_decode, &defaults[BOOT_PNG]),
		ZCBOR_MAP_DECODE_KEY_DECODER("default_gif", zcbor_tstr_decode, &defaults[BOOT_GIF]),
		ZCBOR_MAP_DECODE_KEY_DECODER("default_lua", zcbor_tstr_decode, &defaults[BOOT_LUA]),
	};
	if (zcbor_map_decode_bulk(zsd, fields, ARRAY_SIZE(fields), &decoded)) {
		return -EINVAL;
	}

	/* a value that is out of range keeps its default */
	if (brightness <= 100) {
		settings.brightness = brightness;
	}
	int mode = badge_boot_from_dir(boot.value, boot.len);
	if (mode >= 0) {
		settings.boot = mode;
	}
	if (name.value && badge_name_valid(name.value, name.len)) {
		copy_string(settings.name, sizeof(settings.name), name.value, name.len);
	}
	for (int i = 0; i < BOOT_MODES; i++) {
		if (defaults[i].value && badge_path_valid(defaults[i].value, defaults[i].len)) {
			copy_string(settings.defaults[i], sizeof(settings.defaults[i]),
				    defaults[i].value, defaults[i].len);
		}
	}
	return 0;
}

void badge_settings_load(void)
{
	if (load_cbor() == 0) {
		LOG_INF("Settings loaded");
	} else {
		LOG_WRN("No settings, using the defaults");
	}
	LOG_INF("Brightness %u%%, starts with %s, name \"%s\"", settings.brightness,
		badge_boot_dir(settings.boot), settings.name);
	for (int i = 0; i < BOOT_MODES; i++) {
		if (settings.defaults[i][0]) {
			LOG_INF("Default %s: %s", badge_boot_dir(i), settings.defaults[i]);
		}
	}
}

void badge_settings_flush(void)
{
	struct k_work_sync sync;

	k_work_flush_delayable(&save_work, &sync);
}

void badge_settings_get(struct badge_settings *out)
{
	k_mutex_lock(&settings_lock, K_FOREVER);
	*out = settings;
	k_mutex_unlock(&settings_lock);
}

uint8_t badge_settings_brightness(void)
{
	return settings.brightness;
}

int badge_settings_set_brightness(uint32_t percent)
{
	if (percent > 100) {
		return -EINVAL;
	}
	if (settings.brightness != percent) {
		settings.brightness = percent;
		changed();
	}
	display_brightness_changed();
	return 0;
}

int badge_settings_set_boot(const char *dir, size_t len)
{
	int mode = badge_boot_from_dir(dir, len);

	if (mode < 0) {
		return mode;
	}
	if (settings.boot != mode) {
		settings.boot = mode;
		changed();
	}
	return 0;
}

int badge_settings_set_name(const char *name, size_t len)
{
	if (!badge_name_valid(name, len)) {
		return -EINVAL;
	}
	k_mutex_lock(&settings_lock, K_FOREVER);
	bool same = strlen(settings.name) == len && memcmp(settings.name, name, len) == 0;
	if (!same) {
		memcpy(settings.name, name, len);
		settings.name[len] = '\0';
	}
	k_mutex_unlock(&settings_lock);
	if (!same) {
		changed();
		bluetooth_name_changed();
	}
	return 0;
}

int badge_settings_set_default(uint8_t mode, const char *path, size_t len)
{
	if (mode >= BOOT_MODES || !badge_path_valid(path, len)) {
		return -EINVAL;
	}
	k_mutex_lock(&settings_lock, K_FOREVER);
	char *current = settings.defaults[mode];
	bool same = strlen(current) == len && memcmp(current, path, len) == 0;
	if (!same) {
		copy_string(current, sizeof(settings.defaults[0]), path, len);
	}
	k_mutex_unlock(&settings_lock);
	if (!same) {
		changed();
	}
	return 0;
}

void badge_settings_default(uint8_t mode, char *out, size_t len)
{
	k_mutex_lock(&settings_lock, K_FOREVER);
	strncpy(out, mode < BOOT_MODES ? settings.defaults[mode] : "", len - 1);
	out[len - 1] = '\0';
	k_mutex_unlock(&settings_lock);
}

void badge_settings_path_moved(const char *from, const char *to)
{
	size_t from_len = strlen(from);
	bool any = false;

	k_mutex_lock(&settings_lock, K_FOREVER);
	for (int i = 0; i < BOOT_MODES; i++) {
		char *path = settings.defaults[i];
		/* the file itself, or one in the folder */
		if (!path[0] || strncmp(path, from, from_len) ||
		    (path[from_len] && path[from_len] != '/')) {
			continue;
		}
		char moved[BADGE_PATH_MAX + 1];
		int n = to ? snprintf(moved, sizeof(moved), "%s%s", to, &path[from_len]) : 0;
		if (!to || n >= (int)sizeof(moved)) {
			/* gone, or too long a path now: the file in the root again */
			path[0] = '\0';
		} else {
			strcpy(path, moved);
		}
		any = true;
	}
	k_mutex_unlock(&settings_lock);
	if (any) {
		changed();
	}
}
