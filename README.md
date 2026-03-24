# Senior-Design-Sound-System-UCF
File structure:

lutter_application_1/
├── lib/
│   ├── main.dart                  # App entry point, navigation, BLE connection logic, state handling
│   ├── pages/
│   │   ├── home_page.dart         # Home screen with Bluetooth connect/disconnect status
│   │   ├── control_page.dart      # Main control screen: volume, EQ sliders, histogram
│   │   └── settings_page.dart     # Settings screen for Bluetooth, WiFi SSID, room, latency
│   └── widgets/
│       ├── eq_sliders.dart        # Graphic EQ slider widget
│       ├── eq_histogram.dart      # Histogram widget for FFT / spectrum display
│       └── eq_visualizer.dart     # Older EQ visualizer widget


order of commands to run:
flutter clean 
flutter pub get
flutter run -d chrome
