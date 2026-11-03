# Changelog

- Android 11 compatibility for the stock IMS components and Java telephony interfaces.
- Device overlays for the POCO C40 display cutout, status bar and carrier IMS configuration.
- C40 Bluetooth audio policy and a Settings crash when disconnecting a paired device.
- FM cold start and playback service, kernel recording route and MediaStore saving.
- Camera HDR toggle handling.
- Sensor flush completion routed to the correct sensor and a fix for a stuck wake lock.
- CPU cooling control in the kernel driver.
- USB/ADB asynchronous FunctionFS transfers and gadget endpoint alignment,
  fixing a reproduced kernel usercopy panic.
- APR audio port callback removal and reader synchronization to prevent access
  to a freed audio client after a mode change.
- Hardware PNO support for Wi-Fi scanning with the screen off.
- Brightness fallback in Settings, delayed initialization of the 11-bit HAL range,
  and integer/float setting synchronization without a write loop.
- SystemUI notification icon sizing, notification shade, wallpaper and PIN panel
  lifecycle fixes, and TunerService cleanup for destroyed status bar fragments.
- Volume panel startup no longer initializes the status bar from a background thread.
- Demo-mode broadcasts scoped to SystemUI, with the real battery state restored on exit.
- System service boot headers, usage history recovery after an interrupted
  AtomicFile write, and pidfd waiting without a false StrictMode violation.
- Trust/AMS system user IDs and userspace reboot support checks.
- User switching: LiveDisplay confirmation, rejected ExternalStorageService
  binding cleanup and protection against stale callbacks.
- The stock CNE receives only its already requested cross-user permission,
  guarded by device model, APK path and vendor APK hash checks.
- Stock CNE teardown tolerates its unregistered network callback instead of
  crashing the shared vendor process.
- Invalid A/B slot numbers rejected before they can abort the lpdump service.
- Continuous vendor tcpdump and disk logcat recording disabled. Standard Android
  log buffers and bounded DropBox reports are retained.
- Project APK/APEX signing keys, POCO C40 device identity and added microG support.
- PHH Treble Settings and the generic PHH Updater hidden, with compatibility
  services retained. USB debugging requires PC authorization; root ADB is off by default.
- Separate manual procedures for first installation and updates that preserve
  `userdata` and `metadata`.
- Optional Magisk 26.3 using the same kernel, applied or removed by replacing
  only the boot image.
- Secondary-user setup no longer changes the owner's mobile data setting.
- Vendor disk logcat recording stopped through init after credential-encrypted
  storage becomes ready following a clean installation.
- Messaging startup crash fixed when the SIM has no display name.

See [validation](VALIDATION.md) for the completed device checks.
