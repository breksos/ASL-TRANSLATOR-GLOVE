import '../config/app_config.dart';

/// One 20 ms sensor sample from the glove.
///
/// Holds [AppConfig.channelCount] float values in the order documented in
/// `config/ble_protocol.dart`'s [channelNames].
class SensorFrame {
  final DateTime timestamp;
  final List<double> channels;

  SensorFrame(this.timestamp, this.channels)
      : assert(channels.length == AppConfig.channelCount,
            'expected ${AppConfig.channelCount} channels, got ${channels.length}');

  @override
  String toString() =>
      'SensorFrame(t=$timestamp, ${channels.length} channels)';
}
