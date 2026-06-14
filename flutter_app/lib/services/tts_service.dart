import 'package:flutter/foundation.dart';
import 'package:flutter_tts/flutter_tts.dart';

import '../config/app_config.dart';

/// Text-to-speech wrapper that deduplicates back-to-back identical
/// utterances and gracefully degrades if the requested language isn't
/// installed on the device.
class TtsService {
  final FlutterTts _tts = FlutterTts();
  String _lastSpoken = '';
  DateTime _lastSpokeAt = DateTime.fromMillisecondsSinceEpoch(0);
  bool _enabled = true;
  String _language = 'en-US';

  Future<void> init({String language = 'en-US'}) async {
    await setLanguage(language);
    await _tts.setSpeechRate(0.5);
    await _tts.setVolume(1.0);
    await _tts.setPitch(1.0);
  }

  void setEnabled(bool v) {
    _enabled = v;
    if (!v) _tts.stop();
  }

  /// Set the TTS language. Returns `true` if the requested language was set,
  /// `false` if it isn't available on the device — in that case the previous
  /// language stays active and the UI should surface the failure.
  Future<bool> setLanguage(String lang) async {
    if (lang.isEmpty) return false;
    try {
      final available = await _tts.isLanguageAvailable(lang);
      if (available != true) {
        debugPrint('TtsService: language $lang not installed on device; '
            'keeping $_language');
        return false;
      }
      await _tts.setLanguage(lang);
      _language = lang;
      return true;
    } catch (e) {
      debugPrint('TtsService: setLanguage($lang) threw $e');
      return false;
    }
  }

  /// Speaks [text] unconditionally (bypasses the dedup window). Use for an
  /// explicit user action like the "Speak" button, where re-speaking the same
  /// text on purpose must work.
  Future<void> speak(String text) async {
    if (!_enabled || text.isEmpty) return;
    _lastSpoken = text;
    _lastSpokeAt = DateTime.now();
    try {
      await _tts.stop();
      await _tts.speak(text);
    } catch (e) {
      debugPrint('TtsService: speak failed: $e');
    }
  }

  /// Speaks [text] only if (a) TTS is enabled, (b) it differs from the last
  /// spoken phrase OR enough time has passed since it was last spoken.
  Future<void> speakIfNew(String text) async {
    if (!_enabled || text.isEmpty) return;
    final now = DateTime.now();
    if (text == _lastSpoken &&
        now.difference(_lastSpokeAt) < AppConfig.ttsDebounce) {
      return;
    }
    _lastSpoken = text;
    _lastSpokeAt = now;
    try {
      await _tts.stop();
      await _tts.speak(text);
    } catch (e) {
      debugPrint('TtsService: speak failed: $e');
    }
  }

  Future<void> dispose() async {
    await _tts.stop();
  }
}
