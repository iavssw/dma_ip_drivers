#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif


#ifndef __QDMA_IOCTL_H__
#define __QDMA_IOCTL_H__
#include "qdma_persistent_uapi.h"

#define QDMA_IOCTL_MAGIC 'J'
#define QDMA_CDEV_IOCTL_P2P_DMA_NUM _IOWR(QDMA_IOCTL_MAGIC, 1, struct qdma_p2p_req)
#define QDMA_P2PDMA_IOCTL_DISABLE_NUM _IO(QDMA_IOCTL_MAGIC, 2)
#define QDMA_P2PDMA_IOCTL_ENABLE_NUM _IO(QDMA_IOCTL_MAGIC, 3)

/**
 * Metadata for QDMA P2P Request
 */
enum qdma_p2p_direction {
    // Read from QDMA/FPGA to the specified dma-buf
    QDMA_P2P_READ = 0,
    // Write to QDMA/FPGA from the specified dma-buf
    QDMA_P2P_WRITE = 1
};
struct qdma_p2p_req {
    unsigned long long dev_addr;
    int dmabuf_fd; // file descriptor of the exported dma-buf
    unsigned int num_bytes;
    enum qdma_p2p_direction direction;
    struct {
        unsigned int bytes_transferred;
        unsigned long long transfer_duration_ns;
    } response;
};

enum qdma_cdev_ioctl_cmd {
	QDMA_CDEV_IOCTL_NO_MEMCPY = 0,
	QDMA_CDEV_IOCTL_CMDS,
	QDMA_CDEV_IOCTL_P2P_DMA = QDMA_CDEV_IOCTL_P2P_DMA_NUM
};

enum qdma_p2pdma_ioctl_cmd {
	QDMA_P2PDMA_IOCTL_DISABLE = QDMA_P2PDMA_IOCTL_DISABLE_NUM,
	QDMA_P2PDMA_IOCTL_ENABLE = QDMA_P2PDMA_IOCTL_ENABLE_NUM
};


#endif
