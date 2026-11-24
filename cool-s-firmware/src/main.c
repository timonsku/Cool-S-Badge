/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <stdlib.h>
#include <math.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/poweroff.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

// #include <bluetooth/services/lbs.h>

#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

#include "lodepng.h"
#include "gifdec.h"
#include "update_approval.h"
#include "update_trial.h"
#include "media_files.h"
#include "preview.h"
#include "lua_anim.h"
#include "badge_settings.h"

#ifdef CONFIG_MCUMGR_GRP_STAT
#include <zephyr/mgmt/mcumgr/grp/stat_mgmt/stat_mgmt.h>
#endif

#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/dfu/mcuboot.h>

gd_GIF *gif;
/* the GIF plays from the preview buffer, not from a file */
static bool gif_is_preview;

uint8_t gif_frame[GD_MAX_PIXELS * 3];
uint8_t *gif_color;
int64_t gif_next_frame_at;

#define STORAGE_PARTITION_LABEL	storage_partition
#define STORAGE_PARTITION_ID	FIXED_PARTITION_ID(STORAGE_PARTITION_LABEL)

/* Define an example stats group; approximates seconds since boot. */
STATS_SECT_START(smp_svr_stats)
STATS_SECT_ENTRY(ticks)
STATS_SECT_ENTRY(version_major)
STATS_SECT_ENTRY(version_minor)
STATS_SECT_ENTRY(upload_approval)
STATS_SECT_END;

/* Assign a name to the `ticks` stat. */
STATS_NAME_START(smp_svr_stats)
STATS_NAME(smp_svr_stats, ticks)
STATS_NAME(smp_svr_stats, version_major)
STATS_NAME(smp_svr_stats, version_minor)
STATS_NAME(smp_svr_stats, upload_approval)
STATS_NAME_END(smp_svr_stats);

/* Define an instance of the stats group. */
STATS_SECT_DECL(smp_svr_stats) smp_svr_stats;


FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);
static struct fs_mount_t littlefs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &cstorage,
	.storage_dev = (void *)STORAGE_PARTITION_ID,
	.mnt_point = "/lfs1"
};

// GIFIMAGE gif;


static struct k_sem sync;

#define NAME_PREFIX_LEN (sizeof(BADGE_NAME_PREFIX) - 1)
/* what is advertised, and LED_BLE_COOL plus the end of the address unless the user named the badge */
static char device_name[NAME_PREFIX_LEN + BADGE_NAME_MAX + 1];
static char default_name[sizeof(device_name)];

const struct device *led_en = DEVICE_DT_GET(DT_NODELABEL(led_pwr));
#define STRIP_NODE		DT_ALIAS(led_strip)
#if DT_NODE_HAS_PROP(DT_ALIAS(led_strip), chain_length)
#define STRIP_NUM_PIXELS	DT_PROP(DT_ALIAS(led_strip), chain_length)
#else
#error Unable to determine length of LED strip
#endif
static struct led_rgb pixels_raw[STRIP_NUM_PIXELS];
static struct led_rgb pixels[STRIP_NUM_PIXELS];

// display state machine: entering a display mode goes through a LOADING_*
// state, where the main loop loads the corresponding file and then
// transitions into the display state
enum display_state {
	DISPLAY_LOADING_STATIC,
	DISPLAY_STATIC,
	DISPLAY_LOADING_GIF,
	DISPLAY_GIF,
	DISPLAY_LOADING_LUA,
	DISPLAY_LUA,
};

/* the kinds of files the badge shows, in the order the top buttons step through them */
enum media_kind {
	MEDIA_PNG,
	MEDIA_GIF,
	MEDIA_LUA,
	MEDIA_KINDS,
};

static const struct {
	const char *dir;
	const char *default_path;
	enum display_state loading;
} media_kinds[MEDIA_KINDS] = {
	[MEDIA_PNG] = {"png", DEFAULT_PNG_PATH, DISPLAY_LOADING_STATIC},
	[MEDIA_GIF] = {"gif", DEFAULT_GIF_PATH, DISPLAY_LOADING_GIF},
	[MEDIA_LUA] = {"lua", DEFAULT_LUA_PATH, DISPLAY_LOADING_LUA},
};
/* set from the Bluetooth and input threads, handled in the main loop */
static volatile enum display_state display_state = DISPLAY_LOADING_STATIC;
static bool storage_ok;
static volatile bool bluetooth_ok;
static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);


struct pixel_animation_frame {
	uint16_t duration_ms;
	struct led_rgb pixels[STRIP_NUM_PIXELS];
};

struct pixel_animation {
	uint16_t num_frames;
	struct pixel_animation_frame *frames;
};

struct pixel_animation current_animation;
bool gifLoaded = false;

LOG_MODULE_REGISTER(app);

// #define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
// #define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)


#define RUN_STATUS_LED          DK_LED1
#define CON_STATUS_LED          DK_LED2
#define RUN_LED_BLINK_INTERVAL  1000

#define USER_LED                DK_LED3

#define USER_BUTTON             DK_BTN1_MSK

/*
 * Buttons, as the input thread sees them. Top right and top left step
 * forwards and backwards through the modes (static image, GIF, Lua script),
 * bottom right and bottom left through the files of that kind. A press only
 * acts on release, and not at all if it was part of a combination or
 * answered the upload prompt.
 */
enum {
	BUTTON_TOP_RIGHT = 0,
	BUTTON_TOP_LEFT = 1,
	BUTTON_BOTTOM_RIGHT = 2,
	BUTTON_BOTTOM_LEFT = 3,
	BUTTON_COUNT = 4,
};
static volatile bool button_held[BUTTON_COUNT];
// a release without a press seen, like a button held through boot, does nothing
static bool button_spent[BUTTON_COUNT] = {true, true, true, true};

/* requests from the input and Bluetooth threads, handled by the main loop */
enum {
	REQUEST_NEXT_MODE = BIT(0),
	REQUEST_PREVIOUS_MODE = BIT(6),
	REQUEST_RUN_SCRIPT = BIT(7),
	REQUEST_NEXT = BIT(1),
	REQUEST_PREVIOUS = BIT(2),
	REQUEST_SAVE_DRAWING = BIT(3),
	REQUEST_PREVIEW_PLAY = BIT(4),
	REQUEST_PREVIEW_END = BIT(5),
	REQUEST_RENDER = BIT(8),
};
static atomic_t requests;

/* 0 shows the default file of the current mode, n the n-th file in its folder */
static volatile int selected;

#define SET_DEFAULT_HOLD_MS 3000
#define POWER_OFF_HOLD_MS 1000
static bool                   notify_enabled;
static struct k_work adv_work;
	
#define BT_UUID_IPIXEL_SRV_VAL BT_UUID_16_ENCODE(0x00fa)
	// BT_UUID_128_ENCODE(0x000000fa, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_WRITE_VAL BT_UUID_16_ENCODE(0xfa02)
	// BT_UUID_128_ENCODE(0x0000fa02, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_NOTIFY_VAL BT_UUID_16_ENCODE(0xfa03)
	// BT_UUID_128_ENCODE(0x0000fa03, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_SRV    BT_UUID_DECLARE_16(0x00fa)
#define BT_UUID_IPIXEL_WRITE    BT_UUID_DECLARE_16(0xfa02)
#define BT_UUID_IPIXEL_NOTIFY    BT_UUID_DECLARE_16(0xfa03)


/* the name's length is set by apply_name() */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, 0),
};

static const uint8_t led_scan_resp[] = {0x00, 0x72, 0x00, 0x01, 0x05, 0x01};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_MANUFACTURER_DATA, led_scan_resp,sizeof(led_scan_resp)),
};

struct __attribute__((packed)) image_header {
	uint16_t length;
	uint16_t data_type;
	uint8_t option;
};

int recovery_timeout = 0;
int shutdown_timeout = 0;

/*
 * Any button but the top right one wakes the badge. Waking is a reset, and
 * the top right button held during a reset starts USB recovery.
 */
static const struct gpio_dt_spec wakeup_buttons[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button2), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button3), gpios),
};

#define GAMMA_VAL 2.0
// #define DEVICE_TYPE 0x85 // 64x20
#define DEVICE_TYPE 0x82 // 32x16

void parseCommandPacket(const void *buf, uint16_t len);
void imageCommand(const void *buf, uint16_t len);
int process_gif(const char *path);
void live_draw(uint8_t pixel_index, struct led_rgb color);
struct led_rgb gamma_correction(struct led_rgb color);
struct led_rgb set_brightness(struct led_rgb color, float brightness);
void clear_display();
void display_debug();
void display_warn();
void display_upload_prompt();
void display_flash_white();
void render();
void check_for_recovery_mode();
void check_power_off();
void playback_animation(struct pixel_animation *animation);
void display_state_set(enum display_state state);
void display_state_process();
int save_gif_to_file(const char *filename, const uint8_t* gif_data, size_t gif_size);

static ssize_t iPixel_write(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags);

static ssize_t read_led(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf,
			 uint16_t len, uint16_t offset);

static void lbslc_ccc_cfg_changed(const struct bt_gatt_attr *attr,
				  uint16_t value);

BT_GATT_SERVICE_DEFINE(led_svc,
BT_GATT_PRIMARY_SERVICE(BT_UUID_IPIXEL_SRV),
	BT_GATT_CHARACTERISTIC(BT_UUID_IPIXEL_WRITE,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE |
			       BT_GATT_PERM_PREPARE_WRITE,
			       NULL, iPixel_write, NULL),
	BT_GATT_CUD("COOL-S", BT_GATT_PERM_READ),
	BT_GATT_CHARACTERISTIC(BT_UUID_IPIXEL_NOTIFY,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_led, NULL, NULL),
	BT_GATT_CCC(lbslc_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);


static void lbslc_ccc_cfg_changed(const struct bt_gatt_attr *attr,
				  uint16_t value)
{
	LOG_INF("cfg_change: %u", value);
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	if(notify_enabled) {
		LOG_INF("Notifications enabled");
			uint8_t info_notify2[] = {0x00, 0x05, 0x80, 0x0f, 0x15, 0x01, 0x05};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &info_notify2, sizeof(info_notify2));
			// bt_gatt_notify(NULL, &led_svc_ae.attrs[4], &info_notify2, sizeof(info_notify2));

			uint8_t info_notify[11] = {0x0b, 0x00, 0x01, 0x80, DEVICE_TYPE, 0x02, 0x2c, 0x00, 0x00, 0x01, 0x00};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &info_notify, sizeof(info_notify));
			// bt_gatt_notify(NULL, &led_svc_ae.attrs[4], &info_notify, sizeof(info_notify));
		
	}
}


static ssize_t iPixel_write(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	// LOG_INF("LED attribute write, handle: %u, conn: %p", attr->handle, (void *)conn);

	LOG_INF("\nNew packet of length: %u", len);
	
	if(len < 500){
		LOG_HEXDUMP_INF(buf, len, "data:");
	}
	LOG_INF("Offset: %u", offset);
	LOG_INF("Flags: %u", flags);

	if(len >= 4){
		if(((uint8_t *)buf)[3] == 0x80) {
			LOG_INF("Command packet received");
			parseCommandPacket(buf, len);
		} else {
			LOG_INF("Data packet received");
			imageCommand(buf, len);
		}
	}else {
		LOG_ERR("Write led: Incorrect data length: %u", len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	return len;
}


void parseCommandPacket(const void *buf, uint16_t len)
{
	LOG_INF("Parsing command packet...");
	const uint8_t *data = (const uint8_t *)buf;
	uint8_t commandType = data[0];
	switch(commandType) {
		case 0x08:
			LOG_INF("Device Info Request received");
			uint8_t info_notify[11] = {0x0b, 0x00, 0x01, 0x80, DEVICE_TYPE, 0x02, 0x2c, 0x00, 0x00, 0x01, 0x00};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &info_notify, sizeof(info_notify));
			break;
		case 0x07:
			LOG_INF("Image ack request received");
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x11, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes2, sizeof(bytes2));
			break;
		case 0x05:
			if(data[2] == 0x04) {
				LOG_INF("Brightness adjustment packet received: %u", data[4]);
				badge_settings_set_brightness(MIN(data[4], 100));
			}
			break;
		case 0x04:
			if(data[2] == 0x03) {
				LOG_INF("Clear display command received");
				// clear display
				clear_display();
			}
			break;
		default:
			LOG_WRN("Unknown command type: %#02x", commandType);
			break;
	}
}

void imageCommand(const void *buf, uint16_t len)
{
	LOG_INF("Parsing image command packet...");
	uint8_t *data = (const uint8_t *)buf;
	
	if(len <= 10){
		uint8_t commandType = data[0];
		switch(commandType) {
		case 0x05:
			if(data[2] == 0x04 && data[3] == 0x01) {
				if(data[4] == 0x01){
					LOG_INF("Live draw start packet received");
					clear_display();
					// live drawing renders directly, no file load needed
					selected = 0;
					display_state_set(DISPLAY_STATIC);
				} else if(data[4] == 0x00){
					LOG_INF("Live draw stop packet received");
					// respond with ack by sending packet back
					data[3] = 0x01;
					bt_gatt_notify(NULL, &led_svc.attrs[5], data, len);
					// saved by the main loop, which also loads the default files
					selected = 0;
					atomic_or(&requests, REQUEST_SAVE_DRAWING);
				}
			} else {
				LOG_WRN("Unknown Draw Command Packet received");
			}
			break;
		case 0x0a:
			LOG_INF("Live draw pixel data packet received");
			// live drawing renders directly, no file load needed
			display_state_set(DISPLAY_STATIC);
			if(len > 10){
				LOG_ERR("Invalid live draw pixel data packet length: %u", len);
				break;
			}
			if(data[9] != 0x00) {
				break;
			}
			uint8_t pixel_index = data[8];
			struct led_rgb color;
			color.r = data[5];
			color.g = data[6];
			color.b = data[7];
			live_draw(pixel_index, color);
			break;
		default:
			LOG_WRN("Unknown image command type: %#02x", commandType);
			break;
		}
	}else{
		LOG_INF("Bitmap data packet received, length: %u", len);
		/* 15 byte header plus the file signature. Shorter would wrap len - 15. */
		if(len < 15 + 4){
			LOG_WRN("Bitmap data packet too short");
			return;
		}
		struct image_header img_header;
		memcpy(&img_header, &data[0], sizeof(img_header));
		LOG_INF("Image length: %u, data type: %#04x, option: %#02x", img_header.length, img_header.data_type, img_header.option);

		// GIF
		if(img_header.data_type == 0x03 && data[15] == 0x47 && data[16] == 0x49 && data[17] == 0x46){
			LOG_INF("GIF header detected");
			// acknowledge receipt
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x3, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes2, sizeof(bytes2));

			LOG_HEXDUMP_DBG(data,len,"Raw GIF packet");
			if (save_gif_to_file(DEFAULT_GIF_PATH, &data[15], len - 15) == 0) {
				// only the main loop may open or close the decoder it is playing from
				selected = 0;
				display_state_set(DISPLAY_LOADING_GIF);
			}
		}
		
		// PNG
		if(data[15] == 0x89 && data[16] == 0x50 && data[17] == 0x4e && data[18] == 0x47){
			LOG_INF("PNG header detected");
			if (png_to_pixels(&data[15], len - 15, pixels_raw, STRIP_NUM_PIXELS) == 0) {
				render();
				selected = 0;
				display_state_set(DISPLAY_STATIC);
				// kept as sent, the badge decodes it again at boot
				file_write_replace(DEFAULT_PNG_PATH, MEDIA_ROOT "/.ipixel.tmp", &data[15], len - 15);
			}
			// acknowledge receipt
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x3, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[5], &bytes2, sizeof(bytes2));
		}
	}
	
}


void live_draw(uint8_t pixel_index, struct led_rgb color)
{

	if(pixel_index < STRIP_NUM_PIXELS){
		pixels_raw[pixel_index] = color;
		render();
	} else {
		LOG_ERR("live_draw: Pixel index out of bounds: %u", pixel_index);
	}
}

void clear_display()
{
	LOG_INF("Clearing display");
	memset(&pixels_raw, 0x00, sizeof(pixels_raw));
	render();
}

struct led_rgb gamma_correction(struct led_rgb color) {
	color.r = (uint8_t)(pow(color.r / 255.0, GAMMA_VAL) * 255);
	color.g = (uint8_t)(pow(color.g / 255.0, GAMMA_VAL) * 255);
	color.b = (uint8_t)(pow(color.b / 255.0, GAMMA_VAL) * 255);
	return color;
}

struct led_rgb set_brightness(struct led_rgb color, float brightness) {
	color.r = (uint8_t)(color.r * brightness);
	color.g = (uint8_t)(color.g * brightness);
	color.b = (uint8_t)(color.b * brightness);
	return color;
}

void render(){
	// nothing may paint over, or imitate, the upload prompt
	if(update_approval_waiting()){
		return;
	}
	// check if if all black and turn off power if so
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		if(pixels_raw[i].r != 0 || pixels_raw[i].g != 0 || pixels_raw[i].b != 0) {
			goto draw;
		}
	}
	LOG_INF("All pixels black, turning off power to LED strip");
	regulator_disable(led_en);
	return;

	draw:
	if(!regulator_is_enabled(led_en)) {
		regulator_enable(led_en);
		k_sleep(K_USEC(500));
	}
	float brightness = badge_settings_brightness() / 100.0f;
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels[i] = set_brightness(pixels_raw[i], brightness);
		pixels[i] = gamma_correction(pixels[i]);
	}
	int rc = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
	if (rc) {
		LOG_ERR("couldn't update strip: %d", rc);
	}
}

// on the main loop, which is the one that renders
void display_brightness_changed(void) {
	atomic_or(&requests, REQUEST_RENDER);
}

// used from the next time the badge advertises, which is after a disconnect
static void apply_name(void) {
	struct badge_settings s;
	badge_settings_get(&s);
	if (s.name[0]) {
		snprintk(device_name, sizeof(device_name), BADGE_NAME_PREFIX "%s", s.name);
	} else {
		strcpy(device_name, default_name);
	}
	ad[1].data_len = strlen(device_name);
	bt_set_name(device_name);
	// only succeeds while advertising
	(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	LOG_INF("Device name: %s", device_name);
}

void bluetooth_name_changed(void) {
	apply_name();
}

void bluetooth_default_name(char *out, size_t len) {
	strncpy(out, &default_name[NAME_PREFIX_LEN], len - 1);
	out[len - 1] = '\0';
}




// keeps the GIF that is playing if the new one does not open
int process_gif(const char *path) {
	LOG_INF("Opening GIF %s", path);
	gd_GIF *next = gd_open_gif(path);
	if(!next){
		LOG_ERR("GIF open error");
		return -EINVAL;
	}
	if(gif){
		gd_close_gif(gif);
	}
	gif = next;
	gif_is_preview = false;
	return 0;
}

void display_preview_play(void) {
	atomic_or(&requests, REQUEST_PREVIEW_PLAY);
}

void display_preview_end(void) {
	atomic_or(&requests, REQUEST_PREVIEW_END);
}

/* the file in the lua folder to run, for REQUEST_RUN_SCRIPT */
static atomic_t script_requested;

int display_run_script(const char *name) {
	char path[MEDIA_PATH_MAX];
	int count = media_count("lua");
	for (int i = 1; i <= count; i++) {
		if (media_path("lua", i, path, sizeof(path)) == 0 &&
		    strcmp(strrchr(path, '/') + 1, name) == 0) {
			atomic_set(&script_requested, i);
			atomic_or(&requests, REQUEST_RUN_SCRIPT);
			return 0;
		}
	}
	return -ENOENT;
}

// plays the GIF in the preview buffer, see preview.h
static void play_preview(void) {
	size_t len;
	const uint8_t *data = preview_data(&len);
	if (preview_receiving() || len == 0) {
		return;
	}
	gd_GIF *next = gd_open_gif_memory(data, len);
	if (!next) {
		LOG_WRN("The preview is not a GIF the badge can play");
		return;
	}
	if (gif) {
		gd_close_gif(gif);
	}
	gif = next;
	gif_is_preview = true;
	gif_next_frame_at = 0;
	display_state = DISPLAY_GIF;
}

// the last frame stays up; the buttons go back to the files
static void end_preview(void) {
	if (gif_is_preview && display_state == DISPLAY_GIF) {
		display_state = DISPLAY_STATIC;
	}
}

void playback_animation(struct pixel_animation *animation) {

	int ret;
	if(!gif || display_state != DISPLAY_GIF || k_uptime_get() < gif_next_frame_at){
		return;
	}
	// a new preview is being written over this one
	if(gif_is_preview && preview_receiving()){
		return;
	}
	ret = gd_get_frame(gif);
	if (ret == -1){return;}
	if (ret == 0){
		// reached the GIF end: loop back to the first frame
		gd_rewind(gif);
		ret = gd_get_frame(gif);
		if (ret <= 0){return;}
	}
	gd_render_frame(gif, gif_frame);
	// only the first row of the frame is displayed on the LED strip;
	// for GIFs with a global color table, pixels matching the background
	// color are blanked (bgindex is meaningless without a GCT)
	uint8_t *bgcolor = gif->gct.size ? &gif->gct.colors[gif->bgindex * 3] : NULL;
	for (int j = 0; j < gif->width && j < STRIP_NUM_PIXELS; j++) {
		uint8_t *px = &gif_frame[j * 3];
		if (bgcolor && !memcmp(bgcolor, px, 3)) {
			pixels_raw[j].r = 0;
			pixels_raw[j].g = 0;
			pixels_raw[j].b = 0;
		} else {
			pixels_raw[j].r = px[0];
			pixels_raw[j].g = px[1];
			pixels_raw[j].b = px[2];
		}
	}
	render();
	// GIF delay is in centiseconds. Don't sleep through it: it can be 11 minutes,
	// and the main loop also runs the buttons and the upload prompt.
	gif_next_frame_at = k_uptime_get() + gif->gce.delay * 10;
}

void display_state_set(enum display_state state) {
	display_state = state;
}

static enum media_kind current_kind(void) {
	switch (display_state) {
	case DISPLAY_LOADING_GIF:
	case DISPLAY_GIF:
		return MEDIA_GIF;
	case DISPLAY_LOADING_LUA:
	case DISPLAY_LUA:
		return MEDIA_LUA;
	default:
		return MEDIA_PNG;
	}
}

static bool file_exists(const char *path) {
	struct fs_dirent info;
	return fs_stat(path, &info) == 0 && info.type == FS_DIR_ENTRY_FILE;
}

// index 0 is the default file, n the n-th file in the folder
static int selection_path(enum media_kind kind, int index, char *out, size_t len) {
	if (index == 0) {
		strncpy(out, media_kinds[kind].default_path, len - 1);
		out[len - 1] = 0;
		return 0;
	}
	return media_path(media_kinds[kind].dir, index, out, len);
}

static int run_script(const char *path) {
	if (!file_exists(path)) {
		return -ENOENT;
	}
	return lua_anim_start(path);
}

static int show_static(const char *path) {
	LOG_INF("Showing %s", path);
	int rc = png_file_to_pixels(path, pixels_raw, STRIP_NUM_PIXELS);
	if (rc == 0) {
		render();
	}
	return rc;
}

void display_state_process() {
	char path[MEDIA_PATH_MAX];
	int index = selected;
	int rc;

	switch (display_state) {
	case DISPLAY_LOADING_STATIC:
		// change state first, so that a request arriving while loading is not lost
		display_state = DISPLAY_STATIC;
		rc = selection_path(MEDIA_PNG, index, path, sizeof(path));
		if (rc == 0) {
			rc = show_static(path);
		}
		if (rc != 0 && index == 0 && media_path("png", 1, path, sizeof(path)) == 0) {
			// no default image yet: the first one in the folder
			rc = show_static(path);
			if (rc == 0) {
				selected = 1;
			}
		}
		if (rc != 0) {
			LOG_WRN("Cannot show image %d: %d", index, rc);
			// a broken file in the folder leaves the previous picture up
			if (index == 0) {
				clear_display();
			}
		}
		break;
	case DISPLAY_LOADING_GIF:
		display_state = DISPLAY_GIF;
		rc = selection_path(MEDIA_GIF, index, path, sizeof(path));
		if (rc == 0) {
			rc = process_gif(path);
		}
		if (rc != 0 && index == 0 && media_path("gif", 1, path, sizeof(path)) == 0) {
			rc = process_gif(path);
			if (rc == 0) {
				selected = 1;
			}
		}
		if (rc == 0) {
			gd_rewind(gif);
			gif_next_frame_at = 0;
		} else if (index != 0 && gif) {
			// a broken file in the folder leaves the previous GIF playing
			LOG_WRN("Cannot play GIF %d: %d", index, rc);
		} else {
			LOG_WRN("No GIF available, falling back to static image");
			if (display_state == DISPLAY_GIF) {
				selected = 0;
				display_state = DISPLAY_LOADING_STATIC;
			}
		}
		break;
	case DISPLAY_LOADING_LUA:
		display_state = DISPLAY_LUA;
		rc = selection_path(MEDIA_LUA, index, path, sizeof(path));
		if (rc == 0) {
			rc = run_script(path);
		}
		if (rc != 0 && index == 0 && media_path("lua", 1, path, sizeof(path)) == 0) {
			rc = run_script(path);
			if (rc == 0) {
				selected = 1;
			}
		}
		if (rc != 0 && index != 0) {
			// the script shows that it failed, see lua_anim.c
			LOG_WRN("Cannot run script %d: %d", index, rc);
		} else if (rc != 0) {
			LOG_WRN("No Lua script available, falling back to static image");
			if (display_state == DISPLAY_LUA) {
				selected = 0;
				display_state = DISPLAY_LOADING_STATIC;
			}
		}
		break;
	default:
		break;
	}
}

// whether there is anything to show in a mode; there is always the static image
static bool kind_available(enum media_kind kind) {
	return kind == MEDIA_PNG || file_exists(media_kinds[kind].default_path) ||
	       media_count(media_kinds[kind].dir) > 0;
}

// the next or previous mode that has something to show
static void step_mode(int delta) {
	enum media_kind kind = current_kind();
	for (int step = 1; step < MEDIA_KINDS; step++) {
		enum media_kind next = (kind + delta * step + MEDIA_KINDS * step) % MEDIA_KINDS;
		if (kind_available(next)) {
			LOG_INF("Showing %s files", media_kinds[next].dir);
			selected = 0;
			display_state_set(media_kinds[next].loading);
			return;
		}
	}
	LOG_INF("Nothing else to show");
}

// next or previous file of the current kind, the default file first
static void step_selection(int delta) {
	enum media_kind kind = current_kind();
	int count = media_count(media_kinds[kind].dir);
	if (count < 0) {
		count = 0;
	}
	int first = file_exists(media_kinds[kind].default_path) ? 0 : 1;
	int span = count + 1 - first;
	if (span <= 1) {
		LOG_INF("Nothing else to show");
		return;
	}
	int index = selected;
	if (index < first || index > count) {
		// files were removed since
		index = first;
	}
	index = first + ((index - first + delta) % span + span) % span;
	LOG_INF("Selecting %s file %d of %d", media_kinds[kind].dir, index, count);
	selected = index;
	display_state_set(media_kinds[kind].loading);
}

static void set_selection_as_default(void) {
	enum media_kind kind = current_kind();
	char path[MEDIA_PATH_MAX];
	int index = selected;
	if (index == 0) {
		LOG_INF("The default is already showing");
		return;
	}
	if (media_path(media_kinds[kind].dir, index, path, sizeof(path)) != 0) {
		return;
	}
	int rc = file_copy_replace(path, media_kinds[kind].default_path, MEDIA_ROOT "/.default.tmp");
	if (rc != 0) {
		LOG_ERR("Could not make %s the default: %d", path, rc);
		return;
	}
	LOG_INF("%s is the new default", path);
	display_flash_white();
}

static ssize_t read_led(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf,
			  uint16_t len,
			  uint16_t offset)
{
	const char *value = attr->user_data;

	LOG_INF("Attribute read, handle: %u, conn: %p", attr->handle,
		(void *)conn);

	return 0;
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_INF("Advertising failed to start (err %d)\n", err);
		return;
	}

	bluetooth_ok = true;
	LOG_INF("Advertising successfully started\n");
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_INF("Connection failed, err 0x%02x %s\n", err, bt_hci_err_to_str(err));
		return;
	}
	bluetooth_ok = true;
	LOG_INF("Connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s\n", reason, bt_hci_err_to_str(reason));
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	LOG_INF("Connection parameters: interval %u.%02u ms, latency %u, timeout %u ms",
		interval * 5 / 4, (interval * 125) % 100, latency, timeout * 10);
}

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("ATT MTU: tx %u, rx %u", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = att_mtu_updated,
};

// Shows that SMP requests reach the badge. Only the OS group, which every tool starts with,
// so file and image uploads do not flood the log.
static enum mgmt_cb_return on_smp_command(uint32_t event, enum mgmt_cb_return prev_status,
					  int32_t *rc, uint16_t *group, bool *abort_more,
					  void *data, size_t data_size)
{
	const struct mgmt_evt_op_cmd_arg *cmd = data;

	if (cmd->group == MGMT_GROUP_ID_OS) {
		LOG_INF("SMP request: group %u, id %u, op %u", cmd->group, cmd->id, cmd->op);
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback smp_command_callback = {
	.callback = on_smp_command,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

static void recycled_cb(void)
{
	LOG_INF("Connection object available from previous conn. Disconnect is complete!\n");
	advertising_start();
}


BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
	.le_param_updated = le_param_updated,
#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};


static int button_index(uint16_t code)
{
	switch(code) {
		case INPUT_KEY_0: return 0;
		case INPUT_KEY_1: return 1;
		case INPUT_KEY_2: return 2;
		case INPUT_KEY_3: return 3;
		default: return -1;
	}
}

static void input_event_callback(struct input_event *evt, void *user_data)
{
	int index = button_index(evt->code);
	if (index < 0) {
		LOG_WRN("Unknown button event code: %u", evt->code);
		goto sync;
	}
	// presses that answer the upload prompt must not also act on the display
	bool consumed = update_approval_button(index, evt->value);
	LOG_INF("Button %d %s", index, evt->value ? "pressed" : "released");

	if (evt->value) {
		button_spent[index] = consumed;
		// a combination is not also a single press
		for (int i = 0; i < BUTTON_COUNT; i++) {
			if (i != index && button_held[i]) {
				button_spent[i] = true;
				button_spent[index] = true;
			}
		}
		button_held[index] = true;
	} else {
		button_held[index] = false;
		if (!button_spent[index] && !consumed) {
			switch (index) {
			case BUTTON_TOP_RIGHT:
				atomic_or(&requests, REQUEST_NEXT_MODE);
				break;
			case BUTTON_TOP_LEFT:
				atomic_or(&requests, REQUEST_PREVIOUS_MODE);
				break;
			case BUTTON_BOTTOM_RIGHT:
				atomic_or(&requests, REQUEST_NEXT);
				break;
			case BUTTON_BOTTOM_LEFT:
				atomic_or(&requests, REQUEST_PREVIOUS);
				break;
			}
		}
		button_spent[index] = true;
	}

sync:
	
	if (evt->sync) {
		LOG_INF("Input event sync");
		k_sem_give(&sync);
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_event_callback, NULL);


void display_debug()
{
	// all white
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels_raw[i] = (struct led_rgb){.r = 255, .g = 255, .b = 255};
	}
	render();
}

void display_upload_prompt()
{
	const uint32_t period_ms = 2000;
	const uint32_t min_level = 3;
	const uint32_t max_level = 48;
	uint32_t t = k_uptime_get_32() % period_ms;
	uint32_t ramp = (t < period_ms / 2) ? t : period_ms - t; // 0..1000
	uint32_t level = min_level + (max_level - min_level) * (ramp * ramp / 1000) / 1000;

	if(!regulator_is_enabled(led_en)) {
		regulator_enable(led_en);
		k_sleep(K_USEC(500));
	}
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels[i] = (struct led_rgb){.r = level, .g = level * 2 / 3, .b = 0};
	}
	int rc = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
	if (rc) {
		LOG_ERR("couldn't update strip: %d", rc);
	}
}

void display_flash_white()
{
	// confirms a button action; fixed level, as the brightness setting can be zero
	if(!regulator_is_enabled(led_en)) {
		regulator_enable(led_en);
		k_sleep(K_USEC(500));
	}
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels[i] = (struct led_rgb){.r = 96, .g = 96, .b = 96};
	}
	led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
	k_sleep(K_MSEC(200));
	render();
}

void display_warn()
{
	// all red
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels_raw[i] = (struct led_rgb){.r = 255, .g = 0, .b = 0};
	}
	render();
	k_sleep(K_MSEC(250));
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels_raw[i] = (struct led_rgb){.r = 0, .g = 0, .b = 255};
	}
	render();
	k_sleep(K_MSEC(250));
}

void check_for_recovery_mode()
{
	if(recovery_timeout != 0 && k_uptime_get() > recovery_timeout) {
		LOG_INF("Rebooting into MCUboot serial recovery mode now...");
		recovery_timeout = 0;
		lua_anim_stop();
		badge_settings_flush();
		int rc = fs_unmount(&littlefs_mnt);
		if (rc < 0) {
			LOG_ERR("Error unmounting littlefs [%d]", rc);
		}
		k_yield();
		sys_reboot(SYS_REBOOT_WARM);
	}
	if(button_held[BUTTON_TOP_RIGHT] && button_held[BUTTON_TOP_LEFT] &&
	   button_held[BUTTON_BOTTOM_RIGHT] && button_held[BUTTON_BOTTOM_LEFT]){
		if(recovery_timeout == 0){
			recovery_timeout = k_uptime_get() + 10000;
		}
		display_warn();
		LOG_INF("All buttons pressed, entering MCUboot serial recovery in %d seconds...", (int)((recovery_timeout - k_uptime_get()) / 1000));
	}else{
		if(recovery_timeout != 0){
			LOG_INF("Button combo released, cancelling MCUboot serial recovery.");
		}
		recovery_timeout = 0;
	}
}

int save_gif_to_file(const char *filename, const uint8_t* gif_data, size_t gif_size)
{
	int rc = file_write_replace(filename, MEDIA_ROOT "/.ipixel.tmp", gif_data, gif_size);
	if (rc == 0) {
		LOG_INF("Saved GIF data to file %s", filename);
	}
	return rc;
}

static void power_off_now(void)
{
	LOG_INF("Shutting down now...");
	for (int i = 0; i < ARRAY_SIZE(wakeup_buttons); i++) {
		int rc = gpio_pin_configure_dt(&wakeup_buttons[i], GPIO_INPUT);
		if (rc == 0) {
			rc = gpio_pin_interrupt_configure_dt(&wakeup_buttons[i], GPIO_INT_LEVEL_ACTIVE);
		}
		if (rc < 0) {
			LOG_ERR("Could not configure wakeup button %d (%d)", i, rc);
		}
	}
	lua_anim_stop();
	badge_settings_flush();
	int rc = fs_unmount(&littlefs_mnt);
	if (rc < 0) {
		LOG_ERR("Error unmounting littlefs [%d]", rc);
	}
	k_yield();
	sys_poweroff();
}

// hold the top two buttons, then let go
void check_power_off(){
	static int64_t hold_until;
	bool combo = button_held[BUTTON_TOP_RIGHT] && button_held[BUTTON_TOP_LEFT] &&
		     !button_held[BUTTON_BOTTOM_RIGHT] && !button_held[BUTTON_BOTTOM_LEFT];
	if (!combo || update_approval_waiting()) {
		hold_until = 0;
		return;
	}
	if (hold_until == 0) {
		hold_until = k_uptime_get() + POWER_OFF_HOLD_MS;
		return;
	}
	if (k_uptime_get() < hold_until) {
		return;
	}
	// dark at once, off once released: a held button would wake the badge right away
	LOG_INF("Turning off once the buttons are released");
	regulator_disable(led_en);
	while (button_held[BUTTON_TOP_RIGHT] || button_held[BUTTON_TOP_LEFT]) {
		k_sleep(K_MSEC(20));
	}
	k_sleep(K_MSEC(50));
	power_off_now();
}

// hold the bottom two buttons to make the file that is showing the default
void check_set_default(){
	static int64_t hold_until;
	static bool done;
	bool combo = button_held[BUTTON_BOTTOM_RIGHT] && button_held[BUTTON_BOTTOM_LEFT] &&
		     !button_held[BUTTON_TOP_RIGHT] && !button_held[BUTTON_TOP_LEFT];
	if (!combo || update_approval_waiting()) {
		hold_until = 0;
		done = false;
		return;
	}
	if (done) {
		return;
	}
	if (hold_until == 0) {
		hold_until = k_uptime_get() + SET_DEFAULT_HOLD_MS;
	} else if (k_uptime_get() >= hold_until) {
		done = true;
		set_selection_as_default();
	}
}

// firmware up to v0.6 kept the static picture as raw pixels in default.dat
static void migrate_legacy_default(void)
{
	struct fs_dirent info;
	struct fs_file_t file;
	uint8_t raw[STRIP_NUM_PIXELS * 3];
	struct led_rgb legacy[STRIP_NUM_PIXELS];

	if (fs_stat(DEFAULT_PNG_PATH, &info) == 0 || fs_stat(LEGACY_DAT_PATH, &info) != 0) {
		return;
	}
	memset(raw, 0, sizeof(raw));
	fs_file_t_init(&file);
	if (fs_open(&file, LEGACY_DAT_PATH, FS_O_READ) < 0) {
		return;
	}
	ssize_t got = fs_read(&file, raw, sizeof(raw));
	fs_close(&file);
	if (got < 0) {
		return;
	}
	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		legacy[i] = (struct led_rgb){.r = raw[i * 3], .g = raw[i * 3 + 1], .b = raw[i * 3 + 2]};
	}
	if (pixels_to_png_file(legacy, STRIP_NUM_PIXELS, DEFAULT_PNG_PATH, MEDIA_ROOT "/.migrate.tmp") == 0) {
		LOG_INF("Converted default.dat to default.png");
		fs_unlink(LEGACY_DAT_PATH);
	}
}

static void handle_requests(void)
{
	atomic_val_t r = atomic_clear(&requests);
	if (r & REQUEST_RENDER) {
		render();
	}
	if (r & REQUEST_PREVIEW_PLAY) {
		play_preview();
	}
	if (r & REQUEST_PREVIEW_END) {
		end_preview();
	}
	if (r & REQUEST_SAVE_DRAWING) {
		pixels_to_png_file(pixels_raw, STRIP_NUM_PIXELS, DEFAULT_PNG_PATH, MEDIA_ROOT "/.draw.tmp");
	}
	if (r & REQUEST_RUN_SCRIPT) {
		// started again even if it already runs: the web UI uploaded a new version
		selected = atomic_get(&script_requested);
		display_state_set(DISPLAY_LOADING_LUA);
	}
	if (r & REQUEST_NEXT_MODE) {
		step_mode(1);
	}
	if (r & REQUEST_PREVIOUS_MODE) {
		step_mode(-1);
	}
	if (r & REQUEST_NEXT) {
		step_selection(1);
	}
	if (r & REQUEST_PREVIOUS) {
		step_selection(-1);
	}
}

int main(void)
{
	int err;
	int rc;
	regulator_enable(led_en);
	LOG_INF("build time: " __DATE__ " " __TIME__);

	k_sem_init(&sync, 0, 1);

	rc = STATS_INIT_AND_REG(smp_svr_stats, STATS_SIZE_32, "smp_svr_stats");
	if (rc < 0) {
		LOG_ERR("Error initializing stats system [%d]", rc);
	}

	// a blank partition is formatted by the mount
	storage_format_if_requested(STORAGE_PARTITION_ID);
	rc = fs_mount(&littlefs_mnt);
	if (rc < 0) {
		LOG_ERR("Error mounting littlefs [%d]", rc);
	}
	storage_ok = rc == 0;
	if (storage_ok) {
		migrate_legacy_default();
	}

	if (storage_ok) {
		badge_settings_load();
	}
	// at power on and on waking, the badge shows the mode the settings name
	{
		struct badge_settings s;
		badge_settings_get(&s);
		display_state_set(media_kinds[s.boot < MEDIA_KINDS ? s.boot : MEDIA_PNG].loading);
	}

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s is not ready", strip->name);
		update_trial_failed();
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_INF("Bluetooth init failed (err %d)\n", err);
		update_trial_failed();
		return 0;
	}
	bt_gatt_cb_register(&gatt_callbacks);
	mgmt_callback_register(&smp_command_callback);

	LOG_INF("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	
	
	char addr_s[BT_ADDR_LE_STR_LEN];
	bt_addr_le_t addr = {0};
	size_t count = 1;
	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_s, sizeof(addr_s));
	LOG_INF("Bluetooth Address: %s", addr_s);
	// the last four bytes of the address
	snprintk(default_name, sizeof(default_name), BADGE_NAME_PREFIX "COOL%.2s%.2s%.2s%.2s",
		 &addr_s[6], &addr_s[9], &addr_s[12], &addr_s[15]);
	apply_name();

	STATS_SET(smp_svr_stats, version_major, APP_VERSION_MAJOR);
	STATS_SET(smp_svr_stats, version_minor, APP_VERSION_MINOR);
	// tells update tools that uploads wait for button presses
	STATS_SET(smp_svr_stats, upload_approval, 1);


	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	
	bool prompting = false;

	for (;;) {

		// STATS_INC(smp_svr_stats, ticks);
		if(update_approval_waiting()) {
			prompting = true;
			display_upload_prompt();
			k_sleep(K_MSEC(20));
		}else{
			if(prompting) {
				prompting = false;
				render();
			}
			handle_requests();
			display_state_process();
			if(current_kind() != MEDIA_LUA) {
				// left Lua mode, through a button, an upload, live drawing or a preview
				lua_anim_stop();
			}
			if(display_state == DISPLAY_GIF) {
				playback_animation(&current_animation);
				// until the next frame, but short enough to stay responsive
				k_sleep(K_MSEC(CLAMP(gif_next_frame_at - k_uptime_get(), 1, 20)));
			}else if(display_state == DISPLAY_LUA) {
				if(lua_anim_frame(pixels_raw, STRIP_NUM_PIXELS)) {
					render();
				}
				k_sleep(K_MSEC(5));
			}else{
				k_sleep(K_MSEC(50));
			}
		}
		k_sem_take(&sync, K_MSEC(1));
		check_for_recovery_mode();
		check_power_off();
		check_set_default();
		update_trial_poll(storage_ok && bluetooth_ok);
		
	}
}

