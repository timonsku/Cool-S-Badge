/*
 * A freshly installed image is on trial until it has shown that it works. It
 * confirms itself after running without issues for a while. If it crashes or hangs
 * the badge resets and MCUboot puts the previous image back.
 * Some badges in the field have nothing but BLE and USB to recover with.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include "update_trial.h"

LOG_MODULE_REGISTER(update_trial);

#define PROVEN_AFTER_MS	(30 * MSEC_PER_SEC)
/* a timer, so that a hung main loop reverts too */
#define GIVE_UP_AFTER	K_SECONDS(90)

static bool on_trial;

static void give_up(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_TIMER_DEFINE(give_up_timer, give_up, NULL);

void update_trial_poll(bool healthy)
{
	if (!on_trial || !healthy || k_uptime_get() < PROVEN_AFTER_MS) {
		return;
	}

	if (boot_write_img_confirmed() == 0) {
		on_trial = false;
		k_timer_stop(&give_up_timer);
		LOG_INF("New firmware works, confirmed it");
	}
}

void update_trial_failed(void)
{
	if (on_trial) {
		LOG_ERR("New firmware failed to start, reverting");
		sys_reboot(SYS_REBOOT_COLD);
	}
}

static int update_trial_init(void)
{
	on_trial = !boot_is_img_confirmed();
	if (on_trial) {
		LOG_WRN("New firmware is on trial");
		k_timer_start(&give_up_timer, GIVE_UP_AFTER, K_NO_WAIT);
	}

	return 0;
}

SYS_INIT(update_trial_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
