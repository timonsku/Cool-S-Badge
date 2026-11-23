/*
 * Runs one Lua script at a time for LED animations, see lua_anim.h and
 * docs/LUA.md. The Lua core comes from the lua_zephyr module.
 */
#include "lua_anim.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/random/random.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luaz_utils.h>

LOG_MODULE_REGISTER(lua_anim, LOG_LEVEL_INF);

#define STRIP_NUM_PIXELS DT_PROP(DT_ALIAS(led_strip), chain_length)

#define LUA_HEAP_SIZE (40 * 1024)
/* the parser and VM recurse in C, LUAI_MAXCCALLS (CMakeLists.txt) bounds that */
#define LUA_STACK_SIZE (10 * 1024)
/* lowest application priority: scripts only get the time nothing else needs */
#define LUA_PRIORITY K_LOWEST_APPLICATION_THREAD_PRIO
/* VM instructions between checks for a stop request */
#define HOOK_INTERVAL 1000
#define LOAD_TIMEOUT K_SECONDS(3)
#define STOP_TIMEOUT K_SECONDS(1)
#define DEFAULT_FPS 50
#define MAX_FPS 100

/* raised to unwind a script that is asked to stop */
#define STOP_MESSAGE "stopped"

/*
 * LED centres in chain order, scaled to 0..255 across the LEDs: x from left to
 * right, y from top to bottom, as the badge is worn. From web/js/badge-layout.js.
 */
static const uint8_t led_position[][2] = {
	{11, 146}, {11, 163}, {11, 179}, {11, 194}, {11, 209}, {34, 225},
	{74, 240}, {136, 255}, {195, 240}, {232, 225}, {255, 210}, {255, 194},
	{255, 179}, {255, 163}, {227, 145}, {159, 130}, {91, 115}, {3, 99},
	{0, 80}, {0, 65}, {0, 48}, {2, 31}, {61, 14}, {129, 0},
	{190, 14}, {244, 31}, {249, 48}, {249, 65}, {249, 83}, {249, 98},
};
BUILD_ASSERT(ARRAY_SIZE(led_position) == STRIP_NUM_PIXELS, "LED positions do not match the strip");

static K_THREAD_STACK_DEFINE(lua_stack, LUA_STACK_SIZE);
static struct k_thread lua_thread;
static uint8_t heap_mem[LUA_HEAP_SIZE] __aligned(8);
static struct sys_heap heap;

/* what the script draws on, only the Lua thread uses it */
static struct led_rgb canvas[STRIP_NUM_PIXELS];
/* the last frame shown, for the main loop */
static struct led_rgb shown[STRIP_NUM_PIXELS];
static bool shown_fresh;
static struct k_spinlock shown_lock;

static char script_path[64];
/* why the last script failed, for the web UI; under shown_lock */
static char last_error[160];
static int load_result;
static int64_t started_at;
static atomic_t stop_requested;
static atomic_t running;

static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(loaded_sem, 0, 1);
static K_SEM_DEFINE(idle_sem, 0, 1);
/* given to cut a sleep() short when the script has to stop */
static K_SEM_DEFINE(wake_sem, 0, 1);

static void check_stop(lua_State *L)
{
	if (atomic_get(&stop_requested)) {
		luaL_error(L, STOP_MESSAGE);
	}
}

static void stop_hook(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);
	check_stop(L);
}

static void publish(void)
{
	k_spinlock_key_t key = k_spin_lock(&shown_lock);
	memcpy(shown, canvas, sizeof(shown));
	shown_fresh = true;
	k_spin_unlock(&shown_lock, key);
}

/*
 * pcall() that cannot catch a stop request: the stop error would otherwise
 * be caught again and again by a loop around pcall().
 */
static int l_pcall(lua_State *L)
{
	luaL_checkany(L, 1);
	int status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);

	check_stop(L);
	lua_pushboolean(L, status == LUA_OK);
	lua_insert(L, 1);
	return lua_gettop(L);
}

/* ---- colours: packed 0xRRGGBB integers ---- */

static uint8_t clamp_channel(lua_Number v)
{
	return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f);
}

static lua_Integer pack(uint8_t r, uint8_t g, uint8_t b)
{
	return ((lua_Integer)r << 16) | ((lua_Integer)g << 8) | b;
}

static struct led_rgb unpack(lua_Integer c)
{
	return (struct led_rgb){.r = (c >> 16) & 0xff, .g = (c >> 8) & 0xff, .b = c & 0xff};
}

/* a colour from arguments at idx: either one packed integer or r, g, b */
static struct led_rgb color_arg(lua_State *L, int idx)
{
	if (lua_gettop(L) >= idx + 2) {
		return (struct led_rgb){
			.r = clamp_channel(luaL_checknumber(L, idx)),
			.g = clamp_channel(luaL_checknumber(L, idx + 1)),
			.b = clamp_channel(luaL_checknumber(L, idx + 2)),
		};
	}
	return unpack(luaL_optinteger(L, idx, 0));
}

static lua_Number fract(lua_Number x)
{
	return x - floorf(x);
}

/* rgb(r, g, b): channels 0..255, clamped */
static int l_rgb(lua_State *L)
{
	struct led_rgb c = color_arg(L, 1);
	lua_pushinteger(L, pack(c.r, c.g, c.b));
	return 1;
}

/* hsv(h, s, v): h wraps around at 1, s and v are 0..1 and default to 1 */
static int l_hsv(lua_State *L)
{
	lua_Number h = fract(luaL_checknumber(L, 1)) * 6;
	lua_Number s = luaL_optnumber(L, 2, 1);
	lua_Number v = luaL_optnumber(L, 3, 1) * 255;
	int sector = (int)h;
	lua_Number f = h - sector;
	lua_Number p = v * (1 - s);
	lua_Number q = v * (1 - s * f);
	lua_Number t = v * (1 - s * (1 - f));
	lua_Number r, g, b;

	switch (sector) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	lua_pushinteger(L, pack(clamp_channel(r), clamp_channel(g), clamp_channel(b)));
	return 1;
}

/* mix(a, b, t): from colour a at t = 0 to b at t = 1 */
static int l_mix(lua_State *L)
{
	struct led_rgb a = unpack(luaL_checkinteger(L, 1));
	struct led_rgb b = unpack(luaL_checkinteger(L, 2));
	lua_Number t = luaL_checknumber(L, 3);

	t = t < 0 ? 0 : t > 1 ? 1 : t;
	lua_pushinteger(L, pack(clamp_channel(a.r + (b.r - a.r) * t),
				clamp_channel(a.g + (b.g - a.g) * t),
				clamp_channel(a.b + (b.b - a.b) * t)));
	return 1;
}

/* dim(c, f): c with every channel times f */
static int l_dim(lua_State *L)
{
	struct led_rgb c = unpack(luaL_checkinteger(L, 1));
	lua_Number f = luaL_checknumber(L, 2);

	lua_pushinteger(L, pack(clamp_channel(c.r * f), clamp_channel(c.g * f),
				clamp_channel(c.b * f)));
	return 1;
}

/* split(c): r, g, b */
static int l_split(lua_State *L)
{
	struct led_rgb c = unpack(luaL_checkinteger(L, 1));

	lua_pushinteger(L, c.r);
	lua_pushinteger(L, c.g);
	lua_pushinteger(L, c.b);
	return 3;
}

/* ---- timing and shapes ---- */

/* sleep(ms) */
static int l_sleep(lua_State *L)
{
	lua_Integer ms = luaL_checkinteger(L, 1);

	k_sem_take(&wake_sem, K_MSEC(MAX(ms, 0)));
	check_stop(L);
	return 0;
}

/* millis(): milliseconds since the script started */
static int l_millis(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)(k_uptime_get() - started_at));
	return 1;
}

/* now(): seconds since the script started */
static int l_now(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)(k_uptime_get() - started_at) / 1000);
	return 1;
}

/* wave(x): a sine from 0 to 1 and back, once per 1 of x, starting at 0 */
static int l_wave(lua_State *L)
{
	lua_Number x = luaL_checknumber(L, 1);

	lua_pushnumber(L, 0.5f - 0.5f * cosf(fract(x) * 6.2831853f));
	return 1;
}

/* tri(x): like wave() but in straight lines */
static int l_tri(lua_State *L)
{
	lua_Number x = fract(luaL_checknumber(L, 1));

	lua_pushnumber(L, x < 0.5f ? 2 * x : 2 - 2 * x);
	return 1;
}

/* clamp(x, lo, hi), lo and hi default to 0 and 1 */
static int l_clamp(lua_State *L)
{
	lua_Number x = luaL_checknumber(L, 1);
	lua_Number lo = luaL_optnumber(L, 2, 0);
	lua_Number hi = luaL_optnumber(L, 3, 1);

	lua_pushnumber(L, x < lo ? lo : x > hi ? hi : x);
	return 1;
}

/* lerp(a, b, t) */
static int l_lerp(lua_State *L)
{
	lua_Number a = luaL_checknumber(L, 1);
	lua_Number b = luaL_checknumber(L, 2);

	lua_pushnumber(L, a + (b - a) * luaL_checknumber(L, 3));
	return 1;
}

/* ---- the led table ---- */

/* LED numbers start at 1 and wrap around: 0 is the last LED, count + 1 the first */
static int led_index(lua_State *L, int idx)
{
	lua_Integer i = (lua_Integer)floorf(luaL_checknumber(L, idx)) - 1;

	i %= STRIP_NUM_PIXELS;
	return i < 0 ? i + STRIP_NUM_PIXELS : i;
}

/* led.set(i, c) or led.set(i, r, g, b) */
static int l_led_set(lua_State *L)
{
	int i = led_index(L, 1);

	canvas[i] = color_arg(L, 2);
	return 0;
}

/* led.add(i, c): adds to what the LED already has, for overlapping effects */
static int l_led_add(lua_State *L)
{
	int i = led_index(L, 1);
	struct led_rgb c = color_arg(L, 2);

	canvas[i].r = MIN(canvas[i].r + c.r, 255);
	canvas[i].g = MIN(canvas[i].g + c.g, 255);
	canvas[i].b = MIN(canvas[i].b + c.b, 255);
	return 0;
}

/* led.get(i): the colour as a packed integer */
static int l_led_get(lua_State *L)
{
	struct led_rgb c = canvas[led_index(L, 1)];

	lua_pushinteger(L, pack(c.r, c.g, c.b));
	return 1;
}

/* led.fill(c) or led.fill(r, g, b) */
static int l_led_fill(lua_State *L)
{
	struct led_rgb c = color_arg(L, 1);

	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		canvas[i] = c;
	}
	return 0;
}

static int l_led_clear(lua_State *L)
{
	ARG_UNUSED(L);
	memset(canvas, 0, sizeof(canvas));
	return 0;
}

/* led.fade(f): every LED times f, 0.9 leaves a short trail */
static int l_led_fade(lua_State *L)
{
	lua_Number f = luaL_checknumber(L, 1);

	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		canvas[i].r = clamp_channel(canvas[i].r * f);
		canvas[i].g = clamp_channel(canvas[i].g * f);
		canvas[i].b = clamp_channel(canvas[i].b * f);
	}
	return 0;
}

/* led.rotate(n): moves every colour n LEDs further along the chain */
static int l_led_rotate(lua_State *L)
{
	struct led_rgb copy[STRIP_NUM_PIXELS];
	lua_Integer n = luaL_optinteger(L, 1, 1) % STRIP_NUM_PIXELS;

	if (n < 0) {
		n += STRIP_NUM_PIXELS;
	}
	memcpy(copy, canvas, sizeof(copy));
	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		canvas[(i + n) % STRIP_NUM_PIXELS] = copy[i];
	}
	return 0;
}

/* led.show(): displays what was drawn so far; drawing continues on the same picture */
static int l_led_show(lua_State *L)
{
	check_stop(L);
	publish();
	return 0;
}

static const luaL_Reg led_funcs[] = {
	{"set", l_led_set},
	{"add", l_led_add},
	{"get", l_led_get},
	{"fill", l_led_fill},
	{"clear", l_led_clear},
	{"fade", l_led_fade},
	{"rotate", l_led_rotate},
	{"show", l_led_show},
	{NULL, NULL},
};

static const luaL_Reg global_funcs[] = {
	{"pcall", l_pcall},
	{"rgb", l_rgb},
	{"hsv", l_hsv},
	{"mix", l_mix},
	{"dim", l_dim},
	{"split", l_split},
	{"sleep", l_sleep},
	{"millis", l_millis},
	{"now", l_now},
	{"wave", l_wave},
	{"tri", l_tri},
	{"clamp", l_clamp},
	{"lerp", l_lerp},
	{NULL, NULL},
};

static const struct {
	const char *name;
	uint32_t value;
} named_colors[] = {
	{"BLACK", 0x000000}, {"WHITE", 0xffffff}, {"RED", 0xff0000},
	{"GREEN", 0x00ff00}, {"BLUE", 0x0000ff}, {"YELLOW", 0xffff00},
	{"CYAN", 0x00ffff}, {"MAGENTA", 0xff00ff}, {"ORANGE", 0xff6000},
	{"PURPLE", 0x8000ff}, {"PINK", 0xff2080},
};

/* led.x and led.y: positions from 0 to 1, by LED number */
static void push_positions(lua_State *L, int axis)
{
	lua_createtable(L, STRIP_NUM_PIXELS, 0);
	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		lua_pushnumber(L, led_position[i][axis] / 255.0f);
		lua_rawseti(L, -2, i + 1);
	}
}

static void open_api(lua_State *L)
{
	luaz_openlibs(L);
	/* as globals, like everywhere else Lua runs */
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	/* Lua seeds from time(), which on the badge is only the uptime */
	lua_getfield(L, -1, "randomseed");
	lua_pushinteger(L, (lua_Integer)sys_rand32_get());
	lua_call(L, 1, 0);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	lua_pop(L, 2);

	lua_pushglobaltable(L);
	/*
	 * Everything a script needs is a global. require() would reach zephyr.msleep,
	 * which ignores stop requests, and the loaders could run bytecode or use stdio.
	 * xpcall() could catch stop requests, pcall() is replaced for the same reason.
	 */
	static const char *const removed[] = {"require", "load", "loadfile", "dofile", "xpcall"};
	for (size_t i = 0; i < ARRAY_SIZE(removed); i++) {
		lua_pushnil(L);
		lua_setfield(L, -2, removed[i]);
	}
	luaL_setfuncs(L, global_funcs, 0);
	for (size_t i = 0; i < ARRAY_SIZE(named_colors); i++) {
		lua_pushinteger(L, named_colors[i].value);
		lua_setfield(L, -2, named_colors[i].name);
	}
	lua_pop(L, 1);

	luaL_newlib(L, led_funcs);
	lua_pushinteger(L, STRIP_NUM_PIXELS);
	lua_setfield(L, -2, "count");
	push_positions(L, 0);
	lua_setfield(L, -2, "x");
	push_positions(L, 1);
	lua_setfield(L, -2, "y");
	lua_setglobal(L, "led");
}

/* protected: opens the libraries and leaves the compiled script on the stack */
static int load_script(lua_State *L)
{
	struct fs_dirent info;
	struct fs_file_t file;
	char name[sizeof(script_path) + 1];
	const char *base = strrchr(script_path, '/');
	int rc;

	open_api(L);

	rc = fs_stat(script_path, &info);
	if (rc < 0) {
		return luaL_error(L, "%s: cannot open (%d)", script_path, rc);
	}
	if (info.type != FS_DIR_ENTRY_FILE || info.size == 0 ||
	    info.size > LUA_SCRIPT_MAX_SIZE) {
		return luaL_error(L, "%s: empty or larger than %d bytes", script_path,
				  LUA_SCRIPT_MAX_SIZE);
	}
	/* a userdata, so the garbage collector frees it */
	char *text = lua_newuserdatauv(L, info.size, 0);
	fs_file_t_init(&file);
	rc = fs_open(&file, script_path, FS_O_READ);
	if (rc < 0) {
		return luaL_error(L, "%s: cannot open (%d)", script_path, rc);
	}
	ssize_t got = fs_read(&file, text, info.size);
	fs_close(&file);
	if (got != (ssize_t)info.size) {
		return luaL_error(L, "%s: read error (%d)", script_path, (int)got);
	}

	/* "@name" makes error messages read "name.lua:12: ..." */
	snprintf(name, sizeof(name), "@%s", base ? base + 1 : script_path);
	/* text only: malformed bytecode can crash the VM */
	if (luaL_loadbufferx(L, text, info.size, name, "t") != LUA_OK) {
		return lua_error(L);
	}
	return 1;
}

/*
 * protected: runs the script. If it defines frame(t, dt) and returns, frame
 * is called at the global fps (DEFAULT_FPS) and every frame is shown.
 */
static int run_script(lua_State *L)
{
	lua_call(L, 0, 0);
	if (lua_getglobal(L, "frame") != LUA_TFUNCTION) {
		return 0;
	}
	lua_pop(L, 1);

	int64_t last = k_uptime_get();
	for (;;) {
		lua_Integer fps = (lua_getglobal(L, "fps"), luaL_optinteger(L, -1, DEFAULT_FPS));
		lua_pop(L, 1);
		fps = CLAMP(fps, 1, MAX_FPS);

		int64_t now = k_uptime_get();
		lua_getglobal(L, "frame");
		lua_pushnumber(L, (lua_Number)(now - started_at) / 1000);
		lua_pushnumber(L, (lua_Number)(now - last) / 1000);
		lua_call(L, 2, 0);
		check_stop(L);
		publish();

		last = now;
		int64_t wait = now + 1000 / fps - k_uptime_get();
		/* always yield a little, the script runs on idle time */
		k_sem_take(&wake_sem, K_MSEC(MAX(wait, 1)));
		check_stop(L);
	}
}

/* a script that fails shows every other LED red */
static void show_error(void)
{
	for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
		canvas[i] = (struct led_rgb){.r = (i % 2) ? 0 : 255};
	}
	publish();
}

static void report(lua_State *L, const char *what)
{
	const char *msg = lua_tostring(L, -1);

	if (atomic_get(&stop_requested)) {
		return;
	}
	if (!msg) {
		msg = "(error object is not a string)";
	}
	LOG_ERR("Lua %s: %s", what, msg);
	k_spinlock_key_t key = k_spin_lock(&shown_lock);
	strncpy(last_error, msg, sizeof(last_error) - 1);
	last_error[sizeof(last_error) - 1] = '\0';
	k_spin_unlock(&shown_lock, key);
	show_error();
}

static void lua_thread_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&start_sem, K_FOREVER);

		sys_heap_init(&heap, heap_mem, sizeof(heap_mem));
		memset(canvas, 0, sizeof(canvas));
		started_at = k_uptime_get();

		lua_State *L = lua_newstate(lua_zephyr_allocator, &heap, sys_rand32_get());
		if (!L) {
			LOG_ERR("Lua: no memory for a state");
			load_result = -ENOMEM;
			k_sem_give(&loaded_sem);
		} else {
			lua_pushcfunction(L, load_script);
			int rc = lua_pcall(L, 0, 1, 0);
			load_result = rc == LUA_OK ? 0 : rc == LUA_ERRMEM ? -ENOMEM : -EINVAL;
			if (rc != LUA_OK) {
				report(L, "load error");
			}
			k_sem_give(&loaded_sem);

			if (rc == LUA_OK) {
				LOG_INF("Running %s", script_path);
				lua_sethook(L, stop_hook, LUA_MASKCOUNT, HOOK_INTERVAL);
				lua_pushcfunction(L, run_script);
				lua_insert(L, -2);
				if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
					report(L, "error");
				} else {
					LOG_INF("%s finished", script_path);
				}
				/* finalizers run on close and must not be stopped half way */
				lua_sethook(L, NULL, 0, 0);
			}
			lua_close(L);
		}

		atomic_set(&running, 0);
		k_sem_give(&idle_sem);
	}
}

/*
 * time(), which Lua uses for its default random seed, needs this. The badge
 * has no clock, so the time is the uptime.
 */
int gettimeofday(struct timeval *tv, void *tz)
{
	ARG_UNUSED(tz);
	int64_t us = k_ticks_to_us_floor64(k_uptime_ticks());

	tv->tv_sec = us / USEC_PER_SEC;
	tv->tv_usec = us % USEC_PER_SEC;
	return 0;
}

void lua_anim_stop(void)
{
	if (!atomic_get(&running)) {
		return;
	}
	atomic_set(&stop_requested, 1);
	k_sem_give(&wake_sem);
	if (k_sem_take(&idle_sem, STOP_TIMEOUT) != 0) {
		LOG_ERR("Lua script does not stop");
	}
}

int lua_anim_start(const char *path)
{
	static bool thread_started;

	lua_anim_stop();
	if (atomic_get(&running)) {
		return -EBUSY;
	}
	if (strlen(path) >= sizeof(script_path)) {
		return -ENAMETOOLONG;
	}
	if (!thread_started) {
		k_thread_create(&lua_thread, lua_stack, K_THREAD_STACK_SIZEOF(lua_stack),
				lua_thread_main, NULL, NULL, NULL, LUA_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&lua_thread, "lua");
		thread_started = true;
	}

	strcpy(script_path, path);
	/* nothing the previous script drew is shown anymore */
	k_spinlock_key_t key = k_spin_lock(&shown_lock);
	shown_fresh = false;
	last_error[0] = '\0';
	k_spin_unlock(&shown_lock, key);
	atomic_set(&stop_requested, 0);
	k_sem_reset(&wake_sem);
	k_sem_reset(&loaded_sem);
	k_sem_reset(&idle_sem);
	atomic_set(&running, 1);
	k_sem_give(&start_sem);

	if (k_sem_take(&loaded_sem, LOAD_TIMEOUT) != 0) {
		LOG_ERR("Lua script did not load in time");
		lua_anim_stop();
		return -ETIMEDOUT;
	}
	return load_result;
}

bool lua_anim_status(char *path, size_t path_len, char *error, size_t error_len)
{
	k_spinlock_key_t key = k_spin_lock(&shown_lock);

	strncpy(path, script_path, path_len - 1);
	path[path_len - 1] = '\0';
	strncpy(error, last_error, error_len - 1);
	error[error_len - 1] = '\0';
	k_spin_unlock(&shown_lock, key);
	return atomic_get(&running);
}

bool lua_anim_frame(struct led_rgb *out, size_t n)
{
	bool fresh;
	k_spinlock_key_t key = k_spin_lock(&shown_lock);

	fresh = shown_fresh;
	if (fresh) {
		memcpy(out, shown, MIN(n, (size_t)STRIP_NUM_PIXELS) * sizeof(*out));
		shown_fresh = false;
	}
	k_spin_unlock(&shown_lock, key);
	return fresh;
}
