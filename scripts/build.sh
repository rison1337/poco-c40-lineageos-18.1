#!/usr/bin/env bash
set -eo pipefail
: "${C40_SRC:?Set C40_SRC to the absolute patched Android checkout}"
: "${C40_KEYS:?Set C40_KEYS to your private signing directory}"
cd "$C40_SRC"
test -f "$C40_KEYS/releasekey.pk8"
if [[ -e vendor/xiaomi/frost-signing/keys || -L vendor/xiaomi/frost-signing/keys ]]; then
    [[ $(readlink -f vendor/xiaomi/frost-signing/keys) == "$(readlink -f "$C40_KEYS")" ]]
else
    ln -s "$C40_KEYS" vendor/xiaomi/frost-signing/keys
fi
source build/envsetup.sh
lunch lineage_arm64_bvN-userdebug
m -j"${C40_JOBS:-6}" systemimage
printf '%s\n' 'Built system.img. Static verification and phone tests are still required.'
