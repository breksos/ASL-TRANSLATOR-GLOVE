import 'package:flutter/material.dart';

import '../config/hardware_config.dart';
import '../services/preferences_service.dart';
import '../services/tts_service.dart';

/// Tunables only. The two formerly-required fields (MAC, active model) moved:
///   * MAC → baked into `lib/config/hardware_config.dart` (production constant)
///   * Active model → SegmentedButton on the Predict screen (primary control)
class SettingsScreen extends StatefulWidget {
  final PreferencesService prefs;
  final TtsService ttsService;

  const SettingsScreen({
    super.key,
    required this.prefs,
    required this.ttsService,
  });

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  late double _conf;
  late bool _tts;
  late String _ttsLang;
  late int _autoClear;

  @override
  void initState() {
    super.initState();
    _conf = widget.prefs.confidenceThreshold;
    _tts = widget.prefs.ttsEnabled;
    _ttsLang = widget.prefs.ttsLanguage;
    _autoClear = widget.prefs.autoClearSeconds;
  }

  Future<void> _save() async {
    await widget.prefs.setConfidenceThreshold(_conf);
    await widget.prefs.setTtsEnabled(_tts);
    await widget.prefs.setTtsLanguage(_ttsLang);
    await widget.prefs.setAutoClearSeconds(_autoClear);

    widget.ttsService.setEnabled(_tts);
    final langOk = await widget.ttsService.setLanguage(_ttsLang);

    if (!mounted) return;
    if (!langOk && _tts) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Saved, but TTS language "$_ttsLang" is not '
              'installed on this device. Install it in system settings or '
              'pick another language.'),
          duration: const Duration(seconds: 5),
        ),
      );
    } else {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Saved.')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          if (HardwareConfig.isPlaceholder)
            Card(
              color: Theme.of(context).colorScheme.errorContainer,
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Row(
                  children: [
                    const Icon(Icons.warning_amber_rounded),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        'Receiver MAC still set to placeholder. Edit '
                        'lib/config/hardware_config.dart before shipping.',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          if (HardwareConfig.isPlaceholder) const SizedBox(height: 16),
          Text('Recognition',
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          Text('Confidence threshold: ${(_conf * 100).toStringAsFixed(0)} %'),
          Slider(
            value: _conf,
            min: 0,
            max: 1,
            divisions: 20,
            label: '${(_conf * 100).toStringAsFixed(0)} %',
            onChanged: (v) => setState(() => _conf = v),
          ),
          const SizedBox(height: 8),
          Text(
            'Predictions below this confidence appear in the history but '
            'are not spoken via TTS.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const Divider(height: 32),
          Text('Voice output',
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          SwitchListTile(
            value: _tts,
            onChanged: (v) => setState(() => _tts = v),
            title: const Text('Speak predictions'),
            subtitle: const Text('Read out the recognised sign'),
          ),
          DropdownButtonFormField<String>(
            value: _ttsLang,
            decoration: const InputDecoration(
              labelText: 'Voice language',
              border: OutlineInputBorder(),
            ),
            items: const [
              DropdownMenuItem(value: 'en-US', child: Text('English (US)')),
              DropdownMenuItem(value: 'en-GB', child: Text('English (UK)')),
              DropdownMenuItem(value: 'tr-TR', child: Text('Türkçe')),
              DropdownMenuItem(value: 'de-DE', child: Text('Deutsch')),
            ],
            onChanged: (v) {
              if (v != null) setState(() => _ttsLang = v);
            },
          ),
          const Divider(height: 32),
          Text('Letters mode',
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          Text('Auto-clear text buffer after $_autoClear s'),
          Slider(
            value: _autoClear.toDouble(),
            min: 0,
            max: 30,
            divisions: 30,
            label: '$_autoClear s',
            onChanged: (v) => setState(() => _autoClear = v.toInt()),
          ),
          Text('0 = never clear automatically. Use the Clear button on the '
              'Predict screen for manual control.',
              style: Theme.of(context).textTheme.bodySmall),
          const SizedBox(height: 24),
          FilledButton.icon(
            onPressed: _save,
            icon: const Icon(Icons.save),
            label: const Text('Save'),
          ),
          const SizedBox(height: 32),
          Text('About', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          const Text(
            'GTU CSE496 graduation project.\n'
            'Receiver firmware expects BLE service '
            '12345678-1234-5678-1234-56789abcdef0 with notify '
            'characteristic …def1, sending 45 little-endian float32 per '
            'packet at 50 Hz.',
            style: TextStyle(fontSize: 12),
          ),
        ],
      ),
    );
  }
}
