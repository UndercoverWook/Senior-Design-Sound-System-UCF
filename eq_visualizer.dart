import 'package:flutter/material.dart';

class EqVisualizer extends StatelessWidget {
  const EqVisualizer({super.key});

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 120,
      decoration: BoxDecoration(
        color: Color(0xFF141722),
        borderRadius: BorderRadius.circular(12),
      ),
      child: const Center(
        child: Text("Waiting for data..."),
      ),
    );
  }
}
