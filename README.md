# Senior-Design-Sound-System-UCF
## File Structure

### Main Application
- `lib/main.dart` — app entry point, navigation, BLE connection logic, and shared state

### Pages
- `lib/pages/home_page.dart` — home screen with Bluetooth connect/disconnect status
- `lib/pages/control_page.dart` — main control screen with volume, EQ sliders, and histogram
- `lib/pages/settings_page.dart` — settings screen for Bluetooth, WiFi SSID, room, and latency

### Widgets
- `lib/widgets/eq_sliders.dart` — graphic EQ slider widget
- `lib/widgets/eq_histogram.dart` — histogram widget for FFT / spectrum display
- `lib/widgets/eq_visualizer.dart` — older EQ visualizer widget


## Order of Commands to Run
```bash
flutter clean
flutter pub get
flutter run -d chrome
```
