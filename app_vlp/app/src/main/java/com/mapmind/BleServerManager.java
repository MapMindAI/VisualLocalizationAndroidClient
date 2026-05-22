package com.mapmind;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.UUID;

/**
 * Minimal BLE client manager used by robot control UI.
 */
public class BleServerManager {
  private static final String TAG = "[MapMind ble]";

  private static final UUID CCCD_UUID =
      UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

  private static final UUID SERVICE_UUID_MOKUKU =
      UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
  private static final UUID CHARACTERISTIC_UUID_MOKUKU =
      UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8");

  private static final UUID SERVICE_UUID_MOKUKU_BOT =
      UUID.fromString("000000ff-0000-1000-8000-00805f9b34fb");
  private static final UUID CHARACTERISTIC_UUID_MOKUKU_BOT =
      UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb");

  private static final int MAX_RECONNECT_ATTEMPTS = 100;
  private static final int RECONNECT_DELAY_MS = 2000;
  private static final long SCAN_PERIOD_MS = 20000;
  private static final int WRITE_MAX_RETRIES = 5;
  private static final int WRITE_RETRY_DELAY_MS = 50;

  public boolean isRobot = false;
  public String bleDeviceHeader = "mokuku";
  public String debugMsg = "";

  private final Context context;
  private final BluetoothAdapter bluetoothAdapter;
  private final Handler reconnectHandler = new Handler(Looper.getMainLooper());
  private final Handler stopScanHandler = new Handler(Looper.getMainLooper());

  private BluetoothGatt gatt;
  private BluetoothGattCharacteristic characteristic;
  private BluetoothDevice targetDevice;

  private boolean isReconnecting = false;
  private int reconnectAttempts = 0;

  private boolean isScanning = false;
  private BluetoothLeScanner bleScanner;
  private ScanCallback scanCallback;

  public BleServerManager(Context context) {
    this.context = context;
    BluetoothManager bluetoothManager =
        (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
    bluetoothAdapter = bluetoothManager == null ? null : bluetoothManager.getAdapter();
  }

  public void setTargetDevice(BluetoothDevice device) {
    targetDevice = device;
    scheduleReconnect();
  }

  public void scanMokukuDevices(DeviceScanListener listener) {
    if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled()) {
      debugMsg = "Bluetooth disabled";
      Log.e(TAG, debugMsg);
      return;
    }
    if (isScanning) {
      return;
    }

    listener.onScanStart();
    isScanning = true;
    bleScanner = bluetoothAdapter.getBluetoothLeScanner();
    if (bleScanner == null) {
      isScanning = false;
      debugMsg = "BLE scanner unavailable";
      Log.e(TAG, debugMsg);
      return;
    }

    scanCallback =
        new ScanCallback() {
          @Override
          public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            int rssi = result.getRssi();
            String name =
                result.getScanRecord() != null
                    ? result.getScanRecord().getDeviceName()
                    : device.getName();
            if (name != null && name.startsWith(bleDeviceHeader)) {
              listener.onDeviceFound(device, name + " [" + rssi + "]");
            }
          }

          @Override
          public void onBatchScanResults(List<ScanResult> results) {
            for (ScanResult result : results) {
              onScanResult(ScanSettings.CALLBACK_TYPE_ALL_MATCHES, result);
            }
          }

          @Override
          public void onScanFailed(int errorCode) {
            Log.e(TAG, "Scan failed: " + errorCode);
            debugMsg = "Scan failed: " + errorCode;
          }
        };

    try {
      bleScanner.startScan(scanCallback);
    } catch (SecurityException e) {
      isScanning = false;
      debugMsg = "Missing BLE scan permission";
      Log.e(TAG, debugMsg, e);
      return;
    }

    stopScanHandler.postDelayed(
        () -> {
          stopScan(listener);
          Log.i(TAG, "Scan timeout " + SCAN_PERIOD_MS + "ms");
        },
        SCAN_PERIOD_MS);
  }

  private void stopScan(DeviceScanListener listener) {
    if (!isScanning) {
      return;
    }
    isScanning = false;
    if (bleScanner != null && scanCallback != null) {
      try {
        bleScanner.stopScan(scanCallback);
      } catch (SecurityException ignored) {
      }
    }
    listener.onScanFinished();
  }

  public void startClient() {
    if (bluetoothAdapter == null) {
      debugMsg = "Bluetooth not supported";
      Log.e(TAG, debugMsg);
      return;
    }
    scheduleReconnect();
  }

  private void scheduleReconnect() {
    if (isReconnecting || bluetoothAdapter == null) {
      return;
    }

    isReconnecting = true;
    reconnectAttempts = 0;
    reconnectHandler.postDelayed(
        new Runnable() {
          @Override
          public void run() {
            if (!isReconnecting) {
              return;
            }

            if (gatt != null) {
              gatt.close();
              gatt = null;
              characteristic = null;
            }

            if (targetDevice == null) {
              reconnectHandler.postDelayed(this, RECONNECT_DELAY_MS);
              return;
            }

            debugMsg =
                "Reconnecting "
                    + safeName(targetDevice)
                    + " #"
                    + reconnectAttempts;
            try {
              gatt = targetDevice.connectGatt(context, false, gattCallback);
            } catch (SecurityException e) {
              debugMsg = "Missing BLE connect permission";
              Log.e(TAG, debugMsg, e);
              isReconnecting = false;
              return;
            }

            reconnectAttempts++;
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS && isReconnecting) {
              reconnectHandler.postDelayed(this, RECONNECT_DELAY_MS);
            } else {
              isReconnecting = false;
              debugMsg = "Reconnect stopped";
            }
          }
        },
        RECONNECT_DELAY_MS);
  }

  private final BluetoothGattCallback gattCallback =
      new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
          if (newState == BluetoothProfile.STATE_CONNECTED) {
            isReconnecting = false;
            debugMsg = "Connected " + safeName(gatt.getDevice());
            try {
              gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
              gatt.requestMtu(128);
            } catch (SecurityException e) {
              debugMsg = "Connected; no MTU permission";
            }
          } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
            debugMsg = "Disconnected";
            gatt.close();
            characteristic = null;
            scheduleReconnect();
          }
        }

        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
          if (status == BluetoothGatt.GATT_SUCCESS) {
            gatt.discoverServices();
          } else {
            gatt.discoverServices();
          }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
          if (status != BluetoothGatt.GATT_SUCCESS) {
            return;
          }
          final UUID serviceUuid = isRobot ? SERVICE_UUID_MOKUKU_BOT : SERVICE_UUID_MOKUKU;
          final UUID characteristicUuid =
              isRobot ? CHARACTERISTIC_UUID_MOKUKU_BOT : CHARACTERISTIC_UUID_MOKUKU;
          BluetoothGattService service = gatt.getService(serviceUuid);
          if (service == null) {
            debugMsg = "Service missing";
            return;
          }
          characteristic = service.getCharacteristic(characteristicUuid);
          if (characteristic == null) {
            debugMsg = "Characteristic missing";
            return;
          }
          final int props = characteristic.getProperties();
          if ((props & BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0) {
            characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
          } else {
            characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
          }

          gatt.setCharacteristicNotification(characteristic, true);
          BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCCD_UUID);
          if (descriptor != null) {
            descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            gatt.writeDescriptor(descriptor);
          }
          debugMsg = "Ready: " + safeName(gatt.getDevice());
        }
      };

  public void sendStringMessage(int id, String message) {
    if (characteristic == null || gatt == null || message == null) {
      return;
    }
    int len = message.length();
    if (len > 127) {
      Log.d(TAG, "String too long");
      return;
    }
    byte[] msgBytes = message.getBytes(StandardCharsets.UTF_8);
    byte[] payload = new byte[2 + msgBytes.length];
    payload[0] = (byte) id;
    payload[1] = (byte) len;
    System.arraycopy(msgBytes, 0, payload, 2, msgBytes.length);
    write(payload);
  }

  public void sendUint32Message(int id, int value) {
    if (characteristic == null || gatt == null) {
      return;
    }
    byte[] payload = new byte[5];
    payload[0] = (byte) id;
    payload[1] = (byte) (value & 0xFF);
    payload[2] = (byte) ((value >> 8) & 0xFF);
    payload[3] = (byte) ((value >> 16) & 0xFF);
    payload[4] = (byte) ((value >> 24) & 0xFF);
    write(payload);
  }

  private void write(byte[] payload) {
    writeInternal(payload, 0);
  }

  private void writeInternal(byte[] payload, int attempt) {
    if (characteristic == null || gatt == null) {
      return;
    }
    characteristic.setValue(payload);
    boolean ok;
    try {
      ok = gatt.writeCharacteristic(characteristic);
    } catch (SecurityException e) {
      Log.d(TAG, "BLE write security exception", e);
      return;
    }
    if (ok) {
      return;
    }
    if (attempt >= WRITE_MAX_RETRIES) {
      Log.d(TAG, "BLE write failed after retries");
      return;
    }
    final byte[] retryPayload = payload.clone();
    reconnectHandler.postDelayed(
        () -> writeInternal(retryPayload, attempt + 1), WRITE_RETRY_DELAY_MS);
  }

  public void shutdown() {
    isReconnecting = false;
    reconnectHandler.removeCallbacksAndMessages(null);
    stopScanHandler.removeCallbacksAndMessages(null);
    if (isScanning) {
      isScanning = false;
      if (bleScanner != null && scanCallback != null) {
        try {
          bleScanner.stopScan(scanCallback);
        } catch (SecurityException ignored) {
        }
      }
    }
    if (gatt != null) {
      gatt.close();
      gatt = null;
    }
    characteristic = null;
  }

  private static String safeName(BluetoothDevice device) {
    String n = device.getName();
    return n == null ? device.getAddress() : n;
  }
}
