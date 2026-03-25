import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_web_bluetooth/flutter_web_bluetooth.dart';

import 'pages/home_page.dart';
import 'pages/control_page.dart';
import 'pages/settings_page.dart';

const String kServiceUuid = "12345678-1234-1234-1234-1234567890ab";
const String kRxCharUuid = "abcd1234-5678-1234-5678-abcdef123456";
const String kTxCharUuid = "abcd1234-5678-1234-5678-abcdef123457";

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  int _currentIndex = 0;

  bool _connecting = false;
  bool _connected = false;
  String _status = 'Not connected';

  BluetoothDevice? _device;
  BluetoothService? _eqService;
  BluetoothCharacteristic? _rxChar;
  BluetoothCharacteristic? _txChar;

  StreamSubscription<ByteData>? _txSubscription;

  double _volume = 0.5;
  final List<double> _eqBands = List<double>.filled(5, 0.0);
  List<double> _spectrumData = [];

  bool _bluetoothOn = true;
  String _wifiSsid = 'MyWiFi';
  String _room = 'Living Room';
  double _latencyMs = 50;

  bool _isDarkMode = true;

  @override
  void dispose() {
    _txSubscription?.cancel();
    super.dispose();
  }

  ThemeData _buildLightTheme() {
    return ThemeData(
      brightness: Brightness.light,
      colorScheme: ColorScheme.fromSeed(
        seedColor: Colors.deepPurple,
        brightness: Brightness.light,
      ),
      useMaterial3: true,
    );
  }

  ThemeData _buildDarkTheme() {
    return ThemeData(
      brightness: Brightness.dark,
      colorScheme: ColorScheme.fromSeed(
        seedColor: Colors.deepPurple,
        brightness: Brightness.dark,
      ),
      useMaterial3: true,
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = _isDarkMode ? _buildDarkTheme() : _buildLightTheme();

    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: theme,
      home: Builder(
        builder: (context) {
          final isDark = Theme.of(context).brightness == Brightness.dark;

          return Scaffold(
            body: SafeArea(child: _buildBody()),
            bottomNavigationBar: BottomNavigationBar(
              currentIndex: _currentIndex,
              selectedItemColor: theme.colorScheme.primary,
              unselectedItemColor: isDark ? Colors.white54 : Colors.black54,
              onTap: (i) => setState(() => _currentIndex = i),
              items: const [
                BottomNavigationBarItem(icon: Icon(Icons.home), label: "Home"),
                BottomNavigationBarItem(
                  icon: Icon(Icons.equalizer),
                  label: "Control",
                ),
                BottomNavigationBarItem(
                  icon: Icon(Icons.settings),
                  label: "Settings",
                ),
              ],
            ),
          );
        },
      ),
    );
  }

  Widget _buildBody() {
    switch (_currentIndex) {
      case 0:
        return HomePage(
          connected: _connected,
          connecting: _connecting,
          status: _status,
          onToggleConnect: _toggleConnect,
        );

      case 1:
        return ControlPage(
          connected: _connected,
          volume: _volume,
          onVolumeChanged: (v) {
            setState(() => _volume = v);
            _sendVolumeCommand(v);
          },
          eqValues: _eqBands,
          spectrum: _spectrumData,
          onEqChanged: (i, v) {
            setState(() => _eqBands[i] = v);
            _sendEqCommand(i, v);
          },
          onResetEq: () {
            setState(() {
              for (int i = 0; i < _eqBands.length; i++) {
                _eqBands[i] = 0.0;
              }
            });
            _sendResetEqCommand();
          },
          onPlayTestTone: _sendPlayWavCommand,
        );

      case 2:
        return SettingsPage(
          bluetoothOn: _bluetoothOn,
          onBluetooth: (b) => setState(() => _bluetoothOn = b),
          wifiSsid: _wifiSsid,
          onWifiSsid: (s) => setState(() => _wifiSsid = s),
          room: _room,
          onRoom: (r) => setState(() => _room = r),
          latencyMs: _latencyMs,
          onLatency: (l) => setState(() => _latencyMs = l),
          isDarkMode: _isDarkMode,
          onThemeToggle: (value) => setState(() => _isDarkMode = value),
        );

      default:
        return const SizedBox.shrink();
    }
  }

  Future<void> _toggleConnect() async {
    if (_connected) {
      await _disconnectWeb();
    } else {
      await _connectWeb();
    }
  }

  Future<void> _connectWeb() async {
    if (_connecting) return;

    setState(() {
      _connecting = true;
      _status = 'Requesting device...';
    });

    BluetoothDevice device;

    try {
      final opts = RequestOptionsBuilder.acceptAllDevices(
        optionalServices: [kServiceUuid],
      );
      device = await FlutterWebBluetooth.instance.requestDevice(opts);
    } catch (e) {
      debugPrint("User canceled chooser.");
      setState(() {
        _connecting = false;
        _status = "User canceled";
      });
      return;
    }

    debugPrint("Device chosen: ${device.name}");

    try {
      await device.connect();
      await Future.delayed(const Duration(milliseconds: 700));
    } catch (e) {
      debugPrint("connect() failed: $e");
      setState(() {
        _connecting = false;
        _status = "Connect failed";
      });
      return;
    }

    debugPrint("After connect, gatt.connected = ${device.gatt?.connected}");

    if (device.gatt?.connected != true) {
      debugPrint("GATT not connected.");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "Failed GATT connect";
      });
      return;
    }

    List<BluetoothService> services;
    try {
      services = await device.discoverServices();
    } catch (e) {
      debugPrint("discoverServices failed: $e");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "Service discover fail";
      });
      return;
    }

    debugPrint("Discovered ${services.length} services:");
    for (final s in services) {
      debugPrint("  Service UUID: ${s.uuid}");
    }

    BluetoothService? eqService;
    for (final s in services) {
      if (s.uuid.toLowerCase() == kServiceUuid.toLowerCase()) {
        eqService = s;
        break;
      }
    }

    if (eqService == null) {
      debugPrint("EQ service not found.");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "EQ service missing";
      });
      return;
    }

    BluetoothCharacteristic? rx;
    BluetoothCharacteristic? tx;

    try {
      rx = await eqService.getCharacteristic(kRxCharUuid);
      debugPrint("RX characteristic found.");
    } catch (e) {
      debugPrint("RX characteristic lookup failed: $e");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "RX characteristic missing";
      });
      return;
    }

    try {
      tx = await eqService.getCharacteristic(kTxCharUuid);
      debugPrint("TX characteristic found.");
    } catch (e) {
      debugPrint("TX characteristic lookup failed: $e");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "TX characteristic missing";
      });
      return;
    }

    _device = device;
    _eqService = eqService;
    _rxChar = rx;
    _txChar = tx;

    try {
      if (tx.hasProperties && tx.properties.notify) {
        await tx.startNotifications();
        _txSubscription?.cancel();
        _txSubscription = tx.value.listen((event) {
          try {
            final bytes = event.buffer.asUint8List(
              event.offsetInBytes,
              event.lengthInBytes,
            );
            final msg = utf8.decode(bytes, allowMalformed: true);
            debugPrint("ESP -> App: $msg");

            final parsed = _parseSpectrum(msg);
            if (parsed != null) {
              setState(() {
                _spectrumData = parsed;
              });
            }
          } catch (e) {
            debugPrint("Notification parse error: $e");
          }
        });
      }
    } catch (e) {
      debugPrint("Notification setup failed: $e");
    }

    setState(() {
      _connecting = false;
      _connected = true;
      _status = "Connected to ${device.name}";
    });
  }

  Future<void> _disconnectWeb() async {
    try {
      await _txSubscription?.cancel();
      _txSubscription = null;
      await _txChar?.stopNotifications();
    } catch (_) {}

    try {
      _device?.disconnect();
    } catch (_) {}

    setState(() {
      _device = null;
      _eqService = null;
      _rxChar = null;
      _txChar = null;
      _connected = false;
      _connecting = false;
      _status = 'Not connected';
      _spectrumData = [];
    });
  }

  Future<void> _sendText(String text) async {
    final rx = _rxChar;
    if (rx == null || !_connected) return;

    try {
      final bytes = Uint8List.fromList(utf8.encode(text));
      await rx.writeValueWithResponse(bytes);
      debugPrint("App -> ESP: $text");
    } catch (e) {
      debugPrint("Write failed: $e");
    }
  }

  Future<void> _sendVolumeCommand(double volume) async {
    final percent = (volume * 100).round();
    await _sendText("VOL:$percent");
  }

  Future<void> _sendEqCommand(int band, double value) async {
    await _sendText("EQ$band:${value.toStringAsFixed(1)}");
  }

  Future<void> _sendResetEqCommand() async {
    await _sendText("EQ_RESET");
  }

  Future<void> _sendPlayWavCommand() async {
    await _sendText("PLAY_WAV");
  }

  List<double>? _parseSpectrum(String msg) {
    if (!msg.startsWith("FFT:")) return null;

    try {
      final csv = msg.substring(4).trim();
      if (csv.isEmpty) return null;

      return csv
          .split(',')
          .map((e) => double.tryParse(e.trim()))
          .whereType<double>()
          .toList();
    } catch (_) {
      return null;
    }
  }
}
