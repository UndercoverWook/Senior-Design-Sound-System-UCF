import 'package:flutter/material.dart';

class SettingsPage extends StatelessWidget {
  final bool bluetoothOn;
  final ValueChanged<bool> onBluetooth;
  final String wifiSsid;
  final ValueChanged<String> onWifiSsid;
  final String room;
  final ValueChanged<String> onRoom;
  final double latencyMs;
  final ValueChanged<double> onLatency;

  // NEW
  final bool isDarkMode;
  final ValueChanged<bool> onThemeToggle;

  const SettingsPage({
    super.key,
    required this.bluetoothOn,
    required this.onBluetooth,
    required this.wifiSsid,
    required this.onWifiSsid,
    required this.room,
    required this.onRoom,
    required this.latencyMs,
    required this.onLatency,
    required this.isDarkMode,
    required this.onThemeToggle,
  });

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.all(16),
    child: Column(
      children: [
        SwitchListTile(
          secondary: const Icon(Icons.dark_mode),
          title: const Text("Dark Mode"),
          subtitle: Text(isDarkMode ? "Enabled" : "Disabled"),
          value: isDarkMode,
          onChanged: onThemeToggle,
        ),
        const SizedBox(height: 8),
        SwitchListTile(
          title: const Text("Bluetooth Enabled"),
          value: bluetoothOn,
          onChanged: onBluetooth,
        ),
        TextField(
          controller: TextEditingController(text: wifiSsid),
          decoration: const InputDecoration(labelText: "WiFi SSID"),
          onChanged: onWifiSsid,
        ),
        TextField(
          controller: TextEditingController(text: room),
          decoration: const InputDecoration(labelText: "Room"),
          onChanged: onRoom,
        ),
        Slider(value: latencyMs, min: 0, max: 200, onChanged: onLatency),
      ],
    ),
  );
}
