/// Coarse-grained BLE connection state for UI display.
enum BleConnectionState {
  idle,
  scanning,
  connecting,
  connected,
  reconnecting,
  disconnected,
  permissionDenied,
  bluetoothOff,
  receiverNotFound,
  error,
}

extension BleConnectionStateX on BleConnectionState {
  String get displayName {
    switch (this) {
      case BleConnectionState.idle:
        return 'Not connected';
      case BleConnectionState.scanning:
        return 'Scanning…';
      case BleConnectionState.connecting:
        return 'Connecting…';
      case BleConnectionState.connected:
        return 'Connected';
      case BleConnectionState.reconnecting:
        return 'Reconnecting…';
      case BleConnectionState.disconnected:
        return 'Disconnected';
      case BleConnectionState.permissionDenied:
        return 'Bluetooth permission needed';
      case BleConnectionState.bluetoothOff:
        return 'Turn on Bluetooth';
      case BleConnectionState.receiverNotFound:
        return 'Receiver not found';
      case BleConnectionState.error:
        return 'Error';
    }
  }

  bool get isActive =>
      this == BleConnectionState.scanning ||
      this == BleConnectionState.connecting ||
      this == BleConnectionState.reconnecting;
}
