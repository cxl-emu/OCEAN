/*
 * CXL Type 3 RDMA support header for CXLMemSim integration
 */

#ifndef CXL_TYPE3_RDMA_H
#define CXL_TYPE3_RDMA_H

#include <stdbool.h>
#include <stdint.h>

/* RDMA function prototypes */
int cxl_memsim_rdma_connect(const char *server_addr, int port);
int cxl_memsim_rdma_request(uint8_t op, uint64_t addr, uint64_t size,
                            void *data, void *resp);
void cxl_memsim_rdma_disconnect(void);
bool cxl_memsim_rdma_available(void);

#endif /* CXL_TYPE3_RDMA_H */