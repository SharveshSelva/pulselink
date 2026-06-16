# PulseLink firmware (Stage 1) — ESP32 / FreeRTOS

The on-device half of the pipeline: a FreeRTOS app that samples a sensor and
streams readings to `telemetryd` over the PulseLink protocol. Built on
Arduino-ESP32 (which *is* FreeRTOS underneath — `xTaskCreate`, `xQueueCreate`,
`esp_task_wdt`, … are the real APIs) so it runs in the Wokwi simulator with zero
local toolchain. The same task layout ports to ESP-IDF unchanged.

## Tasks
```
[sensor_task]  prio 3, core 1  --(bounded queue)-->  [net_task]  prio 2, core 0  --TCP--> telemetryd
[heartbeat_task] prio 1, core 0  --UDP--> telemetryd (every 5s)
[stats_task]     prio 1, core 0  --> Serial (every 5s)
```
- **sensor_task** reads the DHT22 every 2 s (its ~0.5 Hz max rate) and enqueues;
  never blocks on the network. A sensor-read error falls back to a synthetic
  value (flagged) so the pipeline keeps flowing.
- **net_task** runs the connection state machine: Wi-Fi join → TCP connect →
  HELLO/HELLO_ACK handshake → stream framed packets, draining `ACK`/`ERROR`
  replies. Any failure tears down and retries with **exponential backoff**
  (0.5s→16s); the queue keeps buffering during the outage.
- **heartbeat_task** sends a UDP `PKT_HEARTBEAT` (uptime, free heap) every 5s.
- A **task watchdog** resets the chip if `net_task` ever wedges.

## Files to upload to Wokwi
`pulselink_fw.ino`, `telemetry.h`, `telemetry.c`, `checksum.c`, `protocol.h`,
`diagram.json`, `libraries.txt`. The diagram wires a **wokwi-dht22** to GPIO15
(plus an LED on GPIO2); `libraries.txt` pulls in **"DHT sensor library for ESPx"**
(provides `DHTesp.h`), which Wokwi installs automatically.

## Run it in Wokwi (firmware only — no server yet)
1. <https://wokwi.com> → **New Project** → **ESP32** (Arduino).
2. Add the files above via the file tabs (`+`), pasting each one's contents.
   Replace the default `sketch.ino` with `pulselink_fw.ino`.
3. Press **▶**. With no server reachable you'll see Wi-Fi join, then the
   reconnect/backoff loop — proof the state machine and the DHT22 read both work:
```
PulseLink firmware (M5): Wi-Fi + TCP uplink, reconnect, heartbeat, watchdog
[net] Wi-Fi up, IP 10.13.37.x
[net] TCP connect host.wokwi.internal:9000
[net] backoff 500ms
[stats] offline  produced=3 sent=0 dropped=0  queue=3/16  heap=...
```
   Click the DHT22 in the diagram while it runs to change temperature/humidity and
   watch the streamed values follow.

## Run the full pipeline (sim → your server)
The sketch already targets `host.wokwi.internal`, which the **Wokwi Private IoT
Gateway** maps to the machine running your server (TCP **and** UDP). On that
machine:
1. Install + run the gateway: download `wokwigw` from
   <https://github.com/wokwi/wokwigw/releases>, run it; it prints that it's
   running. (Linux/Windows; on Windows allow it through the firewall.)
2. Start the server: `./telemetryd -p 9000 -u 9001` (add `-s dev` on the VM to
   write into `/dev/telemetry`, or `-F ./frames` for camera frames).
3. In the Wokwi editor press **F1 → "Enable Private Wokwi IoT Gateway"**, then ▶.

You should then see, in the Wokwi serial monitor:
```
[net] HELLO_ACK session=1
[net] STREAMING
[net] <- ACK seq=8
[stats] online  produced=20 sent=20 dropped=0  queue=0/16  heap=...
```
and in `telemetryd`'s log: the device registering, decoded readings, and UDP
heartbeats arriving. Kill the server mid-run → the sim logs `send failed ->
reconnect` and backs off; restart it → it reconnects and resumes.

> Note: `host.wokwi.internal` only resolves with the **Private** gateway running.
> The default Public gateway gives internet access but **cannot** reach your
> machine — if you only see backoff, the private gateway isn't enabled/running.

## Host-verifying the wire format
The framing/parsing seam is plain C, tested off-device against the server's own
code (both directions):
```
make test   # builds fw_frame_test; firmware <-> server must agree on the wire
```

## Design notes (interview talking points)
- **Bounded queue + producer never blocks.** Sampling and uplink run at
  independent cadences and either can stall. A fixed-length queue decouples them
  and makes a stalled link a *counted drop*, not unbounded RAM growth. Drop-newest
  on the device (a full queue means the link is down, so M5's reconnect — not
  queue contents — is what matters) vs drop-oldest in the kernel ring (a slow
  reader shouldn't get stale frames). Same primitive, opposite policy, each
  justified.
- **Reconnect with exponential backoff, reset on success.** A flapping server or
  Wi-Fi doesn't turn into a tight reconnect spin; the backoff doubles to a 16s
  cap and resets once a connection handshakes. The watchdog is fed *through* the
  long waits so backoff never trips it.
- **Task watchdog on the network task.** `net_task` must check in within 10s or
  the TWDT panics and resets the chip — the standard embedded answer to "what if
  a driver/socket call wedges forever."
- **`read_exact` for replies.** TCP is a byte stream, so a single read can return
  half a header; the helper loops to the exact length with a deadline, the same
  contract as the server's `read_n`.
- **Tasks pinned across both cores** so the blocking network send can't jitter
  the sample cadence (sampler core 1, network core 0).
- **Framing is shared, host-tested C.** `pl_build_*`/`pl_parse_header_fw` write
  and read big-endian bytes explicitly (no `htons`, no packing reliance), compile
  identically on the ESP32 and the host, and `fw_frame_test` proves both ends
  agree — firmware packets parse through the server's decoder and the server's
  replies parse through the firmware's, including bad-magic rejection.

This completes **Stage 1**. Next is the kernel side (Stage 3): a `/dev/telemetry`
character driver the server writes records into and `telemetry_cat` reads live.
