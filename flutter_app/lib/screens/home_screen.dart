import 'package:flutter/material.dart';

import '../services/ble_service.dart';
import '../services/inference_service.dart';
import '../services/preferences_service.dart';
import '../services/tts_service.dart';
import 'predict_screen.dart';
import 'settings_screen.dart';

/// Bottom-nav shell with Predict and Settings tabs.
class HomeScreen extends StatefulWidget {
  final BleService bleService;
  final InferenceService inferenceService;
  final TtsService ttsService;
  final PreferencesService prefs;

  const HomeScreen({
    super.key,
    required this.bleService,
    required this.inferenceService,
    required this.ttsService,
    required this.prefs,
  });

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  int _index = 0;

  @override
  Widget build(BuildContext context) {
    final pages = <Widget>[
      PredictScreen(
        bleService: widget.bleService,
        inferenceService: widget.inferenceService,
        ttsService: widget.ttsService,
        prefs: widget.prefs,
      ),
      SettingsScreen(
        prefs: widget.prefs,
        ttsService: widget.ttsService,
      ),
    ];
    return Scaffold(
      body: IndexedStack(index: _index, children: pages),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (i) => setState(() => _index = i),
        destinations: const [
          NavigationDestination(
              icon: Icon(Icons.sign_language), label: 'Predict'),
          NavigationDestination(
              icon: Icon(Icons.settings), label: 'Settings'),
        ],
      ),
    );
  }
}
