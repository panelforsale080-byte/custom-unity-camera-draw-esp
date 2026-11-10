package com.custom.unitycamera;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

public final class LineView extends View {
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    public LineView(Context context) {
        this(context, null);
    }

    public LineView(Context context, AttributeSet attrs) {
        super(context, attrs);
        line.setColor(Color.WHITE);
        line.setStrokeWidth(3f);
        text.setColor(Color.WHITE);
        text.setTextSize(36f);
        setWillNotDraw(false);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        int w = getWidth();
        int h = getHeight();
        if (w > 0 && h > 0) NativeBridge.nativeSetScreen(w, h);
        float[] lines = NativeBridge.nativeLines();
        if (lines != null) {
            for (int i = 0; i + 3 < lines.length; i += 4) {
                canvas.drawLine(lines[i], lines[i + 1], lines[i + 2], lines[i + 3], line);
            }
        }
        String status = NativeBridge.nativeStatus();
        if (status != null) canvas.drawText(status, 24f, 64f, text);
        postInvalidateDelayed(16);
    }
}
