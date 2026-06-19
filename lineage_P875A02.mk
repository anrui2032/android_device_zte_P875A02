#
# Copyright (C) 2021-2022 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from the device configuration.
$(call inherit-product, device/zte/P875A02/device.mk)

# Inherit from the Lineage configuration.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

PRODUCT_BRAND := ZTE
PRODUCT_DEVICE := P875A02
PRODUCT_MANUFACTURER := ZTE
PRODUCT_MODEL := ZTE A2022
PRODUCT_NAME := lineage_P875A02

PRODUCT_GMS_CLIENTID_BASE := android-zte

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="CN_P875A02-user 12 SKQ1.220213.001 20221020.234446 release-keys" \
    TARGET_DEVICE=P875A02 \
    TARGET_PRODUCT=CN_P875A02

BUILD_FINGERPRINT := ZTE/CN_P875A02/P875A02:12/SKQ1.220213.001/20221020.234446:user/release-keys
