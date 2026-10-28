// SPDX-License-Identifier: GPL-2.0
/*
 * WolOS hints: /sys/kernel/wolos. The session daemon writes the frontmost
 * application's tgid; drivers (i915 waitboost) favour only that process.
 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kstrtox.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/time64.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/wolos.h>

static int wolos_fg_tgid;
/* Serialises foreground switches so old/new clamp refreshes do not interleave. */
static DEFINE_MUTEX(wolos_fg_lock);

int wolos_foreground_tgid(void)
{
	return READ_ONCE(wolos_fg_tgid);
}
EXPORT_SYMBOL_GPL(wolos_foreground_tgid);

static u64 wolos_bg_slack_ns;

u64 wolos_background_timerslack_ns(void)
{
	return READ_ONCE(wolos_bg_slack_ns);
}

/*
 * uclamp_min floor for every thread of the frontmost app (0 = off, the
 * default). Applied in uclamp_eff_get(), so it stacks with per-thread QoS
 * clamps and never lowers them; cpufreq (schedutil) and EAS see the boost.
 */
static unsigned int wolos_fg_uclamp_min;

unsigned int wolos_foreground_uclamp_min(void)
{
	return READ_ONCE(wolos_fg_uclamp_min);
}

/* Called with wolos_fg_lock held when the frontmost app changes. */
static void wolos_foreground_changed(int old, int new)
{
	wolos_uclamp_refresh_tgid(old);
	wolos_uclamp_refresh_tgid(new);
}

static ssize_t abi_version_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", WOLOS_ABI_VERSION);
}

static ssize_t foreground_pid_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", wolos_foreground_tgid());
}

static ssize_t foreground_pid_store(struct kobject *k, struct kobj_attribute *a,
				    const char *buf, size_t n)
{
	int pid;
	int err = kstrtoint(buf, 10, &pid);

	int old;

	if (err)
		return err;
	pid = pid > 0 ? pid : 0;
	mutex_lock(&wolos_fg_lock);
	old = wolos_fg_tgid;
	WRITE_ONCE(wolos_fg_tgid, pid);
	if (old != pid)
		wolos_foreground_changed(old, pid);
	mutex_unlock(&wolos_fg_lock);
	return n;
}

static ssize_t background_timerslack_ns_show(struct kobject *k, struct kobj_attribute *a,
					     char *buf)
{
	return sysfs_emit(buf, "%llu\n", wolos_background_timerslack_ns());
}

/* Applies to threads that enter SCHED_IDLE after the write. Capped at 1 s. */
static ssize_t background_timerslack_ns_store(struct kobject *k, struct kobj_attribute *a,
					      const char *buf, size_t n)
{
	u64 ns;
	int err = kstrtou64(buf, 10, &ns);

	if (err)
		return err;
	if (ns > NSEC_PER_SEC)
		return -EINVAL;
	WRITE_ONCE(wolos_bg_slack_ns, ns);
	return n;
}

static ssize_t foreground_uclamp_min_show(struct kobject *k, struct kobj_attribute *a,
					  char *buf)
{
	return sysfs_emit(buf, "%u\n", wolos_foreground_uclamp_min());
}

static ssize_t foreground_uclamp_min_store(struct kobject *k, struct kobj_attribute *a,
					   const char *buf, size_t n)
{
	unsigned int v;
	int err = kstrtouint(buf, 10, &v);

	if (err)
		return err;
	if (v > SCHED_CAPACITY_SCALE)
		return -EINVAL;
	mutex_lock(&wolos_fg_lock);
	WRITE_ONCE(wolos_fg_uclamp_min, v);
	wolos_uclamp_refresh_tgid(wolos_fg_tgid);
	mutex_unlock(&wolos_fg_lock);
	return n;
}

static struct kobj_attribute abi_version_attr = __ATTR_RO(abi_version);
static struct kobj_attribute foreground_pid_attr = __ATTR_RW_MODE(foreground_pid, 0644);
static struct kobj_attribute background_timerslack_ns_attr =
	__ATTR_RW_MODE(background_timerslack_ns, 0644);
static struct kobj_attribute foreground_uclamp_min_attr =
	__ATTR_RW_MODE(foreground_uclamp_min, 0644);

static struct attribute *wolos_attrs[] = {
	&abi_version_attr.attr,
	&foreground_pid_attr.attr,
	&background_timerslack_ns_attr.attr,
	&foreground_uclamp_min_attr.attr,
	NULL,
};

static const struct attribute_group wolos_group = { .attrs = wolos_attrs };

static int __init wolos_hints_init(void)
{
	struct kobject *kobj = kobject_create_and_add("wolos", kernel_kobj);

	if (!kobj)
		return -ENOMEM;
	return sysfs_create_group(kobj, &wolos_group);
}
subsys_initcall(wolos_hints_init);
