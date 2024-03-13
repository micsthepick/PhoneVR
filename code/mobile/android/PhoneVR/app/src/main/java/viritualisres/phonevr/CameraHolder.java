/* (C)2024 */
package viritualisres.phonevr;

import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.util.Log;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

public class CameraHolder {
    private Camera mCamera = null;

    private static final String TAG = CameraHolder.class.getSimpleName() + "-Java";

    private SurfaceTexture mTexture = null;
    private Camera.Size mPreviewSize = null;

    public void createTexture(int textureID) {
        mTexture = new SurfaceTexture(textureID);
    }

    public void openCamera(int camNr, int desiredWidth, int desiredHeight) {
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
        params.setRecordingHint(true); // Can increase fps in passthrough mode
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

    public void stopPreview() {
        if (mCamera != null) {
            mCamera.stopPreview();
        }
    }

    public void update() {
        if (mTexture != null) {
            mTexture.updateTexImage();
        }
    }
}
