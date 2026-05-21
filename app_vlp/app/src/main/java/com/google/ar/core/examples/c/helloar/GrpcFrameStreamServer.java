package com.google.ar.core.examples.c.helloar;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.graphics.YuvImage;
import android.util.Log;
import io.grpc.MethodDescriptor;
import io.grpc.Server;
import io.grpc.ServerServiceDefinition;
import io.grpc.netty.shaded.io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.ServerCallStreamObserver;
import io.grpc.stub.ServerCalls;
import io.grpc.stub.StreamObserver;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Iterator;
import java.util.concurrent.CopyOnWriteArrayList;

class GrpcFrameStreamServer {
  private static final String TAG = "GrpcFrameStreamServer";
  private static final String SERVICE_NAME = "vlp.FrameStreamService";
  private static final int FRAME_HEADER_MAGIC = 0x564c5032; // "VLP2"
  private static final int RECORD_FRAME_MAGIC = 0x564c5033; // "VLP3"
  private static final int FRAME_HEADER_SIZE = 64;
  private static final int EXT_HEADER_SIZE = 20; // jpeg_len, depth_w, depth_h, depth_fmt, depth_len
  private static final int DEPTH_FORMAT_U16_MM = 1;
  private static final byte[] RECORD_FILE_MAGIC = new byte[] {'V', 'L', 'P', 'R', 'E', 'C', '1', '\n'};
  private static final int RECORD_FILE_VERSION = 1;
  private static final int RECORD_ENTRY_HEADER_SIZE = 12; // <QI>
  private static final int JPEG_QUALITY = 80;

  private static final MethodDescriptor.Marshaller<byte[]> BYTE_ARRAY_MARSHALLER =
      new MethodDescriptor.Marshaller<byte[]>() {
        @Override
        public InputStream stream(byte[] value) {
          return new ByteArrayInputStream(value);
        }

        @Override
        public byte[] parse(InputStream stream) {
          ByteArrayOutputStream output = new ByteArrayOutputStream();
          byte[] buffer = new byte[4096];
          int read;
          try {
            while ((read = stream.read(buffer)) != -1) {
              output.write(buffer, 0, read);
            }
          } catch (IOException e) {
            return new byte[0];
          }
          return output.toByteArray();
        }
      };

  private static final MethodDescriptor<byte[], byte[]> STREAM_FRAMES_METHOD =
      MethodDescriptor.<byte[], byte[]>newBuilder()
          .setType(MethodDescriptor.MethodType.SERVER_STREAMING)
          .setFullMethodName(
              MethodDescriptor.generateFullMethodName(SERVICE_NAME, "StreamFrames"))
          .setRequestMarshaller(BYTE_ARRAY_MARSHALLER)
          .setResponseMarshaller(BYTE_ARRAY_MARSHALLER)
          .build();

  private final int port;
  private final CopyOnWriteArrayList<ServerCallStreamObserver<byte[]>> subscribers =
      new CopyOnWriteArrayList<>();
  private volatile long lastBroadcastTimestampNs = Long.MIN_VALUE;
  private final Object recordLock = new Object();
  private volatile boolean recordingEnabled = false;
  private FileOutputStream recordOutputStream = null;
  private long recordStartTimestampNs = Long.MIN_VALUE;
  private Server server;

  GrpcFrameStreamServer(int port) {
    this.port = port;
  }

  void start() {
    if (server != null) {
      return;
    }
    ServerServiceDefinition service =
        ServerServiceDefinition.builder(SERVICE_NAME)
            .addMethod(
                STREAM_FRAMES_METHOD,
                ServerCalls.asyncServerStreamingCall(this::handleStreamFrames))
            .build();

    try {
      server = NettyServerBuilder.forPort(port).addService(service).build().start();
      Log.i(TAG, "gRPC server started on port " + port);
    } catch (IOException e) {
      Log.e(TAG, "Failed to start gRPC server", e);
      server = null;
    }
  }

  void stop(int timeoutMs) throws InterruptedException {
    if (server == null) {
      return;
    }
    stopRecording();
    for (ServerCallStreamObserver<byte[]> observer : subscribers) {
      try {
        observer.onCompleted();
      } catch (Exception ignored) {
      }
    }
    subscribers.clear();
    server.shutdownNow();
    server.awaitTermination(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS);
    server = null;
  }

  private void handleStreamFrames(
      byte[] request, StreamObserver<byte[]> responseObserver) {
    ServerCallStreamObserver<byte[]> serverObserver =
        (ServerCallStreamObserver<byte[]>) responseObserver;
    serverObserver.setOnCancelHandler(() -> subscribers.remove(serverObserver));
    subscribers.add(serverObserver);
    Log.i(TAG, "Client subscribed, subscribers=" + subscribers.size());
  }

  boolean hasSubscribers() {
    Iterator<ServerCallStreamObserver<byte[]>> iter = subscribers.iterator();
    while (iter.hasNext()) {
      ServerCallStreamObserver<byte[]> observer = iter.next();
      if (observer.isCancelled()) {
        subscribers.remove(observer);
        continue;
      }
      return true;
    }
    return false;
  }

  boolean isRecordingEnabled() {
    return recordingEnabled;
  }

  boolean startRecording(String path) {
    synchronized (recordLock) {
      stopRecordingLocked();
      try {
        File outputFile = new File(path);
        File parent = outputFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
          Log.e(TAG, "Failed to create record parent folder: " + parent.getAbsolutePath());
          return false;
        }
        recordOutputStream = new FileOutputStream(outputFile);
        ByteBuffer fileHeader = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        fileHeader.put(RECORD_FILE_MAGIC);
        fileHeader.putInt(RECORD_FILE_VERSION);
        recordOutputStream.write(fileHeader.array());
        recordOutputStream.flush();
        recordStartTimestampNs = Long.MIN_VALUE;
        recordingEnabled = true;
        Log.i(TAG, "Recording started: " + outputFile.getAbsolutePath());
        return true;
      } catch (IOException e) {
        Log.e(TAG, "Failed to start recording", e);
        stopRecordingLocked();
        return false;
      }
    }
  }

  void stopRecording() {
    synchronized (recordLock) {
      stopRecordingLocked();
    }
  }

  private void stopRecordingLocked() {
    recordingEnabled = false;
    recordStartTimestampNs = Long.MIN_VALUE;
    if (recordOutputStream != null) {
      try {
        recordOutputStream.flush();
        recordOutputStream.close();
      } catch (IOException ignored) {
      }
      recordOutputStream = null;
      Log.i(TAG, "Recording stopped.");
    }
  }

  private void appendRecordFrame(long timestampNs, byte[] payload) {
    synchronized (recordLock) {
      if (!recordingEnabled || recordOutputStream == null) {
        return;
      }
      try {
        if (recordStartTimestampNs == Long.MIN_VALUE) {
          recordStartTimestampNs = timestampNs;
        }
        long relNs = Math.max(0L, timestampNs - recordStartTimestampNs);
        ByteBuffer hdr = ByteBuffer.allocate(RECORD_ENTRY_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        hdr.putLong(relNs);
        hdr.putInt(payload.length);
        recordOutputStream.write(hdr.array());
        recordOutputStream.write(payload);
      } catch (IOException e) {
        Log.e(TAG, "Recording write failed. Stopping recorder.", e);
        stopRecordingLocked();
      }
    }
  }

  void publishLatestFrame(long nativeApplication) {
    if (server == null) {
      return;
    }
    if (subscribers.isEmpty() && !recordingEnabled) {
      return;
    }
    if (!JniInterface.hasLatestStreamFrame(nativeApplication)) {
      return;
    }

    byte[] yuvNv21 = JniInterface.getLatestYuvNv21Image(nativeApplication);
    byte[] depth16 = null;
    long[] dimensionsAndTimestamp =
        JniInterface.getLatestStreamDimensionsAndTimestamp(nativeApplication);
    long[] depthDimsAndTimestamp = null;
    if (recordingEnabled) {
      depth16 = JniInterface.getLatestDepth16Image(nativeApplication);
      depthDimsAndTimestamp = JniInterface.getLatestDepthDimensionsAndTimestamp(nativeApplication);
    }
    float[] intrinsics = JniInterface.getLatestStreamIntrinsics(nativeApplication);
    float[] pose = JniInterface.getLatestStreamPose(nativeApplication);

    if (yuvNv21 == null
        || dimensionsAndTimestamp == null
        || dimensionsAndTimestamp.length < 3
        || intrinsics == null
        || intrinsics.length < 4
        || pose == null
        || pose.length < 7) {
      return;
    }

    int width = (int) dimensionsAndTimestamp[1];
    int height = (int) dimensionsAndTimestamp[2];
    long timestampNs = dimensionsAndTimestamp[0];
    if (timestampNs == lastBroadcastTimestampNs) {
      // No new frame since last publish.
      return;
    }
    int expectedYuv = width * height * 3 / 2;
    if (width <= 0 || height <= 0 || yuvNv21.length < expectedYuv) {
      return;
    }

    boolean hasReadySubscriber = false;
    for (ServerCallStreamObserver<byte[]> observer : subscribers) {
      if (!observer.isCancelled() && observer.isReady()) {
        hasReadySubscriber = true;
        break;
      }
    }
    if (!hasReadySubscriber && !recordingEnabled) {
      return;
    }

    ByteArrayOutputStream jpegStream = new ByteArrayOutputStream();
    YuvImage yuvImage = new YuvImage(yuvNv21, ImageFormat.NV21, width, height, null);
    boolean compressed =
        yuvImage.compressToJpeg(new Rect(0, 0, width, height), JPEG_QUALITY, jpegStream);
    if (!compressed) {
      return;
    }
    byte[] jpegBytes = jpegStream.toByteArray();
    if (jpegBytes.length == 0) {
      return;
    }

    ByteBuffer buffer =
        ByteBuffer.allocate(FRAME_HEADER_SIZE + jpegBytes.length).order(ByteOrder.LITTLE_ENDIAN);
    buffer.putInt(FRAME_HEADER_MAGIC);
    buffer.putLong(timestampNs); // timestamp_ns
    buffer.putInt(width);
    buffer.putInt(height);
    buffer.putFloat(intrinsics[0]); // fx
    buffer.putFloat(intrinsics[1]); // fy
    buffer.putFloat(intrinsics[2]); // cx
    buffer.putFloat(intrinsics[3]); // cy
    buffer.putFloat(pose[0]); // qx
    buffer.putFloat(pose[1]); // qy
    buffer.putFloat(pose[2]); // qz
    buffer.putFloat(pose[3]); // qw
    buffer.putFloat(pose[4]); // tx
    buffer.putFloat(pose[5]); // ty
    buffer.putFloat(pose[6]); // tz
    buffer.put(jpegBytes);
    byte[] payload = buffer.array();

    if (recordingEnabled) {
      int depthWidth = 0;
      int depthHeight = 0;
      int depthLen = 0;
      if (depth16 != null && depthDimsAndTimestamp != null && depthDimsAndTimestamp.length >= 3) {
        depthWidth = (int) depthDimsAndTimestamp[1];
        depthHeight = (int) depthDimsAndTimestamp[2];
        depthLen = depth16.length;
        if (depthWidth <= 0 || depthHeight <= 0 || depthLen <= 0) {
          depthWidth = 0;
          depthHeight = 0;
          depthLen = 0;
        }
      }

      ByteBuffer recordBuffer =
          ByteBuffer.allocate(FRAME_HEADER_SIZE + EXT_HEADER_SIZE + jpegBytes.length + depthLen)
              .order(ByteOrder.LITTLE_ENDIAN);
      recordBuffer.putInt(RECORD_FRAME_MAGIC);
      recordBuffer.putLong(timestampNs);
      recordBuffer.putInt(width);
      recordBuffer.putInt(height);
      recordBuffer.putFloat(intrinsics[0]);
      recordBuffer.putFloat(intrinsics[1]);
      recordBuffer.putFloat(intrinsics[2]);
      recordBuffer.putFloat(intrinsics[3]);
      recordBuffer.putFloat(pose[0]);
      recordBuffer.putFloat(pose[1]);
      recordBuffer.putFloat(pose[2]);
      recordBuffer.putFloat(pose[3]);
      recordBuffer.putFloat(pose[4]);
      recordBuffer.putFloat(pose[5]);
      recordBuffer.putFloat(pose[6]);
      recordBuffer.putInt(jpegBytes.length);
      recordBuffer.putInt(depthWidth);
      recordBuffer.putInt(depthHeight);
      recordBuffer.putInt(depthLen > 0 ? DEPTH_FORMAT_U16_MM : 0);
      recordBuffer.putInt(depthLen);
      recordBuffer.put(jpegBytes);
      if (depthLen > 0) {
        recordBuffer.put(depth16, 0, depthLen);
      }
      appendRecordFrame(timestampNs, recordBuffer.array());
    }

    Iterator<ServerCallStreamObserver<byte[]>> iter = subscribers.iterator();
    while (iter.hasNext()) {
      ServerCallStreamObserver<byte[]> observer = iter.next();
      if (observer.isCancelled()) {
        subscribers.remove(observer);
        continue;
      }
      if (!observer.isReady()) {
        // Skip this frame to prevent queue buildup and long streaming latency.
        continue;
      }
      try {
        observer.onNext(payload);
      } catch (Exception e) {
        subscribers.remove(observer);
      }
    }
    lastBroadcastTimestampNs = timestampNs;
  }
}
