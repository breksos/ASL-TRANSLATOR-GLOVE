import 'dart:typed_data';

import '../config/app_config.dart';
import '../config/ble_protocol.dart';
import '../models/sensor_frame.dart';

/// Parses one BLE notification payload into a [SensorFrame].
///
/// Returns null if the byte count is wrong, the receiver firmware schema is
/// out of sync with [BleProtocol.notificationByteSize], or any float is NaN.
SensorFrame? parseBlePacket(List<int> bytes) {
  if (bytes.length != BleProtocol.notificationByteSize) return null;
  final bd = ByteData.view(Uint8List.fromList(bytes).buffer);
  final ch = List<double>.filled(AppConfig.channelCount, 0);
  for (int i = 0; i < AppConfig.channelCount; i++) {
    final v = bd.getFloat32(i * 4, Endian.little);
    if (v.isNaN || v.isInfinite) return null;
    ch[i] = v;
  }
  return SensorFrame(DateTime.now(), ch);
}
