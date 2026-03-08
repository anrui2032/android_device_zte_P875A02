package org.ifaa.android.manager;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.SystemProperties;
import android.util.Log;
import org.json.JSONObject;

public class ZTEIFAAManager extends IFAAManagerV3 {
    public static final int BIO_TYPE_ENABLE = 1000;
    public static final int BIO_TYPE_UNLOCK = 1001;
    public static final String TAG = "ZTEIFAAManager";
    public static final int TYPE_FACE = 4;
    public static final int TYPE_FINGERPRINT = 1;
    public static final int TYPE_IRIS = 2;
    public static final int TYPE_UNDERSCREEN_FINGERPRINT = 17;
    private static final int VERSION_1 = 1;
    private static final int VERSION_2 = 2;
    private static final int VERSION_3 = 3;
    private static final int VERSION_4 = 4;
    private String mAlipayDeviceModel = null;
    private int mFingerprintSensoruiPositionCenterX = 0;
    private int mFingerprintSensoruiPositionCenterY = 0;
    private int mFingerprintSensoruiPositionCenterR = 0;

    public ZTEIFAAManager(Context context) {
        Intent intent = new Intent();
        intent.setAction(IFAAManager.ACTION_NAME);
        intent.setComponent(this.alipayCompName);
        Log.i(TAG, "ZTEIFAAManager will bindServiceAsUser");
        context.bindService(intent, this.alipayServiceConnection, 1);
        try {
            Thread.sleep(100L);
        } catch (Exception e) {
        }
    }

    @Override
    public int getSupportBIOTypes(Context context) {
        if (IFAAUtil.isUnderScreenFingerprint()) {
            return 17;
        }
        return 1;
    }

    @Override
    public int startBIOManager(Context context, int authType) {
        Log.i(TAG, "authType:" + authType);
        if (authType == 1) {
            try {
                Intent intent = new Intent("android.settings.FINGERPRINT_SETTINGS");
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(intent);
            } catch (Exception e) {
                Log.e(TAG, "startBIOManager with action, exception:" + e.fillInStackTrace());
                return -1;
            }
        }
        return 0;
    }

    @Override
    public String getDeviceModel() {
        String type = Build.MODEL;
        String type2 = type == null ? "" : type.trim();
        if (this.mAlipayDeviceModel == null) {
            this.mAlipayDeviceModel = SystemProperties.get("ro.vendor.feature.zte_alipay_device_model", "");
        }
        String str = this.mAlipayDeviceModel;
        String alipayValue = (str == null || str.trim().length() == 0) ? null : this.mAlipayDeviceModel;
        if (alipayValue == null) {
            if (type2.startsWith("ZTE")) {
                String[] contents = type2.split(" ");
                if (contents.length > 2) {
                    StringBuffer alipayValueSb = new StringBuffer();
                    alipayValueSb.append(contents[0]);
                    alipayValueSb.append("-");
                    alipayValueSb.append(contents[1]);
                    alipayValueSb.append(contents[2]);
                    alipayValue = alipayValueSb.toString();
                } else {
                    alipayValue = type2.replace(" ", "-");
                }
            } else {
                alipayValue = "ZTE-" + type2;
            }
        }
        Log.e(TAG, "alipayValue:" + alipayValue);
        return alipayValue;
    }

    @Override
    public int getVersion() {
        if (IFAAUtil.isUnderScreenFingerprint()) {
            return VERSION_3;
        }
        return 2;
    }

    @Override
    public byte[] processCmdV2(Context context, byte[] param) {
        return processCmd(context, param);
    }

    @Override
    public String getExtInfo(int authType, String keyExtInfo) {
        try {
            if (keyExtInfo.equals(IFAAManagerV3.KEY_GET_SENSOR_LOCATION)) {
                if (this.mFingerprintSensoruiPositionCenterX == 0) {
                    this.mFingerprintSensoruiPositionCenterX = SystemProperties.getInt("ro.vendor.feature.fingerprint_sensorui_position_center_x", 0);
                }
                if (this.mFingerprintSensoruiPositionCenterY == 0) {
                    this.mFingerprintSensoruiPositionCenterY = SystemProperties.getInt("ro.vendor.feature.fingerprint_sensorui_position_center_y", 0);
                }
                if (this.mFingerprintSensoruiPositionCenterR == 0) {
                    this.mFingerprintSensoruiPositionCenterR = SystemProperties.getInt("ro.vendor.feature.fingerprint_sensorui_position_center_r", 0);
                }
                JSONObject jsonObject = new JSONObject();
                jsonObject.put("type", 0);
                JSONObject fullViewObject = new JSONObject();
                fullViewObject.put("startX", this.mFingerprintSensoruiPositionCenterX);
                fullViewObject.put("startY", this.mFingerprintSensoruiPositionCenterY);
                fullViewObject.put("width", this.mFingerprintSensoruiPositionCenterR * 2);
                fullViewObject.put("height", this.mFingerprintSensoruiPositionCenterR * 2);
                fullViewObject.put("navConflict", true);
                jsonObject.put("fullView", fullViewObject);
                return jsonObject.toString();
            }
            return null;
        } catch (Exception ex) {
            Log.e(TAG, "ex:" + ex.fillInStackTrace());
            return null;
        }
    }

    @Override
    public void setExtInfo(int authType, String keyExtInfo, String valExtInfo) {
    }
}
