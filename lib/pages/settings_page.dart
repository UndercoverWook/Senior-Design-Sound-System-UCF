import 'package:flutter/material.dart';

class SettingsPage extends StatelessWidget {
  final bool connected;
  final bool connecting;
  final String status;
  final VoidCallback onToggleConnect;

  final bool bluetoothOn;
  final ValueChanged<bool> onBluetooth;

  final bool isDarkMode;
  final ValueChanged<bool> onThemeToggle;

  const SettingsPage({
    super.key,
    required this.connected,
    required this.connecting,
    required this.status,
    required this.onToggleConnect,
    required this.bluetoothOn,
    required this.onBluetooth,
    required this.isDarkMode,
    required this.onThemeToggle,
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
            child: Column(
              children: [
                SwitchListTile(
                  secondary: const Icon(Icons.dark_mode),
                  title: const Text("Dark Mode"),
                  subtitle: Text(isDarkMode ? "Enabled" : "Disabled"),
                  value: isDarkMode,
                  onChanged: onThemeToggle,
                ),
                const Divider(height: 1),
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
