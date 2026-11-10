package com.custom.unitycamera;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.view.Gravity;
import android.view.WindowManager;

public final class OverlayService extends Service {
    private WindowManager windowManager;
    private LineView lineView;

    @Override
    public void onCreate() {
        super.onCreate();
        String channel = "unity-camera";
        NotificationChannel ch = new NotificationChannel(channel, "Custom Unity Camera",
                NotificationManager.IMPORTANCE_LOW);
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(ch);
        Notification notice = new Notification.Builder(this, channel)
                .setContentTitle("Custom Unity Camera")
                .setContentText("Drawing test lines")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, notice, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, notice);
        }

        lineView = new LineView(this);
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        windowManager.addView(lineView, lp);
    }

    @Override
    public void onDestroy() {
        NativeBridge.nativeStop();
        if (windowManager != null && lineView != null) windowManager.removeView(lineView);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
