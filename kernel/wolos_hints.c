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
#include <linux/wolos.h>

static int wolos_fg_tgid;
struct kobject *wolos_kobj;

int wolos_foreground_tgid(void)
{
	return READ_ONCE(wolos_fg_tgid);
}
EXPORT_SYMBOL_GPL(wolos_foreground_tgid);

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

	if (err)
		return err;
	WRITE_ONCE(wolos_fg_tgid, pid > 0 ? pid : 0);
	return n;
}

static struct kobj_attribute abi_version_attr = __ATTR_RO(abi_version);
static struct kobj_attribute foreground_pid_attr = __ATTR_RW_MODE(foreground_pid, 0644);

static struct attribute *wolos_attrs[] = {
	&abi_version_attr.attr,
	&foreground_pid_attr.attr,
	NULL,
};

static const struct attribute_group wolos_group = { .attrs = wolos_attrs };

static int __init wolos_hints_init(void)
{
	wolos_kobj = kobject_create_and_add("wolos", kernel_kobj);
	if (!wolos_kobj)
		return -ENOMEM;
	return sysfs_create_group(wolos_kobj, &wolos_group);
}
subsys_initcall(wolos_hints_init);
