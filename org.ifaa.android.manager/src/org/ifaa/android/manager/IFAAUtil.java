package org.ifaa.android.manager;

import android.os.SystemProperties;

public class IFAAUtil {
    public static final int IFAA_VERSION_4 = 4;
    public static final int ZTE_IFAA_VERSION = SystemProperties.getInt("ro.vendor.feature.zte_feature_ifaa_version", 4);
    private static Boolean mIsEnableUnderScreenFingerprint = null;

    public static boolean isUnderScreenFingerprint() {
        if (mIsEnableUnderScreenFingerprint == null) {
            mIsEnableUnderScreenFingerprint = Boolean.valueOf(SystemProperties.getBoolean("ro.vendor.feature.zte_underscreen_fingerprint_enable", false));
        }
        return mIsEnableUnderScreenFingerprint.booleanValue();
    }
}
