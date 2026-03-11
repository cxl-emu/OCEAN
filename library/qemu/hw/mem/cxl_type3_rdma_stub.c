/*
 * CXL Type 3 RDMA support stub for CXLMemSim integration
 * This is a stub implementation that allows compilation without full RDMA support
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cxl_type3_rdma.h"

/* Stub RDMA functions */
int cxl_memsim_rdma_connect(const char *server_addr, int port)
{
    error_report("CXL RDMA: RDMA support not fully implemented yet");
    return -1;
}

int cxl_memsim_rdma_request(uint8_t op, uint64_t addr, uint64_t size,
                            void *data, void *resp)
{
    return -1;
}

void cxl_memsim_rdma_disconnect(void)
{
    /* Stub - nothing to do */
}

bool cxl_memsim_rdma_available(void)
{
    return false;  /* RDMA not available in stub */
}