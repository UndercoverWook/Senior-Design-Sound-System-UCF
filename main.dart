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

  StreamSubscription? _txSubscription;

  double _volume = 0.5;
  final List<double> _eqBands = List<double>.filled(5, 0.0);
  List<double> _spectrumData = [];

  bool _bluetoothOn = true;
  String _wifiSsid = 'MyWiFi';
  String _room = 'Living Room';
  double _latencyMs = 50;

  @override
  void dispose() {
    _txSubscription?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(),
      home: Scaffold(
        body: SafeArea(child: _buildBody()),
        bottomNavigationBar: BottomNavigationBar(
          currentIndex: _currentIndex,
          selectedItemColor: Colors.deepPurpleAccent,
          unselectedItemColor: Colors.white54,
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

    try {
      await tx.startNotifications();
      debugPrint("TX notifications started.");
    } catch (e) {
      debugPrint("Failed to start notifications: $e");
      setState(() {
        _connecting = false;
        _connected = false;
        _status = "Notify start failed";
      });
      return;
    }

    _txSubscription?.cancel();
    _txSubscription = tx.value.listen((ByteData data) {
      final bytes = data.buffer.asUint8List(
        data.offsetInBytes,
        data.lengthInBytes,
      );
      final text = utf8.decode(bytes, allowMalformed: true);
      debugPrint("BLE RX notify: $text");
      _handleIncomingText(text);
    });

    setState(() {
      _device = device;
      _eqService = eqService;
      _rxChar = rx;
      _txChar = tx;
      _connected = true;
      _connecting = false;
      _status = "Connected";
    });

    debugPrint("BLE CONNECTED SUCCESSFULLY");
  }

  Future<void> _disconnectWeb() async {
    try {
      await _txSubscription?.cancel();
      _txSubscription = null;
    } catch (_) {}

    try {
      _device?.disconnect();
    } catch (e) {
      debugPrint("disconnect error: $e");
    }

    setState(() {
      _device = null;
      _eqService = null;
      _rxChar = null;
      _txChar = null;
      _connected = false;
      _connecting = false;
      _status = "Not connected";
      _spectrumData = [];
    });
  }

  void _handleIncomingText(String text) {
    if (!text.startsWith("FFT:")) return;

    final raw = text.substring(4).trim();
    if (raw.isEmpty) return;

    final parts = raw.split(',');
    final values = <double>[];

    for (final p in parts) {
      final v = double.tryParse(p.trim());
      if (v != null) {
        values.add(v);
      }
    }

    if (values.isEmpty) return;

    setState(() {
      _spectrumData = values;
    });
  }

  Future<void> _sendText(String text) async {
    if (_rxChar == null || !_connected) {
      debugPrint("Cannot send, RX characteristic is null or not connected.");
      return;
    }

    try {
      await _rxChar!.writeValueWithoutResponse(
        Uint8List.fromList(utf8.encode(text)),
      );
      debugPrint("BLE TX: $text");
    } catch (e) {
      debugPrint("write failed: $e");
    }
  }

  void _sendVolumeCommand(double v) {
    final value = (v * 100).round();
    _sendText("VOL:$value");
  }

  void _sendEqCommand(int index, double value) {
    _sendText("EQ$index:${value.toStringAsFixed(1)}");
  }

  void _sendResetEqCommand() {
    _sendText("EQ_RESET");
  }
}
