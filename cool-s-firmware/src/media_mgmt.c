/*
 * SMP command group for the media folders on littlefs.
 *
 * The stock MCUmgr file system group can only upload, download and stat a file
 * whose path is already known. This group adds what a file manager also needs.
 * File contents still travel through the stock group.
 *
 * Group 64, "dir" is always "gif", "png" or "lua":
 *
 *  id op     request              response
 *   0 read   {}                   {block, blocks, free}        (free is in blocks)
 *   1 read   {dir, skip}          {files: [name, size, ...], next?}
 *   2 write  {dir}                {}  create the folder, drop any stale upload
 *   3 write  {dir, name, size}    {}  move the finished upload into the folder
 *   4 write  {dir, name}          {}  delete a file
 *   5 write  {off, data, len?}    {off}  preview a GIF from RAM, len with off 0
 *   5 write  {len: 0}             {}  end the preview, its frame stays up
 *   6 read   {path, skip}         {entries: [name, is_dir, size, ...], next?}
 *   7 write  {path, size}         {}  move the finished upload to path, making its folders
 *   8 write  {dir: "lua", name}   {}  run that script now, as if picked with the buttons
 *   9 read   {}                   {running, path, error}  the last script started, error ""
 *                                     unless it failed
 *  10 write  {}                   {}  restart and erase all storage, settings included
 *  11 read   {}                   {brightness, boot, name, default, max}
 *  12 write  {brightness?, boot?, name?}  {}  any of them, see badge_settings.h
 *  13 read   {}                   {usb, mv?, percent?}  the battery, see battery.h; mv and
 *                                     percent only off USB, once there is a reading
 *
 * brightness is in percent, boot is the folder of the mode the badge starts
 * in, name is what follows "LED_BLE_" in its Bluetooth name, "" for default,
 * the name used then, and max the longest name. A new name is advertised
 * after the next disconnect.
 *
 * 6 and 7 take a full path such as "/lfs1/settings.cbor", for backups of the
 * whole file system. Names starting with "." (temporary files) are neither
 * listed nor accepted.
 *
 * An upload is: 2, stock file upload to UPLOAD_PATH, 3 or 7. A file in a media
 * folder is therefore always complete, even if the connection drops.
 *
 * A preview is never written to flash, see preview.h. It plays once its last
 * piece has arrived, until the next one, a live drawing or a button press.
*
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/fs/fs.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "media_files.h"
#include "preview.h"
#include "lua_anim.h"
#include "badge_settings.h"
#include "battery.h"

#define MEDIA_GROUP_ID	MGMT_GROUP_ID_PERUSER
#define MOUNT_POINT	"/lfs1"
#define UPLOAD_PATH	MOUNT_POINT "/.upload"
#define DIR_PATH_LEN	(sizeof(MOUNT_POINT "/gif") - 1)
#define LIST_PAGE	6

struct media_req {
	char path[CONFIG_MCUMGR_GRP_FS_PATH_LEN + 1];
	uint32_t skip;
	uint32_t size;
};

/* Too big for the stack. Handlers all run on the single SMP work queue. */
static struct fs_dirent entry;

static int to_mgmt_err(int rc)
{
	switch (rc) {
	case 0:
		return MGMT_ERR_EOK;
	case -ENOENT:
		return MGMT_ERR_ENOENT;
	case -ENOSPC:
		return MGMT_ERR_ENOMEM;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}

/* Builds the folder or file path, refusing anything outside the media folders. */
static int decode_req(struct smp_streamer *ctxt, struct media_req *req, bool with_name)
{
	struct zcbor_string dir = { 0 };
	struct zcbor_string name = { 0 };
	size_t decoded;

	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("dir", zcbor_tstr_decode, &dir),
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &name),
		ZCBOR_MAP_DECODE_KEY_DECODER("skip", zcbor_uint32_decode, &req->skip),
		ZCBOR_MAP_DECODE_KEY_DECODER("size", zcbor_uint32_decode, &req->size),
	};

	req->skip = 0;
	req->size = UINT32_MAX;

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    dir.len != 3 ||
	    (memcmp(dir.value, "gif", 3) && memcmp(dir.value, "png", 3) && memcmp(dir.value, "lua", 3))) {
		return MGMT_ERR_EINVAL;
	}

	memcpy(req->path, MOUNT_POINT "/", sizeof(MOUNT_POINT "/") - 1);
	memcpy(&req->path[DIR_PATH_LEN - 3], dir.value, 3);
	req->path[DIR_PATH_LEN] = '\0';

	if (!with_name) {
		return MGMT_ERR_EOK;
	}

	if (name.len == 0 || name.len > sizeof(req->path) - DIR_PATH_LEN - 2 ||
	    name.value[0] == '.' || memchr(name.value, '/', name.len) ||
	    memchr(name.value, '\0', name.len)) {
		return MGMT_ERR_EINVAL;
	}

	req->path[DIR_PATH_LEN] = '/';
	memcpy(&req->path[DIR_PATH_LEN + 1], name.value, name.len);
	req->path[DIR_PATH_LEN + 1 + name.len] = '\0';

	return MGMT_ERR_EOK;
}

/* {path, skip/size}: a path at or below the mount point, without hidden or empty parts */
static int decode_path_req(struct smp_streamer *ctxt, struct media_req *req, bool root_ok)
{
	const size_t mount_len = sizeof(MOUNT_POINT) - 1;
	struct zcbor_string path = { 0 };
	size_t decoded;

	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("path", zcbor_tstr_decode, &path),
		ZCBOR_MAP_DECODE_KEY_DECODER("skip", zcbor_uint32_decode, &req->skip),
		ZCBOR_MAP_DECODE_KEY_DECODER("size", zcbor_uint32_decode, &req->size),
	};

	req->skip = 0;
	req->size = UINT32_MAX;

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    path.len < mount_len || path.len >= sizeof(req->path) ||
	    memcmp(path.value, MOUNT_POINT, mount_len) || memchr(path.value, '\0', path.len)) {
		return MGMT_ERR_EINVAL;
	}

	memcpy(req->path, path.value, path.len);
	req->path[path.len] = '\0';

	if (path.len == mount_len) {
		return root_ok ? MGMT_ERR_EOK : MGMT_ERR_EINVAL;
	}
	if (req->path[mount_len] != '/') {
		return MGMT_ERR_EINVAL;
	}
	for (const char *s = &req->path[mount_len]; *s; s++) {
		if (*s == '/' && (s[1] == '\0' || s[1] == '/' || s[1] == '.')) {
			return MGMT_ERR_EINVAL;
		}
	}

	return MGMT_ERR_EOK;
}

static int media_info(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct fs_statvfs st;
	int rc = fs_statvfs(MOUNT_POINT, &st);
	bool ok;

	if (rc) {
		return to_mgmt_err(rc);
	}

	/* littlefs may count shared blocks twice, which would wrap f_bfree */
	if (st.f_bfree > st.f_blocks) {
		st.f_bfree = 0;
	}

	ok = zcbor_tstr_put_lit(zse, "block") && zcbor_uint32_put(zse, st.f_frsize) &&
	     zcbor_tstr_put_lit(zse, "blocks") && zcbor_uint32_put(zse, st.f_blocks) &&
	     zcbor_tstr_put_lit(zse, "free") && zcbor_uint32_put(zse, st.f_bfree);

	return MGMT_RETURN_CHECK(ok);
}

static int media_list(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct media_req req;
	struct fs_dir_t dir;
	uint32_t index = 0;
	bool more = false;
	bool ok;
	int rc = decode_req(ctxt, &req, false);

	if (rc) {
		return rc;
	}

	ok = zcbor_tstr_put_lit(zse, "files") && zcbor_list_start_encode(zse, 2 * LIST_PAGE);

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, req.path);
	if (rc == 0) {
		while (ok && (rc = fs_readdir(&dir, &entry)) == 0 && entry.name[0]) {
			if (entry.type != FS_DIR_ENTRY_FILE || index++ < req.skip) {
				continue;
			}
			if (index > req.skip + LIST_PAGE) {
				more = true;
				break;
			}
			ok = zcbor_tstr_put_term(zse, entry.name, sizeof(entry.name)) &&
			     zcbor_uint32_put(zse, entry.size);
		}
		(void)fs_closedir(&dir);
	}

	/* a folder that was never uploaded to is just empty */
	if (rc && rc != -ENOENT) {
		return to_mgmt_err(rc);
	}

	ok = ok && zcbor_list_end_encode(zse, 2 * LIST_PAGE);
	if (more) {
		ok = ok && zcbor_tstr_put_lit(zse, "next") &&
		     zcbor_uint32_put(zse, req.skip + LIST_PAGE);
	}

	return MGMT_RETURN_CHECK(ok);
}

static int media_prepare(struct smp_streamer *ctxt)
{
	struct media_req req;
	int rc = decode_req(ctxt, &req, false);

	if (rc) {
		return rc;
	}

	rc = fs_unlink(UPLOAD_PATH);
	if (rc && rc != -ENOENT) {
		return to_mgmt_err(rc);
	}

	rc = fs_stat(req.path, &entry);
	if (rc == -ENOENT) {
		rc = fs_mkdir(req.path);
	}

	return to_mgmt_err(rc);
}

static int media_commit(struct smp_streamer *ctxt)
{
	struct media_req req;
	int rc = decode_req(ctxt, &req, true);

	if (rc) {
		return rc;
	}

	rc = fs_stat(UPLOAD_PATH, &entry);
	if (rc) {
		return to_mgmt_err(rc);
	}

	/* the stock group ignores errors on close, which is where a full disk shows up */
	if (entry.size != req.size) {
		return MGMT_ERR_EBADSTATE;
	}

	return to_mgmt_err(fs_rename(UPLOAD_PATH, req.path));
}

static int media_walk(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct media_req req;
	struct fs_dir_t dir;
	uint32_t index = 0;
	bool more = false;
	bool ok;
	int rc = decode_path_req(ctxt, &req, true);

	if (rc) {
		return rc;
	}

	ok = zcbor_tstr_put_lit(zse, "entries") && zcbor_list_start_encode(zse, 3 * LIST_PAGE);

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, req.path);
	if (rc == 0) {
		while (ok && (rc = fs_readdir(&dir, &entry)) == 0 && entry.name[0]) {
			if (entry.name[0] == '.' || index++ < req.skip) {
				continue;
			}
			if (index > req.skip + LIST_PAGE) {
				more = true;
				break;
			}
			ok = zcbor_tstr_put_term(zse, entry.name, sizeof(entry.name)) &&
			     zcbor_bool_put(zse, entry.type == FS_DIR_ENTRY_DIR) &&
			     zcbor_uint32_put(zse, entry.size);
		}
		(void)fs_closedir(&dir);
	}

	if (rc) {
		return to_mgmt_err(rc);
	}

	ok = ok && zcbor_list_end_encode(zse, 3 * LIST_PAGE);
	if (more) {
		ok = ok && zcbor_tstr_put_lit(zse, "next") &&
		     zcbor_uint32_put(zse, req.skip + LIST_PAGE);
	}

	return MGMT_RETURN_CHECK(ok);
}

static int media_put(struct smp_streamer *ctxt)
{
	struct media_req req;
	int rc = decode_path_req(ctxt, &req, false);

	if (rc) {
		return rc;
	}

	rc = fs_stat(UPLOAD_PATH, &entry);
	if (rc) {
		return to_mgmt_err(rc);
	}
	if (entry.size != req.size) {
		return MGMT_ERR_EBADSTATE;
	}

	/* each folder on the way, the mount point itself always exists */
	for (char *s = &req.path[sizeof(MOUNT_POINT)]; (s = strchr(s, '/')) != NULL; s++) {
		*s = '\0';
		rc = fs_mkdir(req.path);
		*s = '/';
		if (rc && rc != -EEXIST) {
			return to_mgmt_err(rc);
		}
	}

	return to_mgmt_err(fs_rename(UPLOAD_PATH, req.path));
}

static int media_delete(struct smp_streamer *ctxt)
{
	struct media_req req;
	int rc = decode_req(ctxt, &req, true);

	return rc ? rc : to_mgmt_err(fs_unlink(req.path));
}

static int media_preview(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string data = { 0 };
	uint32_t off = UINT32_MAX;
	uint32_t len = UINT32_MAX;
	size_t decoded;
	int rc;

	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
	};

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded)) {
		return MGMT_ERR_EINVAL;
	}

	if (len == 0) {
		preview_end();
		return MGMT_ERR_EOK;
	}

	rc = preview_receive(off, len, data.value, data.len);
	if (rc < 0) {
		return rc == -EFBIG ? MGMT_ERR_ENOMEM : MGMT_ERR_EINVAL;
	}

	return MGMT_RETURN_CHECK(zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, rc));
}

static int media_run(struct smp_streamer *ctxt)
{
	struct media_req req;
	int rc = decode_req(ctxt, &req, true);

	if (rc) {
		return rc;
	}
	if (memcmp(&req.path[DIR_PATH_LEN - 3], "lua", 3)) {
		return MGMT_ERR_EINVAL;
	}
	return to_mgmt_err(display_run_script(&req.path[DIR_PATH_LEN + 1]));
}

static int media_script_status(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	char path[64];
	char error[160];
	bool running = lua_anim_status(path, sizeof(path), error, sizeof(error));
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "running") && zcbor_bool_put(zse, running) &&
	     zcbor_tstr_put_lit(zse, "path") && zcbor_tstr_put_term(zse, path, sizeof(path)) &&
	     zcbor_tstr_put_lit(zse, "error") && zcbor_tstr_put_term(zse, error, sizeof(error));

	return MGMT_RETURN_CHECK(ok);
}

static int media_format(struct smp_streamer *ctxt)
{
	ARG_UNUSED(ctxt);
	/*
	 * The restart would roll an update on trial back, and the old firmware
	 * would leave the request waiting for some later restart.
	 */
	if (!boot_is_img_confirmed()) {
		return MGMT_ERR_EBUSY;
	}
	storage_format_request();
	return MGMT_ERR_EOK;
}

static int media_settings(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct badge_settings s;
	char name[BADGE_NAME_MAX + 1];
	bool ok;

	badge_settings_get(&s);
	bluetooth_default_name(name, sizeof(name));
	ok = zcbor_tstr_put_lit(zse, "brightness") && zcbor_uint32_put(zse, s.brightness) &&
	     zcbor_tstr_put_lit(zse, "boot") && zcbor_tstr_put_term(zse, badge_boot_dir(s.boot), 4) &&
	     zcbor_tstr_put_lit(zse, "name") && zcbor_tstr_put_term(zse, s.name, sizeof(s.name)) &&
	     zcbor_tstr_put_lit(zse, "default") && zcbor_tstr_put_term(zse, name, sizeof(name)) &&
	     zcbor_tstr_put_lit(zse, "max") && zcbor_uint32_put(zse, BADGE_NAME_MAX);

	return MGMT_RETURN_CHECK(ok);
}

static int media_settings_set(struct smp_streamer *ctxt)
{
	struct zcbor_string boot = { 0 };
	struct zcbor_string name = { 0 };
	uint32_t brightness = 0;
	size_t decoded;

	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("brightness", zcbor_uint32_decode, &brightness),
		ZCBOR_MAP_DECODE_KEY_DECODER("boot", zcbor_tstr_decode, &boot),
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &name),
	};

	if (zcbor_map_decode_bulk(ctxt->reader->zs, fields, ARRAY_SIZE(fields), &decoded)) {
		return MGMT_ERR_EINVAL;
	}
	/* all checked before any is changed */
	if ((fields[0].found && brightness > 100) ||
	    (fields[1].found && badge_boot_from_dir(boot.value, boot.len) < 0) ||
	    (fields[2].found && !badge_name_valid(name.value, name.len))) {
		return MGMT_ERR_EINVAL;
	}
	if (fields[0].found) {
		badge_settings_set_brightness(brightness);
	}
	if (fields[1].found) {
		badge_settings_set_boot(boot.value, boot.len);
	}
	if (fields[2].found) {
		badge_settings_set_name(name.value, name.len);
	}

	return MGMT_ERR_EOK;
}

static int media_battery(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	struct battery_state battery;
	bool ok;

	battery_get(&battery);
	ok = zcbor_tstr_put_lit(zse, "usb") && zcbor_bool_put(zse, battery.usb);
	if (!battery.usb && battery.valid) {
		ok = ok && zcbor_tstr_put_lit(zse, "mv") && zcbor_uint32_put(zse, battery.mv) &&
		     zcbor_tstr_put_lit(zse, "percent") && zcbor_uint32_put(zse, battery.percent);
	}

	return MGMT_RETURN_CHECK(ok);
}

static const struct mgmt_handler media_handlers[] = {
	{ .mh_read = media_info },
	{ .mh_read = media_list },
	{ .mh_write = media_prepare },
	{ .mh_write = media_commit },
	{ .mh_write = media_delete },
	{ .mh_write = media_preview },
	{ .mh_read = media_walk },
	{ .mh_write = media_put },
	{ .mh_write = media_run },
	{ .mh_read = media_script_status },
	{ .mh_write = media_format },
	{ .mh_read = media_settings },
	{ .mh_write = media_settings_set },
	{ .mh_read = media_battery },
};

static struct mgmt_group media_group = {
	.mg_handlers = media_handlers,
	.mg_handlers_count = ARRAY_SIZE(media_handlers),
	.mg_group_id = MEDIA_GROUP_ID,
};

static void media_mgmt_register(void)
{
	mgmt_register_group(&media_group);
}

MCUMGR_HANDLER_DEFINE(media_mgmt, media_mgmt_register);
