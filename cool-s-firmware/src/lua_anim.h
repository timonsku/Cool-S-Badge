/*
 * LED animations written in Lua, see docs/LUA.md for the script API.
 *
 * A script runs in its own low priority thread with its own heap, so a
 * broken or endless script cannot take the badge down. It never touches the
 * LEDs: led.show() hands a frame to the main loop, which picks it up with
 * lua_anim_frame() and renders it like any other picture.
 */
#ifndef LUA_ANIM_H
#define LUA_ANIM_H

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/drivers/led_strip.h>

/* scripts are parsed on the badge, from a copy in the Lua heap */
#define LUA_SCRIPT_MAX_SIZE (12 * 1024)

/*
 * Stop the running script, if any, then load and start the one at path.
 * Returns once the script compiled (0) or failed to (negative errno); its
 * errors are logged.
 */
int lua_anim_start(const char *path);

/* Stop the running script, if any, and wait until it has. */
void lua_anim_stop(void);

/* Copy the latest frame into out (n pixels) if there is a new one. */
bool lua_anim_frame(struct led_rgb *out, size_t n);

/*
 * Whether a script runs, the path of the last one started and why it
 * failed, "" if it did not. Safe from any thread.
 */
bool lua_anim_status(char *path, size_t path_len, char *error, size_t error_len);

/*
 * Implemented in main.c: show the Lua mode with the script name from the lua
 * folder, as if picked with the buttons. Called on the SMP thread.
 */
int display_run_script(const char *name);

#endif
