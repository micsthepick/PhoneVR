/* (C)2023 */
package viritualisres.phonevr;

import android.Manifest;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.BatteryManager;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.WindowManager;
import android.widget.PopupMenu;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.preference.PreferenceManager;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class ALVRActivity extends AppCompatActivity
        implements PopupMenu.OnMenuItemClickListener, BatteryLevelListener, SensorEventListener {

    static {
        System.loadLibrary("native-lib");
    }

    private static final String TAG = ALVRActivity.class.getSimpleName() + "-Java";

    // Permission request codes
    private static final int PERMISSIONS_REQUEST_CODE = 2;

    private GLSurfaceView glView;

    private int displayWidth = 0;
    private int displayHeight = 0;

    private CameraHolder cameraHolder = new CameraHolder();

    private BatteryMonitor bMonitor = new BatteryMonitor(this);

    private SharedPreferences pref = null;

    private SensorManager mSensorManager;

    private Sensor mAccelerometer;

    private long timestamp = 0;

    private long timestamp_i = 0;

    private long passthrough_delay_ms = 600;

    private float lower_bound = 0.8f;

    private float higher_bound = 4f;

    private List<Float>[] accelStore =
            new List[] {new ArrayList<>(), new ArrayList<>(), new ArrayList<>()};

    private int halfSize = 0;

    private boolean trigger_set = false;

    private List<Float> sums = new ArrayList<>();

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (timestamp == 0) {
            timestamp = event.timestamp;
        }

        if (timestamp_i == 0) {
            timestamp_i = event.timestamp;
        }

        boolean initial_full = event.timestamp - timestamp > passthrough_delay_ms * 1000000L;
        boolean over_i = event.timestamp - timestamp_i > passthrough_delay_ms * 1000000L;

        // gather data for passthrough_delay_ms, we get a data point approx. each 60ms
        for (int i = 0; i < accelStore.length; ++i) {
            // derivative, absolute
            accelStore[i].add(event.values[i]);

            if (initial_full) {
                // remove the first element to only store the last X elements
                accelStore[i].remove(0);
                if (halfSize == 0) {
                    halfSize = accelStore[0].size() / 2;
                }
                // check if half size has at least 3 data points
                if (halfSize < 4) {
                    accelStore[i].clear();
                    timestamp = 0;
                    halfSize = 0;
                }
            }
        }

        if (halfSize > 0) {
            // Calculate derivative and absolute values
            List<Float>[] accelAbs =
                    new List[] {new ArrayList<>(), new ArrayList<>(), new ArrayList<>()};
            for (int axis = 0; axis < accelStore.length; ++axis) {
                for (int i = 1; i < accelStore[axis].size(); ++i) {
                    accelAbs[axis].add(
                            Math.abs(accelStore[axis].get(i) - accelStore[axis].get(i - 1)));
                }
            }

            // get the mean for each axis and sum them up
            float[] axis_mean = new float[] {0.0f, 0.0f, 0.0f};
            for (int i = 0; i < accelAbs.length; ++i) {
                for (int j = 0; j < accelAbs[i].size(); ++j) {
                    axis_mean[i] += accelAbs[i].get(j);
                }
                axis_mean[i] /= accelAbs[i].size();
            }
            float sum_mean = axis_mean[0] + axis_mean[1] + axis_mean[2];
            sums.add(sum_mean);

            if (sums.size() < accelAbs[0].size()) {
                return;
            } else {
                // only store the last X elements
                sums.remove(0);
            }

            if (!over_i) {
                // if a potential passthrough_change event is found,
                // check if the device does not move much anymore
                if (!trigger_set && sum_mean < lower_bound) {
                    trigger_set = true;
                    // if passthroughMode not already changed... change it
                    changePassthroughMode();
                }
            } else {
                trigger_set = false;
                // here we check for potential passthrough_change events
                // these may occur on any axis
                for (List<Float> elem : accelAbs) {
                    // we look at two halves of the passthrough_delay
                    // we check:
                    // - if in the first and second half is a real peak
                    // - whether the peaks are in the bounds
                    // - if the summed mean is in the bounds
                    // - if the first summed mean is below lower_bound
                    //   (to assure a resting stage before the triggering)
                    // - if the second maximum is decending under the lower bound until curr value
                    // If all is fulfilled we consider this a potential passthrough_change event.
                    // The sum must then decend under lower bound until passthrough_delay
                    int middle = halfSize - 1;
                    List<Float> t1 = elem.subList(0, halfSize);
                    List<Float> t2 = elem.subList(middle, elem.size());

                    float max1 = Collections.max(t1);
                    float max2 = Collections.max(t2);

                    if (max1 - t1.get(0) < lower_bound
                            || max1 - t1.get(t1.size() - 1) < lower_bound
                            || max2 - t2.get(0) < lower_bound
                            || max2 - t2.get(t2.size() - 1) < lower_bound
                            || max1 < lower_bound
                            || max1 > higher_bound
                            || max2 < lower_bound
                            || max2 > higher_bound
                            || sum_mean < lower_bound
                            || sum_mean > higher_bound) {
                        continue;
                    }

                    int end2 = 0;
                    for (int i = 0; i < t2.size(); ++i) {
                        if (t2.get(i) == max2) {
                            end2 = i;
                        }
                        if (end2 > 0) {
                            if (t2.get(i) < lower_bound) {
                                end2 = i;
                                break;
                            }
                        }
                    }

                    if (sums.get(0) < Math.max(lower_bound - 0.3f, 0.5f) && t2.get(end2) < max2) {
                        timestamp_i = event.timestamp;
                        break;
                    }
                }
            }
        }

        Log.d(
                TAG + "-AccSensor",
                String.format(
                        (Locale) null,
                        "%d: [%f, %f, %f]",
                        event.timestamp,
                        event.values[0],
                        event.values[1],
                        event.values[2]));
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    public class BatteryMonitor extends BroadcastReceiver {
        private BatteryLevelListener listener;

        public BatteryMonitor(BatteryLevelListener listener) {
            this.listener = listener;
        }

        public void startMonitoring(Context context) {
            // Register BroadcastReceiver to monitor battery level changes
            IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            context.registerReceiver(this, filter);
        }

        public void stopMonitoring(Context context) {
            // Unregister the BroadcastReceiver
            context.unregisterReceiver(this);
        }

        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent.getAction().equals(Intent.ACTION_BATTERY_CHANGED)) {
                // Get the current battery level and scale
                int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, 0);
                int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);

                // Calculate the battery percentage as a float value between 0 and 1
                float batteryPercentage = (float) level / scale;

                // Get the charging state
                int chargingState = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1);
                boolean isCharging =
                        chargingState == BatteryManager.BATTERY_PLUGGED_AC
                                || chargingState == BatteryManager.BATTERY_PLUGGED_USB
                                || chargingState == BatteryManager.BATTERY_PLUGGED_WIRELESS;

                // Notify the listener with the current battery percentage and charging state
                if (listener != null) {
                    listener.onBatteryLevelChanged(batteryPercentage, isCharging);
                }
            }
        }
    }

    private void resumePassthroughDetection() {
        timestamp = 0;
        halfSize = 0;
        if (pref.getBoolean("passthrough_tap", true)) {
            try {
                passthrough_delay_ms = Long.parseLong(pref.getString("passthrough_delay", "600"));
            } catch (Exception e) {
                passthrough_delay_ms = 600;
            }
            try {
                lower_bound = Float.parseFloat(pref.getString("passthrough_lower", "0.8"));
            } catch (Exception e) {
                lower_bound = 0.8f;
            }
            try {
                higher_bound = Float.parseFloat(pref.getString("passthrough_upper", "4"));
            } catch (Exception e) {
                higher_bound = 4.f;
            }

            if (mAccelerometer != null) {
                mSensorManager.registerListener(
                        this, mAccelerometer, SensorManager.SENSOR_DELAY_UI);
            }
        } else {
            mSensorManager.unregisterListener(this);
        }
    }

    @Override
    public void onCreate(Bundle savedInstance) {
        super.onCreate(savedInstance);
        Log.d(TAG, "onCreate ALVRActivity");

        DisplayMetrics displayMetrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getMetrics(displayMetrics);
        displayWidth = displayMetrics.widthPixels;
        displayHeight = displayMetrics.heightPixels;

        initializeNative(displayWidth, displayHeight);

        setContentView(R.layout.activity_vr);
        glView = findViewById(R.id.surface_view);
        glView.setEGLContextClientVersion(3);
        Renderer renderer = new Renderer();
        glView.setRenderer(renderer);
        glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);

        setImmersiveSticky();
        View decorView = getWindow().getDecorView();
        decorView.setOnSystemUiVisibilityChangeListener(
                (visibility) -> {
                    if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
                        setImmersiveSticky();
                    }
                });

        // Forces screen to max brightness.
        WindowManager.LayoutParams layout = getWindow().getAttributes();
        layout.screenBrightness = 1.f;
        getWindow().setAttributes(layout);

        // Prevents screen from dimming/locking.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        pref = PreferenceManager.getDefaultSharedPreferences(this);

        mSensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        mAccelerometer = mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER_UNCALIBRATED);
        if (mAccelerometer == null) {
            mAccelerometer = mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        }
    }

    @Override
    public void onBatteryLevelChanged(float batteryPercentage, boolean isPlugged) {
        sendBatteryLevel(batteryPercentage, isPlugged);
        Log.d(TAG, "Battery level changed: " + batteryPercentage + ", isPlugged in? :" + isPlugged);
    }

    @Override
    protected void onPause() {
        super.onPause();
        mSensorManager.unregisterListener(this);
        Log.d(TAG, "Pausing ALVR Activity");
        pauseNative();
        glView.onPause();
        setPassthroughActiveNative(false);
        bMonitor.stopMonitoring(this);
        cameraHolder.releaseCamera();
    }

    @Override
    protected void onResume() {
        super.onResume();
        SharedPreferences.Editor edit = pref.edit();
        edit.putBoolean("passthrough", false);
        edit.apply();

        Log.d(TAG, "Resuming ALVR Activity");

        if (VERSION.SDK_INT < VERSION_CODES.Q && !isReadExternalStorageEnabled()) {
            final String[] permissions = new String[] {Manifest.permission.READ_EXTERNAL_STORAGE};
            ActivityCompat.requestPermissions(this, permissions, PERMISSIONS_REQUEST_CODE);

            return;
        }
        resumePassthroughDetection();
        glView.onResume();
        resumeNative();
        bMonitor.startMonitoring(this);
    }

    protected void setPassthroughMode(boolean passthrough) {
        if (passthrough) {
            int w = displayWidth / 2;
            int h = displayHeight;
            if (displayWidth < displayHeight) {
                w = displayHeight / 2;
                h = displayWidth;
            }
            cameraHolder.openCamera(-1, w, h, pref.getBoolean("passthrough_recording", true));

            float size = 0.5f;
            try {
                size = Float.parseFloat(pref.getString("passthrough_fraction", "0.5"));
            } catch (RuntimeException e) {
            }

            setPassthroughSizeNative(size);
        }
        setPassthroughActiveNative(passthrough);

        if (passthrough) {
            cameraHolder.startPreview();
        } else {
            cameraHolder.releaseCamera();
        }
    }

    protected void changePassthroughMode() {
        boolean pt = !pref.getBoolean("passthrough", false);
        SharedPreferences.Editor edit = pref.edit();
        edit.putBoolean("passthrough", pt);
        edit.apply();
        setPassthroughMode(pt);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "Destroying ALVR Activity");
        destroyNative();
        cameraHolder.releaseCamera();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            setImmersiveSticky();
        }
    }

    private class Renderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 gl10, EGLConfig eglConfig) {
            int texID = surfaceCreatedNative();
            cameraHolder.createTexture(texID);
        }

        @Override
        public void onSurfaceChanged(GL10 gl10, int width, int height) {
            setScreenResolutionNative(width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl10) {
            cameraHolder.update();
            renderNative();
        }
    }

    // Called from activity_vr.xml
    public void closeSample(View view) {
        Log.d(TAG, "Leaving VR sample");
        finish();
    }

    // Called from activity_vr.xml
    public void showSettings(View view) {
        PopupMenu popup = new PopupMenu(this, view);
        MenuInflater inflater = popup.getMenuInflater();
        inflater.inflate(R.menu.settings_menu, popup.getMenu());
        MenuItem box = popup.getMenu().findItem(R.id.passthrough);
        if (box != null) {
            box.setChecked(pref.getBoolean("passthrough", false));
        }
        popup.setOnMenuItemClickListener(this);
        popup.show();
    }

    @Override
    public boolean onMenuItemClick(MenuItem item) {
        if (item.getItemId() == R.id.switch_viewer) {
            switchViewerNative();
            return true;
        }
        if (item.getItemId() == R.id.passthrough_settings) {
            Intent settings = new Intent(this, PassthroughSettingsActivity.class);
            startActivity(settings);
            return true;
        }
        if (item.getItemId() == R.id.passthrough) {
            changePassthroughMode();
            return true;
        }
        return false;
    }

    private boolean isReadExternalStorageEnabled() {
        return ActivityCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (!isReadExternalStorageEnabled()) {
            Toast.makeText(this, R.string.read_storage_permission, Toast.LENGTH_LONG).show();
            if (!ActivityCompat.shouldShowRequestPermissionRationale(
                    this, Manifest.permission.READ_EXTERNAL_STORAGE)) {
                // Permission denied with checking "Do not ask again". Note that in Android R
                // "Do not ask again" is not available anymore.
                Intent intent = new Intent();
                intent.setAction(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
                intent.setData(Uri.fromParts("package", getPackageName(), null));
                startActivity(intent);
            }
            finish();
        }
    }

    private void setImmersiveSticky() {
        getWindow()
                .getDecorView()
                .setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private native void initializeNative(int screenWidth, int screenHeight);

    private native void destroyNative();

    private native void resumeNative();

    private native void pauseNative();

    private native int surfaceCreatedNative();

    private native void setScreenResolutionNative(int width, int height);

    private native void renderNative();

    private native void switchViewerNative();

    private native void sendBatteryLevel(float level, boolean plugged);

    private native void setPassthroughSizeNative(float size);

    private native void setPassthroughActiveNative(boolean activate);
}
