package com.google.ar.core.examples.c.helloar;

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
  private static final int FRAME_HEADER_MAGIC = 0x564c5031; // "VLP1"
  private static final int FRAME_HEADER_SIZE = 64;

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

  void publishLatestFrame(long nativeApplication) {
    if (server == null || subscribers.isEmpty()) {
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

    int width = (int) dimensionsAndTimestamp[1];
    int height = (int) dimensionsAndTimestamp[2];
    long timestampNs = dimensionsAndTimestamp[0];
    if (timestampNs == lastBroadcastTimestampNs) {
      // No new frame since last publish.
      return;
    }
    int expected = width * height;
    if (width <= 0 || height <= 0 || gray.length < expected) {
      return;
    }

    ByteBuffer buffer =
        ByteBuffer.allocate(FRAME_HEADER_SIZE + expected).order(ByteOrder.LITTLE_ENDIAN);
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
    buffer.put(gray, 0, expected);
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
