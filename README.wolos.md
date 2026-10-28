# WolKernel

WolKernel is the Linux kernel of [WolOS](https://github.com/wonderlandos/wolos), packaged as `linux-wolos`.
It is upstream Linux with a small set of desktop and gaming patches, a tuned configuration, a QoS interface for the WolOS session, a short removed-interface list and a stable module ABI per release.

## Base and rebases

- The `wolos` branch sits on a pinned upstream stable ref (today: 7.3.0, `83f32c7`). `master` mirrors upstream.
- On each stable bump the WolOS commits are rebased onto the new ref, the config is re-run with `make olddefconfig`, and every changed option is reviewed before release.
- Each WolOS change is one small commit with a runtime off switch, so a rebase stays cheap. A change that upstream gains is dropped.

## Configuration

`kernel/configs/wolos.config` is a fragment over Arch Linux's x86_64 config; `arch/x86/configs/wolos_defconfig` is the full result.
- Latency: full preemption (`PREEMPT`), 1000 Hz, tickless only when idle.
- Scheduling: `sched_ext` with BPF JIT and BTF; when no BPF scheduler is loaded, the stock EEVDF scheduler runs.
- Memory: transparent hugepages on request (`madvise`), MGLRU, zram and zswap with zstd. PSI on.
- Network: BBRv3 congestion control and fq by default, MPTCP and WireGuard built in.
- Not built: datacenter-only hardware (InfiniBand, Fibre Channel, storage targets, legacy RAID cards, telecom links, cluster filesystems).

## Patches

| commit | what | off switch |
|---|---|---|
| futex: bounded spin before sleeping in FUTEX_WAIT | A waiter spins up to `futex.spin_ns` (default 4000 ns) before sleeping; a wake during the spin skips the sleep and the wakeup IPI. Most lock hand-offs between game threads finish within microseconds. Stops early on `need_resched` or a signal. | `futex.spin_ns=0` (boot) or `/sys/module/futex/parameters/spin_ns` |
| wolos: /sys/kernel/wolos hint interface | `abi_version` (read) and `foreground_pid` (root writes the frontmost app's tgid, 0 = none); `wolos_foreground_tgid()` for drivers; `<linux/wolos.h>` QoS class encoding. `CONFIG_WOLOS_HINTS`. | `CONFIG_WOLOS_HINTS=n` |
| drm/i915: frame-deadline boost, foreground waitboost | i915 fences implement `.set_deadline`: when the compositor's atomic commit (or `SYNC_IOC_SET_DEADLINE`) sets the next vblank as deadline, a request still running is boosted at once instead of waiting for RPS autotuning. Waitboost on a blocking wait applies only to the frontmost app, so background GPU work stops raising clocks. | `i915.wol_deadline_boost=0`, `i915.wol_waitboost=1` (upstream behaviour) or `0` |
| wolos: timer slack for background QoS threads | Threads entering `SCHED_IDLE` (QoS background) get at least `background_timerslack_ns` of timer slack, so their wakeups coalesce. ABI version 2. | write `0` to `/sys/kernel/wolos/background_timerslack_ns` (default) |
| wolos: legacy PTYs, x32, vsyscall emulation, legacy fbcon acceleration depend on BROKEN | Unused interfaces cannot be built; vsyscall default is none. IA32, modify_ldt and fbcon on DRM stay. | `vsyscall=xonly` (boot); revert the commit for the rest |
| tcp/bbr3 (google/bbr v3 via CachyOS) | BBRv3 as `bbr3`, the default congestion control; upstream `bbr` stays loadable. | `sysctl net.ipv4.tcp_congestion_control=cubic`, `CONFIG_TCP_CONG_BBR3=n` |
| CachyOS: CONFIG_CACHY, EEVDF interactivity, MGLRU min_ttl, no watermark boost, no proactive compaction, split-lock throttle off | Desktop defaults from CachyOS (stand in for Zen's `ZEN_INTERACTIVE`; MGLRU `min_ttl` replaces le9). | `CONFIG_CACHY=n`; at runtime `/sys/kernel/mm/lru_gen/min_ttl_ms`, `vm.watermark_boost_factor`, `vm.compaction_proactiveness`, `split_lock_detect=warn` |
| Kconfig: CC_OPTIMIZE_FOR_PERFORMANCE_O3 (CachyOS) | `-O3` choice; not selected in `wolos.config`. | choose `-O2` (default) |
| PCI: ACS override (CachyOS) | Splits IOMMU groups for GPU passthrough on consumer boards. Opt-in. | absent `pcie_acs_override=` (default) |
| ADIOS 3.3.0 I/O scheduler (CachyOS) | Adaptive-deadline MQ scheduler, default for blk-mq devices. | `echo mq-deadline > /sys/block/<dev>/queue/scheduler`, `CONFIG_MQ_IOSCHED_ADIOS=n` |
| sched: BORE (CachyOS bore-cachy) | Burst-aware weighting on top of EEVDF; also marks futex waiters. | `sysctl kernel.sched_bore=0`, `CONFIG_SCHED_BORE=n` |
| mm/swap: configurable default swap readahead | `vm.page-cluster` boot default from `CONFIG_SWAP_PAGE_CLUSTER_DEFAULT`, 0 (one page) with `CONFIG_CACHY`: on zram each read-ahead page is a decompression the faulting task waits for (CachyOS ships the same value as a sysctl). | `sysctl vm.page-cluster=3` |
| selftests/wolos: futex_pingpong | FUTEX_WAIT/WAKE hand-off round trip, cross- and same-CPU; measures `futex.spin_ns`. Stock 7.0 baseline: 2.5 us cross-CPU, 1.3 us same-CPU. | n/a (test) |
| drm/amd: AMD_PRIVATE_COLOR in Kconfig (CachyOS) | Plane color/HDR properties for gamescope. | `CONFIG_AMD_PRIVATE_COLOR=n` |

Already upstream and used as is: `futex_waitv`, ntsync, PSI triggers and the cgroup v2 freezer (WolOS freezes background apps under memory pressure with them), sched_ext.

## Rust port of /sys/kernel/wolos (plan)

Status per part: (1) blocked, no kobject/sysfs-attr bindings; until it lands the interface stays in C. (2)-(4) depend on (1). Scheduler core, futex and mm changes stay C (no Rust bindings there).
At 7.3 the Rust bindings have no kobject or sysfs-attribute abstraction (`rust/kernel/` exposes sysfs only through device and faux helpers), so the hint interface stays in C for now. Plan, behind `CONFIG_WOLOS_HINTS_RUST` (default n, `depends on RUST && !WOLOS_HINTS`):
1. `rust/kernel/kobject.rs`: `KObject::create_and_add(name, parent)` and a `#[vtable]` `KobjAttr` trait (`show`/`store`) over `kobj_attribute`, upstreamable on its own.
2. `kernel/wolos_hints.rs`: `abi_version`, `foreground_pid`, `background_timerslack_ns` with the same modes and text format; state in `AtomicI32`/`AtomicU64`.
3. `kernel/wolos_hints_shim.c`: `wolos_foreground_tgid()` and `wolos_background_timerslack_ns()` stay C exports (`EXPORT_SYMBOL_GPL`) reading `#[no_mangle]` statics, so i915 and the scheduler keep the same symbols and KMI.
4. KUnit (`#[kunit_tests]`): store/show round trip, negative pid clamps to 0, slack above 1 s is `-EINVAL`.
CI turns it on once both builds pass the same sysfs test; then the C file goes.

## Left to userspace

Image items: `vm.swappiness=180` with zram; zram-generator `compression-algorithm = lz4 (type=idle) zstd(level=3)` for recompression, `writeback-device` on a swap partition if present; btrfs fstab options below.
btrfs: no carried patches. `space_cache=v2` and `discard=async` (on SSDs with discard) are upstream defaults since 5.15/6.2; per-subvolume read-policy balancing needs `BTRFS_EXPERIMENTAL` and stays off.

`vm.max_map_count=1048576`, TCP fastopen (`net.ipv4.tcp_fastopen=3`) and larger socket buffers are sysctls (image). App-nap style freezing uses the cgroup v2 freezer from the session daemon. `background_timerslack_ns` is written by the session daemon.

## QoS interface

No new syscall. A QoS class is applied per thread with `sched_setattr(2)` (constants in `<linux/wolos.h>`):

| class | policy | nice | uclamp min..max |
|---|---|---|---|
| 0 `WOLOS_QOS_USER_INTERACTIVE` (compositor, shell, input) | SCHED_OTHER | -10 | 768..1024 |
| 1 `WOLOS_QOS_USER_INITIATED` (frontmost app) | SCHED_OTHER | -5 | 384..1024 |
| 2 `WOLOS_QOS_DEFAULT` | SCHED_OTHER | 0 | 0..1024 |
| 3 `WOLOS_QOS_UTILITY` | SCHED_OTHER | 10 | 0..512 |
| 4 `WOLOS_QOS_BACKGROUND` | SCHED_IDLE | 19 | 0..256 |

EEVDF honours the same nice/uclamp values, so classes keep their meaning with or without a BPF scheduler.
`/sys/kernel/wolos/foreground_pid` feeds the i915 foreground waitboost. `abi_version` increments on any change; fields are never repurposed.

## scx_wolos

A sched_ext scheduler in `tools/sched_ext/scx_wolos.bpf.c` that reads the QoS class above:
interactive tasks get a strict-priority queue with 1 ms slices and kick an idle CPU; frontmost, default and utility tasks are vtime-ordered (frontmost x4, utility /2) on the per-CPU queue of the CPU they last ran on; an idle CPU drains its own queue, then steals from CPUs on its LLC, then from any CPU; background runs when nothing else is runnable, and at least every 100 ms.
Slices follow the class (interactive 1 ms, frontmost 2 ms, rest 4 ms). The loader copies `/sys/kernel/wolos/foreground_pid` in every second, so the frontmost app is boosted even without a QoS class, and fills the CPU-to-LLC map from sysfs. `-g` uses one global queue (A/B); `-s` also prints local / same-LLC / cross-LLC dispatches. While a task runs, its class sets the CPU performance target for schedutil.
If it exits, stalls (5 s watchdog) or is killed, the kernel moves every task back to EEVDF.

```sh
make -C tools/sched_ext VMLINUX_BTF=<O>/vmlinux scx_wolos   # needs clang, bpftool, libbpf; BTF of a WolKernel build
sudo tools/sched_ext/build/bin/scx_wolos -s   # -s prints enqueues per class each second
cat /sys/kernel/sched_ext/root/ops            # "wolos" while attached
```

`tools/sched_ext/wolos/scx_wolos.service` runs it under systemd (restart on failure, at most 5 times a minute).

## Compatibility promise

Anything that runs on Arch Linux's stock kernel runs on WolKernel: 32-bit software and Steam, Wine and Proton, Flatpak, AppImage and Snap, Docker and Podman, KVM, QEMU and VirtualBox, browser sandboxes, and DKMS modules such as NVIDIA, ZFS and v4l2loopback.

Removed interfaces ([`Documentation/wolos/abi-removed.txt`](Documentation/wolos/abi-removed.txt)):
- `X86_X32_ABI`: the x32 ABI. No x32 software ships in Arch, Flatpak runtimes or container images.

## Stable module ABI (KMI)

`CONFIG_MODVERSIONS` is on. Within a release line (one kernel major version) exported symbols and their CRCs do not change, so out-of-tree modules built once keep loading. A new line may change them; DKMS rebuilds once.

## Versioning

`uname -r` ends in `-wolos`. `uname -v` names the WolOS base release (`WOLOS-<release>`), the same release as `/etc/os-release`: kernel, UAPI headers (`linux-wolos-api-headers`) and base userland ship together.

## Build

```sh
make wolos_defconfig            # or: make <your config>; make wolos.config
make -j"$(nproc)"
```

The release package is built by `kernel/linux-wolos/PKGBUILD` in [wonderlandos/wolos](https://github.com/wonderlandos/wolos).

## Publishing

Release kernels are published in the `[wolos]` pacman repository next to the pinned Arch Linux snapshot. Other kernels can be installed with pacman; `linux-wolos` stays the default boot entry.

## License

GPL-2.0, the same as Linux (see `COPYING`).
