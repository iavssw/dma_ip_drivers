/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "qdma_persistent_uapi.h"
#include "qdma_persistent_logic.h"
_Static_assert(sizeof(struct qdma_persistent_caps) == 120, "caps ABI");
_Static_assert(sizeof(struct qdma_persistent_register) == 96, "register ABI");
_Static_assert(sizeof(struct qdma_persistent_transfer) == 88, "transfer ABI");
_Static_assert(sizeof(struct qdma_persistent_unregister) == 48, "unregister ABI");
_Static_assert(sizeof(struct qdma_persistent_stats) == 128, "stats ABI");
_Static_assert(offsetof(struct qdma_persistent_register, handle) == 40, "handle ABI");
_Static_assert(offsetof(struct qdma_persistent_transfer, bytes_transferred) == 32, "response ABI");
_Static_assert(_IOC_NR(QDMA_CDEV_IOCTL_CAPS) == 4, "caps ioctl");
_Static_assert(_IOC_NR(QDMA_CDEV_IOCTL_REGISTER) == 5, "register ioctl");
_Static_assert(_IOC_NR(QDMA_CDEV_IOCTL_TRANSFER_REGISTERED) == 6, "transfer ioctl");
_Static_assert(_IOC_NR(QDMA_CDEV_IOCTL_UNREGISTER) == 7, "unregister ioctl");
_Static_assert(_IOC_NR(QDMA_CDEV_IOCTL_STATS) == 8, "stats ioctl");
_Static_assert(_IOC_SIZE(QDMA_CDEV_IOCTL_TRANSFER_REGISTERED) == 88, "encoded ABI");
static void ranges(void)
{
    assert(qdma_persistent_range_inside(0x1000, 0x100, 0x1000, 0x1000));
    assert(qdma_persistent_range_inside(0x1fff, 1, 0x1000, 0x1000));
    assert(!qdma_persistent_range_inside(0x1fff, 2, 0x1000, 0x1000));
    assert(!qdma_persistent_range_inside(0xfff, 1, 0x1000, 0x1000));
    assert(!qdma_persistent_range_inside(UINT64_MAX-1, 4, 0, UINT64_MAX));
    assert(!qdma_persistent_range_inside(UINT64_MAX-1, 1, UINT64_MAX-1, 4));
    assert(!qdma_persistent_range_inside(0x1000, 0, 0x1000, 0x1000));
    assert(!qdma_persistent_range_inside(0x1000, 1, 0x1000, 0));
}
static void scatterlists(void)
{
    struct qdma_persistent_sg_cursor cursor = { .remaining = 4097 };
    assert(qdma_persistent_take_segment(&cursor, 4096) == 4096);
    assert(qdma_persistent_take_segment(&cursor, 4096) == 1);
    assert(qdma_persistent_take_segment(&cursor, 4096) == 0);
    assert(cursor.entries == 2 && cursor.remaining == 0);
    cursor.remaining = 8193; cursor.entries = 0;
    assert(qdma_persistent_take_segment(&cursor, 0) == 0);
    assert(qdma_persistent_take_segment(&cursor, 4096) == 4096);
    assert(qdma_persistent_take_segment(&cursor, 4096) == 4096);
    assert(cursor.remaining == 1 && cursor.entries == 2); /* insufficient map */
    for (uint32_t target = 1; target < 100000; target += 113) {
        uint64_t sum = 0;
        cursor.remaining = target; cursor.entries = 0;
        for (uint32_t n = 0; n < 1000 && cursor.remaining; ++n) {
            uint32_t available = (n * 127 + target * 17) % 4097;
            uint32_t before = cursor.remaining;
            uint32_t used = qdma_persistent_take_segment(&cursor, available);
            assert(used <= available && used <= before);
            sum += used;
        }
        assert(!cursor.remaining && sum == target);
        assert(qdma_persistent_take_segment(&cursor, UINT32_MAX) == 0);
    }
}
static void completions(void)
{
    assert(qdma_persistent_full_completion(4096, 4096, 0));
    assert(!qdma_persistent_full_completion(4096, 4095, 0));
    assert(!qdma_persistent_full_completion(4096, 0, -6)); /* queue stop */
    assert(!qdma_persistent_full_completion(4096, 4096, -5));
    assert(!qdma_persistent_full_completion(4096, 4097, 0));
    assert(!qdma_persistent_full_completion(0, 0, 0));
}
int main(void)
{
    ranges(); scatterlists(); completions();
    puts("PASS: fixed-width ABI, ioctl numbers, GPU aperture bounds, exact SG accounting, completion proof");
    return 0;
}
