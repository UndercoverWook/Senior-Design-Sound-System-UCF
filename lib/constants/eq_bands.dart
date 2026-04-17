class EqBands {
  static const List<double> centersHz = [
    60,
    150,
    400,
    500,
    1000,
    2000,
    8000,
    16000,
  ];

  static const List<String> labels = [
    '60 Hz',
    '150 Hz',
    '400 Hz',
    '500 Hz',
    '1 kHz',
    '2 kHz',
    '8 kHz',
    '16 kHz',
  ];

  static const List<String> shortLabels = [
    '60',
    '150',
    '400',
    '500',
    '1k',
    '2k',
    '8k',
    '16k',
  ];

  static const List<String> descriptors = [
    'Rumble',
    'Warmth',
    'Low mids',
    'Mids',
    'Clarity',
    'Presence',
    'Brilliance',
    'Air',
  ];
}
