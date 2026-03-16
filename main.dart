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

  double _volume = 0.5;
  final List<double> _eqBands = List<double>.filled(5, 0.0);

  bool _bluetoothOn = true;
  String _wifiSsid = 'MyWiFi';
  String _room = 'Living Room';
  double _latencyMs = 50;

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
          onEqChanged: (i, v) {
            setState(() => _eqBands[i] = v);
            _sendEqCommand(i, v);
          },
          onResetEq: () {
            setState(() {
              for (int i = 0; i < _eqBands.length; i++) _eqBands[i] = 0.0;
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
    } catch (e) {
      debugPrint("connect() failed: $e");
      setState(() {
        _connecting = false;
        _status = "Connect failed";
      });
      return;
    }

    // FIXED: Real boolean check
    if (device.gatt?.connected != true) {
      debugPrint("GATT not connected.");
      setState(() {
        _connecting = false;
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
        _status = "Service discover fail";
      });
      return;
    }

    BluetoothService? eqService;
    for (final s in services) {
      if (s.uuid.toLowerCase() == kServiceUuid.toLowerCase()) {
        eqService = s;
      }
    }
    if (eqService == null) {
      setState(() {
        _connecting = false;
        _status = "EQ service missing";
      });
      return;
    }

    BluetoothCharacteristic? rx;
    try {
      rx = await eqService.getCharacteristic(kRxCharUuid);
    } catch (e) {
      debugPrint("RX char fail: $e");
      setState(() {
        _connecting = false;
        _status = "RX missing";
      });
      return;
    }

    setState(() {
      _device = device;
      _eqService = eqService;
      _rxChar = rx;
      _connected = true;
      _connecting = false;
      _status = "Connected";
    });

    debugPrint("BLE CONNECTED SUCCESSFULLY");
  }

  Future<void> _disconnectWeb() async {
    try {
      _device?.disconnect();
    } catch (_) {}

    setState(() {
      _connected = false;
      _device = null;
      _rxChar = null;
      _eqService = null;
      _status = "Disconnected";
    });

    debugPrint("Disconnected.");
  }

  bool get _canSend => _connected && _rxChar != null;

  Future<void> _writeString(String text) async {
    debugPrint("BLE -> ESP32: $text");

    if (!_canSend) return;

    try {
      final data = Uint8List.fromList(utf8.encode(text));
      await _rxChar!.writeValueWithoutResponse(data);
      setState(() => _status = "Sent: $text");
    } catch (e) {
      debugPrint("SEND ERROR: $e");
    }
  }

  Future<void> _sendEqCommand(int band, double value) async {
    await _writeString("EQ:$band:${value.toStringAsFixed(1)}");
  }

  Future<void> _sendResetEqCommand() async {
    await _writeString("EQ:RESET");
  }

  Future<void> _sendVolumeCommand(double value) async {
    await _writeString("VOL:${(value * 100).toInt()}");
  }
}
