/* (C)2024 */
package viritualisres.phonevr;

import android.content.SharedPreferences;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.util.Log;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

public class Passthrough implements SensorEventListener {

    static {
        System.loadLibrary("native-lib");
    }

    private static final String TAG = Passthrough.class.getSimpleName() + "-Java";

    private Camera mCamera = null;

    private SurfaceTexture mTexture = null;
    private Camera.Size mPreviewSize = null;

    private final SharedPreferences mPref;

    private final SensorManager mSensorManager;

    private final int displayWidth;

    private final int displayHeight;

    private Sensor mAccelerometer = null;

    private long timestamp = 0;

    private long timestamp_i = 0;

    private long passthrough_delay_ms = 600;

    private float lower_bound = 0.8f;

    private float higher_bound = 4f;

    private final List<Float>[] accelStore =
            new List[] {new ArrayList<>(), new ArrayList<>(), new ArrayList<>()};

    private int halfSize = 0;

    private boolean trigger_set = false;

    private final List<Float> sums = new ArrayList<>();

    public Passthrough(
            SharedPreferences pref,
            SensorManager sensorManager,
            int displayWidth,
            int displayHeight) {
        this.displayWidth = displayWidth;
        this.displayHeight = displayHeight;
        mPref = pref;
        mSensorManager = sensorManager;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mAccelerometer =
                    mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER_UNCALIBRATED);
        }
        if (mAccelerometer == null) {
            mAccelerometer = mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        }
    }

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

        Log.v(
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

    protected void onResume() {
        SharedPreferences.Editor edit = mPref.edit();
        edit.putBoolean("passthrough", false);
        edit.apply();

        timestamp = 0;
        halfSize = 0;
        if (mPref.getBoolean("passthrough_tap", true)) {
            try {
                passthrough_delay_ms = Long.parseLong(mPref.getString("passthrough_delay", "600"));
            } catch (Exception e) {
                passthrough_delay_ms = 600;
            }
            try {
                lower_bound = Float.parseFloat(mPref.getString("passthrough_lower", "0.8"));
            } catch (Exception e) {
                lower_bound = 0.8f;
            }
            try {
                higher_bound = Float.parseFloat(mPref.getString("passthrough_upper", "4"));
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

    protected void setPassthroughMode(boolean passthrough) {
        if (passthrough) {
            int w = displayWidth / 2;
            int h = displayHeight;
            if (displayWidth < displayHeight) {
                w = displayHeight / 2;
                h = displayWidth;
            }
            openCamera(-1, w, h, mPref.getBoolean("passthrough_recording", true));

            float size = 0.5f;
            try {
                size = Float.parseFloat(mPref.getString("passthrough_fraction", "0.5"));
            } catch (RuntimeException e) {
            }

            setPassthroughSizeNative(size);
        }
        setPassthroughActiveNative(passthrough);

        if (passthrough) {
            startPreview();
        } else {
            releaseCamera();
        }
    }

    protected void changePassthroughMode() {
        boolean pt = !mPref.getBoolean("passthrough", false);
        SharedPreferences.Editor edit = mPref.edit();
        edit.putBoolean("passthrough", pt);
        edit.apply();
        setPassthroughMode(pt);
    }

    public void createTexture(int textureID) {
        mTexture = new SurfaceTexture(textureID);
    }

    public void openCamera(int camNr, int desiredWidth, int desiredHeight, boolean recordingHint) {
        if (mCamera != null) {
            // TODO maybe print only a warning
            throw new RuntimeException("Camera already initialized");
        }
        Camera.CameraInfo info = new Camera.CameraInfo();
        // TODO add possibility to choose camera
        if (camNr < 0) {
            int numCameras = Camera.getNumberOfCameras();
            for (int i = 0; i < numCameras; ++i) {
                Camera.getCameraInfo(i, info);
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                    mCamera = Camera.open(i);
                    break;
                }
            }
            if (mCamera == null) {
                // TODO log message "No back-facing camera found; opening default"
                mCamera = Camera.open(); // open default
            }
        } else {
            mCamera = Camera.open(camNr);
        }
        if (mCamera == null) {
            throw new RuntimeException("Unable to open camera");
        }
        // TODO the size stuff
        Camera.Parameters params = mCamera.getParameters();
        List<Camera.Size> choices = params.getSupportedPreviewSizes();
        mPreviewSize = chooseOptimalSize(choices, desiredWidth, desiredHeight);
        Log.d(TAG, "PreviewSize: " + mPreviewSize.width + "x" + mPreviewSize.height);
        params.setPreviewSize(mPreviewSize.width, mPreviewSize.height);
        params.setRecordingHint(recordingHint); // Can increase fps in passthrough mode
        mCamera.setParameters(params);
    }

    private Camera.Size chooseOptimalSize(List<Camera.Size> choices, int width, int height) {
        Integer minSize = Integer.max(Integer.min(width, height), 320);

        // Collect the supported resolutions that are at least as big as the preview Surface
        ArrayList<Camera.Size> bigEnough = new ArrayList<>();
        for (Camera.Size option : choices) {
            if (option.width == width && option.height == height) {
                return option;
            }
            if (option.height >= minSize && option.width >= minSize) {
                bigEnough.add(option);
            }
        }

        Comparator<Camera.Size> comp = (a, b) -> (a.width * a.height) - (b.width * b.width);
        // Pick the smallest of those, assuming we found any
        if (bigEnough.size() > 0) {
            return Collections.min(bigEnough, comp);
        } else {
            return choices.get(0);
        }
    }

    public void releaseCamera() {
        if (mCamera != null) {
            mCamera.setPreviewCallback(null);
            mCamera.stopPreview();
            mCamera.release();
            mCamera = null;
        }
    }

    public void startPreview() {
        if (mCamera != null) {
            // Log.d(TAG, "starting camera preview")
            try {
                mCamera.setPreviewTexture(mTexture);
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
            mCamera.startPreview();
        }
    }

    public void onPause() {
        mSensorManager.unregisterListener(this);
        setPassthroughActiveNative(false);
        releaseCamera();
    }

    public void update() {
        if (mTexture != null) {
            mTexture.updateTexImage();
        }
    }

    private native void setPassthroughSizeNative(float size);

    private native void setPassthroughActiveNative(boolean activate);
}
