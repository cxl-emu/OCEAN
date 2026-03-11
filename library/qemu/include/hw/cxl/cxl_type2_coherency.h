/*
 * CXL Type 2 Device - BAR CPU-GPU Coherency Protocol
 * Enhanced coherency tracking for CPU-GPU communication through BAR regions
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CXL_TYPE2_COHERENCY_H
#define CXL_TYPE2_COHERENCY_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include <glib.h>

/* Cache line size for coherency tracking */
#define CXL_CACHE_LINE_SIZE     64
#define CXL_CACHE_LINE_MASK     (~(CXL_CACHE_LINE_SIZE - 1))

/* Maximum tracked regions */
#define CXL_MAX_TRACKED_REGIONS 16

/* Coherency domains */
typedef enum {
    CXL_DOMAIN_CPU = 0,       /* CPU domain - host processor */
    CXL_DOMAIN_GPU = 1,       /* GPU domain - accelerator */
    CXL_DOMAIN_CXL_HOST = 2,  /* CXL host bridge */
    CXL_DOMAIN_CXL_DEV = 3,   /* CXL device */
} CXLCoherencyDomain;

/* Coherency request types (CXL.cache protocol) */
typedef enum {
    CXL_COH_REQ_RD_SHARED = 0,      /* Read with intent to share */
    CXL_COH_REQ_RD_OWN = 1,         /* Read with intent to own/modify */
    CXL_COH_REQ_RD_ANY = 2,         /* Read with any coherency state */
    CXL_COH_REQ_WR_INV = 3,         /* Write invalidate */
    CXL_COH_REQ_WR_CUR = 4,         /* Write current (no invalidate) */
    CXL_COH_REQ_CLR_DEV_CACHE = 5,  /* Clear device cache */
    CXL_COH_REQ_CLEAN_EVICT = 6,    /* Clean evict */
    CXL_COH_REQ_DIRTY_EVICT = 7,    /* Dirty evict (writeback) */
} CXLCoherencyReqType;

/* Coherency response types */
typedef enum {
    CXL_COH_RSP_I = 0,       /* Invalid - no data */
    CXL_COH_RSP_V = 1,       /* Valid - has data */
    CXL_COH_RSP_S = 2,       /* Shared - data shared with others */
    CXL_COH_RSP_E = 3,       /* Exclusive - data exclusive to requester */
    CXL_COH_RSP_M = 4,       /* Modified - data modified by requester */
    CXL_COH_RSP_O = 5,       /* Owned - owned but others have shared copies */
    CXL_COH_RSP_F = 6,       /* Forward - forwarding data */
} CXLCoherencyRspType;

/* Bias mode constants */
#define CXL_BIAS_MODE_HOST      0   /* Host-biased: CPU is coherence home */
#define CXL_BIAS_MODE_DEVICE    1   /* Device-biased: GPU snoop filter is home */

/* Snoop filter entry */
typedef struct CXLSnoopEntry {
    uint64_t addr;              /* Cache line address */
    uint8_t state;              /* Coherency state (CXLCoherencyState) */
    uint8_t domain_mask;        /* Bitmask of domains holding this line */
    uint8_t owner_domain;       /* Domain that owns exclusive/modified copy */
    uint8_t bias_mode;          /* CXL_BIAS_MODE_HOST or CXL_BIAS_MODE_DEVICE */
    uint8_t flags;
    #define CXL_SNOOP_FLAG_DIRTY    (1 << 0)
    #define CXL_SNOOP_FLAG_PENDING  (1 << 1)
    #define CXL_SNOOP_FLAG_GPU_ACC  (1 << 2)
    uint64_t timestamp;         /* Last access timestamp */
    uint32_t access_count;      /* Access counter */
} CXLSnoopEntry;

/* BAR region coherency state */
typedef struct CXLBARCoherencyRegion {
    uint64_t base_addr;         /* BAR region base address */
    uint64_t size;              /* Region size */
    uint32_t bar_index;         /* BAR index (0, 2, 4) */
    bool active;                /* Region is active */
    bool gpu_accessible;        /* GPU can access this region */
    bool cpu_accessible;        /* CPU can access this region */
    bool coherent;              /* Region maintains coherency */

    uint8_t default_bias;       /* Default bias mode for this region */

    /* Per-region statistics */
    uint64_t cpu_reads;
    uint64_t cpu_writes;
    uint64_t gpu_reads;
    uint64_t gpu_writes;
    uint64_t coherency_conflicts;
} CXLBARCoherencyRegion;

/* Pending coherency transaction */
typedef struct CXLCoherencyTransaction {
    uint64_t addr;
    uint64_t size;
    CXLCoherencyReqType req_type;
    CXLCoherencyDomain source;
    CXLCoherencyDomain target;
    bool completed;
    CXLCoherencyRspType response;
    uint64_t start_time;
    uint64_t end_time;
} CXLCoherencyTransaction;

/* Back-invalidation queue entry */
typedef struct CXLBackInvalidateEntry {
    uint64_t addr;
    uint64_t size;
    CXLCoherencyDomain source;  /* Who initiated the back-invalidation */
    bool urgent;                /* High-priority invalidation */
    uint64_t timestamp;
} CXLBackInvalidateEntry;

/* Main BAR coherency state structure */
typedef struct CXLBARCoherencyState {
    QemuMutex lock;

    /* Snoop filter for tracking cache line ownership */
    GHashTable *snoop_filter;    /* Maps address -> CXLSnoopEntry */
    uint32_t snoop_filter_size;
    uint32_t snoop_filter_capacity;

    /* BAR region tracking */
    CXLBARCoherencyRegion regions[CXL_MAX_TRACKED_REGIONS];
    uint32_t num_regions;

    /* Pending transactions */
    GQueue *pending_transactions;

    /* Back-invalidation queue */
    GQueue *back_invalidate_queue;
    QemuMutex bi_queue_lock;
    QemuCond bi_queue_cond;

    /* Coherency configuration */
    bool enabled;
    bool strict_ordering;        /* Enforce strict memory ordering */
    bool speculative_reads;      /* Allow speculative reads */
    uint32_t snoop_timeout_ns;   /* Snoop response timeout */

    /* Statistics */
    struct {
        uint64_t snoop_hits;
        uint64_t snoop_misses;
        uint64_t coherency_requests;
        uint64_t back_invalidations;
        uint64_t writebacks;
        uint64_t evictions;
        uint64_t conflicts;
        uint64_t upgrades;        /* Shared -> Exclusive/Modified */
        uint64_t downgrades;      /* Exclusive/Modified -> Shared/Invalid */
        uint64_t cpu_to_gpu_xfers;
        uint64_t gpu_to_cpu_xfers;
        uint64_t bias_flips;
        uint64_t device_bias_hits;
        uint64_t host_bias_hits;
    } stats;

} CXLBARCoherencyState;

/* Forward declarations */
struct CXLType2State;

/* Initialization/cleanup */
void cxl_bar_coherency_init(CXLBARCoherencyState *state);
void cxl_bar_coherency_cleanup(CXLBARCoherencyState *state);

/* BAR region management */
int cxl_bar_coherency_add_region(CXLBARCoherencyState *state,
                                  uint32_t bar_index,
                                  uint64_t base_addr,
                                  uint64_t size,
                                  bool gpu_accessible,
                                  bool cpu_accessible);
void cxl_bar_coherency_remove_region(CXLBARCoherencyState *state,
                                      uint32_t bar_index);

/* Snoop filter operations */
CXLSnoopEntry *cxl_bar_snoop_lookup(CXLBARCoherencyState *state, uint64_t addr);
void cxl_bar_snoop_insert(CXLBARCoherencyState *state, uint64_t addr,
                          uint8_t state_val, CXLCoherencyDomain domain);
void cxl_bar_snoop_update(CXLBARCoherencyState *state, uint64_t addr,
                          uint8_t new_state, CXLCoherencyDomain domain);
void cxl_bar_snoop_remove(CXLBARCoherencyState *state, uint64_t addr);

/* Coherency request handling */
CXLCoherencyRspType cxl_bar_coherency_request(CXLBARCoherencyState *state,
                                               CXLCoherencyReqType req,
                                               uint64_t addr,
                                               uint64_t size,
                                               CXLCoherencyDomain source,
                                               uint8_t *data);

/* CPU access handlers with coherency */
uint64_t cxl_bar_coherent_read(struct CXLType2State *ct2d,
                                uint64_t bar_offset,
                                unsigned size,
                                uint32_t bar_index);
void cxl_bar_coherent_write(struct CXLType2State *ct2d,
                             uint64_t bar_offset,
                             uint64_t value,
                             unsigned size,
                             uint32_t bar_index);

/* GPU access notification (for coherency tracking) */
void cxl_bar_notify_gpu_access(CXLBARCoherencyState *state,
                                uint64_t addr,
                                uint64_t size,
                                bool is_write);

/* Back-invalidation */
void cxl_bar_back_invalidate(CXLBARCoherencyState *state,
                              uint64_t addr,
                              uint64_t size,
                              CXLCoherencyDomain source);
void cxl_bar_process_back_invalidations(struct CXLType2State *ct2d);

/* Memory barriers/fences */
void cxl_bar_memory_fence(CXLBARCoherencyState *state, CXLCoherencyDomain domain);
void cxl_bar_cache_flush(CXLBARCoherencyState *state,
                          uint64_t addr,
                          uint64_t size);
void cxl_bar_cache_writeback(CXLBARCoherencyState *state,
                              uint64_t addr,
                              uint64_t size);

/* Statistics */
void cxl_bar_coherency_dump_stats(CXLBARCoherencyState *state);

/* Bias mode control */
void cxl_bar_set_bias(CXLBARCoherencyState *state,
                      uint64_t addr, uint64_t size, uint8_t bias_mode);
uint8_t cxl_bar_get_bias(CXLBARCoherencyState *state, uint64_t addr);
void cxl_bar_bias_flip(struct CXLType2State *ct2d,
                       uint64_t addr, uint64_t size, uint8_t new_bias);

/* Atomic operation support */
typedef enum {
    CXL_ATOMIC_ADD = 0,
    CXL_ATOMIC_SUB = 1,
    CXL_ATOMIC_AND = 2,
    CXL_ATOMIC_OR = 3,
    CXL_ATOMIC_XOR = 4,
    CXL_ATOMIC_MIN = 5,
    CXL_ATOMIC_MAX = 6,
    CXL_ATOMIC_CAS = 7,     /* Compare-and-swap */
    CXL_ATOMIC_SWAP = 8,
} CXLAtomicOp;

uint64_t cxl_bar_atomic_op(struct CXLType2State *ct2d,
                            uint64_t addr,
                            uint64_t operand1,
                            uint64_t operand2,
                            CXLAtomicOp op,
                            unsigned size);

#endif /* CXL_TYPE2_COHERENCY_H */
