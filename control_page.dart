import 'package:flutter/material.dart';
import '../widgets/eq_sliders.dart';

class ControlPage extends StatelessWidget {
  final bool connected;
  final double volume;
  final ValueChanged<double> onVolumeChanged;
  final List<double> eqValues;
  final void Function(int, double) onEqChanged;
  final VoidCallback onResetEq;

  const ControlPage({
    super.key,
    required this.connected,
    required this.volume,
    required this.onVolumeChanged,
    required this.eqValues,
    required this.onEqChanged,
    required this.onResetEq,
  });

  @override
  Widget build(BuildContext context) {
    final enabled = connected;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        children: [
          const Text("Master Volume", style: TextStyle(fontSize: 18)),
          Slider(
            value: volume,
            onChanged: enabled ? onVolumeChanged : null,
          ),
          Text("${(volume * 100).toInt()}%"),

          const SizedBox(height: 20),
          const Text("Graphic EQ ±12 dB", style: TextStyle(fontSize: 18)),
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: Color(0xFF141722),
              borderRadius: BorderRadius.circular(12),
            ),
            child: EqSliders(
              values: eqValues,
              enabled: enabled,
              onChange: onEqChanged,
              onResetPressed: onResetEq,
            ),
          ),
          if (!connected)
            const Padding(
              padding: EdgeInsets.only(top: 12),
              child: Text("Not connected", style: TextStyle(color: Colors.red)),
            )
        ],
      ),
    );
  }
}
