/// BLE service and characteristic UUIDs and binary packet layout.
///
/// The receiver (ESP32 DevKit1) advertises a single GATT service with one
/// notify characteristic. Each notification carries exactly one timestep
/// (45 little-endian float32 = 180 bytes).
library;

class BleProtocol {
  /// Custom 128-bit service UUID. Must match the receiver firmware.
  static const String serviceUuid = '12345678-1234-5678-1234-56789abcdef0';

  /// Notify characteristic — receiver pushes sensor frames here.
  static const String sensorCharUuid = '12345678-1234-5678-1234-56789abcdef1';

  /// Control characteristic — app writes 0x01 (wake / start streaming) or
  /// 0x00 (sleep / idle) to gate the whole glove for heat saving.
  static const String controlCharUuid = '12345678-1234-5678-1234-56789abcdef2';

  /// Bytes per BLE notification (one timestep).
  /// 45 channels × 4 bytes (float32) = 180 bytes.
  static const int notificationByteSize = 180;

  /// Local-name prefix the receiver advertises. Used as a fallback when MAC
  /// scanning fails (e.g. iOS does not expose raw MAC). Must match the
  /// firmware's BLE_DEVICE_NAME ("ASL-Glove").
  static const String advertisedNamePrefix = 'ASL-Glove';
}

/// Names of the 45 channels in the order the receiver emits them.
const List<String> channelNames = [
  'thumb_P', 'thumb_R', 'thumb_Y',
  'thumb_gx', 'thumb_gy', 'thumb_gz',
  'thumb_ax', 'thumb_ay', 'thumb_az',
  'index_P', 'index_R', 'index_Y',
  'index_gx', 'index_gy', 'index_gz',
  'index_ax', 'index_ay', 'index_az',
  'middle_P', 'middle_R', 'middle_Y',
  'middle_gx', 'middle_gy', 'middle_gz',
  'middle_ax', 'middle_ay', 'middle_az',
  'ring_P', 'ring_R', 'ring_Y',
  'ring_gx', 'ring_gy', 'ring_gz',
  'ring_ax', 'ring_ay', 'ring_az',
  'pinky_P', 'pinky_R', 'pinky_Y',
  'pinky_gx', 'pinky_gy', 'pinky_gz',
  'pinky_ax', 'pinky_ay', 'pinky_az',
];
