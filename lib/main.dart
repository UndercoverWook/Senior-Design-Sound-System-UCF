import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_web_bluetooth/flutter_web_bluetooth.dart';

import 'constants/app_theme_mode.dart';
import 'constants/eq_bands.dart';
import 'pages/control_page.dart';
import 'pages/settings_page.dart';
import 'widgets/animated_rainbow_background.dart';

const String kDeviceName = "ESP32_AutoEQ";
const String kServiceUuid = "12345678-1234-1234-1234-1234567890ab";
const String kRxCharUuid = "abcd1234-5678-1234-5678-abcdef123456";
const String kTxCharUuid = "abcd1234-5678-1234-5678-abcdef123457";
final int kEqBandCount = EqBands.centersHz.length;

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  int _currentIndex = 1;

  bool _connecting = false;
  bool _connected = false;
  bool _notifyReady = false;
  bool _calibrationActive = false;
  String _status = 'Not connected';

  BluetoothDevice? _device;
  BluetoothService? _eqService;
  BluetoothCharacteristic? _rxChar;
  BluetoothCharacteristic? _txChar;

  StreamSubscription<ByteData>? _txSubscription;

  Future<void> _writeChain = Future.value();
  Timer? _volumeDebounce;
  final List<Timer?> _eqDebounce = List<Timer?>.filled(kEqBandCount, null);

  double _volume = 0.5;
  final List<double> _eqBands = List<double>.filled(kEqBandCount, 0.0);
  List<double> _spectrumData = [];
  List<double> _pendingSpectrumData = [];

  bool _bluetoothOn = true;
  AppThemeMode _themeMode = AppThemeMode.dark;

  bool get _isRainbowMode => _themeMode == AppThemeMode.rainbow;

  @override
  void dispose() {
    _volumeDebounce?.cancel();
    for (final t in _eqDebounce) {
      t?.cancel();
    }
    _txSubscription?.cancel();
    super.dispose();
  }

  ThemeData _buildLightTheme() {
    final scheme = ColorScheme.fromSeed(
      seedColor: Colors.deepPurple,
      brightness: Brightness.light,
    );

    return ThemeData(
      brightness: Brightness.light,
      colorScheme: scheme,
      useMaterial3: true,
      cardTheme: CardThemeData(
        elevation: 2,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
      ),
      bottomNavigationBarTheme: BottomNavigationBarThemeData(
        backgroundColor: scheme.surface,
        selectedItemColor: scheme.primary,
        unselectedItemColor: Colors.black54,
        type: BottomNavigationBarType.fixed,
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
        ),
      ),
    );
  }

  ThemeData _buildDarkTheme() {
    final scheme = ColorScheme.fromSeed(
      seedColor: Colors.deepPurple,
      brightness: Brightness.dark,
    );

    return ThemeData(
      brightness: Brightness.dark,
      colorScheme: scheme,
      useMaterial3: true,
      cardTheme: CardThemeData(
        elevation: 2,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
      ),
      bottomNavigationBarTheme: BottomNavigationBarThemeData(
        backgroundColor: scheme.surface,
        selectedItemColor: scheme.primary,
        unselectedItemColor: Colors.white54,
        type: BottomNavigationBarType.fixed,
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
        ),
      ),
    );
  }

  ThemeData _buildRainbowTheme() {
    final base = ThemeData(
      brightness: Brightness.dark,
      colorScheme: ColorScheme.fromSeed(
        seedColor: const Color(0xFFD500F9),
        brightness: Brightness.dark,
      ).copyWith(
        primary: const Color(0xFFFFEA00),
        onPrimary: Colors.black,
        secondary: const Color(0xFF00E5FF),
        onSecondary: Colors.black,
        tertiary: const Color(0xFFFF80AB),
        onTertiary: Colors.black,
        surface: const Color(0x55120F2B),
        onSurface: Colors.white,
        onSurfaceVariant: const Color(0xFFE8E6FF),
        outline: Colors.white.withOpacity(0.22),
        surfaceContainerHighest: const Color(0x6622154A),
      ),
      useMaterial3: true,
    );

    return base.copyWith(
      scaffoldBackgroundColor: Colors.transparent,
      canvasColor: Colors.transparent,
      shadowColor: Colors.black.withOpacity(0.25),
      dividerColor: Colors.white.withOpacity(0.15),
      cardTheme: CardThemeData(
        color: Colors.white.withOpacity(0.12),
        elevation: 6,
        shadowColor: Colors.black.withOpacity(0.25),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
      ),
      bottomNavigationBarTheme: BottomNavigationBarThemeData(
        backgroundColor: Colors.black.withOpacity(0.22),
        elevation: 0,
        selectedItemColor: const Color(0xFFFFEA00),
        unselectedItemColor: Colors.white70,
        type: BottomNavigationBarType.fixed,
      ),
      iconTheme: const IconThemeData(color: Colors.white),
      textTheme: base.textTheme.apply(
        bodyColor: Colors.white,
        displayColor: Colors.white,
      ),
      dialogTheme: DialogThemeData(
        backgroundColor: const Color(0xE61A1032),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          backgroundColor: Colors.white.withOpacity(0.18),
          foregroundColor: Colors.white,
          disabledBackgroundColor: Colors.white.withOpacity(0.08),
          disabledForegroundColor: Colors.white38,
          padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
        ),
      ),
      segmentedButtonTheme: SegmentedButtonThemeData(
        style: ButtonStyle(
          foregroundColor: WidgetStateProperty.resolveWith((states) {
            if (states.contains(WidgetState.selected)) {
              return Colors.black;
            }
            return Colors.white;
          }),
          backgroundColor: WidgetStateProperty.resolveWith((states) {
            if (states.contains(WidgetState.selected)) {
              return const Color(0xFFFFEA00);
            }
            return Colors.white.withOpacity(0.10);
          }),
          side: WidgetStatePropertyAll(
            BorderSide(color: Colors.white.withOpacity(0.25)),
          ),
          shape: WidgetStatePropertyAll(
            RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
          ),
        ),
      ),
      sliderTheme: base.sliderTheme.copyWith(
        activeTrackColor: const Color(0xFFFFEA00),
        inactiveTrackColor: Colors.white.withOpacity(0.20),
        thumbColor: Colors.white,
        overlayColor: Colors.white.withOpacity(0.12),
      ),
      switchTheme: SwitchThemeData(
        thumbColor: WidgetStateProperty.resolveWith((states) {
          if (states.contains(WidgetState.selected)) {
            return const Color(0xFFFFEA00);
          }
          return Colors.white;
        }),
        trackColor: WidgetStateProperty.resolveWith((states) {
          if (states.contains(WidgetState.selected)) {
            return const Color(0x8800E5FF);
          }
          return Colors.white.withOpacity(0.20);
        }),
      ),
    );
  }

  ThemeData _currentTheme() {
    switch (_themeMode) {
      case AppThemeMode.light:
        return _buildLightTheme();
      case AppThemeMode.dark:
        return _buildDarkTheme();
      case AppThemeMode.rainbow:
        return _buildRainbowTheme();
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = _currentTheme();
    final isDark = theme.brightness == Brightness.dark;

    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: theme,
      home: AnimatedRainbowBackground(
        enabled: _isRainbowMode,
        child: Builder(
          builder: (context) {
            return Scaffold(
              backgroundColor: _isRainbowMode ? Colors.transparent : null,
              body: SafeArea(child: _buildBody()),
              bottomNavigationBar: BottomNavigationBar(
                currentIndex: _currentIndex,
                backgroundColor: _isRainbowMode
                    ? Colors.black.withOpacity(0.22)
                    : theme.bottomNavigationBarTheme.backgroundColor,
                selectedItemColor: theme.colorScheme.primary,
                unselectedItemColor: _isRainbowMode
                    ? Colors.white70
                    : (isDark ? Colors.white54 : Colors.black54),
                onTap: (i) => setState(() => _currentIndex = i),
                items: const [
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
      ),
    );
  }

  Widget _buildBody() {
    switch (_currentIndex) {
      case 0:
        return ControlPage(
          connected: _connected && _notifyReady,
          calibrationActive: _calibrationActive,
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
          onStartCalibration: _startCalibration,
        );

      case 1:
        return SettingsPage(
          connected: _connected,
          connecting: _connecting,
          status: _status,
          onToggleConnect: _toggleConnect,
          bluetoothOn: _bluetoothOn,
          onBluetooth: (b) => setState(() => _bluetoothOn = b),
          themeMode: _themeMode,
          onThemeModeChanged: (mode) => setState(() => _themeMode = mode),
        );

      default:
        return const SizedBox.shrink();
    }
  }

  Future<void> _toggleConnect() async {
    if (!_bluetoothOn && !_connected) return;

    if (_connected) {
      await _disconnectWeb();
    } else {
      await _connectWeb();
    }
  }

  bool _isExpectedDevice(BluetoothDevice device) {
    final name = (device.name ?? '').trim();
    return name == kDeviceName;
  }

  Future<bool> _waitForGattConnection(
    BluetoothDevice device, {
    Duration timeout = const Duration(seconds: 3),
  }) async {
    final sw = Stopwatch()..start();

    while (sw.elapsed < timeout) {
      if (device.gatt?.connected == true) {
        return true;
      }
      await Future.delayed(const Duration(milliseconds: 200));
    }

    return false;
  }

  Future<List<BluetoothService>?> _connectAndDiscoverWithRetries(
    BluetoothDevice device,
  ) async {
    for (int attempt = 1; attempt <= 4; attempt++) {
      debugPrint("Connect attempt $attempt...");

      try {
        await device.connect();
      } catch (e) {
        debugPrint("connect() attempt $attempt failed: $e");
      }

      final connected = await _waitForGattConnection(device);
      debugPrint(
        "After attempt $attempt, gatt.connected = ${device.gatt?.connected}",
      );

      if (!connected) {
        try {
          device.disconnect();
        } catch (_) {}
        await Future.delayed(const Duration(milliseconds: 500));
        continue;
      }

      await Future.delayed(const Duration(milliseconds: 700));

      if (device.gatt?.connected != true) {
        debugPrint("Device dropped before discovery on attempt $attempt.");
        try {
          device.disconnect();
        } catch (_) {}
        await Future.delayed(const Duration(milliseconds: 500));
        continue;
      }

      try {
        final services = await device.discoverServices();
        debugPrint("discoverServices attempt $attempt succeeded.");
        return services;
      } catch (e) {
        debugPrint("discoverServices attempt $attempt failed: $e");

        try {
          device.disconnect();
        } catch (_) {}

        await Future.delayed(const Duration(milliseconds: 700));
      }
    }

    return null;
  }

  Future<void> _connectWeb() async {
    if (_connecting) return;

    await _disconnectWeb(silent: true);

    setState(() {
      _connecting = true;
      _connected = false;
      _notifyReady = false;
      _calibrationActive = false;
      _status = 'Requesting device...';
      _spectrumData = [];
      _pendingSpectrumData = [];
    });

    BluetoothDevice device;

    try {
      final opts = RequestOptionsBuilder.acceptAllDevices(
        optionalServices: [kServiceUuid],
      );
      device = await FlutterWebBluetooth.instance.requestDevice(opts);
    } catch (_) {
      debugPrint("User canceled chooser.");
      setState(() {
        _connecting = false;
        _status = "User canceled";
      });
      return;
    }

    debugPrint("Device chosen: ${device.name}");

    if (!_isExpectedDevice(device)) {
      debugPrint("Wrong device selected. Expected $kDeviceName");
      setState(() {
        _connecting = false;
        _connected = false;
        _notifyReady = false;
        _calibrationActive = false;
        _status = "Pick $kDeviceName only";
      });
      return;
    }

    final services = await _connectAndDiscoverWithRetries(device);

    if (services == null) {
      debugPrint("Could not complete connect + service discovery.");
      setState(() {
        _connecting = false;
        _connected = false;
        _notifyReady = false;
        _calibrationActive = false;
        _status = "Connect/discovery failed";
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
        _notifyReady = false;
        _calibrationActive = false;
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
        _notifyReady = false;
        _calibrationActive = false;
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
        _notifyReady = false;
        _calibrationActive = false;
        _status = "TX characteristic missing";
      });
      return;
    }

    debugPrint(
      "TX props: has=${tx.hasProperties}"
      " notify=${tx.hasProperties ? tx.properties.notify : false}"
      " indicate=${tx.hasProperties ? tx.properties.indicate : false}"
      " read=${tx.hasProperties ? tx.properties.read : false}",
    );

    _device = device;
    _eqService = eqService;
    _rxChar = rx;
    _txChar = tx;

    try {
      await _txSubscription?.cancel();
      _txSubscription = tx.value.listen((event) {
        try {
          final bytes = event.buffer.asUint8List(
            event.offsetInBytes,
            event.lengthInBytes,
          );
          final msg = utf8.decode(bytes, allowMalformed: true);
          debugPrint("ESP -> App: $msg");
          _handleEspMessage(msg);
        } catch (e) {
          debugPrint("Notification parse error: $e");
        }
      });

      await Future.delayed(const Duration(milliseconds: 150));
      await tx.startNotifications();
      await Future.delayed(const Duration(milliseconds: 250));
    } catch (e) {
      debugPrint("Notification setup failed: $e");
      await _disconnectWeb(silent: true);
      setState(() {
        _connecting = false;
        _connected = false;
        _notifyReady = false;
        _calibrationActive = false;
        _status = "Notify setup failed";
      });
      return;
    }

    setState(() {
      _connecting = false;
      _connected = true;
      _notifyReady = true;
      _calibrationActive = false;
      _status = "Connected to ${device.name}";
    });
  }

  Future<void> _disconnectWeb({bool silent = false}) async {
    _volumeDebounce?.cancel();
    for (int i = 0; i < _eqDebounce.length; i++) {
      _eqDebounce[i]?.cancel();
      _eqDebounce[i] = null;
    }

    try {
      await _txSubscription?.cancel();
      _txSubscription = null;
      if (_txChar != null && _txChar!.isNotifying) {
        await _txChar?.stopNotifications();
      }
    } catch (_) {}

    try {
      _device?.disconnect();
    } catch (_) {}

    _device = null;
    _eqService = null;
    _rxChar = null;
    _txChar = null;
    _connected = false;
    _connecting = false;
    _notifyReady = false;
    _calibrationActive = false;
    _spectrumData = [];
    _pendingSpectrumData = [];
    _writeChain = Future.value();

    if (!silent && mounted) {
      setState(() {
        _status = 'Not connected';
      });
    } else if (mounted) {
      setState(() {});
    }
  }

  Future<void> _enqueueWrite(Future<void> Function() op) {
    _writeChain = _writeChain.then((_) => op()).catchError((_) {});
    return _writeChain;
  }

  Future<void> _sendText(String text) async {
    final rx = _rxChar;
    if (rx == null || !_connected || !_notifyReady) return;

    await _enqueueWrite(() async {
      try {
        final bytes = Uint8List.fromList(utf8.encode(text));

        if (rx.hasProperties && rx.properties.writeWithoutResponse) {
          await rx.writeValueWithoutResponse(bytes);
        } else {
          await rx.writeValueWithResponse(bytes);
        }

        debugPrint("App -> ESP: $text");
      } catch (e) {
        debugPrint("Write failed: $e");
      }
    });
  }

  Future<void> _sendVolumeCommand(double volume) async {
    _volumeDebounce?.cancel();
    _volumeDebounce = Timer(const Duration(milliseconds: 90), () async {
      final percent = (volume * 100).round();
      await _sendText("VOL:$percent");
    });
  }

  Future<void> _sendEqCommand(int band, double value) async {
    if (band < 0 || band >= _eqDebounce.length) return;

    final commandIds = EqBands.commandIds;
    if (band >= commandIds.length) return;

    _eqDebounce[band]?.cancel();
    _eqDebounce[band] = Timer(const Duration(milliseconds: 90), () async {
      final commandId = commandIds[band];
      await _sendText("EQ$commandId:${value.toStringAsFixed(1)}");
    });
  }

  Future<void> _sendResetEqCommand() async {
    await _sendText("EQ_RESET");
  }

  Future<void> _startCalibration() async {
    if (!_connected || !_notifyReady || _calibrationActive) return;

    setState(() {
      _calibrationActive = true;
      _spectrumData = [];
      _pendingSpectrumData = [];
      _status = "Calibration running...";
    });

    await _sendText("AUTO_EQ_START");
  }

  void _handleEspMessage(String msg) {
    final parsed = _parseSpectrum(msg);
    if (parsed != null) {
      if (!mounted) return;
      setState(() {
        if (_calibrationActive) {
          _pendingSpectrumData = parsed;
        } else {
          _spectrumData = parsed;
        }
      });
      return;
    }

    if (!mounted) return;

    switch (msg.trim()) {
      case 'ACK:AUTO_EQ_START':
        setState(() {
          _calibrationActive = true;
          _status = 'Calibration running...';
        });
        return;

      case 'CAL_DONE':
        setState(() {
          _calibrationActive = false;
          _status = 'Calibration complete';
          if (_pendingSpectrumData.isNotEmpty) {
            _spectrumData = List<double>.from(_pendingSpectrumData);
          }
        });
        return;

      case 'CAL_FAILED':
        setState(() {
          _calibrationActive = false;
          _status = 'Calibration failed';
        });
        return;

      case 'BUSY:CALIBRATION':
        setState(() {
          _calibrationActive = false;
          _status = 'Calibration already running';
        });
        return;

      case 'ERR:PLAY_WAV':
      case 'FFT_ERROR':
        setState(() {
          _calibrationActive = false;
          _status = 'Calibration failed';
        });
        return;
    }
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
