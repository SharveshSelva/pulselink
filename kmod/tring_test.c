/* tring_test.c — host unit test for the drop-oldest record ring.
 * Build/run: `make test`. This exercises the exact object the kernel links.
 */
#include "tring.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)){ fails++; \
    fprintf(stderr,"FAIL %s:%d ",__FILE__,__LINE__); \
    fprintf(stderr,__VA_ARGS__); fprintf(stderr,"\n"); } } while(0)

static struct telemetry_record rec(unsigned seq)
{
    struct telemetry_record r;
    r.device_id = 0x1001; r.seq = seq;
    r.temperature = (int)(2500 + seq); r.humidity = (int)(5000 - seq);
    r.server_timestamp_ns = (unsigned long long)seq * 1000ull;
    return r;
}

static void test_fifo_order(void)
{
    struct tring r; tring_init(&r);
    for (unsigned i = 1; i <= 10; i++) {
        struct telemetry_record x = rec(i);
        CHECK(tring_push(&r, &x) == 0, "push %u should not drop", i);
    }
    CHECK(tring_count(&r) == 10, "count=10 got %u", tring_count(&r));

    for (unsigned i = 1; i <= 10; i++) {       /* oldest first */
        struct telemetry_record out;
        CHECK(tring_pop(&r, &out) == 1, "pop %u", i);
        CHECK(out.seq == i, "FIFO order: got seq=%u want %u", out.seq, i);
    }
    struct telemetry_record out;
    CHECK(tring_pop(&r, &out) == 0, "pop on empty returns 0");
    CHECK(r.produced == 10 && r.dropped == 0, "counters produced=%llu dropped=%llu",
          r.produced, r.dropped);
}

static void test_drop_oldest(void)
{
    struct tring r; tring_init(&r);

    /* push CAP + 5 records; the ring must keep the last CAP, dropping 5 oldest */
    unsigned total = TRING_CAP + 5;
    int drops = 0;
    for (unsigned i = 1; i <= total; i++) {
        struct telemetry_record x = rec(i);
        drops += tring_push(&r, &x);
    }
    CHECK(tring_count(&r) == TRING_CAP, "count capped at CAP, got %u", tring_count(&r));
    CHECK(drops == 5, "exactly 5 drops, got %d", drops);
    CHECK(r.dropped == 5, "dropped counter = 5, got %llu", r.dropped);
    CHECK(r.produced == total, "produced = %u, got %llu", total, r.produced);

    /* the oldest surviving record should be seq = 6 (1..5 were dropped) */
    struct telemetry_record out;
    CHECK(tring_pop(&r, &out) == 1, "pop after wrap");
    CHECK(out.seq == 6, "oldest survivor seq=6, got %u", out.seq);

    /* drain and confirm strictly increasing, ending at `total` */
    unsigned last = out.seq, n = 1;
    while (tring_pop(&r, &out)) {
        CHECK(out.seq == last + 1, "monotonic: %u after %u", out.seq, last);
        last = out.seq; n++;
    }
    CHECK(n == TRING_CAP, "drained CAP records, got %u", n);
    CHECK(last == total, "last seq = %u, got %u", total, last);
}

int main(void)
{
    test_fifo_order();
    test_drop_oldest();
    if (fails == 0) { printf("tring_test: ALL PASS\n"); return 0; }
    printf("tring_test: %d FAILURES\n", fails);
    return 1;
}
