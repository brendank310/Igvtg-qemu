/*
 * QEMU vGT/XenGT Legacy VGA support
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) Citrix Systems, Inc
 * Copyright (c) Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "vga_int.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "hw/loader.h"
#include "qemu/log.h"
#include "sysemu/arch_init.h"
#include "hw/xen/xen.h"
#include "hw/display/vga.h"

#include <linux/vfio.h>
#include <sys/ioctl.h>

#define DEBUG_VGT

#ifdef DEBUG_VGT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "vgt: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct XenGTDMABuf {
    QemuDmaBuf buf;
    uint32_t pos_x, pos_y, pos_updates;
    uint32_t hot_x, hot_y, hot_updates;
    int dmabuf_id;
    QTAILQ_ENTRY(XenGTDMABuf) next;
} XenGTDMABuf;

typedef struct XenGTDisplay {
    QemuConsole *con;
    struct {
        QTAILQ_HEAD(, XenGTDMABuf) bufs;
        XenGTDMABuf *primary;
        XenGTDMABuf *cursor;
    } dmabuf;
} XenGTDisplay;

typedef struct VGTHostDevice {
    PCIHostDeviceAddress addr;
    int config_fd;
} VGTHostDevice;

typedef struct VGTVMState {
     struct VGACommonState vga;
     struct VGTVGAState* parent;
} VGTVMState;

typedef struct VGTVGAState {
    PCIDevice dev;
    struct VGTVMState state;
    int num_displays;
    VGTHostDevice host_dev;
    bool instance_created;
    int domid;
    int device_fd;
    XenGTDisplay *dpy;
} VGTVGAState;

/* These are the default values */
int vgt_low_gm_sz = 64; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */
int guest_domid = 0;

static int vgt_host_pci_cfg_get(VGTHostDevice *host_dev,
                                void *data, int len, uint32_t addr);
static void cpu_update_state(void *pv, int running, RunState state);


void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len)
{
    assert(dev->devfn == 0x00);

    i440fx_write_config(dev, address, val, len);
}

#ifndef DRM_PLANE_TYPE_PRIMARY
# define DRM_PLANE_TYPE_PRIMARY 1
# define DRM_PLANE_TYPE_CURSOR  2
#endif

static void display_update_cursor(XenGTDMABuf *dmabuf,
                                  struct vfio_device_gfx_plane_info *plane)
{
    if (dmabuf->pos_x != plane->x_pos || dmabuf->pos_y != plane->y_pos) {
        dmabuf->pos_x      = plane->x_pos;
        dmabuf->pos_y      = plane->y_pos;
        dmabuf->pos_updates++;
    }
    if (dmabuf->hot_x != plane->x_hot || dmabuf->hot_y != plane->y_hot) {
        dmabuf->hot_x      = plane->x_hot;
        dmabuf->hot_y      = plane->y_hot;
        dmabuf->hot_updates++;
    }
}

static XenGTDMABuf *display_get_dmabuf(VGTVGAState *vdev,
                                       uint32_t plane_type)
{
    XenGTDisplay *dpy = vdev->dpy;
    struct vfio_device_gfx_plane_info plane;
    XenGTDMABuf *dmabuf;
    int fd, ret;

    memset(&plane, 0, sizeof(plane));
    plane.argsz = sizeof(plane);
    plane.flags = VFIO_GFX_PLANE_TYPE_DMABUF;
    plane.drm_plane_type = plane_type;
    ret = ioctl(vdev->device_fd, VFIO_DEVICE_QUERY_GFX_PLANE, &plane);
    if (ret < 0) {
        return NULL;
    }
    if (!plane.drm_format || !plane.size) {
        return NULL;
    }

    QTAILQ_FOREACH(dmabuf, &dpy->dmabuf.bufs, next) {
        if (dmabuf->dmabuf_id == plane.dmabuf_id) {
            /* found in list, move to head, return it */
            QTAILQ_REMOVE(&dpy->dmabuf.bufs, dmabuf, next);
            QTAILQ_INSERT_HEAD(&dpy->dmabuf.bufs, dmabuf, next);
            if (plane_type == DRM_PLANE_TYPE_CURSOR) {
                display_update_cursor(dmabuf, &plane);
            }
            return dmabuf;
        }
    }

    fd = ioctl(vdev->device_fd, VFIO_DEVICE_GET_GFX_DMABUF, &plane.dmabuf_id);
    if (fd < 0) {
        return NULL;
    }

    dmabuf = g_new0(XenGTDMABuf, 1);
    dmabuf->dmabuf_id  = plane.dmabuf_id;
    dmabuf->buf.width  = plane.width;
    dmabuf->buf.height = plane.height;
    dmabuf->buf.stride = plane.stride;
    dmabuf->buf.fourcc = plane.drm_format;
    dmabuf->buf.fd     = fd;
    if (plane_type == DRM_PLANE_TYPE_CURSOR) {
        display_update_cursor(dmabuf, &plane);
    }

    QTAILQ_INSERT_HEAD(&dpy->dmabuf.bufs, dmabuf, next);
    return dmabuf;
}

static void display_free_one_dmabuf(XenGTDisplay *dpy, XenGTDMABuf *dmabuf)
{
    QTAILQ_REMOVE(&dpy->dmabuf.bufs, dmabuf, next);
    dpy_gl_release_dmabuf(dpy->con, &dmabuf->buf);
    close(dmabuf->buf.fd);
    g_free(dmabuf);
}

static void display_free_dmabufs(VGTVGAState *vdev)
{
    XenGTDisplay *dpy = vdev->dpy;
    XenGTDMABuf *dmabuf, *tmp;
    uint32_t keep = 5;

    QTAILQ_FOREACH_SAFE(dmabuf, &dpy->dmabuf.bufs, next, tmp) {
        if (keep > 0) {
            keep--;
            continue;
        }
        assert(dmabuf != dpy->dmabuf.primary);
        display_free_one_dmabuf(dpy, dmabuf);
    }
}

static void display_dmabuf_update(void *opaque)
{
    VGTVGAState *vdev = opaque;
    XenGTDisplay *dpy = vdev->dpy;
    XenGTDMABuf *primary, *cursor;
    bool free_bufs = false, new_cursor = false;;

    primary = display_get_dmabuf(vdev, DRM_PLANE_TYPE_PRIMARY);
    if (primary == NULL) {
        return;
    }

    if (dpy->dmabuf.primary != primary) {
        dpy->dmabuf.primary = primary;
        qemu_console_resize(dpy->con,
                            primary->buf.width, primary->buf.height);
        dpy_gl_scanout_dmabuf(dpy->con, &primary->buf);
        free_bufs = true;
    }

    cursor = display_get_dmabuf(vdev, DRM_PLANE_TYPE_CURSOR);
    if (dpy->dmabuf.cursor != cursor) {
        dpy->dmabuf.cursor = cursor;
        new_cursor = true;
        free_bufs = true;
    }

    if (cursor && (new_cursor || cursor->hot_updates)) {
        bool have_hot = (cursor->hot_x != 0xffffffff &&
                         cursor->hot_y != 0xffffffff);
        dpy_gl_cursor_dmabuf(dpy->con, &cursor->buf, have_hot,
                             cursor->hot_x, cursor->hot_y);
        cursor->hot_updates = 0;
    } else if (!cursor && new_cursor) {
        dpy_gl_cursor_dmabuf(dpy->con, NULL, false, 0, 0);
    }

    if (cursor && cursor->pos_updates) {
        dpy_gl_cursor_position(dpy->con,
                               cursor->pos_x,
                               cursor->pos_y);
        cursor->pos_updates = 0;
    }

    dpy_gl_update(dpy->con, 0, 0, primary->buf.width, primary->buf.height);

    if (free_bufs) {
        display_free_dmabufs(vdev);
    }
}

static const GraphicHwOps display_dmabuf_ops = {
    .gfx_update = display_dmabuf_update,
};

static int display_dmabuf_init(VGTVGAState *vdev)
{
    if (!display_opengl) {
        return -1;
    }

    vdev->dpy = g_new0(XenGTDisplay, 1);
    vdev->dpy->con = graphic_console_init(DEVICE(vdev), 0,
                                          &display_dmabuf_ops,
                                          vdev);
    return 0;
}

static int display_probe(VGTVGAState *vdev)
{
    struct vfio_device_gfx_plane_info probe;
    int ret;

    memset(&probe, 0, sizeof(probe));
    probe.argsz = sizeof(probe);
    probe.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = ioctl(vdev->device_fd, VFIO_DEVICE_QUERY_GFX_PLANE, &probe);
    if (ret == 0) {
        return display_dmabuf_init(vdev);
    }

    return -1;
}


/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(VGTVGAState *vdev)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;
    int domid = vdev->domid;

    qemu_log("vGT: %s: domid=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, "
        "fence_sz=%d, vgt_primary=%d\n", __func__, domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary);
    if (vgt_low_gm_sz <= 0 || vgt_high_gm_sz <=0 ||
		vgt_primary < -1 || vgt_primary > 1 ||
        vgt_fence_sz <=0) {
        qemu_log("vGT: %s failed: invalid parameters!\n", __func__);
        abort();
    }

    if ((vgt_file = fopen(path, "w")) == NULL) {
        err = errno;
        qemu_log("vGT: open %s failed\n", path);
    }
    /* The format of the string is:
     * domid,aperture_size,gm_size,fence_size. This means we want the vgt
     * driver to create a vgt instanc for Domain domid with the required
     * parameters. NOTE: aperture_size and gm_size are in MB.
     */
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d,0\n", domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, err);
        exit(-1);
    }

    vdev->instance_created = TRUE;

    char devpath[16];
    int device_fd = -1;
    memset(devpath, 0, 16);

    snprintf(devpath, 16, "/dev/gvtg-%hi", vdev->domid);
    device_fd = open(devpath, O_RDWR);
    if (device_fd < 0) {
        err = errno;
        qemu_log("vGT: %s failed to open device: %s. errno=%d\n", __func__, devpath, err);
        exit(-1);
    }

    vdev->device_fd = device_fd;
    display_probe(vdev);
}

/*
 *  Inform vGT driver to close a vGT instance
 */
static void destroy_vgt_instance(VGTVGAState *vdev)
{
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    if(vdev->device_fd > 0) {
        close(vdev->device_fd);
        vdev->device_fd = -1;
    }

    if ((vgt_file = fopen(path, "w")) == NULL) {
        error_report("vgt: error: open %s failed", path);
        err = errno;
    }

    /* -domid means we want the vgt driver to free the vgt instance
     * of Domain domid.
     * */
    if (!err && fprintf(vgt_file, "%d\n", -vdev->domid) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s: failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

static int pch_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

static int vgt_host_device_get(VGTHostDevice *dev)
{
    char name[PATH_MAX];

    snprintf(name, sizeof(name), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config",
             dev->addr.domain, dev->addr.bus, dev->addr.slot, dev->addr.function);
    dev->config_fd = open(name, O_RDONLY);
    if (dev->config_fd == -1) {
        error_report("vgt:open failed: %s\n", strerror(errno));
        return -1;
     }

    return 0;
}

static void vgt_host_device_put(VGTHostDevice *dev)
{
    if (dev->config_fd >= 0) {
        close(dev->config_fd);
        dev->config_fd = -1;
    }
}

static int vgt_host_pci_cfg_get(VGTHostDevice *host_dev,
                                void *data, int len, uint32_t addr)
{
    int ret;

    vgt_host_device_get(host_dev);
    ret = pread(host_dev->config_fd, data, len, addr);
    if (ret < len) {
        ret = ret < 0 ? -errno : -EFAULT;
        error_report("failed to read device config space: %m");
        goto out;
    }

out:
    vgt_host_device_put(host_dev);
    return ret;
}

static void vgt_host_bridge_cap_init(PCIDevice *dev, VGTHostDevice *host_dev)
{
    assert(dev->devfn == 0x00);
    uint8_t cap_ptr = 0;

    vgt_host_pci_cfg_get(host_dev, &cap_ptr, 1, PCI_CAPABILITY_LIST);
    while (cap_ptr !=0) {
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr, 4, cap_ptr);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 4, 4,
                             cap_ptr + 4);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 8, 4,
                             cap_ptr + 8);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 12, 4,
                             cap_ptr + 12);
        vgt_host_pci_cfg_get(host_dev, &cap_ptr, 1, cap_ptr + 1);
    }
}

static void vgt_host_dev_init(PCIDevice *pdev, VGTHostDevice *host_dev)
{
    assert(pdev != NULL && host_dev != NULL);

    host_dev->addr.domain = 0;
    host_dev->addr.bus = pci_dev_bus_num(pdev);
    host_dev->addr.slot = PCI_SLOT(pdev->devfn);
    host_dev->addr.function = PCI_FUNC(pdev->devfn);
}

void vgt_bridge_pci_conf_init(PCIDevice *pdev)
{
    printf("vgt_bridge_pci_conf_init\n");
    VGTHostDevice host_dev;

    vgt_host_dev_init(pdev, &host_dev);

    vgt_host_pci_cfg_get(&host_dev, pdev->config, 2, 0x00);
    printf("vendor id: %x\n", *(uint16_t *)((char *)pdev->config + 0x00));

    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x02, 2, 0x02);
    printf("device id: %x\n", *(uint16_t *)((char *)pdev->config + 0x02));
    /* status */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x06, 2, 0x06);
    /* revision id */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x08, 2, 0x08);
    /* capability */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x34, 1, 0x34);
    vgt_host_bridge_cap_init(pdev, &host_dev);

    /* SNB: processor graphics control register */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x50, 2, 0x50);
    /* processor graphics control register */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x52, 2, 0x52);
}

static void vgt_reset(DeviceState *dev)
{
    PCIDevice *pdev = DO_UPCAST(PCIDevice, qdev, dev);
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, pdev);

    if (d->instance_created) {
        destroy_vgt_instance(d);
    }

    create_vgt_instance(d);
}

static void vgt_cleanupfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    if (d->instance_created) {
        destroy_vgt_instance(d);
    }
}

static int vgt_get_domid(void)
{
    int domid = 0;

    if (xen_enabled()) {
        domid = xen_domid;
    }
    assert(domid > 0);
    guest_domid = domid;

    return domid;
}

static void vgt_initfn(PCIDevice *dev, Error **errp)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    DPRINTF("vgt_initfn\n");
    vgt_host_dev_init(dev, &d->host_dev);
    d->domid = vgt_get_domid();
    d->state.parent = d;
    qemu_add_vm_change_state_handler(cpu_update_state, d);
}

DeviceState *vgt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    PCIBridge *br;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision_id;
    uint16_t class_dev;
    VGTHostDevice host_dev = {
        .addr.domain = 0,
        .addr.bus = 0,
        .addr.slot = 0x1f,
        .addr.function = 0,
    };

    vgt_host_pci_cfg_get(&host_dev, &vendor_id, sizeof(vendor_id),
                         PCI_VENDOR_ID);
    vgt_host_pci_cfg_get(&host_dev, &device_id, sizeof(device_id),
                         PCI_DEVICE_ID);
    vgt_host_pci_cfg_get(&host_dev, &revision_id, sizeof(revision_id),
                         PCI_REVISION_ID);
    vgt_host_pci_cfg_get(&host_dev, &class_dev, sizeof(class_dev),
                         PCI_CLASS_DEVICE);
    DPRINTF("vendor: 0x%hx, device: 0x%hx, revision: 0x%hhx\n",
            vendor_id, device_id, revision_id);

    if (vendor_id != PCI_VENDOR_ID_INTEL) {
        error_report("vgt: error: vgt-vga is only supported on Intel GPUs");
        return NULL;
    }

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x1f, 0), true,
                                   "vgt-isa");
    if (!dev) {
        error_report("vgt: error: vgt-isa not available");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);

    pci_config_set_vendor_id(dev->config, vendor_id);
    pci_config_set_device_id(dev->config, device_id);
    pci_config_set_revision(dev->config, revision_id);
    pci_config_set_class(dev->config, class_dev);
    br = PCI_BRIDGE(dev);
    pci_bridge_map_irq(br, "IGD Bridge",
                       pch_map_irq);

    printf("Create vgt ISA bridge successfully\n");

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x2, 0), true,
                                   "vgt-vga");
    if (!dev) {
        error_report("vgt: error: vgt-vga not available");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);
    printf("Create vgt VGA successfully\n");
    return DEVICE(dev);
}

static void vgt_pause(int domid, bool dev_state)
{
    char file_name[PATH_MAX] = {0};
    FILE *fp = NULL;
    int err = 0;

    snprintf(file_name, PATH_MAX, "/sys/kernel/vgt/vm%d/schedule", domid);

    fp = fopen(file_name, "w");
    if (fp == NULL) {
        err = errno;
        qemu_log("vGT: open %s failed\n", file_name);
    }

    if (!err && fprintf(fp, "%d\n", !dev_state) < 0) {
        err = errno;
    }

    if (!err && fclose(fp) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

static void cpu_update_state(void *pv, int running, RunState state)
{
    VGTVGAState *d = (VGTVGAState *) pv;
    if (!running) {
        vgt_pause(d->domid, true);
    }
}

static void read_write_state(QEMUFile *f, VGTVGAState *d, bool is_read)
{
    char file_name[PATH_MAX] = {0};
    FILE *fp = NULL;
    struct stat st;
    int sz;
    uint8_t *buf = NULL;
    int count = 0;

    snprintf(file_name, PATH_MAX, "/sys/kernel/vgt/vm%d/device_state", d->domid);

    fp = fopen(file_name, "r+");
    if (fp == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d",
                __func__, file_name, errno);
        goto EXIT;
    }

    fstat(fileno(fp), &st);
    sz = st.st_size;

    if (sz <= 0) {
        qemu_log("vGT: failed to achieve file size. file name=%s",
               file_name);
        goto EXIT;
    }

    buf = g_malloc(sz);
    if (buf == NULL) {
        qemu_log("vGT: %s failed to allocate memory size %d! errno = %d",
                __func__, sz, errno);
        goto EXIT;
    }

    DPRINTF("Allocate %d size of buffer for device state\n", sz);

    if (is_read) {
        count = fread(buf, 1, sz, fp);
        qemu_put_buffer(f, buf, sz);
    } else {
        qemu_get_buffer(f, buf, sz);
        count = fwrite(buf, 1, sz, fp);
    }

    if (count != sz) {
        qemu_log("vGT: read/write snapshot file size is differ %d:%d",
                count, sz);
    }

    DPRINTF("[%s] %d size of buffer for device state\n",
            is_read ? "READ":"WRITE", count);

EXIT:
    g_free(buf);
    if (fp) {
        fclose(fp);
    }
    return;
}

static int vgt_device_get(QEMUFile *f, void *pv,
                          size_t size, VMStateField *field)
{
    VGTVGAState *d = (VGTVGAState *) pv;

    read_write_state(f, d, false);

    vgt_pause(d->domid, false);
    return 0;
}

static int vgt_device_put(QEMUFile *f, void *pv, size_t size,
                          VMStateField *field, QJSON *vmdesc)
{
    VGTVGAState *d = (VGTVGAState *) pv;

    read_write_state(f, d, true);

    destroy_vgt_instance(d);
    return 0;
}

static const VMStateInfo vmstate_info_vgt = {
    .name = "vgt state",
    .get  = vgt_device_get,
    .put  = vgt_device_put,
};

static const VMStateDescription vmstate_vga_vgt = {
    .name = "vgt-vga",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        {
            .name         = "vgt dev",
            .version_id   = 0,
            .field_exists = NULL,
            .size         = 0,
            .info         = &vmstate_info_vgt,
            .flags        = VMS_SINGLE,
            .offset       = 0,
        },

        VMSTATE_END_OF_LIST()
    }
};

static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->realize = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_vgt;
}

static TypeInfo vgt_info = {
    .name          = "vgt-vga",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VGTVGAState),
    .class_init    = vgt_class_initfn,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    },
};

static TypeInfo isa_info = {
    .name          = "vgt-isa",
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(PCIBridge),
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    },

};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)