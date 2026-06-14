import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:tflite_flutter/tflite_flutter.dart';

import '../config/app_config.dart';
import '../models/prediction.dart';
import '../models/sensor_frame.dart';
import '../utils/normalizer.dart';
import '../utils/relative_features.dart';
import '../utils/rolling_buffer.dart';

/// Loads a TFLite model + label list, maintains a rolling window of incoming
/// [SensorFrame]s, and emits [Prediction]s at [AppConfig.inferenceInterval].
///
/// The model is expected to accept a `[1, AppConfig.windowTimesteps,
/// AppConfig.channelCount]` input and produce a `[1, num_classes]` output.
/// Both float32 and int8-quantized models are supported — quantized output
/// is dequantized using the model's published scale/zeroPoint before argmax.
class InferenceService {
  final Normalizer _normalizer = Normalizer();
  final RollingBuffer _buffer = RollingBuffer();
  final _predictionController = StreamController<Prediction>.broadcast();

  Stream<Prediction> get predictions => _predictionController.stream;

  Interpreter? _interpreter;
  List<String> _labels = const [];
  bool _useRelativeFeatures = true;
  bool _inputIsQuantized = false;
  double _inputScale = 1.0;
  int _inputZeroPoint = 0;
  bool _outputIsQuantized = false;
  double _outputScale = 1.0;
  int _outputZeroPoint = 0;
  Timer? _ticker;
  bool _disposed = false;

  bool get isLoaded => _interpreter != null && _labels.isNotEmpty;
  List<String> get labels => List.unmodifiable(_labels);

  Future<void> initNormalizer() => _normalizer.load();

  /// Replace the currently loaded model. Safe to call repeatedly when the
  /// user switches between Letters and Words. Returns false if model or
  /// labels asset could not be loaded; the caller (UI) should surface this.
  ///
  /// [statsAsset] is the per-model normalization file — Letters and Words
  /// have different channel distributions, so the matching StandardScaler
  /// mean/std must be swapped in together with the model.
  ///
  /// [useRelativeFeatures] selects the per-model feature transform: Letters
  /// train on inter-finger relative features, Words on absolute. Must match
  /// how the model's training data was preprocessed.
  Future<bool> loadModel({
    required String modelAsset,
    required String labelsAsset,
    required String statsAsset,
    required bool useRelativeFeatures,
  }) async {
    try {
      _useRelativeFeatures = useRelativeFeatures;
      await _normalizer.load(assetPath: statsAsset);
      _interpreter?.close();
      _interpreter = await Interpreter.fromAsset(modelAsset);
      final labelsRaw = await rootBundle.loadString(labelsAsset);
      _labels = labelsRaw
          .split('\n')
          .map((s) => s.trim())
          .where((s) => s.isNotEmpty)
          .toList();

      // Inspect INPUT tensor — int8-quantized EI models expect int8 input,
      // not float32. We must quantize the normalized values before feeding.
      final inT = _interpreter!.getInputTensor(0);
      _inputIsQuantized = inT.type != TensorType.float32;
      if (_inputIsQuantized) {
        _inputScale = inT.params.scale;
        _inputZeroPoint = inT.params.zeroPoint;
        debugPrint('InferenceService: input is quantized '
            '(scale=$_inputScale zp=$_inputZeroPoint)');
      }

      // Inspect output tensor — handle both float32 and quantized models.
      final outT = _interpreter!.getOutputTensor(0);
      _outputIsQuantized = outT.type != TensorType.float32;
      if (_outputIsQuantized) {
        _outputScale = outT.params.scale;
        _outputZeroPoint = outT.params.zeroPoint;
        debugPrint('InferenceService: loaded $modelAsset '
            '(${_labels.length} classes, quantized output, '
            'scale=$_outputScale zp=$_outputZeroPoint)');
      } else {
        debugPrint('InferenceService: loaded $modelAsset '
            '(${_labels.length} classes, float32 output)');
      }

      // Sanity-check labels vs model: model output dimension must match.
      // (Last shape entry is the class count.)
      final outShape = outT.shape;
      final outClasses = outShape.isNotEmpty ? outShape.last : 0;
      if (outClasses > 0 && outClasses != _labels.length) {
        debugPrint('InferenceService: WARNING — model outputs $outClasses '
            'classes but labels file has ${_labels.length}. Predictions will '
            'be padded with "?" past the labels list.');
      }

      _buffer.clear();
      return true;
    } catch (e, st) {
      debugPrint('InferenceService: failed to load $modelAsset: $e\n$st');
      _interpreter = null;
      _labels = const [];
      _inputIsQuantized = false;
      _outputIsQuantized = false;
      return false;
    }
  }

  /// Push one timestep into the rolling buffer. Caller (typically BLE service
  /// listener) should call this on every received [SensorFrame].
  int _pushCount = 0;
  void pushFrame(SensorFrame frame) {
    _buffer.push(frame);
    if (_pushCount++ % 25 == 0) {
      debugPrint('InferenceService: pushFrame #$_pushCount, '
          'buffer ${_buffer.length}/${_buffer.capacity}');
    }
  }

  /// Start the periodic inference timer.
  void start() {
    _ticker?.cancel();
    _ticker = Timer.periodic(AppConfig.inferenceInterval, (_) => _step());
  }

  void stop() {
    _ticker?.cancel();
    _ticker = null;
  }

  /// Legacy continuous-mode tick: snapshot whatever's in the buffer and infer.
  /// Unused by the gated-capture UI but kept for the optional continuous path.
  void _step() {
    if (_disposed) return;
    final flat = _buffer.snapshot();
    if (flat == null) return; // buffer not full yet
    final pred = _infer(flat);
    if (pred != null) _predictionController.add(pred);
  }

  /// Gated single-shot capture, mirroring scripts/record.py: clears the rolling
  /// buffer, waits for a FRESH full window (windowTimesteps frames) to stream
  /// in from BLE, then runs inference exactly once. Returns the [Prediction],
  /// or null if the model isn't loaded or the window didn't fill before
  /// [AppConfig.captureTimeout] (e.g. the link dropped mid-capture).
  ///
  /// The window length here is identical to the training window, so the model
  /// sees the same data shape at inference that it saw during training.
  Future<Prediction?> captureOnce() async {
    if (_disposed || _interpreter == null || _labels.isEmpty) return null;
    _buffer.clear();
    final deadline = DateTime.now().add(AppConfig.captureTimeout);
    while (!_buffer.isFull && DateTime.now().isBefore(deadline)) {
      await Future.delayed(const Duration(milliseconds: 20));
    }
    final flat = _buffer.snapshot();
    if (flat == null) {
      debugPrint('InferenceService: captureOnce timed out — buffer filled '
          '${_buffer.length}/${AppConfig.windowTimesteps} frames '
          '(0 => no frames reached the buffer; partial => stream too slow)');
      return null; // window never filled (no data / disconnected)
    }
    final pred = _infer(flat);
    if (pred != null) _predictionController.add(pred);
    return pred;
  }

  /// Run the model once on a flat window buffer. Applies normalization and
  /// handles both float32 and int8-quantized output. Returns null on failure.
  Prediction? _infer(List<double> flat) {
    if (_interpreter == null || _labels.isEmpty) return null;

    // Per-model feature transform (must run BEFORE normalization and match
    // the model's training preprocessing): Letters = relative (orientation-
    // invariant, scripts/relative_features.py); Words = absolute (skip).
    if (_useRelativeFeatures) {
      applyRelativeFeatures(flat);
    }
    _normalizer.apply(flat);

    // Build the input tensor. int8-quantized models need the normalized
    // values quantized to int8 via the input tensor's scale/zeroPoint;
    // float32 models take the values directly.
    final Object input;
    if (_inputIsQuantized) {
      final qin = List<int>.filled(flat.length, 0);
      for (int i = 0; i < flat.length; i++) {
        int q = (flat[i] / _inputScale).round() + _inputZeroPoint;
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        qin[i] = q;
      }
      input = qin.reshape([
        1,
        AppConfig.windowTimesteps,
        AppConfig.channelCount,
      ]);
    } else {
      input = Float32List.fromList(flat).reshape([
        1,
        AppConfig.windowTimesteps,
        AppConfig.channelCount,
      ]);
    }

    final n = _labels.length;
    List<double> scores;
    try {
      if (_outputIsQuantized) {
        // Allocate an int buffer; dequantize after run().
        final out = List.filled(n, 0).reshape([1, n]);
        _interpreter!.run(input, out);
        final raw = (out[0] as List).cast<int>();
        scores = raw
            .map((q) => _outputScale * (q - _outputZeroPoint))
            .toList();
      } else {
        final out = List.filled(n, 0.0).reshape([1, n]);
        _interpreter!.run(input, out);
        scores = (out[0] as List).cast<double>();
      }
    } catch (e) {
      debugPrint('InferenceService: run() failed: $e');
      return null;
    }

    // Argmax with defensive bounds check.
    int best = 0;
    double bestScore = scores[0];
    for (int i = 1; i < scores.length; i++) {
      if (scores[i] > bestScore) {
        best = i;
        bestScore = scores[i];
      }
    }
    final label = (best < _labels.length) ? _labels[best] : '?';
    return Prediction(
      label: label,
      confidence: bestScore.clamp(0.0, 1.0),
      timestamp: DateTime.now(),
      allClassScores: scores,
    );
  }

  Future<void> dispose() async {
    _disposed = true;
    stop();
    _interpreter?.close();
    _interpreter = null;
    await _predictionController.close();
  }
}
