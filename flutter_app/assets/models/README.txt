Place your Edge Impulse exports here:

  asl-letters.tflite           ← Letters model (A–Z, ~26 classes)
  asl-letters-labels.txt       ← one label per line, in the order trained
  asl-words.tflite             ← Words model (~15-20 classes)
  asl-words-labels.txt         ← one label per line

EXPORT FROM EDGE IMPULSE:
  Sidebar → Deployment → TensorFlow Lite → Build
  Download the zip, extract:
    - model.tflite  → rename to asl-letters.tflite (or asl-words.tflite)
    - labels file   → asl-letters-labels.txt (or asl-words-labels.txt)

LABEL FILE FORMAT:
One class label per line, in the SAME ORDER as the model was trained.
For Letters typically:
  A
  B
  C
  ...
  Z
For Words something like:
  hello
  yes
  no
  thank_you
  ...

If your model file is named differently, either rename it OR adjust
PreferencesService.modelAssetPath in lib/services/preferences_service.dart.
