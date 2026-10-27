# WolKernel

WolKernel is the Linux kernel of [WolOS](https://github.com/wonderlandos/wolos), packaged as `linux-wolos`.
It is upstream Linux with a desktop and gaming configuration, a small removed-interface list and a stable module ABI per release.

## Base and rebases

- The `wolos` branch sits on a pinned upstream stable ref (today: 7.3.0, `83f32c7`). `master` mirrors upstream.
- On each stable bump the WolOS commits are rebased onto the new ref, the config is re-run with `make olddefconfig`, and every changed option is reviewed before release.
- WolOS may apply extra patches at package build time.

## Configuration

`kernel/configs/wolos.config` is a fragment over Arch Linux's x86_64 config; `arch/x86/configs/wolos_defconfig` is the full result.
- Latency: full preemption (`PREEMPT`), 1000 Hz, tickless only when idle.
- Scheduling: `sched_ext` with BPF JIT and BTF; when no BPF scheduler is loaded, the stock EEVDF scheduler runs.
- Memory: transparent hugepages on request (`madvise`), MGLRU, zram and zswap with zstd. PSI on.
- Network: BBR congestion control by default.
- Not built: datacenter-only hardware (InfiniBand, Fibre Channel, storage targets, legacy RAID cards, telecom links, cluster filesystems).

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
