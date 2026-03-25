import 'dart:math' as math;
import 'package:flutter/material.dart';

class EqHistogram extends StatelessWidget {
  final List<double> spectrum; // raw FFT / measured magnitudes
  final List<double> eqValues; // EQ band gains, e.g. 10 sliders
  final bool connected;
  final double maxFrequency; // e.g. 500 Hz like your image
  final double minY;
  final double maxY;

  const EqHistogram({
    super.key,
    required this.spectrum,
    required this.eqValues,
    required this.connected,
    this.maxFrequency = 500,
    this.minY = 0,
    this.maxY = 80,
  });

  @override
  Widget build(BuildContext context) {
    if (!connected) {
      return Container(
        height: 300,
        alignment: Alignment.center,
        padding: const EdgeInsets.all(16),
        child: const Text(
          'Connect to device to view frequency spectrum',
          style: TextStyle(fontSize: 16),
        ),
      );
    }

    if (spectrum.isEmpty) {
      return Container(
        height: 300,
        alignment: Alignment.center,
        padding: const EdgeInsets.all(16),
        child: const Text(
          'No FFT data available yet',
          style: TextStyle(fontSize: 16),
        ),
      );
    }

    final adjustedSpectrum = _applyEqToSpectrum(spectrum, eqValues);

    return Container(
      height: 320,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Theme.of(context).dividerColor),
      ),
      child: Column(
        children: [
          const Text(
            'Frequency Spectrum',
            style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 8),
          Expanded(
            child: CustomPaint(
              painter: SpectrumPainter(
                rawSpectrum: spectrum,
                adjustedSpectrum: adjustedSpectrum,
                maxFrequency: maxFrequency,
                minY: minY,
                maxY: maxY,
              ),
              child: Container(),
            ),
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 16,
            runSpacing: 8,
            children: const [
              _LegendItem(color: Colors.blue, label: 'Measured'),
              _LegendItem(color: Colors.red, label: 'EQ Adjusted'),
            ],
          ),
        ],
      ),
    );
  }

  List<double> _applyEqToSpectrum(List<double> raw, List<double> eq) {
    if (eq.isEmpty) return List<double>.from(raw);

    // Example center frequencies for 10-band EQ
    final List<double> centers = [
      31.25,
      62.5,
      125,
      250,
      500,
      1000,
      2000,
      4000,
      8000,
      16000,
    ];

    final int n = raw.length;
    final List<double> adjusted = [];

    for (int i = 0; i < n; i++) {
      final freq = (i / (n - 1)) * maxFrequency;

      // Interpolate EQ gain at this frequency
      final gain = _interpolateEqGain(freq, centers, eq);

      // Add gain to measured magnitude
      adjusted.add((raw[i] + gain).clamp(minY, maxY));
    }

    return adjusted;
  }

  double _interpolateEqGain(
    double freq,
    List<double> centers,
    List<double> eq,
  ) {
    if (eq.isEmpty) return 0;

    final usableLength = math.min(centers.length, eq.length);
    final c = centers.sublist(0, usableLength);
    final g = eq.sublist(0, usableLength);

    if (freq <= c.first) return g.first;
    if (freq >= c.last) return g.last;

    for (int i = 0; i < c.length - 1; i++) {
      if (freq >= c[i] && freq <= c[i + 1]) {
        final t = (freq - c[i]) / (c[i + 1] - c[i]);
        return g[i] + (g[i + 1] - g[i]) * t;
      }
    }

    return 0;
  }
}

class SpectrumPainter extends CustomPainter {
  final List<double> rawSpectrum;
  final List<double> adjustedSpectrum;
  final double maxFrequency;
  final double minY;
  final double maxY;

  SpectrumPainter({
    required this.rawSpectrum,
    required this.adjustedSpectrum,
    required this.maxFrequency,
    required this.minY,
    required this.maxY,
  });

  @override
  void paint(Canvas canvas, Size size) {
    const double leftPad = 42;
    const double rightPad = 12;
    const double topPad = 12;
    const double bottomPad = 32;

    final plotRect = Rect.fromLTWH(
      leftPad,
      topPad,
      size.width - leftPad - rightPad,
      size.height - topPad - bottomPad,
    );

    final axisPaint = Paint()
      ..color = Colors.black87
      ..strokeWidth = 1.2;

    final gridPaint = Paint()
      ..color = Colors.grey.shade400
      ..strokeWidth = 0.8;

    final rawPaint = Paint()
      ..color = Colors.blue
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final adjustedPaint = Paint()
      ..color = Colors.red
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    // Background
    final bgPaint = Paint()..color = Colors.white;
    canvas.drawRect(plotRect, bgPaint);

    // Grid + Y labels
    const yTicks = 8;
    for (int i = 0; i <= yTicks; i++) {
      final y = plotRect.bottom - (i / yTicks) * plotRect.height;
      canvas.drawLine(
        Offset(plotRect.left, y),
        Offset(plotRect.right, y),
        gridPaint,
      );

      final value = minY + (i / yTicks) * (maxY - minY);
      _drawText(
        canvas,
        value.toStringAsFixed(0),
        Offset(4, y - 8),
        const TextStyle(fontSize: 10, color: Colors.black),
      );
    }

    // Grid + X labels
    const xStep = 50;
    for (int f = 0; f <= maxFrequency.toInt(); f += xStep) {
      final x = plotRect.left + (f / maxFrequency) * plotRect.width;
      canvas.drawLine(
        Offset(x, plotRect.top),
        Offset(x, plotRect.bottom),
        gridPaint,
      );

      _drawText(
        canvas,
        '$f',
        Offset(x - 8, plotRect.bottom + 4),
        const TextStyle(fontSize: 10, color: Colors.black),
      );
    }

    // Axes
    canvas.drawLine(
      Offset(plotRect.left, plotRect.top),
      Offset(plotRect.left, plotRect.bottom),
      axisPaint,
    );
    canvas.drawLine(
      Offset(plotRect.left, plotRect.bottom),
      Offset(plotRect.right, plotRect.bottom),
      axisPaint,
    );

    // Raw spectrum
    if (rawSpectrum.length > 1) {
      final rawPath = _buildPath(rawSpectrum, plotRect);
      canvas.drawPath(rawPath, rawPaint);
    }

    // Adjusted spectrum
    if (adjustedSpectrum.length > 1) {
      final adjustedPath = _buildPath(adjustedSpectrum, plotRect);
      canvas.drawPath(adjustedPath, adjustedPaint);
    }

    // Axis labels
    _drawText(
      canvas,
      'frequency (Hz)',
      Offset(plotRect.left + plotRect.width / 2 - 30, size.height - 18),
      const TextStyle(fontSize: 11, color: Colors.black),
    );

    _drawText(
      canvas,
      'Magnitude',
      const Offset(2, 0),
      const TextStyle(fontSize: 11, color: Colors.black),
    );
  }

  Path _buildPath(List<double> values, Rect plotRect) {
    final path = Path();

    for (int i = 0; i < values.length; i++) {
      final x = plotRect.left + (i / (values.length - 1)) * plotRect.width;
      final normalized = ((values[i] - minY) / (maxY - minY)).clamp(0.0, 1.0);
      final y = plotRect.bottom - normalized * plotRect.height;

      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }

    return path;
  }

  void _drawText(Canvas canvas, String text, Offset offset, TextStyle style) {
    final textPainter = TextPainter(
      text: TextSpan(text: text, style: style),
      textDirection: TextDirection.ltr,
    )..layout();

    textPainter.paint(canvas, offset);
  }

  @override
  bool shouldRepaint(covariant SpectrumPainter oldDelegate) {
    return oldDelegate.rawSpectrum != rawSpectrum ||
        oldDelegate.adjustedSpectrum != adjustedSpectrum ||
        oldDelegate.maxFrequency != maxFrequency ||
        oldDelegate.minY != minY ||
        oldDelegate.maxY != maxY;
  }
}

class _LegendItem extends StatelessWidget {
  final Color color;
  final String label;

  const _LegendItem({required this.color, required this.label});

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(width: 18, height: 3, color: color),
        const SizedBox(width: 6),
        Text(label),
      ],
    );
  }
}
