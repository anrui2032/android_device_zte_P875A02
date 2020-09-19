package org.ifaa.android.manager;

import android.compat.annotation.UnsupportedAppUsage;
import android.content.Context;

public abstract class IFAAManagerV4 extends ZTEIFAAManager {
    @UnsupportedAppUsage
    public abstract int getEnabled(int i);

    @UnsupportedAppUsage
    public abstract int[] getIDList(int i);

    public IFAAManagerV4(Context context) {
        super(context);
    }

    @Override
    public int getVersion() {
        return 4;
    }
}
