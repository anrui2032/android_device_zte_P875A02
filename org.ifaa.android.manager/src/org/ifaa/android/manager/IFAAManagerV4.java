package org.ifaa.android.manager;

import android.content.Context;

public abstract class IFAAManagerV4 extends ZTEIFAAManager {
    public abstract int getEnabled(int i);

    public abstract int[] getIDList(int i);

    public IFAAManagerV4(Context context) {
        super(context);
    }

    @Override
    public int getVersion() {
        return 4;
    }
}
