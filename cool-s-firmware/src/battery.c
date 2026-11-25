/*
 * Battery estimate, see battery.h. The Pro Micro clones the badge uses have
 * no divider on the battery, so the SAADC reads VDDH / 5 (the devicetree's
 * zephyr,user io-channels), on the system work queue.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>

#include "battery.h"

#ifdef CONFIG_BATTERY_LOG
#include <stdio.h>
#include <zephyr/fs/fs.h>
#include "media_files.h"
#endif

LOG_MODULE_REGISTER(battery);

#define BATTERY_INTERVAL K_SECONDS(30)
/* VDDH is read through the SAADC's internal divide by 5 */
#define VDDH_SCALE 5
/*
 * Samples per reading, spread over SAMPLES * SAMPLE_INTERVAL_US so a radio
 * burst or an LED update hits only a few of them. The highest and lowest
 * quarter are left out of the average. Each sample is already 4 conversions
 * averaged by the SAADC itself, see the devicetree.
 */
#define SAMPLES 32
#define SAMPLE_INTERVAL_US 500
#define SAMPLES_TRIMMED (SAMPLES / 4)

static const struct adc_dt_spec vddh = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/*
 * Battery voltage under load for each percent, from a discharge of the badge's
 * cell running rainbow.lua at 30 % brightness (2026-11-24, ~8.0 h, built with
 * CONFIG_BATTERY_LOG). The current is about constant, so percent is the share
 * of that run time left. 0 % is where the badge browned out.
 */
static const uint16_t curve_mv[101] = {
	2790, 2990, 3104, 3188, 3252, 3304, 3345, 3378, 3398, 3411,  /* 0 % */
	3421, 3428, 3434, 3439, 3446, 3452, 3459, 3468, 3475, 3482,  /* 10 % */
	3492, 3498, 3505, 3512, 3518, 3522, 3529, 3534, 3540, 3545,  /* 20 % */
	3552, 3556, 3562, 3566, 3571, 3577, 3582, 3587, 3592, 3596,  /* 30 % */
	3601, 3606, 3612, 3618, 3624, 3628, 3638, 3643, 3650, 3656,  /* 40 % */
	3665, 3671, 3678, 3686, 3695, 3703, 3711, 3721, 3729, 3737,  /* 50 % */
	3746, 3756, 3763, 3771, 3778, 3788, 3794, 3803, 3812, 3822,  /* 60 % */
	3830, 3837, 3848, 3855, 3862, 3871, 3878, 3887, 3896, 3903,  /* 70 % */
	3911, 3918, 3926, 3933, 3942, 3950, 3957, 3964, 3969, 3976,  /* 80 % */
	3983, 3989, 3994, 4001, 4007, 4015, 4024, 4035, 4045, 4059,  /* 90 % */
	4075,                                                    /* 100 % */
};

static struct battery_state state;
static struct k_spinlock lock;
static bool calibrated;

static void measure(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(measure_work, measure);

/* the highest percent whose voltage mv has reached */
static uint8_t percent_from_mv(uint16_t mv)
{
	uint8_t percent = 0;

	while (percent < 100 && mv >= curve_mv[percent + 1]) {
		percent++;
	}
	return percent;
}

static int read_mv(int32_t *mv)
{
	/* on the stack: the work queue and the battery button may both read at once */
	int16_t raw[SAMPLES];
	struct adc_sequence_options options = {
		.interval_us = SAMPLE_INTERVAL_US,
		.extra_samplings = SAMPLES - 1,
	};
	struct adc_sequence sequence = {
		.buffer = raw,
		.buffer_size = sizeof(raw[0]),
		/* the SAADC's offset drifts with temperature; once per start is enough here */
		.calibrate = true,
	};
	int rc;

	adc_sequence_init_dt(&vddh, &sequence);
	/*
	 * Calibration on its own: it takes longer than SAMPLE_INTERVAL_US, and a
	 * sample that overruns the interval fails a timed sequence.
	 */
	if (!calibrated) {
		rc = adc_read_dt(&vddh, &sequence);
		if (rc) {
			return rc;
		}
		calibrated = true;
	}

	sequence.calibrate = false;
	sequence.options = &options;
	sequence.buffer_size = sizeof(raw);
	rc = adc_read_dt(&vddh, &sequence);
	/* -EBUSY: a sample started late, a radio interrupt say. They were all taken. */
	if (rc && rc != -EBUSY) {
		return rc;
	}

	/* insertion sort: 32 values, and they are nearly in order already */
	for (int i = 1; i < SAMPLES; i++) {
		int16_t value = raw[i];
		int j = i;
		for (; j > 0 && raw[j - 1] > value; j--) {
			raw[j] = raw[j - 1];
		}
		raw[j] = value;
	}
	int32_t sum = 0;
	for (int i = SAMPLES_TRIMMED; i < SAMPLES - SAMPLES_TRIMMED; i++) {
		sum += raw[i];
	}
	*mv = sum / (SAMPLES - 2 * SAMPLES_TRIMMED);

	rc = adc_raw_to_millivolts_dt(&vddh, mv);
	*mv *= VDDH_SCALE;
	return rc;
}

#ifdef CONFIG_BATTERY_LOG
/*
 * For profiling the battery: every reading off USB, as "seconds since boot,
 * mV, unloaded" lines after a "# boot" line, appended to BATTERY_LOG_PATH.
 * Each one is written through, so the log ends at the last reading before the
 * battery gave out.
 */
#define BATTERY_LOG_PATH MEDIA_ROOT "/battery.log"

static void log_reading(int32_t mv, bool unloaded)
{
	static bool started;
	struct fs_file_t file;
	char line[40];
	int len = 0;

	fs_file_t_init(&file);
	if (fs_open(&file, BATTERY_LOG_PATH, FS_O_CREATE | FS_O_APPEND | FS_O_WRITE) < 0) {
		return;
	}
	if (!started) {
		len = snprintf(line, sizeof(line), "# boot\n");
		started = true;
	}
	len += snprintf(&line[len], sizeof(line) - len, "%u,%d,%d\n",
			(uint32_t)(k_uptime_get() / 1000), mv, unloaded);
	fs_write(&file, line, len);
	fs_close(&file);
}
#endif

void battery_update(bool unloaded)
{
	bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
	int32_t mv = 0;
	int rc = usb ? 0 : read_mv(&mv);

#ifdef CONFIG_BATTERY_LOG
	if (!usb && rc == 0) {
		log_reading(mv, unloaded);
	}
#endif

	k_spinlock_key_t key = k_spin_lock(&lock);
	if (usb) {
		/* the battery charges, and reads higher than it is for a while after */
		state.valid = false;
	} else if (rc == 0) {
		/*
		 * An average, so a burst of bright LEDs does not swing it. A reading
		 * without the LEDs is the better one, and replaces it.
		 */
		mv = state.valid && !unloaded ? (3 * state.mv + mv) / 4 : mv;
		state.mv = CLAMP(mv, 0, UINT16_MAX);
		state.percent = percent_from_mv(state.mv);
		state.valid = true;
	}
	state.usb = usb;
	struct battery_state now = state;
	k_spin_unlock(&lock, key);

	if (rc) {
		LOG_ERR("Could not read the battery voltage: %d", rc);
	} else if (usb) {
		LOG_DBG("On USB power");
	} else {
		LOG_DBG("Battery %u mV, %u%%%s", now.mv, now.percent, unloaded ? " without load" : "");
	}
}

static void measure(struct k_work *work)
{
	ARG_UNUSED(work);
	battery_update(false);
	k_work_schedule(&measure_work, BATTERY_INTERVAL);
}

int battery_init(void)
{
	if (!adc_is_ready_dt(&vddh)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}
	int rc = adc_channel_setup_dt(&vddh);
	if (rc) {
		LOG_ERR("Could not set up the ADC channel: %d", rc);
		return rc;
	}
	k_work_schedule(&measure_work, K_NO_WAIT);
	return 0;
}

void battery_get(struct battery_state *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*out = state;
	k_spin_unlock(&lock, key);
}
