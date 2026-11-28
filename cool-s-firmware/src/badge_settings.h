/*
 * The badge's settings: brightness, its Bluetooth name and the file it
 * starts with. Kept as CBOR in SETTINGS_PATH, changed by the iPixel
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
/*
 * The default file is a path below the littlefs root, such as "png/cat.png",
 * short enough that MEDIA_ROOT "/" in front of it fits MEDIA_PATH_MAX.
 */
#define BADGE_PATH_MAX 57

struct badge_settings {
	uint8_t brightness; /* percent */
	char name[BADGE_NAME_MAX + 1]; /* after BADGE_NAME_PREFIX, "" for the default */
	/* the file the badge starts with, "" for the first one it has */
	char default_file[BADGE_PATH_MAX + 1];
};

/* At start, once storage is mounted. Falls back to defaults. */
void badge_settings_load(void);

/* Saves a change that is still waiting, before storage is unmounted. */
void badge_settings_flush(void);

/* A consistent copy, safe from any thread. */
void badge_settings_get(struct badge_settings *out);

/* Brightness in percent, for rendering. */
uint8_t badge_settings_brightness(void);

/* Printable ASCII without quotes or backslashes, no spaces at either end; "" for the default. */
bool badge_name_valid(const char *name, size_t len);

/*
 * A path below the root: no empty parts, none starting with "." (those are
 * the badge's temporary files), no control characters, at most
 * BADGE_PATH_MAX long. "" is valid too, for no default file.
 */
bool badge_path_valid(const char *path, size_t len);

/* Safe from any thread. Return -EINVAL for values the badge does not take. */
int badge_settings_set_brightness(uint32_t percent);
int badge_settings_set_name(const char *name, size_t len);
int badge_settings_set_default(const char *path, size_t len);

/* The default file as set, "" for none. */
void badge_settings_default(char *out, size_t len);

/*
 * A file or folder below the root was renamed, to is NULL if it was deleted.
 * A default at or below it follows it, or goes back to "".
 */
void badge_settings_path_moved(const char *from, const char *to);

/* Implemented in main.c, which acts on them. Called on the thread that changed the setting. */
void display_brightness_changed(void);
void bluetooth_name_changed(void);
/* The part after BADGE_NAME_PREFIX the badge uses without a name of its own. */
void bluetooth_default_name(char *out, size_t len);

#endif
