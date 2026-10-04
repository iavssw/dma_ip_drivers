/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QDMA_PERSISTENT_H
#define QDMA_PERSISTENT_H
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include "qdma_persistent_uapi.h"
struct qdma_cdev;
struct qdma_file_ctx {
    struct qdma_cdev *xcdev;
    struct mutex lock;
    struct list_head registrations;
    refcount_t refs;
    bool closing;
    atomic_t quarantined;
    atomic_t uncertain_waits;
    atomic64_t registered, unregistered, live, mappings, invalidations;
    atomic64_t remaps, active, completed, quarantine;
};
/* Serializes open against ordinary queue stop/delete. */
extern struct mutex qdma_cdev_lifecycle_mutex;
int qdma_persistent_init(void);
void qdma_persistent_shutdown(void);
struct qdma_file_ctx *qdma_persistent_open(struct qdma_cdev *xcdev);
void qdma_persistent_close(struct qdma_file_ctx *ctx);
long qdma_persistent_ioctl(struct qdma_file_ctx *ctx, unsigned int cmd,
                           unsigned long arg);
#endif
