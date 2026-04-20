import 'dart:math' as math;

import 'package:flutter/material.dart';

class AnimatedRainbowBackground extends StatefulWidget {
  final bool enabled;
  final Widget child;

  const AnimatedRainbowBackground({
    super.key,
    required this.enabled,
    required this.child,
  });

  @override
  State<AnimatedRainbowBackground> createState() =>
      _AnimatedRainbowBackgroundState();
}

class _AnimatedRainbowBackgroundState extends State<AnimatedRainbowBackground>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 18),
  )..repeat();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!widget.enabled) {
      return widget.child;
    }

    return AnimatedBuilder(
      animation: _controller,
      builder: (context, _) {
        final t = _controller.value;
        final waveA = math.sin(t * math.pi * 2);
        final waveB = math.cos(t * math.pi * 2);
        final waveC = math.sin(t * math.pi * 4);

        return Stack(
          fit: StackFit.expand,
          children: [
            DecoratedBox(
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment(-0.8 + (waveA * 0.6), -1.0 + (waveB * 0.4)),
                  end: Alignment(0.8 + (waveB * 0.6), 1.0 + (waveA * 0.4)),
                  colors: const [
                    Color(0xFFFF1744),
                    Color(0xFFFF9100),
                    Color(0xFFFFEA00),
                    Color(0xFF00E676),
                    Color(0xFF00E5FF),
                    Color(0xFF2979FF),
                    Color(0xFFD500F9),
                  ],
                ),
              ),
            ),
            Opacity(
              opacity: 0.55,
              child: DecoratedBox(
                decoration: BoxDecoration(
                  gradient: RadialGradient(
                    center: Alignment(waveB * 0.85, waveC * 0.75),
                    radius: 1.1,
                    colors: const [
                      Color(0xCCFFFFFF),
                      Color(0x44FF80AB),
                      Color(0x00FFFFFF),
                    ],
                    stops: const [0.0, 0.22, 1.0],
                  ),
                ),
              ),
            ),
            Opacity(
              opacity: 0.38,
              child: DecoratedBox(
                decoration: BoxDecoration(
                  gradient: RadialGradient(
                    center: Alignment(-waveC * 0.9, waveA * 0.8),
                    radius: 1.0,
                    colors: const [
                      Color(0xAA00E5FF),
                      Color(0x33D500F9),
                      Color(0x00000000),
                    ],
                    stops: const [0.0, 0.3, 1.0],
                  ),
                ),
              ),
            ),
            DecoratedBox(
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [
                    Colors.white.withOpacity(0.05),
                    Colors.transparent,
                    Colors.black.withOpacity(0.24),
                  ],
                ),
              ),
            ),
            widget.child,
          ],
        );
      },
    );
  }
}
