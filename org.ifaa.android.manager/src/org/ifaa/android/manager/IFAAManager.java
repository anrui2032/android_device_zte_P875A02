package org.ifaa.android.manager;

import android.annotation.UnsupportedAppUsage;
import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.util.Log;
import com.zte.alipay.service.IAlipayService;

public abstract class IFAAManager {
    public static final String ACTION_NAME = "com.zte.vendor.ifaa.IAlipayService";
    public static final String Alipay_CLASS_NAME = "com.zte.vendor.ifaa.AlipayService";
    public static final String Alipay_PACKAGE_NAME = "com.zte.vendor.ifaa";
    private static final String TAG = "IFAAManager";
    public static IAlipayService mAlipayBinder = null;
    public ComponentName alipayCompName = new ComponentName(Alipay_PACKAGE_NAME, Alipay_CLASS_NAME);
    private final IBinder.DeathRecipient deadListen = new IBinder.DeathRecipient() {
        @Override
        public void binderDied() {
            IFAAManager.mAlipayBinder = null;
        }
    };
    public final ServiceConnection alipayServiceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            if (service != null) {
                IFAAManager.mAlipayBinder = IAlipayService.Stub.asInterface(service);
                try {
                    service.linkToDeath(IFAAManager.this.deadListen, 0);
                } catch (Exception e) {
                    Log.e(IFAAManager.TAG, " linkToDeath error  ", e);
                }
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
        }
    };

    @UnsupportedAppUsage
    public abstract String getDeviceModel();

    @UnsupportedAppUsage
    public abstract int getSupportBIOTypes(Context context);

    @UnsupportedAppUsage
    public abstract int getVersion();

    @UnsupportedAppUsage
    public abstract int startBIOManager(Context context, int i);

    @UnsupportedAppUsage
    public byte[] processCmd(Context context, byte[] param) {
        byte[] response = null;
        try {
            Log.w(TAG, "process cmd invokeCmd start");
            response = mAlipayBinder.startCmd(param);
            Log.w(TAG, "processCmd() invokeCmd end");
            return response;
        } catch (Exception e) {
            Log.e(TAG, "process cmd invokeCmd Exception:" + e.fillInStackTrace());
            return response;
        }
    }

    @UnsupportedAppUsage
    public static byte[] processCmdBinder(Context context, byte[] param) {
        byte[] response = null;
        try {
            Log.w(TAG, "process cmd binder() start");
            response = mAlipayBinder.startCmd(param);
            Log.w(TAG, "processCmdBinder() invokeCmd end");
            return response;
        } catch (Exception e) {
            Log.e(TAG, "process cmd binder() java error:" + e.fillInStackTrace());
            return response;
        }
    }
}
