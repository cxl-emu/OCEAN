/*
 * CXL P2P DMA - Peer-to-Peer DMA between CXL devices
 * Implementation of direct memory transfers between Type 2 and Type 3 devices
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/cxl/cxl_device.h"
#include "hw/cxl/cxl_type2.h"
#include "hw/cxl/cxl_type2_coherency.h"
#include "hw/cxl/cxl_p2p_dma.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bus.h"
#include "system/memory.h"
#include "system/hostmem.h"

/* ========================================================================
 * Initialization and Cleanup
 * ======================================================================== */

void cxl_p2p_dma_init(CXLP2PDMAEngine *engine, void *owner)
{
    qemu_mutex_init(&engine->lock);

    memset(engine->peers, 0, sizeof(engine->peers));
    engine->num_peers = 0;

    engine->pending_transfers = g_queue_new();
    engine->completed_transfers = g_queue_new();

    engine->enabled = true;
    engine->busy = false;
    engine->current_transfer = NULL;

    engine->owner = owner;
    engine->owner_peer_id = 0; /* Will be set when self-registered */

    memset(&engine->stats, 0, sizeof(engine->stats));

    /* Configuration defaults */
    engine->max_transfer_size = 64 * 1024 * 1024; /* 64MB max */
    engine->queue_depth = 64;
    engine->auto_coherency = true;

    qemu_log("CXL P2P DMA: Engine initialized\n");
}

void cxl_p2p_dma_cleanup(CXLP2PDMAEngine *engine)
{
    /* Dump final statistics */
    cxl_p2p_dump_stats(engine);

    /* Clean up queues */
    if (engine->pending_transfers) {
        g_queue_free_full(engine->pending_transfers, g_free);
        engine->pending_transfers = NULL;
    }
    if (engine->completed_transfers) {
        g_queue_free_full(engine->completed_transfers, g_free);
        engine->completed_transfers = NULL;
    }

    qemu_mutex_destroy(&engine->lock);

    qemu_log("CXL P2P DMA: Engine cleaned up\n");
}

/* ========================================================================
 * Peer Management
 * ======================================================================== */

static uint32_t allocate_peer_id(CXLP2PDMAEngine *engine)
{
    static uint32_t next_id = 1;
    return next_id++;
}

int cxl_p2p_register_peer_type3(CXLP2PDMAEngine *engine,
                                 CXLType3Dev *ct3d,
                                 uint32_t *peer_id)
{
    CXLP2PPeer *peer;
    int slot = -1;

    if (!engine || !ct3d) {
        return -1;
    }

    qemu_mutex_lock(&engine->lock);

    /* Find free slot */
    for (int i = 0; i < CXL_P2P_MAX_PEERS; i++) {
        if (!engine->peers[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        qemu_mutex_unlock(&engine->lock);
        qemu_log("CXL P2P DMA: No free peer slots\n");
        return -1;
    }

    peer = &engine->peers[slot];
    peer->pci_dev = PCI_DEVICE(ct3d);
    peer->type = CXL_P2P_PEER_TYPE3;
    peer->peer_id = allocate_peer_id(engine);
    peer->active = true;
    peer->coherent = false; /* Type 3 doesn't have CXL.cache by default */

    /* Get memory region info */
    peer->mem_base = 0;
    peer->mem_size = ct3d->cxl_dstate.static_mem_size;

    /* Get direct memory pointer if available */
    if (ct3d->hostvmem) {
        MemoryRegion *mr = host_memory_backend_get_memory(ct3d->hostvmem);
        if (mr) {
            peer->mem_ptr = memory_region_get_ram_ptr(mr);
            peer->mem_size = memory_region_size(mr);
        }
    } else if (ct3d->hostpmem) {
        MemoryRegion *mr = host_memory_backend_get_memory(ct3d->hostpmem);
        if (mr) {
            peer->mem_ptr = memory_region_get_ram_ptr(mr);
            peer->mem_size = memory_region_size(mr);
        }
    }

    snprintf(peer->name, sizeof(peer->name), "cxl-type3-%d",
             PCI_SLOT(peer->pci_dev->devfn));

    engine->num_peers++;

    if (peer_id) {
        *peer_id = peer->peer_id;
    }

    qemu_mutex_unlock(&engine->lock);

    qemu_log("CXL P2P DMA: Registered Type 3 peer '%s' (id=%u, size=%lu MB)\n",
             peer->name, peer->peer_id,
             (unsigned long)(peer->mem_size / (1024 * 1024)));

    return 0;
}

int cxl_p2p_register_peer_type2(CXLP2PDMAEngine *engine,
                                 CXLType2State *ct2d,
                                 uint32_t *peer_id)
{
    CXLP2PPeer *peer;
    int slot = -1;

    if (!engine || !ct2d) {
        return -1;
    }

    qemu_mutex_lock(&engine->lock);

    /* Find free slot */
    for (int i = 0; i < CXL_P2P_MAX_PEERS; i++) {
        if (!engine->peers[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        qemu_mutex_unlock(&engine->lock);
        qemu_log("CXL P2P DMA: No free peer slots\n");
        return -1;
    }

    peer = &engine->peers[slot];
    peer->pci_dev = PCI_DEVICE(ct2d);
    peer->type = CXL_P2P_PEER_TYPE2;
    peer->peer_id = allocate_peer_id(engine);
    peer->active = true;
    peer->coherent = ct2d->coherency.coherency_enabled;

    /* Get memory region info (device memory BAR4) */
    peer->mem_base = 0;
    peer->mem_size = ct2d->device_mem_size;
    peer->mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);

    snprintf(peer->name, sizeof(peer->name), "cxl-type2-%d",
             PCI_SLOT(peer->pci_dev->devfn));

    engine->num_peers++;

    /* If this is the owner, set owner_peer_id */
    if (ct2d == engine->owner) {
        engine->owner_peer_id = peer->peer_id;
    }

    if (peer_id) {
        *peer_id = peer->peer_id;
    }

    qemu_mutex_unlock(&engine->lock);

    qemu_log("CXL P2P DMA: Registered Type 2 peer '%s' (id=%u, size=%lu MB, coherent=%d)\n",
             peer->name, peer->peer_id,
             (unsigned long)(peer->mem_size / (1024 * 1024)),
             peer->coherent);

    return 0;
}

void cxl_p2p_unregister_peer(CXLP2PDMAEngine *engine, uint32_t peer_id)
{
    qemu_mutex_lock(&engine->lock);

    for (int i = 0; i < CXL_P2P_MAX_PEERS; i++) {
        if (engine->peers[i].active && engine->peers[i].peer_id == peer_id) {
            qemu_log("CXL P2P DMA: Unregistered peer '%s' (id=%u)\n",
                     engine->peers[i].name, peer_id);
            memset(&engine->peers[i], 0, sizeof(CXLP2PPeer));
            engine->num_peers--;
            break;
        }
    }

    qemu_mutex_unlock(&engine->lock);
}

CXLP2PPeer *cxl_p2p_get_peer(CXLP2PDMAEngine *engine, uint32_t peer_id)
{
    for (int i = 0; i < CXL_P2P_MAX_PEERS; i++) {
        if (engine->peers[i].active && engine->peers[i].peer_id == peer_id) {
            return &engine->peers[i];
        }
    }
    return NULL;
}

/* Discover peer CXL devices on the same fabric */
int cxl_p2p_discover_peers(CXLP2PDMAEngine *engine)
{
    CXLType2State *owner = engine->owner;
    PCIDevice *owner_pci;
    PCIBus *bus;
    int discovered = 0;

    if (!owner) {
        return -1;
    }

    owner_pci = PCI_DEVICE(owner);
    bus = pci_get_bus(owner_pci);

    if (!bus) {
        qemu_log("CXL P2P DMA: Cannot get PCI bus for peer discovery\n");
        return -1;
    }

    /* Register self first */
    uint32_t self_id;
    if (cxl_p2p_register_peer_type2(engine, owner, &self_id) == 0) {
        discovered++;
    }

    /* Scan bus for CXL Type 3 devices */
    /* Note: In a real implementation, this would use CXL fabric manager
     * protocols. Here we simplify by scanning the PCI bus. */
    for (int devfn = 0; devfn < 256; devfn++) {
        PCIDevice *dev = bus->devices[devfn];
        if (!dev) continue;

        /* Skip self */
        if (dev == owner_pci) continue;

        /* Check if this is a CXL Type 3 device */
        if (object_dynamic_cast(OBJECT(dev), TYPE_CXL_TYPE3)) {
            CXLType3Dev *ct3d = CXL_TYPE3(dev);
            uint32_t peer_id;

            if (cxl_p2p_register_peer_type3(engine, ct3d, &peer_id) == 0) {
                discovered++;
            }
        }
    }

    qemu_log("CXL P2P DMA: Discovered %d peer devices\n", discovered);
    return discovered;
}

/* ========================================================================
 * DMA Transfer Operations
 * ======================================================================== */

static int perform_p2p_transfer(CXLP2PDMAEngine *engine,
                                 CXLP2PDescriptor *desc)
{
    CXLP2PPeer *src_peer = cxl_p2p_get_peer(engine, desc->src_peer_id);
    CXLP2PPeer *dst_peer = cxl_p2p_get_peer(engine, desc->dst_peer_id);
    CXLType2State *owner = engine->owner;
    uint8_t *src_ptr, *dst_ptr;

    if (!src_peer || !dst_peer) {
        desc->status = CXL_P2P_STATUS_ERROR;
        desc->error_code = 1; /* Invalid peer */
        return -1;
    }

    /* Validate transfer size */
    if (desc->size > engine->max_transfer_size) {
        desc->status = CXL_P2P_STATUS_ERROR;
        desc->error_code = 2; /* Transfer too large */
        return -1;
    }

    /* Validate addresses */
    if (desc->src_addr + desc->size > src_peer->mem_size ||
        desc->dst_addr + desc->size > dst_peer->mem_size) {
        desc->status = CXL_P2P_STATUS_ERROR;
        desc->error_code = 3; /* Address out of range */
        return -1;
    }

    desc->status = CXL_P2P_STATUS_IN_PROGRESS;
    desc->start_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Handle coherency if enabled */
    if (engine->auto_coherency && (desc->flags & CXL_P2P_FLAG_COHERENT)) {
        /* For writes to destination, invalidate destination cache */
        cxl_p2p_ensure_coherency(engine, desc->dst_peer_id,
                                  desc->dst_addr, desc->size, true);

        /* For reads from source, ensure source is coherent */
        cxl_p2p_ensure_coherency(engine, desc->src_peer_id,
                                  desc->src_addr, desc->size, false);

        engine->stats.coherency_ops++;
    }

    /* Get memory pointers */
    src_ptr = NULL;
    dst_ptr = NULL;

    if (src_peer->mem_ptr) {
        src_ptr = (uint8_t *)src_peer->mem_ptr + desc->src_addr;
    }
    if (dst_peer->mem_ptr) {
        dst_ptr = (uint8_t *)dst_peer->mem_ptr + desc->dst_addr;
    }

    /* Perform the actual transfer */
    if (src_ptr && dst_ptr) {
        /* Direct memory copy */
        memcpy(dst_ptr, src_ptr, desc->size);
    } else {
        /* Need to use PCI read/write */
        if (src_peer->type == CXL_P2P_PEER_TYPE3 && !src_ptr) {
            /* Read from Type 3 via cxl_type3_read */
            uint8_t *temp_buf = g_malloc(desc->size);
            MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

            for (uint64_t off = 0; off < desc->size; off += 8) {
                uint64_t data;
                size_t chunk = MIN(8, desc->size - off);
                cxl_type3_read(src_peer->pci_dev,
                              desc->src_addr + off,
                              &data, chunk, attrs);
                memcpy(temp_buf + off, &data, chunk);
            }

            if (dst_ptr) {
                memcpy(dst_ptr, temp_buf, desc->size);
            }
            g_free(temp_buf);
        }

        if (dst_peer->type == CXL_P2P_PEER_TYPE3 && !dst_ptr) {
            /* Write to Type 3 via cxl_type3_write */
            uint8_t *temp_buf = NULL;
            MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

            if (src_ptr) {
                temp_buf = src_ptr;
            } else {
                /* Need to allocate and read source first */
                temp_buf = g_malloc(desc->size);
                if (src_peer->type == CXL_P2P_PEER_TYPE2) {
                    CXLType2State *src_t2 = CXL_TYPE2(src_peer->pci_dev);
                    uint8_t *src_mem = memory_region_get_ram_ptr(&src_t2->device_mem);
                    if (src_mem) {
                        memcpy(temp_buf, src_mem + desc->src_addr, desc->size);
                    }
                }
            }

            for (uint64_t off = 0; off < desc->size; off += 8) {
                uint64_t data = 0;
                size_t chunk = MIN(8, desc->size - off);
                memcpy(&data, temp_buf + off, chunk);
                cxl_type3_write(dst_peer->pci_dev,
                               desc->dst_addr + off,
                               data, chunk, attrs);
            }

            if (!src_ptr && temp_buf) {
                g_free(temp_buf);
            }
        }
    }

    desc->end_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    desc->status = CXL_P2P_STATUS_COMPLETE;

    /* Update statistics */
    engine->stats.transfers_completed++;
    engine->stats.bytes_transferred += desc->size;
    engine->stats.total_latency_ns += (desc->end_time - desc->start_time);

    switch (desc->direction) {
    case CXL_P2P_DIR_T2_TO_T3:
        engine->stats.t2_to_t3_transfers++;
        break;
    case CXL_P2P_DIR_T3_TO_T2:
        engine->stats.t3_to_t2_transfers++;
        break;
    case CXL_P2P_DIR_T3_TO_T3:
        engine->stats.t3_to_t3_transfers++;
        break;
    }

    /* Notify BAR coherency layer if owner is Type 2 */
    if (owner && owner->bar_coherency.enabled) {
        if (desc->dst_peer_id == engine->owner_peer_id) {
            /* Data arrived at GPU - notify for coherency */
            cxl_bar_notify_gpu_access(&owner->bar_coherency,
                                       desc->dst_addr, desc->size, true);
        }
    }

    qemu_log_mask(LOG_TRACE,
                 "CXL P2P DMA: Transfer complete src=%u:0x%lx dst=%u:0x%lx "
                 "size=%lu latency=%lu ns\n",
                 desc->src_peer_id, desc->src_addr,
                 desc->dst_peer_id, desc->dst_addr,
                 desc->size,
                 (unsigned long)(desc->end_time - desc->start_time));

    return 0;
}

int cxl_p2p_dma_submit(CXLP2PDMAEngine *engine, CXLP2PDescriptor *desc)
{
    CXLP2PDescriptor *queued;

    if (!engine || !desc) {
        return -1;
    }

    qemu_mutex_lock(&engine->lock);

    if (!engine->enabled) {
        qemu_mutex_unlock(&engine->lock);
        return -1;
    }

    if (g_queue_get_length(engine->pending_transfers) >= engine->queue_depth) {
        qemu_mutex_unlock(&engine->lock);
        return -1; /* Queue full */
    }

    queued = g_new(CXLP2PDescriptor, 1);
    memcpy(queued, desc, sizeof(CXLP2PDescriptor));
    queued->status = CXL_P2P_STATUS_PENDING;

    g_queue_push_tail(engine->pending_transfers, queued);

    qemu_mutex_unlock(&engine->lock);

    /* Process immediately if sync flag */
    if (desc->flags & CXL_P2P_FLAG_SYNC) {
        qemu_mutex_lock(&engine->lock);
        CXLP2PDescriptor *pending = g_queue_pop_head(engine->pending_transfers);
        if (pending) {
            perform_p2p_transfer(engine, pending);
            desc->status = pending->status;
            desc->error_code = pending->error_code;
            desc->start_time = pending->start_time;
            desc->end_time = pending->end_time;
            g_queue_push_tail(engine->completed_transfers, pending);
        }
        qemu_mutex_unlock(&engine->lock);
    }

    return 0;
}

int cxl_p2p_dma_submit_and_wait(CXLP2PDMAEngine *engine, CXLP2PDescriptor *desc)
{
    desc->flags |= CXL_P2P_FLAG_SYNC;
    return cxl_p2p_dma_submit(engine, desc);
}

CXLP2PStatus cxl_p2p_dma_poll(CXLP2PDMAEngine *engine, CXLP2PDescriptor *desc)
{
    return desc->status;
}

void cxl_p2p_dma_abort(CXLP2PDMAEngine *engine, CXLP2PDescriptor *desc)
{
    qemu_mutex_lock(&engine->lock);

    /* Find and remove from pending queue */
    GList *link = g_queue_find(engine->pending_transfers, desc);
    if (link) {
        g_queue_delete_link(engine->pending_transfers, link);
        desc->status = CXL_P2P_STATUS_ABORTED;
    }

    qemu_mutex_unlock(&engine->lock);
}

/* ========================================================================
 * Convenience Functions
 * ======================================================================== */

int cxl_p2p_gpu_to_mem(CXLP2PDMAEngine *engine,
                        uint32_t t3_peer_id,
                        uint64_t gpu_offset,
                        uint64_t mem_offset,
                        uint64_t size,
                        uint32_t flags)
{
    CXLP2PDescriptor desc = {
        .src_addr = gpu_offset,
        .dst_addr = mem_offset,
        .size = size,
        .src_peer_id = engine->owner_peer_id,
        .dst_peer_id = t3_peer_id,
        .flags = flags | CXL_P2P_FLAG_SYNC,
        .direction = CXL_P2P_DIR_T2_TO_T3,
        .status = CXL_P2P_STATUS_IDLE,
    };

    return cxl_p2p_dma_submit_and_wait(engine, &desc);
}

int cxl_p2p_mem_to_gpu(CXLP2PDMAEngine *engine,
                        uint32_t t3_peer_id,
                        uint64_t mem_offset,
                        uint64_t gpu_offset,
                        uint64_t size,
                        uint32_t flags)
{
    CXLP2PDescriptor desc = {
        .src_addr = mem_offset,
        .dst_addr = gpu_offset,
        .size = size,
        .src_peer_id = t3_peer_id,
        .dst_peer_id = engine->owner_peer_id,
        .flags = flags | CXL_P2P_FLAG_SYNC,
        .direction = CXL_P2P_DIR_T3_TO_T2,
        .status = CXL_P2P_STATUS_IDLE,
    };

    return cxl_p2p_dma_submit_and_wait(engine, &desc);
}

int cxl_p2p_mem_to_mem(CXLP2PDMAEngine *engine,
                        uint32_t src_peer_id,
                        uint32_t dst_peer_id,
                        uint64_t src_offset,
                        uint64_t dst_offset,
                        uint64_t size,
                        uint32_t flags)
{
    CXLP2PDescriptor desc = {
        .src_addr = src_offset,
        .dst_addr = dst_offset,
        .size = size,
        .src_peer_id = src_peer_id,
        .dst_peer_id = dst_peer_id,
        .flags = flags | CXL_P2P_FLAG_SYNC,
        .direction = CXL_P2P_DIR_T3_TO_T3,
        .status = CXL_P2P_STATUS_IDLE,
    };

    return cxl_p2p_dma_submit_and_wait(engine, &desc);
}

/* ========================================================================
 * Coherency Helpers
 * ======================================================================== */

void cxl_p2p_ensure_coherency(CXLP2PDMAEngine *engine,
                               uint32_t peer_id,
                               uint64_t addr,
                               uint64_t size,
                               bool is_write)
{
    CXLType2State *owner = engine->owner;
    CXLP2PPeer *peer = cxl_p2p_get_peer(engine, peer_id);

    if (!peer || !owner) {
        return;
    }

    /* If the peer is the GPU (Type 2), use BAR coherency */
    if (peer->type == CXL_P2P_PEER_TYPE2 && peer_id == engine->owner_peer_id) {
        if (owner->bar_coherency.enabled) {
            if (is_write) {
                /* GPU write - need to invalidate CPU copies */
                cxl_bar_coherency_request(&owner->bar_coherency,
                                          CXL_COH_REQ_WR_INV,
                                          addr, size,
                                          CXL_DOMAIN_GPU, NULL);
            } else {
                /* GPU read - ensure CPU has written back */
                cxl_bar_coherency_request(&owner->bar_coherency,
                                          CXL_COH_REQ_RD_SHARED,
                                          addr, size,
                                          CXL_DOMAIN_GPU, NULL);
            }
        }

        /* Also use traditional cache coherency */
        if (is_write) {
            cxl_type2_cache_invalidate(owner, addr);
        } else {
            cxl_type2_cache_writeback(owner, addr);
        }
    }

    /* For Type 3 peers, there's no cache to manage
     * (Type 3 doesn't support CXL.cache) */
}

void cxl_p2p_invalidate_peer_cache(CXLP2PDMAEngine *engine,
                                    uint32_t peer_id,
                                    uint64_t addr,
                                    uint64_t size)
{
    cxl_p2p_ensure_coherency(engine, peer_id, addr, size, true);
}

/* ========================================================================
 * Statistics
 * ======================================================================== */

void cxl_p2p_dump_stats(CXLP2PDMAEngine *engine)
{
    uint64_t avg_latency = 0;

    if (engine->stats.transfers_completed > 0) {
        avg_latency = engine->stats.total_latency_ns /
                      engine->stats.transfers_completed;
    }

    qemu_log("CXL P2P DMA Statistics:\n");
    qemu_log("  Peers registered: %u\n", engine->num_peers);
    qemu_log("  Transfers completed: %lu\n", engine->stats.transfers_completed);
    qemu_log("  Transfers failed: %lu\n", engine->stats.transfers_failed);
    qemu_log("  Bytes transferred: %lu MB\n",
             engine->stats.bytes_transferred / (1024 * 1024));
    qemu_log("  T2->T3 transfers: %lu\n", engine->stats.t2_to_t3_transfers);
    qemu_log("  T3->T2 transfers: %lu\n", engine->stats.t3_to_t2_transfers);
    qemu_log("  T3->T3 transfers: %lu\n", engine->stats.t3_to_t3_transfers);
    qemu_log("  Coherency ops: %lu\n", engine->stats.coherency_ops);
    qemu_log("  Avg latency: %lu ns\n", (unsigned long)avg_latency);

    /* List peers */
    qemu_log("  Registered peers:\n");
    for (int i = 0; i < CXL_P2P_MAX_PEERS; i++) {
        if (engine->peers[i].active) {
            CXLP2PPeer *p = &engine->peers[i];
            qemu_log("    [%u] %s: Type%d, size=%lu MB, coherent=%d\n",
                     p->peer_id, p->name, p->type,
                     (unsigned long)(p->mem_size / (1024 * 1024)),
                     p->coherent);
        }
    }
}
