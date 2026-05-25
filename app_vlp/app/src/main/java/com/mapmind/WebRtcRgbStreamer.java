package com.mapmind;

import android.content.Context;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;
import org.webrtc.DefaultVideoDecoderFactory;
import org.webrtc.DefaultVideoEncoderFactory;
import org.webrtc.EglBase;
import org.webrtc.IceCandidate;
import org.webrtc.MediaConstraints;
import org.webrtc.NV21Buffer;
import org.webrtc.PeerConnection;
import org.webrtc.PeerConnectionFactory;
import org.webrtc.SessionDescription;
import org.webrtc.SdpObserver;
import org.webrtc.VideoFrame;
import org.webrtc.VideoSource;
import org.webrtc.VideoTrack;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class WebRtcRgbStreamer {
  private static final String TAG = "[MapMind Janus]";
  private static final String VIDEO_TRACK_ID = "rgb_track";
  private static final String STREAM_ID = "rgb_stream";

  private final EglBase eglBase;
  private final PeerConnectionFactory factory;
  private final VideoSource videoSource;
  private final VideoTrack videoTrack;
  private final ExecutorService janusIo = Executors.newSingleThreadExecutor();

  private PeerConnection peerConnection;
  private boolean started = false;
  private boolean ready = false;

  private String janusBaseUrl = "";
  private int roomId = 1234;
  private String roomPin = "";
  private String janusToken = "";
  private String janusApiSecret = "";

  private long sessionId = 0;
  private long handleId = 0;

  public WebRtcRgbStreamer(Context context) {
    PeerConnectionFactory.initialize(
        PeerConnectionFactory.InitializationOptions.builder(context)
            .setEnableInternalTracer(false)
            .createInitializationOptions());
    eglBase = EglBase.create();
    factory =
        PeerConnectionFactory.builder()
            .setVideoEncoderFactory(
                new DefaultVideoEncoderFactory(eglBase.getEglBaseContext(), true, true))
            .setVideoDecoderFactory(new DefaultVideoDecoderFactory(eglBase.getEglBaseContext()))
            .createPeerConnectionFactory();

    videoSource = factory.createVideoSource(false);
    videoTrack = factory.createVideoTrack(VIDEO_TRACK_ID, videoSource);
  }

  public void configure(String janusBaseUrl, int roomId, String roomPin, String token, String apiSecret) {
    this.janusBaseUrl = janusBaseUrl == null ? "" : janusBaseUrl.trim();
    this.roomId = roomId;
    this.roomPin = roomPin == null ? "" : roomPin.trim();
    this.janusToken = token == null ? "" : token.trim();
    this.janusApiSecret = apiSecret == null ? "" : apiSecret.trim();
  }

  public void start() {
    if (started) return;
    if (janusBaseUrl.isEmpty()) {
      Log.e(TAG, "Janus base URL is empty");
      return;
    }
    Log.i(TAG, "Janus start");
    started = true;

    List<PeerConnection.IceServer> iceServers = new ArrayList<>();
    iceServers.add(PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer());
    PeerConnection.RTCConfiguration rtcConfig = new PeerConnection.RTCConfiguration(iceServers);

    peerConnection =
        factory.createPeerConnection(
            rtcConfig,
            new PeerConnection.Observer() {
              @Override
              public void onSignalingChange(PeerConnection.SignalingState signalingState) {}

              @Override
              public void onIceConnectionChange(PeerConnection.IceConnectionState iceConnectionState) {
                Log.i(TAG, "ICE state: " + iceConnectionState);
              }

              @Override
              public void onIceConnectionReceivingChange(boolean b) {}

              @Override
              public void onIceGatheringChange(PeerConnection.IceGatheringState state) {
                Log.i(TAG, "ICE gathering state: " + state);
              }

              @Override
              public void onIceCandidate(IceCandidate iceCandidate) {
                janusIo.execute(() -> sendTrickleCandidate(iceCandidate));
              }

              @Override
              public void onConnectionChange(PeerConnection.PeerConnectionState state) {
                Log.i(TAG, "PeerConnection state: " + state);
              }

              @Override
              public void onIceCandidatesRemoved(IceCandidate[] iceCandidates) {}

              @Override
              public void onAddStream(org.webrtc.MediaStream mediaStream) {}

              @Override
              public void onRemoveStream(org.webrtc.MediaStream mediaStream) {}

              @Override
              public void onDataChannel(org.webrtc.DataChannel dataChannel) {}

              @Override
              public void onRenegotiationNeeded() {}

              @Override
              public void onAddTrack(org.webrtc.RtpReceiver rtpReceiver, org.webrtc.MediaStream[] mediaStreams) {}
            });

    if (peerConnection == null) {
      Log.e(TAG, "Failed to create peerConnection");
      started = false;
      return;
    }

    peerConnection.addTrack(videoTrack, Collections.singletonList(STREAM_ID));
    janusIo.execute(this::janusSetupAndPublish);
  }

  public void stop() {
    started = false;
    ready = false;

    janusIo.execute(() -> {
      try {
        if (sessionId != 0 && handleId != 0) {
          JSONObject detach = baseJanus("detach");
          postJson(sessionHandleUrl(), detach);
        }
        if (sessionId != 0) {
          JSONObject destroy = baseJanus("destroy");
          postJson(sessionUrl(), destroy);
        }
      } catch (Exception e) {
        Log.w(TAG, "Janus stop cleanup failed", e);
      } finally {
        sessionId = 0;
        handleId = 0;
      }
    });

    if (peerConnection != null) {
      peerConnection.close();
      peerConnection.dispose();
      peerConnection = null;
    }
  }

  public void pushNv21Frame(byte[] nv21, int width, int height, long timestampNs) {
    if (!ready || !started || peerConnection == null || nv21 == null || width <= 0 || height <= 0) return;

    int expected = width * height * 3 / 2;
    if (nv21.length < expected) {
      Log.e(TAG, "Invalid NV21 length: " + nv21.length + ", expected >= " + expected);
      return;
    }

    byte[] frameBytes = java.util.Arrays.copyOf(nv21, expected);

    NV21Buffer buffer = new NV21Buffer(frameBytes, width, height, null);
    VideoFrame frame = new VideoFrame(buffer, 0, timestampNs > 0 ? timestampNs : System.nanoTime());

    videoSource.getCapturerObserver().onFrameCaptured(frame);

    frame.release();

    // Log.i(TAG, "pushNv21Frame: " + width + "x" + height + ", len=" + nv21.length);
  }

  public void release() {
    stop();
    videoTrack.dispose();
    videoSource.dispose();
    factory.dispose();
    eglBase.release();
    janusIo.shutdownNow();
  }

  private void janusSetupAndPublish() {
    try {
      // create session
      JSONObject createResp = postJson(janusBaseUrl, baseJanus("create"));
      sessionId = createResp.getJSONObject("data").getLong("id");
      Log.i(TAG, "Janus session: " + sessionId);

      // attach videoroom plugin
      JSONObject attachReq = baseJanus("attach");
      attachReq.put("plugin", "janus.plugin.videoroom");
      JSONObject attachResp = postJson(sessionUrl(), attachReq);
      handleId = attachResp.getJSONObject("data").getLong("id");
      Log.i(TAG, "Janus handle: " + handleId);

      // join as publisher
      JSONObject body = new JSONObject();
      body.put("request", "join");
      body.put("ptype", "publisher");
      body.put("room", roomId);
      if (!roomPin.isEmpty()) body.put("pin", roomPin);
      body.put("display", "android_rgb_publisher");
      JSONObject joinReq = baseJanus("message");
      joinReq.put("body", body);
      postJson(sessionHandleUrl(), joinReq);

      // create local offer and publish
      createOfferAndPublish();

      Log.i(TAG, "Janus publish offer sent, waiting for answer...");

      // poll janus events to receive answer + keepalive events
      while (started && sessionId != 0) {
        JSONObject evt = getJson(sessionUrl() + "?maxev=1&rid=" + System.currentTimeMillis());
        if (evt == null) continue;
        Log.i(TAG, "Janus event: " + evt.toString());

        String janusType = evt.optString("janus", "");
        if ("error".equals(janusType)) {
          throw new RuntimeException("Janus poll error: " + evt.toString());
        }
        if (!"event".equals(janusType)) {
          continue;
        }
        JSONObject jsep = evt.optJSONObject("jsep");
        if (jsep != null && "answer".equalsIgnoreCase(jsep.optString("type"))) {
          String sdp = jsep.optString("sdp", "");
          if (!sdp.isEmpty() && peerConnection != null) {
            SessionDescription answer = new SessionDescription(SessionDescription.Type.ANSWER, sdp);
            // peerConnection.setRemoteDescription(new SimpleSdpObserver(), answer);
            Log.i(TAG, "Janus answer applied");
            peerConnection.setRemoteDescription(
              new SdpObserver() {
                @Override public void onCreateSuccess(SessionDescription sd) {}

                @Override public void onSetSuccess() {
                  ready = true;
                  Log.i(TAG, "Janus answer applied; streamer is ready");
                }

                @Override public void onCreateFailure(String s) {}

                @Override public void onSetFailure(String s) {
                  Log.e(TAG, "setRemoteDescription failed: " + s);
                }
              },
              answer);
            Log.i(TAG, "Janus answer applied");
          }
        }
      }
    } catch (Exception e) {
      Log.e(TAG, "Janus setup failed", e);
      started = false;
    }
  }

  private void createOfferAndPublish() {
    if (peerConnection == null) return;

    MediaConstraints constraints = new MediaConstraints();
    constraints.mandatory.add(new MediaConstraints.KeyValuePair("OfferToReceiveVideo", "false"));
    constraints.mandatory.add(new MediaConstraints.KeyValuePair("OfferToReceiveAudio", "false"));

    peerConnection.createOffer(
        new SdpObserver() {
          @Override
          public void onCreateSuccess(SessionDescription offer) {
            if (peerConnection == null) return;

            peerConnection.setLocalDescription(
                new SdpObserver() {
                  @Override public void onCreateSuccess(SessionDescription sd) {}

                  @Override
                  public void onSetSuccess() {
                    try {
                      JSONObject body = new JSONObject();
                      body.put("request", "publish");
                      body.put("audio", false);
                      body.put("video", true);

                      JSONObject jsep = new JSONObject();
                      jsep.put("type", "offer");
                      jsep.put("sdp", offer.description);

                      JSONObject req = baseJanus("message");
                      req.put("body", body);
                      req.put("jsep", jsep);

                      JSONObject resp = postJson(sessionHandleUrl(), req);
                      Log.i(TAG, "Janus publish sent: " + resp.toString());
                    } catch (Exception e) {
                      Log.e(TAG, "Publish send failed", e);
                    }
                  }

                  @Override public void onCreateFailure(String s) {}

                  @Override
                  public void onSetFailure(String s) {
                    Log.e(TAG, "setLocalDescription failed: " + s);
                  }
                },
                offer);
          }

          @Override public void onSetSuccess() {}

          @Override
          public void onCreateFailure(String s) {
            Log.e(TAG, "createOffer failed: " + s);
          }

          @Override
          public void onSetFailure(String s) {
            Log.e(TAG, "createOffer/set failed: " + s);
          }
        },
        constraints);
  }

  private void sendTrickleCandidate(IceCandidate c) {
    if (!started || sessionId == 0 || handleId == 0) return;
    try {
      JSONObject cand = new JSONObject();
      cand.put("candidate", c.sdp);
      cand.put("sdpMid", c.sdpMid);
      cand.put("sdpMLineIndex", c.sdpMLineIndex);

      JSONObject req = baseJanus("trickle");
      req.put("candidate", cand);
      postJson(sessionHandleUrl(), req);
    } catch (Exception e) {
      Log.w(TAG, "Trickle failed", e);
    }
  }

  private JSONObject baseJanus(String janusType) throws Exception {
    JSONObject o = new JSONObject();
    o.put("janus", janusType);
    o.put("transaction", tx());
    if (!janusToken.isEmpty()) o.put("token", janusToken);
    if (!janusApiSecret.isEmpty()) o.put("apisecret", janusApiSecret);
    return o;
  }

  private String tx() {
    return UUID.randomUUID().toString().replace("-", "").substring(0, 12);
  }

  private String sessionUrl() {
    return janusBaseUrl + "/" + sessionId;
  }

  private String sessionHandleUrl() {
    return janusBaseUrl + "/" + sessionId + "/" + handleId;
  }

  private JSONObject postJson(String url, JSONObject body) throws Exception {
    HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
    conn.setConnectTimeout(8000);
    conn.setReadTimeout(10000);
    conn.setRequestMethod("POST");
    conn.setRequestProperty("Content-Type", "application/json");
    conn.setDoOutput(true);
    try (DataOutputStream dos = new DataOutputStream(conn.getOutputStream())) {
      dos.write(body.toString().getBytes());
      dos.flush();
    }
    int code = conn.getResponseCode();
    BufferedReader br =
        new BufferedReader(
            new InputStreamReader(
                code >= 200 && code < 300 ? conn.getInputStream() : conn.getErrorStream()));
    StringBuilder sb = new StringBuilder();
    String line;
    while ((line = br.readLine()) != null) {
      sb.append(line);
    }
    br.close();
    String text = sb.toString();
    if (code < 200 || code >= 300) {
      throw new RuntimeException("HTTP " + code + " " + text);
    }
    JSONObject out = new JSONObject(text);
    String janus = out.optString("janus", "");
    if ("error".equals(janus)) {
      throw new RuntimeException("Janus error: " + out.toString());
    }
    return out;
  }

  private JSONObject getJson(String url) throws Exception {
    try {
      HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
      conn.setConnectTimeout(8000);
      conn.setReadTimeout(15000);
      conn.setRequestMethod("GET");
      int code = conn.getResponseCode();
      if (code < 200 || code >= 300) {
        return null;
      }
      BufferedReader br = new BufferedReader(new InputStreamReader(conn.getInputStream()));
      StringBuilder sb = new StringBuilder();
      String line;
      while ((line = br.readLine()) != null) sb.append(line);
      br.close();
      String text = sb.toString();
      if (text.isEmpty()) return null;
      return new JSONObject(text);
    } catch (SocketTimeoutException timeout) {
      // Janus long-poll can legitimately timeout when no event is pending.
      return null;
    }
  }

  private static class SimpleSdpObserver implements SdpObserver {
    @Override
    public void onCreateSuccess(SessionDescription sessionDescription) {}

    @Override
    public void onSetSuccess() {}

    @Override
    public void onCreateFailure(String s) {}

    @Override
    public void onSetFailure(String s) {}
  }
}
