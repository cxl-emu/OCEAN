/*
 * CXL Type 2 Device - BAR CPU-GPU Coherency Protocol Implementation
 * Enhanced coherency tracking for CPU-GPU communication through BAR regions
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/cxl/cxl_type2.h"
#include "hw/cxl/cxl_type2_coherency.h"
#include "system/memory.h"

/* Hash function for snoop filter (uses cache line aligned addresses) */
static guint snoop_addr_hash(gconstpointer key)
{
    uint64_t addr = *(const uint64_t *)key;
    /* Align to cache line and hash */
    addr = addr & CXL_CACHE_LINE_MASK;
    return g_int64_hash(&addr);
}

static gboolean snoop_addr_equal(gconstpointer a, gconstpointer b)
{
    uint64_t addr_a = *(const uint64_t *)a & CXL_CACHE_LINE_MASK;
    uint64_t addr_b = *(const uint64_t *)b & CXL_CACHE_LINE_MASK;
    return addr_a == addr_b;
}

/* ========================================================================
 * Initialization and Cleanup
 * ======================================================================== */

void cxl_bar_coherency_init(CXLBARCoherencyState *state)
{
    qemu_mutex_init(&state->lock);
    qemu_mutex_init(&state->bi_queue_lock);
    qemu_cond_init(&state->bi_queue_cond);

    /* Initialize snoop filter with default capacity */
    state->snoop_filter = g_hash_table_new_full(
        snoop_addr_hash, snoop_addr_equal, g_free, g_free);
    state->snoop_filter_size = 0;
    state->snoop_filter_capacity = 4096; /* Default 4K entries */

    /* Initialize regions */
    memset(state->regions, 0, sizeof(state->regions));
    state->num_regions = 0;

    /* Initialize queues */
    state->pending_transactions = g_queue_new();
    state->back_invalidate_queue = g_queue_new();

    /* Default configuration */
    state->enabled = true;
    state->strict_ordering = true;
    state->speculative_reads = false;
    state->snoop_timeout_ns = 1000000; /* 1ms default timeout */

    /* Clear statistics */
    memset(&state->stats, 0, sizeof(state->stats));

    qemu_log("CXL BAR Coherency: Initialized (capacity=%u)\n",
             state->snoop_filter_capacity);
}

void cxl_bar_coherency_cleanup(CXLBARCoherencyState *state)
{
    /* Dump final statistics */
    cxl_bar_coherency_dump_stats(state);

    /* Clean up snoop filter */
    if (state->snoop_filter) {
        g_hash_table_destroy(state->snoop_filter);
        state->snoop_filter = NULL;
    }

    /* Clean up queues */
    if (state->pending_transactions) {
        g_queue_free_full(state->pending_transactions, g_free);
        state->pending_transactions = NULL;
    }
    if (state->back_invalidate_queue) {
        g_queue_free_full(state->back_invalidate_queue, g_free);
        state->back_invalidate_queue = NULL;
    }

    qemu_cond_destroy(&state->bi_queue_cond);
    qemu_mutex_destroy(&state->bi_queue_lock);
    qemu_mutex_destroy(&state->lock);

    qemu_log("CXL BAR Coherency: Cleaned up\n");
}

/* ========================================================================
 * BAR Region Management
 * ======================================================================== */

int cxl_bar_coherency_add_region(CXLBARCoherencyState *state,
                                  uint32_t bar_index,
                                  uint64_t base_addr,
                                  uint64_t size,
                                  bool gpu_accessible,
                                  bool cpu_accessible)
{
    qemu_mutex_lock(&state->lock);

    if (state->num_regions >= CXL_MAX_TRACKED_REGIONS) {
        qemu_mutex_unlock(&state->lock);
        qemu_log("CXL BAR Coherency: Max regions exceeded\n");
        return -1;
    }

    /* Find existing region with same BAR index or free slot */
    int slot = -1;
    for (uint32_t i = 0; i < CXL_MAX_TRACKED_REGIONS; i++) {
        if (state->regions[i].active && state->regions[i].bar_index == bar_index) {
            slot = i;
            break;
        }
        if (!state->regions[i].active && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        qemu_mutex_unlock(&state->lock);
        return -1;
    }

    CXLBARCoherencyRegion *region = &state->regions[slot];
    region->base_addr = base_addr;
    region->size = size;
    region->bar_index = bar_index;
    region->active = true;
    region->gpu_accessible = gpu_accessible;
    region->cpu_accessible = cpu_accessible;
    region->coherent = gpu_accessible && cpu_accessible; /* Coherent if both can access */

    /* Clear statistics */
    region->cpu_reads = 0;
    region->cpu_writes = 0;
    region->gpu_reads = 0;
    region->gpu_writes = 0;
    region->coherency_conflicts = 0;

    if (!state->regions[slot].active) {
        state->num_regions++;
    }

    qemu_mutex_unlock(&state->lock);

    qemu_log("CXL BAR Coherency: Added BAR%u region [0x%lx - 0x%lx] "
             "GPU:%d CPU:%d Coherent:%d\n",
             bar_index, base_addr, base_addr + size - 1,
             gpu_accessible, cpu_accessible, region->coherent);

    return 0;
}

void cxl_bar_coherency_remove_region(CXLBARCoherencyState *state,
                                      uint32_t bar_index)
{
    qemu_mutex_lock(&state->lock);

    for (uint32_t i = 0; i < CXL_MAX_TRACKED_REGIONS; i++) {
        if (state->regions[i].active && state->regions[i].bar_index == bar_index) {
            state->regions[i].active = false;
            state->num_regions--;
            qemu_log("CXL BAR Coherency: Removed BAR%u region\n", bar_index);
            break;
        }
    }

    qemu_mutex_unlock(&state->lock);
}

/* Find region containing address */
static CXLBARCoherencyRegion *find_region_for_addr(CXLBARCoherencyState *state,
                                                    uint64_t addr)
{
    for (uint32_t i = 0; i < CXL_MAX_TRACKED_REGIONS; i++) {
        CXLBARCoherencyRegion *region = &state->regions[i];
        if (region->active &&
            addr >= region->base_addr &&
            addr < region->base_addr + region->size) {
            return region;
        }
    }
    return NULL;
}

/* ========================================================================
 * Snoop Filter Operations
 * ======================================================================== */

CXLSnoopEntry *cxl_bar_snoop_lookup(CXLBARCoherencyState *state, uint64_t addr)
{
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;
    CXLSnoopEntry *entry;

    qemu_mutex_lock(&state->lock);
    entry = g_hash_table_lookup(state->snoop_filter, &aligned_addr);
    if (entry) {
        state->stats.snoop_hits++;
    } else {
        state->stats.snoop_misses++;
    }
    qemu_mutex_unlock(&state->lock);

    return entry;
}

void cxl_bar_snoop_insert(CXLBARCoherencyState *state, uint64_t addr,
                          uint8_t state_val, CXLCoherencyDomain domain)
{
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;

    qemu_mutex_lock(&state->lock);

    /* Check if already exists */
    CXLSnoopEntry *existing = g_hash_table_lookup(state->snoop_filter, &aligned_addr);
    if (existing) {
        /* Update existing entry */
        existing->state = state_val;
        existing->domain_mask |= (1 << domain);
        if (state_val == CXL_COHERENCY_EXCLUSIVE ||
            state_val == CXL_COHERENCY_MODIFIED) {
            existing->owner_domain = domain;
        }
        existing->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        existing->access_count++;
    } else {
        /* Create new entry */
        uint64_t *key = g_new(uint64_t, 1);
        CXLSnoopEntry *entry = g_new0(CXLSnoopEntry, 1);

        *key = aligned_addr;
        entry->addr = aligned_addr;
        entry->state = state_val;
        entry->domain_mask = (1 << domain);
        entry->owner_domain = domain;
        entry->flags = 0;
        entry->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        entry->access_count = 1;

        g_hash_table_insert(state->snoop_filter, key, entry);
        state->snoop_filter_size++;
    }

    qemu_mutex_unlock(&state->lock);

    qemu_log_mask(LOG_TRACE, "CXL BAR Snoop: Insert 0x%lx state=%d domain=%d\n",
                 aligned_addr, state_val, domain);
}

void cxl_bar_snoop_update(CXLBARCoherencyState *state, uint64_t addr,
                          uint8_t new_state, CXLCoherencyDomain domain)
{
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;

    qemu_mutex_lock(&state->lock);

    CXLSnoopEntry *entry = g_hash_table_lookup(state->snoop_filter, &aligned_addr);
    if (entry) {
        uint8_t old_state = entry->state;
        entry->state = new_state;
        entry->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        entry->access_count++;

        /* Track upgrades and downgrades */
        if (new_state > old_state) {
            state->stats.upgrades++;
        } else if (new_state < old_state) {
            state->stats.downgrades++;
        }

        /* Update ownership for exclusive/modified states */
        if (new_state == CXL_COHERENCY_EXCLUSIVE ||
            new_state == CXL_COHERENCY_MODIFIED) {
            entry->owner_domain = domain;
            /* Clear other domains when going exclusive */
            entry->domain_mask = (1 << domain);
        } else if (new_state == CXL_COHERENCY_SHARED) {
            entry->domain_mask |= (1 << domain);
        } else if (new_state == CXL_COHERENCY_INVALID) {
            entry->domain_mask &= ~(1 << domain);
        }

        qemu_log_mask(LOG_TRACE,
                     "CXL BAR Snoop: Update 0x%lx state=%d->%d domain=%d\n",
                     aligned_addr, old_state, new_state, domain);
    }

    qemu_mutex_unlock(&state->lock);
}

void cxl_bar_snoop_remove(CXLBARCoherencyState *state, uint64_t addr)
{
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;

    qemu_mutex_lock(&state->lock);

    if (g_hash_table_remove(state->snoop_filter, &aligned_addr)) {
        state->snoop_filter_size--;
        state->stats.evictions++;
        qemu_log_mask(LOG_TRACE, "CXL BAR Snoop: Remove 0x%lx\n", aligned_addr);
    }

    qemu_mutex_unlock(&state->lock);
}

/* ========================================================================
 * Coherency Request Handling
 * ======================================================================== */

CXLCoherencyRspType cxl_bar_coherency_request(CXLBARCoherencyState *state,
                                               CXLCoherencyReqType req,
                                               uint64_t addr,
                                               uint64_t size,
                                               CXLCoherencyDomain source,
                                               uint8_t *data)
{
    CXLCoherencyRspType response = CXL_COH_RSP_I;
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;

    if (!state->enabled) {
        return CXL_COH_RSP_V; /* No coherency tracking - assume valid */
    }

    qemu_mutex_lock(&state->lock);
    state->stats.coherency_requests++;

    CXLSnoopEntry *entry = g_hash_table_lookup(state->snoop_filter, &aligned_addr);

    switch (req) {
    case CXL_COH_REQ_RD_SHARED:
        if (!entry) {
            /* Cache miss - fetch from memory */
            response = CXL_COH_RSP_S;
            cxl_bar_snoop_insert(state, addr, CXL_COHERENCY_SHARED, source);
        } else if (entry->state == CXL_COHERENCY_MODIFIED) {
            /* Need writeback from owner first */
            state->stats.writebacks++;
            /* Downgrade owner to shared */
            entry->state = CXL_COHERENCY_SHARED;
            entry->domain_mask |= (1 << source);
            entry->flags &= ~CXL_SNOOP_FLAG_DIRTY;
            response = CXL_COH_RSP_S;
        } else {
            /* Can share existing line */
            entry->domain_mask |= (1 << source);
            response = CXL_COH_RSP_S;
        }
        /* Track bias hits */
        if (entry) {
            if (entry->bias_mode == CXL_BIAS_MODE_DEVICE &&
                source == CXL_DOMAIN_GPU) {
                state->stats.device_bias_hits++;
            } else if (entry->bias_mode == CXL_BIAS_MODE_HOST &&
                       source == CXL_DOMAIN_CPU) {
                state->stats.host_bias_hits++;
            }
        }
        break;

    case CXL_COH_REQ_RD_OWN:
        if (!entry) {
            /* Cache miss - allocate exclusive */
            response = CXL_COH_RSP_E;
            cxl_bar_snoop_insert(state, addr, CXL_COHERENCY_EXCLUSIVE, source);
        } else {
            /* Check bias mode for fast path */
            if (entry->bias_mode == CXL_BIAS_MODE_DEVICE &&
                source == CXL_DOMAIN_GPU) {
                /* Device-biased: GPU gets fast exclusive access */
                entry->state = CXL_COHERENCY_EXCLUSIVE;
                entry->domain_mask = (1 << source);
                entry->owner_domain = source;
                state->stats.device_bias_hits++;
                response = CXL_COH_RSP_E;
            } else if (entry->owner_domain != source) {
                /* Need to invalidate other copies */
                state->stats.back_invalidations++;
                if (entry->state == CXL_COHERENCY_MODIFIED) {
                    state->stats.writebacks++;
                }
                entry->state = CXL_COHERENCY_EXCLUSIVE;
                entry->domain_mask = (1 << source);
                entry->owner_domain = source;
                response = CXL_COH_RSP_E;
            } else {
                /* Already owner - upgrade to exclusive if needed */
                entry->state = CXL_COHERENCY_EXCLUSIVE;
                entry->domain_mask = (1 << source);
                response = CXL_COH_RSP_E;
            }
            if (entry->bias_mode == CXL_BIAS_MODE_HOST &&
                source == CXL_DOMAIN_CPU) {
                state->stats.host_bias_hits++;
            }
        }
        break;

    case CXL_COH_REQ_WR_INV:
        /* Write with invalidation of other copies */
        if (entry) {
            if (entry->owner_domain != source &&
                entry->state != CXL_COHERENCY_INVALID) {
                state->stats.back_invalidations++;
                if (entry->state == CXL_COHERENCY_MODIFIED) {
                    state->stats.writebacks++;
                }
            }
            entry->state = CXL_COHERENCY_MODIFIED;
            entry->domain_mask = (1 << source);
            entry->owner_domain = source;
            entry->flags |= CXL_SNOOP_FLAG_DIRTY;
        } else {
            cxl_bar_snoop_insert(state, addr, CXL_COHERENCY_MODIFIED, source);
            entry = g_hash_table_lookup(state->snoop_filter, &aligned_addr);
            if (entry) {
                entry->flags |= CXL_SNOOP_FLAG_DIRTY;
            }
        }
        response = CXL_COH_RSP_M;
        break;

    case CXL_COH_REQ_CLR_DEV_CACHE:
        /* Clear device cache - invalidate all lines owned by device */
        if (entry && (entry->domain_mask & (1 << CXL_DOMAIN_GPU))) {
            if (entry->state == CXL_COHERENCY_MODIFIED &&
                entry->owner_domain == CXL_DOMAIN_GPU) {
                state->stats.writebacks++;
            }
            entry->domain_mask &= ~(1 << CXL_DOMAIN_GPU);
            if (entry->domain_mask == 0) {
                entry->state = CXL_COHERENCY_INVALID;
            } else {
                entry->state = CXL_COHERENCY_SHARED;
            }
        }
        response = CXL_COH_RSP_I;
        break;

    case CXL_COH_REQ_CLEAN_EVICT:
        /* Clean eviction - remove from snoop filter without writeback */
        if (entry) {
            entry->domain_mask &= ~(1 << source);
            if (entry->domain_mask == 0) {
                cxl_bar_snoop_remove(state, addr);
            }
        }
        response = CXL_COH_RSP_I;
        break;

    case CXL_COH_REQ_DIRTY_EVICT:
        /* Dirty eviction - writeback and remove */
        if (entry && entry->owner_domain == source) {
            state->stats.writebacks++;
            entry->domain_mask &= ~(1 << source);
            entry->flags &= ~CXL_SNOOP_FLAG_DIRTY;
            if (entry->domain_mask == 0) {
                cxl_bar_snoop_remove(state, addr);
            } else {
                entry->state = CXL_COHERENCY_SHARED;
            }
        }
        response = CXL_COH_RSP_I;
        break;

    default:
        response = CXL_COH_RSP_I;
        break;
    }

    qemu_mutex_unlock(&state->lock);

    qemu_log_mask(LOG_TRACE,
                 "CXL BAR Coherency: req=%d addr=0x%lx src=%d -> rsp=%d\n",
                 req, addr, source, response);

    return response;
}

/* ========================================================================
 * CPU/GPU Access Handlers
 * ======================================================================== */

uint64_t cxl_bar_coherent_read(CXLType2State *ct2d,
                                uint64_t bar_offset,
                                unsigned size,
                                uint32_t bar_index)
{
    CXLBARCoherencyState *coh_state = &ct2d->bar_coherency;
    uint64_t value = 0;
    MemoryRegion *mr = NULL;
    uint8_t *mem_ptr = NULL;

    /* Find the memory region for this BAR */
    switch (bar_index) {
    case 0:
        /* Component registers - no coherency needed */
        return 0;
    case 2:
        mr = &ct2d->cache_mem;
        mem_ptr = memory_region_get_ram_ptr(mr);
        break;
    case 4:
        mr = &ct2d->device_mem;
        mem_ptr = memory_region_get_ram_ptr(mr);
        break;
    default:
        return 0;
    }

    if (!mem_ptr) {
        return 0;
    }

    /* Track coherency if enabled */
    if (coh_state->enabled) {
        CXLBARCoherencyRegion *region = find_region_for_addr(coh_state, bar_offset);
        if (region) {
            region->cpu_reads++;

            if (region->coherent) {
                /* Perform coherency request */
                cxl_bar_coherency_request(coh_state,
                                          CXL_COH_REQ_RD_SHARED,
                                          bar_offset, size,
                                          CXL_DOMAIN_CPU, NULL);
            }
        }

        /* Update snoop filter */
        cxl_bar_snoop_insert(coh_state, bar_offset, CXL_COHERENCY_SHARED,
                             CXL_DOMAIN_CPU);
    }

    /* Perform the actual read */
    if (bar_offset + size <= memory_region_size(mr)) {
        memcpy(&value, mem_ptr + bar_offset, MIN(size, 8));
    }

    ct2d->stats.read_ops++;
    ct2d->stats.cpu_accesses++;

    return value;
}

void cxl_bar_coherent_write(CXLType2State *ct2d,
                             uint64_t bar_offset,
                             uint64_t value,
                             unsigned size,
                             uint32_t bar_index)
{
    CXLBARCoherencyState *coh_state = &ct2d->bar_coherency;
    MemoryRegion *mr = NULL;
    uint8_t *mem_ptr = NULL;

    /* Find the memory region for this BAR */
    switch (bar_index) {
    case 0:
        /* Component registers - no coherency needed */
        return;
    case 2:
        mr = &ct2d->cache_mem;
        mem_ptr = memory_region_get_ram_ptr(mr);
        break;
    case 4:
        mr = &ct2d->device_mem;
        mem_ptr = memory_region_get_ram_ptr(mr);
        break;
    default:
        return;
    }

    if (!mem_ptr) {
        return;
    }

    /* Track coherency if enabled */
    if (coh_state->enabled) {
        CXLBARCoherencyRegion *region = find_region_for_addr(coh_state, bar_offset);
        if (region) {
            region->cpu_writes++;

            if (region->coherent) {
                /* Perform coherency request - need exclusive access for write */
                cxl_bar_coherency_request(coh_state,
                                          CXL_COH_REQ_WR_INV,
                                          bar_offset, size,
                                          CXL_DOMAIN_CPU, NULL);
            }
        }

        /* Update snoop filter with modified state */
        cxl_bar_snoop_insert(coh_state, bar_offset, CXL_COHERENCY_MODIFIED,
                             CXL_DOMAIN_CPU);

        /* Check if GPU might have a stale copy - queue back-invalidation */
        CXLSnoopEntry *entry = cxl_bar_snoop_lookup(coh_state, bar_offset);
        if (entry && (entry->domain_mask & (1 << CXL_DOMAIN_GPU))) {
            cxl_bar_back_invalidate(coh_state, bar_offset, size, CXL_DOMAIN_CPU);
        }
    }

    /* Perform the actual write */
    if (bar_offset + size <= memory_region_size(mr)) {
        memcpy(mem_ptr + bar_offset, &value, MIN(size, 8));
    }

    ct2d->stats.write_ops++;
    ct2d->stats.cpu_accesses++;
}

void cxl_bar_notify_gpu_access(CXLBARCoherencyState *state,
                                uint64_t addr,
                                uint64_t size,
                                bool is_write)
{
    if (!state->enabled) {
        return;
    }

    CXLBARCoherencyRegion *region = find_region_for_addr(state, addr);
    if (region) {
        if (is_write) {
            region->gpu_writes++;

            /* GPU write - invalidate CPU cache lines */
            cxl_bar_coherency_request(state,
                                      CXL_COH_REQ_WR_INV,
                                      addr, size,
                                      CXL_DOMAIN_GPU, NULL);

            /* Mark as modified by GPU */
            cxl_bar_snoop_update(state, addr, CXL_COHERENCY_MODIFIED,
                                 CXL_DOMAIN_GPU);
        } else {
            region->gpu_reads++;

            /* GPU read - ensure CPU has written back */
            cxl_bar_coherency_request(state,
                                      CXL_COH_REQ_RD_SHARED,
                                      addr, size,
                                      CXL_DOMAIN_GPU, NULL);
        }

        state->stats.gpu_to_cpu_xfers += is_write ? 1 : 0;
        state->stats.cpu_to_gpu_xfers += is_write ? 0 : 1;
    }
}

/* ========================================================================
 * Back-Invalidation
 * ======================================================================== */

void cxl_bar_back_invalidate(CXLBARCoherencyState *state,
                              uint64_t addr,
                              uint64_t size,
                              CXLCoherencyDomain source)
{
    CXLBackInvalidateEntry *bi_entry = g_new0(CXLBackInvalidateEntry, 1);

    bi_entry->addr = addr & CXL_CACHE_LINE_MASK;
    bi_entry->size = (size + CXL_CACHE_LINE_SIZE - 1) & CXL_CACHE_LINE_MASK;
    bi_entry->source = source;
    bi_entry->urgent = false;
    bi_entry->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_mutex_lock(&state->bi_queue_lock);
    g_queue_push_tail(state->back_invalidate_queue, bi_entry);
    qemu_cond_signal(&state->bi_queue_cond);
    qemu_mutex_unlock(&state->bi_queue_lock);

    state->stats.back_invalidations++;

    qemu_log_mask(LOG_TRACE,
                 "CXL BAR BI: Queued addr=0x%lx size=%lu src=%d\n",
                 addr, size, source);
}

void cxl_bar_process_back_invalidations(CXLType2State *ct2d)
{
    CXLBARCoherencyState *state = &ct2d->bar_coherency;
    CXLBackInvalidateEntry *bi_entry;

    qemu_mutex_lock(&state->bi_queue_lock);

    while (!g_queue_is_empty(state->back_invalidate_queue)) {
        bi_entry = g_queue_pop_head(state->back_invalidate_queue);

        qemu_mutex_unlock(&state->bi_queue_lock);

        /* Process the back-invalidation */
        if (bi_entry->source == CXL_DOMAIN_GPU) {
            /* GPU caused invalidation - clear CPU cache entry */
            cxl_type2_cache_invalidate(ct2d, bi_entry->addr);
        } else {
            /* CPU caused invalidation - notify GPU if hetGPU is active */
            if (ct2d->gpu_info.hetgpu_state.initialized) {
                /* The GPU will see updated data on next access */
                cxl_bar_snoop_update(state, bi_entry->addr,
                                     CXL_COHERENCY_INVALID, CXL_DOMAIN_GPU);
            }
        }

        g_free(bi_entry);

        qemu_mutex_lock(&state->bi_queue_lock);
    }

    qemu_mutex_unlock(&state->bi_queue_lock);
}

/* ========================================================================
 * Memory Barriers and Cache Operations
 * ======================================================================== */

void cxl_bar_memory_fence(CXLBARCoherencyState *state, CXLCoherencyDomain domain)
{
    if (!state->enabled) {
        return;
    }

    /* Memory fence - ensure all pending operations complete */
    qemu_mutex_lock(&state->lock);

    /* Process any pending transactions */
    while (!g_queue_is_empty(state->pending_transactions)) {
        CXLCoherencyTransaction *txn = g_queue_pop_head(state->pending_transactions);
        txn->completed = true;
        txn->end_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g_free(txn);
    }

    qemu_mutex_unlock(&state->lock);

    __sync_synchronize(); /* Full memory barrier */

    qemu_log_mask(LOG_TRACE, "CXL BAR: Memory fence domain=%d\n", domain);
}

void cxl_bar_cache_flush(CXLBARCoherencyState *state,
                          uint64_t addr,
                          uint64_t size)
{
    if (!state->enabled) {
        return;
    }

    uint64_t aligned_start = addr & CXL_CACHE_LINE_MASK;
    uint64_t aligned_end = (addr + size + CXL_CACHE_LINE_SIZE - 1) & CXL_CACHE_LINE_MASK;

    qemu_mutex_lock(&state->lock);

    for (uint64_t a = aligned_start; a < aligned_end; a += CXL_CACHE_LINE_SIZE) {
        CXLSnoopEntry *entry = g_hash_table_lookup(state->snoop_filter, &a);
        if (entry && (entry->flags & CXL_SNOOP_FLAG_DIRTY)) {
            state->stats.writebacks++;
            entry->flags &= ~CXL_SNOOP_FLAG_DIRTY;
            entry->state = CXL_COHERENCY_SHARED;
        }
    }

    qemu_mutex_unlock(&state->lock);

    qemu_log_mask(LOG_TRACE, "CXL BAR: Cache flush 0x%lx - 0x%lx\n",
                 aligned_start, aligned_end);
}

void cxl_bar_cache_writeback(CXLBARCoherencyState *state,
                              uint64_t addr,
                              uint64_t size)
{
    /* Writeback is same as flush but marks lines as clean without invalidating */
    cxl_bar_cache_flush(state, addr, size);
}

/* ========================================================================
 * Bias Mode Control
 * ======================================================================== */

void cxl_bar_set_bias(CXLBARCoherencyState *state,
                      uint64_t addr, uint64_t size, uint8_t bias_mode)
{
    uint64_t aligned_start = addr & CXL_CACHE_LINE_MASK;
    uint64_t aligned_end = (addr + size + CXL_CACHE_LINE_SIZE - 1) &
                           CXL_CACHE_LINE_MASK;

    qemu_mutex_lock(&state->lock);

    for (uint64_t a = aligned_start; a < aligned_end; a += CXL_CACHE_LINE_SIZE) {
        CXLSnoopEntry *entry = g_hash_table_lookup(state->snoop_filter, &a);
        if (entry) {
            entry->bias_mode = bias_mode;
        } else {
            /* Create entry with the bias setting */
            uint64_t *key = g_new(uint64_t, 1);
            CXLSnoopEntry *new_entry = g_new0(CXLSnoopEntry, 1);
            *key = a;
            new_entry->addr = a;
            new_entry->state = CXL_COHERENCY_INVALID;
            new_entry->bias_mode = bias_mode;
            new_entry->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            g_hash_table_insert(state->snoop_filter, key, new_entry);
            state->snoop_filter_size++;
        }
    }

    qemu_mutex_unlock(&state->lock);

    qemu_log_mask(LOG_TRACE,
                 "CXL BAR Bias: Set 0x%lx-0x%lx to %s\n",
                 (unsigned long)aligned_start, (unsigned long)aligned_end,
                 bias_mode == CXL_BIAS_MODE_DEVICE ? "device" : "host");
}

uint8_t cxl_bar_get_bias(CXLBARCoherencyState *state, uint64_t addr)
{
    uint64_t aligned_addr = addr & CXL_CACHE_LINE_MASK;
    uint8_t bias = CXL_BIAS_MODE_HOST; /* Default to host-biased */

    qemu_mutex_lock(&state->lock);

    CXLSnoopEntry *entry = g_hash_table_lookup(state->snoop_filter,
                                                &aligned_addr);
    if (entry) {
        bias = entry->bias_mode;
    }

    qemu_mutex_unlock(&state->lock);
    return bias;
}

void cxl_bar_bias_flip(CXLType2State *ct2d,
                       uint64_t addr, uint64_t size, uint8_t new_bias)
{
    CXLBARCoherencyState *state = &ct2d->bar_coherency;
    uint64_t aligned_start = addr & CXL_CACHE_LINE_MASK;
    uint64_t aligned_end = (addr + size + CXL_CACHE_LINE_SIZE - 1) &
                           CXL_CACHE_LINE_MASK;

    /* Step 1: Flush old home domain caches */
    if (new_bias == CXL_BIAS_MODE_DEVICE) {
        /* Switching to device-bias: flush CPU caches for this range */
        cxl_bar_cache_flush(state, addr, size);
        /* Invalidate CPU entries so device becomes home */
        for (uint64_t a = aligned_start; a < aligned_end;
             a += CXL_CACHE_LINE_SIZE) {
            cxl_bar_coherency_request(state, CXL_COH_REQ_CLR_DEV_CACHE,
                                      a, CXL_CACHE_LINE_SIZE,
                                      CXL_DOMAIN_CPU, NULL);
        }
    } else {
        /* Switching to host-bias: flush GPU caches for this range */
        for (uint64_t a = aligned_start; a < aligned_end;
             a += CXL_CACHE_LINE_SIZE) {
            cxl_bar_coherency_request(state, CXL_COH_REQ_CLR_DEV_CACHE,
                                      a, CXL_CACHE_LINE_SIZE,
                                      CXL_DOMAIN_GPU, NULL);
        }
    }

    /* Step 2: Set the new bias mode */
    cxl_bar_set_bias(state, addr, size, new_bias);

    /* Step 3: Update statistics */
    qemu_mutex_lock(&state->lock);
    state->stats.bias_flips++;
    qemu_mutex_unlock(&state->lock);

    /* Step 4: Memory fence to ensure ordering */
    __sync_synchronize();

    qemu_log("CXL BAR Bias: Flip 0x%lx-0x%lx to %s (total flips: %lu)\n",
             (unsigned long)aligned_start, (unsigned long)aligned_end,
             new_bias == CXL_BIAS_MODE_DEVICE ? "device" : "host",
             (unsigned long)state->stats.bias_flips);
}

/* ========================================================================
 * Atomic Operations
 * ======================================================================== */

uint64_t cxl_bar_atomic_op(CXLType2State *ct2d,
                            uint64_t addr,
                            uint64_t operand1,
                            uint64_t operand2,
                            CXLAtomicOp op,
                            unsigned size)
{
    CXLBARCoherencyState *state = &ct2d->bar_coherency;
    uint64_t old_value = 0;
    uint64_t new_value = 0;
    uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);

    if (!mem_ptr || addr >= ct2d->device_mem_size) {
        return 0;
    }

    /* Atomic operations require exclusive access */
    if (state->enabled) {
        cxl_bar_coherency_request(state,
                                  CXL_COH_REQ_RD_OWN,
                                  addr, size,
                                  CXL_DOMAIN_CPU, NULL);
    }

    qemu_mutex_lock(&state->lock);

    /* Read current value */
    memcpy(&old_value, mem_ptr + addr, MIN(size, 8));

    /* Perform atomic operation */
    switch (op) {
    case CXL_ATOMIC_ADD:
        new_value = old_value + operand1;
        break;
    case CXL_ATOMIC_SUB:
        new_value = old_value - operand1;
        break;
    case CXL_ATOMIC_AND:
        new_value = old_value & operand1;
        break;
    case CXL_ATOMIC_OR:
        new_value = old_value | operand1;
        break;
    case CXL_ATOMIC_XOR:
        new_value = old_value ^ operand1;
        break;
    case CXL_ATOMIC_MIN:
        new_value = (old_value < operand1) ? old_value : operand1;
        break;
    case CXL_ATOMIC_MAX:
        new_value = (old_value > operand1) ? old_value : operand1;
        break;
    case CXL_ATOMIC_CAS:
        if (old_value == operand1) {
            new_value = operand2;
        } else {
            new_value = old_value;
        }
        break;
    case CXL_ATOMIC_SWAP:
        new_value = operand1;
        break;
    default:
        new_value = old_value;
        break;
    }

    /* Write new value */
    memcpy(mem_ptr + addr, &new_value, MIN(size, 8));

    qemu_mutex_unlock(&state->lock);

    /* Update coherency state */
    if (state->enabled) {
        cxl_bar_snoop_update(state, addr, CXL_COHERENCY_MODIFIED, CXL_DOMAIN_CPU);

        /* Invalidate GPU copy if present */
        CXLSnoopEntry *entry = cxl_bar_snoop_lookup(state, addr);
        if (entry && (entry->domain_mask & (1 << CXL_DOMAIN_GPU))) {
            cxl_bar_back_invalidate(state, addr, size, CXL_DOMAIN_CPU);
        }
    }

    qemu_log_mask(LOG_TRACE, "CXL BAR Atomic: op=%d addr=0x%lx old=0x%lx new=0x%lx\n",
                 op, addr, old_value, new_value);

    return old_value;
}

/* ========================================================================
 * Statistics
 * ======================================================================== */

void cxl_bar_coherency_dump_stats(CXLBARCoherencyState *state)
{
    qemu_log("CXL BAR Coherency Statistics:\n");
    qemu_log("  Snoop Filter: %u entries (hits=%lu, misses=%lu)\n",
             state->snoop_filter_size,
             state->stats.snoop_hits,
             state->stats.snoop_misses);
    qemu_log("  Coherency Requests: %lu\n", state->stats.coherency_requests);
    qemu_log("  Back Invalidations: %lu\n", state->stats.back_invalidations);
    qemu_log("  Writebacks: %lu\n", state->stats.writebacks);
    qemu_log("  Evictions: %lu\n", state->stats.evictions);
    qemu_log("  Conflicts: %lu\n", state->stats.conflicts);
    qemu_log("  Upgrades: %lu, Downgrades: %lu\n",
             state->stats.upgrades, state->stats.downgrades);
    qemu_log("  CPU->GPU Transfers: %lu, GPU->CPU: %lu\n",
             state->stats.cpu_to_gpu_xfers, state->stats.gpu_to_cpu_xfers);
    qemu_log("  Bias Flips: %lu, Device-Bias Hits: %lu, Host-Bias Hits: %lu\n",
             state->stats.bias_flips, state->stats.device_bias_hits,
             state->stats.host_bias_hits);

    /* Per-region statistics */
    for (uint32_t i = 0; i < CXL_MAX_TRACKED_REGIONS; i++) {
        CXLBARCoherencyRegion *region = &state->regions[i];
        if (region->active) {
            qemu_log("  BAR%u Region [0x%lx - 0x%lx]:\n",
                     region->bar_index,
                     region->base_addr,
                     region->base_addr + region->size - 1);
            qemu_log("    CPU: R=%lu W=%lu\n",
                     region->cpu_reads, region->cpu_writes);
            qemu_log("    GPU: R=%lu W=%lu\n",
                     region->gpu_reads, region->gpu_writes);
            qemu_log("    Conflicts: %lu\n", region->coherency_conflicts);
        }
    }
}
