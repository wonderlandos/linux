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
- Network: BBR congestion control by default.
- Not built: datacenter-only hardware (InfiniBand, Fibre Channel, storage targets, legacy RAID cards, telecom links, cluster filesystems).

## Patches

| commit | what | off switch |
|---|---|---|
| futex: bounded spin before sleeping in FUTEX_WAIT | A waiter spins up to `futex.spin_ns` (default 4000 ns) before sleeping; a wake during the spin skips the sleep and the wakeup IPI. Most lock hand-offs between game threads finish within microseconds. Stops early on `need_resched` or a signal. | `futex.spin_ns=0` (boot) or `/sys/module/futex/parameters/spin_ns` |
| wolos: /sys/kernel/wolos hint interface | `abi_version` (read) and `foreground_pid` (root writes the frontmost app's tgid, 0 = none); `wolos_foreground_tgid()` for drivers; `<linux/wolos.h>` QoS class encoding. `CONFIG_WOLOS_HINTS`. | `CONFIG_WOLOS_HINTS=n` |
| drm/i915: frame-deadline boost, foreground waitboost | i915 fences implement `.set_deadline`: when the compositor's atomic commit (or `SYNC_IOC_SET_DEADLINE`) sets the next vblank as deadline, a request still running is boosted at once instead of waiting for RPS autotuning. Waitboost on a blocking wait applies only to the frontmost app, so background GPU work stops raising clocks. | `i915.wol_deadline_boost=0`, `i915.wol_waitboost=1` (upstream behaviour) or `0` |

Already upstream and used as is: `futex_waitv`, ntsync, PSI triggers and the cgroup v2 freezer (WolOS freezes background apps under memory pressure with them), sched_ext.

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
interactive tasks get a strict-priority queue with 1 ms slices and kick an idle CPU; frontmost, default and utility tasks share a weighted virtual-time queue (frontmost x4, utility /2); background runs when nothing else is runnable, and at least every 100 ms. While a task runs, its class sets the CPU performance target for schedutil.
If it exits, stalls (5 s watchdog) or is killed, the kernel moves every task back to EEVDF.

```sh
make -C tools/sched_ext scx_wolos         # needs clang, bpftool, libbpf; BTF from the running kernel
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
