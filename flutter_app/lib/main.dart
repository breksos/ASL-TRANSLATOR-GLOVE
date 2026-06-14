import 'package:flutter/material.dart';

import 'screens/home_screen.dart';
import 'services/ble_service.dart';
import 'services/inference_service.dart';
import 'services/preferences_service.dart';
import 'services/tts_service.dart';
import 'theme/app_theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final prefs = PreferencesService();
  await prefs.init();

  final inference = InferenceService();
  // Try to load the user's selected model on launch (loadModel also loads the
  // matching per-model normalization stats). Failures are logged and surfaced
  // in the UI by InferenceService.isLoaded == false.
  await inference.loadModel(
    modelAsset: prefs.modelAssetPath,
    labelsAsset: prefs.labelsAssetPath,
    statsAsset: prefs.statsAssetPath,
    useRelativeFeatures: prefs.useRelativeFeatures,
  );

  final tts = TtsService();
  await tts.init(language: prefs.ttsLanguage);
  tts.setEnabled(prefs.ttsEnabled);

  final ble = BleService();

  // Pipe BLE → inference rolling buffer. Inference runs on demand via the
  // gated "Listen" capture (InferenceService.captureOnce), not a continuous
  // ticker — this keeps the inference window identical to the training /
  // record.py window. Frames stream continuously into the buffer regardless.
  ble.frames.listen(inference.pushFrame);

  runApp(AslGloveApp(
    bleService: ble,
    inferenceService: inference,
    ttsService: tts,
    prefs: prefs,
  ));
}

class AslGloveApp extends StatelessWidget {
  final BleService bleService;
  final InferenceService inferenceService;
  final TtsService ttsService;
  final PreferencesService prefs;

  const AslGloveApp({
    super.key,
    required this.bleService,
    required this.inferenceService,
    required this.ttsService,
    required this.prefs,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ASL Glove',
      theme: AppTheme.light(),
      darkTheme: AppTheme.dark(),
      home: HomeScreen(
        bleService: bleService,
        inferenceService: inferenceService,
        ttsService: ttsService,
        prefs: prefs,
      ),
    );
  }
}
