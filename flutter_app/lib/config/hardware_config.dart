/// Receiver-specific constants that should be baked into the app once the
/// physical hardware is finalised. End users never see or edit these.
library;

class HardwareConfig {
  /// BLE MAC address of the production DevKit1 receiver.
  ///
  /// Replace this placeholder with the receiver's real MAC once the glove
  /// is assembled. The receiver firmware prints it on the serial monitor at
  /// boot (`Receiver MAC: XX:XX:XX:XX:XX:XX`), or scan for advertised name
  /// `ASL_Glove` with any BLE scanner app.
  ///
  /// While the placeholder is left in, the app's "Connect" button will fail
  /// to find a device — useful as a deliberate dev-time sanity check.
  static const String receiverMac = '58:8c:81:ad:e1:22';

  /// True if [receiverMac] still holds the placeholder. Used to surface a
  /// warning banner in the UI so a misconfigured build is obvious.
  static bool get isPlaceholder => receiverMac == 'AA:BB:CC:DD:EE:FF';
}
