import 'package:flutter/material.dart';

import '../constants/eq_bands.dart';

class EqSliders extends StatelessWidget {
  final List<double> values;
  final bool enabled;
  final void Function(int, double) onChange;
  final VoidCallback onResetPressed;

  const EqSliders({
    super.key,
    required this.values,
    required this.enabled,
    required this.onChange,
    required this.onResetPressed,
  });

  @override
  Widget build(BuildContext context) {
    final count = EqBands.labels.length;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SizedBox(
          height: 260,
          child: SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: List.generate(count, (i) {
                return SizedBox(
                  width: 82,
                  child: Column(
                    children: [
                      Text(
                        EqBands.labels[i],
                        textAlign: TextAlign.center,
                        style: const TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        EqBands.descriptors[i],
                        textAlign: TextAlign.center,
                        style: Theme.of(context).textTheme.bodySmall,
                        maxLines: 2,
                        overflow: TextOverflow.ellipsis,
                      ),
                      const SizedBox(height: 4),
                      Expanded(
                        child: RotatedBox(
                          quarterTurns: -1,
                          child: Slider(
                            min: -12,
                            max: 12,
                            divisions: 48,
                            value: i < values.length ? values[i] : 0,
                            onChanged: enabled ? (v) => onChange(i, v) : null,
                          ),
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        '${(i < values.length ? values[i] : 0).toStringAsFixed(1)} dB',
                        textAlign: TextAlign.center,
                        style: const TextStyle(fontSize: 12),
                      ),
                    ],
                  ),
                );
              }),
            ),
          ),
        ),
        const SizedBox(height: 12),
        ElevatedButton(
          onPressed: enabled ? onResetPressed : null,
          child: const Text('Reset EQ'),
        ),
      ],
    );
  }
}
