/*
 * CXL P2P DMA - Peer-to-Peer DMA between CXL devices
 * Enables direct memory transfers between Type 2 (GPU) and Type 3 (memory) devices
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CXL_P2P_DMA_H
#define CXL_P2P_DMA_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/pci/pci.h"

/* Maximum number of peer devices that can be registered */
#define CXL_P2P_MAX_PEERS       16

/* P2P DMA transfer flags */
#define CXL_P2P_FLAG_SYNC       (1 << 0)  /* Synchronous transfer */
#define CXL_P2P_FLAG_COHERENT   (1 << 1)  /* Maintain coherency */
#define CXL_P2P_FLAG_NOTIFY     (1 << 2)  /* Notify on completion */
#define CXL_P2P_FLAG_CHAIN      (1 << 3)  /* Part of chained transfer */

/* P2P DMA transfer direction */
typedef enum {
    CXL_P2P_DIR_T2_TO_T3 = 0,   /* Type 2 -> Type 3 (GPU to memory) */
    CXL_P2P_DIR_T3_TO_T2 = 1,   /* Type 3 -> Type 2 (memory to GPU) */
    CXL_P2P_DIR_T3_TO_T3 = 2,   /* Type 3 -> Type 3 (memory to memory) */
} CXLP2PDirection;

/* P2P DMA transfer status */
typedef enum {
    CXL_P2P_STATUS_IDLE = 0,
    CXL_P2P_STATUS_PENDING = 1,
    CXL_P2P_STATUS_IN_PROGRESS = 2,
    CXL_P2P_STATUS_COMPLETE = 3,
    CXL_P2P_STATUS_ERROR = 4,
    CXL_P2P_STATUS_ABORTED = 5,
} CXLP2PStatus;

/* P2P peer device type */
typedef enum {
    CXL_P2P_PEER_TYPE2 = 2,     /* Type 2 accelerator */
    CXL_P2P_PEER_TYPE3 = 3,     /* Type 3 memory expander */
} CXLP2PPeerType;

/* P2P peer device info */
typedef struct CXLP2PPeer {
    PCIDevice *pci_dev;         /* PCI device pointer */
    CXLP2PPeerType type;        /* Device type */
    uint64_t mem_base;          /* Memory region base (DPA) */
    uint64_t mem_size;          /* Memory region size */
    void *mem_ptr;              /* Direct memory pointer (if available) */
    bool active;                /* Peer is active */
    bool coherent;              /* Supports cache coherency */
    uint32_t peer_id;           /* Unique peer ID */
    char name[64];              /* Device name */
} CXLP2PPeer;

/* P2P DMA descriptor for a single transfer */
typedef struct CXLP2PDescriptor {
    uint64_t src_addr;          /* Source address (DPA or device offset) */
    uint64_t dst_addr;          /* Destination address */
    uint64_t size;              /* Transfer size in bytes */
    uint32_t src_peer_id;       /* Source peer ID */
    uint32_t dst_peer_id;       /* Destination peer ID */
    uint32_t flags;             /* Transfer flags */
    CXLP2PDirection direction;  /* Transfer direction */
    CXLP2PStatus status;        /* Transfer status */
    uint64_t start_time;        /* Transfer start timestamp */
    uint64_t end_time;          /* Transfer completion timestamp */
    uint32_t error_code;        /* Error code if status is ERROR */
} CXLP2PDescriptor;

/* P2P DMA engine state */
typedef struct CXLP2PDMAEngine {
    QemuMutex lock;

    /* Registered peer devices */
    CXLP2PPeer peers[CXL_P2P_MAX_PEERS];
    uint32_t num_peers;

    /* DMA queue */
    GQueue *pending_transfers;
    GQueue *completed_transfers;

    /* Engine state */
    bool enabled;
    bool busy;
    CXLP2PDescriptor *current_transfer;

    /* Self reference (Type 2 device) */
    void *owner;                /* CXLType2State pointer */
    uint32_t owner_peer_id;     /* Own peer ID */

    /* Statistics */
    struct {
        uint64_t transfers_completed;
        uint64_t transfers_failed;
        uint64_t bytes_transferred;
        uint64_t t2_to_t3_transfers;
        uint64_t t3_to_t2_transfers;
        uint64_t t3_to_t3_transfers;
        uint64_t coherency_ops;
        uint64_t total_latency_ns;
    } stats;

    /* Configuration */
    uint32_t max_transfer_size;     /* Maximum single transfer size */
    uint32_t queue_depth;           /* Maximum pending transfers */
    bool auto_coherency;            /* Auto-manage coherency */

} CXLP2PDMAEngine;

/* Forward declarations */
struct CXLType2State;
struct CXLType3Dev;

/* Initialization and cleanup */
void cxl_p2p_dma_init(CXLP2PDMAEngine *engine, void *owner);
void cxl_p2p_dma_cleanup(CXLP2PDMAEngine *engine);

/* Peer management */
int cxl_p2p_register_peer_type3(CXLP2PDMAEngine *engine,
                                 struct CXLType3Dev *ct3d,
                                 uint32_t *peer_id);
int cxl_p2p_register_peer_type2(CXLP2PDMAEngine *engine,
                                 struct CXLType2State *ct2d,
                                 uint32_t *peer_id);
void cxl_p2p_unregister_peer(CXLP2PDMAEngine *engine, uint32_t peer_id);
CXLP2PPeer *cxl_p2p_get_peer(CXLP2PDMAEngine *engine, uint32_t peer_id);
int cxl_p2p_discover_peers(CXLP2PDMAEngine *engine);

/* DMA transfer operations */
int cxl_p2p_dma_submit(CXLP2PDMAEngine *engine,
                        CXLP2PDescriptor *desc);
int cxl_p2p_dma_submit_and_wait(CXLP2PDMAEngine *engine,
                                 CXLP2PDescriptor *desc);
CXLP2PStatus cxl_p2p_dma_poll(CXLP2PDMAEngine *engine,
                               CXLP2PDescriptor *desc);
void cxl_p2p_dma_abort(CXLP2PDMAEngine *engine,
                        CXLP2PDescriptor *desc);

/* Convenience functions for common transfers */

/* GPU (Type 2) -> Memory (Type 3) */
int cxl_p2p_gpu_to_mem(CXLP2PDMAEngine *engine,
                        uint32_t t3_peer_id,
                        uint64_t gpu_offset,
                        uint64_t mem_offset,
                        uint64_t size,
                        uint32_t flags);

/* Memory (Type 3) -> GPU (Type 2) */
int cxl_p2p_mem_to_gpu(CXLP2PDMAEngine *engine,
                        uint32_t t3_peer_id,
                        uint64_t mem_offset,
                        uint64_t gpu_offset,
                        uint64_t size,
                        uint32_t flags);

/* Memory (Type 3) -> Memory (Type 3) */
int cxl_p2p_mem_to_mem(CXLP2PDMAEngine *engine,
                        uint32_t src_peer_id,
                        uint32_t dst_peer_id,
                        uint64_t src_offset,
                        uint64_t dst_offset,
                        uint64_t size,
                        uint32_t flags);

/* Coherency helpers */
void cxl_p2p_ensure_coherency(CXLP2PDMAEngine *engine,
                               uint32_t peer_id,
                               uint64_t addr,
                               uint64_t size,
                               bool is_write);
void cxl_p2p_invalidate_peer_cache(CXLP2PDMAEngine *engine,
                                    uint32_t peer_id,
                                    uint64_t addr,
                                    uint64_t size);

/* Statistics */
void cxl_p2p_dump_stats(CXLP2PDMAEngine *engine);

/* GPU command interface extensions for P2P */

/* P2P GPU commands (extension to CXLGPUCommand enum) */
#define CXL_GPU_CMD_P2P_DISCOVER        0x90  /* Discover P2P peers */
#define CXL_GPU_CMD_P2P_GET_PEER_INFO   0x91  /* Get peer device info */
#define CXL_GPU_CMD_P2P_GPU_TO_MEM      0x92  /* GPU -> Type3 transfer */
#define CXL_GPU_CMD_P2P_MEM_TO_GPU      0x93  /* Type3 -> GPU transfer */
#define CXL_GPU_CMD_P2P_MEM_TO_MEM      0x94  /* Type3 -> Type3 transfer */
#define CXL_GPU_CMD_P2P_SYNC            0x95  /* Wait for P2P completion */
#define CXL_GPU_CMD_P2P_GET_STATUS      0x96  /* Get transfer status */

/* P2P register offsets (in GPU command region) */
#define CXL_GPU_REG_P2P_NUM_PEERS       0x0200  /* Number of discovered peers */
#define CXL_GPU_REG_P2P_PEER_ID         0x0204  /* Current peer ID for queries */
#define CXL_GPU_REG_P2P_PEER_TYPE       0x0208  /* Peer device type */
#define CXL_GPU_REG_P2P_PEER_MEM_SIZE   0x0210  /* Peer memory size */
#define CXL_GPU_REG_P2P_STATUS          0x0218  /* P2P engine status */
#define CXL_GPU_REG_P2P_XFER_COUNT      0x0220  /* Transfer counter */

#endif /* CXL_P2P_DMA_H */
