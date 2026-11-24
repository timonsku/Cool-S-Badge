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

static const uint16_t curve_mv[101] = {
	3300, 3352, 3403, 3450, 3489, 3520, 3543, 3561, 3576, 3589,  /* 0 % */
	3600, 3610, 3619, 3626, 3632, 3638, 3642, 3647, 3651, 3655,  /* 10 % */
	3660, 3665, 3669, 3673, 3677, 3681, 3685, 3689, 3692, 3696,  /* 20 % */
	3700, 3704, 3708, 3712, 3716, 3720, 3724, 3728, 3732, 3736,  /* 30 % */
	3740, 3744, 3748, 3752, 3756, 3759, 3763, 3767, 3771, 3776,  /* 40 % */
	3780, 3785, 3789, 3794, 3799, 3804, 3809, 3814, 3819, 3825,  /* 50 % */
	3830, 3836, 3841, 3847, 3853, 3859, 3865, 3871, 3877, 3884,  /* 60 % */
	3890, 3897, 3903, 3910, 3916, 3923, 3930, 3937, 3945, 3952,  /* 70 % */
	3960, 3968, 3977, 3985, 3994, 4003, 4012, 4022, 4031, 4041,  /* 80 % */
	4050, 4060, 4069, 4079, 4089, 4099, 4110, 4120, 4130, 4140,  /* 90 % */
	4150,                                                        /* 100 % */
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

void battery_update(bool unloaded)
{
	bool usb = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
	int32_t mv = 0;
	int rc = usb ? 0 : read_mv(&mv);

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
