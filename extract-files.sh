#!/bin/bash
#
# SPDX-FileCopyrightText: 2016 The CyanogenMod Project
# SPDX-FileCopyrightText: 2017-2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=P875A02
VENDOR=zte

# Load extract utilities and do some sanity checks.
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "${MY_DIR}" ]]; then MY_DIR="${PWD}"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

export TARGET_ENABLE_CHECKELF=true

# If XML files don't have comments before the XML header, use this flag
# Can still be used with broken XML files by using blob_fixup
export TARGET_DISABLE_XML_FIXING=true

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

# Default to sanitizing the vendor folder before extraction.
CLEAN_VENDOR=true

KANG=
SECTION=

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        -n | --no-cleanup)
            CLEAN_VENDOR=false
            ;;
        -k | --kang)
            KANG="--kang"
            ;;
        -s | --section)
            SECTION="${2}"
            shift
            CLEAN_VENDOR=false
            ;;
        *)
            SRC="${1}"
            ;;
    esac
    shift
done

if [ -z "${SRC}" ]; then
    SRC="adb"
fi

function blob_fixup() {
    case "${1}" in
    vendor/etc/media_codecs.xml)
    [ "$2" = "" ] && return 0
        sed -Ei "/media_codecs_(google_audio|google_telephony|google_video|vendor_audio)/d" "${2}"
        sed -i "/media_codecs_google_c2/d" "${2}"
        ;;
    vendor/etc/media_codecs_lahaina_vendor.xml)
    [ "$2" = "" ] && return 0
        sed -Ei "/media_codecs_(google_audio|google_telephony|google_video|vendor_audio)/d" "${2}"
        sed -i "/media_codecs_google_c2/d" "${2}"
        ;;
    vendor/etc/wifi/wlan/WCNSS_qcom_cfg.ini)
    [ "$2" = "" ] && return 0
        sed -i '/^END$/i read_mac_addr_from_mac_file=1' "${2}"
        ;;
    vendor/lib64/libFNVfbEngineHAL.so)
    [ "$2" = "" ] && return 0
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_allocate" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_describe" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_lock" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_release" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_unlock" "${2}"
        ;;
    vendor/lib64/libZEffectLib.so)
    [ "$2" = "" ] && return 0
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_allocate" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_describe" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_lock" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_lockPlanes" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_release" "${2}"
        "${PATCHELF_0_17_2}" --clear-symbol-version "AHardwareBuffer_unlock" "${2}"
        ;;
    # Change soname for fingerprint.gf95xx.default.so.
    vendor/lib64/hw/fingerprint.lahaina.so)
    [ "$2" = "" ] && return 0
        sed -i 's/\x00libfingerprint.default.so\x00/\x00fingerprint.lahaina.so\x00\x00\x00\x00/' "${2}"
        sed -i 's/\x00fingerprint.gf95xx\x00/\x00fingerprint\x00\x00\x00\x00\x00\x00\x00\x00/' "${2}"
        ;;
    esac

    return 0
}

function blob_fixup_dry() {
    blob_fixup "$1" ""
}

# Initialize the helper.
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}" false "${CLEAN_VENDOR}"

extract "${MY_DIR}/proprietary-files.txt" "${SRC}" "${KANG}" --section "${SECTION}"

"${MY_DIR}/setup-makefiles.sh"
