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
  private static final int FRAME_HEADER_SIZE = 64;
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

  void publishLatestFrame(long nativeApplication) {
    if (server == null || subscribers.isEmpty()) {
      return;
    }
    if (!JniInterface.hasLatestStreamFrame(nativeApplication)) {
      return;
    }

    byte[] yuvNv21 = JniInterface.getLatestYuvNv21Image(nativeApplication);
    long[] dimensionsAndTimestamp =
        JniInterface.getLatestStreamDimensionsAndTimestamp(nativeApplication);
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
    if (!hasReadySubscriber) {
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
