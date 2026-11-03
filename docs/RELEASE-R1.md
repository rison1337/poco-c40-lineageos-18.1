# LineageOS 18.1 · POCO C40 · R1

Android 11 · Linux 5.4.242 · PHH Treble patches and a custom kernel.

For POCO C40 (`frost`) with an unlocked bootloader and MIUI base
**V13.0.17.0.RGFRUXM (RU)**. Vendor is retained; other bases have not been tested.

Flash `boot` in bootloader fastboot and `system` in fastbootd with 4 MiB chunks.
The first installation erases user data; updates of this series preserve it.
The kernel is included in the firmware. Magisk is in a separate archive.

Bluetooth, FM recording, HDR, sensors, USB, brightness, SystemUI and Messaging fixes.
GPS, calls and mobile data, Wi-Fi, Bluetooth, cameras, FM, fingerprint unlocking,
sensors and sleep/suspend were checked during development.

[Installation](../README.md#installation) · [Changelog](CHANGELOG.md) ·
[Validation](VALIDATION.md) · [Kernel](https://github.com/rison1337/poco-c40-kernel).
