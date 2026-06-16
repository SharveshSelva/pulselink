# PulseLink — IoT Sensor Telemetry 

A tiny sensor device measures temperature and humidity, sends those readings over the network to a server, the server files each reading into a special slot in the Linux operating system, and a small reader prints them live.

It's a project in C that touches three areas that rarely appear together: **embedded firmware** (a simulated ESP32 running FreeRTOS), a **multithreaded network server**, and a **Linux kernel driver** — all speaking one shared message format. 

```
ESP32 firmware  --TCP/UDP-->  telemetryd server  --write()-->  /dev/telemetry  --read()-->  telemetry_cat
  (reads sensor,             (decodes, tracks                (Linux kernel             (prints the
   sends packets)             devices, files it)              char device)              live stream)
```

---

## Components

1. **The device** (`firmware/`) — a simulated ESP32 chip with a temperature/humidity sensor. Every 2 seconds it reads the sensor, wraps the numbers in a small packet, and sends it over WiFi. If the network drops, it keeps retrying. Runs in [Wokwi](https://wokwi.com).

2. **The server** (`server/` → `telemetryd`) — listens for packets, checks each one isn't corrupted, records which device sent what, and passes it along. Handles many devices at once.

3. **The Linux "mailbox"** (`kmod/` → `/dev/telemetry`) — a kernel driver that makes the server's readings show up as a *file* on Linux. The server writes readings in; anyone can read them out live.

4. **The reader** (`kmod/telemetry_cat`) — opens that mailbox file and prints each reading as it arrives.

There's also a **bonus imaging path** (`imaging/`): the device can send a small picture, and the server runs edge-detection on it and saves a BMP.
A **test client** (`tools/fake_client`) stands in for the real device so the whole server + kernel pipeline can be demonstrated on one machine.

---

## Repo layout

```
pulselink/
├── common/    protocol.h, checksum — the wire contract
├── server/    telemetryd: listeners, workers, device table, record sinks
├── tools/     fake_client (test device)
├── firmware/  ESP32 / FreeRTOS firmware (Wokwi) + single-file sketch
├── kmod/      /dev/telemetry character driver + telemetry_cat reader
└── imaging/   3x3 convolution / Sobel / hand-rolled BMP writer
```
---

## Quick start

```bash
# 1. build + run the server
cd server && make && ./telemetryd -p 9000 -u 9001 -n 16

# 2. in another terminal, drive it with the test device
cd tools && make
./fake_client -h 127.0.0.1 -p 9000 -u 9001 -d 0x1001 -r 10 -n 50
```

You'll watch the server register the device, decode live temperature/humidity readings, and ACK every 8th packet. That's the pipeline working end to end.

```bash
# run the test suite (no network needed)
cd common  && make test                              # checksum
cd server  && make dt_test    && ./dt_test           # device table, 8-thread stress
cd server  && make integ      && ./integ_test        # full server pipeline
cd server  && make integ_tsan && ./integ_test_tsan   # same, race-checked
cd imaging && make test                              # imaging path
```

---

## The message format (the wire protocol)

Every packet starts with a 12-byte header (all multi-byte fields big-endian):

| offset | size | field |
|---|---|---|
| 0 | 2 | magic `0x504C` ("PL") |
| 2 | 1 | version `0x01` |
| 3 | 1 | type |
| 4 | 4 | device_id |
| 8 | 2 | payload length |
| 10 | 2 | checksum (RFC-1071) |

Packet types: HELLO / HELLO_ACK / SENSOR / ACK / HEARTBEAT (UDP) / FRAME / ERROR. The full layouts live in `common/protocol.h` — the single source of truth, compiled identically on the ESP32 and the server.

---

<details>
<summary><b>Design decisions</b></summary>

**Protocol & checksum**
- **RFC-1071 ones'-complement checksum** ("the IP checksum"): cheap (add + carry-fold + complement), endian-defined, catches all single-bit and most burst errors. Not cryptographic — guards against line noise, not tampering. Its self-verifying property is exercised directly by the unit test.
- **`protocol.h` has no networking dependency.** Byte-swapping happens at call sites so the identical header compiles on the ESP32 and Linux. A `_Static_assert` on every struct size means a stray padding byte breaks the build instead of corrupting the wire.

**Server concurrency**
- **Self-pipe trick for shutdown.** SIGINT/SIGTERM does only async-signal-safe work (an atomic store + a one-byte pipe write); the listener `poll()`s the socket *and* the pipe, so shutdown is immediate with no work inside the handler.
- **Counting semaphore, reject-at-the-door.** The listener tries the semaphore *after* `accept()` and closes the connection when full, rather than blocking the accept loop — overload is surfaced, not hidden in the backlog.
- **`read_n` over `recv`.** TCP is a byte stream; one `recv` can return half a header. `read_n` loops to the exact count and distinguishes EOF vs error vs timeout, which is what lets the worker frame messages and reap dead peers reliably.
- **`MSG_NOSIGNAL` on every send** so a vanished peer yields `EPIPE` instead of killing the process.
- **Device table: rwlock guards the structure, atomics guard the fields.** The rwlock protects only the bucket chains (insert = write lock, lookup = read lock); hot per-device fields are C11 atomics updated with no table lock. Inserts are rare, lookups are constant, so N workers update N devices in parallel and the stats dumper never blocks them — exactly where a rwlock beats a mutex.
- **Devices are never removed, only flipped OFFLINE.** That makes the lock-free field updates safe: a `device_t*` handed to a worker stays valid for the process lifetime.
- **`get_or_create` upgrades with a re-check.** POSIX rwlocks can't atomically upgrade, so the miss path drops the read lock, takes the write lock, and re-searches before inserting. Verified race-free under ThreadSanitizer with an 8-thread, 160k-op stress test.
- **One `sink_t` v-table** for the record store: the in-memory ring (M3) and the `/dev/telemetry` kernel device (M7) slot in behind the same interface with no worker changes.

**Firmware**
- **Bounded queue decouples sampling from uplink.** A full queue is a *counted drop*, not unbounded RAM growth. The device drops the *newest* (if the queue's full the link is down, so reconnect is what matters); the kernel ring drops the *oldest* (a slow reader shouldn't get stale frames) — same primitive, opposite policy, each justified.
- **Same host-tested framing as the server.** `pl_build_sensor()` writes big-endian bytes explicitly (no `htons`, no packing reliance); `fw_frame_test` builds a packet and parses it back through the server's own parser, proving both ends agree before any socket exists.
- **Reconnect with exponential backoff + a task watchdog.** Backoff doubles to a 16s cap and resets on a good handshake; the watchdog (fed through the backoff waits) resets the chip if the network task ever wedges.

**Kernel driver**
- **Spinlock over mutex, drop-oldest ring, misc device.** The locked section is a bounded O(1) record copy with no sleeping, and all user-copying happens outside the lock — so a spinlock is correct and cheaper. An empty ring blocks the reader on a wait queue (`-EAGAIN` for `O_NONBLOCK`, `-ERESTARTSYS` on a signal, plus `poll` support). `misc_register` auto-creates `/dev/telemetry` with far less boilerplate than a manual cdev. The ring logic is freestanding C, so the same object the kernel links is host unit-tested.
- **ioctl + /proc.** `PL_IOC_GET_STATS`/`RESET` give tools a stable binary interface; `/proc/pulselink` shows the same counters as text. The `-s dev` sink `write(2)`s the native record straight in (no byte-swap — it's local IPC, unlike the network wire), and falls back to the in-memory ring if the module isn't loaded.

**Imaging**
- **Frames ride the same protocol.** A frame is just `ceil(w*h/1024)` `PKT_FRAME` packets — same header and checksum — so no new transport was needed. The reassembler is defensive (dimension caps, exact chunk lengths, dedup), then runs an edge-clamped 3×3 Sobel and writes an 8-bit grayscale BMP by hand (explicit little-endian, palette, bottom-up padded rows). `frame_e2e_test` drives a chunked frame through the real worker and checks the BMP byte-for-byte.

</details>

<details>
<summary><b>Running the kernel pipeline & imaging (click to expand)</b></summary>

**Full pipeline through the kernel** (on a Linux box with kernel headers):
```bash
cd kmod && make && sudo insmod pulselink_telemetry.ko   # creates /dev/telemetry
../server/telemetryd -s dev &                            # readings -> /dev/telemetry
./telemetry_cat                                          # live records stream out
cat /proc/pulselink                                      # produced/dropped counters
sudo rmmod pulselink_telemetry
```

**Camera frames → edge BMPs:**
```bash
./telemetryd -F ./frames &
cd tools && ./fake_client -p 9000 -d 0x1001 --frame      # send one synthetic frame
ls ../frames/                                            # dev00001001_in.bmp + _edges.bmp
cd ../imaging && make demo                               # or run imaging standalone
```

**Fault-injection demos:**
```bash
./fake_client -p 9000 -d 0x2002 --bad-checksum -n 10     # server: ERROR x3 -> drop
kill -USR1 $(pgrep telemetryd)                           # dump device table + stats
```

Notes: the kernel `.ko` and the ESP32 `.ino` can't be built in a plain CI sandbox (no kernel headers / no xtensa toolchain) — the `.ko` runs on a real Linux box and the `.ino` runs in Wokwi. Everything else builds clean with `-Wall -Wextra -Werror -std=c11`.

</details>
