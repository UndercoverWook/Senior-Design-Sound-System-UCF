import 'package:flutter/material.dart';

import '../constants/eq_bands.dart';

class EqHistogram extends StatelessWidget {
  final List<double> spectrum;
  final List<double> eqValues;
  final bool connected;
  final double minY;
  final double maxY;

  const EqHistogram({
    super.key,
    required this.spectrum,
    required this.eqValues,
    required this.connected,
    this.minY = 0,
    this.maxY = 80,
  });

  static const List<double> bandCenters = EqBands.centersHz;
  static const List<String> bandLabels = EqBands.shortLabels;

  @override
  Widget build(BuildContext context) {
    if (!connected) {
      return Container(
        height: 340,
        alignment: Alignment.center,
        padding: const EdgeInsets.all(16),
        child: const Text(
          'Connect to device to view calibration data',
          style: TextStyle(fontSize: 16),
        ),
      );
    }

    if (spectrum.isEmpty) {
      return Container(
        height: 340,
        alignment: Alignment.center,
        padding: const EdgeInsets.all(16),
        child: const Text(
          'No calibration FFT data available yet',
          style: TextStyle(fontSize: 16),
        ),
      );
    }

    final adjustedSpectrum = _applyEqToSpectrum(spectrum, eqValues);

    return Container(
      height: 340,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Theme.of(context).dividerColor),
      ),
      child: Column(
        children: [
          const Text(
            'Calibration Frequency Spectrum',
            style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 8),
          Expanded(
            child: CustomPaint(
              painter: SpectrumPainter(
                rawSpectrum: spectrum,
                adjustedSpectrum: adjustedSpectrum,
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

    final usableLength = eq.length < bandCenters.length
        ? eq.length
        : bandCenters.length;
    final centers = bandCenters.sublist(0, usableLength);
    final gains = eq.sublist(0, usableLength);

    return List<double>.generate(raw.length, (i) {
      final freq = i < bandCenters.length ? bandCenters[i] : bandCenters.last;
      final gain = _interpolateEqGain(freq, centers, gains);
      return (raw[i] + gain).clamp(minY, maxY);
    });
  }

  double _interpolateEqGain(
    double freq,
    List<double> centers,
    List<double> gains,
  ) {
    if (gains.isEmpty) return 0;
    if (freq <= centers.first) return gains.first;
    if (freq >= centers.last) return gains.last;

    for (int i = 0; i < centers.length - 1; i++) {
      if (freq >= centers[i] && freq <= centers[i + 1]) {
        final t = (freq - centers[i]) / (centers[i + 1] - centers[i]);
        return gains[i] + (gains[i + 1] - gains[i]) * t;
      }
    }
    return 0;
  }
}

class SpectrumPainter extends CustomPainter {
  final List<double> rawSpectrum;
  final List<double> adjustedSpectrum;
  final double minY;
  final double maxY;

  SpectrumPainter({
    required this.rawSpectrum,
    required this.adjustedSpectrum,
    required this.minY,
    required this.maxY,
  });

  @override
  void paint(Canvas canvas, Size size) {
    const double leftPad = 52;
    const double rightPad = 12;
    const double topPad = 12;
    const double bottomPad = 40;

    final plotRect = Rect.fromLTWH(
      leftPad,
      topPad,
      size.width - leftPad - rightPad,
      size.height - topPad - bottomPad,
    );

    final colorScheme =
        WidgetsBinding.instance.platformDispatcher.platformBrightness ==
        Brightness.dark;
    final axisColor = colorScheme ? Colors.white70 : Colors.black87;
    final gridColor = colorScheme ? Colors.white24 : Colors.grey.shade400;
    final bgColor = colorScheme ? Colors.black12 : Colors.white;

    final axisPaint = Paint()
      ..color = axisColor
      ..strokeWidth = 1.2;
    final gridPaint = Paint()
      ..color = gridColor
      ..strokeWidth = 0.8;
    final rawPaint = Paint()
      ..color = Colors.blue
      ..strokeWidth = 1.7
      ..style = PaintingStyle.stroke;
    final adjustedPaint = Paint()
      ..color = Colors.red
      ..strokeWidth = 1.7
      ..style = PaintingStyle.stroke;

    canvas.drawRect(plotRect, Paint()..color = bgColor);

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
        Offset(6, y - 8),
        TextStyle(fontSize: 10, color: axisColor),
      );
    }

    final tickCount = EqHistogram.bandLabels.length;
    for (int i = 0; i < tickCount; i++) {
      final x = _xForIndex(i, tickCount, plotRect);
      canvas.drawLine(
        Offset(x, plotRect.top),
        Offset(x, plotRect.bottom),
        gridPaint,
      );
      _drawText(
        canvas,
        EqHistogram.bandLabels[i],
        Offset(x - 12, plotRect.bottom + 4),
        TextStyle(fontSize: 10, color: axisColor),
      );
    }

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

    if (rawSpectrum.length > 1) {
      canvas.drawPath(_buildPath(rawSpectrum, plotRect), rawPaint);
    }
    if (adjustedSpectrum.length > 1) {
      canvas.drawPath(_buildPath(adjustedSpectrum, plotRect), adjustedPaint);
    }

    _drawText(
      canvas,
      'Frequency (Hz)',
      Offset(plotRect.left + plotRect.width / 2 - 34, size.height - 20),
      TextStyle(fontSize: 11, color: axisColor),
    );
    _drawText(
      canvas,
      'Magnitude',
      const Offset(2, 0),
      TextStyle(fontSize: 11, color: axisColor),
    );
  }

  Path _buildPath(List<double> values, Rect plotRect) {
    final path = Path();
    final total = values.length < 2 ? 2 : values.length;
    for (int i = 0; i < values.length; i++) {
      final x = _xForIndex(i, total, plotRect);
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

  double _xForIndex(int index, int totalCount, Rect plotRect) {
    if (totalCount <= 1) return plotRect.left;
    final t = index / (totalCount - 1);
    return plotRect.left + t * plotRect.width;
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
