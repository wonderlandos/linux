/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_wolos: WolOS QoS classes on sched_ext (port of the FreeBSD wolos_qos policy).
 * The class is read from what userspace set with sched_setattr(2) (<linux/wolos.h>):
 *   SCHED_IDLE -> background; uclamp_min >= 768 -> interactive; >= 384 -> foreground app;
 *   uclamp_max <= 512 -> utility; else default. EEVDF reads the same nice/uclamp values,
 *   so the classes keep meaning when this scheduler is not loaded.
 * Order: interactive (strict, short slices) > foreground/default/utility (one vtime queue,
 * foreground weighted x4, utility /2) > background (only when idle, or after BG_STARVE_NS).
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

#define DSQ_INTERACTIVE	0
#define DSQ_NORMAL	1
#define DSQ_BG		2
#define SCHED_IDLE_POLICY 5

enum { C_INTERACTIVE, C_FOREGROUND, C_DEFAULT, C_UTILITY, C_BACKGROUND, C_NR };

const volatile u64 slice_interactive_ns = 1000000ULL;	/* 1 ms: keeps input/frames snappy */
const volatile u64 slice_ns = 4000000ULL;		/* 4 ms */
const volatile u64 bg_starve_ns = 100000000ULL;		/* background runs at least every 100 ms */
/* schedutil target per class (0..1024); HWP/EPP drivers decide on their own. */
static const u32 class_perf[C_NR] = { 1024, 1024, 768, 512, 256 };

static u64 vtime_now;
static u64 bg_last_ns;
UEI_DEFINE(uei);

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, C_NR);
} stats SEC(".maps");	/* enqueues per class, read by `scx_wolos --stats` */

static __always_inline int task_class(const struct task_struct *p)
{
	u32 umin, umax;

	if (p->policy == SCHED_IDLE_POLICY)
		return C_BACKGROUND;
	umin = p->uclamp_req[0].value;
	umax = p->uclamp_req[1].value;
	if (umin >= 768)
		return C_INTERACTIVE;
	if (umin >= 384)
		return C_FOREGROUND;
	if (umax <= 512)
		return C_UTILITY;
	return C_DEFAULT;
}

static void stat_inc(u32 c)
{
	u64 *n = bpf_map_lookup_elem(&stats, &c);

	if (n)
		(*n)++;
}

s32 BPF_STRUCT_OPS(wolos_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	if (idle) {
		int c = task_class(p);

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL,
				   c == C_INTERACTIVE ? slice_interactive_ns : slice_ns, 0);
	}
	return cpu;
}

void BPF_STRUCT_OPS(wolos_enqueue, struct task_struct *p, u64 enq_flags)
{
	int c = task_class(p);
	u64 vtime = p->scx.dsq_vtime;

	stat_inc(c);
	if (c == C_INTERACTIVE) {
		scx_bpf_dsq_insert(p, DSQ_INTERACTIVE, slice_interactive_ns, enq_flags);
		/* Kick an idle CPU, else preempt one running something lower. */
		scx_bpf_kick_cpu(scx_bpf_task_cpu(p), SCX_KICK_IDLE);
		return;
	}
	/* An idle task keeps at most one slice of credit. */
	if (time_before(vtime, vtime_now - slice_ns))
		vtime = vtime_now - slice_ns;
	if (c == C_BACKGROUND)
		scx_bpf_dsq_insert_vtime(p, DSQ_BG, slice_ns, vtime, enq_flags);
	else
		scx_bpf_dsq_insert_vtime(p, DSQ_NORMAL, slice_ns, vtime, enq_flags);
}

void BPF_STRUCT_OPS(wolos_dispatch, s32 cpu, struct task_struct *prev)
{
	u64 now = bpf_ktime_get_ns();

	if (now - bg_last_ns > bg_starve_ns && scx_bpf_dsq_move_to_local(DSQ_BG, 0)) {
		bg_last_ns = now;
		return;
	}
	if (scx_bpf_dsq_move_to_local(DSQ_INTERACTIVE, 0) ||
	    scx_bpf_dsq_move_to_local(DSQ_NORMAL, 0))
		return;
	if (scx_bpf_dsq_move_to_local(DSQ_BG, 0))
		bg_last_ns = now;
}

void BPF_STRUCT_OPS(wolos_running, struct task_struct *p)
{
	int c = task_class(p);

	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
	if (c >= 0 && c < C_NR)
		scx_bpf_cpuperf_set(scx_bpf_task_cpu(p), class_perf[c]);
}

void BPF_STRUCT_OPS(wolos_stopping, struct task_struct *p, bool runnable)
{
	int c = task_class(p);
	u64 used = slice_ns > p->scx.slice ? slice_ns - p->scx.slice : 0;
	u64 delta = scale_by_task_weight_inverse(p, used);

	if (c == C_FOREGROUND)
		delta /= 4;
	else if (c == C_UTILITY)
		delta *= 2;
	scx_bpf_task_set_dsq_vtime(p, p->scx.dsq_vtime + delta);
}

void BPF_STRUCT_OPS(wolos_enable, struct task_struct *p)
{
	scx_bpf_task_set_dsq_vtime(p, vtime_now);
}

s32 BPF_STRUCT_OPS_SLEEPABLE(wolos_init)
{
	int ret;

	ret = scx_bpf_create_dsq(DSQ_INTERACTIVE, -1);
	if (!ret)
		ret = scx_bpf_create_dsq(DSQ_NORMAL, -1);
	if (!ret)
		ret = scx_bpf_create_dsq(DSQ_BG, -1);
	if (ret)
		scx_bpf_error("create DSQ failed (%d)", ret);
	return ret;
}

void BPF_STRUCT_OPS(wolos_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(wolos_ops,
	       .select_cpu	= (void *)wolos_select_cpu,
	       .enqueue		= (void *)wolos_enqueue,
	       .dispatch	= (void *)wolos_dispatch,
	       .running		= (void *)wolos_running,
	       .stopping	= (void *)wolos_stopping,
	       .enable		= (void *)wolos_enable,
	       .init		= (void *)wolos_init,
	       .exit		= (void *)wolos_exit,
	       .timeout_ms	= 5000,
	       .name		= "wolos");
