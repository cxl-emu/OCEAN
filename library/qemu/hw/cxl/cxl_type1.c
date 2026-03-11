/*
 * CXL Type 1 Device (Accelerator with Cache)
 * Forwards virtio accelerator requests to CXLMemSim server
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
#include "qapi/error.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_device.h"
#include "hw/cxl/cxl_component.h"
#include "hw/cxl/cxl_cdat.h"
#include "hw/cxl/cxl_pci.h"
#include "hw/cxl/cxl_type1.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_sriov.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/resettable.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-crypto.h"
#include "io/channel-socket.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "system/memory.h"

#define TYPE_CXL_TYPE1_DEV "cxl-type1"
#define CXL_T1_VENDOR_ID 0x8086
#define CXL_T1_DEVICE_ID 0x0d90

/* Extended CXL Type 1 State with implementation details */
typedef struct CXLType1StateImpl {
    PCIDevice parent_obj;
    
    CXLComponentState cxl_cstate;
    CXLDeviceState cxl_dstate;
    
    struct {
        uint32_t cache_mem;
    } reg_state;
    
    MemoryRegion device_registers;
    MemoryRegion component_registers;
    MemoryRegion bar0;
    MemoryRegion cache_mem;
    MemoryRegion cache_io;
    
    struct {
        QIOChannelSocket *socket;
        char *server_addr;
        uint16_t server_port;
        QemuThread recv_thread;
        bool connected;
        QemuMutex lock;
    } cxlmemsim;
    
    struct {
        VirtQueue *dataq;
        VirtQueue *ctrlq;
        uint32_t max_queues;
        bool enabled;
    } virtio_accel;
    
    uint64_t device_size;
    uint64_t cache_size;
    uint64_t sn;
    
    struct {
        char *host;
        uint16_t port;
        void *socket_unused;
        bool connected_unused;
    } memsim_server;
    
    bool latency_enabled;
    uint32_t read_latency_ns;
    uint32_t write_latency_ns;
} CXLType1StateImpl;

#define CXL_TYPE1_DEV(obj) \
    OBJECT_CHECK(CXLType1StateImpl, (obj), TYPE_CXL_TYPE1_DEV)

typedef struct CXLMemSimMessage {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint8_t data[];
} CXLMemSimMessage;

enum CXLMemSimMsgType {
    CXLMEMSIM_MSG_READ = 1,
    CXLMEMSIM_MSG_WRITE,
    CXLMEMSIM_MSG_CRYPTO_OP,
    CXLMEMSIM_MSG_CACHE_FLUSH,
    CXLMEMSIM_MSG_RESPONSE,
};


static void build_dvsecs(CXLType1StateImpl *ct1d)
{
    CXLComponentState *cxl_cstate = &ct1d->cxl_cstate;
    uint8_t *dvsec;
    
    dvsec = (uint8_t *)&(CXLDVSECDevice){
        .cap = 0x1f,  /* Bit 0: Cache+, Bit 1: IO+, Bit 2: Mem+, Bit 3: Mem HWInit+, Bit 4: HDMCount=1 */
        .ctrl = 0x7,  /* Cache+ IO+ Mem+ enabled */
        .status = 0,
        .ctrl2 = 0,
        .status2 = 0x2,
        .lock = 0,
        .cap2 = (ct1d->cache_size >> 20) & 0xFFFF,  /* Cache size in MB for cap2 field */
        .range1_size_hi = ct1d->cache_size >> 32,
        .range1_size_lo = (ct1d->cache_size & 0xFFFFFFF0) | 0x3,  /* Valid, Active, Type=Cache */
        .range1_base_hi = 0,
        .range1_base_lo = 0,
        .range2_size_hi = ct1d->device_size >> 32,
        .range2_size_lo = (ct1d->device_size & 0xFFFFFFF0) | 0x1,  /* Valid, Type=Volatile */
        .range2_base_hi = 0,
        .range2_base_lo = 0,
    };
    
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                              PCIE_CXL_DEVICE_DVSEC_LENGTH,
                              PCIE_CXL_DEVICE_DVSEC,
                              PCIE_CXL31_DEVICE_DVSEC_REVID,
                              dvsec);
    
    dvsec = (uint8_t *)&(CXLDVSECRegisterLocator){
        .rsvd = 0,
        .reg0_base_lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg0_base_hi = 0,
        .reg1_base_lo = RBI_CXL_DEVICE_REG | CXL_DEVICE_REG_BAR_IDX,
        .reg1_base_hi = 0,
    };
    
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                              REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                              REG_LOC_DVSEC_REVID, dvsec);
    
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

static void cxl_type1_reset(DeviceState *dev)
{
    /* Reset function for CXL Type 1 device */
    CXLType1StateImpl *ct1d = CXL_TYPE1_DEV(dev);
    CXLComponentState *cxl_cstate = &ct1d->cxl_cstate;
    uint32_t *reg_state = cxl_cstate->crb.cache_mem_registers;
    uint32_t *write_msk = cxl_cstate->crb.cache_mem_regs_write_mask;
    
    /* Initialize component registers */
    cxl_component_register_init_common(reg_state, write_msk, CXL2_TYPE3_DEVICE);
}

static void cxlmemsim_connect(CXLType1StateImpl *ct1d)
{
    Error *err = NULL;
    SocketAddress addr;

    if (ct1d->cxlmemsim.connected) {
        return;
    }

    /* Check if SHM transport mode is configured - skip TCP connection */
    const char *transport = getenv("CXL_TRANSPORT_MODE");
    if (!transport || !transport[0]) {
        transport = getenv("CXL_MEMSIM_TRANSPORT");
    }

    qemu_log("CXL Type1: Transport mode = %s\n", transport ? transport : "(not set)");

    if (transport && (strcmp(transport, "shm") == 0 || strcmp(transport, "pgas") == 0)) {
        /* SHM mode - Type3 device handles connection, Type1 doesn't need TCP */
        qemu_log("CXL Type1: Using SHM transport mode - skipping TCP connection\n");
        return;
    }

    qemu_log("CXL Type1: Using TCP transport mode\n");

    addr.type = SOCKET_ADDRESS_TYPE_INET;
    addr.u.inet.host = ct1d->cxlmemsim.server_addr;
    addr.u.inet.port = g_strdup_printf("%u", ct1d->cxlmemsim.server_port);

    ct1d->cxlmemsim.socket = qio_channel_socket_new();
    if (qio_channel_socket_connect_sync(ct1d->cxlmemsim.socket, &addr, &err) < 0) {
        qemu_log("Warning: Failed to connect to CXLMemSim server at %s:%s: %s\n",
                addr.u.inet.host, addr.u.inet.port, error_get_pretty(err));
        error_free(err);
        g_free(addr.u.inet.port);
        object_unref(OBJECT(ct1d->cxlmemsim.socket));
        ct1d->cxlmemsim.socket = NULL;
        return;
    }

    ct1d->cxlmemsim.connected = true;
    g_free(addr.u.inet.port);
    qemu_log("Connected to CXLMemSim server at %s:%u\n",
            ct1d->cxlmemsim.server_addr, ct1d->cxlmemsim.server_port);
}

static void cxlmemsim_disconnect(CXLType1StateImpl *ct1d)
{
    if (!ct1d->cxlmemsim.connected) {
        return;
    }
    
    ct1d->cxlmemsim.connected = false;
    if (ct1d->cxlmemsim.socket) {
        qio_channel_close(QIO_CHANNEL(ct1d->cxlmemsim.socket), NULL);
        object_unref(OBJECT(ct1d->cxlmemsim.socket));
        ct1d->cxlmemsim.socket = NULL;
    }
}


static void *cxlmemsim_recv_thread(void *opaque)
{
    CXLType1StateImpl *ct1d = opaque;
    CXLMemSimMessage header;
    Error *err = NULL;
    
    while (ct1d->cxlmemsim.connected) {
        if (qio_channel_read_all(QIO_CHANNEL(ct1d->cxlmemsim.socket),
                                 (char *)&header, sizeof(header), &err) < 0) {
            if (ct1d->cxlmemsim.connected) {
                error_report("Failed to receive from CXLMemSim: %s",
                           error_get_pretty(err));
                error_free(err);
            }
            break;
        }
        
        if (header.type == CXLMEMSIM_MSG_RESPONSE && header.size > 0) {
            uint8_t *data = g_malloc(header.size);
            if (qio_channel_read_all(QIO_CHANNEL(ct1d->cxlmemsim.socket),
                                    (char *)data, header.size, &err) < 0) {
                error_report("Failed to receive data from CXLMemSim: %s",
                           error_get_pretty(err));
                error_free(err);
                g_free(data);
                break;
            }
            
            g_free(data);
        }
    }
    
    return NULL;
}



static void ct1_reg_write(void *opaque, hwaddr offset, uint64_t value, 
                         unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;
    
    if (offset >= CXL2_COMPONENT_CM_REGION_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                     "CXL Type1: Unimplemented register write at 0x%lx\n",
                     offset);
        return;
    }
    
    stl_le_p((uint8_t *)cxl_cstate->crb.cache_mem_registers + offset, value);
}

static uint64_t ct1_reg_read(void *opaque, hwaddr offset, unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;
    
    if (offset >= CXL2_COMPONENT_CM_REGION_SIZE) {
        qemu_log_mask(LOG_UNIMP,
                     "CXL Type1: Unimplemented register read at 0x%lx\n",
                     offset);
        return 0;
    }
    
    return ldl_le_p((uint8_t *)cxl_cstate->crb.cache_mem_registers + offset);
}

static const MemoryRegionOps ct1_reg_ops = {
    .read = ct1_reg_read,
    .write = ct1_reg_write,
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

/* Forward declarations for cache ops */
static uint64_t cxl_type1_cache_read(void *opaque, hwaddr addr, unsigned size);
static void cxl_type1_cache_write(void *opaque, hwaddr addr, uint64_t value, unsigned size);

static const MemoryRegionOps cxl_type1_cache_ops = {
    .read = cxl_type1_cache_read,
    .write = cxl_type1_cache_write,
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

static void cxl_type1_realize(PCIDevice *pci_dev, Error **errp)
{
    CXLType1StateImpl *ct1d = CXL_TYPE1_DEV(pci_dev);
    CXLComponentState *cxl_cstate = &ct1d->cxl_cstate;
    
    pci_config_set_prog_interface(pci_dev->config, 0x10);
    
    /* Set default values */
    if (!ct1d->cxlmemsim.server_addr) {
        ct1d->cxlmemsim.server_addr = g_strdup("127.0.0.1");
    }
    if (ct1d->cxlmemsim.server_port == 0) {
        ct1d->cxlmemsim.server_port = 9999;
    }
    if (ct1d->device_size == 0) {
        ct1d->device_size = 256 * MiB;
    }
    if (ct1d->cache_size == 0) {
        ct1d->cache_size = 64 * MiB;
    }
    
    qemu_mutex_init(&ct1d->cxlmemsim.lock);
    
    pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ct1d->sn != 0) {
        pcie_dev_ser_num_init(pci_dev, 0x100, ct1d->sn);
        cxl_cstate->dvsec_offset = 0x100 + 0x0c;
    } else {
        cxl_cstate->dvsec_offset = 0x100;
    }
    
    ct1d->cxl_cstate.pdev = pci_dev;
    build_dvsecs(ct1d);
    
    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_CXL_TYPE1_DEV);
    
    /* BAR0: Component registers */
    memory_region_init(&ct1d->bar0, OBJECT(ct1d), "cxl-type1-bar0",
                      CXL2_COMPONENT_BLOCK_SIZE);
    
    memory_region_init_io(&ct1d->component_registers, OBJECT(ct1d),
                         &ct1_reg_ops, cxl_cstate, "cxl-type1-component",
                         CXL2_COMPONENT_CM_REGION_SIZE);
    memory_region_add_subregion(&ct1d->bar0, 0, &ct1d->component_registers);
    
    pci_register_bar(pci_dev, 0,
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_TYPE_64,
                    &ct1d->bar0);
    
    /* BAR2: Cache memory region for CXL.cache operations */
    memory_region_init_ram(&ct1d->cache_mem, OBJECT(ct1d), 
                          "cxl-type1-cache", ct1d->cache_size, errp);
    if (*errp) {
        error_prepend(errp, "Failed to initialize cache memory: ");
        return;
    }
    
    /* Create IO region for cache access with our read/write functions */
    memory_region_init_io(&ct1d->cache_io, OBJECT(ct1d),
                         &cxl_type1_cache_ops, ct1d, 
                         "cxl-type1-cache-io", ct1d->cache_size);
    
    /* Map cache IO region over RAM for intercepting accesses */
    memory_region_add_subregion_overlap(&ct1d->cache_mem, 0, &ct1d->cache_io, 1);
    
    pci_register_bar(pci_dev, 2,
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_TYPE_64 |
                    PCI_BASE_ADDRESS_MEM_PREFETCH,
                    &ct1d->cache_mem);
    
    /* Initialize MSI-X */
    if (msix_init_exclusive_bar(pci_dev, 10, 4, NULL)) {
        error_setg(errp, "Failed to initialize MSI-X");
        return;
    }
    
    /* Connect to CXLMemSim */
    cxlmemsim_connect(ct1d);
    if (ct1d->cxlmemsim.connected) {
        qemu_thread_create(&ct1d->cxlmemsim.recv_thread, "cxlmemsim-recv",
                          cxlmemsim_recv_thread, ct1d, QEMU_THREAD_JOINABLE);
    }
    
    qemu_log("CXL Type1: Device realized with %zu MB cache at BAR2\n", 
             ct1d->cache_size / MiB);
}

static void cxl_type1_exit(PCIDevice *pci_dev)
{
    CXLType1StateImpl *ct1d = CXL_TYPE1_DEV(pci_dev);
    
    cxlmemsim_disconnect(ct1d);
    if (ct1d->cxlmemsim.recv_thread.thread) {
        qemu_thread_join(&ct1d->cxlmemsim.recv_thread);
    }
    qemu_mutex_destroy(&ct1d->cxlmemsim.lock);
}

static const Property cxl_type1_props[] = {
    DEFINE_PROP_SIZE("size", CXLType1StateImpl, device_size, 256 * MiB),
    DEFINE_PROP_SIZE("cache-size", CXLType1StateImpl, cache_size, 64 * MiB),
    DEFINE_PROP_STRING("cxlmemsim-addr", CXLType1StateImpl, cxlmemsim.server_addr),
    DEFINE_PROP_UINT16("cxlmemsim-port", CXLType1StateImpl, cxlmemsim.server_port, 9999),
};

static void cxl_type1_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    
    pc->realize = cxl_type1_realize;
    pc->exit = cxl_type1_exit;
    pc->vendor_id = CXL_T1_VENDOR_ID;
    pc->device_id = CXL_T1_DEVICE_ID;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_MEMORY_CXL;
    
    dc->desc = "CXL Type 1 Accelerator Device";
    device_class_set_legacy_reset(dc, cxl_type1_reset);
    device_class_set_props(dc, cxl_type1_props);
}

static const TypeInfo cxl_type1_info = {
    .name = TYPE_CXL_TYPE1_DEV,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CXLType1StateImpl),
    .class_init = cxl_type1_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CXL_DEVICE },
        { }
    },
};

static void cxl_type1_register_types(void)
{
    type_register_static(&cxl_type1_info);
}

type_init(cxl_type1_register_types)

/* Cache memory access functions for CXL Type 1 device */
static uint64_t cxl_type1_cache_read(void *opaque, hwaddr addr, unsigned size)
{
    CXLType1StateImpl *ct1d = opaque;
    uint64_t value = 0;
    uint8_t *cache_ptr;
    
    if (addr + size > ct1d->cache_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "CXL Type1: Cache read out of bounds at 0x%lx\n", addr);
        return 0;
    }
    
    /* Read from cache memory - this would be coherent with CPU caches via CXL.cache */
    cache_ptr = memory_region_get_ram_ptr(&ct1d->cache_mem);
    if (cache_ptr) {
        memcpy(&value, cache_ptr + addr, size);
    }
    
    /* Track cache hits for statistics */
    if (ct1d->cxlmemsim.connected) {
        /* Send cache hit notification to CXLMemSim */
        struct {
            uint32_t type;
            uint32_t size;
            uint64_t addr_data;
        } msg = {
            .type = 0x10, /* CACHE_HIT */
            .size = sizeof(uint64_t),
            .addr_data = addr
        };
        qio_channel_write_all(QIO_CHANNEL(ct1d->cxlmemsim.socket),
                             (char *)&msg, sizeof(msg), NULL);
    }
    
    qemu_log_mask(LOG_TRACE, "CXL Type1: Cache read at 0x%lx = 0x%lx (size %u)\n", 
                 addr, value, size);
    return value;
}

static void cxl_type1_cache_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    CXLType1StateImpl *ct1d = opaque;
    uint8_t *cache_ptr;
    
    if (addr + size > ct1d->cache_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "CXL Type1: Cache write out of bounds at 0x%lx\n", addr);
        return;
    }
    
    /* Write to cache memory - maintains coherency via CXL.cache protocol */
    cache_ptr = memory_region_get_ram_ptr(&ct1d->cache_mem);
    if (cache_ptr) {
        memcpy(cache_ptr + addr, &value, size);
    }
    
    /* Send cache line update to CXLMemSim for coherency tracking */
    if (ct1d->cxlmemsim.connected) {
        struct {
            uint32_t type;
            uint32_t size;
            uint64_t addr;
            uint64_t data;
        } msg = {
            .type = 0x11, /* CACHE_UPDATE */
            .size = sizeof(uint64_t) * 2,
            .addr = addr,
            .data = value
        };
        
        qio_channel_write_all(QIO_CHANNEL(ct1d->cxlmemsim.socket),
                             (char *)&msg, sizeof(msg), NULL);
    }
    
    qemu_log_mask(LOG_TRACE, "CXL Type1: Cache write at 0x%lx = 0x%lx (size %u)\n",
                 addr, value, size);
}