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

#ifdef CONFIG_MCUMGR_GRP_STAT
#include <zephyr/mgmt/mcumgr/grp/stat_mgmt/stat_mgmt.h>
#endif

#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/dfu/mcuboot.h>

#define STORAGE_PARTITION_LABEL	storage_partition
#define STORAGE_PARTITION_ID	FIXED_PARTITION_ID(STORAGE_PARTITION_LABEL)

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 4

/* Define an example stats group; approximates seconds since boot. */
STATS_SECT_START(smp_svr_stats)
STATS_SECT_ENTRY(ticks)
STATS_SECT_ENTRY(version_major)
STATS_SECT_ENTRY(version_minor)
STATS_SECT_END;

/* Assign a name to the `ticks` stat. */
STATS_NAME_START(smp_svr_stats)
STATS_NAME(smp_svr_stats, ticks)
STATS_NAME(smp_svr_stats, version_major)
STATS_NAME(smp_svr_stats, version_minor)
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


// #define SPNG_USE_MINIZ
// #include "spng.h"
static struct k_sem sync;

static uint8_t device_name[] = {'L', 'E', 'D', '_', 'B', 'L','E','_','C', 'O', 'O', 'L','0','0','0','0'};

const struct device *led_en = DEVICE_DT_GET(DT_NODELABEL(led_pwr));
#define STRIP_NODE		DT_ALIAS(led_strip)
#if DT_NODE_HAS_PROP(DT_ALIAS(led_strip), chain_length)
#define STRIP_NUM_PIXELS	DT_PROP(DT_ALIAS(led_strip), chain_length)
#else
#error Unable to determine length of LED strip
#endif
static struct led_rgb pixels_raw[STRIP_NUM_PIXELS];
static struct led_rgb pixels[STRIP_NUM_PIXELS];
static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#define GIF_IMPLEMENTATION
#include "gif.h"
#define SCRATCH_BUFFER_SIZE GIF_SCRATCH_BUFFER_REQUIRED_SIZE
uint8_t scratch_buffer[SCRATCH_BUFFER_SIZE]; 

struct pixel_animation_frame {
	uint16_t duration_ms;
	struct led_rgb pixels[STRIP_NUM_PIXELS];
};

struct pixel_animation {
	uint16_t num_frames;
	struct pixel_animation_frame *frames;
};

struct pixel_animation current_animation;
bool playAnimation = false;

LOG_MODULE_REGISTER(app);

// #define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
// #define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)


#define RUN_STATUS_LED          DK_LED1
#define CON_STATUS_LED          DK_LED2
#define RUN_LED_BLINK_INTERVAL  1000

#define USER_LED                DK_LED3

#define USER_BUTTON             DK_BTN1_MSK

static bool button0 = false;
static bool button1 = false;
static bool button2 = false;
static bool button3 = false;
static bool                   notify_enabled;
static struct k_work adv_work;

#define BT_UUID_IPIXEL_SRV_VAL \
	BT_UUID_128_ENCODE(0x000000fa, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)
	// BT_UUID_128_ENCODE(0x0000fee0, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_WRITE_VAL \
	BT_UUID_128_ENCODE(0x0000fa02, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_NOTIFY_VAL \
	BT_UUID_128_ENCODE(0x0000fa03, 0x0000, 0x1000, 0x8000, 0x00805f9b34fb)

#define BT_UUID_IPIXEL_SRV    BT_UUID_DECLARE_128(BT_UUID_IPIXEL_SRV_VAL)
#define BT_UUID_IPIXEL_WRITE    BT_UUID_DECLARE_128(BT_UUID_IPIXEL_WRITE_VAL)
#define BT_UUID_IPIXEL_NOTIFY    BT_UUID_DECLARE_128(BT_UUID_IPIXEL_NOTIFY_VAL)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name)),
	
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
float global_brightness = 0.3f;
static const struct gpio_dt_spec wakeup_button = GPIO_DT_SPEC_GET(DT_ALIAS(wakeup), gpios);

#define GAMMA_VAL 2.0
// #define DEVICE_TYPE 0x85 // 64x20
#define DEVICE_TYPE 0x82 // 32x16

void parseCommandPacket(const void *buf, uint16_t len);
void imageCommand(const void *buf, uint16_t len);
void process_gif(const uint8_t* gif_data, size_t gif_size);
void process_png(const uint8_t* png_data, size_t png_size);
void live_draw(uint8_t pixel_index, struct led_rgb color);
struct led_rgb gamma_correction(struct led_rgb color);
struct led_rgb set_brightness(struct led_rgb color, float brightness);
void set_global_brightness(float brightness);
void clear_display();
void display_debug();
void display_warn();
void render();
void check_for_recovery_mode();
void check_power_off();
void playback_animation(struct pixel_animation *animation);
int save_pixels_to_file(const char *filename);

static ssize_t write_led(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags);
static void lbslc_ccc_cfg_changed(const struct bt_gatt_attr *attr,
				  uint16_t value);

BT_GATT_SERVICE_DEFINE(led_svc,
BT_GATT_PRIMARY_SERVICE(BT_UUID_IPIXEL_SRV),
	BT_GATT_CHARACTERISTIC(BT_UUID_IPIXEL_WRITE,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE |
			       BT_GATT_PERM_PREPARE_WRITE,
			       NULL, write_led, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_IPIXEL_NOTIFY,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       NULL, NULL, NULL),
	BT_GATT_CCC(lbslc_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);



static void lbslc_ccc_cfg_changed(const struct bt_gatt_attr *attr,
				  uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	if(notify_enabled) {
		LOG_INF("Notifications enabled");
	}
}


static ssize_t write_led(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	// LOG_INF("LED attribute write, handle: %u, conn: %p", attr->handle, (void *)conn);

	LOG_INF("\nNew packet of length: %u", len);
	
	if(len < 250){
		LOG_HEXDUMP_INF(buf, len, "data:");
	}
	// LOG_INF("Offset: %u", offset);
	// LOG_INF("Flags: %u", flags);

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
			bt_gatt_notify(NULL, &led_svc.attrs[4], &info_notify, sizeof(info_notify));
			break;
		case 0x07:
			LOG_INF("Image ack request received");
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x11, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes2, sizeof(bytes2));
			break;
		case 0x05:
			if(data[2] == 0x04) {
				LOG_INF("Brightness adjustment packet received: %f / %u", (double)(data[4] / 100.0f), data[4]);
				float brightness = data[4] / 100.0f;
				set_global_brightness(brightness);
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
				} else if(data[4] == 0x00){
					LOG_INF("Live draw stop packet received");
					// respond with ack by sending packet back
					data[3] = 0x01;
					bt_gatt_notify(NULL, &led_svc.attrs[4], data, len);
					save_pixels_to_file("/lfs1/default.dat");
				}
			} else {
				LOG_WRN("Unknown Draw Command Packet received");
			}
			break;
		case 0x0a:
			LOG_INF("Live draw pixel data packet received");
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
		struct image_header img_header;
		memcpy(&img_header, &data[0], sizeof(img_header));
		LOG_INF("Image length: %u, data type: %#04x, option: %#02x", img_header.length, img_header.data_type, img_header.option);

		// GIF
		if(img_header.data_type == 0x03 && data[15] == 0x47 && data[16] == 0x49 && data[17] == 0x46){
			LOG_INF("GIF header detected");
			process_gif(&data[15], len - 15);
			// acknowledge receipt
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x3, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes2, sizeof(bytes2));
			playAnimation = true;
		}
		
		// PNG
		if(data[15] == 0x89 && data[16] == 0x50 && data[17] == 0x4e && data[18] == 0x47){
			LOG_INF("PNG header detected");
			process_png(&data[15], len - 15);
			// acknowledge receipt
			unsigned char bytes[] = {0x5, 0x0, 0x8, 0x80, 0x1};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes, sizeof(bytes));
			unsigned char bytes2[] = {0x5, 0x0, 0x3, 0x0, 0x3};
			bt_gatt_notify(NULL, &led_svc.attrs[4], &bytes2, sizeof(bytes2));
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
	for(int i = 0; i < STRIP_NUM_PIXELS; i++) {
		pixels[i] = set_brightness(pixels_raw[i], global_brightness);
		pixels[i] = gamma_correction(pixels[i]);
	}
	int rc = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
	if (rc) {
		LOG_ERR("couldn't update strip: %d", rc);
	}
}

void set_global_brightness(float brightness) {
	global_brightness = brightness;
	render();
}

void process_png(const uint8_t* png_data, size_t png_size) {
	LOG_INF("Decoding PNG");

}

void process_gif(const uint8_t* gif_data, size_t gif_size) {
	LOG_INF("Decoding GIF");
    GIF_Context ctx;
    int result = gif_init(&ctx, gif_data, gif_size, 
                         scratch_buffer, sizeof(scratch_buffer));
    
    if(result != GIF_SUCCESS) {
        LOG_ERR("Error: %s\n", gif_get_error_string(result));
        return;
    }

    int width, height;
    gif_get_info(&ctx, &width, &height);
	LOG_INF("GIF dimensions: %dx%d", width, height);
    
    uint8_t* frame_buffer = malloc(width * height * 3);
	if(frame_buffer == NULL) {
		LOG_ERR("Failed to allocate GIF frame buffer\n");
		gif_close(&ctx);
		return;
	}
    int delay_ms;
    int frame_result;
    
    while((frame_result = gif_next_frame(&ctx, frame_buffer, &delay_ms)) > 0) {
        // Process frame
        // delay_ms contains frame duration
        LOG_INF("Frame decoded, delay: %d ms", delay_ms);
		LOG_HEXDUMP_INF(frame_buffer, 30 * 3, "Decoded GIF frame data:");
		// save frame to current_animation
		struct pixel_animation_frame frame;
		frame.duration_ms = delay_ms;
		memcpy(frame.pixels, frame_buffer, sizeof(frame.pixels));
		current_animation.frames = realloc(current_animation.frames, sizeof(struct pixel_animation_frame) * (current_animation.num_frames + 1));
		memcpy(&current_animation.frames[current_animation.num_frames], &frame, sizeof(struct pixel_animation_frame));
		current_animation.num_frames++;
    }
    
    if(frame_result < 0) {
        LOG_ERR("Decoding error: %s\n", gif_get_error_string(frame_result));
    }
    
    gif_close(&ctx);
    free(frame_buffer);
}

void playback_animation(struct pixel_animation *animation) {
	for(int i = 0; i < animation->num_frames; i++) {
		memcpy(pixels_raw, animation->frames[i].pixels, sizeof(pixels_raw));
		render();
		k_sleep(K_MSEC(animation->frames[i].duration_ms));
	}
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

	// if (lbs_cb.button_cb) {
	// 	button_state = lbs_cb.button_cb();
	// 	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
	// 				 sizeof(*value));
	// }

	return 0;
}



static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_INF("Advertising failed to start (err %d)\n", err);
		return;
	}

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
	LOG_INF("Connected\n");
	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected, reason 0x%02x %s\n", reason, bt_hci_err_to_str(reason));

	dk_set_led_off(CON_STATUS_LED);
}

static void recycled_cb(void)
{
	LOG_INF("Connection object available from previous conn. Disconnect is complete!\n");
	advertising_start();
}


BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
#ifdef CONFIG_BT_LBS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};


static void input_event_callback(struct input_event *evt, void *user_data)
{
	switch(evt->code) {
		case INPUT_KEY_0:
			LOG_INF("Button 0 state: %s", evt->value ? "pressed" : "released");
			button0 = evt->value;
			break;
		case INPUT_KEY_1:
			LOG_INF("Button 1 state: %s", evt->value ? "pressed" : "released");
			button1 = evt->value;
			break;
		case INPUT_KEY_2:
			LOG_INF("Button 2 state: %s", evt->value ? "pressed" : "released");
			button2 = evt->value;
			break;
		case INPUT_KEY_3:
			LOG_INF("Button 3 state: %s", evt->value ? "pressed" : "released");
			button3 = evt->value;
			break;
		default:
			LOG_WRN("Unknown button event code: %u", evt->code);
			break;
	}
	
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
		sys_reboot(SYS_REBOOT_WARM);
	}
	if(button0 && button1 && button2 && button3){
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


int save_pixels_to_file(const char *filename)
{
	struct fs_file_t file;
	int rc;

	fs_file_t_init(&file);

	rc = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		LOG_ERR("Error opening file %s: %d", filename, rc);
		return rc;
	}
	rc = fs_write(&file, pixels_raw, sizeof(pixels_raw));
	if (rc < 0) {
		LOG_ERR("Error writing to file %s: %d", filename, rc);
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	LOG_INF("Saved pixel data to file %s", filename);
	return 0;
}

int load_pixels_from_file(const char *filename)
{
	struct fs_file_t file;
	int rc;

	fs_file_t_init(&file);

	rc = fs_open(&file, filename, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Error opening file %s: %d", filename, rc);
		return rc;
	}
	rc = fs_read(&file, pixels_raw, sizeof(pixels_raw));
	if (rc < 0) {
		LOG_ERR("Error reading from file %s: %d", filename, rc);
		fs_close(&file);
		return rc;
	}
	fs_close(&file);
	LOG_INF("Loaded pixel data from file %s", filename);
	render();
	return 0;
}

void check_power_off(){
	if(shutdown_timeout != 0 && k_uptime_get() > shutdown_timeout) {
		LOG_INF("Shutting down now...");
		shutdown_timeout = 0;
		int rc = gpio_pin_configure_dt(&wakeup_button, GPIO_INPUT);
		if (rc < 0) {
			printf("Could not configure wakeup button GPIO (%d)\n", rc);
		}

		rc = gpio_pin_interrupt_configure_dt(&wakeup_button, GPIO_INT_LEVEL_ACTIVE);
		if (rc < 0) {
			printf("Could not configure wakeup button GPIO interrupt (%d)\n", rc);
		}
		regulator_disable(led_en);
		sys_poweroff();
	}
	if(button2 && button3){
		if(shutdown_timeout == 0){
			shutdown_timeout = k_uptime_get() + 1000;
		}
		LOG_INF("Shutdown in %d seconds...", (int)((shutdown_timeout - k_uptime_get()) / 1000));
	}else{
		if(shutdown_timeout != 0){
			LOG_INF("Button combo released, cancelling shutdown");
		}
		shutdown_timeout = 0;
	}
}

int main(void)
{
	int blink_status = 0;
	int err;
	size_t color = 0;
	int rc;
	// NRF_POWER->GPREGRET = 0x57;
	// sys_reboot(0x57);
	regulator_enable(led_en);
	LOG_INF("build time: " __DATE__ " " __TIME__);

	k_sem_init(&sync, 0, 1);

	rc = STATS_INIT_AND_REG(smp_svr_stats, STATS_SIZE_32, "smp_svr_stats");
	if (rc < 0) {
		LOG_ERR("Error initializing stats system [%d]", rc);
	}

	rc = fs_mount(&littlefs_mnt);
	if (rc < 0) {
		LOG_ERR("Error mounting littlefs [%d]", rc);
	}

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s is not ready", strip->name);
		return 0;
	}

	err = dk_leds_init();
	if (err) {
		LOG_INF("LEDs init failed (err %d)\n", err);
		return 0;
	}


	err = bt_enable(NULL);
	if (err) {
		LOG_INF("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

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
	memcpy(&device_name[sizeof(device_name) - 8], &addr_s[6], 2);
	memcpy(&device_name[sizeof(device_name) - 6], &addr_s[9], 2);
	memcpy(&device_name[sizeof(device_name) - 4], &addr_s[12], 2);
	memcpy(&device_name[sizeof(device_name) - 2], &addr_s[15], 2);
	LOG_INF("Device name: %s.", device_name);
	bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	STATS_SET(smp_svr_stats, version_major, FW_VERSION_MAJOR);
	STATS_SET(smp_svr_stats, version_minor, FW_VERSION_MINOR);


	k_work_init(&adv_work, adv_work_handler);
	advertising_start();
	clear_display();
	load_pixels_from_file("/lfs1/default.dat");
	
	if (boot_is_img_confirmed() == 0) {
        LOG_WRN("Boot image not confirmed. Confirming now.");
        boot_write_img_confirmed();
    }
	
	for (;;) {
		// dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		
		// STATS_INC(smp_svr_stats, ticks);
		// k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
		if(playAnimation) {
			playback_animation(&current_animation);
		}
		k_sem_take(&sync, K_MSEC(1));
		check_for_recovery_mode();
		check_power_off();
		
	}
}

