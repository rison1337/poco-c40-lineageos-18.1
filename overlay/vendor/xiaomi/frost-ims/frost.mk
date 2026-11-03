# POCO C40 distribution profile; no personal keys or network credentials.
PRODUCT_PACKAGES += C40Ims C40ImsTelephonyOverlay c40_device_init c40_release_components c40_cne_default_permissions qti-telephony-hidl-wrapper qti-telephony-utils ims-ext-common qti_telephony_hidl_wrapper.xml qti_telephony_utils.xml ims_ext_common.xml
BOARD_PLAT_PUBLIC_SEPOLICY_DIR += vendor/xiaomi/frost-ims/sepolicy/public
BOARD_PLAT_PRIVATE_SEPOLICY_DIR += vendor/xiaomi/frost-ims/sepolicy/private
PRODUCT_SYSTEM_DEFAULT_PROPERTIES += ro.adb.root_default=0
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += vendor/xiaomi/frost-ims/vintf/compatibility_matrix.xml

PRODUCT_PACKAGES += c40_sensors_hub_flush_fix
PRODUCT_PACKAGES += C40WifiOverlay
PRODUCT_PACKAGES += c40_cne_cross_user_init c40_cne_cross_user_permission

# Keep rear fingerprint touches from acting as keyboard navigation.
PRODUCT_COPY_FILES += \
    vendor/xiaomi/frost-ims/keylayout/Vendor_0666_Product_0888.kl:$(TARGET_COPY_OUT_SYSTEM)/usr/keylayout/Vendor_0666_Product_0888.kl
