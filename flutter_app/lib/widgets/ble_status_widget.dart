import 'package:flutter/material.dart';

import '../models/ble_connection_state.dart';

/// Compact pill showing the BLE connection status, with optional callback
/// when the user taps it (e.g. to reconnect).
class BleStatusWidget extends StatelessWidget {
  final BleConnectionState state;
  final VoidCallback? onTap;

  const BleStatusWidget({super.key, required this.state, this.onTap});

  Color _bg(BuildContext ctx) {
    final cs = Theme.of(ctx).colorScheme;
    switch (state) {
      case BleConnectionState.connected:
        return cs.primaryContainer;
      case BleConnectionState.scanning:
      case BleConnectionState.connecting:
      case BleConnectionState.reconnecting:
        return cs.secondaryContainer;
      case BleConnectionState.permissionDenied:
      case BleConnectionState.bluetoothOff:
      case BleConnectionState.receiverNotFound:
      case BleConnectionState.error:
        return cs.errorContainer;
      case BleConnectionState.idle:
      case BleConnectionState.disconnected:
        return cs.surfaceContainerHighest;
    }
  }

  IconData _icon() {
    switch (state) {
      case BleConnectionState.connected:
        return Icons.bluetooth_connected;
      case BleConnectionState.scanning:
        return Icons.bluetooth_searching;
      case BleConnectionState.connecting:
      case BleConnectionState.reconnecting:
        return Icons.bluetooth_searching;
      case BleConnectionState.bluetoothOff:
        return Icons.bluetooth_disabled;
      case BleConnectionState.permissionDenied:
        return Icons.lock;
      case BleConnectionState.receiverNotFound:
      case BleConnectionState.error:
        return Icons.error_outline;
      case BleConnectionState.idle:
      case BleConnectionState.disconnected:
        return Icons.bluetooth;
    }
  }

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(20),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        decoration: BoxDecoration(
          color: _bg(context),
          borderRadius: BorderRadius.circular(20),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (state.isActive)
              const SizedBox(
                width: 16,
                height: 16,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            else
              Icon(_icon(), size: 18),
            const SizedBox(width: 8),
            Text(state.displayName,
                style: Theme.of(context).textTheme.labelMedium),
          ],
        ),
      ),
    );
  }
}
