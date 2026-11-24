/*
 * Battery state of charge, estimated from the battery voltage with a lookup
 * table for a single LiPo cell.
 *
 * The voltage is VDDH, which the battery supplies. On USB, VDDH comes from
 * USB and the battery is charging, so there is no reading then.
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

struct battery_state {
	bool usb;        /* on USB power: mv and percent say nothing about the battery */
	bool valid;      /* a reading was taken since the badge left USB power */
	uint16_t mv;     /* averaged battery voltage */
	uint8_t percent; /* 0..100 */
};

/* Takes the first reading and then one every BATTERY_INTERVAL. */
int battery_init(void);

/*
 * Takes a reading now, for a caller that needs an up to date one. unloaded
 * says the LEDs are off: the reading then replaces the average instead of
 * going into it. Safe from any thread; takes about 10 ms.
 */
void battery_update(bool unloaded);

/* The latest estimate, safe from any thread. */
void battery_get(struct battery_state *out);

#endif
