#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "preview.h"

/*
 * Static, so a preview can never run the heap out. The main loop may still be
 * decoding an older preview from here while a new one arrives: it skips frames
 * while receiving is set, and the decoder never reads past the buffer.
 */
static uint8_t buffer[PREVIEW_MAX];
static size_t size;
static size_t received;
static atomic_t receiving;

int preview_receive(uint32_t off, uint32_t len, const uint8_t *data, size_t n)
{
	if (off == 0) {
		if (len == 0 || len > PREVIEW_MAX) {
			return -EFBIG;
		}
		atomic_set(&receiving, 1);
		size = len;
		received = 0;
	}

	if (!atomic_get(&receiving) || off != received || n > size - received) {
		return -EINVAL;
	}

	memcpy(&buffer[off], data, n);
	received += n;
	if (received == size) {
		atomic_set(&receiving, 0);
		display_preview_play();
	}
	return received;
}

void preview_end(void)
{
	atomic_set(&receiving, 0);
	display_preview_end();
}

bool preview_receiving(void)
{
	return atomic_get(&receiving);
}

const uint8_t *preview_data(size_t *len)
{
	*len = received == size ? size : 0;
	return buffer;
}
