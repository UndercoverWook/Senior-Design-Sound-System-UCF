import 'package:flutter/material.dart';

class HomePage extends StatelessWidget {
  final bool connected;
  final bool connecting;
  final String status;
  final VoidCallback onToggleConnect;

  const HomePage({
    super.key,
    required this.connected,
    required this.connecting,
    required this.status,
    required this.onToggleConnect,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        children: [
          const Text("Smart Auto-EQ", style: TextStyle(fontSize: 24)),
          const SizedBox(height: 20),
          Icon(
            connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
            size: 60,
          ),
          const SizedBox(height: 10),
          Text(status),
          const SizedBox(height: 20),
          ElevatedButton(
            onPressed: connecting ? null : onToggleConnect,
            child: Text(connected ? "Disconnect" : "Connect"),
          ),
        ],
      ),
    );
  }
}
