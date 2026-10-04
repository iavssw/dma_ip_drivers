/* SPDX-License-Identifier: GPL-2.0 */
/* Small hardware-independent predicates shared with the device-free tests. */
#ifndef QDMA_PERSISTENT_LOGIC_H
#define QDMA_PERSISTENT_LOGIC_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 qdma_logic_u64;
typedef u32 qdma_logic_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t qdma_logic_u64;
typedef uint32_t qdma_logic_u32;
#endif
static inline bool qdma_persistent_range_inside(qdma_logic_u64 address,
    qdma_logic_u64 length, qdma_logic_u64 aperture_start, qdma_logic_u64 aperture_length)
{
    qdma_logic_u64 maximum = ~(qdma_logic_u64)0;
    if (!length || !aperture_length || aperture_start > maximum - aperture_length ||
        address < aperture_start || address > maximum - length)
        return false;
    return address + length <= aperture_start + aperture_length;
}
struct qdma_persistent_sg_cursor {
    qdma_logic_u32 remaining;
    qdma_logic_u32 entries;
};
static inline qdma_logic_u32 qdma_persistent_take_segment(
    struct qdma_persistent_sg_cursor *cursor, qdma_logic_u32 available)
{
    qdma_logic_u32 used = available < cursor->remaining ? available : cursor->remaining;
    if (used) {
        cursor->remaining -= used;
        cursor->entries++;
    }
    return used;
}
static inline bool qdma_persistent_full_completion(qdma_logic_u32 requested,
    qdma_logic_u32 completed, int error)
{
    return requested && !error && requested == completed;
}
#endif
