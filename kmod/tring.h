/* tring.h — fixed-capacity record ring with a drop-oldest policy.
 *
 * Deliberately freestanding: no libc, no kernel headers, no locking, no I/O —
 * just the data-structure logic. That is exactly what makes it host-testable
 * (tring_test.c) while the SAME object links into the kernel module, which
 * wraps it with a spinlock + wait queue. Records are copied by value (struct
 * assignment), so there's no memcpy dependency either.
 */
#ifndef PULSELINK_TRING_H
#define PULSELINK_TRING_H

#include "telemetry_uapi.h"

#define TRING_CAP 256

struct tring {
    struct telemetry_record buf[TRING_CAP];
    unsigned          head;       /* index of next write slot          */
    unsigned          count;      /* number of valid records (<= CAP)  */
    unsigned long long produced;  /* total ever pushed                 */
    unsigned long long dropped;   /* total overwritten (ring was full) */
};

void tring_init(struct tring *r);

/* Push one record. If the ring is full, the OLDEST record is overwritten and 1
 * is returned (a drop); otherwise 0. */
int  tring_push(struct tring *r, const struct telemetry_record *rec);

/* Pop the oldest record into *out. Returns 1 if a record was popped, 0 if the
 * ring was empty. */
int  tring_pop(struct tring *r, struct telemetry_record *out);

static inline unsigned tring_count(const struct tring *r) { return r->count; }

#endif /* PULSELINK_TRING_H */
