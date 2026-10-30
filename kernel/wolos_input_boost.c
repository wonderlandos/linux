// SPDX-License-Identifier: GPL-2.0
/*
 * WolOS input boost: the first frame after the user touches the keyboard,
 * mouse, touchpad or a gamepad should not run at idle clocks. Each input
 * event (rate-limited to one per 50 ms) opens a boost window of
 * wolos_input_boost.ms milliseconds during which schedutil keeps every CPU at
 * least at wolos_input_boost.floor_pct percent of its capacity, and GPU drivers
 * on wolos_input_boost_chain get a call. ms=0 turns it off.
 */
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/wolos.h>
#include <linux/wolos_policy.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "wolos_input_boost."

static unsigned int boost_ms = 100;
module_param_named(ms, boost_ms, uint, 0644);
MODULE_PARM_DESC(ms, "Boost window after an input event in ms (0 = off)");

static unsigned int floor_pct = 60;
module_param_named(floor_pct, floor_pct, uint, 0644);
MODULE_PARM_DESC(floor_pct, "CPU utilization floor during the window, percent of capacity");

#define WIB_GAP_MS 50

static unsigned long boost_until;
static unsigned long last_fire;

ATOMIC_NOTIFIER_HEAD(wolos_input_boost_chain);
EXPORT_SYMBOL_GPL(wolos_input_boost_chain);

unsigned long wolos_input_boost_util(unsigned long cap)
{
	return wolos_boost_floor(jiffies, READ_ONCE(boost_until), cap,
				 READ_ONCE(floor_pct));
}

static void wib_event(struct input_handle *handle, unsigned int type,
		      unsigned int code, int value)
{
	unsigned long now = jiffies, ms = READ_ONCE(boost_ms);

	if (!ms || (type != EV_KEY && type != EV_REL && type != EV_ABS))
		return;
	if (!wolos_boost_should_fire(now, READ_ONCE(last_fire),
				     msecs_to_jiffies(WIB_GAP_MS)))
		return;
	WRITE_ONCE(last_fire, now);
	WRITE_ONCE(boost_until, now + msecs_to_jiffies(ms));
	atomic_notifier_call_chain(&wolos_input_boost_chain, ms, NULL);
}

static int wib_connect(struct input_handler *handler, struct input_dev *dev,
		       const struct input_device_id *id)
{
	struct input_handle *handle;
	int err;

	handle = kzalloc_obj(*handle);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "wolos_input_boost";
	err = input_register_handle(handle);
	if (err)
		goto free;
	err = input_open_device(handle);
	if (err)
		goto unregister;
	return 0;
unregister:
	input_unregister_handle(handle);
free:
	kfree(handle);
	return err;
}

static void wib_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/*
 * Devices with keys or buttons only: keyboards, mice, touchpads, gamepads.
 * Sensors that stream EV_ABS without buttons (accelerometers) never boost.
 */
static const struct input_device_id wib_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = { BIT_MASK(EV_KEY) } },
	{ }
};

static struct input_handler wib_handler = {
	.event		= wib_event,
	.connect	= wib_connect,
	.disconnect	= wib_disconnect,
	.name		= "wolos_input_boost",
	.id_table	= wib_ids,
};

static int __init wolos_input_boost_init(void)
{
	last_fire = jiffies - msecs_to_jiffies(WIB_GAP_MS);
	boost_until = jiffies;
	return input_register_handler(&wib_handler);
}
late_initcall(wolos_input_boost_init);
