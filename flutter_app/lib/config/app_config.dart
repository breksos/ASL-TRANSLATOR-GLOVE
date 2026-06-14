/// Global constants for the ASL Glove app.
///
/// All numeric values that govern data shape, timing, or RF protocol live
/// here so changes to the firmware / model are mirrored by changing one file.
library;

class AppConfig {
  /// Number of sensor channels per timestep emitted by the receiver.
  /// 5 fingers × 9 fields (pitch, roll, yaw, gx, gy, gz, ax, ay, az).
  /// Palm was dropped after A/B testing showed no accuracy benefit.
  static const int channelCount = 45;

  /// Sample rate of the incoming sensor stream.
  /// Master firmware sends one frame per 40 ms cycle = 25 Hz.
  static const int sampleRateHz = 25;

  /// Inference window length, in timesteps.
  /// 3000 ms @ 25 Hz = 75 timesteps. MUST match the Edge Impulse impulse
  /// window AND scripts/record.py's capture window — the model input is
  /// [1, windowTimesteps, channelCount].
  static const int windowTimesteps = 75;

  /// Capture window duration, derived from the two constants above
  /// (75 / 25 Hz = 3000 ms). This is how long the gated "Listen" capture
  /// records, mirroring record.py.
  static const Duration captureWindow =
      Duration(milliseconds: windowTimesteps * 1000 ~/ sampleRateHz);

  /// Prep countdown shown before a capture starts ("3-2-1 → GO"), giving the
  /// signer time to settle into the handshape (or into signing-rest for the
  /// dynamic letters J/Z). Does not affect the captured data window.
  static const int capturePrepSeconds = 3;

  /// Hard timeout for a single capture: window + slack for BLE latency. If the
  /// buffer hasn't filled by then, the capture aborts (likely disconnected).
  static const Duration captureTimeout =
      Duration(milliseconds: windowTimesteps * 1000 ~/ sampleRateHz + 2000);

  /// How often to run inference in legacy continuous mode (unused by the
  /// gated-capture UI, kept for the optional continuous path).
  static const Duration inferenceInterval = Duration(milliseconds: 200);

  /// Below this softmax confidence, predictions are shown but not spoken.
  static const double defaultConfidenceThreshold = 0.7;

  /// Speech-dedup window. Two predictions of the same label within this
  /// timeframe are spoken only once.
  static const Duration ttsDebounce = Duration(milliseconds: 800);

  /// BLE scan timeout when looking for a specific MAC.
  static const Duration scanTimeout = Duration(seconds: 10);

  /// Reconnect backoff schedule when the link drops.
  static const List<Duration> reconnectBackoff = [
    Duration(seconds: 1),
    Duration(seconds: 2),
    Duration(seconds: 4),
    Duration(seconds: 8),
  ];

  /// Default Letters-mode auto-clear duration (seconds).
  static const int defaultAutoClearSeconds = 5;
}
