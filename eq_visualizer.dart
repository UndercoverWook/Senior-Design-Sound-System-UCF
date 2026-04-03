import 'package:flutter/material.dart';

class EqVisualizer extends StatelessWidget {
  const EqVisualizer({super.key});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;

    return Container(
      height: 120,
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(12),
      ),
      child: const Center(child: Text("Waiting for data...")),
    );
  }
}
