PulseLink — easiest Wokwi run (paste 3 files, no multi-file juggling)

1. Open https://wokwi.com  ->  New Project  ->  ESP32  (Arduino, NOT C++/ESP-IDF).
2. You'll see two tabs: sketch.ino and diagram.json. Add a third with the "+":
      - sketch.ino       <- paste this folder's sketch.ino (replace all)
      - diagram.json     <- paste this folder's diagram.json (replace all)
      - libraries.txt    <- click "+", name it libraries.txt, paste this folder's
3. Press the green ▶.  Wokwi auto-installs the DHT library on first run.

WITHOUT a server you should see: Wi-Fi join, DHT22 reads, then a backoff loop
("[net] backoff 500ms ...", "[stats] offline ..."). That alone proves the
firmware + sensor + state machine work.

WITH your server (full pipeline): run wokwigw + telemetryd on your machine, then
in Wokwi press F1 -> "Enable Private Wokwi IoT Gateway", ▶. See ../README.md
("Run the full pipeline") for the exact commands.

This single file is auto-generated from the modular sources; the wire-framing in
it is byte-for-byte the same code that's host-tested in the repo.
