import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../config/app_config.dart';
import '../config/ble_protocol.dart';
import '../models/ble_connection_state.dart';
import '../models/sensor_frame.dart';
import '../utils/packet_parser.dart';

/// Manages the BLE link to the receiver: permissions, scanning by MAC,
/// connection lifecycle, subscribing to the sensor characteristic, and
/// emitting parsed [SensorFrame]s downstream.
///
/// Auto-reconnects with exponential backoff on link drops.
class BleService {
  final _frameController = StreamController<SensorFrame>.broadcast();
  final _stateController =
      StreamController<BleConnectionState>.broadcast();

  Stream<SensorFrame> get frames => _frameController.stream;
  Stream<BleConnectionState> get connectionState => _stateController.stream;

  BleConnectionState _currentState = BleConnectionState.idle;
  BleConnectionState get currentState => _currentState;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _controlChar;
  StreamSubscription<List<int>>? _charSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  int _reconnectAttempt = 0;
  String? _targetMac;
  bool _disposed = false;

  void _setState(BleConnectionState s) {
    if (_disposed || _currentState == s) return;
    _currentState = s;
    _stateController.add(s);
  }

  Future<bool> _ensurePermissions() async {
    // Android 12+ : BLUETOOTH_SCAN + BLUETOOTH_CONNECT are the runtime perms.
    // The manifest declares BLUETOOTH_SCAN with neverForLocation, so location
    // is NOT required (and isn't even declared on 12+, capped at maxSdk 30).
    // Requiring locationWhenInUse here is what broke connect on 12+ — its
    // request returns "no permissions in manifest" and fails the gate.
    final scan = await Permission.bluetoothScan.request();
    final connect = await Permission.bluetoothConnect.request();
    if (!scan.isGranted || !connect.isGranted) return false;

    // Legacy Android < 12: bluetoothScan/Connect auto-grant there and BLE
    // scanning needs location instead. Request it best-effort — never gate the
    // modern path on it.
    if (await Permission.locationWhenInUse.isDenied) {
      await Permission.locationWhenInUse.request();
    }
    return true;
  }

  /// Connect (or reconnect) to the device with [mac]. Cancels any in-flight
  /// reconnect schedule and starts fresh.
  Future<void> connect(String mac) async {
    if (_disposed) return;
    _targetMac = mac.trim().toUpperCase();
    _reconnectAttempt = 0;
    await _runConnectAttempt();
  }

  Future<void> _runConnectAttempt() async {
    if (_disposed || _targetMac == null) return;
    final mac = _targetMac!;

    _setState(BleConnectionState.scanning);
    if (!await _ensurePermissions()) {
      _setState(BleConnectionState.permissionDenied);
      return;
    }
    if (!(await FlutterBluePlus.isSupported)) {
      _setState(BleConnectionState.error);
      return;
    }
    if (await FlutterBluePlus.adapterState.first != BluetoothAdapterState.on) {
      _setState(BleConnectionState.bluetoothOff);
      return;
    }

    final found = await _scanForMac(mac);
    if (found == null) {
      _setState(BleConnectionState.receiverNotFound);
      _scheduleReconnect();
      return;
    }

    _device = found;
    _setState(BleConnectionState.connecting);
    try {
      await found.connect(autoConnect: false);
    } catch (e) {
      debugPrint('BleService: connect failed: $e');
      _setState(BleConnectionState.error);
      _scheduleReconnect();
      return;
    }

    // Negotiate a larger ATT MTU. The sensor payload is 180 bytes; the BLE
    // default MTU of 23 only carries 20 bytes per notification, so without
    // this the phone would receive truncated packets that parseBlePacket
    // rejects (length != 180) — i.e. "no data captured". Android honours this;
    // on iOS it's a no-op (the OS auto-negotiates ~185). 247 → 244 usable.
    try {
      final mtu = await found.requestMtu(247);
      debugPrint('BleService: negotiated MTU = $mtu');
    } catch (e) {
      debugPrint('BleService: requestMtu failed (continuing): $e');
    }

    final services = await found.discoverServices();
    BluetoothCharacteristic? sensorChar;
    _controlChar = null;
    for (final s in services) {
      if (s.uuid.str.toLowerCase() == BleProtocol.serviceUuid.toLowerCase()) {
        for (final c in s.characteristics) {
          final cu = c.uuid.str.toLowerCase();
          if (cu == BleProtocol.sensorCharUuid.toLowerCase()) {
            sensorChar = c;
          } else if (cu == BleProtocol.controlCharUuid.toLowerCase()) {
            _controlChar = c;
          }
        }
      }
    }
    if (sensorChar == null) {
      debugPrint('BleService: required characteristic not found on device.');
      _setState(BleConnectionState.error);
      _scheduleReconnect();
      return;
    }

    await sensorChar.setNotifyValue(true);
    _charSub = sensorChar.onValueReceived.listen(_onPacket);

    _connSub = found.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected) {
        _setState(BleConnectionState.reconnecting);
        _scheduleReconnect();
      }
    });

    _setState(BleConnectionState.connected);
    _reconnectAttempt = 0;

    // Heat-saving: idle the glove immediately on connect. It only wakes for a
    // capture (Listen press) via setStreaming(true). No-op if the firmware
    // doesn't expose the control characteristic (older build).
    await setStreaming(false);
  }

  /// Wake (true) or idle (false) the glove. Writes the control characteristic;
  /// the master gates its own MPU/notify and broadcasts WAKE/SLEEP to the
  /// slaves. Safe to call when disconnected or on firmware without the char.
  Future<void> setStreaming(bool on) async {
    final c = _controlChar;
    if (c == null) return;
    try {
      await c.write([on ? 0x01 : 0x00], withoutResponse: true);
    } catch (e) {
      debugPrint('BleService: setStreaming($on) failed: $e');
    }
  }

  /// Scan for the given MAC (or fall back to advertised name prefix) with a
  /// hard timeout. Returns null on timeout/not-found.
  Future<BluetoothDevice?> _scanForMac(String mac) async {
    final completer = Completer<BluetoothDevice?>();
    late StreamSubscription<List<ScanResult>> sub;
    Timer? deadline;

    void finish(BluetoothDevice? d) {
      if (completer.isCompleted) return;
      completer.complete(d);
      deadline?.cancel();
      sub.cancel();
      FlutterBluePlus.stopScan();
    }

    sub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        if (r.device.remoteId.str.toUpperCase() == mac ||
            r.advertisementData.advName
                .startsWith(BleProtocol.advertisedNamePrefix)) {
          finish(r.device);
          return;
        }
      }
    }, onError: (Object e) {
      debugPrint('BleService: scan error: $e');
      finish(null);
    });

    deadline = Timer(AppConfig.scanTimeout, () {
      debugPrint('BleService: scan timeout after ${AppConfig.scanTimeout.inSeconds}s');
      finish(null);
    });

    try {
      await FlutterBluePlus.startScan(timeout: AppConfig.scanTimeout);
    } catch (e) {
      debugPrint('BleService: startScan threw: $e');
      finish(null);
    }

    return completer.future;
  }

  int _pktLogCount = 0;
  int _dropLogCount = 0;
  void _onPacket(List<int> bytes) {
    // Unconditional throttled log — confirms the Dart handler is invoked at all.
    if (_pktLogCount++ % 25 == 0) {
      debugPrint('BleService: _onPacket #$_pktLogCount, ${bytes.length} bytes');
    }
    final frame = parseBlePacket(bytes);
    if (frame != null) {
      _frameController.add(frame);
    } else {
      if (_dropLogCount++ % 25 == 0) {
        debugPrint('BleService: dropped packet — got ${bytes.length} bytes, '
            'need ${BleProtocol.notificationByteSize} '
            '(20 => MTU too small; 180 => NaN/Inf in payload)');
      }
    }
  }

  void _scheduleReconnect() {
    if (_disposed || _targetMac == null) return;
    final idx =
        _reconnectAttempt.clamp(0, AppConfig.reconnectBackoff.length - 1);
    final delay = AppConfig.reconnectBackoff[idx];
    _reconnectAttempt++;
    debugPrint('BleService: reconnect attempt #$_reconnectAttempt in '
        '${delay.inSeconds} s');
    Future.delayed(delay, () {
      if (!_disposed) _runConnectAttempt();
    });
  }

  Future<void> disconnect() async {
    _targetMac = null;
    _reconnectAttempt = 0;
    await _charSub?.cancel();
    await _connSub?.cancel();
    _charSub = null;
    _connSub = null;
    _controlChar = null;
    if (_device != null) {
      try {
        await _device!.disconnect();
      } catch (_) {}
    }
    _device = null;
    _setState(BleConnectionState.idle);
  }

  Future<void> dispose() async {
    _disposed = true;
    await disconnect();
    await _frameController.close();
    await _stateController.close();
  }
}
