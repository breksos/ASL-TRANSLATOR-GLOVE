import 'package:shared_preferences/shared_preferences.dart';

import '../config/app_config.dart';

/// Thin wrapper around SharedPreferences for the only two settings the user
/// must configure (MAC, active model) plus a few optional tunables.
class PreferencesService {
  static const _keyMac = 'receiver_mac';
  static const _keyModel = 'active_model';
  static const _keyConfidence = 'confidence_threshold';
  static const _keyTtsEnabled = 'tts_enabled';
  static const _keyTtsLang = 'tts_language';
  static const _keyAutoClear = 'auto_clear_seconds';

  /// Available models the user can choose between.
  static const String modelLetters = 'letters';
  static const String modelWords = 'words';

  late final SharedPreferences _prefs;

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
  }

  // MAC -----------------------------------------------------------------
  String get receiverMac => _prefs.getString(_keyMac) ?? '';
  Future<void> setReceiverMac(String mac) =>
      _prefs.setString(_keyMac, mac.trim().toUpperCase());

  /// Validates xx:xx:xx:xx:xx:xx format.
  static bool isValidMac(String mac) =>
      RegExp(r'^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$').hasMatch(mac.trim());

  // Active model -------------------------------------------------------
  String get activeModel => _prefs.getString(_keyModel) ?? modelLetters;
  Future<void> setActiveModel(String m) => _prefs.setString(_keyModel, m);

  /// Asset path for the user's active model.
  String get modelAssetPath => 'assets/models/asl-$activeModel.tflite';

  /// Asset path for the labels file matching the active model.
  String get labelsAssetPath => 'assets/models/asl-$activeModel-labels.txt';

  /// Asset path for the normalization stats matching the active model.
  /// Letters and Words have different channel distributions, so each model
  /// ships its own StandardScaler mean/std (see scripts/compute_stats.py).
  String get statsAssetPath => 'assets/normalization/stats-$activeModel.json';

  /// Whether the active model expects the orientation-invariant relative
  /// transform. As of Letters v3 BOTH models are dynamic gestures trained on
  /// ABSOLUTE features, so this is always false. History (measured live):
  ///   static letters + absolute -> failed (calibration drift);
  ///   static letters + relative -> failed (thumb-cluster sensor resolution,
  ///     e.g. T misread as A);
  ///   dynamic + absolute -> works (motion is calibration-independent).
  /// The relative transform (utils/relative_features.dart +
  /// scripts/relative_features.py) is kept for reference and the report.
  bool get useRelativeFeatures => false;

  // Confidence ---------------------------------------------------------
  double get confidenceThreshold =>
      _prefs.getDouble(_keyConfidence) ?? AppConfig.defaultConfidenceThreshold;
  Future<void> setConfidenceThreshold(double v) =>
      _prefs.setDouble(_keyConfidence, v.clamp(0.0, 1.0));

  // TTS ----------------------------------------------------------------
  bool get ttsEnabled => _prefs.getBool(_keyTtsEnabled) ?? true;
  Future<void> setTtsEnabled(bool v) => _prefs.setBool(_keyTtsEnabled, v);

  String get ttsLanguage => _prefs.getString(_keyTtsLang) ?? 'en-US';
  Future<void> setTtsLanguage(String v) => _prefs.setString(_keyTtsLang, v);

  // Letters auto-clear -------------------------------------------------
  int get autoClearSeconds =>
      _prefs.getInt(_keyAutoClear) ?? AppConfig.defaultAutoClearSeconds;
  Future<void> setAutoClearSeconds(int v) =>
      _prefs.setInt(_keyAutoClear, v.clamp(0, 60));
}
