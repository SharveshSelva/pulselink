# PulseLink kernel module (Stage 3) — `/dev/telemetry`

A character device that buffers telemetry records in the kernel: `telemetryd`'s
`-s dev` sink (M7) `write(2)`s records in, and any number of `telemetry_cat`
readers `read(2)` them out — oldest first, blocking until data arrives.

```
telemetryd  --write()-->  /dev/telemetry  --read()-->  telemetry_cat (xN)
                          [spinlock-guarded record ring, wait queue]
```

## Files
- `telemetry_uapi.h` — the record struct (identical layout to the server's
  `sensor_record_t`), shared by the module, `telemetry_cat`, and the host test.
- `tring.{c,h}` — the drop-oldest record ring. Freestanding logic with no locks
  or I/O, so the *same object* links into the module and into the host unit test.
- `telemetry_drv.c` — the module: misc device, `read`/`write`/`poll`, wait
  queue, spinlock.
- `telemetry_cat.c` — userspace live reader.
- `telemetry_stat.c` — query/reset ring counters via `ioctl`.
- `tring_test.c` — host unit test for the ring.

## Build & run (on a Linux VM with kernel headers)
```
sudo apt install build-essential linux-headers-$(uname -r)   # once
make                       # builds pulselink_telemetry.ko
sudo insmod pulselink_telemetry.ko
ls -l /dev/telemetry       # created automatically (misc device)
dmesg | tail               # "/dev/telemetry ready ..."

make telemetry_cat
./telemetry_cat            # blocks, printing records as they arrive

# inspect status without the server:
cat /proc/pulselink                 # capacity / in_ring / produced / dropped
make telemetry_stat && ./telemetry_stat        # same via ioctl GET_STATS
./telemetry_stat --reset                        # ioctl RESET the counters

# quick smoke test without the server: write a raw 24-byte record
#   (device_id=0x1001 seq=1 temp=2500 humid=5000 ts=0, little-endian)
printf '\x01\x10\x00\x00\x01\x00\x00\x00\xc4\x09\x00\x00\x88\x13\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' \
  > /dev/telemetry
# telemetry_cat prints: dev=0x00001001 seq=1  25.00 C  50.00 %  ts=0

sudo rmmod pulselink_telemetry
```

## The full pipeline (Stage 3 live)
With the module loaded, run the whole chain end to end:
```
sudo insmod pulselink_telemetry.ko
../server/telemetryd -s dev            # server writes readings into /dev/telemetry
./telemetry_cat                        # in another shell: live records stream out
cat /proc/pulselink                    # produced/dropped climbing
```
Drive it with the firmware (ESP32 → TCP) or `../tools/fake_client` for a quick
synthetic stream. If the module isn't loaded, `telemetryd -s dev` prints a notice
and falls back to the in-memory sink, so it never hard-fails.

## Host-verifying the ring (no kernel needed)
```
make test        # tring_test: FIFO order, drop-oldest, wraparound, counters
```

## Design notes (interview talking points)
- **Spinlock, not a mutex.** The locked critical section is a bounded O(1) struct
  copy with no sleeping; everything that *can* sleep or fault — `copy_to_user`,
  `copy_from_user` — happens outside the lock. So a spinlock is both correct and
  cheaper than a mutex (no scheduler involvement on the hot path).
- **Blocking read via a wait queue.** An empty ring puts the reader to sleep on
  `wait_event_interruptible`; a writer `wake_up`s it. `O_NONBLOCK` gets `-EAGAIN`,
  and a signal gets `-ERESTARTSYS`, so the device behaves like a well-mannered
  Unix file (and supports `poll`/`select`/`epoll`).
- **Drop-oldest when full — the opposite of the firmware.** A slow reader should
  fall behind on *stale* data, never stall the producer (the server), so the ring
  overwrites its oldest record. The firmware's bounded queue drops the *newest*
  instead, because there a full queue means the uplink is down and reconnect, not
  buffered staleness, is the answer. Same structure, opposite policy, each for a
  concrete reason.
- **Misc device, not a manual cdev.** `misc_register` gives a dynamically-allocated
  minor and auto-creates `/dev/telemetry` via udev — far less boilerplate than
  `alloc_chrdev_region` + `cdev_add` + manual class/device creation, which buys
  nothing for a single-node driver.
- **Records copied in/out in batches.** `read` moves up to 32 records per call
  and `write` accepts any whole number of records, so a busy pipeline isn't one
  syscall per reading.
- **`ioctl` + `/proc` for observability.** `PL_IOC_GET_STATS`/`PL_IOC_RESET` give
  a stable binary interface for tools; `/proc/pulselink` gives the same numbers
  as human-readable text for a quick `cat`. Both read the counters under the same
  spinlock.

This completes **Stage 3** — `tag stage3-done`. The full chain runs live:
ESP32 → `telemetryd -s dev` → `/dev/telemetry` → `telemetry_cat`. The only thing
left is the optional Stage 4 imaging path (M8).
