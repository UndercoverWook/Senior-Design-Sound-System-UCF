import 'package:flutter/material.dart';

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

  static const labels = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        SizedBox(
          height: 220,
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: List.generate(5, (i) {
              return Column(
                children: [
                  Text(labels[i]),
                  Expanded(
                    child: RotatedBox(
                      quarterTurns: -1,
                      child: Slider(
                        min: -12,
                        max: 12,
                        value: values[i],
                        onChanged: enabled ? (v) => onChange(i, v) : null,
                      ),
                    ),
                  ),
                  Text("${values[i].toStringAsFixed(1)} dB")
                ],
              );
            }),
          ),
        ),
        ElevatedButton(
          onPressed: enabled ? onResetPressed : null,
          child: Text("Reset EQ"),
        )
      ],
    );
  }
}
