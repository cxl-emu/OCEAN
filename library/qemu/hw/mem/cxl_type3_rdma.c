/*
 * CXL Type 3 RDMA support for CXLMemSim integration
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define RDMA_BUFFER_SIZE 4096
#define RDMA_CQ_SIZE 1024
#define RDMA_MAX_WR 512

/* RDMA message structures matching CXLMemSim */
typedef struct __attribute__((packed)) {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint8_t host_id;
    uint64_t virtual_addr;
    uint8_t data[64];
} RDMARequest;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint64_t latency_ns;
    uint8_t cache_state;
    uint8_t data[64];
} RDMAResponse;

typedef struct __attribute__((packed)) {
    RDMARequest request;
    RDMAResponse response;
} RDMAMessage;

/* RDMA connection state */
struct CXLMemSimRDMA {
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *event_channel;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp;
    struct ibv_comp_channel *comp_channel;
    void *buffer;
    size_t buffer_size;
    bool connected;
    pthread_mutex_t lock;
    char server_addr[256];
    int server_port;
};

static struct CXLMemSimRDMA g_rdma_conn = {
    .connected = false,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/* Initialize RDMA connection resources */
static int cxl_rdma_setup_resources(void)
{
    struct ibv_qp_init_attr qp_attr;
    
    /* Allocate protection domain */
    g_rdma_conn.pd = ibv_alloc_pd(g_rdma_conn.context);
    if (!g_rdma_conn.pd) {
        error_report("CXL RDMA: Failed to allocate PD");
        return -1;
    }
    
    /* Create completion channel */
    g_rdma_conn.comp_channel = ibv_create_comp_channel(g_rdma_conn.context);
    if (!g_rdma_conn.comp_channel) {
        error_report("CXL RDMA: Failed to create completion channel");
        return -1;
    }
    
    /* Create CQs */
    g_rdma_conn.send_cq = ibv_create_cq(g_rdma_conn.context, RDMA_CQ_SIZE,
                                        NULL, g_rdma_conn.comp_channel, 0);
    g_rdma_conn.recv_cq = ibv_create_cq(g_rdma_conn.context, RDMA_CQ_SIZE,
                                        NULL, g_rdma_conn.comp_channel, 0);
    if (!g_rdma_conn.send_cq || !g_rdma_conn.recv_cq) {
        error_report("CXL RDMA: Failed to create CQ");
        return -1;
    }
    
    /* Allocate and register memory */
    g_rdma_conn.buffer_size = RDMA_BUFFER_SIZE * sizeof(RDMAMessage);
    g_rdma_conn.buffer = g_malloc0(g_rdma_conn.buffer_size);
    
    g_rdma_conn.mr = ibv_reg_mr(g_rdma_conn.pd, g_rdma_conn.buffer,
                                g_rdma_conn.buffer_size,
                                IBV_ACCESS_LOCAL_WRITE | 
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_REMOTE_READ);
    if (!g_rdma_conn.mr) {
        error_report("CXL RDMA: Failed to register MR");
        return -1;
    }
    
    /* Create QP */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = g_rdma_conn.send_cq;
    qp_attr.recv_cq = g_rdma_conn.recv_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.sq_sig_all = 1;
    qp_attr.cap.max_send_wr = RDMA_MAX_WR;
    qp_attr.cap.max_recv_wr = RDMA_MAX_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    
    if (rdma_create_qp(g_rdma_conn.cm_id, g_rdma_conn.pd, &qp_attr)) {
        error_report("CXL RDMA: Failed to create QP");
        return -1;
    }
    
    g_rdma_conn.qp = g_rdma_conn.cm_id->qp;
    return 0;
}

/* Connect to CXLMemSim RDMA server */
int cxl_memsim_rdma_connect(const char *server_addr, int port)
{
    struct rdma_cm_event *event;
    struct sockaddr_in addr;
    
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (g_rdma_conn.connected) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return 0;
    }
    
    /* Create event channel */
    g_rdma_conn.event_channel = rdma_create_event_channel();
    if (!g_rdma_conn.event_channel) {
        error_report("CXL RDMA: Failed to create event channel");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Create CM ID */
    if (rdma_create_id(g_rdma_conn.event_channel, &g_rdma_conn.cm_id,
                      NULL, RDMA_PS_TCP)) {
        error_report("CXL RDMA: Failed to create CM ID");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Resolve address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, server_addr, &addr.sin_addr);
    
    if (rdma_resolve_addr(g_rdma_conn.cm_id, NULL,
                         (struct sockaddr *)&addr, 2000)) {
        error_report("CXL RDMA: Failed to resolve address");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Wait for address resolution */
    if (rdma_get_cm_event(g_rdma_conn.event_channel, &event)) {
        error_report("CXL RDMA: Failed to get CM event");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        error_report("CXL RDMA: Address resolution failed");
        rdma_ack_cm_event(event);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    rdma_ack_cm_event(event);
    
    /* Resolve route */
    if (rdma_resolve_route(g_rdma_conn.cm_id, 2000)) {
        error_report("CXL RDMA: Failed to resolve route");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Wait for route resolution */
    if (rdma_get_cm_event(g_rdma_conn.event_channel, &event)) {
        error_report("CXL RDMA: Failed to get CM event");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        error_report("CXL RDMA: Route resolution failed");
        rdma_ack_cm_event(event);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    rdma_ack_cm_event(event);
    
    g_rdma_conn.context = g_rdma_conn.cm_id->verbs;
    
    /* Setup resources */
    if (cxl_rdma_setup_resources() < 0) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Connect */
    struct rdma_conn_param conn_param = {
        .initiator_depth = 1,
        .responder_resources = 1,
        .retry_count = 7,
        .rnr_retry_count = 7
    };
    
    if (rdma_connect(g_rdma_conn.cm_id, &conn_param)) {
        error_report("CXL RDMA: Failed to connect");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Wait for connection */
    if (rdma_get_cm_event(g_rdma_conn.event_channel, &event)) {
        error_report("CXL RDMA: Failed to get CM event");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        error_report("CXL RDMA: Connection failed");
        rdma_ack_cm_event(event);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    rdma_ack_cm_event(event);
    
    g_rdma_conn.connected = true;
    strncpy(g_rdma_conn.server_addr, server_addr, sizeof(g_rdma_conn.server_addr) - 1);
    g_rdma_conn.server_port = port;
    
    info_report("CXL RDMA: Connected to %s:%d", server_addr, port);
    pthread_mutex_unlock(&g_rdma_conn.lock);
    return 0;
}

/* Send RDMA request and receive response */
int cxl_memsim_rdma_request(uint8_t op, uint64_t addr, uint64_t size,
                            void *data, RDMAResponse *resp)
{
    RDMAMessage *msg;
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int ret;
    
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (!g_rdma_conn.connected) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    msg = (RDMAMessage *)g_rdma_conn.buffer;
    
    /* Prepare request */
    memset(msg, 0, sizeof(*msg));
    msg->request.op_type = op;
    msg->request.addr = addr;
    msg->request.size = size;
    msg->request.timestamp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    msg->request.host_id = 0;  /* QEMU host ID */
    
    if (op == 1 && data) {  /* Write operation */
        memcpy(msg->request.data, data, MIN(size, 64));
    }
    
    /* Post receive for response */
    sge.addr = (uintptr_t)&msg->response;
    sge.length = sizeof(msg->response);
    sge.lkey = g_rdma_conn.mr->lkey;
    
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    
    if (ibv_post_recv(g_rdma_conn.qp, &recv_wr, &bad_recv_wr)) {
        error_report("CXL RDMA: Failed to post receive");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Post send */
    sge.addr = (uintptr_t)&msg->request;
    sge.length = sizeof(msg->request);
    sge.lkey = g_rdma_conn.mr->lkey;
    
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 2;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    
    if (ibv_post_send(g_rdma_conn.qp, &wr, &bad_wr)) {
        error_report("CXL RDMA: Failed to post send");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Wait for completion */
    do {
        ret = ibv_poll_cq(g_rdma_conn.send_cq, 1, &wc);
    } while (ret == 0);
    
    if (ret < 0 || wc.status != IBV_WC_SUCCESS) {
        error_report("CXL RDMA: Send failed");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Wait for response */
    do {
        ret = ibv_poll_cq(g_rdma_conn.recv_cq, 1, &wc);
    } while (ret == 0);
    
    if (ret < 0 || wc.status != IBV_WC_SUCCESS) {
        error_report("CXL RDMA: Receive failed");
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Copy response */
    if (resp) {
        memcpy(resp, &msg->response, sizeof(*resp));
    }
    
    pthread_mutex_unlock(&g_rdma_conn.lock);
    return 0;
}

/* Disconnect RDMA */
void cxl_memsim_rdma_disconnect(void)
{
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (!g_rdma_conn.connected) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return;
    }
    
    if (g_rdma_conn.cm_id) {
        rdma_disconnect(g_rdma_conn.cm_id);
        rdma_destroy_qp(g_rdma_conn.cm_id);
        rdma_destroy_id(g_rdma_conn.cm_id);
    }
    
    if (g_rdma_conn.mr) {
        ibv_dereg_mr(g_rdma_conn.mr);
    }
    
    if (g_rdma_conn.buffer) {
        g_free(g_rdma_conn.buffer);
    }
    
    if (g_rdma_conn.recv_cq) {
        ibv_destroy_cq(g_rdma_conn.recv_cq);
    }
    
    if (g_rdma_conn.send_cq) {
        ibv_destroy_cq(g_rdma_conn.send_cq);
    }
    
    if (g_rdma_conn.comp_channel) {
        ibv_destroy_comp_channel(g_rdma_conn.comp_channel);
    }
    
    if (g_rdma_conn.pd) {
        ibv_dealloc_pd(g_rdma_conn.pd);
    }
    
    if (g_rdma_conn.event_channel) {
        rdma_destroy_event_channel(g_rdma_conn.event_channel);
    }
    
    g_rdma_conn.connected = false;
    info_report("CXL RDMA: Disconnected");
    pthread_mutex_unlock(&g_rdma_conn.lock);
}

/* Check if RDMA is available */
bool cxl_memsim_rdma_available(void)
{
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (dev_list) {
        ibv_free_device_list(dev_list);
        return true;
    }
    return false;
}