# Kernel 1 for POCO C40

Linux 5.4.242 for POCO C40 (`frost`) on MIUI vendor V13.0.17.0.RGFRUXM.
The firmware already includes this kernel.
Extract `boot.img` from the kernel archive.

With an unlocked bootloader and authorized USB debugging:

```sh
adb reboot bootloader
fastboot getvar product
fastboot getvar unlocked
fastboot getvar is-userspace
fastboot getvar current-slot
```

Expect `product: frost`, `unlocked: yes` and `is-userspace: no`.
If `is-userspace` is `yes`, run `fastboot reboot bootloader` and repeat the checks.
For `current-slot: b`, use the commands below. For `a`, use `boot_a` instead of `boot_b`.

```sh
fastboot flash boot_b boot.img
fastboot reboot
```

[Kernel source](https://github.com/rison1337/poco-c40-kernel) ·
[Full flashing guide](https://github.com/rison1337/poco-c40-lineageos-18.1/blob/main/docs/FLASHING.md)
