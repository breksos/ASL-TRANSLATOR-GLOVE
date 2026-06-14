/// Result of one model inference pass.
class Prediction {
  final String label;
  final double confidence;
  final DateTime timestamp;
  final List<double>? allClassScores;

  Prediction({
    required this.label,
    required this.confidence,
    required this.timestamp,
    this.allClassScores,
  });

  @override
  String toString() =>
      'Prediction($label, ${(confidence * 100).toStringAsFixed(1)}%)';
}
