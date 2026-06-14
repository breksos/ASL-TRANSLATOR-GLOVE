import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show HapticFeedback;

import '../config/app_config.dart';
import '../utils/label_format.dart';
import '../config/hardware_config.dart';
import '../models/ble_connection_state.dart';
import '../models/prediction.dart';
import '../services/ble_service.dart';
import '../services/inference_service.dart';
import '../services/preferences_service.dart';
import '../services/tts_service.dart';
import '../widgets/ble_status_widget.dart';
import '../widgets/prediction_display.dart';

/// Main interactive screen — GATED CAPTURE (mirrors scripts/record.py):
///   Listen -> 3-2-1 countdown -> SIGN NOW (3 s window) -> result
/// Letters mode accumulates a text buffer (Speak / Clear). Words mode speaks
/// the recognized word automatically.
enum _CapturePhase { idle, countdown, recording, processing }

class PredictScreen extends StatefulWidget {
  final BleService bleService;
  final InferenceService inferenceService;
  final TtsService ttsService;
  final PreferencesService prefs;

  const PredictScreen({
    super.key,
    required this.bleService,
    required this.inferenceService,
    required this.ttsService,
    required this.prefs,
  });

  @override
  State<PredictScreen> createState() => _PredictScreenState();
}

class _PredictScreenState extends State<PredictScreen> {
  Prediction? _last;
  BleConnectionState _bleState = BleConnectionState.idle;
  String _letterBuffer = '';
  late String _activeModel;
  bool _modelSwitchInFlight = false;

  _CapturePhase _phase = _CapturePhase.idle;
  int _countdown = 0;

  StreamSubscription<BleConnectionState>? _stateSub;

  @override
  void initState() {
    super.initState();
    _activeModel = widget.prefs.activeModel;
    _stateSub = widget.bleService.connectionState.listen((s) {
      if (mounted) setState(() => _bleState = s);
    });
  }

  @override
  void dispose() {
    _stateSub?.cancel();
    super.dispose();
  }

  bool get _isLetters => _activeModel == PreferencesService.modelLetters;
  bool get _isConnected => _bleState == BleConnectionState.connected;
  bool get _isBusy => _phase != _CapturePhase.idle || _modelSwitchInFlight;

  // ---- Capture flow -------------------------------------------------------

  Future<void> _onListenPressed() async {
    if (_isBusy) return;
    if (!_isConnected) {
      _snack('Connect the glove first.');
      return;
    }
    if (!widget.inferenceService.isLoaded) {
      _snack('Model not loaded.');
      return;
    }

    // Wake the glove (heat-saving: it idles between captures). The prep
    // countdown below gives the 4 slaves time to resume sampling before the
    // window starts.
    await widget.bleService.setStreaming(true);

    // Prep countdown — gives the signer time to form the handshape (or settle
    // into signing-rest for the dynamic letters J/Z before the motion).
    for (int i = AppConfig.capturePrepSeconds; i >= 1; i--) {
      if (!mounted) return;
      setState(() {
        _phase = _CapturePhase.countdown;
        _countdown = i;
      });
      HapticFeedback.selectionClick();
      await Future.delayed(const Duration(seconds: 1));
    }
    if (!mounted) return;

    // GO — record the window.
    setState(() => _phase = _CapturePhase.recording);
    HapticFeedback.mediumImpact();

    final pred = await widget.inferenceService.captureOnce();

    // Idle the glove again as soon as the window is captured.
    await widget.bleService.setStreaming(false);

    if (!mounted) return;
    setState(() => _phase = _CapturePhase.processing);
    await Future.delayed(const Duration(milliseconds: 120));
    if (!mounted) return;

    setState(() {
      _phase = _CapturePhase.idle;
      _last = pred;
    });

    if (pred == null) {
      // captureOnce distinguishes a real streaming failure from an inference
      // failure (e.g. a model/labels class-count mismatch) — show whichever it
      // actually was rather than always blaming the glove.
      _snack(widget.inferenceService.lastCaptureError ??
          'No data captured — is the glove streaming?');
      return;
    }

    final accepted = pred.confidence >= widget.prefs.confidenceThreshold;
    if (!accepted) {
      _snack('Low confidence '
          '(${(pred.confidence * 100).toStringAsFixed(0)} %) — try again.');
      return;
    }

    HapticFeedback.mediumImpact();
    if (_isLetters) {
      _appendLetter(pred.label);
    } else {
      // Words mode: speak the recognized word/phrase automatically, expanding
      // compact tokens (HOWAREYOU -> "HOW ARE YOU") so TTS pronounces them.
      widget.ttsService.speakIfNew(displayLabel(pred.label));
    }
  }

  void _appendLetter(String label) {
    final l = label.trim();
    setState(() {
      if (l.toLowerCase() == 'space') {
        _letterBuffer += ' ';
      } else if (l.toLowerCase() == 'backspace') {
        if (_letterBuffer.isNotEmpty) {
          _letterBuffer = _letterBuffer.substring(0, _letterBuffer.length - 1);
        }
      } else {
        _letterBuffer += l; // single character (or multi-char fallback)
      }
    });
  }

  // ---- Buttons ------------------------------------------------------------

  Future<void> _onConnectPressed() async {
    if (HardwareConfig.isPlaceholder) {
      _snack('Receiver MAC not configured. Set HardwareConfig.receiverMac '
          'in lib/config/hardware_config.dart and rebuild.');
      return;
    }
    await widget.bleService.connect(HardwareConfig.receiverMac);
  }

  Future<void> _onDisconnectPressed() async {
    await widget.bleService.disconnect();
  }

  void _onSpeakPressed() {
    if (_letterBuffer.trim().isEmpty) return;
    widget.ttsService.speak(_letterBuffer.trim());
  }

  void _onClearPressed() {
    setState(() {
      _letterBuffer = '';
      _last = null;
    });
  }

  Future<void> _onModelChanged(String? next) async {
    if (next == null || next == _activeModel || _isBusy) return;
    setState(() {
      _modelSwitchInFlight = true;
      _activeModel = next;
      _letterBuffer = '';
      _last = null;
    });
    await widget.prefs.setActiveModel(next);
    final ok = await widget.inferenceService.loadModel(
      modelAsset: widget.prefs.modelAssetPath,
      labelsAsset: widget.prefs.labelsAssetPath,
      statsAsset: widget.prefs.statsAssetPath,
      useRelativeFeatures: widget.prefs.useRelativeFeatures,
    );
    if (!mounted) return;
    setState(() => _modelSwitchInFlight = false);
    if (!ok) {
      _snack('Model "$next" not loaded — check assets/models/');
    }
  }

  void _snack(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
    );
  }

  // ---- UI -----------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    final modelLoaded = widget.inferenceService.isLoaded;
    return Scaffold(
      appBar: AppBar(
        title: const Text('ASL Glove'),
        actions: [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 8),
            child: Center(
              child: BleStatusWidget(
                state: _bleState,
                onTap:
                    _isConnected ? _onDisconnectPressed : _onConnectPressed,
              ),
            ),
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            if (!modelLoaded) _modelNotLoadedBanner(context),
            _modelSwitch(),
            if (_modelSwitchInFlight)
              const Padding(
                padding: EdgeInsets.only(top: 8),
                child: LinearProgressIndicator(),
              ),
            const SizedBox(height: 12),
            Expanded(flex: 3, child: _centerStage(context)),
            if (_isLetters) _letterBufferCard(context),
            const SizedBox(height: 12),
            _actionRow(context),
          ],
        ),
      ),
    );
  }

  Widget _modelNotLoadedBanner(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Card(
      color: cs.errorContainer,
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          children: [
            Icon(Icons.error_outline, color: cs.onErrorContainer),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                'Model not loaded. Place asl-$_activeModel.tflite + '
                'asl-$_activeModel-labels.txt in assets/models/ and restart.',
                style: TextStyle(color: cs.onErrorContainer),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _modelSwitch() {
    return SegmentedButton<String>(
      segments: const [
        ButtonSegment<String>(
          value: PreferencesService.modelLetters,
          label: Text('Letters'),
          icon: Icon(Icons.abc),
        ),
        ButtonSegment<String>(
          value: PreferencesService.modelWords,
          label: Text('Words'),
          icon: Icon(Icons.text_fields),
        ),
      ],
      selected: {_activeModel},
      onSelectionChanged:
          _isBusy ? null : (set) => _onModelChanged(set.first),
      showSelectedIcon: false,
    );
  }

  /// The big central area: shows the capture phase, or the last result.
  Widget _centerStage(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    switch (_phase) {
      case _CapturePhase.countdown:
        return _bigCircle(
          context,
          text: '$_countdown',
          caption: 'Get ready…',
          color: cs.secondaryContainer,
          fg: cs.onSecondaryContainer,
        );
      case _CapturePhase.recording:
        return _bigCircle(
          context,
          text: 'SIGN\nNOW',
          caption: 'Hold the sign (${AppConfig.captureWindow.inSeconds} s)…',
          color: cs.primary,
          fg: cs.onPrimary,
          pulse: true,
        );
      case _CapturePhase.processing:
        return _bigCircle(
          context,
          text: '…',
          caption: 'Reading…',
          color: cs.surfaceContainerHighest,
          fg: cs.onSurface,
        );
      case _CapturePhase.idle:
        if (_last == null) {
          return Center(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.front_hand_outlined, size: 64, color: cs.outline),
                const SizedBox(height: 12),
                Text('Press Listen and make a sign',
                    style: Theme.of(context).textTheme.titleMedium),
              ],
            ),
          );
        }
        return PredictionDisplay(
          prediction: _last,
          threshold: widget.prefs.confidenceThreshold,
        );
    }
  }

  Widget _bigCircle(
    BuildContext context, {
    required String text,
    required String caption,
    required Color color,
    required Color fg,
    bool pulse = false,
  }) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 160,
            height: 160,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            alignment: Alignment.center,
            child: Text(
              text,
              textAlign: TextAlign.center,
              style: TextStyle(
                color: fg,
                fontSize: 44,
                fontWeight: FontWeight.bold,
                height: 1.0,
              ),
            ),
          ),
          const SizedBox(height: 16),
          Text(caption, style: Theme.of(context).textTheme.titleMedium),
          if (pulse)
            const Padding(
              padding: EdgeInsets.only(top: 12),
              child: SizedBox(width: 120, child: LinearProgressIndicator()),
            ),
        ],
      ),
    );
  }

  Widget _letterBufferCard(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.only(top: 12),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: cs.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(12),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Text buffer', style: Theme.of(context).textTheme.labelMedium),
          const SizedBox(height: 4),
          Text(
            _letterBuffer.isEmpty ? '—' : _letterBuffer,
            style: Theme.of(context).textTheme.headlineSmall,
          ),
        ],
      ),
    );
  }

  Widget _actionRow(BuildContext context) {
    return Column(
      children: [
        // Primary: Listen (capture)
        SizedBox(
          width: double.infinity,
          child: FilledButton.icon(
            onPressed: (_isBusy || !_isConnected) ? null : _onListenPressed,
            icon: const Icon(Icons.mic),
            label: Text(_isConnected ? 'Listen' : 'Connect glove to listen'),
            style: FilledButton.styleFrom(
              padding: const EdgeInsets.symmetric(vertical: 16),
            ),
          ),
        ),
        const SizedBox(height: 8),
        // Wrap (not Row) so the buttons reflow instead of overflowing on
        // narrow screens.
        Wrap(
          alignment: WrapAlignment.center,
          spacing: 8,
          runSpacing: 8,
          children: [
            OutlinedButton.icon(
              onPressed:
                  _isConnected ? _onDisconnectPressed : _onConnectPressed,
              icon: Icon(
                  _isConnected ? Icons.bluetooth_disabled : Icons.bluetooth),
              label: Text(_isConnected ? 'Disconnect' : 'Connect'),
            ),
            if (_isLetters) ...[
              OutlinedButton.icon(
                onPressed:
                    _letterBuffer.trim().isEmpty ? null : _onSpeakPressed,
                icon: const Icon(Icons.volume_up),
                label: const Text('Speak'),
              ),
              OutlinedButton.icon(
                onPressed: _letterBuffer.isEmpty ? null : _onClearPressed,
                icon: const Icon(Icons.backspace_outlined),
                label: const Text('Clear'),
              ),
            ],
          ],
        ),
      ],
    );
  }
}
