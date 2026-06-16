# PulseLink — IoT Sensor Telemetry Pipeline

A FreeRTOS device (simulated ESP32) streams sensor telemetry over a custom
binary protocol to a multithreaded Linux server, which writes records into a
Linux character-device driver that any number of userspace consumers read live.

```
ESP32 (Wokwi, FreeRTOS) --TCP/UDP--> telemetryd (C, pthreads) --write()--> /dev/telemetry --read()--> telemetry_cat
```

## Build order
The stages are independent but build in this order so each has something real
to talk to: **server (Stage 2) → firmware (Stage 1) → kernel module (Stage 3) → imaging (Stage 4)**.

## Repo layout
```
pulselink/
├── common/    protocol.h, checksum.c, checksum_test.c   <-- the wire contract
├── server/    telemetryd: listeners, workers, device table, record sinks
├── tools/     fake_client (test rig), telemetry_cat (live reader)
├── firmware/  ESP32 / FreeRTOS firmware (Wokwi)
├── kmod/      /dev/telemetry character driver + UAPI header
├── imaging/   3x3 convolution / Sobel / hand-rolled BMP writer (Stage 4)
└── tests/     scripted scenarios driven by fake_client
```

## The wire protocol (summary)
All multi-byte fields are big-endian. Every packet starts with a 12-byte header:

| off | size | field        |
|-----|------|--------------|
| 0   | 2    | magic 0x504C |
| 2   | 1    | version 0x01 |
| 3   | 1    | type         |
| 4   | 4    | device_id    |
| 8   | 2    | payload_len  |
| 10  | 2    | checksum     |

Types: HELLO / HELLO_ACK / SENSOR / ACK / HEARTBEAT(UDP) / FRAME / ERROR.
Full detail and payload layouts live in `common/protocol.h` — the single source
of truth, compiled identically on both the ESP32 and the server.

## Build & test
```
cd common && make test      # builds and runs the checksum unit tests
```

## Milestone tracker
- [x] **M1** protocol.h + checksum + unit test (cross-checked vs RFC 1071) ✅
- [x] **M2** server: TCP listener + worker state machine + HELLO handshake; fake_client test rig ✅
- [x] **M3** server: device table, full sensor handling + ACK-every-8th, UDP heartbeats, record sink; tsan-clean → **stage2-done** ✅
- [x] **M4** Wokwi firmware: FreeRTOS sampler→queue→reporter + heartbeat; framing host-tested vs server ✅
- [x] **M5** Wokwi firmware: Wi-Fi + TCP uplink, HELLO handshake, reconnect-with-backoff, UDP heartbeat, task watchdog → **stage1-done** ✅
- [x] **M6** kernel module: `/dev/telemetry` misc device, write/read + wait queue, spinlock-guarded drop-oldest ring; `telemetry_cat` reader; ring host-tested ✅
- [x] **M7** kernel `ioctl` (stats/reset) + `/proc/pulselink`; server `-s dev` writes into `/dev/telemetry` → full pipeline live → **stage3-done** ✅
- [x] **M8** imaging path: `PKT_FRAME` reassembly, 3×3 convolution / Sobel edge detection, hand-rolled BMP writer; server `-F` emits edge BMPs from received frames → **stage4-done** ✅

**Status: complete (8/8).** All four stages ship and interoperate — the ESP32
firmware, the multithreaded `telemetryd`, the `/dev/telemetry` kernel driver, and
the Stage 4 imaging path all speak the same protocol/record formats, verified end
to end. The server can stream sensor readings into the kernel device *and* turn
received `PKT_FRAME` camera frames into edge-detected BMPs.

## Design decisions (interview talking points — grows per stage)
- **Checksum = RFC 1071 ones'-complement (the "IP checksum").** Cheap to compute
  (add + carry-fold + complement), endian-defined, catches all single-bit and
  most burst errors. Not cryptographic — it guards against line noise and
  truncation, not tampering. It has a self-verifying property (sum payload +
  checksum, complement → 0) which the unit test exercises directly.
- **`protocol.h` carries no `arpa/inet.h` / lwIP dependency.** Byte-swapping
  happens at call sites so the identical header compiles on the ESP32 and Linux.
  `_Static_assert` on every struct size means a stray padding byte breaks the
  build instead of corrupting the wire.
- **Signal handling uses the self-pipe trick.** SIGINT/SIGTERM does only
  async-signal-safe work (an atomic store + a one-byte `write()` to a pipe). The
  TCP listener `poll()`s the listening socket *and* the pipe's read end, so a
  signal becomes a pollable event and shutdown is immediate — no blocking in
  `accept()`, no real work inside the handler.
- **Concurrency cap = counting semaphore, reject-at-the-door.** The listener
  `sem_trywait`s *after* `accept()` and closes the connection when full, rather
  than `sem_wait`-blocking the accept loop. This keeps the server responsive to
  shutdown and surfaces overload instead of hiding it in the listen backlog.
- **`read_n` over `recv`.** TCP is a byte stream; a single `recv` can hand back
  half a header. `read_n` loops to the exact count and reports EOF (short read)
  vs error vs `SO_RCVTIMEO` timeout (EAGAIN) distinctly, which is what lets the
  worker reap silent peers and frame messages reliably.
- **`MSG_NOSIGNAL` on every send** so a vanished peer yields `EPIPE` instead of
  killing the process with `SIGPIPE`.
- **Device table: rwlock guards structure, atomics guard fields.** The rwlock
  protects only the bucket chains — *inserting* a new device takes the write
  lock, *looking one up* takes the read lock. Every hot per-device field
  (counters, last_seq, last_seen, state) is a C11 atomic updated with no table
  lock at all. This is what makes the rwlock the right call: inserts are rare
  (once per new device), lookups + the stats dump are constant, so N workers
  update N different devices fully in parallel and a SIGUSR1 reader never blocks
  them. A plain mutex would serialize all of that.
- **Devices are never removed — only flipped to OFFLINE.** That single decision
  is what makes the lock-free field updates safe: a `device_t*` handed to a
  worker stays valid for the whole process lifetime, so the worker can keep
  updating atomics without re-taking the table lock or fearing a free().
- **`get_or_create` = read-lock lookup, then upgrade-with-recheck.** POSIX
  rwlocks can't atomically upgrade, so the miss path drops the read lock, takes
  the write lock, and *re-searches* before inserting (another thread may have
  created the device in the gap). Verified race-free with the 8-thread,
  160k-op `dt_test` under ThreadSanitizer.
- **One `sink_t` v-table seam for the record store.** The worker writes each
  reading through `sink->write_record(sensor_record_t*)`. The M3 sink is an
  in-memory ring buffer; the Stage-3 `/dev/telemetry` kernel device slots in
  behind the identical interface with no worker changes, and the record layout
  already matches the planned kernel struct.
- **Shutdown wakes each thread the cheapest way that fits it.** The
  latency-critical accept loop uses the self-pipe for an instant wakeup; the UDP
  and reaper loops just re-check the flag on a 500ms tick; detached workers wake
  on a 2s recv tick (which also doubles as the 30s dead-peer reap counter). Main
  joins the three service threads, then drains workers via an `active_workers`
  atomic before tearing down shared state.
- **SIGUSR1 stays trivially async-signal-safe.** The handler only sets an atomic
  flag; the reaper thread notices it and does the actual `dt_dump` under the
  read lock — proving rwlock read-concurrency while workers keep writing.
- **Firmware (M4): bounded queue decouples sampling from uplink.** A fixed-length
  FreeRTOS queue between the sampler and the uplink task means the producer never
  blocks on a stalled consumer, and a full queue is a *counted drop* rather than
  unbounded RAM growth. Drop-newest on the device (if the queue fills, the link
  is effectively down and M5's reconnect is what matters) vs drop-oldest in the
  kernel ring (a slow reader shouldn't be served stale frames) — same primitive,
  opposite policy, each justified. Tasks are pinned across both ESP32 cores so
  the blocking network send added in M5 can't jitter the sample cadence.
- **Firmware framing is the same host-tested C as the server.** `pl_build_sensor()`
  writes big-endian bytes explicitly (no `htons`, no packing reliance) so it
  compiles identically on the ESP32 and the host; `fw_frame_test` builds a packet
  and parses it back through the server's own `pl_parse_header`, proving the two
  ends agree byte-for-byte before any real socket exists.
- **Firmware (M5): reconnect with exponential backoff + a task watchdog.** A
  flapping link doesn't become a tight reconnect spin — backoff doubles to a 16s
  cap and resets on a successful handshake — and the watchdog (fed *through* the
  backoff waits) resets the chip if the network task ever wedges. The bounded
  queue keeps buffering during an outage, so a transient disconnect costs the
  oldest unsent samples, not a crash. The reply path uses a `read_exact` helper
  with the same byte-stream contract as the server's `read_n`.
- **Kernel driver (M6): spinlock over mutex, drop-oldest ring, misc device.**
  The locked critical section is a bounded O(1) record copy with no sleeping, and
  all user-copying happens outside the lock — so a spinlock is correct and
  cheaper than a mutex. An empty ring blocks the reader on a wait queue
  (`-EAGAIN` for `O_NONBLOCK`, `-ERESTARTSYS` on a signal, plus `poll` support).
  The ring drops the *oldest* record when full (a slow reader falls behind on
  stale data rather than stalling the server) — deliberately the opposite of the
  firmware's drop-newest. `misc_register` auto-creates `/dev/telemetry` with far
  less boilerplate than a manual cdev. The ring logic is freestanding C, so the
  same object the kernel links is host unit-tested.
- **Stage 3 wiring (M7): ioctl + /proc, and a record sink over write(2).** The
  driver exposes `PL_IOC_GET_STATS`/`PL_IOC_RESET` (a stable binary interface for
  tools) and `/proc/pulselink` (the same counters as text for a quick `cat`),
  both read under the ring spinlock. The server's `-s dev` sink just `write(2)`s
  the native `sensor_record_t` into `/dev/telemetry` — no byte-swapping, because
  this hop is local IPC on one machine, unlike the big-endian network wire. If the
  module isn't loaded the sink falls back to the in-memory ring rather than
  hard-failing. The record path was proven end-to-end on the host (sink writes →
  reader decodes via the kernel's own struct) before ever touching a real `.ko`.
- **Imaging (M8): frames on the same protocol, a hand-rolled BMP, no image libs.**
  A camera frame is just `ceil(w*h/1024)` `PKT_FRAME` packets — same header and
  checksum as everything else — so Stage 4 needed no new transport. The server
  reassembles defensively (dimension caps, exact per-chunk lengths, consistent
  `chunk_count`, dedup) then runs an edge-clamped 3×3 Sobel and writes an 8-bit
  grayscale BMP by hand (explicit little-endian headers, palette, bottom-up rows,
  4-byte padding) — the little-endian mirror of the wire code. The whole path is
  host-tested, and `server/frame_e2e_test` drives a chunked frame through the real
  `client_worker` and checks the BMP it produces is byte-identical to the source.
```

## Running it
```
# unit + concurrency tests (no network needed)
cd server
make dt_test    && ./dt_test       # device table: unit + 8-thread stress
make integ      && ./integ_test     # full worker pipeline over socketpairs
make integ_tsan && ./integ_test_tsan # same, under ThreadSanitizer (race-check)

# live server
make
./telemetryd -p 9000 -u 9001 -n 16              # TCP 9000, UDP heartbeats 9001
kill -USR1 $(pgrep telemetryd)                  # dump device table + sink stats
kill -INT  $(pgrep telemetryd)                  # graceful shutdown

# drive it
cd ../tools && make
./fake_client -h 127.0.0.1 -p 9000 -u 9001 -d 0x1001 -r 10 -n 50   # stream + heartbeats
./fake_client -p 9000 -d 0x2002 --bad-checksum -n 10               # ERROR x3 -> drop
```
The server also accepts `-T <seconds>` to self-shutdown after a deadline (a test
aid for sandboxes that can't background a long-lived process).

### Full pipeline through the kernel (Stage 3, on a Linux VM)
```
cd kmod && make && sudo insmod pulselink_telemetry.ko   # creates /dev/telemetry
../server/telemetryd -s dev &                           # readings -> /dev/telemetry
./telemetry_cat                                         # live records stream out
cat /proc/pulselink                                     # produced/dropped counters
```
Drive the server with the ESP32 firmware (Wokwi/real hardware) or `fake_client`.

### Camera frames → edge BMPs (Stage 4)
```
./telemetryd -F ./frames &                          # -F enables frame output
cd tools && ./fake_client -p 9000 -d 0x1001 --frame # send one synthetic frame
ls ../frames/                                        # dev00001001_in.bmp + _edges.bmp
cd ../imaging && make demo                           # or run the imaging path standalone
```
