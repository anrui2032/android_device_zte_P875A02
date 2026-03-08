package org.ifaa.android.manager;

import android.content.Context;
import android.util.Log;

public class IFAAManagerFactory {
    private static final String TAG = "IFAAManagerInstance";
    private static IFAAManager ifaaManager;

    public static IFAAManager getIFAAManager(Context context, int authType) {
        if (ifaaManager == null) {
            if (IFAAUtil.ZTE_IFAA_VERSION == 4) {
                Log.i(TAG, "the current version supports ifaa 4.0");
                ifaaManager = new ZTEIFAAManagerV4(context);
            } else {
                Log.i(TAG, "the current version does not support ifaa 4.0");
                ifaaManager = new ZTEIFAAManager(context);
            }
        }
        return ifaaManager;
    }
}
