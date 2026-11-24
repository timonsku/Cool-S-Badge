/*
 * Console output over Bluetooth. Everything printed to the console (the log,
 * printk and Lua's print) also goes out on the Nordic UART Service, which the
 * web UI shows. The USB console gets it as before, and the shell stays on USB.
 *
 * Output only. The newest BUFFER_SIZE bytes are kept, so whoever subscribes
 * also sees what came before, the start of the log included.
 *
 * Zephyr's nus-console snippet would move the console and the shell to NUS
 * instead, and with them mcumgr over USB.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/nus.h>

#define BUFFER_SIZE 2048
/* what is dropped at once when the buffer is full, so a full buffer stays cheap */
#define DROP_SIZE 64
/* ATT_MTU 247 minus the notification header, which every phone can take */
#define CHUNK_MAX 244

static const struct device *const usb_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

RING_BUF_DECLARE(output, BUFFER_SIZE);
static struct k_spinlock lock;
static atomic_t subscribed;

/*
 * Its own queue: notifications wait there for a free buffer, which would hold
 * up the system work queue, and printing must never wait for Bluetooth.
 */
static K_THREAD_STACK_DEFINE(send_stack, 1024);
static struct k_work_q send_queue;
static void send(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_work, send);

static void smallest_mtu(struct bt_conn *conn, void *data)
{
	uint16_t *mtu = data;

	*mtu = MIN(*mtu, bt_gatt_get_mtu(conn));
}

static void send(struct k_work *work)
{
	uint8_t chunk[CHUNK_MAX];

	ARG_UNUSED(work);
	while (atomic_get(&subscribed)) {
		uint16_t mtu = CHUNK_MAX + 3;
		bt_conn_foreach(BT_CONN_TYPE_LE, smallest_mtu, &mtu);

		k_spinlock_key_t key = k_spin_lock(&lock);
		uint32_t len = ring_buf_peek(&output, chunk, MIN(sizeof(chunk), mtu - 3));
		k_spin_unlock(&lock, key);
		if (len == 0) {
			return;
		}
		int rc = bt_nus_send(NULL, chunk, len);

		/* only taken out now, so a full buffer drops older output rather than this */
		key = k_spin_lock(&lock);
		ring_buf_get(&output, NULL, len);
		k_spin_unlock(&lock, key);
		if (rc) {
			/* the connection went away; what is left waits for the next one */
			return;
		}
	}
}

static int console_out(int c)
{
	uint8_t byte = c;

	if (device_is_ready(usb_console)) {
		if (c == '\n') {
			uart_poll_out(usb_console, '\r');
		}
		uart_poll_out(usb_console, byte);
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ring_buf_space_get(&output) == 0) {
		ring_buf_get(&output, NULL, DROP_SIZE);
	}
	ring_buf_put(&output, &byte, 1);
	k_spin_unlock(&lock, key);

	if (atomic_get(&subscribed)) {
		/* a moment later, so a line goes out in one notification */
		k_work_schedule_for_queue(&send_queue, &send_work, K_MSEC(5));
	}
	return c;
}

static void notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);
	atomic_set(&subscribed, enabled);
	if (enabled) {
		k_work_schedule_for_queue(&send_queue, &send_work, K_NO_WAIT);
	}
}

static struct bt_nus_cb nus_callbacks = {
	.notif_enabled = notif_enabled,
};

static int nus_console_init(void)
{
	k_work_queue_start(&send_queue, send_stack, K_THREAD_STACK_SIZEOF(send_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO - 1, NULL);
	k_thread_name_set(&send_queue.thread, "nus_console");
	bt_nus_cb_register(&nus_callbacks, NULL);

	/* in place of the UART console's hooks, which were installed before */
	__stdout_hook_install(console_out);
	__printk_hook_install(console_out);
	return 0;
}

SYS_INIT(nus_console_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
