/*
 * Files the badge shows: the defaults in the littlefs root and the media
 * folders the web UI manages, plus PNG conversion to and from LED pixels.
 */
#ifndef MEDIA_FILES_H
#define MEDIA_FILES_H

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/led_strip.h>

#define MEDIA_ROOT "/lfs1"
#define DEFAULT_GIF_PATH MEDIA_ROOT "/default.gif"
#define DEFAULT_PNG_PATH MEDIA_ROOT "/default.png"
#define DEFAULT_LUA_PATH MEDIA_ROOT "/default.lua"
/* raw pixel dump written by firmware up to v0.6, replaced by default.png */
#define LEGACY_DAT_PATH MEDIA_ROOT "/default.dat"

#define MEDIA_PATH_MAX 64

/* Larger PNGs would not fit the decoder into the heap. */
#define PNG_MAX_FILE_SIZE (16 * 1024)
#define PNG_MAX_PIXELS 1024

/* Number of files in MEDIA_ROOT/<dir>, hidden files left out. */
int media_count(const char *dir);

/* Path of the index-th file (1-based) in MEDIA_ROOT/<dir>, in directory order. */
int media_path(const char *dir, int index, char *out, size_t len);

/* Index (1-based) of the file at full path if it is in MEDIA_ROOT/<dir>, else 0 or an error. */
int media_index(const char *dir, const char *path);

/*
 * Around deleting, renaming or replacing path (a file or a folder) from
 * another thread. A GIF playing from there is closed first, as it streams
 * from its file, and the badge shows its default again afterwards. Scripts
 * and PNGs are read whole, so they are not affected. Implemented in main.c.
 */
void display_files_lock(const char *path);
void display_files_unlock(void);

/* Replace path with data, through tmp, so a reset leaves the old or the new file. */
int file_write_replace(const char *path, const char *tmp, const void *data, size_t len);

/*
 * Decode a PNG into the first n pixels, row by row. Pixels the image does
 * not have are black. out is only written if the whole PNG decodes.
 */
int png_to_pixels(const uint8_t *png, size_t len, struct led_rgb *out, size_t n);
int png_file_to_pixels(const char *path, struct led_rgb *out, size_t n);

/* Save n pixels as an n x 1 RGB PNG. */
int pixels_to_png_file(const struct led_rgb *pixels, size_t n, const char *path, const char *tmp);

/*
 * Erasing all storage: requested over SMP, done at the next start before
 * anything is mounted, as files stay open while the badge runs. The request
 * waits in GPREGRET2, which a soft reset keeps and a power-on reset clears.
 */
void storage_format_request(void);
/* Call before mounting. Erases the partition if requested; the mount then formats it. */
void storage_format_if_requested(uint8_t partition_id);

#endif
