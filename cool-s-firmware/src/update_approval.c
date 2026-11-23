/*
 * A firmware image only gets onto the badge if someone holding it agrees, by
 * pressing each of the four buttons twice. SMP needs no pairing and released
 * images are signed with MCUboot's public example key, so this is what stops
 * anyone in radio range from installing firmware.
 *
 * There are two gates, both on the SMP work queue:
 *
 *  - The first chunk of an upload waits for the presses before anything is
 *    written. It has to be this chunk: Nordic's apps allow it 40 seconds, and
 *    every other request about 2.
 *
 *  - Scheduling the other slot to boot (boot_set_next, wrapped at link time)
 *    passes only for the image that was approved. An image that was already in
 *    the slot needs the presses at this point instead.
 *
 * An approval is for the file's SHA-256, not for a connection, so an upload can
 * resume after a dropped link without new presses. The slot is hashed once the
 * upload completes: if someone else finished it with other bytes, it is wiped.
 * The size limit keeps uploads out of the slot's trailer, where the bytes that
 * schedule a swap live.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/logging/log.h>

#include "update_approval.h"

LOG_MODULE_REGISTER(update_approval);

#define PRESSES_PER_BUTTON	2
/* shorter than the 40 seconds Nordic's apps wait for the first chunk */
#define APPROVAL_TIMEOUT	K_SECONDS(30)

#define SHA256_LEN	32
#define SECTOR_SIZE	4096
#define UPLOAD_SLOT_ID	FIXED_PARTITION_ID(slot1_partition)
/* swap-using-move keeps a sector for the trailer and one to move through */
#define MAX_IMAGE_SIZE	(FIXED_PARTITION_SIZE(slot1_partition) - 2 * SECTOR_SIZE)

enum session {
	SESSION_NONE,		/* nothing approved since boot */
	SESSION_UPLOADING,	/* approved, not complete */
	SESSION_VERIFIED,	/* the slot holds what was approved */
};

/* only touched on the SMP work queue */
static enum session session;
static uint8_t approved_sha[SHA256_LEN];
static size_t approved_size;

/* shared between the SMP work queue, the input thread and Bluetooth */
static struct k_spinlock lock;
static K_SEM_DEFINE(answered, 0, 1);
static bool waiting;
static bool granted;
static uint8_t presses[UPDATE_APPROVAL_BUTTONS];
static uint8_t held;

bool update_approval_waiting(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool result = waiting;

	k_spin_unlock(&lock, key);

	return result;
}

bool update_approval_button(unsigned int button, bool pressed)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool consumed = waiting;
	bool done = false;

	if (waiting && button < UPDATE_APPROVAL_BUTTONS) {
		if (pressed) {
			held |= BIT(button);
		} else if (held & BIT(button)) {
			/* count releases, of presses that began during this prompt */
			held &= ~BIT(button);
			if (presses[button] < PRESSES_PER_BUTTON) {
				presses[button]++;
			}

			done = true;
			for (int i = 0; i < UPDATE_APPROVAL_BUTTONS; i++) {
				done = done && presses[i] >= PRESSES_PER_BUTTON;
			}
			if (done) {
				waiting = false;
				granted = true;
			}
		}
	}

	k_spin_unlock(&lock, key);

	if (done) {
		k_sem_give(&answered);
	}

	return consumed;
}

static bool ask_for_presses(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool result;

	k_sem_reset(&answered);
	memset(presses, 0, sizeof(presses));
	held = 0;
	granted = false;
	waiting = true;
	k_spin_unlock(&lock, key);

	LOG_INF("Firmware update: waiting for each button to be pressed twice");
	(void)k_sem_take(&answered, APPROVAL_TIMEOUT);

	key = k_spin_lock(&lock);
	waiting = false;
	result = granted;
	k_spin_unlock(&lock, key);

	LOG_INF("Firmware update %s", result ? "approved" : "not approved");

	return result;
}

/* The requester is gone. Not waiting out the timeout also frees SMP for a reconnect. */
static void cancel_on_disconnect(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);

	if (update_approval_waiting()) {
		k_sem_give(&answered);
	}
}

BT_CONN_CB_DEFINE(approval_conn_callbacks) = {
	.disconnected = cancel_on_disconnect,
};

static int check_chunk(const struct img_mgmt_upload_req *req)
{
	if (req->off != 0) {
		return session == SESSION_UPLOADING ? MGMT_ERR_EOK : MGMT_ERR_EACCESSDENIED;
	}

	session = SESSION_NONE;

	if (req->image != 0 || req->data_sha.len != SHA256_LEN ||
	    req->size > MAX_IMAGE_SIZE || req->img_data.len > req->size) {
		LOG_WRN("Upload refused: needs a full SHA-256 and a size that fits");
		return MGMT_ERR_EINVAL;
	}

	if (!ask_for_presses()) {
		return MGMT_ERR_EACCESSDENIED;
	}

	memcpy(approved_sha, req->data_sha.value, SHA256_LEN);
	approved_size = req->size;
	session = SESSION_UPLOADING;

	return MGMT_ERR_EOK;
}

static void verify_upload(void)
{
	static uint8_t buf[512];
	const struct flash_area_check check = {
		.match = approved_sha,
		.clen = approved_size,
		.rbuf = buf,
		.rblen = sizeof(buf),
	};
	const struct flash_area *fa;

	if (session != SESSION_UPLOADING || flash_area_open(UPLOAD_SLOT_ID, &fa) != 0) {
		return;
	}

	if (flash_area_check_int_sha256(fa, &check) == 0) {
		session = SESSION_VERIFIED;
		LOG_INF("Uploaded image matches the approved one");
	} else {
		session = SESSION_NONE;
		LOG_ERR("Uploaded image is not the approved one, erasing it");
		/* without its header it can neither be listed nor activated */
		(void)flash_area_flatten(fa, 0, SECTOR_SIZE);
	}

	flash_area_close(fa);
}

static enum mgmt_cb_return on_image_event(uint32_t event, enum mgmt_cb_return prev_status,
					  int32_t *rc, uint16_t *group, bool *abort_more,
					  void *data, size_t data_size)
{
	if (event == MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK) {
		const struct img_mgmt_upload_check *check = data;
		int err = check_chunk(check->req);

		if (err != MGMT_ERR_EOK) {
			/* ERROR_RC, unlike ERROR_ERR, makes img_mgmt forget the upload */
			*rc = err;
			return MGMT_CB_ERROR_RC;
		}
	} else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING) {
		/* "pending" here means that the last chunk has been written */
		verify_upload();
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback image_callback = {
	.callback = on_image_event,
	.event_id = (MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK | MGMT_EVT_OP_IMG_MGMT_DFU_PENDING),
};

int __real_boot_set_next(const struct flash_area *fa, bool active, bool confirm);

int __wrap_boot_set_next(const struct flash_area *fa, bool active, bool confirm)
{
	/* confirming the running image needs no approval */
	if (!active && session != SESSION_VERIFIED) {
		if (session == SESSION_UPLOADING || !ask_for_presses()) {
			return -EACCES;
		}
		/* clients give up after 2 seconds here, so let their retry through */
		session = SESSION_VERIFIED;
	}

	return __real_boot_set_next(fa, active, confirm);
}

static int update_approval_init(void)
{
	mgmt_callback_register(&image_callback);

	return 0;
}

SYS_INIT(update_approval_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
