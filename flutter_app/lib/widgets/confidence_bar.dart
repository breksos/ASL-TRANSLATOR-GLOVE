import 'package:flutter/material.dart';

/// Horizontal progress bar showing 0..1 confidence with a colour change at
/// the threshold and a visible tick marking the threshold position.
class ConfidenceBar extends StatelessWidget {
  final double value;        // 0..1
  final double threshold;    // 0..1
  final double height;

  const ConfidenceBar({
    super.key,
    required this.value,
    required this.threshold,
    this.height = 14,
  });

  Color _colorFor(BuildContext context) {
    if (value >= threshold) {
      return Theme.of(context).colorScheme.primary;
    }
    if (value >= threshold * 0.7) {
      return Colors.orange;
    }
    return Colors.redAccent;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final width = constraints.maxWidth;
        final tickLeft =
            (threshold.clamp(0.0, 1.0) * width).clamp(0.0, width - 2.0);
        return Stack(
          children: [
            // Track
            Container(
              height: height,
              width: width,
              decoration: BoxDecoration(
                color: Theme.of(context).colorScheme.surfaceContainerHighest,
                borderRadius: BorderRadius.circular(height / 2),
              ),
            ),
            // Filled portion
            FractionallySizedBox(
              widthFactor: value.clamp(0.0, 1.0),
              child: Container(
                height: height,
                decoration: BoxDecoration(
                  color: _colorFor(context),
                  borderRadius: BorderRadius.circular(height / 2),
                ),
              ),
            ),
            // Threshold tick
            Positioned(
              left: tickLeft,
              top: 0,
              child: Container(
                height: height,
                width: 2,
                color: Colors.black.withValues(alpha: 0.45),
              ),
            ),
          ],
        );
      },
    );
  }
}
