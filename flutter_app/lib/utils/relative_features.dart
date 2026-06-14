import '../config/app_config.dart';

/// Makes handshape features invariant to global hand orientation, exactly
/// mirroring scripts/relative_features.py.
///
/// For the orientation channels (pitch, roll, yaw, ax, ay, az) of each finger,
/// subtract the per-timestep MEAN across the 5 fingers. Any rotation/offset
/// common to all fingers (global hand tilt, calibration drift) cancels; only
/// the inter-finger differences — the actual handshape — survive. Gyro
/// channels (gx, gy, gz) are angular rate (motion), already orientation-
/// independent, so they pass through unchanged.
///
/// MUST stay identical to scripts/relative_features.py (same field list, same
/// math) or the model will see different features in training vs inference.
///
/// Applied in-place to a flat window buffer of length
/// windowTimesteps * channelCount, BEFORE z-score normalization.
const int _fieldsPerFinger = 9;
const int _numFingers = 5;
// Mean-centered fields within each finger: 0=P 1=R 2=Y 6=ax 7=ay 8=az.
// gyro (3,4,5) left absolute. Keep in sync with relative_features.py.
const List<int> _orientationFields = [0, 1, 2, 6, 7, 8];

void applyRelativeFeatures(List<double> flat) {
  const n = AppConfig.channelCount; // 45
  final timesteps = flat.length ~/ n;
  for (int t = 0; t < timesteps; t++) {
    final base = t * n;
    for (final fj in _orientationFields) {
      double sum = 0.0;
      for (int fi = 0; fi < _numFingers; fi++) {
        sum += flat[base + fi * _fieldsPerFinger + fj];
      }
      final mean = sum / _numFingers;
      for (int fi = 0; fi < _numFingers; fi++) {
        flat[base + fi * _fieldsPerFinger + fj] -= mean;
      }
    }
  }
}
