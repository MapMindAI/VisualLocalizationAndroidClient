package com.mapmind;

import android.bluetooth.BluetoothDevice;

public interface DeviceScanListener {
  void onScanStart();
  void onDeviceFound(BluetoothDevice device, String displayName);
  void onDeviceConnect(BluetoothDevice device, String displayName);
  void onScanFinished();
}
