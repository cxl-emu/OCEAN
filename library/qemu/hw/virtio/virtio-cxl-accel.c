/*
 * Virtio CXL Type 1 Accelerator Bridge
 * Forwards virtio accelerator requests to CXLMemSim via CXL Type 1 device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-crypto.h"
#include "hw/qdev-properties.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_crypto.h"
#include "io/channel-socket.h"

#define TYPE_VIRTIO_CXL_ACCEL "virtio-cxl-accel-device"
#define VIRTIO_CXL_ACCEL(obj) \
    OBJECT_CHECK(VirtIOCXLAccel, (obj), TYPE_VIRTIO_CXL_ACCEL)

#define TYPE_VIRTIO_CXL_ACCEL_PCI "virtio-cxl-accel-pci"
#define VIRTIO_CXL_ACCEL_PCI(obj) \
    OBJECT_CHECK(VirtIOCXLAccelPCI, (obj), TYPE_VIRTIO_CXL_ACCEL_PCI)

typedef struct VirtIOCXLAccelConf {
    uint32_t max_size;
    uint32_t max_cipher_key_len;
    uint32_t max_auth_key_len;
    uint32_t max_queues;
    char *cxlmemsim_addr;
    uint16_t cxlmemsim_port;
} VirtIOCXLAccelConf;

typedef struct VirtIOCXLAccel {
    VirtIODevice parent_obj;
    
    VirtQueue *dataq[64];
    VirtQueue *ctrlq;
    
    VirtIOCXLAccelConf conf;
    
    QIOChannelSocket *socket;
    bool connected;
    QemuMutex lock;
    
    uint32_t max_dataqueues;
    uint32_t status;
    
    uint32_t curr_queues;
    QTAILQ_HEAD(, VirtIOCXLAccelReq) requests;
} VirtIOCXLAccel;

typedef struct VirtIOCXLAccelPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOCXLAccel vdev;
} VirtIOCXLAccelPCI;

typedef struct VirtIOCXLAccelReq {
    VirtQueueElement *elem;
    VirtIOCXLAccel *vcxl;
    VirtQueue *vq;
    uint32_t opcode;
    uint32_t status;
    QTAILQ_ENTRY(VirtIOCXLAccelReq) next;
} VirtIOCXLAccelReq;

typedef struct CXLMemSimPacket {
    uint32_t type;
    uint32_t opcode;
    uint32_t size;
    uint64_t session_id;
    uint8_t data[];
} __attribute__((packed)) CXLMemSimPacket;

enum CXLMemSimOpType {
    CXLMEMSIM_TYPE_CRYPTO = 1,
    CXLMEMSIM_TYPE_COMPRESS,
    CXLMEMSIM_TYPE_AI_ACCEL,
    CXLMEMSIM_TYPE_CUSTOM,
};

enum CXLMemSimCryptoOp {
    CXLMEMSIM_CRYPTO_ENCRYPT = 1,
    CXLMEMSIM_CRYPTO_DECRYPT,
    CXLMEMSIM_CRYPTO_HASH,
    CXLMEMSIM_CRYPTO_MAC,
    CXLMEMSIM_CRYPTO_AEAD,
};

static void virtio_cxl_accel_connect(VirtIOCXLAccel *vcxl)
{
    Error *err = NULL;
    SocketAddress addr;

    if (vcxl->connected) {
        return;
    }

    /* Check if SHM transport mode is configured - skip TCP connection */
    const char *transport = getenv("CXL_TRANSPORT_MODE");
    if (!transport || !transport[0]) {
        transport = getenv("CXL_MEMSIM_TRANSPORT");
    }
    if (transport && (strcmp(transport, "shm") == 0 || strcmp(transport, "pgas") == 0)) {
        /* SHM mode - Type3 device handles connection */
        qemu_log("VirtIO CXL Accel: Using SHM transport mode - skipping TCP connection\n");
        return;
    }

    addr.type = SOCKET_ADDRESS_TYPE_INET;
    addr.u.inet.host = vcxl->conf.cxlmemsim_addr ?: g_strdup("127.0.0.1");
    addr.u.inet.port = g_strdup_printf("%u",
                                       vcxl->conf.cxlmemsim_port ?: 9999);

    vcxl->socket = qio_channel_socket_new();
    if (qio_channel_socket_connect_sync(vcxl->socket, &addr, &err) < 0) {
        qemu_log("Warning: Failed to connect to CXLMemSim at %s:%s: %s\n",
                addr.u.inet.host, addr.u.inet.port, error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(vcxl->socket));
        vcxl->socket = NULL;
    } else {
        vcxl->connected = true;
        qemu_log("Connected to CXLMemSim at %s:%s\n",
                addr.u.inet.host, addr.u.inet.port);
    }

    g_free(addr.u.inet.port);
    if (addr.u.inet.host != vcxl->conf.cxlmemsim_addr) {
        g_free(addr.u.inet.host);
    }
}

static void virtio_cxl_accel_disconnect(VirtIOCXLAccel *vcxl)
{
    if (!vcxl->connected) {
        return;
    }
    
    vcxl->connected = false;
    if (vcxl->socket) {
        qio_channel_close(QIO_CHANNEL(vcxl->socket), NULL);
        object_unref(OBJECT(vcxl->socket));
        vcxl->socket = NULL;
    }
}

static int virtio_cxl_accel_send_packet(VirtIOCXLAccel *vcxl, 
                                        CXLMemSimPacket *pkt)
{
    Error *err = NULL;
    size_t total_size = sizeof(CXLMemSimPacket) + pkt->size;
    
    qemu_mutex_lock(&vcxl->lock);
    
    if (!vcxl->connected) {
        virtio_cxl_accel_connect(vcxl);
        if (!vcxl->connected) {
            qemu_mutex_unlock(&vcxl->lock);
            return -1;
        }
    }
    
    if (qio_channel_write_all(QIO_CHANNEL(vcxl->socket),
                              (char *)pkt, total_size, &err) < 0) {
        error_report("Failed to send to CXLMemSim: %s",
                    error_get_pretty(err));
        error_free(err);
        virtio_cxl_accel_disconnect(vcxl);
        qemu_mutex_unlock(&vcxl->lock);
        return -1;
    }
    
    qemu_mutex_unlock(&vcxl->lock);
    return 0;
}

static void virtio_cxl_accel_handle_crypto_req(VirtIOCXLAccel *vcxl,
                                               VirtQueueElement *elem)
{
    struct virtio_crypto_op_header op_hdr;
    CXLMemSimPacket *pkt;
    size_t s, data_len = 0;
    uint32_t crypto_op = CXLMEMSIM_CRYPTO_ENCRYPT;
    
    if (elem->out_num == 0) {
        goto error;
    }
    
    s = iov_to_buf(elem->out_sg, elem->out_num, 0, &op_hdr, sizeof(op_hdr));
    if (s != sizeof(op_hdr)) {
        goto error;
    }
    
    switch (le32_to_cpu(op_hdr.opcode)) {
    case VIRTIO_CRYPTO_CIPHER_ENCRYPT:
        crypto_op = CXLMEMSIM_CRYPTO_ENCRYPT;
        break;
    case VIRTIO_CRYPTO_CIPHER_DECRYPT:
        crypto_op = CXLMEMSIM_CRYPTO_DECRYPT;
        break;
    case VIRTIO_CRYPTO_HASH:
        crypto_op = CXLMEMSIM_CRYPTO_HASH;
        break;
    case VIRTIO_CRYPTO_MAC:
        crypto_op = CXLMEMSIM_CRYPTO_MAC;
        break;
    case VIRTIO_CRYPTO_AEAD_ENCRYPT:
    case VIRTIO_CRYPTO_AEAD_DECRYPT:
        crypto_op = CXLMEMSIM_CRYPTO_AEAD;
        break;
    default:
        goto error;
    }
    
    for (int i = 1; i < elem->out_num; i++) {
        data_len += elem->out_sg[i].iov_len;
    }
    
    pkt = g_malloc0(sizeof(CXLMemSimPacket) + data_len);
    pkt->type = CXLMEMSIM_TYPE_CRYPTO;
    pkt->opcode = crypto_op;
    pkt->size = data_len;
    pkt->session_id = le64_to_cpu(op_hdr.session_id);
    
    size_t offset = 0;
    for (int i = 1; i < elem->out_num && offset < data_len; i++) {
        size_t copy_len = MIN(elem->out_sg[i].iov_len, data_len - offset);
        memcpy(pkt->data + offset, elem->out_sg[i].iov_base, copy_len);
        offset += copy_len;
    }
    
    if (virtio_cxl_accel_send_packet(vcxl, pkt) == 0) {
        if (elem->in_num > 0) {
            struct virtio_crypto_op_header resp_hdr = {0};
            resp_hdr.algo = VIRTIO_CRYPTO_NO_CIPHER;
            iov_from_buf(elem->in_sg, elem->in_num, 0, 
                        &resp_hdr, sizeof(resp_hdr));
        }
    } else {
        goto error_free;
    }
    
    g_free(pkt);
    return;
    
error_free:
    g_free(pkt);
error:
    if (elem->in_num > 0) {
        struct virtio_crypto_op_header resp_hdr = {0};
        resp_hdr.algo = VIRTIO_CRYPTO_NO_CIPHER;
        iov_from_buf(elem->in_sg, elem->in_num, 0, 
                    &resp_hdr, sizeof(resp_hdr));
    }
}


static void virtio_cxl_accel_handle_dataq(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOCXLAccel *vcxl = VIRTIO_CXL_ACCEL(vdev);
    VirtQueueElement *elem;
    
    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        virtio_cxl_accel_handle_crypto_req(vcxl, elem);
        
        virtqueue_push(vq, elem, elem->in_sg[0].iov_len);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_cxl_accel_handle_ctrlq(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtQueueElement *elem;
    
    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        if (elem->out_num > 0) {
            struct virtio_crypto_op_ctrl_req ctrl_req;
            size_t s = iov_to_buf(elem->out_sg, elem->out_num, 0,
                                 &ctrl_req, sizeof(ctrl_req));
            
            if (s == sizeof(ctrl_req)) {
                qemu_log("Control request: header.opcode=%d\n", 
                        ctrl_req.header.opcode);
            }
        }
        
        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static uint64_t virtio_cxl_accel_get_features(VirtIODevice *vdev,
                                              uint64_t features,
                                              Error **errp)
{
    return features;
}

static void virtio_cxl_accel_reset(VirtIODevice *vdev)
{
    VirtIOCXLAccel *vcxl = VIRTIO_CXL_ACCEL(vdev);
    
    vcxl->curr_queues = 1;
    vcxl->status = 0;
    
    virtio_cxl_accel_disconnect(vcxl);
}

static void virtio_cxl_accel_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCXLAccel *vcxl = VIRTIO_CXL_ACCEL(dev);
    int i;
    
    /* Set default values */
    if (vcxl->conf.max_queues == 0) {
        vcxl->conf.max_queues = 1;
    }
    if (vcxl->conf.max_cipher_key_len == 0) {
        vcxl->conf.max_cipher_key_len = 64;
    }
    if (vcxl->conf.max_auth_key_len == 0) {
        vcxl->conf.max_auth_key_len = 512;
    }
    if (vcxl->conf.cxlmemsim_port == 0) {
        vcxl->conf.cxlmemsim_port = 9999;
    }
    
    vcxl->max_dataqueues = vcxl->conf.max_queues;
    if (vcxl->max_dataqueues > 64) {
        vcxl->max_dataqueues = 64;
    }
    
    virtio_init(vdev, VIRTIO_ID_CRYPTO, 0);
    
    vcxl->curr_queues = 1;
    for (i = 0; i < vcxl->max_dataqueues; i++) {
        vcxl->dataq[i] = virtio_add_queue(vdev, 1024,
                                          virtio_cxl_accel_handle_dataq);
    }
    
    vcxl->ctrlq = virtio_add_queue(vdev, 64,
                                   virtio_cxl_accel_handle_ctrlq);
    
    qemu_mutex_init(&vcxl->lock);
    QTAILQ_INIT(&vcxl->requests);
    
    virtio_cxl_accel_connect(vcxl);
}

static void virtio_cxl_accel_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCXLAccel *vcxl = VIRTIO_CXL_ACCEL(dev);
    VirtIOCXLAccelReq *req, *next;
    int i;
    
    QTAILQ_FOREACH_SAFE(req, &vcxl->requests, next, next) {
        QTAILQ_REMOVE(&vcxl->requests, req, next);
        virtqueue_push(req->vq, req->elem, 0);
        virtio_notify(vdev, req->vq);
        g_free(req->elem);
        g_free(req);
    }
    
    virtio_cxl_accel_disconnect(vcxl);
    qemu_mutex_destroy(&vcxl->lock);
    
    for (i = 0; i < vcxl->max_dataqueues; i++) {
        virtio_delete_queue(vcxl->dataq[i]);
    }
    virtio_delete_queue(vcxl->ctrlq);
    
    virtio_cleanup(vdev);
}

static const VMStateDescription vmstate_virtio_cxl_accel = {
    .name = "virtio-cxl-accel",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

/* Properties disabled due to initialization issue
static Property virtio_cxl_accel_properties[] = {
    DEFINE_PROP_UINT32("max-queues", VirtIOCXLAccel, conf.max_queues, 1),
    DEFINE_PROP_UINT32("max-cipher-key-len", VirtIOCXLAccel, 
                      conf.max_cipher_key_len, 64),
    DEFINE_PROP_UINT32("max-auth-key-len", VirtIOCXLAccel,
                      conf.max_auth_key_len, 512),
    DEFINE_PROP_STRING("cxlmemsim-addr", VirtIOCXLAccel, conf.cxlmemsim_addr),
    DEFINE_PROP_UINT16("cxlmemsim-port", VirtIOCXLAccel, conf.cxlmemsim_port, 12345),
    {},
};
*/

static void virtio_cxl_accel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    
    /* TODO: Properties causing issues, skip for now */
    /* device_class_set_props(dc, virtio_cxl_accel_properties); */
    
    dc->vmsd = &vmstate_virtio_cxl_accel;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    
    vdc->realize = virtio_cxl_accel_device_realize;
    vdc->unrealize = virtio_cxl_accel_device_unrealize;
    vdc->get_features = virtio_cxl_accel_get_features;
    vdc->reset = virtio_cxl_accel_reset;
}

static const TypeInfo virtio_cxl_accel_info = {
    .name = TYPE_VIRTIO_CXL_ACCEL,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOCXLAccel),
    .class_init = virtio_cxl_accel_class_init,
};

static void virtio_cxl_accel_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOCXLAccelPCI *vcxl = VIRTIO_CXL_ACCEL_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&vcxl->vdev);
    
    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_cxl_accel_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *vpc = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev = PCI_DEVICE_CLASS(klass);
    
    vpc->realize = virtio_cxl_accel_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    
    pcidev->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev->device_id = 0x1050;
    pcidev->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev->class_id = PCI_CLASS_OTHERS;
}

static void virtio_cxl_accel_pci_instance_init(Object *obj)
{
    VirtIOCXLAccelPCI *vcxl = VIRTIO_CXL_ACCEL_PCI(obj);
    
    virtio_instance_init_common(obj, &vcxl->vdev, sizeof(vcxl->vdev),
                                TYPE_VIRTIO_CXL_ACCEL);
}

static const VirtioPCIDeviceTypeInfo virtio_cxl_accel_pci_info = {
    .generic_name = TYPE_VIRTIO_CXL_ACCEL_PCI,
    .instance_size = sizeof(VirtIOCXLAccelPCI),
    .instance_init = virtio_cxl_accel_pci_instance_init,
    .class_init = virtio_cxl_accel_pci_class_init,
};

static void virtio_cxl_accel_register_types(void)
{
    type_register_static(&virtio_cxl_accel_info);
    virtio_pci_types_register(&virtio_cxl_accel_pci_info);
}

type_init(virtio_cxl_accel_register_types)