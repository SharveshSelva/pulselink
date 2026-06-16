/* tring.c — see tring.h. Pure logic; compiles in the kernel and on the host. */
#include "tring.h"

void tring_init(struct tring *r)
{
    r->head     = 0;
    r->count    = 0;
    r->produced = 0;
    r->dropped  = 0;
}

int tring_push(struct tring *r, const struct telemetry_record *rec)
{
    int dropped = 0;

    if (r->count == TRING_CAP) {
        /* Full: head == oldest. Overwrite it and advance; count stays at CAP,
         * so the new record becomes the newest and the oldest is gone. */
        r->dropped++;
        dropped = 1;
        r->buf[r->head] = *rec;
        r->head = (r->head + 1) % TRING_CAP;
    } else {
        r->buf[r->head] = *rec;
        r->head = (r->head + 1) % TRING_CAP;
        r->count++;
    }
    r->produced++;
    return dropped;
}

int tring_pop(struct tring *r, struct telemetry_record *out)
{
    unsigned tail;

    if (r->count == 0)
        return 0;

    tail = (r->head - r->count + TRING_CAP) % TRING_CAP;   /* oldest slot */
    *out = r->buf[tail];
    r->count--;
    return 1;
}
