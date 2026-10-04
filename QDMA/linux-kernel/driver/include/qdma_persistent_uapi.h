/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef QDMA_PERSISTENT_UAPI_H
#define QDMA_PERSISTENT_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define QDMA_PERSISTENT_ABI_VERSION 1U
#define QDMA_PERSISTENT_BUILD_ID "persistent-dmabuf-v1-20260918"
#define QDMA_PERSISTENT_F_DYNAMIC_ATTACH (1ULL << 0)
#define QDMA_PERSISTENT_F_VRAM_REQUIRED  (1ULL << 1)
#define QDMA_PERSISTENT_F_RESV_FENCES    (1ULL << 2)
#define QDMA_PERSISTENT_F_SAFE_TIMEOUT  (1ULL << 3)
#define QDMA_PERSISTENT_F_STATS         (1ULL << 4)
#define QDMA_PERSISTENT_FEATURES 31ULL
/* All requests must be zero-initialized, then size and version set. */
struct qdma_persistent_caps {
    __u32 size;
    __u32 version;
    __u64 features;
    __u64 max_transfer_bytes;
    char build_id[64];
    __u64 reserved[4];
};
struct qdma_persistent_register {
    __u32 size;
    __u32 version;
    __s32 dmabuf_fd;
    __u32 direction; /* legacy values: 0 FPGA->GPU, 1 GPU->FPGA */
    __u64 length;
    __u32 gpu_domain;
    __u32 gpu_bus;
    __u32 gpu_device;
    __u32 gpu_function;
    __u64 handle; /* output, scoped to this open file description */
    __u64 registration_ns;
    __u64 mapping_generation;
    __u64 reserved[4];
};
struct qdma_persistent_transfer {
    __u32 size;
    __u32 version;
    __u64 handle;
    __u64 device_address;
    __u64 length; /* offset is zero in ABI v1 */
    __u64 bytes_transferred;
    __u64 transfer_duration_ns; /* enqueue through hardware callback */
    __u64 mapping_generation;
    __u64 reserved[4];
};
struct qdma_persistent_unregister {
    __u32 size;
    __u32 version;
    __u64 handle;
    __u64 reserved[4];
};
struct qdma_persistent_stats {
    __u32 size;
    __u32 version;
    __u64 handle; /* zero: this file's aggregate; otherwise live handle */
    __u64 registrations;
    __u64 unregistrations;
    __u64 live_registrations;
    __u64 mappings;
    __u64 invalidations;
    __u64 remaps;
    __u64 active_requests;
    __u64 completed_requests;
    __u64 quarantined_requests;
    __u64 mapping_generation; /* per-handle only; zero for aggregate */
    __u64 reserved[4];
};
#define QDMA_CDEV_IOCTL_CAPS _IOWR('J', 4, struct qdma_persistent_caps)
#define QDMA_CDEV_IOCTL_REGISTER _IOWR('J', 5, struct qdma_persistent_register)
#define QDMA_CDEV_IOCTL_TRANSFER_REGISTERED _IOWR('J', 6, struct qdma_persistent_transfer)
#define QDMA_CDEV_IOCTL_UNREGISTER _IOWR('J', 7, struct qdma_persistent_unregister)
#define QDMA_CDEV_IOCTL_STATS _IOWR('J', 8, struct qdma_persistent_stats)
#endif
