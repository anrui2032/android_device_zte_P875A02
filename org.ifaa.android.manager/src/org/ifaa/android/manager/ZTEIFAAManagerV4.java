package org.ifaa.android.manager;

import android.content.Context;
import android.util.Log;

public class ZTEIFAAManagerV4 extends IFAAManagerV4 {
    public static final int BIO_TYPE_ENABLE = 1000;
    public static final int BIO_TYPE_UNLOCK = 1001;

    public ZTEIFAAManagerV4(Context context) {
        super(context);
    }

    @Override
    public int[] getIDList(int bioType) {
        if (bioType == 1) {
            try {
                Log.i(ZTEIFAAManager.TAG, "getIDlist enter");
                return mAlipayBinder.getIdList();
            } catch (Exception e) {
                Log.e(ZTEIFAAManager.TAG, "e:" + e.fillInStackTrace());
                return null;
            }
        }
        return null;
    }

    @Override
    public int getEnabled(int bioType) {
        if (bioType == 1) {
            return 1000;
        }
        return 1001;
    }
}
