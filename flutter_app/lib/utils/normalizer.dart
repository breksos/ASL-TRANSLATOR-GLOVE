import 'dart:convert';

import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter/foundation.dart';

import '../config/app_config.dart';

/// Applies per-channel z-score normalization to a flat window buffer.
///
/// During Edge Impulse training we use scikit-learn StandardScaler. The
/// learned per-channel mean and std are exported to `assets/normalization/
/// stats.json` and applied here at inference time so the model sees inputs
/// on the same scale.
///
/// If stats.json is missing or malformed we fall back to identity (no
/// transform) and log a warning. The model will still run but accuracy will
/// degrade — useful for first-boot before you've copied the file in.
class Normalizer {
  List<double>? _mean;
  List<double>? _std;
  bool get isLoaded => _mean != null && _std != null;

  Future<void> load({String assetPath = 'assets/normalization/stats.json'}) async {
    // Reset first so a failed load never silently keeps the PREVIOUS model's
    // stats (worse than identity when switching between Letters and Words).
    _mean = null;
    _std = null;
    try {
      final raw = await rootBundle.loadString(assetPath);
      final parsed = jsonDecode(raw) as Map<String, dynamic>;
      final mean = (parsed['mean'] as List).cast<num>().map((e) => e.toDouble()).toList();
      final std = (parsed['std'] as List).cast<num>().map((e) => e.toDouble()).toList();
      if (mean.length != AppConfig.channelCount || std.length != AppConfig.channelCount) {
        debugPrint('Normalizer: stats.json length mismatch '
            '(mean=${mean.length}, std=${std.length}, '
            'expected ${AppConfig.channelCount}). Falling back to identity.');
        return;
      }
      _mean = mean;
      _std = std.map((s) => s == 0 ? 1.0 : s).toList(); // guard div-by-zero
      debugPrint('Normalizer: loaded ${_mean!.length}-channel stats.');
    } catch (e) {
      debugPrint('Normalizer: could not load $assetPath ($e). '
          'Inference will use raw values — model accuracy will suffer.');
    }
  }

  /// Normalize a flat snapshot in-place. No-op if stats are not loaded.
  void apply(List<double> snapshot) {
    if (!isLoaded) return;
    final mean = _mean!;
    final std = _std!;
    const n = AppConfig.channelCount;
    for (int i = 0; i < snapshot.length; i++) {
      final c = i % n;
      snapshot[i] = (snapshot[i] - mean[c]) / std[c];
    }
  }
}
