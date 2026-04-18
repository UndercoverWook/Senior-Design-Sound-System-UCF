import 'package:flutter/material.dart';

import '../constants/app_theme_mode.dart';

class SettingsPage extends StatelessWidget {
  final bool connected;
  final bool connecting;
  final String status;
  final VoidCallback onToggleConnect;

  final bool bluetoothOn;
  final ValueChanged<bool> onBluetooth;

  final AppThemeMode themeMode;
  final ValueChanged<AppThemeMode> onThemeModeChanged;

  const SettingsPage({
    super.key,
    required this.connected,
    required this.connecting,
    required this.status,
    required this.onToggleConnect,
    required this.bluetoothOn,
    required this.onBluetooth,
    required this.themeMode,
    required this.onThemeModeChanged,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(20),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const SizedBox(height: 12),
          const Text(
            "Smart Auto-EQ",
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 28, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 20),
          Icon(
            connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
            size: 72,
            color: connected
                ? colorScheme.primary
                : colorScheme.onSurfaceVariant,
          ),
          const SizedBox(height: 12),
          Text(
            status,
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 16, color: colorScheme.onSurfaceVariant),
          ),
          const SizedBox(height: 24),
          Center(
            child: SizedBox(
              width: 280,
              height: 60,
              child: ElevatedButton.icon(
                onPressed: connecting || (!bluetoothOn && !connected)
                    ? null
                    : onToggleConnect,
                icon: Icon(
                  connected ? Icons.link_off : Icons.bluetooth_searching,
                  size: 26,
                ),
                label: Text(
                  connecting
                      ? "Connecting..."
                      : (connected
                            ? "Disconnect Bluetooth"
                            : "Connect Bluetooth"),
                  style: const TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(height: 28),
          Card(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 18, 16, 12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Row(
                    children: [
                      Icon(Icons.palette_outlined, color: colorScheme.primary),
                      const SizedBox(width: 10),
                      const Expanded(
                        child: Text(
                          'Color Mode',
                          style: TextStyle(
                            fontSize: 17,
                            fontWeight: FontWeight.w700,
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 6),
                  Text(
                    themeMode.description,
                    style: TextStyle(
                      fontSize: 13,
                      color: colorScheme.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(height: 14),
                  SegmentedButton<AppThemeMode>(
                    showSelectedIcon: false,
                    multiSelectionEnabled: false,
                    segments: const [
                      ButtonSegment<AppThemeMode>(
                        value: AppThemeMode.light,
                        icon: Icon(Icons.light_mode),
                        label: Text('Light'),
                      ),
                      ButtonSegment<AppThemeMode>(
                        value: AppThemeMode.dark,
                        icon: Icon(Icons.dark_mode),
                        label: Text('Dark'),
                      ),
                      ButtonSegment<AppThemeMode>(
                        value: AppThemeMode.rainbow,
                        icon: Icon(Icons.auto_awesome),
                        label: Text('Rainbow'),
                      ),
                    ],
                    selected: {themeMode},
                    onSelectionChanged: (selection) {
                      if (selection.isNotEmpty) {
                        onThemeModeChanged(selection.first);
                      }
                    },
                  ),
                  const SizedBox(height: 10),
                ],
              ),
            ),
          ),
          const SizedBox(height: 14),
          Card(
            child: Column(
              children: [
                SwitchListTile(
                  secondary: const Icon(Icons.bluetooth),
                  title: const Text("Bluetooth Enabled"),
                  subtitle: Text(bluetoothOn ? "On" : "Off"),
                  value: bluetoothOn,
                  onChanged: onBluetooth,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
