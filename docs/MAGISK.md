# Magisk for POCO C40

Extract `Magisk-26.3-POCO-C40.zip` from the firmware release and install
`Magisk.v26.3.apk` on the phone. Enable USB debugging and authorize your PC.

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

## Install

```sh
fastboot flash boot_b boot-magisk.img
fastboot reboot
```

## Remove

Return to bootloader fastboot and check the active slot:

```sh
fastboot flash boot_b boot.img
fastboot reboot
```
