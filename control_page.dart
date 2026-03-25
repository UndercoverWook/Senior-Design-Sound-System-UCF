import 'package:flutter/material.dart';
import '../widgets/eq_sliders.dart';
import '../widgets/eq_histogram.dart';

class ControlPage extends StatelessWidget {
  final bool connected;
  final double volume;
  final ValueChanged<double> onVolumeChanged;
  final List<double> eqValues;
  final List<double> spectrum;
  final void Function(int, double) onEqChanged;
  final VoidCallback onResetEq;
  final VoidCallback onPlayTestTone;

  const ControlPage({
    super.key,
    required this.connected,
    required this.volume,
    required this.onVolumeChanged,
    required this.eqValues,
    required this.spectrum,
    required this.onEqChanged,
    required this.onResetEq,
    required this.onPlayTestTone,
  });

  @override
  Widget build(BuildContext context) {
    final enabled = connected;
    final colorScheme = Theme.of(context).colorScheme;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text(
            "Master Volume",
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
          ),
          Slider(value: volume, onChanged: enabled ? onVolumeChanged : null),
          Text("${(volume * 100).toInt()}%"),

          const SizedBox(height: 20),

          ElevatedButton.icon(
            onPressed: enabled ? onPlayTestTone : null,
            icon: const Icon(Icons.play_arrow),
            label: const Text("Play Test Tone"),
          ),

          const SizedBox(height: 20),

          const Text(
            "Graphic EQ ±12 dB",
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
          ),
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: colorScheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(12),
            ),
            child: EqSliders(
              values: eqValues,
              enabled: enabled,
              onChange: onEqChanged,
              onResetPressed: onResetEq,
            ),
          ),

          const SizedBox(height: 20),

          EqHistogram(
            spectrum: spectrum,
            eqValues: eqValues,
            connected: connected,
          ),

          if (!connected)
            const Padding(
              padding: EdgeInsets.only(top: 12),
              child: Text("Not connected", style: TextStyle(color: Colors.red)),
            ),
        ],
      ),
    );
  }
}
