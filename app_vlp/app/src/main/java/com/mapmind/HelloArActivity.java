/*
 * Copyright 2017 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.mapmind;

import android.Manifest;
import android.bluetooth.BluetoothDevice;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.hardware.display.DisplayManager;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import com.google.android.material.snackbar.Snackbar;
import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
/**
 * This is a simple example that shows how to create an augmented reality (AR) application using the
 * ARCore C API.
 */
public class HelloArActivity extends AppCompatActivity
    implements GLSurfaceView.Renderer, DisplayManager.DisplayListener, DeviceScanListener {
  private static final String TAG = HelloArActivity.class.getSimpleName();
  private static final int SNACKBAR_UPDATE_INTERVAL_MILLIS = 1000; // In milliseconds.
  private static final int DEPTH_SOURCE_NONE = 0;
  private static final int DEPTH_SOURCE_ARCORE = 1;
  private static final int DEPTH_SOURCE_DA2 = 2;
  private static final int NUM_INSTANT_PLACEMENT_SETTINGS_CHECKBOXES = 1;
  private static final int BLE_PERMISSION_REQUEST_CODE = 1002;
  // Update these to your Janus gateway settings.
  private static final String JANUS_BASE_URL = "http://8.134.189.19:8080/janus";
  private static final int JANUS_ROOM_ID = 1234;
  private static final String JANUS_ROOM_PIN = "XR2c0MdhhmuxkV9Bi/HPtXevj4V1+WM3";
  private static final String JANUS_TOKEN = "";
  private static final String JANUS_API_SECRET = "";

  private GLSurfaceView surfaceView;
  private TextView msgView;
  private TextView webrtcStatusView;
  private TextView robotStatusView;
  private View robotPanelView;
  private Button robotPanelToggleButton;
  private BleServerManager bleServerManager;
  private boolean robotPanelVisible = true;
  private WebRtcRgbStreamer webRtcStreamer;
  private boolean webRtcEnabled = false;
  private final Handler webRtcFrameHandler = new Handler(Looper.getMainLooper());

  private boolean viewportChanged = false;
  private int viewportWidth;
  private int viewportHeight;

  private final DepthSettings depthSettings = new DepthSettings();
  private int selectedDepthSource = DEPTH_SOURCE_NONE;

  private final InstantPlacementSettings instantPlacementSettings = new InstantPlacementSettings();
  private boolean[] instantPlacementSettingsMenuDialogCheckboxes =
      new boolean[NUM_INSTANT_PLACEMENT_SETTINGS_CHECKBOXES];

  // Opaque native pointer to the native application instance.
  private long nativeApplication;
  private GestureDetector gestureDetector;
  private boolean renderEnabled = true;
  private boolean sceneContentEnabled = false;

  private Snackbar snackbar;
  private Snackbar da2LoadingSnackbar;
  private boolean wasDa2Loading = false;
  // private Handler planeStatusCheckingHandler;
  // private final Runnable planeStatusCheckingRunnable =
  //     new Runnable() {
  //       @Override
  //       public void run() {
  //         // The runnable is executed on main UI thread.
  //         try {
  //           if (JniInterface.hasDetectedPlanes(nativeApplication)) {
  //             if (snackbar != null) {
  //               snackbar.dismiss();
  //             }
  //             snackbar = null;
  //           } else {
  //             planeStatusCheckingHandler.postDelayed(
  //                 planeStatusCheckingRunnable, SNACKBAR_UPDATE_INTERVAL_MILLIS);
  //           }
  //         } catch (Exception e) {
  //           Log.e(TAG, e.getMessage());
  //         }
  //       }
  //     };

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);
    surfaceView = (GLSurfaceView) findViewById(R.id.surfaceview);
    msgView = findViewById(R.id.msg);
    webrtcStatusView = findViewById(R.id.webrtc_status);
    robotPanelView = findViewById(R.id.robot_panel);
    robotPanelToggleButton = findViewById(R.id.robot_panel_toggle_button);
    robotStatusView = findViewById(R.id.robot_status);

    // Set up touch listener.
    gestureDetector =
        new GestureDetector(
            this,
            new GestureDetector.SimpleOnGestureListener() {
              @Override
              public boolean onSingleTapUp(final MotionEvent e) {
                if (!renderEnabled || !sceneContentEnabled) {
                  return true;
                }
                // For devices that support the Depth API, shows a dialog to suggest enabling
                // depth-based occlusion. This dialog needs to be spawned on the UI thread.
                HelloArActivity.this.runOnUiThread(() -> showOcclusionDialogIfNeeded());

                surfaceView.queueEvent(
                    () -> JniInterface.onTouched(nativeApplication, e.getX(), e.getY()));
                return true;
              }

              @Override
              public boolean onDown(MotionEvent e) {
                return true;
              }
            });

    surfaceView.setOnTouchListener(
        (View v, MotionEvent event) -> gestureDetector.onTouchEvent(event));

    // Set up renderer.
    surfaceView.setPreserveEGLContextOnPause(true);
    surfaceView.setEGLContextClientVersion(2);
    surfaceView.setEGLConfigChooser(8, 8, 8, 8, 16, 0); // Alpha used for plane blending.
    surfaceView.setRenderer(this);
    surfaceView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    surfaceView.setWillNotDraw(false);

    JniInterface.assetManager = getAssets();
    nativeApplication = JniInterface.createNativeApplication(getAssets());
    // planeStatusCheckingHandler = new Handler();

    depthSettings.onCreate(this);
    instantPlacementSettings.onCreate(this);
    JniInterface.setDepthSource(nativeApplication, selectedDepthSource);
    setupRobotPanel();
    setupWebRtcPanel();

    File externalStorage = new File(getExternalFilesDir(null), "myfile.txt");
    Log.i("Mobili", externalStorage.getAbsolutePath());

    // 如果文件夹不存在，创建它
    if (!externalStorage.exists()) {
      if (!externalStorage.mkdirs()) {
        Log.e("Mobili", "Failed to create directory: " + externalStorage.getAbsolutePath());
        return;
      }
    }

    Button startrecButton = findViewById(R.id.startrec);
    startrecButton.setOnClickListener(new View.OnClickListener() {
      @Override
      public void onClick(View v) {
        // Format current date and time
        String timeStamp =
            new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.getDefault()).format(new Date());
        File recordFolder = new File(getExternalFilesDir(null), timeStamp);
        String record_folder = recordFolder.getAbsolutePath();
        if (!recordFolder.exists()) {
          if (!recordFolder.mkdirs()) {
            Log.e("Mobili", "Failed to create directory: " + record_folder);
            displayInSnackbar("Failed to create directory");
            return;
          }
        }
        Log.i("Mobili", "Create directory: " + record_folder);
        String recordFile = new File(recordFolder, "vlp_stream.rec").getAbsolutePath();
        boolean ok = JniInterface.onStartRec(recordFile) != 0;
        displayInSnackbar(ok ? ("Start Recording " + recordFile) : "Start Recording failed");
      }
    });
    //
    Button stoprecButton = findViewById(R.id.stoprec);
    stoprecButton.setOnClickListener(new View.OnClickListener() {
      @Override
      public void onClick(View v) {
        JniInterface.onStopRec();
        displayInSnackbar("Stop Recoding");
      }
    });
    final Button renderToggleButton = findViewById(R.id.render_toggle_button);
    final Button sceneToggleButton = findViewById(R.id.scene_toggle_button);
    renderToggleButton.setText("Disable Render");
    renderToggleButton.setOnClickListener(new View.OnClickListener() {
      @Override
      public void onClick(View v) {
        renderEnabled = !renderEnabled;
        JniInterface.setRenderEnabled(nativeApplication, renderEnabled);
        renderToggleButton.setText(renderEnabled ? "Disable Render" : "Enable Render");
        if (renderEnabled) {
          surfaceView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
          displayInSnackbar("Render enabled");
        } else {
          surfaceView.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
          displayInSnackbar("Render disabled, printing pose");
        }
      }
    });
    sceneToggleButton.setText("Show Scene");
    sceneToggleButton.setOnClickListener(
        new View.OnClickListener() {
          @Override
          public void onClick(View v) {
            sceneContentEnabled = !sceneContentEnabled;
            JniInterface.setSceneContentEnabled(nativeApplication, sceneContentEnabled);
            sceneToggleButton.setText(sceneContentEnabled ? "Hide Scene" : "Show Scene");
            displayInSnackbar(sceneContentEnabled ? "Scene enabled" : "Scene hidden");
          }
        });

    final Button depthSourceButton = findViewById(R.id.depth_source_button);
    updateTopControlLabels(depthSourceButton);
    depthSourceButton.setOnClickListener(
        new View.OnClickListener() {
          @Override
          public void onClick(View v) {
            selectedDepthSource = (selectedDepthSource + 1) % 3;
            JniInterface.setDepthSource(nativeApplication, selectedDepthSource);
            updateDa2LoadingUi();
            updateTopControlLabels(depthSourceButton);
          }
        });
    // TODO(yeliu): download tsdf mesh
  }

  private void setupWebRtcPanel() {
    final Button webrtcToggleButton = findViewById(R.id.webrtc_toggle_button);
    webRtcStreamer = new WebRtcRgbStreamer(this);
    webRtcStreamer.configure(
        JANUS_BASE_URL, JANUS_ROOM_ID, JANUS_ROOM_PIN, JANUS_TOKEN, JANUS_API_SECRET);
    webrtcStatusView.setText("WebRTC: idle");
    webrtcToggleButton.setOnClickListener(
        v -> {
          webRtcEnabled = !webRtcEnabled;
          if (webRtcEnabled) {
            JniInterface.setStreamImageEnabled(nativeApplication, true);
            webRtcStreamer.start();
            webrtcToggleButton.setText("Stop WebRTC");
            webrtcStatusView.setText("WebRTC: connecting Janus room " + JANUS_ROOM_ID);
            startWebRtcFramePump();
          } else {
            JniInterface.setStreamImageEnabled(nativeApplication, false);
            stopWebRtcFramePump();
            webRtcStreamer.stop();
            webrtcToggleButton.setText("Start WebRTC");
            webrtcStatusView.setText("WebRTC: stopped");
          }
        });
  }

  private void startWebRtcFramePump() {
    webRtcFrameHandler.post(
        new Runnable() {
          @Override
          public void run() {
            if (!webRtcEnabled || webRtcStreamer == null) {
              return;
            }
            byte[] nv21 = JniInterface.getLatestYuvNv21Image(nativeApplication);
            long[] info = JniInterface.getLatestStreamDimensionsAndTimestamp(nativeApplication);
            if (nv21 != null && info != null && info.length >= 3) {
              int width = (int) info[1];
              int height = (int) info[2];
              long tsNs = info[0];
              if (width > 0 && height > 0 && nv21.length >= (width * height * 3 / 2)) {
                webRtcStreamer.pushNv21Frame(nv21, width, height, tsNs);
                webrtcStatusView.setText("WebRTC: streaming " + width + "x" + height);
              }
            }
            webRtcFrameHandler.postDelayed(this, 66);
          }
        });
  }

  private void stopWebRtcFramePump() {
    webRtcFrameHandler.removeCallbacksAndMessages(null);
  }

  private void setupRobotPanel() {
    robotPanelToggleButton.setOnClickListener(
        v -> {
          robotPanelVisible = !robotPanelVisible;
          robotPanelView.setVisibility(robotPanelVisible ? View.VISIBLE : View.GONE);
          robotPanelToggleButton.setText(robotPanelVisible ? "Hide BLE" : "Show BLE");
        });

    findViewById(R.id.robot_scan_button)
        .setOnClickListener(
            v -> {
              if (!ensureBlePermissions()) return;
              if (bleServerManager == null) return;
              robotStatusView.setText("BLE scanning...");
              bleServerManager.scanMokukuDevices(this);
            });

    View.OnTouchListener robotMoveListener =
        (v, event) -> {
          int dir = 0;
          int id = v.getId();
          if (id == R.id.robot_up_button) dir = 8;
          else if (id == R.id.robot_down_button) dir = 2;
          else if (id == R.id.robot_left_button) dir = 4;
          else if (id == R.id.robot_right_button) dir = 6;

          if (event.getAction() == MotionEvent.ACTION_DOWN) {
            if (bleServerManager != null) {
              bleServerManager.sendStringMessage(dir, "MOVE");
            }
          } else if (event.getAction() == MotionEvent.ACTION_UP
              || event.getAction() == MotionEvent.ACTION_CANCEL) {
            if (bleServerManager != null) {
              bleServerManager.sendStringMessage(5, "STOP");
            }
          }
          return true;
        };
    findViewById(R.id.robot_up_button).setOnTouchListener(robotMoveListener);
    findViewById(R.id.robot_down_button).setOnTouchListener(robotMoveListener);
    findViewById(R.id.robot_left_button).setOnTouchListener(robotMoveListener);
    findViewById(R.id.robot_right_button).setOnTouchListener(robotMoveListener);

    if (!ensureBlePermissions()) {
      robotStatusView.setText("BLE permission required");
      return;
    }
    bleServerManager = new BleServerManager(this);
    bleServerManager.bleDeviceHeader = "BOTMOKUKU";
    bleServerManager.isRobot = true;
    bleServerManager.startClient();
    bleServerManager.scanMokukuDevices(this);
  }

  private boolean ensureBlePermissions() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
      boolean fineLocationGranted =
          ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
              == PackageManager.PERMISSION_GRANTED;
      if (fineLocationGranted) {
        return true;
      }
      ActivityCompat.requestPermissions(
          this, new String[] {Manifest.permission.ACCESS_FINE_LOCATION}, BLE_PERMISSION_REQUEST_CODE);
      return false;
    }
    boolean scanGranted =
        ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN)
            == PackageManager.PERMISSION_GRANTED;
    boolean connectGranted =
        ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
            == PackageManager.PERMISSION_GRANTED;
    if (scanGranted && connectGranted) {
      return true;
    }
    ActivityCompat.requestPermissions(
        this,
        new String[] {Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT},
        BLE_PERMISSION_REQUEST_CODE);
    return false;
  }

  @Override
  protected void onResume() {
    super.onResume();
    // ARCore requires camera permissions to operate. If we did not yet obtain runtime
    // permission on Android M and above, now is a good time to ask the user for it.
    if (!CameraPermissionHelper.hasCameraPermission(this)) {
      CameraPermissionHelper.requestCameraPermission(this);
      return;
    }
    CameraPermissionHelper.checkStoragePermission(this);

    try {
      JniInterface.onSettingsChange(
        nativeApplication, instantPlacementSettings.isInstantPlacementEnabled());
      JniInterface.setDepthSource(nativeApplication, selectedDepthSource);
      JniInterface.setRenderEnabled(nativeApplication, renderEnabled);
      JniInterface.setSceneContentEnabled(nativeApplication, sceneContentEnabled);
      surfaceView.setRenderMode(
          renderEnabled
              ? GLSurfaceView.RENDERMODE_CONTINUOUSLY
              : GLSurfaceView.RENDERMODE_WHEN_DIRTY);
      JniInterface.onResume(nativeApplication, getApplicationContext(), this);
      surfaceView.onResume();
    } catch (Exception e) {
      Log.e(TAG, "Exception creating session", e);
      displayInSnackbar(e.getMessage());
      return;
    }

    // Listen to display changed events to detect 180° rotation, which does not cause a config
    // change or view resize.
    getSystemService(DisplayManager.class).registerDisplayListener(this, null);
  }

  @Override
  public void onPause() {
    super.onPause();
    surfaceView.onPause();
    JniInterface.onPause(nativeApplication);
    stopWebRtcFramePump();

    getSystemService(DisplayManager.class).unregisterDisplayListener(this);
  }

  @Override
  public void onDestroy() {
    super.onDestroy();
    stopWebRtcFramePump();
    JniInterface.setStreamImageEnabled(nativeApplication, false);
    if (webRtcStreamer != null) {
      webRtcStreamer.release();
      webRtcStreamer = null;
    }
    if (bleServerManager != null) {
      bleServerManager.shutdown();
    }

    // Synchronized to avoid racing onDrawFrame.
    synchronized (this) {
      JniInterface.destroyNativeApplication(nativeApplication);
      nativeApplication = 0;
    }
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (hasFocus) {
      // Standard Android full-screen functionality.
      getWindow()
          .getDecorView()
          .setSystemUiVisibility(
              View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
      getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
  }

  @Override
  public void onSurfaceCreated(GL10 gl, EGLConfig config) {
    GLES20.glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    JniInterface.onGlSurfaceCreated(nativeApplication);
  }

  @Override
  public void onSurfaceChanged(GL10 gl, int width, int height) {
    viewportWidth = width;
    viewportHeight = height;
    viewportChanged = true;
  }

  @Override
  public void onDrawFrame(GL10 gl) {
    // Synchronized to avoid racing onDestroy.
    synchronized (this) {
      if (nativeApplication == 0) {
        return;
      }
      if (viewportChanged) {
        int displayRotation = getWindowManager().getDefaultDisplay().getRotation();
        JniInterface.onDisplayGeometryChanged(
            nativeApplication, displayRotation, viewportWidth, viewportHeight);
        viewportChanged = false;
      }
      JniInterface.onGlSurfaceDrawFrame(
          nativeApplication,
          false,
          false);
    }
  }

  @Override
  public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
    super.onRequestPermissionsResult(requestCode, permissions, results);
    if (requestCode == BLE_PERMISSION_REQUEST_CODE) {
      if (ensureBlePermissions() && bleServerManager == null) {
        setupRobotPanel();
      }
      return;
    }
    if (!CameraPermissionHelper.hasCameraPermission(this)) {
      Toast.makeText(this, "Camera permission is needed to run this application", Toast.LENGTH_LONG)
          .show();
      if (!CameraPermissionHelper.shouldShowRequestPermissionRationale(this)) {
        // Permission denied with checking "Do not ask again".
        CameraPermissionHelper.launchPermissionSettings(this);
      }
      finish();
    }
  }

  @Override
  public void onScanStart() {
    runOnUiThread(() -> robotStatusView.setText("BLE scanning..."));
  }

  @Override
  public void onDeviceFound(BluetoothDevice device, String displayName) {
    runOnUiThread(() -> {
      if (bleServerManager != null) {
        bleServerManager.setTargetDevice(device);
      }
      Log.i(TAG, "BLE device found, connecting: " + displayName);
      robotStatusView.setText("Found " + displayName + ", connecting...");
    });
  }

  @Override
  public void onDeviceConnect(BluetoothDevice device, String displayName) {
    runOnUiThread(() -> {
      Log.i(TAG, "BLE connected: " + displayName);
      robotStatusView.setText("Connected: " + displayName);
    });
  }

  @Override
  public void onScanFinished() {
    // runOnUiThread(() -> robotStatusView.setText("BLE scan done"));
  }

  /**
   * Display the message in the snackbar.
   */
  private void displayInSnackbar(String message) {
    if (snackbar != null && snackbar.isShown()) {
      snackbar.dismiss();
    }
    snackbar =
        Snackbar.make(
            HelloArActivity.this.findViewById(android.R.id.content),
            message, Snackbar.LENGTH_SHORT);

    // Set the snackbar background to light transparent black color.
    snackbar.getView().setBackgroundColor(0xbf323232);
    snackbar.show();
  }

  private void updateDa2LoadingUi() {
    boolean showLoading = !JniInterface.isDa2Ready(nativeApplication);
    if (showLoading) {
      wasDa2Loading = true;
      if (da2LoadingSnackbar == null || !da2LoadingSnackbar.isShown()) {
        da2LoadingSnackbar =
            Snackbar.make(
                HelloArActivity.this.findViewById(android.R.id.content),
                "Loading DA2 model...",
                Snackbar.LENGTH_INDEFINITE);
        da2LoadingSnackbar.getView().setBackgroundColor(0xbf323232);
        da2LoadingSnackbar.show();
      }
    } else {
      if (da2LoadingSnackbar != null) {
        da2LoadingSnackbar.dismiss();
        da2LoadingSnackbar = null;
      }
      if (wasDa2Loading) {
        displayInSnackbar("DA2 model loaded");
      }
      wasDa2Loading = false;
    }
  }

  /**
   * Shows a pop-up dialog on the first call, determining whether the user wants to enable
   * depth-based occlusion. The result of this dialog can be retrieved with useDepthForOcclusion().
   */
  private void showOcclusionDialogIfNeeded() {
    boolean isDepthSupported = JniInterface.isDepthSupported(nativeApplication);
    if (!depthSettings.shouldShowDepthEnableDialog() || !isDepthSupported) {
      return; // Don't need to show dialog.
    }

    // Asks the user whether they want to use depth-based occlusion.
    new AlertDialog.Builder(this)
        .setTitle(R.string.options_title_with_depth)
        .setMessage(R.string.depth_use_explanation)
        .setPositiveButton(
            R.string.button_text_enable_depth,
            (DialogInterface dialog, int which) -> {
              depthSettings.setUseDepthForOcclusion(true);
            })
        .setNegativeButton(
            R.string.button_text_disable_depth,
            (DialogInterface dialog, int which) -> {
              depthSettings.setUseDepthForOcclusion(false);
            })
        .show();
  }

  private void launchInstantPlacementSettingsMenuDialog() {
    resetSettingsMenuDialogCheckboxes();
    Resources resources = getResources();
    new AlertDialog.Builder(this)
        .setTitle(R.string.options_title_instant_placement)
        .setMultiChoiceItems(
            resources.getStringArray(R.array.instant_placement_options_array),
            instantPlacementSettingsMenuDialogCheckboxes,
            (DialogInterface dialog, int which, boolean isChecked) ->
                instantPlacementSettingsMenuDialogCheckboxes[which] = isChecked)
        .setPositiveButton(
            R.string.done,
            (DialogInterface dialogInterface, int which) -> applySettingsMenuDialogCheckboxes())
        .setNegativeButton(
            android.R.string.cancel,
            (DialogInterface dialog, int which) -> resetSettingsMenuDialogCheckboxes())
        .show();
  }

  /** Shows checkboxes to the user to facilitate toggling of depth-based effects. */
  private void launchDepthSettingsMenuDialog() {
    Resources resources = getResources();
    final int[] pendingSource = {selectedDepthSource};
    new AlertDialog.Builder(this)
        .setTitle(R.string.options_title_with_depth)
        .setSingleChoiceItems(
            resources.getStringArray(R.array.depth_source_options_array),
            selectedDepthSource,
            (DialogInterface dialog, int which) -> pendingSource[0] = which)
        .setPositiveButton(
            R.string.done,
            (DialogInterface dialogInterface, int which) -> {
              selectedDepthSource = pendingSource[0];
              applySettingsMenuDialogCheckboxes();
            })
        .setNegativeButton(android.R.string.cancel, null)
        .show();
  }

  private void applySettingsMenuDialogCheckboxes() {
    instantPlacementSettings.setInstantPlacementEnabled(
        instantPlacementSettingsMenuDialogCheckboxes[0]);

    JniInterface.onSettingsChange(
        nativeApplication, instantPlacementSettings.isInstantPlacementEnabled());
    JniInterface.setDepthSource(nativeApplication, selectedDepthSource);
  }

  private void resetSettingsMenuDialogCheckboxes() {
    instantPlacementSettingsMenuDialogCheckboxes[0] =
        instantPlacementSettings.isInstantPlacementEnabled();
  }

  private String depthSourceLabel() {
    if (selectedDepthSource == DEPTH_SOURCE_DA2) {
      return "DA2";
    }
    if (selectedDepthSource == DEPTH_SOURCE_ARCORE) {
      return "ARCore";
    }
    return "None";
  }

  private void updateTopControlLabels(Button depthSourceButton) {
    depthSourceButton.setText("Depth: " + depthSourceLabel());
  }

  // DisplayListener methods
  @Override
  public void onDisplayAdded(int displayId) {}

  @Override
  public void onDisplayRemoved(int displayId) {}

  @Override
  public void onDisplayChanged(int displayId) {
    viewportChanged = true;
  }
}
