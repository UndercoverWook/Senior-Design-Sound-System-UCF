import 'package:flutter/material.dart';
import '../widgets/eq_sliders.dart';
import '../widgets/eq_histogram.dart';

class ControlPage extends StatelessWidget {
  final bool connected;
  final bool calibrationActive;
  final double volume;
  final ValueChanged<double> onVolumeChanged;
  final List<double> eqValues;
  final List<double> spectrum;
  final void Function(int, double) onEqChanged;
  final VoidCallback onResetEq;
  final VoidCallback onStartCalibration;

  const ControlPage({
    super.key,
    required this.connected,
    required this.calibrationActive,
    required this.volume,
    required this.onVolumeChanged,
    required this.eqValues,
    required this.spectrum,
    required this.onEqChanged,
    required this.onResetEq,
    required this.onStartCalibration,
  });

  Future<void> _showCalibrationPrompt(BuildContext context) async {
    await showDialog(
      context: context,
      barrierDismissible: false,
      builder: (dialogContext) {
        return Dialog(
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
          child: Padding(
            padding: const EdgeInsets.fromLTRB(20, 16, 20, 20),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Row(
                  children: [
                    const Expanded(
                      child: Text(
                        'Calibration Setup',
                        style: TextStyle(
                          fontSize: 18,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.close),
                      tooltip: 'Close',
                      onPressed: () {
                        Navigator.of(dialogContext).pop();
                      },
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                const Text(
                  'Please set up the microphone level with the speakers and ensure there is no outside noise. When you continue, the system will automatically play the calibration WAV file while it records the response. Are you ready to continue?',
                  style: TextStyle(fontSize: 15),
                ),
                const SizedBox(height: 20),
                SizedBox(
                  width: double.infinity,
                  child: ElevatedButton(
                    onPressed: () {
                      Navigator.of(dialogContext).pop();
                      onStartCalibration();
                    },
                    child: const Text('Yes'),
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final enabled = connected;
    final colorScheme = Theme.of(context).colorScheme;
    final showSpectrum = calibrationActive || spectrum.isNotEmpty;

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
            onPressed: (!enabled || calibrationActive)
                ? null
                : () => _showCalibrationPrompt(context),
            icon: Icon(
              calibrationActive ? Icons.hourglass_top : Icons.graphic_eq,
            ),
            label: Text(
              calibrationActive ? "Calibration Running..." : "Start Calibration",
            ),
          ),

          const SizedBox(height: 8),
          Text(
            calibrationActive
                ? "The calibration WAV file is playing while the ESP32 captures the microphone response."
                : "Start Calibration to automatically play the WAV file and capture the room response.",
            style: TextStyle(color: colorScheme.onSurfaceVariant),
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

          if (showSpectrum)
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
