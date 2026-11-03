#!/system/bin/sh
# The stock single-user CNE service requests this development permission.
# Grant only the verified frost vendor APK; leave other packages and policies alone.
PATH=/system/bin
export PATH
[ "$(getprop ro.product.vendor.device)" = frost ] || exit 0
[ "$(getprop ro.debuggable)" = 1 ] || exit 0
[ "$(pm path com.qualcomm.qti.cne)" = package:/vendor/app/CneApp/CneApp.apk ] || exit 0
expected=ed8a1c8a24c4205306e2ef155526a46f09d50e5fa934695d7e909974191d041f
actual=$(sha256sum /vendor/app/CneApp/CneApp.apk) || exit 1
[ "${actual%% *}" = "$expected" ] || exit 0
if pm grant --user 0 com.qualcomm.qti.cne android.permission.INTERACT_ACROSS_USERS; then
    log -t c40-cne 'Verified stock CNE cross-user permission applied'
else
    log -p w -t c40-cne 'Failed to apply verified stock CNE cross-user permission'
    exit 1
fi
