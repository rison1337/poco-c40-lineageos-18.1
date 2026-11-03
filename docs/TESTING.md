# Testing on the phone

Automated checks run from a PC for a limited test session. The release adds no
continuous log collector, background GPS tracker or automatic report uploader.
Standard bounded Android logs can retain crashes and ANRs, but may miss brief
glitches. Include the time, app name and steps to reproduce when reporting a bug.

Manual checks:

- Add a fingerprint and unlock several times with the screen off.
- Pair Bluetooth earbuds and check music, microphone input in a voice app,
  a phone call and automatic reconnection after reboot.
- Check incoming and outgoing calls, the earpiece and wired headphones.
  Test mobile data with a SIM that has an active data allowance.
- Start an outdoor GPS session and wait for a location fix. The optional test
  app saves only sessions started by the user and does not access the network.
  Omit coordinates from public bug reports.
- Check your apps, moving home screen icons and the brightness display.
  Measure sleep and battery drain with USB power disconnected.
- If used, check microSD and USB OTG reading, writing and reconnection with
  important files backed up.

Results from earlier revisions are identified separately in
[validation](VALIDATION.md); they do not imply that every physical check has
been repeated on each release.
