package com.custom.unitycamera;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity {
    private TextView status;
    private ArrayAdapter<String> adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        status = findViewById(R.id.status);
        ListView list = findViewById(R.id.processes);
        Button refresh = findViewById(R.id.refresh);
        adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1);
        list.setAdapter(adapter);
        refresh.setOnClickListener(v -> loadProcesses());
        list.setOnItemClickListener((parent, view, position, id) -> select(adapter.getItem(position)));
        loadProcesses();
    }

    private void loadProcesses() {
        String[] rows = NativeBridge.nativeListProcesses();
        adapter.clear();
        if (rows != null) {
            for (String row : rows) adapter.add(row);
        }
        status.setText(adapter.getCount() + " processes. Tap one to find Unity Camera and draw lines.");
    }

    private void select(String row) {
        if (row == null) return;
        int space = row.indexOf(' ');
        if (space <= 0) return;
        int pid;
        try {
            pid = Integer.parseInt(row.substring(0, space).trim());
        } catch (NumberFormatException e) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
            Toast.makeText(this, "Allow overlay, then tap the process again", Toast.LENGTH_LONG).show();
            return;
        }
        String note = NativeBridge.nativeAttach(pid);
        status.setText(note);
        startForegroundService(new Intent(this, OverlayService.class));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }
}
