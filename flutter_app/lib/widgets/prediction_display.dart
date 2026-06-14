import 'package:flutter/material.dart';

import '../models/prediction.dart';
import 'confidence_bar.dart';

/// Large display showing the most recent prediction label and confidence.
class PredictionDisplay extends StatelessWidget {
  final Prediction? prediction;
  final double threshold;

  const PredictionDisplay({
    super.key,
    required this.prediction,
    required this.threshold,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    if (prediction == null) {
      return Center(
        child: Text(
          '…',
          style: theme.textTheme.displayLarge?.copyWith(
            color: theme.colorScheme.outline,
          ),
        ),
      );
    }
    final p = prediction!;
    final isStrong = p.confidence >= threshold;
    return Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        Text(
          p.label,
          style: theme.textTheme.displayLarge?.copyWith(
            color: isStrong
                ? theme.colorScheme.primary
                : theme.colorScheme.outline,
          ),
        ),
        const SizedBox(height: 24),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 32),
          child: ConfidenceBar(
            value: p.confidence,
            threshold: threshold,
          ),
        ),
        const SizedBox(height: 8),
        Text(
          '${(p.confidence * 100).toStringAsFixed(1)} % confidence',
          style: theme.textTheme.bodyMedium,
        ),
      ],
    );
  }
}
