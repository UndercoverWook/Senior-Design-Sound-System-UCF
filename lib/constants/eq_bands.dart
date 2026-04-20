class EqBands {
  static const List<double> centersHz = [
    60,
    125,
    500,
    1000,
    2000,
    4000,
    8000,
    16000,
  ];

  static const List<String> labels = [
    '60 Hz',
    '125 Hz',
    '500 Hz',
    '1 kHz',
    '2 kHz',
    '4 kHz',
    '8 kHz',
    '16 kHz',
  ];

  static const List<String> shortLabels = [
    '60',
    '125',
    '500',
    '1k',
    '2k',
    '4k',
    '8k',
    '16k',
  ];

  static const List<String> commandIds = [
    '60',
    '125',
    '500',
    '1000',
    '2000',
    '4000',
    '8000',
    '16000',
  ];

  static const List<String> descriptors = [
    'Bass',
    'Punch',
    'Low mids',
    'Mids',
    'Presence',
    'Definition',
    'Brilliance',
    'Air',
  ];
}
