/*
 * CXL Type 3 (memory expander) device
 *
 * Copyright(C) 2020 Intel Corporation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-v2-only
 */
#include <math.h>

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/qapi-commands-cxl.h"
#include "hw/mem/memory-device.h"
#include "hw/mem/pc-dimm.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/pmem.h"
#include "qemu/range.h"
#include "qemu/rcu.h"
#include "qemu/guest-random.h"
#include "system/hostmem.h"
#include "system/numa.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "hw/cxl/cxl.h"
#include "hw/pci/msix.h"

/* type3 device private */
enum CXL_T3_MSIX_VECTOR {
    CXL_T3_MSIX_PCIE_DOE_TABLE_ACCESS = 0,
    CXL_T3_MSIX_EVENT_START = 2,
    CXL_T3_MSIX_MBOX = CXL_T3_MSIX_EVENT_START + CXL_EVENT_TYPE_MAX,
    CXL_T3_MSIX_VECTOR_NR
};

#define DWORD_BYTE 4
#define CXL_CAPACITY_MULTIPLIER   (256 * MiB)

/* Default CDAT entries for a memory region */
enum {
    CT3_CDAT_DSMAS,
    CT3_CDAT_DSLBIS0,
    CT3_CDAT_DSLBIS1,
    CT3_CDAT_DSLBIS2,
    CT3_CDAT_DSLBIS3,
    CT3_CDAT_DSEMTS,
    CT3_CDAT_NUM_ENTRIES
};

static void ct3_build_cdat_entries_for_mr(CDATSubHeader **cdat_table,
                                          int dsmad_handle, uint64_t size,
                                          bool is_pmem, bool is_dynamic,
                                          uint64_t dpa_base)
{
    CDATDsmas *dsmas;
    CDATDslbis *dslbis0;
    CDATDslbis *dslbis1;
    CDATDslbis *dslbis2;
    CDATDslbis *dslbis3;
    CDATDsemts *dsemts;

    dsmas = g_malloc(sizeof(*dsmas));
    *dsmas = (CDATDsmas) {
        .header = {
            .type = CDAT_TYPE_DSMAS,
            .length = sizeof(*dsmas),
        },
        .DSMADhandle = dsmad_handle,
        .flags = (is_pmem ? CDAT_DSMAS_FLAG_NV : 0) |
                 (is_dynamic ? CDAT_DSMAS_FLAG_DYNAMIC_CAP : 0),
        .DPA_base = dpa_base,
        .DPA_length = size,
    };

    /* For now, no memory side cache, plausiblish numbers */
    dslbis0 = g_malloc(sizeof(*dslbis0));
    *dslbis0 = (CDATDslbis) {
        .header = {
            .type = CDAT_TYPE_DSLBIS,
            .length = sizeof(*dslbis0),
        },
        .handle = dsmad_handle,
        .flags = HMAT_LB_MEM_MEMORY,
        .data_type = HMAT_LB_DATA_READ_LATENCY,
        .entry_base_unit = 10000, /* 10ns base */
        .entry[0] = 15, /* 150ns */
    };

    dslbis1 = g_malloc(sizeof(*dslbis1));
    *dslbis1 = (CDATDslbis) {
        .header = {
            .type = CDAT_TYPE_DSLBIS,
            .length = sizeof(*dslbis1),
        },
        .handle = dsmad_handle,
        .flags = HMAT_LB_MEM_MEMORY,
        .data_type = HMAT_LB_DATA_WRITE_LATENCY,
        .entry_base_unit = 10000,
        .entry[0] = 25, /* 250ns */
    };

    dslbis2 = g_malloc(sizeof(*dslbis2));
    *dslbis2 = (CDATDslbis) {
        .header = {
            .type = CDAT_TYPE_DSLBIS,
            .length = sizeof(*dslbis2),
        },
        .handle = dsmad_handle,
        .flags = HMAT_LB_MEM_MEMORY,
        .data_type = HMAT_LB_DATA_READ_BANDWIDTH,
        .entry_base_unit = 1000, /* GB/s */
        .entry[0] = 16,
    };

    dslbis3 = g_malloc(sizeof(*dslbis3));
    *dslbis3 = (CDATDslbis) {
        .header = {
            .type = CDAT_TYPE_DSLBIS,
            .length = sizeof(*dslbis3),
        },
        .handle = dsmad_handle,
        .flags = HMAT_LB_MEM_MEMORY,
        .data_type = HMAT_LB_DATA_WRITE_BANDWIDTH,
        .entry_base_unit = 1000, /* GB/s */
        .entry[0] = 16,
    };

    dsemts = g_malloc(sizeof(*dsemts));
    *dsemts = (CDATDsemts) {
        .header = {
            .type = CDAT_TYPE_DSEMTS,
            .length = sizeof(*dsemts),
        },
        .DSMAS_handle = dsmad_handle,
        /*
         * NV: Reserved - the non volatile from DSMAS matters
         * V: EFI_MEMORY_SP
         */
        .EFI_memory_type_attr = is_pmem ? 2 : 1,
        .DPA_offset = 0,
        .DPA_length = size,
    };

    /* Header always at start of structure */
    cdat_table[CT3_CDAT_DSMAS] = (CDATSubHeader *)dsmas;
    cdat_table[CT3_CDAT_DSLBIS0] = (CDATSubHeader *)dslbis0;
    cdat_table[CT3_CDAT_DSLBIS1] = (CDATSubHeader *)dslbis1;
    cdat_table[CT3_CDAT_DSLBIS2] = (CDATSubHeader *)dslbis2;
    cdat_table[CT3_CDAT_DSLBIS3] = (CDATSubHeader *)dslbis3;
    cdat_table[CT3_CDAT_DSEMTS] = (CDATSubHeader *)dsemts;
}

static int ct3_build_cdat_table(CDATSubHeader ***cdat_table, void *priv)
{
    g_autofree CDATSubHeader **table = NULL;
    CXLType3Dev *ct3d = priv;
    MemoryRegion *volatile_mr = NULL, *nonvolatile_mr = NULL;
    MemoryRegion *dc_mr = NULL;
    uint64_t vmr_size = 0, pmr_size = 0;
    int dsmad_handle = 0;
    int cur_ent = 0;
    int len = 0;

    if (!ct3d->hostpmem && !ct3d->hostvmem && !ct3d->dc.num_regions) {
        return 0;
    }

    if (ct3d->hostvmem) {
        volatile_mr = host_memory_backend_get_memory(ct3d->hostvmem);
        if (!volatile_mr) {
            return -EINVAL;
        }
        len += CT3_CDAT_NUM_ENTRIES;
        vmr_size = memory_region_size(volatile_mr);
    }

    if (ct3d->hostpmem) {
        nonvolatile_mr = host_memory_backend_get_memory(ct3d->hostpmem);
        if (!nonvolatile_mr) {
            return -EINVAL;
        }
        len += CT3_CDAT_NUM_ENTRIES;
        pmr_size = memory_region_size(nonvolatile_mr);
    }

    if (ct3d->dc.num_regions) {
        if (!ct3d->dc.host_dc) {
            return -EINVAL;
        }
        dc_mr = host_memory_backend_get_memory(ct3d->dc.host_dc);
        if (!dc_mr) {
            return -EINVAL;
        }
        len += CT3_CDAT_NUM_ENTRIES * ct3d->dc.num_regions;
    }

    table = g_malloc0(len * sizeof(*table));

    /* Now fill them in */
    if (volatile_mr) {
        ct3_build_cdat_entries_for_mr(table, dsmad_handle++, vmr_size,
                                      false, false, 0);
        cur_ent = CT3_CDAT_NUM_ENTRIES;
    }

    if (nonvolatile_mr) {
        uint64_t base = vmr_size;
        ct3_build_cdat_entries_for_mr(&(table[cur_ent]), dsmad_handle++,
                                      pmr_size, true, false, base);
        cur_ent += CT3_CDAT_NUM_ENTRIES;
    }

    if (dc_mr) {
        int i;
        uint64_t region_base = vmr_size + pmr_size;

        /*
         * We assume the dynamic capacity to be volatile for now.
         * Non-volatile dynamic capacity will be added if needed in the
         * future.
         */
        for (i = 0; i < ct3d->dc.num_regions; i++) {
            ct3d->dc.regions[i].nonvolatile = false;
            ct3d->dc.regions[i].sharable = false;
            ct3d->dc.regions[i].hw_managed_coherency = false;
            ct3d->dc.regions[i].ic_specific_dc_management = false;
            ct3d->dc.regions[i].rdonly = false;
            ct3_build_cdat_entries_for_mr(&(table[cur_ent]),
                                          dsmad_handle++,
                                          ct3d->dc.regions[i].len,
                                          ct3d->dc.regions[i].nonvolatile,
                                          true, region_base);
            ct3d->dc.regions[i].dsmadhandle = dsmad_handle - 1;

            cur_ent += CT3_CDAT_NUM_ENTRIES;
            region_base += ct3d->dc.regions[i].len;
        }
    }

    assert(len == cur_ent);

    *cdat_table = g_steal_pointer(&table);

    return len;
}

static void ct3_free_cdat_table(CDATSubHeader **cdat_table, int num, void *priv)
{
    int i;

    for (i = 0; i < num; i++) {
        g_free(cdat_table[i]);
    }
    g_free(cdat_table);
}

static bool cxl_doe_cdat_rsp(DOECap *doe_cap)
{
    CDATObject *cdat = &CXL_TYPE3(doe_cap->pdev)->cxl_cstate.cdat;
    uint16_t ent;
    void *base;
    uint32_t len;
    CDATReq *req = pcie_doe_get_write_mbox_ptr(doe_cap);
    CDATRsp rsp;

    assert(cdat->entry_len);

    /* Discard if request length mismatched */
    if (pcie_doe_get_obj_len(req) <
        DIV_ROUND_UP(sizeof(CDATReq), DWORD_BYTE)) {
        return false;
    }

    ent = req->entry_handle;
    base = cdat->entry[ent].base;
    len = cdat->entry[ent].length;

    rsp = (CDATRsp) {
        .header = {
            .vendor_id = CXL_VENDOR_ID,
            .data_obj_type = CXL_DOE_TABLE_ACCESS,
            .reserved = 0x0,
            .length = DIV_ROUND_UP((sizeof(rsp) + len), DWORD_BYTE),
        },
        .rsp_code = CXL_DOE_TAB_RSP,
        .table_type = CXL_DOE_TAB_TYPE_CDAT,
        .entry_handle = (ent < cdat->entry_len - 1) ?
                        ent + 1 : CXL_DOE_TAB_ENT_MAX,
    };

    memcpy(doe_cap->read_mbox, &rsp, sizeof(rsp));
    memcpy(doe_cap->read_mbox + DIV_ROUND_UP(sizeof(rsp), DWORD_BYTE),
           base, len);

    doe_cap->read_mbox_len += rsp.header.length;

    return true;
}

static uint32_t ct3d_config_read(PCIDevice *pci_dev, uint32_t addr, int size)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    uint32_t val;

    if (pcie_doe_read_config(&ct3d->doe_cdat, addr, size, &val)) {
        return val;
    }

    return pci_default_read_config(pci_dev, addr, size);
}

static void ct3d_config_write(PCIDevice *pci_dev, uint32_t addr, uint32_t val,
                              int size)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);

    pcie_doe_write_config(&ct3d->doe_cdat, addr, val, size);
    pci_default_write_config(pci_dev, addr, val, size);
    pcie_aer_write_config(pci_dev, addr, val, size);
}

/*
 * Null value of all Fs suggested by IEEE RA guidelines for use of
 * EU, OUI and CID
 */
#define UI64_NULL ~(0ULL)

static void build_dvsecs(CXLType3Dev *ct3d)
{
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    uint8_t *dvsec;
    uint32_t range1_size_hi, range1_size_lo,
             range1_base_hi = 0, range1_base_lo = 0,
             range2_size_hi = 0, range2_size_lo = 0,
             range2_base_hi = 0, range2_base_lo = 0;

    /*
     * Volatile memory is mapped as (0x0)
     * Persistent memory is mapped at (volatile->size)
     */
    if (ct3d->hostvmem) {
        range1_size_hi = ct3d->hostvmem->size >> 32;
        range1_size_lo = (2 << 5) | (2 << 2) | 0x3 |
                         (ct3d->hostvmem->size & 0xF0000000);
        if (ct3d->hostpmem) {
            range2_size_hi = ct3d->hostpmem->size >> 32;
            range2_size_lo = (2 << 5) | (2 << 2) | 0x3 |
                             (ct3d->hostpmem->size & 0xF0000000);
        }
    } else if (ct3d->hostpmem) {
        range1_size_hi = ct3d->hostpmem->size >> 32;
        range1_size_lo = (2 << 5) | (2 << 2) | 0x3 |
                         (ct3d->hostpmem->size & 0xF0000000);
    } else {
        /*
         * For DCD with no static memory, set memory active, memory class bits.
         * No range is set.
         */
        range1_size_hi = 0;
        range1_size_lo = (2 << 5) | (2 << 2) | 0x3;
    }

    dvsec = (uint8_t *)&(CXLDVSECDevice){
        .cap = 0x1e,
        .ctrl = 0x2,
        .status2 = 0x2,
        .range1_size_hi = range1_size_hi,
        .range1_size_lo = range1_size_lo,
        .range1_base_hi = range1_base_hi,
        .range1_base_lo = range1_base_lo,
        .range2_size_hi = range2_size_hi,
        .range2_size_lo = range2_size_lo,
        .range2_base_hi = range2_base_hi,
        .range2_base_lo = range2_base_lo,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               PCIE_CXL_DEVICE_DVSEC_LENGTH,
                               PCIE_CXL_DEVICE_DVSEC,
                               PCIE_CXL31_DEVICE_DVSEC_REVID, dvsec);

    dvsec = (uint8_t *)&(CXLDVSECRegisterLocator){
        .rsvd         = 0,
        .reg0_base_lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg0_base_hi = 0,
        .reg1_base_lo = RBI_CXL_DEVICE_REG | CXL_DEVICE_REG_BAR_IDX,
        .reg1_base_hi = 0,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                               REG_LOC_DVSEC_REVID, dvsec);
    dvsec = (uint8_t *)&(CXLDVSECDeviceGPF){
        .phase2_duration = 0x603, /* 3 seconds */
        .phase2_power = 0x33, /* 0x33 miliwatts */
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               GPF_DEVICE_DVSEC_LENGTH, GPF_DEVICE_DVSEC,
                               GPF_DEVICE_DVSEC_REVID, dvsec);

    dvsec = (uint8_t *)&(CXLDVSECPortFlexBus){
        .cap                     = 0x26, /* 68B, IO, Mem, non-MLD */
        .ctrl                    = 0x02, /* IO always enabled */
        .status                  = 0x26, /* same as capabilities */
        .rcvd_mod_ts_data_phase1 = 0xef, /* WTF? */
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE3_DEVICE,
                               PCIE_CXL3_FLEXBUS_PORT_DVSEC_LENGTH,
                               PCIE_FLEXBUS_PORT_DVSEC,
                               PCIE_CXL3_FLEXBUS_PORT_DVSEC_REVID, dvsec);
}

static void hdm_decoder_commit(CXLType3Dev *ct3d, int which)
{
    int hdm_inc = R_CXL_HDM_DECODER1_BASE_LO - R_CXL_HDM_DECODER0_BASE_LO;
    ComponentRegisters *cregs = &ct3d->cxl_cstate.crb;
    uint32_t *cache_mem = cregs->cache_mem_registers;
    uint32_t ctrl;

    ctrl = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_CTRL + which * hdm_inc);
    /* TODO: Sanity checks that the decoder is possible */
    ctrl = FIELD_DP32(ctrl, CXL_HDM_DECODER0_CTRL, ERR, 0);
    ctrl = FIELD_DP32(ctrl, CXL_HDM_DECODER0_CTRL, COMMITTED, 1);

    stl_le_p(cache_mem + R_CXL_HDM_DECODER0_CTRL + which * hdm_inc, ctrl);
}

static void hdm_decoder_uncommit(CXLType3Dev *ct3d, int which)
{
    int hdm_inc = R_CXL_HDM_DECODER1_BASE_LO - R_CXL_HDM_DECODER0_BASE_LO;
    ComponentRegisters *cregs = &ct3d->cxl_cstate.crb;
    uint32_t *cache_mem = cregs->cache_mem_registers;
    uint32_t ctrl;

    ctrl = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_CTRL + which * hdm_inc);

    ctrl = FIELD_DP32(ctrl, CXL_HDM_DECODER0_CTRL, ERR, 0);
    ctrl = FIELD_DP32(ctrl, CXL_HDM_DECODER0_CTRL, COMMITTED, 0);

    stl_le_p(cache_mem + R_CXL_HDM_DECODER0_CTRL + which * hdm_inc, ctrl);
}

static int ct3d_qmp_uncor_err_to_cxl(CxlUncorErrorType qmp_err)
{
    switch (qmp_err) {
    case CXL_UNCOR_ERROR_TYPE_CACHE_DATA_PARITY:
        return CXL_RAS_UNC_ERR_CACHE_DATA_PARITY;
    case CXL_UNCOR_ERROR_TYPE_CACHE_ADDRESS_PARITY:
        return CXL_RAS_UNC_ERR_CACHE_ADDRESS_PARITY;
    case CXL_UNCOR_ERROR_TYPE_CACHE_BE_PARITY:
        return CXL_RAS_UNC_ERR_CACHE_BE_PARITY;
    case CXL_UNCOR_ERROR_TYPE_CACHE_DATA_ECC:
        return CXL_RAS_UNC_ERR_CACHE_DATA_ECC;
    case CXL_UNCOR_ERROR_TYPE_MEM_DATA_PARITY:
        return CXL_RAS_UNC_ERR_MEM_DATA_PARITY;
    case CXL_UNCOR_ERROR_TYPE_MEM_ADDRESS_PARITY:
        return CXL_RAS_UNC_ERR_MEM_ADDRESS_PARITY;
    case CXL_UNCOR_ERROR_TYPE_MEM_BE_PARITY:
        return CXL_RAS_UNC_ERR_MEM_BE_PARITY;
    case CXL_UNCOR_ERROR_TYPE_MEM_DATA_ECC:
        return CXL_RAS_UNC_ERR_MEM_DATA_ECC;
    case CXL_UNCOR_ERROR_TYPE_REINIT_THRESHOLD:
        return CXL_RAS_UNC_ERR_REINIT_THRESHOLD;
    case CXL_UNCOR_ERROR_TYPE_RSVD_ENCODING:
        return CXL_RAS_UNC_ERR_RSVD_ENCODING;
    case CXL_UNCOR_ERROR_TYPE_POISON_RECEIVED:
        return CXL_RAS_UNC_ERR_POISON_RECEIVED;
    case CXL_UNCOR_ERROR_TYPE_RECEIVER_OVERFLOW:
        return CXL_RAS_UNC_ERR_RECEIVER_OVERFLOW;
    case CXL_UNCOR_ERROR_TYPE_INTERNAL:
        return CXL_RAS_UNC_ERR_INTERNAL;
    case CXL_UNCOR_ERROR_TYPE_CXL_IDE_TX:
        return CXL_RAS_UNC_ERR_CXL_IDE_TX;
    case CXL_UNCOR_ERROR_TYPE_CXL_IDE_RX:
        return CXL_RAS_UNC_ERR_CXL_IDE_RX;
    default:
        return -EINVAL;
    }
}

static int ct3d_qmp_cor_err_to_cxl(CxlCorErrorType qmp_err)
{
    switch (qmp_err) {
    case CXL_COR_ERROR_TYPE_CACHE_DATA_ECC:
        return CXL_RAS_COR_ERR_CACHE_DATA_ECC;
    case CXL_COR_ERROR_TYPE_MEM_DATA_ECC:
        return CXL_RAS_COR_ERR_MEM_DATA_ECC;
    case CXL_COR_ERROR_TYPE_CRC_THRESHOLD:
        return CXL_RAS_COR_ERR_CRC_THRESHOLD;
    case CXL_COR_ERROR_TYPE_RETRY_THRESHOLD:
        return CXL_RAS_COR_ERR_RETRY_THRESHOLD;
    case CXL_COR_ERROR_TYPE_CACHE_POISON_RECEIVED:
        return CXL_RAS_COR_ERR_CACHE_POISON_RECEIVED;
    case CXL_COR_ERROR_TYPE_MEM_POISON_RECEIVED:
        return CXL_RAS_COR_ERR_MEM_POISON_RECEIVED;
    case CXL_COR_ERROR_TYPE_PHYSICAL:
        return CXL_RAS_COR_ERR_PHYSICAL;
    default:
        return -EINVAL;
    }
}

static void ct3d_reg_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CXLComponentState *cxl_cstate = opaque;
    ComponentRegisters *cregs = &cxl_cstate->crb;
    CXLType3Dev *ct3d = container_of(cxl_cstate, CXLType3Dev, cxl_cstate);
    uint32_t *cache_mem = cregs->cache_mem_registers;
    bool should_commit = false;
    bool should_uncommit = false;
    int which_hdm = -1;

    assert(size == 4);
    g_assert(offset < CXL2_COMPONENT_CM_REGION_SIZE);

    switch (offset) {
    case A_CXL_HDM_DECODER0_CTRL:
        should_commit = FIELD_EX32(value, CXL_HDM_DECODER0_CTRL, COMMIT);
        should_uncommit = !should_commit;
        which_hdm = 0;
        break;
    case A_CXL_HDM_DECODER1_CTRL:
        should_commit = FIELD_EX32(value, CXL_HDM_DECODER0_CTRL, COMMIT);
        should_uncommit = !should_commit;
        which_hdm = 1;
        break;
    case A_CXL_HDM_DECODER2_CTRL:
        should_commit = FIELD_EX32(value, CXL_HDM_DECODER0_CTRL, COMMIT);
        should_uncommit = !should_commit;
        which_hdm = 2;
        break;
    case A_CXL_HDM_DECODER3_CTRL:
        should_commit = FIELD_EX32(value, CXL_HDM_DECODER0_CTRL, COMMIT);
        should_uncommit = !should_commit;
        which_hdm = 3;
        break;
    case A_CXL_RAS_UNC_ERR_STATUS:
    {
        uint32_t capctrl = ldl_le_p(cache_mem + R_CXL_RAS_ERR_CAP_CTRL);
        uint32_t fe = FIELD_EX32(capctrl, CXL_RAS_ERR_CAP_CTRL,
                                 FIRST_ERROR_POINTER);
        CXLError *cxl_err;
        uint32_t unc_err;

        /*
         * If single bit written that corresponds to the first error
         * pointer being cleared, update the status and header log.
         */
        if (!QTAILQ_EMPTY(&ct3d->error_list)) {
            if ((1 << fe) ^ value) {
                CXLError *cxl_next;
                /*
                 * Software is using wrong flow for multiple header recording
                 * Following behavior in PCIe r6.0 and assuming multiple
                 * header support. Implementation defined choice to clear all
                 * matching records if more than one bit set - which corresponds
                 * closest to behavior of hardware not capable of multiple
                 * header recording.
                 */
                QTAILQ_FOREACH_SAFE(cxl_err, &ct3d->error_list, node,
                                    cxl_next) {
                    if ((1 << cxl_err->type) & value) {
                        QTAILQ_REMOVE(&ct3d->error_list, cxl_err, node);
                        g_free(cxl_err);
                    }
                }
            } else {
                /* Done with previous FE, so drop from list */
                cxl_err = QTAILQ_FIRST(&ct3d->error_list);
                QTAILQ_REMOVE(&ct3d->error_list, cxl_err, node);
                g_free(cxl_err);
            }

            /*
             * If there is another FE, then put that in place and update
             * the header log
             */
            if (!QTAILQ_EMPTY(&ct3d->error_list)) {
                uint32_t *header_log = &cache_mem[R_CXL_RAS_ERR_HEADER0];
                int i;

                cxl_err = QTAILQ_FIRST(&ct3d->error_list);
                for (i = 0; i < CXL_RAS_ERR_HEADER_NUM; i++) {
                    stl_le_p(header_log + i, cxl_err->header[i]);
                }
                capctrl = FIELD_DP32(capctrl, CXL_RAS_ERR_CAP_CTRL,
                                     FIRST_ERROR_POINTER, cxl_err->type);
            } else {
                /*
                 * If no more errors, then follow recommendation of PCI spec
                 * r6.0 6.2.4.2 to set the first error pointer to a status
                 * bit that will never be used.
                 */
                capctrl = FIELD_DP32(capctrl, CXL_RAS_ERR_CAP_CTRL,
                                     FIRST_ERROR_POINTER,
                                     CXL_RAS_UNC_ERR_CXL_UNUSED);
            }
            stl_le_p((uint8_t *)cache_mem + A_CXL_RAS_ERR_CAP_CTRL, capctrl);
        }
        unc_err = 0;
        QTAILQ_FOREACH(cxl_err, &ct3d->error_list, node) {
            unc_err |= 1 << cxl_err->type;
        }
        stl_le_p((uint8_t *)cache_mem + offset, unc_err);

        return;
    }
    case A_CXL_RAS_COR_ERR_STATUS:
    {
        uint32_t rw1c = value;
        uint32_t temp = ldl_le_p((uint8_t *)cache_mem + offset);
        temp &= ~rw1c;
        stl_le_p((uint8_t *)cache_mem + offset, temp);
        return;
    }
    default:
        break;
    }

    stl_le_p((uint8_t *)cache_mem + offset, value);
    if (should_commit) {
        hdm_decoder_commit(ct3d, which_hdm);
    } else if (should_uncommit) {
        hdm_decoder_uncommit(ct3d, which_hdm);
    }
}

/*
 * TODO: dc region configuration will be updated once host backend and address
 * space support is added for DCD.
 */
static bool cxl_create_dc_regions(CXLType3Dev *ct3d, Error **errp)
{
    int i;
    uint64_t region_base = 0;
    uint64_t region_len;
    uint64_t decode_len;
    uint64_t blk_size = 2 * MiB;
    /* Only 1 block size is supported for now. */
    uint64_t supported_blk_size_bitmask = blk_size;
    CXLDCRegion *region;
    MemoryRegion *mr;
    uint64_t dc_size;

    mr = host_memory_backend_get_memory(ct3d->dc.host_dc);
    dc_size = memory_region_size(mr);
    region_len = DIV_ROUND_UP(dc_size, ct3d->dc.num_regions);

    if (dc_size % (ct3d->dc.num_regions * CXL_CAPACITY_MULTIPLIER) != 0) {
        error_setg(errp,
                   "backend size is not multiple of region len: 0x%" PRIx64,
                   region_len);
        return false;
    }
    if (region_len % CXL_CAPACITY_MULTIPLIER != 0) {
        error_setg(errp, "DC region size is unaligned to 0x%" PRIx64,
                   CXL_CAPACITY_MULTIPLIER);
        return false;
    }
    decode_len = region_len;

    if (ct3d->hostvmem) {
        mr = host_memory_backend_get_memory(ct3d->hostvmem);
        region_base += memory_region_size(mr);
    }
    if (ct3d->hostpmem) {
        mr = host_memory_backend_get_memory(ct3d->hostpmem);
        region_base += memory_region_size(mr);
    }
    if (region_base % CXL_CAPACITY_MULTIPLIER != 0) {
        error_setg(errp, "DC region base not aligned to 0x%" PRIx64,
                   CXL_CAPACITY_MULTIPLIER);
        return false;
    }

    for (i = 0, region = &ct3d->dc.regions[0];
         i < ct3d->dc.num_regions;
         i++, region++, region_base += region_len) {
        *region = (CXLDCRegion) {
            .base = region_base,
            .decode_len = decode_len,
            .len = region_len,
            .block_size = blk_size,
            /* dsmad_handle set when creating CDAT table entries */
            .flags = 0,
            .supported_blk_size_bitmask = supported_blk_size_bitmask,
        };
        ct3d->dc.total_capacity += region->len;
        region->blk_bitmap = bitmap_new(region->len / region->block_size);
        qemu_mutex_init(&region->bitmap_lock);
    }
    QTAILQ_INIT(&ct3d->dc.extents);
    QTAILQ_INIT(&ct3d->dc.extents_pending);

    return true;
}

static void cxl_destroy_dc_regions(CXLType3Dev *ct3d)
{
    CXLDCExtent *ent, *ent_next;
    CXLDCExtentGroup *group, *group_next;
    int i;
    CXLDCRegion *region;

    QTAILQ_FOREACH_SAFE(ent, &ct3d->dc.extents, node, ent_next) {
        cxl_remove_extent_from_extent_list(&ct3d->dc.extents, ent);
    }

    QTAILQ_FOREACH_SAFE(group, &ct3d->dc.extents_pending, node, group_next) {
        QTAILQ_REMOVE(&ct3d->dc.extents_pending, group, node);
        QTAILQ_FOREACH_SAFE(ent, &group->list, node, ent_next) {
            cxl_remove_extent_from_extent_list(&group->list, ent);
        }
        g_free(group);
    }

    for (i = 0; i < ct3d->dc.num_regions; i++) {
        region = &ct3d->dc.regions[i];
        g_free(region->blk_bitmap);
    }
}

static bool cxl_setup_memory(CXLType3Dev *ct3d, Error **errp)
{
    DeviceState *ds = DEVICE(ct3d);

    if (!ct3d->hostmem && !ct3d->hostvmem && !ct3d->hostpmem
        && !ct3d->dc.num_regions) {
        error_setg(errp, "at least one memdev property must be set");
        return false;
    } else if (ct3d->hostmem && ct3d->hostpmem) {
        error_setg(errp, "[memdev] cannot be used with new "
                         "[persistent-memdev] property");
        return false;
    } else if (ct3d->hostmem) {
        /* Use of hostmem property implies pmem */
        ct3d->hostpmem = ct3d->hostmem;
        ct3d->hostmem = NULL;
    }

    if (ct3d->hostpmem && !ct3d->lsa) {
        error_setg(errp, "lsa property must be set for persistent devices");
        return false;
    }

    if (ct3d->hostvmem) {
        MemoryRegion *vmr;
        char *v_name;

        vmr = host_memory_backend_get_memory(ct3d->hostvmem);
        if (!vmr) {
            error_setg(errp, "volatile memdev must have backing device");
            return false;
        }
        if (host_memory_backend_is_mapped(ct3d->hostvmem)) {
            error_setg(errp, "memory backend %s can't be used multiple times.",
               object_get_canonical_path_component(OBJECT(ct3d->hostvmem)));
            return false;
        }
        memory_region_set_nonvolatile(vmr, false);
        memory_region_set_enabled(vmr, true);
        host_memory_backend_set_mapped(ct3d->hostvmem, true);
        if (ds->id) {
            v_name = g_strdup_printf("cxl-type3-dpa-vmem-space:%s", ds->id);
        } else {
            v_name = g_strdup("cxl-type3-dpa-vmem-space");
        }
        address_space_init(&ct3d->hostvmem_as, vmr, v_name);
        ct3d->cxl_dstate.vmem_size = memory_region_size(vmr);
        ct3d->cxl_dstate.static_mem_size += memory_region_size(vmr);
        info_report("CXL Type3: Volatile memory initialized - size=%lu MB", 
                   memory_region_size(vmr) / (1024*1024));
        g_free(v_name);
    }

    if (ct3d->hostpmem) {
        MemoryRegion *pmr;
        char *p_name;

        pmr = host_memory_backend_get_memory(ct3d->hostpmem);
        if (!pmr) {
            error_setg(errp, "persistent memdev must have backing device");
            return false;
        }
        if (host_memory_backend_is_mapped(ct3d->hostpmem)) {
            error_setg(errp, "memory backend %s can't be used multiple times.",
               object_get_canonical_path_component(OBJECT(ct3d->hostpmem)));
            return false;
        }
        memory_region_set_nonvolatile(pmr, true);
        memory_region_set_enabled(pmr, true);
        host_memory_backend_set_mapped(ct3d->hostpmem, true);
        if (ds->id) {
            p_name = g_strdup_printf("cxl-type3-dpa-pmem-space:%s", ds->id);
        } else {
            p_name = g_strdup("cxl-type3-dpa-pmem-space");
        }
        address_space_init(&ct3d->hostpmem_as, pmr, p_name);
        ct3d->cxl_dstate.pmem_size = memory_region_size(pmr);
        ct3d->cxl_dstate.static_mem_size += memory_region_size(pmr);
        info_report("CXL Type3: Persistent memory initialized - size=%lu MB",
                   memory_region_size(pmr) / (1024*1024));
        g_free(p_name);
    }

    ct3d->dc.total_capacity = 0;
    if (ct3d->dc.num_regions > 0) {
        MemoryRegion *dc_mr;
        char *dc_name;

        if (!ct3d->dc.host_dc) {
            error_setg(errp, "dynamic capacity must have a backing device");
            return false;
        }

        dc_mr = host_memory_backend_get_memory(ct3d->dc.host_dc);
        if (!dc_mr) {
            error_setg(errp, "dynamic capacity must have a backing device");
            return false;
        }

        if (host_memory_backend_is_mapped(ct3d->dc.host_dc)) {
            error_setg(errp, "memory backend %s can't be used multiple times.",
               object_get_canonical_path_component(OBJECT(ct3d->dc.host_dc)));
            return false;
        }
        /*
         * Set DC regions as volatile for now, non-volatile support can
         * be added in the future if needed.
         */
        memory_region_set_nonvolatile(dc_mr, false);
        memory_region_set_enabled(dc_mr, true);
        host_memory_backend_set_mapped(ct3d->dc.host_dc, true);
        if (ds->id) {
            dc_name = g_strdup_printf("cxl-dcd-dpa-dc-space:%s", ds->id);
        } else {
            dc_name = g_strdup("cxl-dcd-dpa-dc-space");
        }
        address_space_init(&ct3d->dc.host_dc_as, dc_mr, dc_name);
        g_free(dc_name);

        if (!cxl_create_dc_regions(ct3d, errp)) {
            error_append_hint(errp, "setup DC regions failed");
            return false;
        }
    }

    return true;
}

static DOEProtocol doe_cdat_prot[] = {
    { CXL_VENDOR_ID, CXL_DOE_TABLE_ACCESS, cxl_doe_cdat_rsp },
    { }
};

/* Initialize CXL device alerts with default threshold values. */
static void init_alert_config(CXLType3Dev *ct3d)
{
    ct3d->alert_config = (CXLAlertConfig) {
        .life_used_crit_alert_thresh = 75,
        .life_used_warn_thresh = 40,
        .over_temp_crit_alert_thresh = 35,
        .under_temp_crit_alert_thresh = 10,
        .over_temp_warn_thresh = 25,
        .under_temp_warn_thresh = 20
    };
}

static void ct3_realize(PCIDevice *pci_dev, Error **errp)
{
    ERRP_GUARD();
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;
    MemoryRegion *mr = &regs->component_registers;
    uint8_t *pci_conf = pci_dev->config;
    int i, rc;
    uint16_t count;

    QTAILQ_INIT(&ct3d->error_list);

    if (!cxl_setup_memory(ct3d, errp)) {
        return;
    }

    pci_config_set_prog_interface(pci_conf, 0x10);

    pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ct3d->sn != UI64_NULL) {
        pcie_dev_ser_num_init(pci_dev, 0x100, ct3d->sn);
        cxl_cstate->dvsec_offset = 0x100 + 0x0c;
    } else {
        cxl_cstate->dvsec_offset = 0x100;
    }

    ct3d->cxl_cstate.pdev = pci_dev;
    build_dvsecs(ct3d);

    regs->special_ops = g_new0(MemoryRegionOps, 1);
    regs->special_ops->write = ct3d_reg_write;

    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_CXL_TYPE3);

    pci_register_bar(
        pci_dev, CXL_COMPONENT_REG_BAR_IDX,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, mr);

    cxl_device_register_block_init(OBJECT(pci_dev), &ct3d->cxl_dstate,
                                   &ct3d->cci);
    pci_register_bar(pci_dev, CXL_DEVICE_REG_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &ct3d->cxl_dstate.device_registers);

    /* MSI(-X) Initialization */
    rc = msix_init_exclusive_bar(pci_dev, CXL_T3_MSIX_VECTOR_NR, 4, errp);
    if (rc) {
        goto err_free_special_ops;
    }
    for (i = 0; i < CXL_T3_MSIX_VECTOR_NR; i++) {
        msix_vector_use(pci_dev, i);
    }

    /* DOE Initialization */
    pcie_doe_init(pci_dev, &ct3d->doe_cdat, 0x190, doe_cdat_prot, true,
                  CXL_T3_MSIX_PCIE_DOE_TABLE_ACCESS);

    cxl_cstate->cdat.build_cdat_table = ct3_build_cdat_table;
    cxl_cstate->cdat.free_cdat_table = ct3_free_cdat_table;
    cxl_cstate->cdat.private = ct3d;
    if (!cxl_doe_cdat_init(cxl_cstate, errp)) {
        goto err_msix_uninit;
    }

    init_alert_config(ct3d);
    pcie_cap_deverr_init(pci_dev);
    /* Leave a bit of room for expansion */
    rc = pcie_aer_init(pci_dev, PCI_ERR_VER, 0x200, PCI_ERR_SIZEOF, errp);
    if (rc) {
        goto err_release_cdat;
    }
    cxl_event_init(&ct3d->cxl_dstate, CXL_T3_MSIX_EVENT_START);

    /* Set default value for patrol scrub attributes */
    ct3d->patrol_scrub_attrs.scrub_cycle_cap =
                           CXL_MEMDEV_PS_SCRUB_CYCLE_CHANGE_CAP_DEFAULT |
                           CXL_MEMDEV_PS_SCRUB_REALTIME_REPORT_CAP_DEFAULT;
    ct3d->patrol_scrub_attrs.scrub_cycle =
                           CXL_MEMDEV_PS_CUR_SCRUB_CYCLE_DEFAULT |
                           (CXL_MEMDEV_PS_MIN_SCRUB_CYCLE_DEFAULT << 8);
    ct3d->patrol_scrub_attrs.scrub_flags = CXL_MEMDEV_PS_ENABLE_DEFAULT;

    /* Set default value for DDR5 ECS read attributes */
    ct3d->ecs_attrs.ecs_log_cap = CXL_ECS_LOG_ENTRY_TYPE_DEFAULT;
    for (count = 0; count < CXL_ECS_NUM_MEDIA_FRUS; count++) {
        ct3d->ecs_attrs.fru_attrs[count].ecs_cap =
                            CXL_ECS_REALTIME_REPORT_CAP_DEFAULT;
        ct3d->ecs_attrs.fru_attrs[count].ecs_config =
                            CXL_ECS_THRESHOLD_COUNT_DEFAULT |
                            (CXL_ECS_MODE_DEFAULT << 3);
        /* Reserved */
        ct3d->ecs_attrs.fru_attrs[count].ecs_flags = 0;
    }

    return;

err_release_cdat:
    cxl_doe_cdat_release(cxl_cstate);
err_msix_uninit:
    msix_uninit_exclusive_bar(pci_dev);
err_free_special_ops:
    g_free(regs->special_ops);
    if (ct3d->dc.host_dc) {
        cxl_destroy_dc_regions(ct3d);
        address_space_destroy(&ct3d->dc.host_dc_as);
    }
    if (ct3d->hostpmem) {
        address_space_destroy(&ct3d->hostpmem_as);
    }
    if (ct3d->hostvmem) {
        address_space_destroy(&ct3d->hostvmem_as);
    }
}

static void ct3_exit(PCIDevice *pci_dev)
{
    CXLType3Dev *ct3d = CXL_TYPE3(pci_dev);
    CXLComponentState *cxl_cstate = &ct3d->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;

    pcie_aer_exit(pci_dev);
    cxl_doe_cdat_release(cxl_cstate);
    msix_uninit_exclusive_bar(pci_dev);
    g_free(regs->special_ops);
    cxl_destroy_cci(&ct3d->cci);
    if (ct3d->dc.host_dc) {
        cxl_destroy_dc_regions(ct3d);
        address_space_destroy(&ct3d->dc.host_dc_as);
    }
    if (ct3d->hostpmem) {
        address_space_destroy(&ct3d->hostpmem_as);
    }
    if (ct3d->hostvmem) {
        address_space_destroy(&ct3d->hostvmem_as);
    }
}

/*
 * Mark the DPA range [dpa, dap + len - 1] to be backed and accessible. This
 * happens when a DC extent is added and accepted by the host.
 */
void ct3_set_region_block_backed(CXLType3Dev *ct3d, uint64_t dpa,
                                 uint64_t len)
{
    CXLDCRegion *region;

    region = cxl_find_dc_region(ct3d, dpa, len);
    if (!region) {
        return;
    }

    QEMU_LOCK_GUARD(&region->bitmap_lock);
    bitmap_set(region->blk_bitmap, (dpa - region->base) / region->block_size,
               len / region->block_size);
}

/*
 * Check whether the DPA range [dpa, dpa + len - 1] is backed with DC extents.
 * Used when validating read/write to dc regions
 */
bool ct3_test_region_block_backed(CXLType3Dev *ct3d, uint64_t dpa,
                                  uint64_t len)
{
    CXLDCRegion *region;
    uint64_t nbits;
    long nr;

    region = cxl_find_dc_region(ct3d, dpa, len);
    if (!region) {
        return false;
    }

    nr = (dpa - region->base) / region->block_size;
    nbits = DIV_ROUND_UP(len, region->block_size);
    /*
     * if bits between [dpa, dpa + len) are all 1s, meaning the DPA range is
     * backed with DC extents, return true; else return false.
     */
    QEMU_LOCK_GUARD(&region->bitmap_lock);
    return find_next_zero_bit(region->blk_bitmap, nr + nbits, nr) == nr + nbits;
}

/*
 * Mark the DPA range [dpa, dap + len - 1] to be unbacked and inaccessible.
 * This happens when a dc extent is released by the host.
 */
void ct3_clear_region_block_backed(CXLType3Dev *ct3d, uint64_t dpa,
                                   uint64_t len)
{
    CXLDCRegion *region;
    uint64_t nbits;
    long nr;

    region = cxl_find_dc_region(ct3d, dpa, len);
    if (!region) {
        return;
    }

    nr = (dpa - region->base) / region->block_size;
    nbits = len / region->block_size;
    QEMU_LOCK_GUARD(&region->bitmap_lock);
    bitmap_clear(region->blk_bitmap, nr, nbits);
}

static bool cxl_type3_dpa(CXLType3Dev *ct3d, hwaddr host_addr, uint64_t *dpa)
{
    int hdm_inc = R_CXL_HDM_DECODER1_BASE_LO - R_CXL_HDM_DECODER0_BASE_LO;
    uint32_t *cache_mem = ct3d->cxl_cstate.crb.cache_mem_registers;
    unsigned int hdm_count;
    uint32_t cap;
    uint64_t dpa_base = 0;
    int i;

    cap = ldl_le_p(cache_mem + R_CXL_HDM_DECODER_CAPABILITY);
    hdm_count = cxl_decoder_count_dec(FIELD_EX32(cap,
                                                 CXL_HDM_DECODER_CAPABILITY,
                                                 DECODER_COUNT));

    for (i = 0; i < hdm_count; i++) {
        uint64_t decoder_base, decoder_size, hpa_offset, skip;
        uint32_t hdm_ctrl, low, high;
        int ig, iw;

        low = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_BASE_LO + i * hdm_inc);
        high = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_BASE_HI + i * hdm_inc);
        decoder_base = ((uint64_t)high << 32) | (low & 0xf0000000);

        low = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_SIZE_LO + i * hdm_inc);
        high = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_SIZE_HI + i * hdm_inc);
        decoder_size = ((uint64_t)high << 32) | (low & 0xf0000000);

        low = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_DPA_SKIP_LO +
                       i * hdm_inc);
        high = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_DPA_SKIP_HI +
                        i * hdm_inc);
        skip = ((uint64_t)high << 32) | (low & 0xf0000000);
        dpa_base += skip;

        hpa_offset = (uint64_t)host_addr - decoder_base;

        hdm_ctrl = ldl_le_p(cache_mem + R_CXL_HDM_DECODER0_CTRL + i * hdm_inc);
        iw = FIELD_EX32(hdm_ctrl, CXL_HDM_DECODER0_CTRL, IW);
        ig = FIELD_EX32(hdm_ctrl, CXL_HDM_DECODER0_CTRL, IG);
        if (!FIELD_EX32(hdm_ctrl, CXL_HDM_DECODER0_CTRL, COMMITTED)) {
            return false;
        }
        if (((uint64_t)host_addr < decoder_base) ||
            (hpa_offset >= decoder_size)) {
            int decoded_iw = cxl_interleave_ways_dec(iw, &error_fatal);

            if (decoded_iw == 0) {
                return false;
            }

            dpa_base += decoder_size / decoded_iw;
            continue;
        }

        if (iw < 8) {
            *dpa = dpa_base +
                ((MAKE_64BIT_MASK(0, 8 + ig) & hpa_offset) |
                 ((MAKE_64BIT_MASK(8 + ig + iw, 64 - 8 - ig - iw) & hpa_offset)
                  >> iw));
        } else {
            *dpa = dpa_base +
                ((MAKE_64BIT_MASK(0, 8 + ig) & hpa_offset) |
                 ((((MAKE_64BIT_MASK(ig + iw, 64 - ig - iw) & hpa_offset)
                   >> (ig + iw)) / 3) << (ig + 8)));
        }

        return true;
    }
    return false;
}

static int cxl_type3_hpa_to_as_and_dpa(CXLType3Dev *ct3d,
                                       hwaddr host_addr,
                                       unsigned int size,
                                       AddressSpace **as,
                                       uint64_t *dpa_offset)
{
    MemoryRegion *vmr = NULL, *pmr = NULL, *dc_mr = NULL;
    uint64_t vmr_size = 0, pmr_size = 0, dc_size = 0;

    if (ct3d->hostvmem) {
        vmr = host_memory_backend_get_memory(ct3d->hostvmem);
        vmr_size = memory_region_size(vmr);
    }
    if (ct3d->hostpmem) {
        pmr = host_memory_backend_get_memory(ct3d->hostpmem);
        pmr_size = memory_region_size(pmr);
    }
    if (ct3d->dc.host_dc) {
        dc_mr = host_memory_backend_get_memory(ct3d->dc.host_dc);
        dc_size = memory_region_size(dc_mr);
    }

    if (!vmr && !pmr && !dc_mr) {
        return -ENODEV;
    }

    if (!cxl_type3_dpa(ct3d, host_addr, dpa_offset)) {
        return -EINVAL;
    }

    if (*dpa_offset >= vmr_size + pmr_size + dc_size) {
        return -EINVAL;
    }

    if (*dpa_offset < vmr_size) {
        *as = &ct3d->hostvmem_as;
    } else if (*dpa_offset < vmr_size + pmr_size) {
        *as = &ct3d->hostpmem_as;
        *dpa_offset -= vmr_size;
    } else {
        if (!ct3_test_region_block_backed(ct3d, *dpa_offset, size)) {
            return -ENODEV;
        }

        *as = &ct3d->dc.host_dc_as;
        *dpa_offset -= (vmr_size + pmr_size);
    }

    return 0;
}

/* CXLMemSim Integration */
#define CXL_MEMSIM_DEFAULT_HOST "127.0.0.1"
#define CXL_MEMSIM_DEFAULT_PORT 9999

/* Operation type constants - matching server */
#define CXL_OP_READ         0
#define CXL_OP_WRITE        1
#define CXL_OP_GET_SHM_INFO 2
#define CXL_OP_ATOMIC_FAA   3   /* Fetch-and-Add */
#define CXL_OP_ATOMIC_CAS   4   /* Compare-and-Swap */
#define CXL_OP_FENCE        5   /* Memory fence */
#define CXL_OP_LSA_READ     6   /* Label Storage Area read */
#define CXL_OP_LSA_WRITE    7   /* Label Storage Area write */

/* LSA address offset - distinguishes LSA from regular memory */
#define CXL_LSA_ADDR_OFFSET 0xFFFF000000000000ULL

/* Request structure - matching server's ServerRequest (must be packed for wire protocol) */
typedef struct __attribute__((packed)) {
    uint8_t op_type;       /* 0=READ, 1=WRITE, 2=GET_SHM_INFO, 3=FAA, 4=CAS, 5=FENCE */
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint64_t value;        /* Value for FAA (add value) or CAS (desired value) */
    uint64_t expected;     /* Expected value for CAS operation */
    uint8_t data[64];
} CXLMemSimRequest;

/* Response structure - matching server's ServerResponse (must be packed for wire protocol) */
typedef struct __attribute__((packed)) {
    uint8_t status;
    uint64_t latency_ns;
    uint64_t old_value;    /* Previous value returned by atomic operations */
    uint8_t data[64];
} CXLMemSimResponse;

/* ============================================================================
 * PGAS Shared Memory Protocol (matches cxl_backend.h exactly)
 * ============================================================================ */
#define CXL_SHM_DEFAULT_NAME "/cxlmemsim_pgas"
#define CXL_SHM_MAGIC        0x43584C53484D454DULL  /* "CXLSHMEM" */
#define CXL_SHM_VERSION      1
#define CXL_SHM_MAX_SLOTS    64
#define CXL_SHM_CACHELINE_SIZE 64

/* Request types */
#define CXL_SHM_REQ_NONE          0
#define CXL_SHM_REQ_READ          1
#define CXL_SHM_REQ_WRITE         2
#define CXL_SHM_REQ_ATOMIC_FAA    3
#define CXL_SHM_REQ_ATOMIC_CAS    4
#define CXL_SHM_REQ_FENCE         5
#define CXL_SHM_REQ_READ_META     6   /* Read with metadata */
#define CXL_SHM_REQ_WRITE_META    7   /* Write with metadata */
#define CXL_SHM_REQ_GET_META      8   /* Get metadata only */
#define CXL_SHM_REQ_SET_META      9   /* Set metadata only */

/* Response status */
#define CXL_SHM_RESP_NONE     0
#define CXL_SHM_RESP_OK       1
#define CXL_SHM_RESP_ERROR    2

/* MESI Cache States */
#define CXL_CACHE_INVALID     0
#define CXL_CACHE_SHARED      1
#define CXL_CACHE_EXCLUSIVE   2
#define CXL_CACHE_MODIFIED    3

/* Metadata flags */
#define CXL_META_FLAG_DIRTY   0x01
#define CXL_META_FLAG_LOCKED  0x02
#define CXL_META_FLAG_PINNED  0x04

/* Header flags */
#define CXL_SHM_FLAG_METADATA_ENABLED  0x01

/* Cacheline metadata structure (64 bytes) - compatible with cxl_backend.h */
typedef struct __attribute__((packed)) {
    uint8_t cache_state;        /* MESI state */
    uint8_t owner_id;           /* Current owner host/thread ID */
    uint16_t sharers_bitmap;    /* Bitmap of hosts/threads sharing this line */
    uint32_t access_count;      /* Number of accesses */
    uint64_t last_access_time;  /* Timestamp of last access */
    uint64_t virtual_addr;      /* Virtual address mapping */
    uint64_t physical_addr;     /* Physical address */
    uint32_t version;           /* Version number for coherency */
    uint8_t flags;              /* Various flags (dirty, locked, etc.) */
    uint8_t reserved[23];       /* Reserved for future use */
} CXLCachelineMetadata;

/* Shared memory slot for request/response (256-byte aligned, matches cxl_shm_slot_t) */
typedef struct __attribute__((aligned(256))) {
    volatile uint32_t req_type;      /* Request type */
    volatile uint32_t resp_status;   /* Response status */
    volatile uint64_t addr;          /* Address for operation */
    volatile uint64_t size;          /* Size of operation */
    volatile uint64_t value;         /* Value for atomics */
    volatile uint64_t expected;      /* Expected value for CAS */
    volatile uint64_t latency_ns;    /* Simulated latency */
    volatile uint64_t timestamp;     /* Request timestamp */
    uint8_t data[CXL_SHM_CACHELINE_SIZE];  /* Data buffer (64 bytes) */
    CXLCachelineMetadata metadata;         /* Metadata buffer (64 bytes) */
} CXLShmSlot;

/* Shared memory header (matches cxl_shm_header_t) */
typedef struct __attribute__((aligned(64))) {
    uint64_t magic;                  /* Magic number for validation */
    uint32_t version;                /* Protocol version */
    uint32_t num_slots;              /* Number of request slots */
    volatile uint32_t server_ready;  /* Server is ready flag */
    uint32_t flags;                  /* Header flags */
    uint64_t memory_base;            /* Base address of simulated memory */
    uint64_t memory_size;            /* Size of simulated memory */
    uint64_t num_cachelines;         /* Number of cachelines (memory_size/64) */
    uint32_t metadata_enabled;       /* 1 if metadata transfer is enabled */
    uint32_t entry_size;             /* Size of each entry (64 or 128 bytes) */
    uint8_t padding[64 - 56];        /* Pad header to 64 bytes */
    CXLShmSlot slots[];              /* Request/response slots */
} CXLShmHeader;

/* Size calculation macro */
#define CXL_SHM_HEADER_SIZE(nslots) \
    (sizeof(CXLShmHeader) + (nslots) * sizeof(CXLShmSlot))

/* Transport mode enum */
enum CXLTransportMode {
    CXL_TRANSPORT_TCP = 0,
    CXL_TRANSPORT_RDMA = 1,
    CXL_TRANSPORT_SHM = 2
};

/* Include RDMA support header */
#include "cxl_type3_rdma.h"

static struct {
    bool enabled;
    bool initialized;
    char host[256];
    int port;
    enum CXLTransportMode transport_mode;
    int socket_fd;
    bool connected;
    pthread_mutex_t lock;
    uint64_t stats_reads;
    uint64_t stats_writes;
    uint64_t stats_atomics;
    uint64_t stats_fences;
    /* PGAS shared memory fields */
    char shm_name[256];
    int shm_fd;
    CXLShmHeader *shm_header;
    void *shm_memory;
    size_t shm_memory_size;
    int shm_slot_id;
    /* Active latency injection */
    bool latency_inject;          /* Enable active latency enforcement */
    uint64_t latency_inject_min;  /* Minimum latency to bother injecting (ns) */
    uint64_t stats_injected_ns;   /* Total injected delay (ns) */
    uint64_t stats_inject_count;  /* Number of times delay was injected */
} g_memsim = {
    .enabled = false,
    .initialized = false,
    .transport_mode = CXL_TRANSPORT_TCP,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .socket_fd = -1,
    .shm_fd = -1,
    .shm_header = NULL,
    .shm_slot_id = 0,
    .latency_inject = false,
    .latency_inject_min = 0,
    .stats_injected_ns = 0,
    .stats_inject_count = 0,
};

/*
 * Active latency enforcement: spin-wait until the target latency has elapsed.
 *
 * call_start_ns: CLOCK_MONOTONIC timestamp taken BEFORE the IPC request.
 * target_latency_ns: the simulated CXL latency returned by the server.
 *
 * If the IPC round-trip already consumed more time than target_latency_ns,
 * no extra delay is added.  Otherwise we busy-wait for the remainder so
 * that the total wall-clock time of the memory access is >= target_latency_ns.
 *
 * We use a spin loop rather than nanosleep() because:
 *   1. CXL latencies are typically 80-300 ns — too short for the kernel
 *      scheduler to handle accurately (nanosleep minimum ~50 us on Linux).
 *   2. We WANT the vCPU thread to stall: this is the memory access path,
 *      and stalling it is semantically correct for simulating slow memory.
 */
static void cxl_memsim_inject_latency(uint64_t call_start_ns,
                                      uint64_t target_latency_ns)
{
    struct timespec now;

    if (!g_memsim.latency_inject || target_latency_ns < g_memsim.latency_inject_min) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec
                        - call_start_ns;

    if (elapsed_ns >= target_latency_ns) {
        return;  /* IPC already took longer than the simulated latency */
    }

    uint64_t remaining_ns = target_latency_ns - elapsed_ns;

    /*
     * For long delays (>10 us) use nanosleep for the bulk, then spin
     * for the last microsecond to get precise timing.
     */
    if (remaining_ns > 10000) {
        struct timespec sleep_ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)(remaining_ns - 1000)   /* leave 1 us for spin */
        };
        nanosleep(&sleep_ts, NULL);
    }

    /* Spin-wait for the final portion */
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t total_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec
                          - call_start_ns;
        if (total_ns >= target_latency_ns) {
            break;
        }
    }

    g_memsim.stats_injected_ns += remaining_ns;
    g_memsim.stats_inject_count++;
}

static void cxl_memsim_init(void) {
    /* Use double-checked locking for thread safety */
    if (g_memsim.initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_memsim.lock);
    
    /* Check again under lock */
    if (g_memsim.initialized) {
        pthread_mutex_unlock(&g_memsim.lock);
        return;
    }
    
    info_report("CXL Type3: Initializing CXLMemSim integration");
    
    const char *host = getenv("CXL_MEMSIM_HOST");
    const char *port_str = getenv("CXL_MEMSIM_PORT");
    const char *transport = getenv("CXL_TRANSPORT_MODE");
    if (!transport || !transport[0]) {
        transport = getenv("CXL_MEMSIM_TRANSPORT");
    }
    const char *rdma_server = getenv("CXL_MEMSIM_RDMA_SERVER");
    const char *rdma_port = getenv("CXL_MEMSIM_RDMA_PORT");

    if (!host || !host[0]) {
        host = CXL_MEMSIM_DEFAULT_HOST;
    }

    int port = CXL_MEMSIM_DEFAULT_PORT;
    if (port_str && port_str[0]) {
        port = atoi(port_str);
    }

    /* Default to SHM transport when unspecified */
    if (!transport || !transport[0]) {
        transport = "shm";
    }

    /* Determine transport mode */
    if (transport) {
        if (strcmp(transport, "rdma") == 0) {
            g_memsim.transport_mode = CXL_TRANSPORT_RDMA;
            /* Use RDMA-specific server and port if available */
            if (rdma_server && rdma_server[0]) {
                g_strlcpy(g_memsim.host, rdma_server, sizeof(g_memsim.host));
            } else {
                g_strlcpy(g_memsim.host, host, sizeof(g_memsim.host));
            }
            if (rdma_port) {
                g_memsim.port = atoi(rdma_port);
            } else {
                g_memsim.port = port;
            }
            g_memsim.enabled = true;
            g_memsim.initialized = true;
            info_report("CXL Type3: CXLMemSim RDMA mode - %s:%d", g_memsim.host, g_memsim.port);
        } else if (strcmp(transport, "shm") == 0 || strcmp(transport, "pgas") == 0) {
            g_memsim.transport_mode = CXL_TRANSPORT_SHM;
            /* Get shared memory name from environment */
            const char *shm_name = getenv("CXL_PGAS_SHM");
            if (shm_name && shm_name[0]) {
                g_strlcpy(g_memsim.shm_name, shm_name, sizeof(g_memsim.shm_name));
            } else {
                g_strlcpy(g_memsim.shm_name, CXL_SHM_DEFAULT_NAME, sizeof(g_memsim.shm_name));
            }
            g_memsim.enabled = true;  /* Enable for SHM mode */
            g_memsim.initialized = true;
            info_report("CXL Type3: CXLMemSim SHM mode - %s", g_memsim.shm_name);
        } else {
            /* TCP mode */
            g_strlcpy(g_memsim.host, host, sizeof(g_memsim.host));
            g_memsim.port = port;
            g_memsim.enabled = true;
            g_memsim.initialized = true;
            info_report("CXL Type3: CXLMemSim TCP mode - %s:%d", g_memsim.host, g_memsim.port);
        }
    } else {
        /* Should not reach here; default to SHM semantics */
        g_memsim.transport_mode = CXL_TRANSPORT_SHM;
        g_memsim.enabled = false;
        g_memsim.initialized = true;
        info_report("CXL Type3: CXLMemSim SHM mode (no network connection)");
    }

    /* Parse active latency injection settings.
     *   CXL_LATENCY_INJECT=1          — enable active delay enforcement
     *   CXL_LATENCY_INJECT_MIN_NS=80  — skip injection below this threshold (default 0)
     */
    const char *inject_env = getenv("CXL_LATENCY_INJECT");
    if (inject_env && (strcmp(inject_env, "1") == 0 || strcmp(inject_env, "true") == 0)) {
        g_memsim.latency_inject = true;
        const char *min_ns_env = getenv("CXL_LATENCY_INJECT_MIN_NS");
        if (min_ns_env && min_ns_env[0]) {
            g_memsim.latency_inject_min = strtoull(min_ns_env, NULL, 10);
        }
        info_report("CXL Type3: Active latency injection ENABLED (min=%lu ns)",
                    (unsigned long)g_memsim.latency_inject_min);
    } else {
        info_report("CXL Type3: Active latency injection disabled "
                    "(set CXL_LATENCY_INJECT=1 to enable)");
    }

    pthread_mutex_unlock(&g_memsim.lock);
}

/* Connect function - assumes lock is already held */
static int cxl_memsim_connect_locked(void) {
    struct sockaddr_in server_addr = {0};
    
    if (g_memsim.connected) {
        return 0;
    }

    if (g_memsim.transport_mode == CXL_TRANSPORT_SHM) {
        /* Connect to shared memory */
        g_memsim.shm_fd = shm_open(g_memsim.shm_name, O_RDWR, 0666);
        if (g_memsim.shm_fd < 0) {
            error_report("CXL Type3: Failed to open shared memory %s: %s",
                        g_memsim.shm_name, strerror(errno));
            return -1;
        }

        /* Get size from stat */
        struct stat sb;
        if (fstat(g_memsim.shm_fd, &sb) < 0) {
            error_report("CXL Type3: fstat failed on shm: %s", strerror(errno));
            close(g_memsim.shm_fd);
            g_memsim.shm_fd = -1;
            return -1;
        }

        size_t shm_size = sb.st_size;

        /* Map shared memory */
        void *mapped = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           g_memsim.shm_fd, 0);
        if (mapped == MAP_FAILED) {
            error_report("CXL Type3: mmap failed on shm: %s", strerror(errno));
            close(g_memsim.shm_fd);
            g_memsim.shm_fd = -1;
            return -1;
        }

        g_memsim.shm_header = (CXLShmHeader *)mapped;

        /* Validate magic number */
        if (g_memsim.shm_header->magic != CXL_SHM_MAGIC) {
            error_report("CXL Type3: SHM invalid magic (got 0x%lx, expected 0x%lx)",
                        (unsigned long)g_memsim.shm_header->magic,
                        (unsigned long)CXL_SHM_MAGIC);
            munmap(mapped, shm_size);
            close(g_memsim.shm_fd);
            g_memsim.shm_fd = -1;
            g_memsim.shm_header = NULL;
            return -1;
        }

        /* Wait for server ready */
        int retries = 100;
        while (!__atomic_load_n(&g_memsim.shm_header->server_ready, __ATOMIC_ACQUIRE) && retries > 0) {
            usleep(10000);  /* 10ms */
            retries--;
        }

        if (retries == 0) {
            error_report("CXL Type3: SHM server not ready after timeout");
            munmap(mapped, shm_size);
            close(g_memsim.shm_fd);
            g_memsim.shm_fd = -1;
            g_memsim.shm_header = NULL;
            return -1;
        }

        /* Calculate memory area pointer (after header + slots) */
        size_t header_size = CXL_SHM_HEADER_SIZE(g_memsim.shm_header->num_slots);
        g_memsim.shm_memory = (uint8_t *)mapped + header_size;
        g_memsim.shm_memory_size = g_memsim.shm_header->memory_size;

        /* Assign slot based on process/thread ID */
        g_memsim.shm_slot_id = getpid() % g_memsim.shm_header->num_slots;

        g_memsim.connected = true;
        info_report("CXL Type3: SHM connected to %s (mem_size=%lu, slot=%d, "
                   "num_cachelines=%lu, metadata=%s)",
                   g_memsim.shm_name,
                   (unsigned long)g_memsim.shm_memory_size,
                   g_memsim.shm_slot_id,
                   (unsigned long)g_memsim.shm_header->num_cachelines,
                   g_memsim.shm_header->metadata_enabled ? "enabled" : "disabled");
        return 0;
    }

    /* Try RDMA connection if configured */
    if (g_memsim.transport_mode == CXL_TRANSPORT_RDMA) {
        if (cxl_memsim_rdma_available()) {
            /* Use TCP port 9999 for RDMA-aware connections */
            int connection_port = 9999;
            info_report("CXL Type3: Attempting RDMA connection to %s:%d", 
                       g_memsim.host, connection_port);
            int ret = cxl_memsim_rdma_connect(g_memsim.host, connection_port);
            if (ret == 0) {
                g_memsim.connected = true;
                info_report("CXL Type3: RDMA-mode connection successful");
                return 0;
            }
            error_report("CXL Type3: RDMA-mode connection failed, falling back to TCP");
        } else {
            error_report("CXL Type3: RDMA not available, falling back to TCP");
        }
        g_memsim.transport_mode = CXL_TRANSPORT_TCP;
    }
    
    /* Add debugging to track connection source with backtrace */
    static int connection_count = 0;
    connection_count++;
    info_report("CXL Type3: Connection attempt #%d to CXLMemSim server at %s:%d",
                connection_count, g_memsim.host, g_memsim.port);
    
    g_memsim.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_memsim.socket_fd < 0) {
        return -1;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_memsim.port);
    inet_pton(AF_INET, g_memsim.host, &server_addr.sin_addr);
    
    if (connect(g_memsim.socket_fd, (struct sockaddr *)&server_addr, 
                sizeof(server_addr)) < 0) {
        close(g_memsim.socket_fd);
        g_memsim.socket_fd = -1;
        error_report("CXL Type3: Failed to connect to CXLMemSim");
        return -1;
    }
    
    g_memsim.connected = true;
    info_report("CXL Type3: Successfully connected to CXLMemSim (fd=%d)", 
                g_memsim.socket_fd);
    
    return 0;
}

/* SHM request function - assumes lock is held */
static int cxl_memsim_shm_request(uint8_t op, uint64_t addr, uint64_t size,
                                  void *data, uint64_t value, uint64_t expected,
                                  CXLMemSimResponse *resp) {
    if (!g_memsim.shm_header || !g_memsim.connected) {
        return -1;
    }

    CXLShmSlot *slot = &g_memsim.shm_header->slots[g_memsim.shm_slot_id];

    /* Wait for slot to be free */
    int retries = 1000;
    while (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) != CXL_SHM_REQ_NONE && retries > 0) {
        usleep(100);
        retries--;
    }

    if (retries == 0) {
        error_report("CXL Type3: SHM slot busy timeout");
        return -1;
    }

    /* Fill request */
    slot->addr = addr;
    slot->size = size;
    slot->timestamp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    slot->value = value;
    slot->expected = expected;

    if (op == CXL_OP_WRITE && data) {
        memcpy((void *)slot->data, data, MIN(size, CXL_SHM_CACHELINE_SIZE));
    }

    /* Memory barrier before setting request type */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* Set request type (triggers server processing) */
    uint32_t shm_req_type;
    switch (op) {
        case CXL_OP_READ:       shm_req_type = CXL_SHM_REQ_READ; break;
        case CXL_OP_WRITE:      shm_req_type = CXL_SHM_REQ_WRITE; break;
        case CXL_OP_ATOMIC_FAA: shm_req_type = CXL_SHM_REQ_ATOMIC_FAA; break;
        case CXL_OP_ATOMIC_CAS: shm_req_type = CXL_SHM_REQ_ATOMIC_CAS; break;
        case CXL_OP_FENCE:      shm_req_type = CXL_SHM_REQ_FENCE; break;
        default:                shm_req_type = CXL_SHM_REQ_NONE; break;
    }
    __atomic_store_n(&slot->req_type, shm_req_type, __ATOMIC_RELEASE);

    /* Wait for response */
    retries = 10000;
    while (__atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE) == CXL_SHM_RESP_NONE && retries > 0) {
        usleep(10);
        retries--;
    }

    if (retries == 0) {
        error_report("CXL Type3: SHM response timeout");
        return -1;
    }

    /* Memory barrier before reading response */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    /* Read response */
    resp->status = (slot->resp_status == CXL_SHM_RESP_OK) ? 0 : 1;
    resp->latency_ns = slot->latency_ns;

    if (op == CXL_OP_READ) {
        memcpy(resp->data, (void *)slot->data, MIN(size, CXL_SHM_CACHELINE_SIZE));
    } else if (op == CXL_OP_ATOMIC_FAA || op == CXL_OP_ATOMIC_CAS) {
        /* For atomic ops, old_value is stored in slot->value after server processes */
        resp->old_value = slot->value;
    }

    /* Clear response status for next request */
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);

    return 0;
}

/* Extended request function with atomic operation support */
static int cxl_memsim_request_ext(uint8_t op, uint64_t addr, uint64_t size,
                                  void *data, uint64_t value, uint64_t expected,
                                  CXLMemSimResponse *resp) {
    static int request_count = 0;
    request_count++;

    pthread_mutex_lock(&g_memsim.lock);

    /* Debug logging */
    if (request_count <= 10) {
        const char *op_name;
        switch (op) {
            case CXL_OP_READ:       op_name = "READ"; break;
            case CXL_OP_WRITE:      op_name = "WRITE"; break;
            case CXL_OP_ATOMIC_FAA: op_name = "FAA"; break;
            case CXL_OP_ATOMIC_CAS: op_name = "CAS"; break;
            case CXL_OP_FENCE:      op_name = "FENCE"; break;
            case CXL_OP_LSA_READ:   op_name = "LSA_READ"; break;
            case CXL_OP_LSA_WRITE:  op_name = "LSA_WRITE"; break;
            default:                op_name = "UNKNOWN"; break;
        }
        info_report("CXL Type3: Request #%d (op=%s, connected=%d, transport=%s)",
                    request_count, op_name, g_memsim.connected,
                    g_memsim.transport_mode == CXL_TRANSPORT_SHM ? "PGAS" :
                    g_memsim.transport_mode == CXL_TRANSPORT_RDMA ? "RDMA" : "TCP");
    }

    /* Periodic latency injection stats (every 100k requests) */
    if (g_memsim.latency_inject && (request_count % 100000) == 0) {
        info_report("CXL Type3: Latency injection stats @ %d ops: "
                    "injected=%lu times, total_delay=%lu us, "
                    "avg=%.1f ns/injection",
                    request_count,
                    (unsigned long)g_memsim.stats_inject_count,
                    (unsigned long)(g_memsim.stats_injected_ns / 1000),
                    g_memsim.stats_inject_count > 0
                        ? (double)g_memsim.stats_injected_ns / g_memsim.stats_inject_count
                        : 0.0);
    }

    /* Ensure connected */
    if (!g_memsim.connected) {
        if (cxl_memsim_connect_locked() < 0) {
            pthread_mutex_unlock(&g_memsim.lock);
            return -1;
        }
    }

    int ret = -1;

    /* SHM mode */
    if (g_memsim.transport_mode == CXL_TRANSPORT_SHM) {
        ret = cxl_memsim_shm_request(op, addr, size, data, value, expected, resp);
        if (ret == 0) {
            switch (op) {
                case CXL_OP_READ:       g_memsim.stats_reads++; break;
                case CXL_OP_WRITE:      g_memsim.stats_writes++; break;
                case CXL_OP_ATOMIC_FAA:
                case CXL_OP_ATOMIC_CAS: g_memsim.stats_atomics++; break;
                case CXL_OP_FENCE:      g_memsim.stats_fences++; break;
            }
        }
        pthread_mutex_unlock(&g_memsim.lock);
        return ret;
    }

    /* RDMA mode */
    if (g_memsim.transport_mode == CXL_TRANSPORT_RDMA) {
        ret = cxl_memsim_rdma_request(op, addr, size, data, resp);
        if (ret < 0) {
            g_memsim.connected = false;
        } else {
            if (op == CXL_OP_READ) g_memsim.stats_reads++;
            else if (op == CXL_OP_WRITE) g_memsim.stats_writes++;
            else g_memsim.stats_atomics++;
        }
        pthread_mutex_unlock(&g_memsim.lock);
        return ret;
    }

    /* TCP mode */
    CXLMemSimRequest req = {
        .op_type = op,
        .addr = addr,
        .size = size,
        .timestamp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        .value = value,
        .expected = expected
    };

    if ((op == CXL_OP_WRITE || op == CXL_OP_LSA_WRITE) && data) {
        memcpy(req.data, data, MIN(size, 64));
    }

    if (send(g_memsim.socket_fd, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) {
        error_report("CXL Type3: Failed to send request to CXLMemSim");
        g_memsim.connected = false;
        close(g_memsim.socket_fd);
        g_memsim.socket_fd = -1;
        pthread_mutex_unlock(&g_memsim.lock);
        return -1;
    }

    if (recv(g_memsim.socket_fd, resp, sizeof(*resp), MSG_WAITALL) != sizeof(*resp)) {
        error_report("CXL Type3: Failed to receive response from CXLMemSim");
        g_memsim.connected = false;
        close(g_memsim.socket_fd);
        g_memsim.socket_fd = -1;
        pthread_mutex_unlock(&g_memsim.lock);
        return -1;
    }

    switch (op) {
        case CXL_OP_READ:       g_memsim.stats_reads++; break;
        case CXL_OP_WRITE:      g_memsim.stats_writes++; break;
        case CXL_OP_ATOMIC_FAA:
        case CXL_OP_ATOMIC_CAS: g_memsim.stats_atomics++; break;
        case CXL_OP_FENCE:      g_memsim.stats_fences++; break;
    }

    pthread_mutex_unlock(&g_memsim.lock);
    return 0;
}

/* Simple request wrapper for backward compatibility */
static int cxl_memsim_request(uint8_t op, uint64_t addr, uint64_t size,
                              void *data, CXLMemSimResponse *resp) {
    return cxl_memsim_request_ext(op, addr, size, data, 0, 0, resp);
}

/* Atomic Fetch-and-Add operation */
static int __attribute__((unused))
cxl_memsim_atomic_faa(uint64_t addr, uint64_t add_value, uint64_t *old_value) {
    CXLMemSimResponse resp = {0};
    int ret = cxl_memsim_request_ext(CXL_OP_ATOMIC_FAA, addr, sizeof(uint64_t),
                                     NULL, add_value, 0, &resp);
    if (ret == 0 && old_value) {
        *old_value = resp.old_value;
    }
    return ret;
}

/* Atomic Compare-and-Swap operation */
static int __attribute__((unused))
cxl_memsim_atomic_cas(uint64_t addr, uint64_t expected, uint64_t desired,
                      uint64_t *old_value) {
    CXLMemSimResponse resp = {0};
    int ret = cxl_memsim_request_ext(CXL_OP_ATOMIC_CAS, addr, sizeof(uint64_t),
                                     NULL, desired, expected, &resp);
    if (ret == 0 && old_value) {
        *old_value = resp.old_value;
    }
    /* Return 0 if CAS succeeded (old_value == expected) */
    return (ret == 0 && resp.old_value == expected) ? 0 : 1;
}

/* Memory fence operation */
static void __attribute__((unused))
cxl_memsim_fence(void) {
    CXLMemSimResponse resp = {0};
    cxl_memsim_request_ext(CXL_OP_FENCE, 0, 0, NULL, 0, 0, &resp);
}

MemTxResult cxl_type3_read(PCIDevice *d, hwaddr host_addr, uint64_t *data,
                           unsigned size, MemTxAttrs attrs)
{
    CXLType3Dev *ct3d = CXL_TYPE3(d);
    uint64_t dpa_offset = 0;
    AddressSpace *as = NULL;
    int res;
    
    /* Initialize CXLMemSim on first use */
    cxl_memsim_init();
    
    /* Log all CXL Type3 reads */
    //info_report("CXL_TYPE3_READ: host_addr=0x%lx size=%u", 
    //           (unsigned long)host_addr, size);

    res = cxl_type3_hpa_to_as_and_dpa(ct3d, host_addr, size,
                                      &as, &dpa_offset);
    if (res) {
        return MEMTX_ERROR;
    }

    if (cxl_dev_media_disabled(&ct3d->cxl_dstate)) {
        qemu_guest_getrandom_nofail(data, size);
        return MEMTX_OK;
    }

    /* Forward to CXLMemSim if enabled */
    if (g_memsim.enabled && g_memsim.connected) {
        CXLMemSimResponse resp = {0};

        /* Record wall-clock time before IPC for latency compensation */
        struct timespec ts_before;
        uint64_t start_ns = 0;
        if (g_memsim.latency_inject) {
            clock_gettime(CLOCK_MONOTONIC, &ts_before);
            start_ns = (uint64_t)ts_before.tv_sec * 1000000000ULL
                     + ts_before.tv_nsec;
        }

        if (cxl_memsim_request(CXL_OP_READ, dpa_offset, size, NULL, &resp) == 0) {
            if (resp.status == 0 && size <= 64) {
                memcpy(data, resp.data, size);
            }

            /* Enforce simulated CXL latency */
            cxl_memsim_inject_latency(start_ns, resp.latency_ns);

            /* For TCP/RDMA mode, CXLMemSim is authoritative - skip local read */
            if (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
                g_memsim.transport_mode == CXL_TRANSPORT_RDMA) {
                return MEMTX_OK;
            }
        }
    }

    return address_space_read(as, dpa_offset, attrs, data, size);
}

MemTxResult cxl_type3_write(PCIDevice *d, hwaddr host_addr, uint64_t data,
                            unsigned size, MemTxAttrs attrs)
{
    CXLType3Dev *ct3d = CXL_TYPE3(d);
    uint64_t dpa_offset = 0;
    AddressSpace *as = NULL;
    int res;
    
    /* Initialize CXLMemSim on first use */
    cxl_memsim_init();
    
    /* Log all CXL Type3 writes */
    // info_report("CXL_TYPE3_WRITE: host_addr=0x%lx size=%u data=0x%lx",
    //            (unsigned long)host_addr, size, (unsigned long)data);

    res = cxl_type3_hpa_to_as_and_dpa(ct3d, host_addr, size,
                                      &as, &dpa_offset);
    if (res) {
        return MEMTX_ERROR;
    }

    if (cxl_dev_media_disabled(&ct3d->cxl_dstate)) {
        return MEMTX_OK;
    }

    /* Forward to CXLMemSim if enabled */
    if (g_memsim.enabled && g_memsim.connected) {
        CXLMemSimResponse resp = {0};

        /* Record wall-clock time before IPC for latency compensation */
        struct timespec ts_before;
        uint64_t start_ns = 0;
        if (g_memsim.latency_inject) {
            clock_gettime(CLOCK_MONOTONIC, &ts_before);
            start_ns = (uint64_t)ts_before.tv_sec * 1000000000ULL
                     + ts_before.tv_nsec;
        }

        if (cxl_memsim_request(CXL_OP_WRITE, dpa_offset, size, &data, &resp) == 0) {
            /* Enforce simulated CXL latency */
            cxl_memsim_inject_latency(start_ns, resp.latency_ns);

            /* For TCP/RDMA mode, CXLMemSim is authoritative - skip local write */
            if (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
                g_memsim.transport_mode == CXL_TRANSPORT_RDMA) {
                return MEMTX_OK;
            }
        }
    }

    /* Perform local write (SHM mode or fallback) */
    return address_space_write(as, dpa_offset, attrs, &data, size);
}

static void ct3d_reset(DeviceState *dev)
{
    CXLType3Dev *ct3d = CXL_TYPE3(dev);
    uint32_t *reg_state = ct3d->cxl_cstate.crb.cache_mem_registers;
    uint32_t *write_msk = ct3d->cxl_cstate.crb.cache_mem_regs_write_mask;

    pcie_cap_fill_link_ep_usp(PCI_DEVICE(dev), ct3d->width, ct3d->speed);
    cxl_component_register_init_common(reg_state, write_msk, CXL2_TYPE3_DEVICE);
    cxl_device_register_init_t3(ct3d, CXL_T3_MSIX_MBOX);

    /*
     * Bring up an endpoint to target with MCTP over VDM.
     * This device is emulating an MLD with single LD for now.
     */
    if (ct3d->vdm_fm_owned_ld_mctp_cci.initialized) {
        cxl_destroy_cci(&ct3d->vdm_fm_owned_ld_mctp_cci);
    }
    cxl_initialize_t3_fm_owned_ld_mctpcci(&ct3d->vdm_fm_owned_ld_mctp_cci,
                                          DEVICE(ct3d), DEVICE(ct3d),
                                          512); /* Max payload made up */
    if (ct3d->ld0_cci.initialized) {
        cxl_destroy_cci(&ct3d->ld0_cci);
    }
    cxl_initialize_t3_ld_cci(&ct3d->ld0_cci, DEVICE(ct3d), DEVICE(ct3d),
                             512); /* Max payload made up */
}

static const Property ct3_props[] = {
    DEFINE_PROP_LINK("memdev", CXLType3Dev, hostmem, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *), /* for backward compatibility */
    DEFINE_PROP_LINK("persistent-memdev", CXLType3Dev, hostpmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("volatile-memdev", CXLType3Dev, hostvmem,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_LINK("lsa", CXLType3Dev, lsa, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_UINT64("sn", CXLType3Dev, sn, UI64_NULL),
    DEFINE_PROP_STRING("cdat", CXLType3Dev, cxl_cstate.cdat.filename),
    DEFINE_PROP_UINT8("num-dc-regions", CXLType3Dev, dc.num_regions, 0),
    DEFINE_PROP_LINK("volatile-dc-memdev", CXLType3Dev, dc.host_dc,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
    DEFINE_PROP_PCIE_LINK_SPEED("x-speed", CXLType3Dev,
                                speed, PCIE_LINK_SPEED_32),
    DEFINE_PROP_PCIE_LINK_WIDTH("x-width", CXLType3Dev,
                                width, PCIE_LINK_WIDTH_16),
};

static uint64_t get_lsa_size(CXLType3Dev *ct3d)
{
    MemoryRegion *mr;

    if (!ct3d->lsa) {
        return 0;
    }

    mr = host_memory_backend_get_memory(ct3d->lsa);
    return memory_region_size(mr);
}

static void validate_lsa_access(MemoryRegion *mr, uint64_t size,
                                uint64_t offset)
{
    assert(offset + size <= memory_region_size(mr));
    assert(offset + size > offset);
}

static uint64_t get_lsa(CXLType3Dev *ct3d, void *buf, uint64_t size,
                    uint64_t offset)
{
    MemoryRegion *mr;
    void *lsa;

    if (!ct3d->lsa) {
        return 0;
    }

    /* Route LSA reads through CXLMemSim server when connected */
    if (g_memsim.enabled && g_memsim.connected &&
        (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
         g_memsim.transport_mode == CXL_TRANSPORT_RDMA)) {
        uint64_t remaining = size;
        uint64_t cur_offset = offset;
        uint8_t *dst = (uint8_t *)buf;

        while (remaining > 0) {
            uint64_t chunk = MIN(remaining, 64);
            CXLMemSimResponse resp = {0};

            if (cxl_memsim_request_ext(CXL_OP_LSA_READ, cur_offset, chunk,
                                       NULL, 0, 0, &resp) != 0 ||
                resp.status != 0) {
                error_report("CXL Type3: LSA read failed at offset 0x%lx",
                             (unsigned long)cur_offset);
                goto fallback_read;
            }
            memcpy(dst, resp.data, chunk);
            dst += chunk;
            cur_offset += chunk;
            remaining -= chunk;
        }
        return size;
    }

fallback_read:
    mr = host_memory_backend_get_memory(ct3d->lsa);
    validate_lsa_access(mr, size, offset);

    lsa = memory_region_get_ram_ptr(mr) + offset;
    memcpy(buf, lsa, size);

    return size;
}

static void set_lsa(CXLType3Dev *ct3d, const void *buf, uint64_t size,
                    uint64_t offset)
{
    MemoryRegion *mr;
    void *lsa;

    if (!ct3d->lsa) {
        return;
    }

    /* Route LSA writes through CXLMemSim server when connected */
    if (g_memsim.enabled && g_memsim.connected &&
        (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
         g_memsim.transport_mode == CXL_TRANSPORT_RDMA)) {
        uint64_t remaining = size;
        uint64_t cur_offset = offset;
        const uint8_t *src = (const uint8_t *)buf;

        while (remaining > 0) {
            uint64_t chunk = MIN(remaining, 64);
            CXLMemSimResponse resp = {0};
            uint8_t chunk_data[64];

            memcpy(chunk_data, src, chunk);
            if (cxl_memsim_request_ext(CXL_OP_LSA_WRITE, cur_offset, chunk,
                                       chunk_data, 0, 0, &resp) != 0 ||
                resp.status != 0) {
                error_report("CXL Type3: LSA write failed at offset 0x%lx",
                             (unsigned long)cur_offset);
                goto fallback_write;
            }
            src += chunk;
            cur_offset += chunk;
            remaining -= chunk;
        }
        return;
    }

fallback_write:
    mr = host_memory_backend_get_memory(ct3d->lsa);
    validate_lsa_access(mr, size, offset);

    lsa = memory_region_get_ram_ptr(mr) + offset;
    memcpy(lsa, buf, size);
    memory_region_set_dirty(mr, offset, size);

    /*
     * Just like the PMEM, if the guest is not allowed to exit gracefully, label
     * updates will get lost.
     */
}

static bool set_cacheline(CXLType3Dev *ct3d, uint64_t dpa_offset, uint8_t *data)
{
    MemoryRegion *vmr = NULL, *pmr = NULL, *dc_mr = NULL;
    AddressSpace *as;
    uint64_t vmr_size = 0, pmr_size = 0, dc_size = 0;

    if (ct3d->hostvmem) {
        vmr = host_memory_backend_get_memory(ct3d->hostvmem);
        vmr_size = memory_region_size(vmr);
    }
    if (ct3d->hostpmem) {
        pmr = host_memory_backend_get_memory(ct3d->hostpmem);
        pmr_size = memory_region_size(pmr);
    }
    if (ct3d->dc.host_dc) {
        dc_mr = host_memory_backend_get_memory(ct3d->dc.host_dc);
        dc_size = memory_region_size(dc_mr);
     }

    if (!vmr && !pmr && !dc_mr) {
        return false;
    }

    if (dpa_offset + CXL_CACHE_LINE_SIZE > vmr_size + pmr_size + dc_size) {
        return false;
    }

    if (dpa_offset < vmr_size) {
        as = &ct3d->hostvmem_as;
    } else if (dpa_offset < vmr_size + pmr_size) {
        as = &ct3d->hostpmem_as;
        dpa_offset -= vmr_size;
    } else {
        as = &ct3d->dc.host_dc_as;
        dpa_offset -= (vmr_size + pmr_size);
    }

    address_space_write(as, dpa_offset, MEMTXATTRS_UNSPECIFIED, data,
                        CXL_CACHE_LINE_SIZE);
    return true;
}

void cxl_set_poison_list_overflowed(CXLType3Dev *ct3d)
{
        ct3d->poison_list_overflowed = true;
        ct3d->poison_list_overflow_ts =
            cxl_device_get_timestamp(&ct3d->cxl_dstate);
}

void cxl_clear_poison_list_overflowed(CXLType3Dev *ct3d)
{
    ct3d->poison_list_overflowed = false;
    ct3d->poison_list_overflow_ts = 0;
}

void qmp_cxl_inject_poison(const char *path, uint64_t start, uint64_t length,
                           Error **errp)
{
    Object *obj = object_resolve_path(path, NULL);
    CXLType3Dev *ct3d;
    CXLPoison *p;

    if (length % 64) {
        error_setg(errp, "Poison injection must be in multiples of 64 bytes");
        return;
    }
    if (start % 64) {
        error_setg(errp, "Poison start address must be 64 byte aligned");
        return;
    }
    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }
    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }

    ct3d = CXL_TYPE3(obj);

    QLIST_FOREACH(p, &ct3d->poison_list, node) {
        if ((start < p->start + p->length) && (start + length > p->start)) {
            error_setg(errp,
                       "Overlap with existing poisoned region not supported");
            return;
        }
    }

    p = g_new0(CXLPoison, 1);
    p->length = length;
    p->start = start;
    /* Different from injected via the mbox */
    p->type = CXL_POISON_TYPE_INTERNAL;

    if (ct3d->poison_list_cnt < CXL_POISON_LIST_LIMIT) {
        QLIST_INSERT_HEAD(&ct3d->poison_list, p, node);
        ct3d->poison_list_cnt++;
    } else {
        if (!ct3d->poison_list_overflowed) {
            cxl_set_poison_list_overflowed(ct3d);
        }
        QLIST_INSERT_HEAD(&ct3d->poison_list_bkp, p, node);
    }
}

/* For uncorrectable errors include support for multiple header recording */
void qmp_cxl_inject_uncorrectable_errors(const char *path,
                                         CXLUncorErrorRecordList *errors,
                                         Error **errp)
{
    Object *obj = object_resolve_path(path, NULL);
    static PCIEAERErr err = {};
    CXLType3Dev *ct3d;
    CXLError *cxl_err;
    uint32_t *reg_state;
    uint32_t unc_err;
    bool first;

    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }

    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }

    err.status = PCI_ERR_UNC_INTN;
    err.source_id = pci_requester_id(PCI_DEVICE(obj));
    err.flags = 0;

    ct3d = CXL_TYPE3(obj);

    first = QTAILQ_EMPTY(&ct3d->error_list);
    reg_state = ct3d->cxl_cstate.crb.cache_mem_registers;
    while (errors) {
        uint32List *header = errors->value->header;
        uint8_t header_count = 0;
        int cxl_err_code;

        cxl_err_code = ct3d_qmp_uncor_err_to_cxl(errors->value->type);
        if (cxl_err_code < 0) {
            error_setg(errp, "Unknown error code");
            return;
        }

        /* If the error is masked, nothing to do here */
        if (!((1 << cxl_err_code) &
              ~ldl_le_p(reg_state + R_CXL_RAS_UNC_ERR_MASK))) {
            errors = errors->next;
            continue;
        }

        cxl_err = g_malloc0(sizeof(*cxl_err));

        cxl_err->type = cxl_err_code;
        while (header && header_count < 32) {
            cxl_err->header[header_count++] = header->value;
            header = header->next;
        }
        if (header_count > 32) {
            error_setg(errp, "Header must be 32 DWORD or less");
            return;
        }
        QTAILQ_INSERT_TAIL(&ct3d->error_list, cxl_err, node);

        errors = errors->next;
    }

    if (first && !QTAILQ_EMPTY(&ct3d->error_list)) {
        uint32_t *cache_mem = ct3d->cxl_cstate.crb.cache_mem_registers;
        uint32_t capctrl = ldl_le_p(cache_mem + R_CXL_RAS_ERR_CAP_CTRL);
        uint32_t *header_log = &cache_mem[R_CXL_RAS_ERR_HEADER0];
        int i;

        cxl_err = QTAILQ_FIRST(&ct3d->error_list);
        for (i = 0; i < CXL_RAS_ERR_HEADER_NUM; i++) {
            stl_le_p(header_log + i, cxl_err->header[i]);
        }

        capctrl = FIELD_DP32(capctrl, CXL_RAS_ERR_CAP_CTRL,
                             FIRST_ERROR_POINTER, cxl_err->type);
        stl_le_p(cache_mem + R_CXL_RAS_ERR_CAP_CTRL, capctrl);
    }

    unc_err = 0;
    QTAILQ_FOREACH(cxl_err, &ct3d->error_list, node) {
        unc_err |= (1 << cxl_err->type);
    }
    if (!unc_err) {
        return;
    }

    stl_le_p(reg_state + R_CXL_RAS_UNC_ERR_STATUS, unc_err);
    pcie_aer_inject_error(PCI_DEVICE(obj), &err);
}

void qmp_cxl_inject_correctable_error(const char *path, CxlCorErrorType type,
                                      Error **errp)
{
    static PCIEAERErr err = {};
    Object *obj = object_resolve_path(path, NULL);
    CXLType3Dev *ct3d;
    uint32_t *reg_state;
    uint32_t cor_err;
    int cxl_err_type;

    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }
    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }

    err.status = PCI_ERR_COR_INTERNAL;
    err.source_id = pci_requester_id(PCI_DEVICE(obj));
    err.flags = PCIE_AER_ERR_IS_CORRECTABLE;

    ct3d = CXL_TYPE3(obj);
    reg_state = ct3d->cxl_cstate.crb.cache_mem_registers;
    cor_err = ldl_le_p(reg_state + R_CXL_RAS_COR_ERR_STATUS);

    cxl_err_type = ct3d_qmp_cor_err_to_cxl(type);
    if (cxl_err_type < 0) {
        error_setg(errp, "Invalid COR error");
        return;
    }
    /* If the error is masked, nothting to do here */
    if (!((1 << cxl_err_type) &
          ~ldl_le_p(reg_state + R_CXL_RAS_COR_ERR_MASK))) {
        return;
    }

    cor_err |= (1 << cxl_err_type);
    stl_le_p(reg_state + R_CXL_RAS_COR_ERR_STATUS, cor_err);

    pcie_aer_inject_error(PCI_DEVICE(obj), &err);
}

void cxl_assign_event_header(CXLEventRecordHdr *hdr,
                             const QemuUUID *uuid, uint32_t flags,
                             uint8_t length, uint64_t timestamp)
{
    st24_le_p(&hdr->flags, flags);
    hdr->length = length;
    memcpy(&hdr->id, uuid, sizeof(hdr->id));
    stq_le_p(&hdr->timestamp, timestamp);
}

static const QemuUUID gen_media_uuid = {
    .data = UUID(0xfbcd0a77, 0xc260, 0x417f,
                 0x85, 0xa9, 0x08, 0x8b, 0x16, 0x21, 0xeb, 0xa6),
};

static const QemuUUID dram_uuid = {
    .data = UUID(0x601dcbb3, 0x9c06, 0x4eab, 0xb8, 0xaf,
                 0x4e, 0x9b, 0xfb, 0x5c, 0x96, 0x24),
};

static const QemuUUID memory_module_uuid = {
    .data = UUID(0xfe927475, 0xdd59, 0x4339, 0xa5, 0x86,
                 0x79, 0xba, 0xb1, 0x13, 0xb7, 0x74),
};

#define CXL_GMER_VALID_CHANNEL                          BIT(0)
#define CXL_GMER_VALID_RANK                             BIT(1)
#define CXL_GMER_VALID_DEVICE                           BIT(2)
#define CXL_GMER_VALID_COMPONENT                        BIT(3)

static int ct3d_qmp_cxl_event_log_enc(CxlEventLog log)
{
    switch (log) {
    case CXL_EVENT_LOG_INFORMATIONAL:
        return CXL_EVENT_TYPE_INFO;
    case CXL_EVENT_LOG_WARNING:
        return CXL_EVENT_TYPE_WARN;
    case CXL_EVENT_LOG_FAILURE:
        return CXL_EVENT_TYPE_FAIL;
    case CXL_EVENT_LOG_FATAL:
        return CXL_EVENT_TYPE_FATAL;
    default:
        return -EINVAL;
    }
}
/* Component ID is device specific.  Define this as a string. */
void qmp_cxl_inject_general_media_event(const char *path, CxlEventLog log,
                                        uint8_t flags, uint64_t dpa,
                                        uint8_t descriptor, uint8_t type,
                                        uint8_t transaction_type,
                                        bool has_channel, uint8_t channel,
                                        bool has_rank, uint8_t rank,
                                        bool has_device, uint32_t device,
                                        const char *component_id,
                                        Error **errp)
{
    Object *obj = object_resolve_path(path, NULL);
    CXLEventGenMedia gem;
    CXLEventRecordHdr *hdr = &gem.hdr;
    CXLDeviceState *cxlds;
    CXLType3Dev *ct3d;
    uint16_t valid_flags = 0;
    uint8_t enc_log;
    int rc;

    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }
    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }
    ct3d = CXL_TYPE3(obj);
    cxlds = &ct3d->cxl_dstate;

    rc = ct3d_qmp_cxl_event_log_enc(log);
    if (rc < 0) {
        error_setg(errp, "Unhandled error log type");
        return;
    }
    enc_log = rc;

    memset(&gem, 0, sizeof(gem));
    cxl_assign_event_header(hdr, &gen_media_uuid, flags, sizeof(gem),
                            cxl_device_get_timestamp(&ct3d->cxl_dstate));

    stq_le_p(&gem.phys_addr, dpa);
    gem.descriptor = descriptor;
    gem.type = type;
    gem.transaction_type = transaction_type;

    if (has_channel) {
        gem.channel = channel;
        valid_flags |= CXL_GMER_VALID_CHANNEL;
    }

    if (has_rank) {
        gem.rank = rank;
        valid_flags |= CXL_GMER_VALID_RANK;
    }

    if (has_device) {
        st24_le_p(gem.device, device);
        valid_flags |= CXL_GMER_VALID_DEVICE;
    }

    if (component_id) {
        strncpy((char *)gem.component_id, component_id,
                sizeof(gem.component_id) - 1);
        valid_flags |= CXL_GMER_VALID_COMPONENT;
    }

    stw_le_p(&gem.validity_flags, valid_flags);

    if (cxl_event_insert(cxlds, enc_log, (CXLEventRecordRaw *)&gem)) {
        cxl_event_irq_assert(ct3d);
    }
}

#define CXL_DRAM_VALID_CHANNEL                          BIT(0)
#define CXL_DRAM_VALID_RANK                             BIT(1)
#define CXL_DRAM_VALID_NIBBLE_MASK                      BIT(2)
#define CXL_DRAM_VALID_BANK_GROUP                       BIT(3)
#define CXL_DRAM_VALID_BANK                             BIT(4)
#define CXL_DRAM_VALID_ROW                              BIT(5)
#define CXL_DRAM_VALID_COLUMN                           BIT(6)
#define CXL_DRAM_VALID_CORRECTION_MASK                  BIT(7)

void qmp_cxl_inject_dram_event(const char *path, CxlEventLog log, uint8_t flags,
                               uint64_t dpa, uint8_t descriptor,
                               uint8_t type, uint8_t transaction_type,
                               bool has_channel, uint8_t channel,
                               bool has_rank, uint8_t rank,
                               bool has_nibble_mask, uint32_t nibble_mask,
                               bool has_bank_group, uint8_t bank_group,
                               bool has_bank, uint8_t bank,
                               bool has_row, uint32_t row,
                               bool has_column, uint16_t column,
                               bool has_correction_mask,
                               uint64List *correction_mask,
                               Error **errp)
{
    Object *obj = object_resolve_path(path, NULL);
    CXLEventDram dram;
    CXLEventRecordHdr *hdr = &dram.hdr;
    CXLDeviceState *cxlds;
    CXLType3Dev *ct3d;
    uint16_t valid_flags = 0;
    uint8_t enc_log;
    int rc;

    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }
    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }
    ct3d = CXL_TYPE3(obj);
    cxlds = &ct3d->cxl_dstate;

    rc = ct3d_qmp_cxl_event_log_enc(log);
    if (rc < 0) {
        error_setg(errp, "Unhandled error log type");
        return;
    }
    enc_log = rc;

    memset(&dram, 0, sizeof(dram));
    cxl_assign_event_header(hdr, &dram_uuid, flags, sizeof(dram),
                            cxl_device_get_timestamp(&ct3d->cxl_dstate));
    stq_le_p(&dram.phys_addr, dpa);
    dram.descriptor = descriptor;
    dram.type = type;
    dram.transaction_type = transaction_type;

    if (has_channel) {
        dram.channel = channel;
        valid_flags |= CXL_DRAM_VALID_CHANNEL;
    }

    if (has_rank) {
        dram.rank = rank;
        valid_flags |= CXL_DRAM_VALID_RANK;
    }

    if (has_nibble_mask) {
        st24_le_p(dram.nibble_mask, nibble_mask);
        valid_flags |= CXL_DRAM_VALID_NIBBLE_MASK;
    }

    if (has_bank_group) {
        dram.bank_group = bank_group;
        valid_flags |= CXL_DRAM_VALID_BANK_GROUP;
    }

    if (has_bank) {
        dram.bank = bank;
        valid_flags |= CXL_DRAM_VALID_BANK;
    }

    if (has_row) {
        st24_le_p(dram.row, row);
        valid_flags |= CXL_DRAM_VALID_ROW;
    }

    if (has_column) {
        stw_le_p(&dram.column, column);
        valid_flags |= CXL_DRAM_VALID_COLUMN;
    }

    if (has_correction_mask) {
        int count = 0;
        while (correction_mask && count < 4) {
            stq_le_p(&dram.correction_mask[count],
                     correction_mask->value);
            count++;
            correction_mask = correction_mask->next;
        }
        valid_flags |= CXL_DRAM_VALID_CORRECTION_MASK;
    }

    stw_le_p(&dram.validity_flags, valid_flags);

    if (cxl_event_insert(cxlds, enc_log, (CXLEventRecordRaw *)&dram)) {
        cxl_event_irq_assert(ct3d);
    }
}

void qmp_cxl_inject_memory_module_event(const char *path, CxlEventLog log,
                                        uint8_t flags, uint8_t type,
                                        uint8_t health_status,
                                        uint8_t media_status,
                                        uint8_t additional_status,
                                        uint8_t life_used,
                                        int16_t temperature,
                                        uint32_t dirty_shutdown_count,
                                        uint32_t corrected_volatile_error_count,
                                        uint32_t corrected_persist_error_count,
                                        Error **errp)
{
    Object *obj = object_resolve_path(path, NULL);
    CXLEventMemoryModule module;
    CXLEventRecordHdr *hdr = &module.hdr;
    CXLDeviceState *cxlds;
    CXLType3Dev *ct3d;
    uint8_t enc_log;
    int rc;

    if (!obj) {
        error_setg(errp, "Unable to resolve path");
        return;
    }
    if (!object_dynamic_cast(obj, TYPE_CXL_TYPE3)) {
        error_setg(errp, "Path does not point to a CXL type 3 device");
        return;
    }
    ct3d = CXL_TYPE3(obj);
    cxlds = &ct3d->cxl_dstate;

    rc = ct3d_qmp_cxl_event_log_enc(log);
    if (rc < 0) {
        error_setg(errp, "Unhandled error log type");
        return;
    }
    enc_log = rc;

    memset(&module, 0, sizeof(module));
    cxl_assign_event_header(hdr, &memory_module_uuid, flags, sizeof(module),
                            cxl_device_get_timestamp(&ct3d->cxl_dstate));

    module.type = type;
    module.health_status = health_status;
    module.media_status = media_status;
    module.additional_status = additional_status;
    module.life_used = life_used;
    stw_le_p(&module.temperature, temperature);
    stl_le_p(&module.dirty_shutdown_count, dirty_shutdown_count);
    stl_le_p(&module.corrected_volatile_error_count,
             corrected_volatile_error_count);
    stl_le_p(&module.corrected_persistent_error_count,
             corrected_persist_error_count);

    if (cxl_event_insert(cxlds, enc_log, (CXLEventRecordRaw *)&module)) {
        cxl_event_irq_assert(ct3d);
    }
}

/*
 * Check whether the range [dpa, dpa + len - 1] has overlaps with extents in
 * the list.
 * Return value: return true if has overlaps; otherwise, return false
 */
bool cxl_extents_overlaps_dpa_range(CXLDCExtentList *list,
                                    uint64_t dpa, uint64_t len)
{
    CXLDCExtent *ent;
    Range range1, range2;

    if (!list) {
        return false;
    }

    range_init_nofail(&range1, dpa, len);
    QTAILQ_FOREACH(ent, list, node) {
        range_init_nofail(&range2, ent->start_dpa, ent->len);
        if (range_overlaps_range(&range1, &range2)) {
            return true;
        }
    }
    return false;
}

/*
 * Check whether the range [dpa, dpa + len - 1] is contained by extents in
 * the list.
 * Will check multiple extents containment once superset release is added.
 * Return value: return true if range is contained; otherwise, return false
 */
bool cxl_extents_contains_dpa_range(CXLDCExtentList *list,
                                    uint64_t dpa, uint64_t len)
{
    CXLDCExtent *ent;
    Range range1, range2;

    if (!list) {
        return false;
    }

    range_init_nofail(&range1, dpa, len);
    QTAILQ_FOREACH(ent, list, node) {
        range_init_nofail(&range2, ent->start_dpa, ent->len);
        if (range_contains_range(&range2, &range1)) {
            return true;
        }
    }
    return false;
}

bool cxl_extent_groups_overlaps_dpa_range(CXLDCExtentGroupList *list,
                                          uint64_t dpa, uint64_t len)
{
    CXLDCExtentGroup *group;

    if (!list) {
        return false;
    }

    QTAILQ_FOREACH(group, list, node) {
        if (cxl_extents_overlaps_dpa_range(&group->list, dpa, len)) {
            return true;
        }
    }
    return false;
}

/*
 * The main function to process dynamic capacity event with extent list.
 * Currently DC extents add/release requests are processed.
 */
static void qmp_cxl_process_dynamic_capacity_prescriptive(const char *path,
        uint16_t hid, CXLDCEventType type, uint8_t rid,
        CxlDynamicCapacityExtentList *records, Error **errp)
{
    Object *obj;
    CXLType3Dev *dcd;
    uint32_t num_extents = 0;
    CxlDynamicCapacityExtentList *list;
    CXLDCExtentGroup *group = NULL;
    g_autofree CXLDCExtentRaw *extents = NULL;
    uint64_t dpa, offset, len, block_size;
    g_autofree unsigned long *blk_bitmap = NULL;
    int i;

    obj = object_resolve_path_type(path, TYPE_CXL_TYPE3, NULL);
    if (!obj) {
        error_setg(errp, "Unable to resolve CXL type 3 device");
        return;
    }

    dcd = CXL_TYPE3(obj);
    if (!dcd->dc.num_regions) {
        error_setg(errp, "No dynamic capacity support from the device");
        return;
    }


    if (rid >= dcd->dc.num_regions) {
        error_setg(errp, "region id is too large");
        return;
    }
    block_size = dcd->dc.regions[rid].block_size;
    blk_bitmap = bitmap_new(dcd->dc.regions[rid].len / block_size);

    /* Sanity check and count the extents */
    list = records;
    while (list) {
        offset = list->value->offset;
        len = list->value->len;
        dpa = offset + dcd->dc.regions[rid].base;

        if (len == 0) {
            error_setg(errp, "extent with 0 length is not allowed");
            return;
        }

        if (offset % block_size || len % block_size) {
            error_setg(errp, "dpa or len is not aligned to region block size");
            return;
        }

        if (offset + len > dcd->dc.regions[rid].len) {
            error_setg(errp, "extent range is beyond the region end");
            return;
        }

        /* No duplicate or overlapped extents are allowed */
        if (test_any_bits_set(blk_bitmap, offset / block_size,
                              len / block_size)) {
            error_setg(errp, "duplicate or overlapped extents are detected");
            return;
        }
        bitmap_set(blk_bitmap, offset / block_size, len / block_size);

        if (type == DC_EVENT_RELEASE_CAPACITY) {
            if (cxl_extent_groups_overlaps_dpa_range(&dcd->dc.extents_pending,
                                                     dpa, len)) {
                error_setg(errp,
                           "cannot release extent with pending DPA range");
                return;
            }
            if (!ct3_test_region_block_backed(dcd, dpa, len)) {
                error_setg(errp,
                           "cannot release extent with non-existing DPA range");
                return;
            }
        } else if (type == DC_EVENT_ADD_CAPACITY) {
            if (cxl_extents_overlaps_dpa_range(&dcd->dc.extents, dpa, len)) {
                error_setg(errp,
                           "cannot add DPA already accessible to the same LD");
                return;
            }
            if (cxl_extent_groups_overlaps_dpa_range(&dcd->dc.extents_pending,
                                                     dpa, len)) {
                error_setg(errp,
                           "cannot add DPA again while still pending");
                return;
            }
        }
        list = list->next;
        num_extents++;
    }

    /* Create extent list for event being passed to host */
    i = 0;
    list = records;
    extents = g_new0(CXLDCExtentRaw, num_extents);
    while (list) {
        offset = list->value->offset;
        len = list->value->len;
        dpa = dcd->dc.regions[rid].base + offset;

        extents[i].start_dpa = dpa;
        extents[i].len = len;
        memset(extents[i].tag, 0, 0x10);
        extents[i].shared_seq = 0;
        if (type == DC_EVENT_ADD_CAPACITY) {
            group = cxl_insert_extent_to_extent_group(group,
                                                      extents[i].start_dpa,
                                                      extents[i].len,
                                                      extents[i].tag,
                                                      extents[i].shared_seq);
        }

        list = list->next;
        i++;
    }
    if (group) {
        cxl_extent_group_list_insert_tail(&dcd->dc.extents_pending, group);
        dcd->dc.total_extent_count += num_extents;
    }

    cxl_create_dc_event_records_for_extents(dcd, type, extents, num_extents);
}

void qmp_cxl_add_dynamic_capacity(const char *path, uint16_t host_id,
                                  CxlExtentSelectionPolicy sel_policy,
                                  uint8_t region, const char *tag,
                                  CxlDynamicCapacityExtentList  *extents,
                                  Error **errp)
{
    switch (sel_policy) {
    case CXL_EXTENT_SELECTION_POLICY_PRESCRIPTIVE:
        qmp_cxl_process_dynamic_capacity_prescriptive(path, host_id,
                                                      DC_EVENT_ADD_CAPACITY,
                                                      region, extents, errp);
        return;
    default:
        error_setg(errp, "Selection policy not supported");
        return;
    }
}

void qmp_cxl_release_dynamic_capacity(const char *path, uint16_t host_id,
                                      CxlExtentRemovalPolicy removal_policy,
                                      bool has_forced_removal,
                                      bool forced_removal,
                                      bool has_sanitize_on_release,
                                      bool sanitize_on_release,
                                      uint8_t region,
                                      const char *tag,
                                      CxlDynamicCapacityExtentList  *extents,
                                      Error **errp)
{
    CXLDCEventType type = DC_EVENT_RELEASE_CAPACITY;

    if (has_forced_removal && forced_removal) {
        /* TODO: enable forced removal in the future */
        type = DC_EVENT_FORCED_RELEASE_CAPACITY;
        error_setg(errp, "Forced removal not supported yet");
        return;
    }

    switch (removal_policy) {
    case CXL_EXTENT_REMOVAL_POLICY_PRESCRIPTIVE:
        qmp_cxl_process_dynamic_capacity_prescriptive(path, host_id, type,
                                                      region, extents, errp);
        return;
    default:
        error_setg(errp, "Removal policy not supported");
        return;
    }
}

static void ct3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    CXLType3Class *cvc = CXL_TYPE3_CLASS(oc);

    pc->realize = ct3_realize;
    pc->exit = ct3_exit;
    pc->class_id = PCI_CLASS_MEMORY_CXL;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0xd93; /* LVF for now */
    pc->revision = 1;

    pc->config_write = ct3d_config_write;
    pc->config_read = ct3d_config_read;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "CXL Memory Device (Type 3)";
    device_class_set_legacy_reset(dc, ct3d_reset);
    device_class_set_props(dc, ct3_props);

    cvc->get_lsa_size = get_lsa_size;
    cvc->get_lsa = get_lsa;
    cvc->set_lsa = set_lsa;
    cvc->set_cacheline = set_cacheline;
}

static const TypeInfo ct3d_info = {
    .name = TYPE_CXL_TYPE3,
    .parent = TYPE_PCI_DEVICE,
    .class_size = sizeof(struct CXLType3Class),
    .class_init = ct3_class_init,
    .instance_size = sizeof(CXLType3Dev),
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CXL_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        {}
    },
};

static void ct3d_registers(void)
{
    type_register_static(&ct3d_info);
}

type_init(ct3d_registers);
