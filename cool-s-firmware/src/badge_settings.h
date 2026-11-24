/*
 * The badge's settings: brightness, the display mode it starts in and its
 * Bluetooth name. Kept as CBOR in SETTINGS_PATH, changed by the iPixel
 * brightness packet and over SMP (media_mgmt.c), saved a few seconds
 * after the last change so a slider does not wear out the flash.
 */
#ifndef BADGE_SETTINGS_H
#define BADGE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_PATH "/lfs1/settings.cbor"

/* The iPixel app and the web UI only find badges whose name starts with this. */
#define BADGE_NAME_PREFIX "LED_BLE_"
/* The advertised name has to fit into 31 bytes next to the flags: 31 - 3 - 2 - 8. */
#define BADGE_NAME_MAX 18

/* The modes the badge can start in, in the order of main.c's media kinds. */
enum badge_boot_mode {
	BOOT_PNG,
	BOOT_GIF,
	BOOT_LUA,
	BOOT_MODES,
};

struct badge_settings {
	uint8_t brightness; /* percent */
	uint8_t boot;       /* enum badge_boot_mode */
	char name[BADGE_NAME_MAX + 1]; /* after BADGE_NAME_PREFIX, "" for the default */
};

/* At start, once storage is mounted. Falls back to defaults. */
void badge_settings_load(void);

/* Saves a change that is still waiting, before storage is unmounted. */
void badge_settings_flush(void);

/* A consistent copy, safe from any thread. */
void badge_settings_get(struct badge_settings *out);

/* Brightness in percent, for rendering. */
uint8_t badge_settings_brightness(void);

/* "png", "gif" or "lua" */
const char *badge_boot_dir(uint8_t boot);
/* enum badge_boot_mode of a folder name, -EINVAL if there is none */
int badge_boot_from_dir(const char *dir, size_t len);
/* Printable ASCII without quotes or backslashes, no spaces at either end; "" for the default. */
bool badge_name_valid(const char *name, size_t len);

/* Safe from any thread. Return -EINVAL for values the badge does not take. */
int badge_settings_set_brightness(uint32_t percent);
int badge_settings_set_boot(const char *dir, size_t len);
int badge_settings_set_name(const char *name, size_t len);

/* Implemented in main.c, which acts on them. Called on the thread that changed the setting. */
void display_brightness_changed(void);
void bluetooth_name_changed(void);
/* The part after BADGE_NAME_PREFIX the badge uses without a name of its own. */
void bluetooth_default_name(char *out, size_t len);

#endif
