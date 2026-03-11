/*
 * CXL Type 2 Device (Accelerator with Coherent Memory)
 * GPU Passthrough Forwarder with CPU-GPU Coherency
 *
 * This implements a CXL Type 2 device that combines:
 * - Type 1 cache coherency (CXL.cache protocol)
 * - Type 3 device-attached memory (CXL.mem protocol)
 * - GPU passthrough via VFIO
 * - Full coherency protocol between CPU and GPU memory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "qemu/rcu.h"
#include "qemu/sockets.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_device.h"
#include "hw/cxl/cxl_component.h"
#include "hw/cxl/cxl_cdat.h"
#include "hw/cxl/cxl_pci.h"
#include "hw/cxl/cxl_type2.h"
#include "hw/cxl/cxl_hetgpu.h"
#include "hw/cxl/cxl_type2_gpu_cmd.h"
#include "hw/cxl/cxl_type2_coherency.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/memory.h"
#include "io/channel-socket.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <linux/vfio.h>
#include <unistd.h>

/* Forward declarations for hetGPU coherency integration */
static void cxl_type2_hetgpu_coherency_callback(void *opaque, uint64_t addr,
                                                 uint64_t size, bool invalidate);

/* ========================================================================
 * Coherency Protocol Implementation
 * ======================================================================== */

void cxl_type2_coherency_init(CXLType2State *ct2d)
{
    qemu_mutex_init(&ct2d->coherency.lock);
    ct2d->coherency.cache_lines = g_hash_table_new_full(
        g_int64_hash, g_int64_equal, g_free, g_free);
    ct2d->coherency.cache_hits = 0;
    ct2d->coherency.cache_misses = 0;
    ct2d->coherency.coherency_ops = 0;
    ct2d->coherency.snoops = 0;
    ct2d->coherency.coherency_enabled = true;

    qemu_log("CXL Type2: Coherency protocol initialized\n");
}

void cxl_type2_coherency_cleanup(CXLType2State *ct2d)
{
    if (ct2d->coherency.cache_lines) {
        g_hash_table_destroy(ct2d->coherency.cache_lines);
        ct2d->coherency.cache_lines = NULL;
    }
    qemu_mutex_destroy(&ct2d->coherency.lock);

    qemu_log("CXL Type2: Coherency stats - Hits: %lu, Misses: %lu, Ops: %lu, Snoops: %lu\n",
             ct2d->coherency.cache_hits, ct2d->coherency.cache_misses,
             ct2d->coherency.coherency_ops, ct2d->coherency.snoops);
}

CXLCacheLine *cxl_type2_cache_lookup(CXLType2State *ct2d, uint64_t addr)
{
    /* Align to cache line boundary */
    uint64_t cache_line_addr = addr & ~0x3F;
    CXLCacheLine *line;

    qemu_mutex_lock(&ct2d->coherency.lock);
    line = g_hash_table_lookup(ct2d->coherency.cache_lines,
                               &cache_line_addr);

    if (line) {
        ct2d->coherency.cache_hits++;
    } else {
        ct2d->coherency.cache_misses++;
    }
    qemu_mutex_unlock(&ct2d->coherency.lock);

    return line;
}

void cxl_type2_cache_insert(CXLType2State *ct2d, uint64_t addr,
                            const uint8_t *data, CXLCoherencyState state)
{
    uint64_t cache_line_addr = addr & ~0x3F;
    CXLCacheLine *line = g_new0(CXLCacheLine, 1);
    uint64_t *key = g_new(uint64_t, 1);

    *key = cache_line_addr;
    line->tag = cache_line_addr;
    line->state = state;
    line->dirty = false;
    line->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (data) {
        memcpy(line->data, data, 64);
    }

    qemu_mutex_lock(&ct2d->coherency.lock);
    g_hash_table_insert(ct2d->coherency.cache_lines, key, line);
    ct2d->coherency.coherency_ops++;
    qemu_mutex_unlock(&ct2d->coherency.lock);

    qemu_log_mask(LOG_TRACE, "CXL Type2: Cache insert at 0x%lx, state=%d\n",
                 cache_line_addr, state);
}

void cxl_type2_cache_invalidate(CXLType2State *ct2d, uint64_t addr)
{
    uint64_t cache_line_addr = addr & ~0x3F;

    qemu_mutex_lock(&ct2d->coherency.lock);
    if (g_hash_table_remove(ct2d->coherency.cache_lines, &cache_line_addr)) {
        ct2d->coherency.coherency_ops++;
        qemu_log_mask(LOG_TRACE, "CXL Type2: Cache invalidate at 0x%lx\n",
                     cache_line_addr);
    }
    qemu_mutex_unlock(&ct2d->coherency.lock);
}

void cxl_type2_cache_writeback(CXLType2State *ct2d, uint64_t addr)
{
    uint64_t cache_line_addr = addr & ~0x3F;
    CXLCacheLine *line;

    qemu_mutex_lock(&ct2d->coherency.lock);
    line = g_hash_table_lookup(ct2d->coherency.cache_lines, &cache_line_addr);

    if (line && line->dirty) {
        /* Write back to device memory */
        if (cache_line_addr < ct2d->device_mem_size) {
            uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
            if (mem_ptr) {
                memcpy(mem_ptr + cache_line_addr, line->data, 64);
            }
        }

        /* Send writeback to CXLMemSim */
        if (ct2d->memsim.connected) {
            CXLType2Message msg = {
                .type = CXL_T2_MSG_WRITEBACK,
                .size = 64,
                .addr = cache_line_addr,
                .timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                .coherency_state = line->state,
                .source_id = 0,
            };
            memcpy(msg.data, line->data, 64);

            qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                 (char *)&msg, sizeof(msg), NULL);
        }

        line->dirty = false;
        ct2d->coherency.coherency_ops++;

        qemu_log_mask(LOG_TRACE, "CXL Type2: Cache writeback at 0x%lx\n",
                     cache_line_addr);
    }
    qemu_mutex_unlock(&ct2d->coherency.lock);
}

bool cxl_type2_snoop_request(CXLType2State *ct2d, uint64_t addr, bool invalidate)
{
    uint64_t cache_line_addr = addr & ~0x3F;
    CXLCacheLine *line;
    bool hit = false;

    qemu_mutex_lock(&ct2d->coherency.lock);
    ct2d->coherency.snoops++;

    line = g_hash_table_lookup(ct2d->coherency.cache_lines, &cache_line_addr);
    if (line) {
        hit = true;

        /* If dirty, write back before invalidation */
        if (line->dirty) {
            qemu_mutex_unlock(&ct2d->coherency.lock);
            cxl_type2_cache_writeback(ct2d, addr);
            qemu_mutex_lock(&ct2d->coherency.lock);
            line = g_hash_table_lookup(ct2d->coherency.cache_lines, &cache_line_addr);
        }

        if (invalidate && line) {
            g_hash_table_remove(ct2d->coherency.cache_lines, &cache_line_addr);
        } else if (line) {
            /* Downgrade to shared */
            line->state = CXL_COHERENCY_SHARED;
        }
    }

    qemu_mutex_unlock(&ct2d->coherency.lock);

    qemu_log_mask(LOG_TRACE, "CXL Type2: Snoop request at 0x%lx, hit=%d, invalidate=%d\n",
                 cache_line_addr, hit, invalidate);

    return hit;
}

/* ========================================================================
 * GPU Passthrough Implementation - VFIO Helpers
 * ======================================================================== */

/* Open and setup VFIO container */
static int cxl_type2_vfio_container_init(CXLType2State *ct2d, Error **errp)
{
    int container_fd, version;

    /* Open VFIO container */
    container_fd = open("/dev/vfio/vfio", O_RDWR);
    if (container_fd < 0) {
        error_setg(errp, "Failed to open /dev/vfio/vfio: %s", strerror(errno));
        return -1;
    }

    /* Check VFIO API version */
    version = ioctl(container_fd, VFIO_GET_API_VERSION);
    if (version != VFIO_API_VERSION) {
        error_setg(errp, "VFIO API version mismatch: expected %d, got %d",
                   VFIO_API_VERSION, version);
        close(container_fd);
        return -1;
    }

    /* Check VFIO extension support */
    if (!ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) &&
        !ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU)) {
        error_setg(errp, "VFIO does not support TYPE1 or TYPE1v2 IOMMU");
        close(container_fd);
        return -1;
    }

    ct2d->gpu_info.vfio_container = GINT_TO_POINTER(container_fd);
    qemu_log("CXL Type2: VFIO container initialized (fd=%d)\n", container_fd);

    return container_fd;
}

/* Setup VFIO group for GPU device */
static int cxl_type2_vfio_group_init(CXLType2State *ct2d, const char *pci_addr,
                                      Error **errp)
{
    char group_path[256];
    char iommu_group_path[512];
    char *group_name;
    int group_fd, container_fd;
    ssize_t len;
    struct vfio_group_status group_status = { .argsz = sizeof(group_status) };

    /* Get IOMMU group for the device */
    snprintf(iommu_group_path, sizeof(iommu_group_path),
             "/sys/bus/pci/devices/%s/iommu_group", pci_addr);

    len = readlink(iommu_group_path, group_path, sizeof(group_path) - 1);
    if (len < 0) {
        error_setg(errp, "Failed to read IOMMU group for %s: %s",
                   pci_addr, strerror(errno));
        return -1;
    }
    group_path[len] = '\0';

    /* Extract group number from path */
    group_name = basename(group_path);

    /* Open VFIO group */
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%s", group_name);
    group_fd = open(group_path, O_RDWR);
    if (group_fd < 0) {
        error_setg(errp, "Failed to open VFIO group %s: %s",
                   group_path, strerror(errno));
        return -1;
    }

    /* Check group is viable */
    if (ioctl(group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0) {
        error_setg(errp, "Failed to get VFIO group status: %s", strerror(errno));
        close(group_fd);
        return -1;
    }

    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        error_setg(errp, "VFIO group is not viable (not all devices bound to VFIO)");
        close(group_fd);
        return -1;
    }

    /* Add group to container */
    container_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_container);
    if (ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd) < 0) {
        error_setg(errp, "Failed to add group to container: %s", strerror(errno));
        close(group_fd);
        return -1;
    }

    /* Enable IOMMU for the container */
    if (ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0) {
        if (ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
            error_setg(errp, "Failed to set IOMMU type: %s", strerror(errno));
            close(group_fd);
            return -1;
        }
    }

    ct2d->gpu_info.vfio_group = GINT_TO_POINTER(group_fd);
    qemu_log("CXL Type2: VFIO group %s initialized (fd=%d)\n", group_name, group_fd);

    return group_fd;
}

/* Open and setup VFIO device */
static int cxl_type2_vfio_device_init(CXLType2State *ct2d, const char *pci_addr,
                                       Error **errp)
{
    int group_fd, device_fd;
    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
    struct vfio_region_info region_info = { .argsz = sizeof(region_info) };

    group_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_group);

    /* Get device FD */
    device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
    if (device_fd < 0) {
        error_setg(errp, "Failed to get device FD for %s: %s",
                   pci_addr, strerror(errno));
        return -1;
    }

    /* Get device info */
    if (ioctl(device_fd, VFIO_DEVICE_GET_INFO, &device_info) < 0) {
        error_setg(errp, "Failed to get device info: %s", strerror(errno));
        close(device_fd);
        return -1;
    }

    qemu_log("CXL Type2: GPU device %s has %d regions, %d IRQs\n",
             pci_addr, device_info.num_regions, device_info.num_irqs);

    /* Get BAR0 region info (GPU memory typically in BAR0 or BAR1) */
    region_info.index = VFIO_PCI_BAR0_REGION_INDEX;
    if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region_info) < 0) {
        error_setg(errp, "Failed to get BAR0 region info: %s", strerror(errno));
        close(device_fd);
        return -1;
    }

    /* Map GPU memory region */
    if (region_info.size > 0) {
        void *bar_mem = mmap(NULL, region_info.size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            device_fd,
                            region_info.offset);

        if (bar_mem == MAP_FAILED) {
            error_setg(errp, "Failed to mmap GPU BAR0: %s", strerror(errno));
            close(device_fd);
            return -1;
        }

        ct2d->gpu_info.gpu_mem_base = region_info.offset;
        ct2d->gpu_info.gpu_mem_size = region_info.size;

        qemu_log("CXL Type2: Mapped GPU BAR0 at offset 0x%llx, size %llu MB\n",
                 region_info.offset, region_info.size / (1024 * 1024));
    }

    ct2d->gpu_info.vfio_device_fd = device_fd;

    return device_fd;
}

/* Setup DMA mapping for GPU coherent access */
static int cxl_type2_vfio_dma_map(CXLType2State *ct2d, Error **errp)
{
    int container_fd;
    struct vfio_iommu_type1_dma_map dma_map = {
        .argsz = sizeof(dma_map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = 0,  /* Will be set to device memory */
        .iova = 0,   /* IO virtual address */
        .size = 0,   /* Will be set to device memory size */
    };

    container_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_container);

    /* Get device memory pointer */
    uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
    if (!mem_ptr) {
        error_setg(errp, "Failed to get device memory pointer");
        return -1;
    }

    dma_map.vaddr = (uint64_t)mem_ptr;
    dma_map.iova = 0;  /* Map at IOVA 0 */
    dma_map.size = ct2d->device_mem_size;

    /* Map the device memory for DMA */
    if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
        error_setg(errp, "Failed to map DMA region: %s", strerror(errno));
        return -1;
    }

    qemu_log("CXL Type2: DMA mapping configured - IOVA: 0x%llx, Size: %llu MB\n",
             dma_map.iova, dma_map.size / (1024 * 1024));

    return 0;
}

/* Interrupt handling thread for GPU events */
static void *cxl_type2_irq_thread(void *opaque)
{
    CXLType2State *ct2d = opaque;
    PCIDevice *pci_dev = PCI_DEVICE(ct2d);
    int device_fd = ct2d->gpu_info.vfio_device_fd;
    struct vfio_irq_info irq_info = { .argsz = sizeof(irq_info) };
    int *event_fds = NULL;
    int num_irqs = 0;
    int ret, i;

    /* Get IRQ info for MSI-X */
    irq_info.index = VFIO_PCI_MSIX_IRQ_INDEX;
    if (ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info) < 0) {
        qemu_log("CXL Type2: Failed to get MSI-X IRQ info: %s\n", strerror(errno));
        return NULL;
    }

    num_irqs = irq_info.count;
    if (num_irqs == 0) {
        qemu_log("CXL Type2: No MSI-X interrupts available\n");
        return NULL;
    }

    qemu_log("CXL Type2: Setting up %d MSI-X interrupts\n", num_irqs);

    /* Create event FDs for each interrupt */
    event_fds = g_new0(int, num_irqs);
    for (i = 0; i < num_irqs; i++) {
        event_fds[i] = eventfd(0, EFD_NONBLOCK);
        if (event_fds[i] < 0) {
            qemu_log("CXL Type2: Failed to create event FD %d: %s\n", i, strerror(errno));
            goto cleanup;
        }
    }

    /* Set up VFIO IRQ forwarding */
    struct vfio_irq_set *irq_set;
    size_t irq_set_size = sizeof(*irq_set) + num_irqs * sizeof(int);
    irq_set = g_malloc0(irq_set_size);
    irq_set->argsz = irq_set_size;
    irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
    irq_set->start = 0;
    irq_set->count = num_irqs;
    memcpy(irq_set->data, event_fds, num_irqs * sizeof(int));

    if (ioctl(device_fd, VFIO_DEVICE_SET_IRQS, irq_set) < 0) {
        qemu_log("CXL Type2: Failed to set IRQs: %s\n", strerror(errno));
        g_free(irq_set);
        goto cleanup;
    }

    g_free(irq_set);
    qemu_log("CXL Type2: MSI-X interrupt forwarding configured\n");

    /* Monitor interrupts and forward to guest */
    fd_set rfds;
    int max_fd = 0;
    for (i = 0; i < num_irqs; i++) {
        if (event_fds[i] > max_fd) {
            max_fd = event_fds[i];
        }
    }

    while (ct2d->gpu_info.passthrough_enabled) {
        FD_ZERO(&rfds);
        for (i = 0; i < num_irqs; i++) {
            FD_SET(event_fds[i], &rfds);
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            qemu_log("CXL Type2: IRQ select failed: %s\n", strerror(errno));
            break;
        }

        if (ret == 0) {
            /* Timeout, check if still enabled */
            continue;
        }

        /* Check which interrupt fired */
        for (i = 0; i < num_irqs; i++) {
            if (FD_ISSET(event_fds[i], &rfds)) {
                uint64_t count;
                ssize_t s = read(event_fds[i], &count, sizeof(count));
                if (s == sizeof(count)) {
                    qemu_log_mask(LOG_TRACE, "CXL Type2: GPU interrupt %d triggered\n", i);

                    /* Notify guest via MSI-X */
                    if (msix_enabled(pci_dev)) {
                        msix_notify(pci_dev, i);
                    }

                    /* Update statistics */
                    ct2d->stats.gpu_accesses++;
                }
            }
        }
    }

cleanup:
    /* Clean up event FDs */
    for (i = 0; i < num_irqs; i++) {
        if (event_fds[i] >= 0) {
            close(event_fds[i]);
        }
    }
    g_free(event_fds);

    qemu_log("CXL Type2: IRQ thread exiting\n");
    return NULL;
}

/* ========================================================================
 * hetGPU Backend Implementation
 * ======================================================================== */

/* Coherency callback for hetGPU operations */
static void cxl_type2_hetgpu_coherency_callback(void *opaque, uint64_t addr,
                                                 uint64_t size, bool invalidate)
{
    CXLType2State *ct2d = opaque;

    if (!ct2d || !ct2d->coherency.coherency_enabled) {
        return;
    }

    /* Notify enhanced BAR coherency layer of GPU access */
    if (ct2d->bar_coherency.enabled) {
        cxl_bar_notify_gpu_access(&ct2d->bar_coherency, addr, size, invalidate);
    }

    if (invalidate) {
        /* GPU is writing - invalidate CPU cache lines */
        if (addr && size) {
            uint64_t aligned_addr = addr & ~0x3F;
            uint64_t end_addr = (addr + size + 63) & ~0x3F;
            for (uint64_t a = aligned_addr; a < end_addr; a += 64) {
                cxl_type2_cache_invalidate(ct2d, a);
            }
        }
    } else {
        /* GPU is reading - write back CPU cache lines */
        if (addr && size) {
            uint64_t aligned_addr = addr & ~0x3F;
            uint64_t end_addr = (addr + size + 63) & ~0x3F;
            for (uint64_t a = aligned_addr; a < end_addr; a += 64) {
                cxl_type2_cache_writeback(ct2d, a);
            }
        }
    }

    ct2d->coherency.coherency_ops++;
}

int cxl_type2_hetgpu_init(CXLType2State *ct2d, Error **errp)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;
    const char *lib_path;

    fprintf(stderr, "cxl_type2_hetgpu_init: ENTERED\n");
    fflush(stderr);

    /* Determine library path - try multiple locations */
    lib_path = ct2d->gpu_info.hetgpu_lib_path;
    fprintf(stderr, "cxl_type2_hetgpu_init: hetgpu_lib_path = '%s'\n", lib_path ? lib_path : "(null)");
    fflush(stderr);
    if (!lib_path || lib_path[0] == '\0') {
        lib_path = getenv("HETGPU_LIB_PATH");
        fprintf(stderr, "cxl_type2_hetgpu_init: env HETGPU_LIB_PATH = '%s'\n", lib_path ? lib_path : "(null)");
        fflush(stderr);
    }
    if (!lib_path || lib_path[0] == '\0') {
        /* Try system CUDA library first for real GPU passthrough */
        lib_path = "/usr/lib/x86_64-linux-gnu/libcuda.so";
    }

    fprintf(stderr, "cxl_type2_hetgpu_init: Using library: %s\n", lib_path);
    fprintf(stderr, "cxl_type2_hetgpu_init: backend=%d, device_index=%d\n",
            ct2d->gpu_info.hetgpu_backend, ct2d->gpu_info.hetgpu_device_index);
    fflush(stderr);

    /* Initialize hetGPU */
    err = hetgpu_init(hetgpu,
                      ct2d->gpu_info.hetgpu_backend,
                      ct2d->gpu_info.hetgpu_device_index,
                      lib_path);
    if (err != HETGPU_SUCCESS) {
        error_setg(errp, "hetGPU initialization failed: %s",
                   hetgpu_get_error_string(err));
        return -1;
    }

    /* Create context */
    err = hetgpu_create_context(hetgpu);
    if (err != HETGPU_SUCCESS) {
        error_setg(errp, "hetGPU context creation failed: %s",
                   hetgpu_get_error_string(err));
        hetgpu_cleanup(hetgpu);
        return -1;
    }

    /* Set up coherency callback */
    hetgpu_set_coherency_callback(hetgpu,
                                   cxl_type2_hetgpu_coherency_callback,
                                   ct2d);

    ct2d->gpu_info.passthrough_enabled = true;
    ct2d->gpu_info.gpu_mem_size = hetgpu->props.total_memory;

    qemu_log("CXL Type2: hetGPU initialized - Backend: %s, Device: %s\n",
             hetgpu_get_backend_name(hetgpu->backend),
             hetgpu->props.name);
    qemu_log("CXL Type2: GPU Memory: %lu MB, Compute: %d.%d\n",
             hetgpu->props.total_memory / (1024 * 1024),
             hetgpu->props.compute_capability_major,
             hetgpu->props.compute_capability_minor);

    return 0;
}

void cxl_type2_hetgpu_cleanup(CXLType2State *ct2d)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;

    if (!hetgpu->initialized) {
        return;
    }

    qemu_log("CXL Type2: Cleaning up hetGPU backend\n");
    hetgpu_cleanup(hetgpu);
}

int cxl_type2_hetgpu_load_ptx(CXLType2State *ct2d, const char *ptx_source,
                               void **module)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    err = hetgpu_load_ptx(hetgpu, ptx_source, (HetGPUModule *)module);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: Failed to load PTX: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    return 0;
}

int cxl_type2_hetgpu_launch_kernel(CXLType2State *ct2d, void *function,
                                    uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
                                    uint32_t block_x, uint32_t block_y, uint32_t block_z,
                                    uint32_t shared_mem, void **args, size_t num_args)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPULaunchConfig config;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    config.grid_dim[0] = grid_x;
    config.grid_dim[1] = grid_y;
    config.grid_dim[2] = grid_z;
    config.block_dim[0] = block_x;
    config.block_dim[1] = block_y;
    config.block_dim[2] = block_z;
    config.shared_mem_bytes = shared_mem;
    config.stream = NULL;

    err = hetgpu_launch_kernel(hetgpu, function, &config, args, num_args);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: Kernel launch failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    ct2d->stats.gpu_accesses++;
    return 0;
}

int cxl_type2_hetgpu_malloc(CXLType2State *ct2d, size_t size, uint64_t *dev_ptr)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized || !dev_ptr) {
        return -1;
    }

    err = hetgpu_malloc(hetgpu, size, HETGPU_MEM_HOST_MAPPED,
                        (HetGPUDevicePtr *)dev_ptr);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: Memory allocation failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    return 0;
}

int cxl_type2_hetgpu_free(CXLType2State *ct2d, uint64_t dev_ptr)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    err = hetgpu_free(hetgpu, dev_ptr);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: Memory free failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    return 0;
}

int cxl_type2_hetgpu_memcpy_htod(CXLType2State *ct2d, uint64_t dst,
                                  const void *src, size_t size)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    /* Invalidate cache before GPU write */
    cxl_type2_cache_invalidate(ct2d, dst);

    err = hetgpu_memcpy_htod(hetgpu, dst, src, size);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: HtoD memcpy failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    ct2d->stats.gpu_accesses++;
    return 0;
}

int cxl_type2_hetgpu_memcpy_dtoh(CXLType2State *ct2d, void *dst,
                                  uint64_t src, size_t size)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    /* Writeback cache before GPU read */
    cxl_type2_cache_writeback(ct2d, src);

    err = hetgpu_memcpy_dtoh(hetgpu, dst, src, size);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: DtoH memcpy failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    ct2d->stats.gpu_accesses++;
    return 0;
}

int cxl_type2_hetgpu_sync(CXLType2State *ct2d)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;

    if (!hetgpu->initialized) {
        return -1;
    }

    err = hetgpu_synchronize(hetgpu);
    if (err != HETGPU_SUCCESS) {
        qemu_log("CXL Type2: Sync failed: %s\n",
                 hetgpu_get_error_string(err));
        return -1;
    }

    return 0;
}

/* ========================================================================
 * GPU Passthrough Implementation - Main Functions
 * ======================================================================== */

int cxl_type2_gpu_init(CXLType2State *ct2d, Error **errp)
{
    Error *local_err = NULL;
    int ret;

    /* Determine GPU mode if auto */
    if (ct2d->gpu_info.mode == CXL_TYPE2_GPU_MODE_AUTO) {
        if (ct2d->gpu_info.vfio_device && ct2d->gpu_info.vfio_device[0]) {
            ct2d->gpu_info.mode = CXL_TYPE2_GPU_MODE_VFIO;
        } else {
            /* Default to hetGPU mode - will use system libcuda.so for real GPU */
            ct2d->gpu_info.mode = CXL_TYPE2_GPU_MODE_HETGPU;
        }
    }

    /* Initialize based on mode */
    fprintf(stderr, "CXL Type2: GPU mode = %d (NONE=0, VFIO=1, HETGPU=2, AUTO=3)\n", ct2d->gpu_info.mode);
    fflush(stderr);
    switch (ct2d->gpu_info.mode) {
    case CXL_TYPE2_GPU_MODE_HETGPU:
        fprintf(stderr, "CXL Type2: Initializing hetGPU backend...\n");
        fflush(stderr);
        ret = cxl_type2_hetgpu_init(ct2d, errp);
        fprintf(stderr, "CXL Type2: cxl_type2_hetgpu_init returned %d\n", ret);
        fflush(stderr);
        if (ret == 0) {
            fprintf(stderr, "CXL Type2: hetGPU backend initialized successfully\n");
            fflush(stderr);
            return 0;
        }
        /* Fall through to VFIO or simulation if hetGPU fails */
        fprintf(stderr, "CXL Type2: hetGPU init failed, trying fallback\n");
        fflush(stderr);
        /* fall through */

    case CXL_TYPE2_GPU_MODE_VFIO:
        if (!ct2d->gpu_info.vfio_device || !ct2d->gpu_info.vfio_device[0]) {
            qemu_log("CXL Type2: No VFIO device specified\n");
            ct2d->gpu_info.mode = CXL_TYPE2_GPU_MODE_NONE;
            /* fall through */
        } else {
            qemu_log("CXL Type2: Initializing GPU passthrough for device %s\n",
                     ct2d->gpu_info.vfio_device);
            break; /* Continue with VFIO init below */
        }
        /* fall through */

    case CXL_TYPE2_GPU_MODE_NONE:
    default:
        qemu_log("CXL Type2: GPU passthrough not configured (simulation mode)\n");
        ct2d->gpu_info.gpu_mem_base = 0;
        ct2d->gpu_info.gpu_mem_size = ct2d->device_mem_size;
        ct2d->gpu_info.passthrough_enabled = false;
        return 0;
    }

    /* VFIO passthrough initialization continues here */
    qemu_log("CXL Type2: Initializing VFIO passthrough for device %s\n",
             ct2d->gpu_info.vfio_device);

    /* Step 1: Open VFIO container */
    ret = cxl_type2_vfio_container_init(ct2d, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        return -1;
    }

    /* Step 2: Setup VFIO group */
    ret = cxl_type2_vfio_group_init(ct2d, ct2d->gpu_info.vfio_device, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        goto err_close_container;
    }

    /* Step 3: Get device FD and map BARs */
    ret = cxl_type2_vfio_device_init(ct2d, ct2d->gpu_info.vfio_device, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        goto err_close_group;
    }

    /* Step 4: Setup DMA mapping for coherent access */
    ret = cxl_type2_vfio_dma_map(ct2d, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        goto err_close_device;
    }

    ct2d->gpu_info.passthrough_enabled = true;

    /* Step 5: Start IRQ forwarding thread */
    ct2d->gpu_info.irq_thread_running = true;
    qemu_thread_create(&ct2d->gpu_info.irq_thread, "cxl-type2-irq",
                       cxl_type2_irq_thread, ct2d, QEMU_THREAD_JOINABLE);

    qemu_log("CXL Type2: GPU passthrough successfully initialized\n");
    qemu_log("CXL Type2: GPU memory: base=0x%lx, size=%lu MB\n",
             ct2d->gpu_info.gpu_mem_base,
             ct2d->gpu_info.gpu_mem_size / (1024 * 1024));

    return 0;

err_close_device:
    if (ct2d->gpu_info.vfio_device_fd >= 0) {
        close(ct2d->gpu_info.vfio_device_fd);
        ct2d->gpu_info.vfio_device_fd = -1;
    }

err_close_group:
    if (ct2d->gpu_info.vfio_group) {
        close(GPOINTER_TO_INT(ct2d->gpu_info.vfio_group));
        ct2d->gpu_info.vfio_group = NULL;
    }

err_close_container:
    if (ct2d->gpu_info.vfio_container) {
        close(GPOINTER_TO_INT(ct2d->gpu_info.vfio_container));
        ct2d->gpu_info.vfio_container = NULL;
    }

    ct2d->gpu_info.passthrough_enabled = false;
    return -1;
}

void cxl_type2_gpu_cleanup(CXLType2State *ct2d)
{
    if (!ct2d->gpu_info.passthrough_enabled) {
        return;
    }

    /* Handle hetGPU cleanup */
    if (ct2d->gpu_info.mode == CXL_TYPE2_GPU_MODE_HETGPU) {
        cxl_type2_hetgpu_cleanup(ct2d);
        ct2d->gpu_info.passthrough_enabled = false;
        return;
    }

    /* VFIO cleanup */
    /* Stop IRQ forwarding thread */
    if (ct2d->gpu_info.irq_thread_running) {
        ct2d->gpu_info.irq_thread_running = false;
        ct2d->gpu_info.passthrough_enabled = false;  /* Signal thread to exit */
        qemu_thread_join(&ct2d->gpu_info.irq_thread);
        qemu_log("CXL Type2: IRQ thread stopped\n");
    }

    /* Unmap DMA regions */
    if (ct2d->gpu_info.vfio_container) {
        int container_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_container);
        struct vfio_iommu_type1_dma_unmap dma_unmap = {
            .argsz = sizeof(dma_unmap),
            .flags = 0,
            .iova = 0,
            .size = ct2d->device_mem_size,
        };

        if (ioctl(container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0) {
            qemu_log("CXL Type2: Warning - Failed to unmap DMA: %s\n", strerror(errno));
        }
    }

    /* Disable VFIO interrupts */
    if (ct2d->gpu_info.vfio_device_fd >= 0) {
        struct vfio_irq_set irq_set = {
            .argsz = sizeof(irq_set),
            .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
            .index = VFIO_PCI_MSIX_IRQ_INDEX,
            .start = 0,
            .count = 0,
        };
        ioctl(ct2d->gpu_info.vfio_device_fd, VFIO_DEVICE_SET_IRQS, &irq_set);
    }

    /* Close VFIO device */
    if (ct2d->gpu_info.vfio_device_fd >= 0) {
        close(ct2d->gpu_info.vfio_device_fd);
        ct2d->gpu_info.vfio_device_fd = -1;
    }

    /* Close VFIO group */
    if (ct2d->gpu_info.vfio_group) {
        int group_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_group);
        close(group_fd);
        ct2d->gpu_info.vfio_group = NULL;
    }

    /* Close VFIO container */
    if (ct2d->gpu_info.vfio_container) {
        int container_fd = GPOINTER_TO_INT(ct2d->gpu_info.vfio_container);
        close(container_fd);
        ct2d->gpu_info.vfio_container = NULL;
    }

    qemu_log("CXL Type2: GPU passthrough cleanup complete\n");
}

int cxl_type2_gpu_read(CXLType2State *ct2d, uint64_t offset, void *buf, size_t size)
{
    ssize_t ret;

    if (!ct2d->gpu_info.passthrough_enabled) {
        return -1;
    }

    /* If VFIO device is configured, read from actual GPU BAR */
    if (ct2d->gpu_info.vfio_device_fd >= 0) {
        uint64_t bar_offset = ct2d->gpu_info.gpu_mem_base + offset;

        ret = pread(ct2d->gpu_info.vfio_device_fd, buf, size, bar_offset);
        if (ret < 0) {
            qemu_log("CXL Type2: GPU read failed at offset 0x%lx: %s\n",
                     offset, strerror(errno));
            return -1;
        }

        if (ret != size) {
            qemu_log("CXL Type2: GPU read incomplete at offset 0x%lx: %zd/%zu bytes\n",
                     offset, ret, size);
            return -1;
        }

        ct2d->stats.gpu_accesses++;
        return 0;
    }

    /* Fallback to device memory for simulation mode */
    if (offset + size <= ct2d->device_mem_size) {
        uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
        if (mem_ptr) {
            memcpy(buf, mem_ptr + offset, size);
            ct2d->stats.gpu_accesses++;
            return 0;
        }
    }

    return -1;
}

int cxl_type2_gpu_write(CXLType2State *ct2d, uint64_t offset, const void *buf, size_t size)
{
    ssize_t ret;

    if (!ct2d->gpu_info.passthrough_enabled) {
        return -1;
    }

    /* If VFIO device is configured, write to actual GPU BAR */
    if (ct2d->gpu_info.vfio_device_fd >= 0) {
        uint64_t bar_offset = ct2d->gpu_info.gpu_mem_base + offset;

        ret = pwrite(ct2d->gpu_info.vfio_device_fd, buf, size, bar_offset);
        if (ret < 0) {
            qemu_log("CXL Type2: GPU write failed at offset 0x%lx: %s\n",
                     offset, strerror(errno));
            return -1;
        }

        if (ret != size) {
            qemu_log("CXL Type2: GPU write incomplete at offset 0x%lx: %zd/%zu bytes\n",
                     offset, ret, size);
            return -1;
        }

        /* Invalidate cache line if present for coherency */
        cxl_type2_cache_invalidate(ct2d, offset);

        /* Notify CXLMemSim of GPU write for coherency protocol */
        if (ct2d->memsim.connected) {
            CXLType2Message msg = {
                .type = CXL_T2_MSG_GPU_ACCESS,
                .size = size,
                .addr = offset,
                .timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                .coherency_state = CXL_COHERENCY_MODIFIED,
                .source_id = 1,  /* GPU source */
            };

            qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                 (char *)&msg, sizeof(msg), NULL);
        }

        ct2d->stats.gpu_accesses++;
        return 0;
    }

    /* Fallback to device memory for simulation mode */
    if (offset + size <= ct2d->device_mem_size) {
        uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
        if (mem_ptr) {
            memcpy(mem_ptr + offset, buf, size);

            /* Invalidate cache line if present */
            cxl_type2_cache_invalidate(ct2d, offset);

            ct2d->stats.gpu_accesses++;
            return 0;
        }
    }

    return -1;
}

/* ========================================================================
 * CXLMemSim Communication
 * ======================================================================== */

static void cxlmemsim_connect(CXLType2State *ct2d)
{
    Error *err = NULL;
    SocketAddress addr;

    if (ct2d->memsim.connected) {
        return;
    }

    /* Check if using shared memory mode */
    const char *transport_mode = getenv("CXL_TRANSPORT_MODE");
    if (!transport_mode || !transport_mode[0]) {
        transport_mode = getenv("CXL_MEMSIM_TRANSPORT");
    }

    qemu_log("CXL Type2: Transport mode = %s\n", transport_mode ? transport_mode : "(not set)");

    if (transport_mode && (strcmp(transport_mode, "shm") == 0 ||
                           strcmp(transport_mode, "pgas") == 0)) {
        ct2d->memsim.use_shm = true;
        qemu_log("CXL Type2: Using shared memory transport - skipping TCP connection\n");
        /* Type3 device handles SHM connection */
        return;
    }

    qemu_log("CXL Type2: Using TCP transport mode\n");

    /* TCP connection to CXLMemSim */
    addr.type = SOCKET_ADDRESS_TYPE_INET;
    addr.u.inet.host = ct2d->memsim.server_addr;
    addr.u.inet.port = g_strdup_printf("%u", ct2d->memsim.server_port);

    ct2d->memsim.socket = qio_channel_socket_new();
    if (qio_channel_socket_connect_sync(ct2d->memsim.socket, &addr, &err) < 0) {
        qemu_log("Warning: Failed to connect to CXLMemSim at %s:%s: %s\n",
                addr.u.inet.host, addr.u.inet.port, error_get_pretty(err));
        error_free(err);
        g_free(addr.u.inet.port);
        object_unref(OBJECT(ct2d->memsim.socket));
        ct2d->memsim.socket = NULL;
        return;
    }

    ct2d->memsim.connected = true;
    g_free(addr.u.inet.port);

    qemu_log("CXL Type2: Connected to CXLMemSim at %s:%u\n",
            ct2d->memsim.server_addr, ct2d->memsim.server_port);
}

static void cxlmemsim_disconnect(CXLType2State *ct2d)
{
    if (!ct2d->memsim.connected) {
        return;
    }

    ct2d->memsim.connected = false;

    if (ct2d->memsim.socket) {
        qio_channel_close(QIO_CHANNEL(ct2d->memsim.socket), NULL);
        object_unref(OBJECT(ct2d->memsim.socket));
        ct2d->memsim.socket = NULL;
    }

    if (ct2d->memsim.use_shm && ct2d->memsim.shm_base) {
        munmap(ct2d->memsim.shm_base, ct2d->memsim.shm_size);
        ct2d->memsim.shm_base = NULL;
    }
}

static void *cxlmemsim_recv_thread(void *opaque)
{
    CXLType2State *ct2d = opaque;
    CXLType2Message msg;
    Error *err = NULL;

    while (ct2d->memsim.connected) {
        if (qio_channel_read_all(QIO_CHANNEL(ct2d->memsim.socket),
                                 (char *)&msg, sizeof(msg), &err) < 0) {
            if (ct2d->memsim.connected) {
                error_report("CXL Type2: Failed to receive from CXLMemSim: %s",
                           error_get_pretty(err));
                error_free(err);
            }
            break;
        }

        /* Handle incoming coherency messages */
        switch (msg.type) {
        case CXL_T2_MSG_SNOOP_REQ:
            /* Remote snoop request */
            cxl_type2_snoop_request(ct2d, msg.addr, msg.coherency_state != 0);
            break;

        case CXL_T2_MSG_INVALIDATE:
            /* Remote invalidation */
            cxl_type2_cache_invalidate(ct2d, msg.addr);
            break;

        case CXL_T2_MSG_RESPONSE:
            /* Response to our request */
            qemu_log_mask(LOG_TRACE, "CXL Type2: Received response for addr 0x%lx\n",
                         msg.addr);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "CXL Type2: Unhandled message type %d\n",
                         msg.type);
            break;
        }
    }

    return NULL;
}

/* ========================================================================
 * Memory Access Handlers with Coherency
 * ======================================================================== */

/* Forward declaration for GPU command handler */
static uint64_t cxl_type2_gpu_cmd_read(void *opaque, hwaddr addr, unsigned size);
static void cxl_type2_gpu_cmd_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);

static uint64_t cxl_type2_cache_read(void *opaque, hwaddr addr, unsigned size)
{
    CXLType2State *ct2d = opaque;
    CXLCacheLine *line;
    uint64_t value = 0;
    uint64_t cache_line_addr = addr & ~0x3F;
    size_t offset = addr & 0x3F;

    /* Check if this is a GPU command register access */
    if (addr < CXL_GPU_CMD_REG_SIZE) {
        return cxl_type2_gpu_cmd_read(opaque, addr, size);
    }

    ct2d->stats.cpu_accesses++;

    /* Track with enhanced BAR coherency */
    if (ct2d->bar_coherency.enabled) {
        cxl_bar_coherency_request(&ct2d->bar_coherency,
                                  CXL_COH_REQ_RD_SHARED,
                                  addr, size,
                                  CXL_DOMAIN_CPU, NULL);
    }

    /* Check coherency protocol */
    line = cxl_type2_cache_lookup(ct2d, addr);

    if (line && line->state != CXL_COHERENCY_INVALID) {
        /* Cache hit */
        memcpy(&value, &line->data[offset], MIN(size, 64 - offset));

        qemu_log_mask(LOG_TRACE, "CXL Type2: Cache read hit at 0x%lx = 0x%lx\n",
                     addr, value);
    } else {
        /* Cache miss - fetch from device memory */
        uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
        if (mem_ptr && addr < ct2d->device_mem_size) {
            memcpy(&value, mem_ptr + addr, size);

            /* Insert into cache */
            uint8_t cache_data[64];
            memcpy(cache_data, mem_ptr + cache_line_addr, 64);
            cxl_type2_cache_insert(ct2d, addr, cache_data, CXL_COHERENCY_SHARED);
        }

        /* Notify CXLMemSim of cache miss */
        if (ct2d->memsim.connected) {
            CXLType2Message msg = {
                .type = CXL_T2_MSG_READ,
                .size = size,
                .addr = addr,
                .timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                .coherency_state = CXL_COHERENCY_SHARED,
                .source_id = 0,
            };

            qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                 (char *)&msg, sizeof(msg), NULL);
        }

        qemu_log_mask(LOG_TRACE, "CXL Type2: Cache read miss at 0x%lx = 0x%lx\n",
                     addr, value);
    }

    ct2d->stats.read_ops++;
    return value;
}

static void cxl_type2_cache_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    CXLType2State *ct2d = opaque;
    CXLCacheLine *line;
    uint64_t cache_line_addr = addr & ~0x3F;
    size_t offset = addr & 0x3F;

    /* Check if this is a GPU command register access */
    if (addr < CXL_GPU_CMD_REG_SIZE) {
        cxl_type2_gpu_cmd_write(opaque, addr, value, size);
        return;
    }

    ct2d->stats.cpu_accesses++;
    ct2d->stats.write_ops++;

    /* Track with enhanced BAR coherency - write requires exclusive access */
    if (ct2d->bar_coherency.enabled) {
        cxl_bar_coherency_request(&ct2d->bar_coherency,
                                  CXL_COH_REQ_WR_INV,
                                  addr, size,
                                  CXL_DOMAIN_CPU, NULL);
    }

    /* Check if we have the cache line */
    line = cxl_type2_cache_lookup(ct2d, addr);

    if (!line || line->state == CXL_COHERENCY_INVALID) {
        /* Need to fetch cache line first */
        uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
        if (mem_ptr && cache_line_addr < ct2d->device_mem_size) {
            uint8_t cache_data[64];
            memcpy(cache_data, mem_ptr + cache_line_addr, 64);
            cxl_type2_cache_insert(ct2d, addr, cache_data, CXL_COHERENCY_MODIFIED);
            line = cxl_type2_cache_lookup(ct2d, addr);
        }
    }

    if (line) {
        /* Update cache line */
        qemu_mutex_lock(&ct2d->coherency.lock);
        memcpy(&line->data[offset], &value, MIN(size, 64 - offset));
        line->state = CXL_COHERENCY_MODIFIED;
        line->dirty = true;
        line->timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        qemu_mutex_unlock(&ct2d->coherency.lock);

        /* Write through to device memory */
        uint8_t *mem_ptr = memory_region_get_ram_ptr(&ct2d->device_mem);
        if (mem_ptr && addr < ct2d->device_mem_size) {
            memcpy(mem_ptr + addr, &value, size);
        }

        /* Notify CXLMemSim of write */
        if (ct2d->memsim.connected) {
            CXLType2Message msg = {
                .type = CXL_T2_MSG_WRITE,
                .size = size,
                .addr = addr,
                .timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                .coherency_state = CXL_COHERENCY_MODIFIED,
                .source_id = 0,
            };
            memcpy(msg.data, &value, MIN(size, 64));

            qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                 (char *)&msg, sizeof(msg), NULL);
        }
    }

    qemu_log_mask(LOG_TRACE, "CXL Type2: Cache write at 0x%lx = 0x%lx\n",
                 addr, value);
}

static uint64_t cxl_type2_device_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Forward all device memory reads through the cache coherency layer */
    return cxl_type2_cache_read(opaque, addr, size);
}

static void cxl_type2_device_mem_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    /* Forward all device memory writes through the cache coherency layer */
    cxl_type2_cache_write(opaque, addr, value, size);
}

/* Component register operations - for BAR0 CXL component registers */
static uint64_t cxl_type2_component_reg_read(void *opaque, hwaddr offset, unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;

    if (offset >= CXL2_COMPONENT_CM_REGION_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                     "CXL Type2: Unimplemented component register read at 0x%lx\n",
                     offset);
        return 0;
    }

    return ldl_le_p((uint8_t *)cxl_cstate->crb.cache_mem_registers + offset);
}

static void cxl_type2_component_reg_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;

    if (offset >= CXL2_COMPONENT_CM_REGION_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                     "CXL Type2: Unimplemented component register write at 0x%lx\n",
                     offset);
        return;
    }

    stl_le_p((uint8_t *)cxl_cstate->crb.cache_mem_registers + offset, value);
}

static const MemoryRegionOps cxl_type2_component_reg_ops = {
    .read = cxl_type2_component_reg_read,
    .write = cxl_type2_component_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps cxl_type2_cache_ops = {
    .read = cxl_type2_cache_read,
    .write = cxl_type2_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps cxl_type2_device_mem_ops = {
    .read = cxl_type2_device_mem_read,
    .write = cxl_type2_device_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* ========================================================================
 * Coherent Pool Allocator
 * ======================================================================== */

/* First-fit allocator from free list, page-aligned (4KB) */
static int64_t cxl_coherent_pool_alloc(CXLType2State *ct2d, uint64_t size)
{
    uint64_t aligned_size = (size + 0xFFF) & ~0xFFFULL; /* 4KB page align */
    CXLCohFreeBlock **prev = &ct2d->coherent_pool.free_list;
    CXLCohFreeBlock *blk = ct2d->coherent_pool.free_list;

    qemu_mutex_lock(&ct2d->coherent_pool.lock);

    while (blk) {
        if (blk->size >= aligned_size) {
            uint64_t alloc_offset = blk->offset;

            if (blk->size == aligned_size) {
                /* Exact fit - remove block */
                *prev = blk->next;
                g_free(blk);
            } else {
                /* Split block */
                blk->offset += aligned_size;
                blk->size -= aligned_size;
            }

            /* Track allocation */
            uint64_t *key = g_new(uint64_t, 1);
            uint64_t *val = g_new(uint64_t, 1);
            *key = alloc_offset;
            *val = aligned_size;
            g_hash_table_insert(ct2d->coherent_pool.allocations, key, val);
            ct2d->coherent_pool.used += aligned_size;

            qemu_mutex_unlock(&ct2d->coherent_pool.lock);

            qemu_log("CXL Type2: Coherent pool alloc: offset=0x%lx size=%lu\n",
                     (unsigned long)alloc_offset, (unsigned long)aligned_size);
            return (int64_t)alloc_offset;
        }
        prev = &blk->next;
        blk = blk->next;
    }

    qemu_mutex_unlock(&ct2d->coherent_pool.lock);
    qemu_log("CXL Type2: Coherent pool alloc FAILED: size=%lu (used=%lu/%lu)\n",
             (unsigned long)size,
             (unsigned long)ct2d->coherent_pool.used,
             (unsigned long)ct2d->coherent_pool.size);
    return -1;
}

/* Free allocation and coalesce with adjacent free blocks */
static int cxl_coherent_pool_free(CXLType2State *ct2d, uint64_t offset)
{
    qemu_mutex_lock(&ct2d->coherent_pool.lock);

    uint64_t *alloc_size = g_hash_table_lookup(ct2d->coherent_pool.allocations,
                                                &offset);
    if (!alloc_size) {
        qemu_mutex_unlock(&ct2d->coherent_pool.lock);
        qemu_log("CXL Type2: Coherent pool free FAILED: offset=0x%lx not found\n",
                 (unsigned long)offset);
        return -1;
    }

    uint64_t size = *alloc_size;
    g_hash_table_remove(ct2d->coherent_pool.allocations, &offset);
    ct2d->coherent_pool.used -= size;

    /* Insert back into sorted free list with coalescing */
    CXLCohFreeBlock *new_blk = g_new0(CXLCohFreeBlock, 1);
    new_blk->offset = offset;
    new_blk->size = size;
    new_blk->next = NULL;

    CXLCohFreeBlock **prev = &ct2d->coherent_pool.free_list;
    CXLCohFreeBlock *cur = ct2d->coherent_pool.free_list;

    /* Find insertion point (sorted by offset) */
    while (cur && cur->offset < offset) {
        prev = &cur->next;
        cur = cur->next;
    }

    new_blk->next = cur;
    *prev = new_blk;

    /* Coalesce with next block */
    if (new_blk->next && new_blk->offset + new_blk->size == new_blk->next->offset) {
        CXLCohFreeBlock *merged = new_blk->next;
        new_blk->size += merged->size;
        new_blk->next = merged->next;
        g_free(merged);
    }

    /* Coalesce with previous block */
    if (prev != &ct2d->coherent_pool.free_list) {
        CXLCohFreeBlock *prev_blk = ct2d->coherent_pool.free_list;
        while (prev_blk && prev_blk->next != new_blk) {
            prev_blk = prev_blk->next;
        }
        if (prev_blk && prev_blk->offset + prev_blk->size == new_blk->offset) {
            prev_blk->size += new_blk->size;
            prev_blk->next = new_blk->next;
            g_free(new_blk);
        }
    }

    qemu_mutex_unlock(&ct2d->coherent_pool.lock);

    qemu_log("CXL Type2: Coherent pool free: offset=0x%lx size=%lu\n",
             (unsigned long)offset, (unsigned long)size);
    return 0;
}

/* ========================================================================
 * GPU Command Interface
 * ======================================================================== */

static void cxl_type2_gpu_execute_cmd(CXLType2State *ct2d, uint32_t cmd)
{
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    HetGPUError err;
    uint64_t dev_ptr;
    size_t size;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "CXL GPU: execute cmd 0x%x, hetgpu_init=%d, ctx=%p\n",
                  cmd, hetgpu->initialized, hetgpu->context);

    ct2d->gpu_cmd.cmd_status = CXL_GPU_CMD_STATUS_RUNNING;
    ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;

    switch (cmd) {
    case CXL_GPU_CMD_NOP:
        break;

    case CXL_GPU_CMD_INIT:
        if (!hetgpu->initialized) {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    case CXL_GPU_CMD_GET_DEVICE_COUNT:
        ct2d->gpu_cmd.results[0] = hetgpu->initialized ? 1 : 0;
        break;

    case CXL_GPU_CMD_GET_DEVICE:
        if (ct2d->gpu_cmd.params[0] == 0 && hetgpu->initialized) {
            ct2d->gpu_cmd.results[0] = 0; /* Device handle */
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_DEVICE;
        }
        break;

    case CXL_GPU_CMD_GET_DEVICE_PROPS:
        if (hetgpu->initialized) {
            HetGPUDeviceProps props;
            err = hetgpu_get_device_props(hetgpu, &props);
            if (err == HETGPU_SUCCESS) {
                /* Copy device name to data buffer */
                memcpy(ct2d->gpu_cmd.data, props.name, sizeof(props.name));
                ct2d->gpu_cmd.results[0] = props.total_memory;
                ct2d->gpu_cmd.results[1] = (props.compute_capability_major << 16) |
                                           props.compute_capability_minor;
                ct2d->gpu_cmd.results[2] = props.multiprocessor_count;
                ct2d->gpu_cmd.results[3] = props.max_threads_per_block;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_DEVICE;
            }
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    case CXL_GPU_CMD_GET_TOTAL_MEM:
        if (hetgpu->initialized) {
            ct2d->gpu_cmd.results[0] = hetgpu->props.total_memory;
        } else {
            ct2d->gpu_cmd.results[0] = ct2d->device_mem_size;
        }
        break;

    case CXL_GPU_CMD_CTX_CREATE:
        if (hetgpu->initialized) {
            err = hetgpu_create_context(hetgpu);
            if (err == HETGPU_SUCCESS) {
                ct2d->gpu_cmd.results[0] = (uint64_t)(uintptr_t)hetgpu->context;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_CONTEXT;
            }
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    case CXL_GPU_CMD_CTX_DESTROY:
        /* Context destroyed on device cleanup */
        break;

    case CXL_GPU_CMD_CTX_SYNC:
        if (hetgpu->initialized) {
            err = hetgpu_synchronize(hetgpu);
            if (err != HETGPU_SUCCESS) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_UNKNOWN;
            }
        }
        break;

    case CXL_GPU_CMD_MEM_ALLOC:
        size = ct2d->gpu_cmd.params[0];
        if (hetgpu->initialized) {
            err = hetgpu_malloc(hetgpu, size, HETGPU_MEM_DEFAULT, &dev_ptr);
            if (err == HETGPU_SUCCESS) {
                ct2d->gpu_cmd.results[0] = dev_ptr;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_OUT_OF_MEMORY;
            }
        } else {
            /* Fallback: allocate from device memory region */
            /* Simple bump allocator - in real impl, use proper allocator */
            static uint64_t next_alloc = 0;
            if (next_alloc + size <= ct2d->device_mem_size) {
                ct2d->gpu_cmd.results[0] = next_alloc;
                next_alloc += (size + 0xFFF) & ~0xFFF; /* Page align */
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_OUT_OF_MEMORY;
            }
        }
        break;

    case CXL_GPU_CMD_MEM_FREE:
        dev_ptr = ct2d->gpu_cmd.params[0];
        if (hetgpu->initialized) {
            err = hetgpu_free(hetgpu, dev_ptr);
            if (err != HETGPU_SUCCESS) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
        }
        /* Fallback: no-op for simple allocator */
        break;

    case CXL_GPU_CMD_MEM_COPY_HTOD:
        dev_ptr = ct2d->gpu_cmd.params[0];  /* dst device ptr */
        size = ct2d->gpu_cmd.params[1];     /* size */
        /* Data is in ct2d->gpu_cmd.data buffer */
        if (size > ct2d->gpu_cmd.data_size) {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            break;
        }
        if (hetgpu->initialized) {
            err = hetgpu_memcpy_htod(hetgpu, dev_ptr, ct2d->gpu_cmd.data, size);
            if (err != HETGPU_SUCCESS) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
            /* Also update shadow copy in device_mem for coherency tracking */
            if (dev_ptr + size <= ct2d->device_mem_size) {
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem) {
                    memcpy(mem + dev_ptr, ct2d->gpu_cmd.data, size);
                    /* Notify BAR coherency layer of GPU write */
                    if (ct2d->bar_coherency.enabled) {
                        cxl_bar_notify_gpu_access(&ct2d->bar_coherency,
                                                   dev_ptr, size, true);
                    }
                }
            }
        } else {
            /* Fallback: copy to device memory region */
            if (dev_ptr + size <= ct2d->device_mem_size) {
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem) {
                    memcpy(mem + dev_ptr, ct2d->gpu_cmd.data, size);
                    /* Notify BAR coherency layer of GPU write */
                    if (ct2d->bar_coherency.enabled) {
                        cxl_bar_notify_gpu_access(&ct2d->bar_coherency,
                                                   dev_ptr, size, true);
                    }
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
        }
        break;

    case CXL_GPU_CMD_MEM_COPY_DTOH:
        dev_ptr = ct2d->gpu_cmd.params[0];  /* src device ptr */
        size = ct2d->gpu_cmd.params[1];     /* size */
        if (size > ct2d->gpu_cmd.data_size) {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            break;
        }
        if (hetgpu->initialized) {
            /* Notify BAR coherency layer before GPU read */
            if (ct2d->bar_coherency.enabled) {
                cxl_bar_notify_gpu_access(&ct2d->bar_coherency,
                                           dev_ptr, size, false);
            }
            err = hetgpu_memcpy_dtoh(hetgpu, ct2d->gpu_cmd.data, dev_ptr, size);
            if (err != HETGPU_SUCCESS) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
            /* Update shadow copy from GPU for coherency */
            if (dev_ptr + size <= ct2d->device_mem_size) {
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem) {
                    memcpy(mem + dev_ptr, ct2d->gpu_cmd.data, size);
                }
            }
        } else {
            /* Fallback: copy from device memory region */
            if (dev_ptr + size <= ct2d->device_mem_size) {
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem) {
                    /* Notify BAR coherency layer before GPU read */
                    if (ct2d->bar_coherency.enabled) {
                        cxl_bar_notify_gpu_access(&ct2d->bar_coherency,
                                                   dev_ptr, size, false);
                    }
                    memcpy(ct2d->gpu_cmd.data, mem + dev_ptr, size);
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
        }
        break;

    case CXL_GPU_CMD_MODULE_LOAD_PTX:
        if (hetgpu->initialized && ct2d->gpu_cmd.num_modules < 64) {
            /* PTX source is in data buffer */
            void *module = NULL;
            err = hetgpu_load_ptx(hetgpu, (const char *)ct2d->gpu_cmd.data,
                                  (HetGPUModule *)&module);
            if (err == HETGPU_SUCCESS) {
                ct2d->gpu_cmd.modules[ct2d->gpu_cmd.num_modules] = module;
                ct2d->gpu_cmd.results[0] = ct2d->gpu_cmd.num_modules;
                ct2d->gpu_cmd.num_modules++;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_PTX;
            }
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    case CXL_GPU_CMD_FUNC_GET:
        if (hetgpu->initialized && ct2d->gpu_cmd.num_functions < 256) {
            uint32_t module_id = ct2d->gpu_cmd.params[0];
            /* Function name is in data buffer */
            if (module_id < ct2d->gpu_cmd.num_modules) {
                void *func = NULL;
                err = hetgpu_get_function(hetgpu,
                                          ct2d->gpu_cmd.modules[module_id],
                                          (const char *)ct2d->gpu_cmd.data,
                                          (HetGPUFunction *)&func);
                if (err == HETGPU_SUCCESS) {
                    ct2d->gpu_cmd.functions[ct2d->gpu_cmd.num_functions] = func;
                    ct2d->gpu_cmd.results[0] = ct2d->gpu_cmd.num_functions;
                    ct2d->gpu_cmd.num_functions++;
                } else {
                    ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_FOUND;
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_HANDLE;
            }
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    case CXL_GPU_CMD_LAUNCH_KERNEL:
        if (hetgpu->initialized) {
            uint32_t func_id = ct2d->gpu_cmd.params[0];
            if (func_id < ct2d->gpu_cmd.num_functions) {
                HetGPULaunchConfig config;
                config.grid_dim[0] = ct2d->gpu_cmd.params[1] & 0xFFFFFFFF;
                config.grid_dim[1] = (ct2d->gpu_cmd.params[1] >> 32) & 0xFFFFFFFF;
                config.grid_dim[2] = ct2d->gpu_cmd.params[2] & 0xFFFFFFFF;
                config.block_dim[0] = (ct2d->gpu_cmd.params[2] >> 32) & 0xFFFFFFFF;
                config.block_dim[1] = ct2d->gpu_cmd.params[3] & 0xFFFFFFFF;
                config.block_dim[2] = (ct2d->gpu_cmd.params[3] >> 32) & 0xFFFFFFFF;
                config.shared_mem_bytes = ct2d->gpu_cmd.params[4] & 0xFFFFFFFF;
                config.stream = NULL;

                /* Kernel args are in data buffer as array of pointers */
                uint32_t num_args = (ct2d->gpu_cmd.params[4] >> 32) & 0xFF;
                void **args = (void **)ct2d->gpu_cmd.data;

                err = hetgpu_launch_kernel(hetgpu,
                                           ct2d->gpu_cmd.functions[func_id],
                                           &config, args, num_args);
                if (err != HETGPU_SUCCESS) {
                    ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_LAUNCH_FAILED;
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_HANDLE;
            }
        } else {
            ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
        }
        break;

    /* Bulk transfer commands - optimized for large memory operations */
    case CXL_GPU_CMD_BULK_HTOD:
        /* Bulk host-to-device transfer using BAR4 region */
        {
            uint64_t bar4_offset = ct2d->gpu_cmd.params[0];  /* Offset in BAR4 */
            uint64_t dst_dev_ptr = ct2d->gpu_cmd.params[1];  /* Device destination */
            size_t xfer_size = ct2d->gpu_cmd.params[2];       /* Transfer size */

            if (xfer_size > CXL_GPU_BULK_TRANSFER_SIZE) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                break;
            }

            if (hetgpu->initialized) {
                /* Get data from device memory region (BAR4/HDM) */
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem && bar4_offset + xfer_size <= ct2d->device_mem_size) {
                    err = hetgpu_memcpy_htod(hetgpu, dst_dev_ptr,
                                             mem + bar4_offset, xfer_size);
                    if (err != HETGPU_SUCCESS) {
                        ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                    }
                } else {
                    ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
            }
        }
        break;

    case CXL_GPU_CMD_BULK_DTOH:
        /* Bulk device-to-host transfer using BAR4 region */
        {
            uint64_t src_dev_ptr = ct2d->gpu_cmd.params[0];   /* Device source */
            uint64_t bar4_offset = ct2d->gpu_cmd.params[1];   /* Offset in BAR4 */
            size_t xfer_size = ct2d->gpu_cmd.params[2];        /* Transfer size */

            if (xfer_size > CXL_GPU_BULK_TRANSFER_SIZE) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                break;
            }

            if (hetgpu->initialized) {
                /* Write data to device memory region (BAR4/HDM) */
                uint8_t *mem = memory_region_get_ram_ptr(&ct2d->device_mem);
                if (mem && bar4_offset + xfer_size <= ct2d->device_mem_size) {
                    err = hetgpu_memcpy_dtoh(hetgpu, mem + bar4_offset,
                                             src_dev_ptr, xfer_size);
                    if (err != HETGPU_SUCCESS) {
                        ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                    }
                } else {
                    ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
            }
        }
        break;

    case CXL_GPU_CMD_BULK_DTOD:
        /* Bulk device-to-device transfer */
        {
            uint64_t src_dev_ptr = ct2d->gpu_cmd.params[0];   /* Source device ptr */
            uint64_t dst_dev_ptr = ct2d->gpu_cmd.params[1];   /* Dest device ptr */
            size_t xfer_size = ct2d->gpu_cmd.params[2];        /* Transfer size */

            if (hetgpu->initialized) {
                err = hetgpu_memcpy_dtod(hetgpu, dst_dev_ptr, src_dev_ptr, xfer_size);
                if (err != HETGPU_SUCCESS) {
                    ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
                }
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
            }
        }
        break;

    /* CXL.cache coherency commands */
    case CXL_GPU_CMD_CACHE_FLUSH:
        /* Flush cache lines to device - notify CXLMemSim */
        {
            uint64_t flush_addr = ct2d->gpu_cmd.params[0];
            size_t flush_size = ct2d->gpu_cmd.params[1];

            if (ct2d->coherency.coherency_enabled && ct2d->memsim.connected) {
                CXLType2Message msg;
                msg.type = CXL_T2_MSG_CACHE_FLUSH;
                msg.addr = flush_addr;
                msg.size = flush_size;
                msg.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                msg.coherency_state = CXL_COHERENCY_INVALID;
                msg.source_id = 0;  /* GPU */

                qemu_mutex_lock(&ct2d->memsim.lock);
                if (ct2d->memsim.socket) {
                    qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                          (const char *)&msg, sizeof(msg), NULL);
                }
                qemu_mutex_unlock(&ct2d->memsim.lock);

                /* Invalidate local cache entries */
                for (uint64_t addr = flush_addr; addr < flush_addr + flush_size; addr += 64) {
                    cxl_type2_cache_invalidate(ct2d, addr);
                }
                ct2d->coherency.coherency_ops++;
            }
        }
        break;

    case CXL_GPU_CMD_CACHE_INVALIDATE:
        /* Invalidate cache lines */
        {
            uint64_t inv_addr = ct2d->gpu_cmd.params[0];
            size_t inv_size = ct2d->gpu_cmd.params[1];

            if (ct2d->coherency.coherency_enabled) {
                for (uint64_t addr = inv_addr; addr < inv_addr + inv_size; addr += 64) {
                    cxl_type2_cache_invalidate(ct2d, addr);
                }
                ct2d->coherency.coherency_ops++;

                /* Notify CXLMemSim if connected */
                if (ct2d->memsim.connected) {
                    CXLType2Message msg;
                    msg.type = CXL_T2_MSG_INVALIDATE;
                    msg.addr = inv_addr;
                    msg.size = inv_size;
                    msg.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    msg.coherency_state = CXL_COHERENCY_INVALID;
                    msg.source_id = 0;

                    qemu_mutex_lock(&ct2d->memsim.lock);
                    if (ct2d->memsim.socket) {
                        qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                              (const char *)&msg, sizeof(msg), NULL);
                    }
                    qemu_mutex_unlock(&ct2d->memsim.lock);
                }
            }
        }
        break;

    case CXL_GPU_CMD_CACHE_WRITEBACK:
        /* Writeback dirty cache lines */
        {
            uint64_t wb_addr = ct2d->gpu_cmd.params[0];
            size_t wb_size = ct2d->gpu_cmd.params[1];

            if (ct2d->coherency.coherency_enabled) {
                for (uint64_t addr = wb_addr; addr < wb_addr + wb_size; addr += 64) {
                    cxl_type2_cache_writeback(ct2d, addr);
                }
                ct2d->coherency.coherency_ops++;

                /* Notify CXLMemSim if connected */
                if (ct2d->memsim.connected) {
                    CXLType2Message msg;
                    msg.type = CXL_T2_MSG_WRITEBACK;
                    msg.addr = wb_addr;
                    msg.size = wb_size;
                    msg.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    msg.coherency_state = CXL_COHERENCY_SHARED;
                    msg.source_id = 0;

                    qemu_mutex_lock(&ct2d->memsim.lock);
                    if (ct2d->memsim.socket) {
                        qio_channel_write_all(QIO_CHANNEL(ct2d->memsim.socket),
                                              (const char *)&msg, sizeof(msg), NULL);
                    }
                    qemu_mutex_unlock(&ct2d->memsim.lock);
                }
            }
        }
        break;

    /* P2P DMA commands */
    case CXL_GPU_CMD_P2P_DISCOVER:
        /* Discover P2P peer devices */
        {
            int num_peers = cxl_p2p_discover_peers(&ct2d->p2p_engine);
            if (num_peers >= 0) {
                ct2d->gpu_cmd.results[0] = num_peers;
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_NOT_INITIALIZED;
            }
        }
        break;

    case CXL_GPU_CMD_P2P_GET_PEER_INFO:
        /* Get peer device info: params[0] = peer_id */
        {
            uint32_t peer_id = ct2d->gpu_cmd.params[0];
            CXLP2PPeer *peer = cxl_p2p_get_peer(&ct2d->p2p_engine, peer_id);
            if (peer && peer->active) {
                ct2d->gpu_cmd.results[0] = peer->type;
                ct2d->gpu_cmd.results[1] = peer->mem_size;
                ct2d->gpu_cmd.results[2] = peer->coherent;
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
        }
        break;

    case CXL_GPU_CMD_P2P_GPU_TO_MEM:
        /* GPU -> Type3 transfer: params[0]=peer_id, params[1]=gpu_off, params[2]=mem_off, params[3]=size */
        {
            uint32_t t3_peer_id = ct2d->gpu_cmd.params[0];
            uint64_t gpu_offset = ct2d->gpu_cmd.params[1];
            uint64_t mem_offset = ct2d->gpu_cmd.params[2];
            uint64_t xfer_size = ct2d->gpu_cmd.params[3];
            uint32_t flags = CXL_P2P_FLAG_COHERENT;

            int ret = cxl_p2p_gpu_to_mem(&ct2d->p2p_engine, t3_peer_id,
                                          gpu_offset, mem_offset, xfer_size, flags);
            ct2d->gpu_cmd.cmd_result = (ret == 0) ? CXL_GPU_SUCCESS
                                                   : CXL_GPU_ERROR_OUT_OF_MEMORY;
        }
        break;

    case CXL_GPU_CMD_P2P_MEM_TO_GPU:
        /* Type3 -> GPU transfer: params[0]=peer_id, params[1]=mem_off, params[2]=gpu_off, params[3]=size */
        {
            uint32_t t3_peer_id = ct2d->gpu_cmd.params[0];
            uint64_t mem_offset = ct2d->gpu_cmd.params[1];
            uint64_t gpu_offset = ct2d->gpu_cmd.params[2];
            uint64_t xfer_size = ct2d->gpu_cmd.params[3];
            uint32_t flags = CXL_P2P_FLAG_COHERENT;

            int ret = cxl_p2p_mem_to_gpu(&ct2d->p2p_engine, t3_peer_id,
                                          mem_offset, gpu_offset, xfer_size, flags);
            ct2d->gpu_cmd.cmd_result = (ret == 0) ? CXL_GPU_SUCCESS
                                                   : CXL_GPU_ERROR_OUT_OF_MEMORY;
        }
        break;

    case CXL_GPU_CMD_P2P_MEM_TO_MEM:
        /* Type3 -> Type3 transfer: params[0]=src_peer, params[1]=dst_peer, params[2]=src_off, params[3]=dst_off, params[4]=size */
        {
            uint32_t src_peer_id = ct2d->gpu_cmd.params[0];
            uint32_t dst_peer_id = ct2d->gpu_cmd.params[1];
            uint64_t src_offset = ct2d->gpu_cmd.params[2];
            uint64_t dst_offset = ct2d->gpu_cmd.params[3];
            uint64_t xfer_size = ct2d->gpu_cmd.params[4];
            uint32_t flags = CXL_P2P_FLAG_COHERENT;

            int ret = cxl_p2p_mem_to_mem(&ct2d->p2p_engine, src_peer_id, dst_peer_id,
                                          src_offset, dst_offset, xfer_size, flags);
            ct2d->gpu_cmd.cmd_result = (ret == 0) ? CXL_GPU_SUCCESS
                                                   : CXL_GPU_ERROR_OUT_OF_MEMORY;
        }
        break;

    case CXL_GPU_CMD_P2P_SYNC:
        /* Wait for all pending P2P transfers */
        /* Currently all transfers are synchronous, so this is a no-op */
        ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        break;

    case CXL_GPU_CMD_P2P_GET_STATUS:
        /* Get P2P engine status and stats */
        {
            ct2d->gpu_cmd.results[0] = ct2d->p2p_engine.num_peers;
            ct2d->gpu_cmd.results[1] = ct2d->p2p_engine.stats.transfers_completed;
            ct2d->gpu_cmd.results[2] = ct2d->p2p_engine.stats.bytes_transferred;
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    /* ---- Coherent shared memory pool commands ---- */
    case CXL_GPU_CMD_COHERENT_ALLOC:
        {
            uint64_t alloc_size = ct2d->gpu_cmd.params[0];
            int64_t offset = cxl_coherent_pool_alloc(ct2d, alloc_size);
            if (offset >= 0) {
                ct2d->gpu_cmd.results[0] = (uint64_t)offset;
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_OUT_OF_MEMORY;
            }
        }
        break;

    case CXL_GPU_CMD_COHERENT_FREE:
        {
            uint64_t free_offset = ct2d->gpu_cmd.params[0];
            if (cxl_coherent_pool_free(ct2d, free_offset) == 0) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            } else {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            }
        }
        break;

    case CXL_GPU_CMD_COHERENT_GET_INFO:
        {
            ct2d->gpu_cmd.results[0] = ct2d->coherent_pool.base_offset;
            ct2d->gpu_cmd.results[1] = ct2d->coherent_pool.size;
            ct2d->gpu_cmd.results[2] = ct2d->coherent_pool.size -
                                        ct2d->coherent_pool.used;
            ct2d->gpu_cmd.results[3] = ct2d->bar_coherency.snoop_filter_size;
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    case CXL_GPU_CMD_COHERENT_FENCE:
        {
            /* Memory fence - ensure all pending coherency ops complete */
            cxl_bar_memory_fence(&ct2d->bar_coherency, CXL_DOMAIN_CPU);
            cxl_bar_process_back_invalidations(ct2d);
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    /* ---- Device-biased directory commands ---- */
    case CXL_GPU_CMD_SET_BIAS:
        {
            uint64_t bias_addr = ct2d->gpu_cmd.params[0];
            uint64_t bias_size = ct2d->gpu_cmd.params[1];
            uint8_t bias_mode = (uint8_t)ct2d->gpu_cmd.params[2];
            if (bias_mode > CXL_BIAS_DEVICE) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            } else {
                cxl_bar_set_bias(&ct2d->bar_coherency, bias_addr, bias_size,
                                 bias_mode);
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            }
        }
        break;

    case CXL_GPU_CMD_GET_BIAS:
        {
            uint64_t query_addr = ct2d->gpu_cmd.params[0];
            ct2d->gpu_cmd.results[0] = cxl_bar_get_bias(&ct2d->bar_coherency,
                                                          query_addr);
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    case CXL_GPU_CMD_BIAS_FLIP:
        {
            uint64_t flip_addr = ct2d->gpu_cmd.params[0];
            uint64_t flip_size = ct2d->gpu_cmd.params[1];
            uint8_t new_bias = (uint8_t)ct2d->gpu_cmd.params[2];
            if (new_bias > CXL_BIAS_DEVICE) {
                ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
            } else {
                cxl_bar_bias_flip(ct2d, flip_addr, flip_size, new_bias);
                ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
            }
        }
        break;

    /* ---- Coherency statistics commands ---- */
    case CXL_GPU_CMD_COH_GET_STATS:
        {
            CXLBARCoherencyState *coh = &ct2d->bar_coherency;
            /* Pack stats into results and data buffer */
            ct2d->gpu_cmd.results[0] = coh->stats.snoop_hits;
            ct2d->gpu_cmd.results[1] = coh->stats.snoop_misses;
            ct2d->gpu_cmd.results[2] = coh->stats.coherency_requests;
            ct2d->gpu_cmd.results[3] = coh->stats.back_invalidations;
            /* Extended stats in data buffer */
            if (ct2d->gpu_cmd.data_size >= 64) {
                uint64_t *stats_buf = (uint64_t *)ct2d->gpu_cmd.data;
                stats_buf[0] = coh->stats.writebacks;
                stats_buf[1] = coh->stats.evictions;
                stats_buf[2] = coh->stats.bias_flips;
                stats_buf[3] = coh->stats.device_bias_hits;
                stats_buf[4] = coh->stats.host_bias_hits;
                stats_buf[5] = coh->stats.upgrades;
                stats_buf[6] = coh->stats.downgrades;
                stats_buf[7] = coh->snoop_filter_size;
            }
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    case CXL_GPU_CMD_COH_RESET_STATS:
        {
            memset(&ct2d->bar_coherency.stats, 0,
                   sizeof(ct2d->bar_coherency.stats));
            ct2d->gpu_cmd.cmd_result = CXL_GPU_SUCCESS;
        }
        break;

    default:
        ct2d->gpu_cmd.cmd_result = CXL_GPU_ERROR_INVALID_VALUE;
        break;
    }

    ct2d->gpu_cmd.cmd_status = CXL_GPU_CMD_STATUS_COMPLETE;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "CXL GPU: cmd 0x%x done, result=%u results[0]=0x%lx\n",
                  cmd, ct2d->gpu_cmd.cmd_result,
                  (unsigned long)ct2d->gpu_cmd.results[0]);
}

static uint64_t cxl_type2_gpu_cmd_read(void *opaque, hwaddr addr, unsigned size)
{
    CXLType2State *ct2d = opaque;
    HetGPUState *hetgpu = &ct2d->gpu_info.hetgpu_state;
    uint64_t value = 0;

    switch (addr) {
    case CXL_GPU_REG_MAGIC:
        value = CXL_GPU_MAGIC;
        break;
    case CXL_GPU_REG_VERSION:
        value = CXL_GPU_VERSION;
        break;
    case CXL_GPU_REG_STATUS:
        value = ct2d->gpu_cmd.status;
        if (hetgpu->initialized) {
            value |= CXL_GPU_STATUS_READY;
        }
        if (hetgpu->context) {
            value |= CXL_GPU_STATUS_CTX_ACTIVE;
        }
        break;
    case CXL_GPU_REG_CAPS:
        value = ct2d->gpu_cmd.capabilities;
        break;
    case CXL_GPU_REG_CMD_STATUS:
        value = ct2d->gpu_cmd.cmd_status;
        // fprintf(stderr, "CXL GPU: read CMD_STATUS = %lu\n", (unsigned long)value);
        break;
    case CXL_GPU_REG_CMD_RESULT:
        value = ct2d->gpu_cmd.cmd_result;
        break;
    case CXL_GPU_REG_RESULT0:
        value = ct2d->gpu_cmd.results[0];
        // fprintf(stderr, "CXL GPU: read RESULT0 = 0x%lx\n", (unsigned long)value);
        break;
    case CXL_GPU_REG_RESULT1:
        value = ct2d->gpu_cmd.results[1];
        break;
    case CXL_GPU_REG_RESULT2:
        value = ct2d->gpu_cmd.results[2];
        break;
    case CXL_GPU_REG_RESULT3:
        value = ct2d->gpu_cmd.results[3];
        break;
    case CXL_GPU_REG_TOTAL_MEM:
        if (hetgpu->initialized && hetgpu->props.total_memory > 0) {
            value = hetgpu->props.total_memory;
        } else {
            value = ct2d->device_mem_size;
        }
        // fprintf(stderr, "CXL GPU: read TOTAL_MEM = 0x%lx (%lu MB)\n",
        //         (unsigned long)value, (unsigned long)(value / (1024*1024)));
        break;
    case CXL_GPU_REG_FREE_MEM:
        /* Report device memory as free memory */
        value = ct2d->device_mem_size;
        break;
    case CXL_GPU_REG_CC_MAJOR:
        value = hetgpu->initialized ? hetgpu->props.compute_capability_major : 8;
        break;
    case CXL_GPU_REG_CC_MINOR:
        value = hetgpu->initialized ? hetgpu->props.compute_capability_minor : 0;
        break;
    case CXL_GPU_REG_MP_COUNT:
        value = hetgpu->initialized ? hetgpu->props.multiprocessor_count : 80;
        break;
    case CXL_GPU_REG_MAX_THREADS:
        value = hetgpu->initialized ? hetgpu->props.max_threads_per_block : 1024;
        break;
    case CXL_GPU_REG_WARP_SIZE:
        value = hetgpu->initialized ? hetgpu->props.warp_size : 32;
        break;
    case CXL_GPU_REG_BACKEND:
        value = hetgpu->backend;
        break;
    /* Coherent pool registers */
    case CXL_GPU_REG_COH_POOL_BASE:
        value = ct2d->coherent_pool.base_offset;
        break;
    case CXL_GPU_REG_COH_POOL_SIZE:
        value = ct2d->coherent_pool.size;
        break;
    case CXL_GPU_REG_COH_POOL_FREE:
        value = ct2d->coherent_pool.size - ct2d->coherent_pool.used;
        break;
    case CXL_GPU_REG_COH_DIR_SIZE:
        value = ct2d->bar_coherency.snoop_filter_capacity;
        break;
    case CXL_GPU_REG_COH_DIR_USED:
        value = ct2d->bar_coherency.snoop_filter_size;
        break;

    default:
        /* Data region */
        if (addr >= CXL_GPU_DATA_OFFSET &&
            addr < CXL_GPU_DATA_OFFSET + CXL_GPU_DATA_SIZE) {
            size_t offset = addr - CXL_GPU_DATA_OFFSET;
            if (offset + size <= ct2d->gpu_cmd.data_size) {
                memcpy(&value, &ct2d->gpu_cmd.data[offset], MIN(size, 8));
            }
        } else if (addr >= CXL_GPU_REG_DEV_NAME &&
                   addr < CXL_GPU_REG_DEV_NAME + 64) {
            /* Device name */
            size_t offset = addr - CXL_GPU_REG_DEV_NAME;
            if (hetgpu->initialized) {
                memcpy(&value, &hetgpu->props.name[offset], MIN(size, 8));
            }
        }
        break;
    }

    return value;
}

static void cxl_type2_gpu_cmd_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    CXLType2State *ct2d = opaque;

    switch (addr) {
    case CXL_GPU_REG_CMD:
        /* Execute command */
        cxl_type2_gpu_execute_cmd(ct2d, value);
        break;
    case CXL_GPU_REG_PARAM0:
        ct2d->gpu_cmd.params[0] = value;
        break;
    case CXL_GPU_REG_PARAM1:
        ct2d->gpu_cmd.params[1] = value;
        break;
    case CXL_GPU_REG_PARAM2:
        ct2d->gpu_cmd.params[2] = value;
        break;
    case CXL_GPU_REG_PARAM3:
        ct2d->gpu_cmd.params[3] = value;
        break;
    case CXL_GPU_REG_PARAM4:
        ct2d->gpu_cmd.params[4] = value;
        break;
    case CXL_GPU_REG_PARAM5:
        ct2d->gpu_cmd.params[5] = value;
        break;
    case CXL_GPU_REG_PARAM6:
        ct2d->gpu_cmd.params[6] = value;
        break;
    case CXL_GPU_REG_PARAM7:
        ct2d->gpu_cmd.params[7] = value;
        break;
    default:
        /* Data region */
        if (addr >= CXL_GPU_DATA_OFFSET &&
            addr < CXL_GPU_DATA_OFFSET + CXL_GPU_DATA_SIZE) {
            size_t offset = addr - CXL_GPU_DATA_OFFSET;
            if (offset + size <= ct2d->gpu_cmd.data_size) {
                memcpy(&ct2d->gpu_cmd.data[offset], &value, MIN(size, 8));
            }
        }
        break;
    }
}

/* GPU command ops handled directly in cache_read/cache_write */

/* ========================================================================
 * DVSEC Configuration
 * ======================================================================== */

static void build_dvsecs(CXLType2State *ct2d)
{
    CXLComponentState *cxl_cstate = &ct2d->cxl_cstate;
    uint8_t *dvsec;

    /* Type 2 Device DVSEC - includes both cache and memory capabilities */
    dvsec = (uint8_t *)&(CXLDVSECDevice){
        .cap = 0x1f,  /* Cache+, IO+, Mem+, Mem HWInit+, HDMCount=1 */
        .ctrl = 0x7,  /* Cache+, IO+, Mem+ enabled */
        .status = 0,
        .ctrl2 = 0,
        .status2 = 0x2,
        .lock = 0,
        .cap2 = (ct2d->cache_size >> 20) & 0xFFFF,  /* Cache size in MB */
        .range1_size_hi = ct2d->cache_size >> 32,
        .range1_size_lo = (ct2d->cache_size & 0xFFFFFFF0) | 0x3,  /* Cache: Valid, Active */
        .range1_base_hi = 0,
        .range1_base_lo = 0,
        .range2_size_hi = ct2d->device_mem_size >> 32,
        .range2_size_lo = (ct2d->device_mem_size & 0xFFFFFFF0) | 0x1,  /* Mem: Valid */
        .range2_base_hi = 0,
        .range2_base_lo = 0,
    };

    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                              PCIE_CXL_DEVICE_DVSEC_LENGTH,
                              PCIE_CXL_DEVICE_DVSEC,
                              PCIE_CXL31_DEVICE_DVSEC_REVID,
                              dvsec);

    /* Register Locator DVSEC
     * Type 2 devices only have component registers in BAR0
     * BAR2 is used for cache memory, not CXL device registers
     */
    dvsec = (uint8_t *)&(CXLDVSECRegisterLocator){
        .rsvd = 0,
        .reg0_base_lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg0_base_hi = 0,
        .reg1_base_lo = RBI_EMPTY,  /* No device registers - Type 2 uses cache memory at BAR2 */
        .reg1_base_hi = 0,
    };

    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                              REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                              REG_LOC_DVSEC_REVID, dvsec);

    /* FlexBus Port DVSEC */
    dvsec = (uint8_t *)&(CXLDVSECPortFlexBus){
        .cap = 0x26,
        .ctrl = 0x02,
        .status = 0x26,
        .rcvd_mod_ts_data_phase1 = 0xef,
    };

    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                              PCIE_CXL3_FLEXBUS_PORT_DVSEC_LENGTH,
                              PCIE_FLEXBUS_PORT_DVSEC,
                              PCIE_CXL3_FLEXBUS_PORT_DVSEC_REVID, dvsec);
}

/* ========================================================================
 * Device Lifecycle
 * ======================================================================== */

static void cxl_type2_reset(DeviceState *dev)
{
    CXLType2State *ct2d = CXL_TYPE2(dev);
    CXLComponentState *cxl_cstate = &ct2d->cxl_cstate;
    uint32_t *reg_state = cxl_cstate->crb.cache_mem_registers;
    uint32_t *write_msk = cxl_cstate->crb.cache_mem_regs_write_mask;

    cxl_component_register_init_common(reg_state, write_msk, CXL2_TYPE3_DEVICE);

    /* Reset statistics */
    memset(&ct2d->stats, 0, sizeof(ct2d->stats));

    qemu_log("CXL Type2: Device reset\n");
}

static void cxl_type2_realize(PCIDevice *pci_dev, Error **errp)
{
    CXLType2State *ct2d = CXL_TYPE2(pci_dev);
    CXLComponentState *cxl_cstate = &ct2d->cxl_cstate;
    Error *local_err = NULL;

    pci_config_set_prog_interface(pci_dev->config, 0x10);

    /* Set default values */
    if (!ct2d->memsim.server_addr) {
        ct2d->memsim.server_addr = g_strdup("127.0.0.1");
    }
    if (ct2d->memsim.server_port == 0) {
        ct2d->memsim.server_port = 9999;
    }
    if (ct2d->cache_size == 0) {
        ct2d->cache_size = CXL_TYPE2_DEFAULT_CACHE_SIZE;
    }
    if (ct2d->device_mem_size == 0) {
        ct2d->device_mem_size = CXL_TYPE2_DEFAULT_MEM_SIZE;
    }

    /* Initialize coherency protocol */
    cxl_type2_coherency_init(ct2d);

    /* Initialize enhanced BAR coherency tracking */
    cxl_bar_coherency_init(&ct2d->bar_coherency);

    /* Initialize P2P DMA engine */
    cxl_p2p_dma_init(&ct2d->p2p_engine, ct2d);

    /* Initialize CXLMemSim connection */
    qemu_mutex_init(&ct2d->memsim.lock);

    /* Setup PCIe capabilities */
    pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ct2d->sn != 0) {
        pcie_dev_ser_num_init(pci_dev, 0x100, ct2d->sn);
        cxl_cstate->dvsec_offset = 0x100 + 0x0c;
    } else {
        cxl_cstate->dvsec_offset = 0x100;
    }

    ct2d->cxl_cstate.pdev = pci_dev;
    build_dvsecs(ct2d);

    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_CXL_TYPE2);

    /* BAR0: Component registers */
    memory_region_init(&ct2d->bar0, OBJECT(ct2d), "cxl-type2-bar0",
                      CXL2_COMPONENT_BLOCK_SIZE);

    memory_region_init_io(&ct2d->component_registers, OBJECT(ct2d),
                         &cxl_type2_component_reg_ops, cxl_cstate,
                         "cxl-type2-component",
                         CXL2_COMPONENT_CM_REGION_SIZE);
    memory_region_add_subregion(&ct2d->bar0, 0, &ct2d->component_registers);

    pci_register_bar(pci_dev, 0,
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_TYPE_64,
                    &ct2d->bar0);

    /* BAR2: Cache memory region (Type 1 feature) */
    memory_region_init_ram(&ct2d->cache_mem, OBJECT(ct2d),
                          "cxl-type2-cache", ct2d->cache_size, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    memory_region_init_io(&ct2d->cache_io, OBJECT(ct2d),
                         &cxl_type2_cache_ops, ct2d,
                         "cxl-type2-cache-io", ct2d->cache_size);

    memory_region_add_subregion_overlap(&ct2d->cache_mem, 0, &ct2d->cache_io, 1);

    pci_register_bar(pci_dev, 2,
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_TYPE_64 |
                    PCI_BASE_ADDRESS_MEM_PREFETCH,
                    &ct2d->cache_mem);

    /* BAR4: Device-attached memory (Type 3 feature) */
    memory_region_init_ram(&ct2d->device_mem, OBJECT(ct2d),
                          "cxl-type2-device-mem", ct2d->device_mem_size, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    memory_region_init_io(&ct2d->device_mem_io, OBJECT(ct2d),
                         &cxl_type2_device_mem_ops, ct2d,
                         "cxl-type2-device-mem-io", ct2d->device_mem_size);

    memory_region_add_subregion_overlap(&ct2d->device_mem, 0, &ct2d->device_mem_io, 1);

    pci_register_bar(pci_dev, 4,
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_TYPE_64 |
                    PCI_BASE_ADDRESS_MEM_PREFETCH,
                    &ct2d->device_mem);

    /* Register BAR regions for enhanced coherency tracking */
    cxl_bar_coherency_add_region(&ct2d->bar_coherency, 2, 0, ct2d->cache_size,
                                  true,   /* GPU accessible */
                                  true);  /* CPU accessible */
    cxl_bar_coherency_add_region(&ct2d->bar_coherency, 4, 0, ct2d->device_mem_size,
                                  true,   /* GPU accessible */
                                  true);  /* CPU accessible */

    /* Initialize GPU command state (handled directly in cache_read/write) */
    memset(&ct2d->gpu_cmd, 0, sizeof(ct2d->gpu_cmd));
    /* NOTE: Do NOT set CXL_GPU_STATUS_READY here - it will be set dynamically
     * in the register read handler based on hetgpu->initialized.
     * Setting it here unconditionally would make cuInit succeed even when
     * the GPU backend failed to initialize. */
    ct2d->gpu_cmd.status = 0;
    ct2d->gpu_cmd.cmd_status = CXL_GPU_CMD_STATUS_IDLE;

    /* Allocate larger data buffer for optimized transfers (1MB) */
    ct2d->gpu_cmd.data_size = CXL_GPU_DATA_SIZE;  /* 1MB */
    ct2d->gpu_cmd.data = g_malloc0(ct2d->gpu_cmd.data_size);
    if (!ct2d->gpu_cmd.data) {
        error_setg(errp, "Failed to allocate GPU command data buffer");
        return;
    }

    /* Set capabilities */
    ct2d->gpu_cmd.capabilities = CXL_GPU_CAP_BULK_TRANSFER |
                                 CXL_GPU_CAP_CACHE_COHERENT |
                                 CXL_GPU_CAP_COHERENT_POOL |
                                 CXL_GPU_CAP_DEVICE_BIAS;

    /* Initialize coherent shared memory pool at top of BAR4 */
    {
        uint64_t coh_pool_size = 256 * MiB; /* Default 256MB */
        if (coh_pool_size > ct2d->device_mem_size / 2) {
            coh_pool_size = ct2d->device_mem_size / 4;
        }
        ct2d->coherent_pool.size = coh_pool_size;
        ct2d->coherent_pool.base_offset = ct2d->device_mem_size - coh_pool_size;
        ct2d->coherent_pool.used = 0;
        ct2d->coherent_pool.allocations = g_hash_table_new(g_int64_hash,
                                                            g_int64_equal);
        /* Initialize free list with single block spanning the whole pool */
        CXLCohFreeBlock *initial = g_new0(CXLCohFreeBlock, 1);
        initial->offset = ct2d->coherent_pool.base_offset;
        initial->size = coh_pool_size;
        initial->next = NULL;
        ct2d->coherent_pool.free_list = initial;
        qemu_mutex_init(&ct2d->coherent_pool.lock);

        qemu_log("CXL Type2: Coherent pool initialized: base=0x%lx size=%lu MB\n",
                 (unsigned long)ct2d->coherent_pool.base_offset,
                 (unsigned long)(coh_pool_size / MiB));
    }

    qemu_log("CXL Type2: GPU command interface enabled at BAR2 offset 0\n");
    qemu_log("CXL Type2: Data buffer size: %zu KB (optimized for large transfers)\n",
             ct2d->gpu_cmd.data_size / 1024);

    /* Initialize MSI-X */
    if (msix_init_exclusive_bar(pci_dev, 16, 6, NULL)) {
        error_setg(errp, "Failed to initialize MSI-X");
        return;
    }

    /* Initialize GPU passthrough */
    if (cxl_type2_gpu_init(ct2d, &local_err) < 0) {
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }

    /* Connect to CXLMemSim */
    cxlmemsim_connect(ct2d);
    if (ct2d->memsim.connected && !ct2d->memsim.use_shm) {
        qemu_thread_create(&ct2d->memsim.recv_thread, "cxlmemsim-recv",
                          cxlmemsim_recv_thread, ct2d, QEMU_THREAD_JOINABLE);
    }

    qemu_log("CXL Type2: Device realized - Cache: %zu MB, DevMem: %zu MB\n",
             ct2d->cache_size / MiB, ct2d->device_mem_size / MiB);
}

static void cxl_type2_exit(PCIDevice *pci_dev)
{
    CXLType2State *ct2d = CXL_TYPE2(pci_dev);

    /* Disconnect from CXLMemSim */
    cxlmemsim_disconnect(ct2d);
    if (ct2d->memsim.recv_thread.thread) {
        qemu_thread_join(&ct2d->memsim.recv_thread);
    }
    qemu_mutex_destroy(&ct2d->memsim.lock);

    /* Cleanup GPU passthrough */
    cxl_type2_gpu_cleanup(ct2d);

    /* Cleanup coherency protocol */
    cxl_type2_coherency_cleanup(ct2d);

    /* Cleanup enhanced BAR coherency tracking */
    cxl_bar_coherency_cleanup(&ct2d->bar_coherency);

    /* Cleanup P2P DMA engine */
    cxl_p2p_dma_cleanup(&ct2d->p2p_engine);

    /* Cleanup coherent pool */
    if (ct2d->coherent_pool.allocations) {
        g_hash_table_destroy(ct2d->coherent_pool.allocations);
        ct2d->coherent_pool.allocations = NULL;
    }
    {
        CXLCohFreeBlock *blk = ct2d->coherent_pool.free_list;
        while (blk) {
            CXLCohFreeBlock *next = blk->next;
            g_free(blk);
            blk = next;
        }
        ct2d->coherent_pool.free_list = NULL;
    }
    qemu_mutex_destroy(&ct2d->coherent_pool.lock);

    /* Free GPU command data buffer */
    if (ct2d->gpu_cmd.data) {
        g_free(ct2d->gpu_cmd.data);
        ct2d->gpu_cmd.data = NULL;
    }

    /* Free bulk transfer region if allocated */
    if (ct2d->bulk_transfer_ptr) {
        g_free(ct2d->bulk_transfer_ptr);
        ct2d->bulk_transfer_ptr = NULL;
    }

    qemu_log("CXL Type2: Device exit complete\n");
}

static const Property cxl_type2_props[] = {
    DEFINE_PROP_SIZE("cache-size", CXLType2State, cache_size,
                     CXL_TYPE2_DEFAULT_CACHE_SIZE),
    DEFINE_PROP_SIZE("mem-size", CXLType2State, device_mem_size,
                     CXL_TYPE2_DEFAULT_MEM_SIZE),
    DEFINE_PROP_UINT64("sn", CXLType2State, sn, 0),
    DEFINE_PROP_STRING("cxlmemsim-addr", CXLType2State, memsim.server_addr),
    DEFINE_PROP_UINT16("cxlmemsim-port", CXLType2State, memsim.server_port, 9999),
    DEFINE_PROP_STRING("gpu-device", CXLType2State, gpu_info.vfio_device),
    DEFINE_PROP_BOOL("coherency-enabled", CXLType2State,
                     coherency.coherency_enabled, true),
    /* hetGPU backend configuration */
    DEFINE_PROP_UINT32("gpu-mode", CXLType2State, gpu_info.mode,
                       CXL_TYPE2_GPU_MODE_AUTO),
    DEFINE_PROP_STRING("hetgpu-lib", CXLType2State, gpu_info.hetgpu_lib_path),
    DEFINE_PROP_INT32("hetgpu-device", CXLType2State, gpu_info.hetgpu_device_index, 0),
    DEFINE_PROP_UINT32("hetgpu-backend", CXLType2State, gpu_info.hetgpu_backend,
                       HETGPU_BACKEND_AUTO),
};

static void cxl_type2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = cxl_type2_realize;
    pc->exit = cxl_type2_exit;
    pc->vendor_id = CXL_TYPE2_VENDOR_ID;
    pc->device_id = CXL_TYPE2_DEVICE_ID;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_MEMORY_CXL;

    dc->desc = "CXL Type 2 Accelerator Device with Coherent Memory (GPU Passthrough)";
    device_class_set_legacy_reset(dc, cxl_type2_reset);
    device_class_set_props(dc, cxl_type2_props);
}

static const TypeInfo cxl_type2_info = {
    .name = TYPE_CXL_TYPE2,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CXLType2State),
    .class_init = cxl_type2_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CXL_DEVICE },
        { }
    },
};

static void cxl_type2_register_types(void)
{
    type_register_static(&cxl_type2_info);
}

type_init(cxl_type2_register_types)
