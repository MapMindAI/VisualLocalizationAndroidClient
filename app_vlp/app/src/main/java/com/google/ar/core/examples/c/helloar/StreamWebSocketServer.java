package com.google.ar.core.examples.c.helloar;

import android.util.Base64;
import android.util.Log;
import java.net.InetSocketAddress;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;
import org.json.JSONException;
import org.json.JSONObject;

class StreamWebSocketServer extends WebSocketServer {
  private static final String TAG = "StreamWebSocketServer";

  StreamWebSocketServer(int port) {
    super(new InetSocketAddress(port));
  }

  @Override
  public void onOpen(WebSocket conn, ClientHandshake handshake) {
    Log.i(TAG, "Client connected: " + conn.getRemoteSocketAddress());
  }

  @Override
  public void onClose(WebSocket conn, int code, String reason, boolean remote) {
    Log.i(TAG, "Client disconnected: " + conn.getRemoteSocketAddress() + ", reason=" + reason);
  }

  @Override
  public void onMessage(WebSocket conn, String message) {
    // No-op. This server is push-only.
  }

  @Override
  public void onError(WebSocket conn, Exception ex) {
    Log.e(TAG, "WebSocket error", ex);
  }

  @Override
  public void onStart() {
    Log.i(TAG, "WebSocket server started on port " + getPort());
  }

  void publishLatestFrame(long nativeApplication) {
    if (getConnections().isEmpty()) {
      return;
    }
    if (!JniInterface.hasLatestStreamFrame(nativeApplication)) {
      return;
    }

    byte[] gray = JniInterface.getLatestGrayImage(nativeApplication);
    long[] dimensionsAndTimestamp =
        JniInterface.getLatestStreamDimensionsAndTimestamp(nativeApplication);
    float[] intrinsics = JniInterface.getLatestStreamIntrinsics(nativeApplication);
    float[] pose = JniInterface.getLatestStreamPose(nativeApplication);

    if (gray == null
        || dimensionsAndTimestamp == null
        || dimensionsAndTimestamp.length < 3
        || intrinsics == null
        || intrinsics.length < 4
        || pose == null
        || pose.length < 7) {
      return;
    }

    try {
      JSONObject payload = new JSONObject();
      payload.put("type", "frame");
      payload.put("timestamp_ns", dimensionsAndTimestamp[0]);
      payload.put("width", dimensionsAndTimestamp[1]);
      payload.put("height", dimensionsAndTimestamp[2]);

      JSONObject intrinsicsJson = new JSONObject();
      intrinsicsJson.put("fx", intrinsics[0]);
      intrinsicsJson.put("fy", intrinsics[1]);
      intrinsicsJson.put("cx", intrinsics[2]);
      intrinsicsJson.put("cy", intrinsics[3]);
      payload.put("intrinsics", intrinsicsJson);

      JSONObject poseJson = new JSONObject();
      poseJson.put("qx", pose[0]);
      poseJson.put("qy", pose[1]);
      poseJson.put("qz", pose[2]);
      poseJson.put("qw", pose[3]);
      poseJson.put("tx", pose[4]);
      poseJson.put("ty", pose[5]);
      poseJson.put("tz", pose[6]);
      payload.put("pose", poseJson);

      payload.put("gray_b64", Base64.encodeToString(gray, Base64.NO_WRAP));
      broadcast(payload.toString());
    } catch (JSONException e) {
      Log.e(TAG, "Failed to serialize frame payload", e);
    }
  }
}
