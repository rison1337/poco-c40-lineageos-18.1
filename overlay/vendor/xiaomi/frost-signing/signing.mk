# Private keys are supplied by enter-build.sh, never committed here.
PRODUCT_DEFAULT_DEV_CERTIFICATE := vendor/xiaomi/frost-signing/keys/releasekey
PRODUCT_CERTIFICATE_OVERRIDES += \
    com.android.adbd:C40_com_android_adbd_certificate \
    com.android.art.release:C40_com_android_art_release_certificate \
    com.android.cellbroadcast:C40_com_android_cellbroadcast_certificate \
    com.android.conscrypt:C40_com_android_conscrypt_certificate \
    com.android.extservices:C40_com_android_extservices_certificate \
    com.android.i18n:C40_com_android_i18n_certificate \
    com.android.ipsec:C40_com_android_ipsec_certificate \
    com.android.media:C40_com_android_media_certificate \
    com.android.media.swcodec:C40_com_android_media_swcodec_certificate \
    com.android.mediaprovider:C40_com_android_mediaprovider_certificate \
    com.android.neuralnetworks:C40_com_android_neuralnetworks_certificate \
    com.android.os.statsd:C40_com_android_os_statsd_certificate \
    com.android.permission:C40_com_android_permission_certificate \
    com.android.resolv:C40_com_android_resolv_certificate \
    com.android.runtime:C40_com_android_runtime_certificate \
    com.android.sdkext:C40_com_android_sdkext_certificate \
    com.android.tethering:C40_com_android_tethering_certificate \
    com.android.tzdata:C40_com_android_tzdata_certificate \
    com.android.vndk:C40_com_android_vndk_current_certificate \
    com.android.wifi:C40_com_android_wifi_certificate \
    ServiceWifiResources:C40_ServiceWifiResources_certificate \
    OsuLogin:C40_OsuLogin_certificate
