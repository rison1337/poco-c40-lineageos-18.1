# LineageOS 18.1 for POCO C40

Android 11 · R2 · Linux 5.4.242. PHH Treble patches and a custom kernel.

[Download](https://github.com/rison1337/poco-c40-lineageos-18.1/releases/latest) ·
[Kernel](https://github.com/rison1337/poco-c40-kernel/releases/latest) ·
[Magisk](docs/MAGISK.md)

The firmware ZIP includes the kernel. Individual images are also attached to the release.

## Installation

For POCO C40 (`frost`) with an unlocked bootloader and MIUI base
**V13.0.17.0.RGFRUXM (RU)**. Stock vendor is retained; other bases have not been tested.
Extract `system.img`, `boot.img` and `vbmeta.img`.

Install [Platform-Tools](https://developer.android.com/tools/releases/platform-tools)
and open a terminal in the image directory. On Windows, use the Android
Bootloader Interface driver. In PowerShell, use `./adb.exe` and `./fastboot.exe`
for tools in the current directory.

## Preparation

Enable USB debugging and authorize your PC:

```sh
adb shell getprop ro.product.vendor.device
adb shell getprop ro.vendor.build.version.incremental
adb reboot bootloader
```

The first two results must be `frost` and `V13.0.17.0.RGFRUXM`.

```sh
fastboot getvar product
fastboot getvar unlocked
fastboot getvar is-userspace
fastboot getvar current-slot
```

Expect `product: frost`, `unlocked: yes` and `is-userspace: no`.
If `is-userspace` is `yes`, run `fastboot reboot bootloader` and repeat the checks.

- If `current-slot` is `b`, use the commands below as written.
- If it is `a`, use `boot_a`, `vbmeta_a`, `system_a` and `fastboot set_active a`
  instead of the slot `b` versions.

## First installation

**Apps and files in internal storage will be erased.**
After preparation, in bootloader fastboot:

```sh
fastboot --disable-verity flash vbmeta_b vbmeta.img
fastboot flash boot_b boot.img
fastboot reboot fastboot
```

Wait for fastbootd and check:

```sh
fastboot getvar product
fastboot getvar current-slot
fastboot getvar is-userspace
fastboot getvar snapshot-update-status
fastboot getvar is-logical:system_b
```

Expect `frost`, the same slot, `yes`, `none`, and `yes`.
Finish any pending snapshot update before continuing.

```sh
fastboot -S 4M flash system_b system.img
```

**Only after the entire system image was written successfully**, erase data:

```sh
fastboot erase userdata
fastboot erase metadata
fastboot set_active b
fastboot reboot
```

## Update without losing data

For this LineageOS series. Extract the new archive and repeat preparation
and slot checks. In bootloader fastboot:

```sh
fastboot flash boot_b boot.img
fastboot reboot fastboot
```

Repeat **only the fastbootd checks** from the first-install section, then:

```sh
fastboot -S 4M flash system_b system.img
fastboot set_active b
fastboot reboot
```

**Do not erase `userdata` or `metadata`, and do not flash `vbmeta`.**

## Kernel and Magisk

[Kernel instructions](docs/KERNEL.md) · [Magisk instructions](docs/MAGISK.md)

## Source and testing

[Kernel source](https://github.com/rison1337/poco-c40-kernel) ·
[Changelog](docs/CHANGELOG.md) · [Validation](docs/VALIDATION.md) ·
[Testing](docs/TESTING.md) · [Build instructions](docs/BUILD.md)

If fastboot reports `Couldn't download data`, stay in fastbootd. Run
`fastboot reboot fastboot`, repeat the fastbootd checks, then flash
`system` again. Reboot Android only after the entire write succeeds.
