/*
 * A GIF the web UI plays on the badge without saving it. It arrives over SMP
 * (media_mgmt.c) into RAM, and the main loop plays it from there. Nothing
 * about a preview is written to flash.
 */
#ifndef PREVIEW_H
#define PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A 30 x 1 frame takes about 50 bytes, so this holds about 300 frames. */
#define PREVIEW_MAX (16 * 1024)

/*
 * Called on the SMP thread. off 0 starts a new preview of len bytes. Returns
 * the number of bytes received so far, -EFBIG if len is too large, or -EINVAL
 * if the piece does not continue the preview. The last piece starts playback.
 */
int preview_receive(uint32_t off, uint32_t len, const uint8_t *data, size_t n);

/* Called on the SMP thread: stops the preview, leaving its frame showing. */
void preview_end(void);

/* For the main loop. While a preview is arriving, its buffer must not be played. */
bool preview_receiving(void);
const uint8_t *preview_data(size_t *len);

/* Implemented in main.c, which acts on them in its own loop. */
void display_preview_play(void);
void display_preview_end(void);

#endif
