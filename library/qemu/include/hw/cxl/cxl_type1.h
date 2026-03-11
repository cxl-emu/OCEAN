/*
 * CXL Type 1 Device (Accelerator) Header
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CXL_TYPE1_H
#define CXL_TYPE1_H

#include "hw/pci/pci_device.h"
#include "hw/cxl/cxl_device.h"
#include "hw/cxl/cxl_component.h"

#define TYPE_CXL_TYPE1 "cxl-type1"

typedef struct CXLType1State {
    PCIDevice parent;
    
    CXLComponentState cxl_cstate;
    CXLDeviceState cxl_dstate;
    
    MemoryRegion cache_mem;
    
    uint64_t cache_size;
    uint64_t sn;
    
    struct {
        char *host;
        uint16_t port;
        void *socket;
        bool connected;
    } memsim_server;
} CXLType1State;

#define CXL_TYPE1(obj) OBJECT_CHECK(CXLType1State, (obj), TYPE_CXL_TYPE1)

/* These functions are currently internal to cxl_type1.c */

#endif