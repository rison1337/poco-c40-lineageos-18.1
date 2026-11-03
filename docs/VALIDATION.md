# Validation

Verified working during development: GPS with a real satellite fix, Wi-Fi,
cellular calls and mobile data, Bluetooth audio and microphones, wired audio,
cameras and HDR, FM playback and recording, fingerprint unlocking, sensors,
screen rotation, vibration and USB. Sleep/suspend was also checked with USB
power disconnected.

The release images were tested on a physical POCO C40: boot, an update preserving user data and
hardware-backed keys, verification of 95 system files, the Messaging startup
fix and SystemUI operation. Earbud pairing, fingerprints, apps and settings
were retained.

The owner confirmed fingerprint unlocking, earbud audio and microphones.

See [test procedures](TESTING.md). Detailed working logs are kept locally.

The rear fingerprint navigation regression was reproduced with short taps and
a long press, then checked with all 12 advertised sensor key codes. After
the fix, physical touches produced no selection or long-press actions, and
fingerprint unlocking succeeded. The R2 image booted with the corrected
key layout and all 95 checked system files matched the build.
