/// Maps a model's compact label token to a human-readable phrase for display
/// and text-to-speech.
///
/// Multi-word signs are trained as spaceless tokens (e.g. HOWAREYOU) so the
/// label round-trips cleanly between the labels file, the model output, and
/// the letter-buffer logic. The UI and TTS, however, need the spaced form —
/// otherwise "HOWAREYOU" is shown run-together and spoken as one garbled word.
///
/// Anything not in the map is returned unchanged (single letters, single
/// words), so this is safe to call on every label.
const Map<String, String> _phraseLabels = {
  'HOWAREYOU': 'HOW ARE YOU',
  'ILOVEYOU': 'I LOVE YOU',
  'THANKYOU': 'THANK YOU',
};

String displayLabel(String raw) {
  return _phraseLabels[raw.trim().toUpperCase()] ?? raw;
}
