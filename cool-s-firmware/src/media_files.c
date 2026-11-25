#include "media_files.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

#include "lodepng.h"

LOG_MODULE_REGISTER(media_files);

static bool is_media(const struct fs_dirent *entry)
{
	return entry->type == FS_DIR_ENTRY_FILE && entry->name[0] != '.';
}

/*
 * index <= 0 counts the files, otherwise copies the index-th path into out.
 * With find, returns the index of the file of that name instead.
 */
static int media_walk(const char *dir, int index, char *out, size_t len, const char *find)
{
	char path[MEDIA_PATH_MAX];
	struct fs_dir_t d;
	struct fs_dirent entry;
	int count = 0;
	int rc;

	snprintf(path, sizeof(path), MEDIA_ROOT "/%s", dir);
	fs_dir_t_init(&d);
	rc = fs_opendir(&d, path);
	if (rc < 0) {
		/* the folder only exists after the first upload */
		return rc == -ENOENT ? (index > 0 ? -ENOENT : 0) : rc;
	}
	for (;;) {
		rc = fs_readdir(&d, &entry);
		if (rc < 0 || entry.name[0] == '\0') {
			break;
		}
		if (!is_media(&entry)) {
			continue;
		}
		count++;
		if (find && strcmp(entry.name, find) == 0) {
			fs_closedir(&d);
			return count;
		}
		if (!find && count == index) {
			if (snprintf(out, len, "%s/%s", path, entry.name) >= (int)len) {
				rc = -ENAMETOOLONG;
			}
			break;
		}
	}
	fs_closedir(&d);
	if (rc < 0) {
		return rc;
	}
	if (find || index > 0) {
		return find || count != index ? -ENOENT : 0;
	}
	return count;
}

int media_count(const char *dir)
{
	return media_walk(dir, 0, NULL, 0, NULL);
}

int media_path(const char *dir, int index, char *out, size_t len)
{
	if (index <= 0) {
		return -EINVAL;
	}
	return media_walk(dir, index, out, len, NULL);
}

int media_index(const char *dir, const char *path)
{
	char folder[MEDIA_PATH_MAX];
	int n = snprintf(folder, sizeof(folder), MEDIA_ROOT "/%s/", dir);
	const char *name = &path[n];

	if (strncmp(path, folder, n) || !*name || strchr(name, '/')) {
		return -ENOENT;
	}
	return media_walk(dir, 0, NULL, 0, name);
}

int file_write_replace(const char *path, const char *tmp, const void *data, size_t len)
{
	struct fs_file_t file;
	int rc;

	fs_unlink(tmp);
	fs_file_t_init(&file);
	rc = fs_open(&file, tmp, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		LOG_ERR("open %s: %d", tmp, rc);
		return rc;
	}
	ssize_t written = fs_write(&file, data, len);
	int closed = fs_close(&file);
	if (written != (ssize_t)len || closed < 0) {
		LOG_ERR("write %s: %d", tmp, written < 0 ? (int)written : closed < 0 ? closed : -ENOSPC);
		fs_unlink(tmp);
		return written < 0 ? (int)written : closed < 0 ? closed : -ENOSPC;
	}
	rc = fs_rename(tmp, path);
	if (rc < 0) {
		LOG_ERR("rename %s: %d", path, rc);
		fs_unlink(tmp);
	}
	return rc;
}

int png_to_pixels(const uint8_t *png, size_t len, struct led_rgb *out, size_t n)
{
	unsigned int width;
	unsigned int height;
	unsigned char *image = NULL;
	LodePNGState state;

	/* the header alone, so a huge image is refused before anything is allocated */
	lodepng_state_init(&state);
	unsigned int error = lodepng_inspect(&width, &height, &state, png, len);
	lodepng_state_cleanup(&state);
	if (error) {
		LOG_ERR("PNG: %s", lodepng_error_text(error));
		return -EINVAL;
	}
	if ((uint64_t)width * height > PNG_MAX_PIXELS) {
		LOG_ERR("PNG: %ux%u is too large", width, height);
		return -EFBIG;
	}
	error = lodepng_decode_memory(&image, &width, &height, png, len, LCT_RGB, 8);
	if (error) {
		LOG_ERR("PNG: %s", lodepng_error_text(error));
		free(image);
		return -EINVAL;
	}
	size_t count = (size_t)width * height;
	for (size_t i = 0; i < n; i++) {
		if (i < count) {
			out[i].r = image[i * 3];
			out[i].g = image[i * 3 + 1];
			out[i].b = image[i * 3 + 2];
		} else {
			out[i].r = 0;
			out[i].g = 0;
			out[i].b = 0;
		}
	}
	free(image);
	return 0;
}

int png_file_to_pixels(const char *path, struct led_rgb *out, size_t n)
{
	struct fs_dirent info;
	struct fs_file_t file;
	int rc;

	rc = fs_stat(path, &info);
	if (rc < 0) {
		return rc;
	}
	if (info.type != FS_DIR_ENTRY_FILE || info.size == 0) {
		return -EINVAL;
	}
	if (info.size > PNG_MAX_FILE_SIZE) {
		LOG_ERR("%s: %u bytes is too large", path, (unsigned int)info.size);
		return -EFBIG;
	}
	uint8_t *data = malloc(info.size);
	if (!data) {
		return -ENOMEM;
	}
	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc == 0) {
		ssize_t got = fs_read(&file, data, info.size);
		fs_close(&file);
		rc = got == (ssize_t)info.size ? png_to_pixels(data, info.size, out, n)
					       : (got < 0 ? (int)got : -EIO);
	}
	free(data);
	return rc;
}

/*
 * lodepng is built without its 8 KB of CRC tables (LODEPNG_NO_COMPILE_CRC),
 * so it uses this one, which is also what the PNG writer below uses.
 */
unsigned lodepng_crc32(const unsigned char *data, size_t length)
{
	static const uint32_t table[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
		0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
		0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};
	uint32_t r = 0xffffffffu;

	while (length--) {
		r = table[(r ^ *data) & 0xf] ^ (r >> 4);
		r = table[(r ^ (*data >> 4)) & 0xf] ^ (r >> 4);
		data++;
	}
	return r ^ 0xffffffffu;
}

static uint8_t *put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
	return p + 4;
}

/* type and data are already in place after the length, the CRC follows them */
static uint8_t *finish_chunk(uint8_t *chunk, uint32_t len)
{
	put_be32(chunk, len);
	return put_be32(chunk + 8 + len, lodepng_crc32(chunk + 4, len + 4));
}

/*
 * An n x 1 RGB PNG, written by hand so that lodepng's encoder can be left
 * out of the image: one scanline without filter, in one stored deflate block.
 */
int pixels_to_png_file(const struct led_rgb *pixels, size_t n, const char *path, const char *tmp)
{
	static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	const size_t row = 1 + n * 3;
	/* zlib header, stored block header, scanline, Adler-32 */
	const size_t idat_len = 2 + 5 + row + 4;
	const size_t total = sizeof(signature) + (12 + 13) + (12 + idat_len) + 12;
	uint32_t a = 1;
	uint32_t b = 0;
	int rc;

	/* a single stored block holds at most 65535 bytes */
	if (n == 0 || row > 0xffff) {
		return -EINVAL;
	}
	uint8_t *png = malloc(total);
	if (!png) {
		return -ENOMEM;
	}

	uint8_t *p = png;
	memcpy(p, signature, sizeof(signature));
	p += sizeof(signature);

	uint8_t *chunk = p;
	memcpy(chunk + 4, "IHDR", 4);
	p = put_be32(chunk + 8, n);
	p = put_be32(p, 1);
	*p++ = 8; /* bit depth */
	*p++ = 2; /* RGB */
	*p++ = 0; /* deflate */
	*p++ = 0; /* adaptive filtering */
	*p++ = 0; /* not interlaced */
	p = finish_chunk(chunk, 13);

	chunk = p;
	memcpy(chunk + 4, "IDAT", 4);
	p = chunk + 8;
	*p++ = 0x78; /* deflate, 32 KB window */
	*p++ = 0x01; /* no preset dictionary, header check */
	*p++ = 0x01; /* final block, stored */
	*p++ = row & 0xff;
	*p++ = row >> 8;
	*p++ = ~row & 0xff;
	*p++ = (~row >> 8) & 0xff;
	uint8_t *scanline = p;
	*p++ = 0; /* filter type None */
	for (size_t i = 0; i < n; i++) {
		*p++ = pixels[i].r;
		*p++ = pixels[i].g;
		*p++ = pixels[i].b;
	}
	for (size_t i = 0; i < row; i++) {
		a = (a + scanline[i]) % 65521;
		b = (b + a) % 65521;
	}
	put_be32(p, (b << 16) | a);
	p = finish_chunk(chunk, idat_len);

	chunk = p;
	memcpy(chunk + 4, "IEND", 4);
	p = finish_chunk(chunk, 0);

	rc = file_write_replace(path, tmp, png, p - png);
	free(png);
	return rc;
}

/* GPREGRET2: GPREGRET (index 0) is where reboot types usually go */
#define FORMAT_REGISTER 1
#define FORMAT_MAGIC 0xF5

static void format_reboot(struct k_work *work)
{
	ARG_UNUSED(work);
	/* littlefs survives a reset at any point, and is erased next anyway */
	sys_reboot(SYS_REBOOT_WARM);
}

static K_WORK_DELAYABLE_DEFINE(format_work, format_reboot);

void storage_format_request(void)
{
	LOG_WRN("Erasing the storage after a restart");
	nrf_power_gpregret_set(NRF_POWER, FORMAT_REGISTER, FORMAT_MAGIC);
	/* time for the SMP response to go out */
	k_work_schedule(&format_work, K_MSEC(500));
}

void storage_format_if_requested(uint8_t partition_id)
{
	const struct flash_area *fa;

	if (nrf_power_gpregret_get(NRF_POWER, FORMAT_REGISTER) != FORMAT_MAGIC) {
		return;
	}
	/* first, so an erase that fails cannot loop */
	nrf_power_gpregret_set(NRF_POWER, FORMAT_REGISTER, 0);

	int rc = flash_area_open(partition_id, &fa);
	if (rc == 0) {
		rc = flash_area_erase(fa, 0, fa->fa_size);
		flash_area_close(fa);
	}
	if (rc < 0) {
		LOG_ERR("Erasing the storage failed: %d", rc);
	} else {
		LOG_WRN("Storage erased");
	}
}
