/*
 * CXL Type 3 RDMA implementation for CXLMemSim integration
 * Simplified RDMA client using TCP-based RDMA emulation
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "cxl_type3_rdma.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

/* RDMA-like message structures */
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

/* Connection state */
static struct {
    int socket_fd;
    bool connected;
    char server_addr[256];
    int server_port;
    pthread_mutex_t lock;
} g_rdma_conn = {
    .socket_fd = -1,
    .connected = false,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/* Connect to CXLMemSim RDMA server using TCP transport */
int cxl_memsim_rdma_connect(const char *server_addr, int port)
{
    struct sockaddr_in addr;
    int sock;
    int opt = 1;
    
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (g_rdma_conn.connected) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return 0;
    }
    
    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        error_report("CXL RDMA: Failed to create socket: %s", strerror(errno));
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Set socket options */
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        error_report("CXL RDMA: Failed to set socket options");
        close(sock);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Configure server address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_addr, &addr.sin_addr) <= 0) {
        error_report("CXL RDMA: Invalid address: %s", server_addr);
        close(sock);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Connect to server - use TCP port, not RDMA port */
    info_report("CXL RDMA: Connecting to %s:%d via TCP transport", server_addr, port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("CXL RDMA: Failed to connect: %s", strerror(errno));
        close(sock);
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    g_rdma_conn.socket_fd = sock;
    g_rdma_conn.connected = true;
    strncpy(g_rdma_conn.server_addr, server_addr, sizeof(g_rdma_conn.server_addr) - 1);
    g_rdma_conn.server_port = port;
    
    info_report("CXL RDMA: Successfully connected (TCP transport mode)");
    pthread_mutex_unlock(&g_rdma_conn.lock);
    return 0;
}

/* Send request and receive response */
int cxl_memsim_rdma_request(uint8_t op, uint64_t addr, uint64_t size,
                            void *data, void *resp)
{
    RDMARequest req;
    RDMAResponse *response = (RDMAResponse *)resp;
    ssize_t ret;
    
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (!g_rdma_conn.connected) {
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Prepare request */
    memset(&req, 0, sizeof(req));
    req.op_type = op;
    req.addr = addr;
    req.size = size;
    req.timestamp = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    req.host_id = 0;
    
    if (op == 1 && data) {  /* Write operation */
        memcpy(req.data, data, MIN(size, 64));
    }
    
    /* Send request */
    ret = send(g_rdma_conn.socket_fd, &req, sizeof(req), MSG_NOSIGNAL);
    if (ret != sizeof(req)) {
        error_report("CXL RDMA: Failed to send request");
        g_rdma_conn.connected = false;
        close(g_rdma_conn.socket_fd);
        g_rdma_conn.socket_fd = -1;
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    /* Receive response */
    ret = recv(g_rdma_conn.socket_fd, response, sizeof(RDMAResponse), MSG_WAITALL);
    if (ret != sizeof(RDMAResponse)) {
        error_report("CXL RDMA: Failed to receive response");
        g_rdma_conn.connected = false;
        close(g_rdma_conn.socket_fd);
        g_rdma_conn.socket_fd = -1;
        pthread_mutex_unlock(&g_rdma_conn.lock);
        return -1;
    }
    
    pthread_mutex_unlock(&g_rdma_conn.lock);
    return 0;
}

/* Disconnect */
void cxl_memsim_rdma_disconnect(void)
{
    pthread_mutex_lock(&g_rdma_conn.lock);
    
    if (g_rdma_conn.connected && g_rdma_conn.socket_fd >= 0) {
        close(g_rdma_conn.socket_fd);
        g_rdma_conn.socket_fd = -1;
        g_rdma_conn.connected = false;
        info_report("CXL RDMA: Disconnected");
    }
    
    pthread_mutex_unlock(&g_rdma_conn.lock);
}

/* Check if RDMA is available - always true for TCP transport */
bool cxl_memsim_rdma_available(void)
{
    return true;  /* TCP transport is always available */
}