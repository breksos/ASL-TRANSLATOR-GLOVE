import '../config/app_config.dart';
import '../models/sensor_frame.dart';

/// Fixed-size circular buffer holding the most recent N [SensorFrame]s.
///
/// On every [push], the oldest sample is discarded once the buffer fills.
/// [snapshot] returns the contents in chronological order as a flat
/// `List<double>` of length `N × channelCount`, ready to feed into a
/// `Reshape((N, channelCount))` model input.
class RollingBuffer {
  final int capacity;
  final List<SensorFrame?> _slots;
  int _next = 0;
  int _filled = 0;

  RollingBuffer({this.capacity = AppConfig.windowTimesteps})
      : _slots = List<SensorFrame?>.filled(capacity, null);

  /// Whether enough samples have been pushed to fill a full window.
  bool get isFull => _filled >= capacity;

  /// Number of frames currently stored (0..capacity).
  int get length => _filled;

  /// Append a frame, evicting the oldest if full.
  void push(SensorFrame frame) {
    _slots[_next] = frame;
    _next = (_next + 1) % capacity;
    if (_filled < capacity) _filled++;
  }

  /// Returns the buffer's contents as a flat List in chronological order.
  /// Length is `capacity * AppConfig.channelCount`. Calling this when
  /// `!isFull` returns null (caller should wait).
  List<double>? snapshot() {
    if (!isFull) return null;
    final out = List<double>.filled(capacity * AppConfig.channelCount, 0.0);
    // Iterate from oldest (_next) to newest (_next - 1 mod capacity).
    for (int i = 0; i < capacity; i++) {
      final f = _slots[(_next + i) % capacity]!;
      final base = i * AppConfig.channelCount;
      for (int c = 0; c < AppConfig.channelCount; c++) {
        out[base + c] = f.channels[c];
      }
    }
    return out;
  }

  void clear() {
    for (int i = 0; i < capacity; i++) {
      _slots[i] = null;
    }
    _next = 0;
    _filled = 0;
  }
}
