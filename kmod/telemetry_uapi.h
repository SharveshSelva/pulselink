/* telemetry_uapi.h — the userspace/kernel contract for /dev/telemetry.
 *
 * The record layout is identical to the server's sensor_record_t (see
 * server/sink.h), so telemetryd's "dev" sink (M7) writes these straight through
 * write(2) and telemetry_cat reads them straight back. One struct, three
 * builds: the kernel module, telemetry_cat, and the host ring unit-test.
 */
#ifndef _PULSELINK_TELEMETRY_UAPI_H
#define _PULSELINK_TELEMETRY_UAPI_H

#ifdef __KERNEL__
#  include <linux/types.h>
#  define PL_U32 __u32
#  define PL_S32 __s32
#  define PL_U64 __u64
#else
#  include <stdint.h>
#  define PL_U32 uint32_t
#  define PL_S32 int32_t
#  define PL_U64 uint64_t
#endif

#define PL_TELEMETRY_DEVNAME "telemetry"   /* -> /dev/telemetry */

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#else
#  include <sys/ioctl.h>
#endif

struct telemetry_record {
    PL_U32 device_id;
    PL_U32 seq;
    PL_S32 temperature;          /* centi-degC */
    PL_S32 humidity;             /* centi-%    */
    PL_U64 server_timestamp_ns;
};   /* 24 bytes; identical layout on both sides */

/* ioctl interface (M7): query / reset the ring counters. */
struct pl_stats {
    PL_U64 produced;             /* total records ever written       */
    PL_U64 dropped;              /* total overwritten (ring full)    */
    PL_U32 in_ring;              /* records currently buffered       */
    PL_U32 capacity;            /* ring capacity in records         */
};

#define PL_IOC_MAGIC      'P'
#define PL_IOC_GET_STATS  _IOR(PL_IOC_MAGIC, 1, struct pl_stats)
#define PL_IOC_RESET      _IO(PL_IOC_MAGIC, 2)

#endif /* _PULSELINK_TELEMETRY_UAPI_H */
