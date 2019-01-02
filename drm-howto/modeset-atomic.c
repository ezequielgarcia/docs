/*
 * modeset-atomic - DRM Double-Buffered Atomic-API Modesetting Example
 *
 * Based on:
 *
 * modeset - DRM Double-Buffered VSync'ed Modesetting Example
 *
 * Written 2012 by David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain.
 */

/*
 * DRM Double-Buffered VSync'ed Modesetting Howto
 * This example extends modeset-double-buffered.c and introduces page-flips
 * synced with vertical-blanks (vsync'ed). A vertical-blank is the time-period
 * when a display-controller pauses from scanning out the framebuffer. After the
 * vertical-blank is over, the framebuffer is again scanned out line by line and
 * followed again by a vertical-blank.
 *
 * Vertical-blanks are important when changing a framebuffer. We already
 * introduced double-buffering, so this example shows how we can flip the
 * buffers during a vertical blank and _not_ during the scanout period.
 *
 * This example assumes that you are familiar with modeset-double-buffered. Only
 * the differences between both files are highlighted here.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct modeset_buf;
struct modeset_dev;
static int modeset_find_plane(int fd, struct modeset_dev *dev);
static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_buf *buf);
static void modeset_destroy_fb(int fd, struct modeset_buf *buf);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_open(int *out, const char *node);
static int modeset_prepare(int fd);
static void modeset_draw(int fd);
static void modeset_draw_dev(int fd, struct modeset_dev *dev);
static void modeset_cleanup(int fd);

static int preferred_w = 1280;
static int preferred_h = 720;

/*
 * modeset_open() stays the same.
 */

static int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t has_dumb;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
		return ret;
	}

	/* Setting atomic caps implies universal planes caps.
	 * However, it's better not to rely in such implicit
	 * behavior. So we set universal planes cap explicitly.
	 */
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "failed to set atomic cap, %d", ret);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

/*
 * modeset_buf and modeset_dev stay mostly the same. But 6 new fields are added
 * to modeset_dev: r, g, b, r_up, g_up, b_up. They are used to compute the
 * current color that is drawn on this output device. You can ignore them as
 * they aren't important for this example.
 * The modeset-double-buffered.c example used exactly the same fields but as
 * local variables in modeset_draw().
 *
 * The \pflip_pending variable is true when a page-flip is currently pending,
 * that is, the kernel will flip buffers on the next vertical blank. The
 * \cleanup variable is true if the device is currently cleaned up and no more
 * pageflips should be scheduled. They are used to synchronize the cleanup
 * routines.
 */

struct drm_object {
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct modeset_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_dev {
	struct modeset_dev *next;

	unsigned int front_buf;
	struct modeset_buf bufs[2];

	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;

	uint32_t plane_id;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t crtc_index; /* this is not really persistent */
	drmModeCrtc *saved_crtc;

	bool pflip_pending;
	bool cleanup;

	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};

static struct modeset_dev *modeset_list = NULL;

/*
 * modeset_prepare() stays the same.
 */

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_dev *dev;
	int ret;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn_id = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = modeset_setup_dev(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

int modeset_setup_objects(int fd, struct modeset_dev *dev)
{
	struct drm_object *connector = &dev->connector;
	struct drm_object *crtc = &dev->crtc;
	struct drm_object *plane = &dev->plane;
	uint32_t plane_id = dev->plane_id;
	uint32_t crtc_id = dev->crtc_id;
	uint32_t conn_id = dev->conn_id;
	int i;

	/* Connector properties */
	connector->props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props) {
		fprintf(stderr, "cannot get connector %d properties: %s\n", conn_id, strerror(errno));
		return -ENOMEM;
	}
	connector->props_info = calloc(connector->props->count_props, sizeof(connector->props_info));
	for (i = 0; i < connector->props->count_props; i++)
		connector->props_info[i] = drmModeGetProperty(fd, connector->props->props[i]);	

	/* CRTC properties */
	crtc->props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props) {
		fprintf(stderr, "cannot get crtc %d properties: %s\n", crtc_id, strerror(errno));
		return -ENOMEM;
	}
	crtc->props_info = calloc(crtc->props->count_props, sizeof(crtc->props_info));
	for (i = 0; i < crtc->props->count_props; i++)
		crtc->props_info[i] = drmModeGetProperty(fd, crtc->props->props[i]);	

	/* Plane properties */
	plane->props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!plane->props) {
		fprintf(stderr, "cannot get plane %d properties: %s\n", plane_id, strerror(errno));
		return -ENOMEM;
	}
	plane->props_info = calloc(plane->props->count_props, sizeof(plane->props_info));
	for (i = 0; i < plane->props->count_props; i++)
		plane->props_info[i] = drmModeGetProperty(fd, plane->props->props[i]);	

	return 0;
}

/*
 * modeset_setup_dev() stays the same.
 */

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	bool found_mode = false;
	int i, ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	/* find a matching mode to user requested mode */
	for (i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].hdisplay == preferred_w &&
		    conn->modes[i].vdisplay == preferred_h) {
			/* copy the mode information into our device structure and into both
			 * buffers */
			memcpy(&dev->mode, &conn->modes[i], sizeof(dev->mode));
			dev->bufs[0].width = conn->modes[i].hdisplay;
			dev->bufs[0].height = conn->modes[i].vdisplay;
			dev->bufs[1].width = conn->modes[i].hdisplay;
			dev->bufs[1].height = conn->modes[i].vdisplay;
			fprintf(stdout, "mode for connector %u is %ux%u\n",
				conn->connector_id, dev->bufs[0].width, dev->bufs[0].height);
			found_mode = true;
			break;
		}
	}

	if (!found_mode) {
		fprintf(stderr, "mode %ux%u not found for connector %u\n",
			preferred_w, preferred_h, conn->connector_id);
		return -ENOENT;
	}

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* with a connector and crtc, find a primary plane */
	ret = modeset_find_plane(fd, dev);
	if (ret) {
		fprintf(stderr, "no valid plane for crtc %u\n", dev->crtc_id);
		return ret;
	}

	ret = modeset_setup_objects(fd, dev);
	if (ret) {
		fprintf(stderr, "cannot get plane properties\n");
		return ret;
	}

	/* create framebuffer #1 for this CRTC */
	ret = modeset_create_fb(fd, &dev->bufs[0]);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* create framebuffer #2 for this CRTC */
	ret = modeset_create_fb(fd, &dev->bufs[1]);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		modeset_destroy_fb(fd, &dev->bufs[0]);
		return ret;
	}

	return 0;
}

/*
 * modeset_find_crtc() stays the same.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	struct modeset_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;

			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				fprintf(stdout, "crtc %u already connected to encoder %u\n",
					crtc, conn->encoder_id);
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc) {
					crtc = -1;
					break;
				}
			}

			/* We have found a CRTC, so save it and return.
			 * Note that we have to save its index as well.
			 * The CRTC index (not its ID) will be used
			 * when searching for a suitable plane.
			 * TODO: the index is not really persistent?
			 */
			if (crtc >= 0) {
				fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
					crtc, conn->encoders[i]);;
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc;
				dev->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

static uint64_t get_property_value(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	int j;
	
	for (j = 0; j < props->count_props; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name))
			value = props->prop_values[j];
		drmModeFreeProperty(prop);
	}
	return value;
}

/*
 * TODO: need doc
 */
static int modeset_find_plane(int fd, struct modeset_dev *dev)
{
	drmModePlaneResPtr plane_res;
	bool found_primary = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -ENOENT;
	}

	for (i = 0; (i < plane_res->count_planes) && !found_primary; i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id, strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << dev->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

			/* primary or not, this plane is good enough to use. */
			dev->plane_id = plane_id;
			ret = 0;

			/* Get the "type" property and check if this is a primary plane */
			if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY)
				/* found our primary plane, lets use that! */
				found_primary = true;
			
			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if (found_primary)
		fprintf(stdout, "found primary plane, id: %d\n", dev->plane_id);
	else
		fprintf(stdout, "found non-primary plane, id: %d\n", dev->plane_id);
	return ret;
}
/*
 * modeset_create_fb() stays the same.
 */

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
			   buf->handle, &buf->fb);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/*
 * modeset_destroy_fb() stays the same.
 */

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(fd, buf->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

/*
 * main() also stays the same.
 */

int main(int argc, char **argv)
{
	int ret, fd;
	const char *card;
	struct modeset_dev *iter;

	/* check which DRM device to open */
	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	if (argc > 3) {
		preferred_w = atoi(argv[2]);
		preferred_h = atoi(argv[3]);
	}

	fprintf(stderr, "using card '%s'\n", card);

	/* open the DRM device */
	ret = modeset_open(&fd, card);
	if (ret)
		goto out_return;

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	/* save crtc object, so we can restore it later */
	for (iter = modeset_list; iter; iter = iter->next)
		iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc_id);

	/* draw some colors for 5seconds */
	modeset_draw(fd);

	/* cleanup everything */
	modeset_cleanup(fd);

	ret = 0;

out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return ret;
}

/*
 * modeset_page_flip_event() is a callback-helper for modeset_draw() below.
 * Please see modeset_draw() for more information.
 *
 * Note that this does nothing if the device is currently cleaned up. This
 * allows to wait for outstanding page-flips during cleanup.
 */

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modeset_dev *dev = data;

	dev->pflip_pending = false;
	if (!dev->cleanup)
		modeset_draw_dev(fd, dev);
}

static int set_crtc_property(drmModeAtomicReq *req,
	struct drm_object *crtc, uint32_t crtc_id,
	const char *name, uint64_t value)
{
	int i, prop_id = -1;

	for (i = 0; i < crtc->props->count_props; i++) {
		if (!strcmp(crtc->props_info[i]->name, name)) {
			prop_id = crtc->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
                fprintf(stderr, "no crtc property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, crtc_id, prop_id, value);
}

static int set_connector_property(drmModeAtomicReq *req,
	struct drm_object *connector, uint32_t conn_id,
	const char *name, uint64_t value)
{
	int i, prop_id = -1;

	for (i = 0; i < connector->props->count_props; i++) {
		if (!strcmp(connector->props_info[i]->name, name)) {
			prop_id = connector->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
                fprintf(stderr, "no connector property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, conn_id, prop_id, value);
}

static int set_plane_property(drmModeAtomicReq *req,
	struct drm_object *plane, uint32_t plane_id,
	const char *name, uint64_t value)
{
        int i, prop_id = -1;

        for (i = 0 ; i < plane->props->count_props ; i++) {
                if (!strcmp(plane->props_info[i]->name, name)) {
                        prop_id = plane->props_info[i]->prop_id;
                        break;
                }
        }

        if (prop_id < 0) {
                fprintf(stderr, "no plane property: %s\n", name);
                return -EINVAL;
        }

        return drmModeAtomicAddProperty(req, plane_id, prop_id, value);
}

static int modeset_atomic_modeset(int fd, struct modeset_dev *dev)
{
	drmModeAtomicReq *req;
	uint32_t blob_id;
	int ret, flags;

	req = drmModeAtomicAlloc();

	if (set_connector_property(req, &dev->connector, dev->conn_id, "CRTC_ID", dev->crtc_id) < 0)
		return -1;

	if (drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode), &blob_id) != 0)
		return -1;

	if (set_crtc_property(req, &dev->crtc, dev->crtc_id, "MODE_ID", blob_id) < 0)
		return -1;

	if (set_crtc_property(req, &dev->crtc, dev->crtc_id, "ACTIVE", 1) < 0)
		return -1;

	flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);

	drmModeAtomicFree(req);

	return ret;
}

static int modeset_atomic_commit(int fd, struct modeset_buf *buf, struct modeset_dev *dev)
{
	struct drm_object *plane = &dev->plane;
        drmModeAtomicReq *req;
	int ret, flags;

        req = drmModeAtomicAlloc();

        set_plane_property(req, plane, dev->plane_id, "FB_ID", buf->fb);
        set_plane_property(req, plane, dev->plane_id, "CRTC_ID", dev->crtc_id);
        set_plane_property(req, plane, dev->plane_id, "SRC_X", 0);
        set_plane_property(req, plane, dev->plane_id, "SRC_Y", 0);
        set_plane_property(req, plane, dev->plane_id, "SRC_W", buf->width << 16);
        set_plane_property(req, plane, dev->plane_id, "SRC_H", buf->height << 16);
        set_plane_property(req, plane, dev->plane_id, "CRTC_X", 0);
        set_plane_property(req, plane, dev->plane_id, "CRTC_Y", 0);
        set_plane_property(req, plane, dev->plane_id, "CRTC_W", buf->width);
        set_plane_property(req, plane, dev->plane_id, "CRTC_H", buf->height);

	/* We want to receive the page-flip event and we want this
	 * call to be non-blocking.
	 */
	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
        ret = drmModeAtomicCommit(fd, req, flags, dev);

	drmModeAtomicFree(req);

	return ret;
}

/*
 * modeset_draw() changes heavily from all previous examples. The rendering has
 * moved into another helper modeset_draw_dev() below, but modeset_draw() is now
 * responsible of controlling when we have to redraw the outputs.
 *
 * So what we do: first redraw all outputs. We initialize the r/g/b/_up
 * variables of each output first, although, you can safely ignore these.
 * They're solely used to compute the next color. Then we call
 * modeset_draw_dev() for each output. This function _always_ redraws the output
 * and schedules a buffer-swap/flip for the next vertical-blank.
 * We now have to wait for each vertical-blank to happen so we can draw the next
 * frame. If a vblank happens, we simply call modeset_draw_dev() again and wait
 * for the next vblank.
 *
 * Note: Different monitors can have different refresh-rates. That means, a
 * vblank event is always assigned to a CRTC. Hence, we get different vblank
 * events for each CRTC/modeset_dev that we use. This also means, that our
 * framerate-controlled color-morphing is different on each monitor. If you want
 * exactly the same frame on all monitors, we would have to share the
 * color-values between all devices. However, for simplicity reasons, we don't
 * do this here.
 *
 * So the last piece missing is how we get vblank events. libdrm provides
 * drmWaitVBlank(), however, we aren't interested in _all_ vblanks, but only in
 * the vblanks for our page-flips. We could use drmWaitVBlank() but there is a
 * more convenient way: drmModePageFlip()
 * drmModePageFlip() schedules a buffer-flip for the next vblank and then
 * notifies us about it. It takes a CRTC-id, fb-id and an arbitrary
 * data-pointer and then schedules the page-flip. This is fully asynchronous and
 * returns immediately.
 * When the page-flip happens, the DRM-fd will become readable and we can call
 * drmHandleEvent(). This will read all vblank/page-flip events and call our
 * modeset_page_flip_event() callback with the data-pointer that we passed to
 * drmModePageFlip(). We simply call modeset_draw_dev() then so the next frame
 * is rendered..
 *
 *
 * So modeset_draw() is reponsible of waiting for the page-flip/vblank events
 * for _all_ currently used output devices and schedule a redraw for them. We
 * could easily do this in a while (1) { drmHandleEvent() } loop, however, this
 * example shows how you can use the DRM-fd to integrate this into your own
 * main-loop. If you aren't familiar with select(), poll() or epoll, please read
 * it up somewhere else. There is plenty of documentation elsewhere on the
 * internet.
 *
 * So what we do is adding the DRM-fd and the keyboard-input-fd (more precisely:
 * the stdin FD) to a select-set and then we wait on this set. If the DRM-fd is
 * readable, we call drmHandleEvents() to handle the page-flip events. If the
 * input-fd is readable, we exit. So on any keyboard input we exit this loop
 * (you need to press RETURN after each keyboard input to make this work).
 */

static void modeset_draw(int fd)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;
	struct modeset_dev *iter;

	/* Initial modeset on all outputs.
	 * This may be a full-modeset, if needed.
	 * Also, note that this is done with a blocking
	 * call.
	 */
	for (iter = modeset_list; iter; iter = iter->next) {
		modeset_atomic_modeset(fd, iter);
	}

	/* draw on all outputs for the first time */
	for (iter = modeset_list; iter; iter = iter->next) {
		iter->r = rand() % 0xff;
		iter->g = rand() % 0xff;
		iter->b = rand() % 0xff;
		iter->r_up = iter->g_up = iter->b_up = true;

		modeset_draw_dev(fd, iter);
	}

	/* init variables */
	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));
	/* Set this to only the latest version you support. Version 2
	 * introduced the page_flip_handler, so we use that. */
	ev.version = 2;
	ev.page_flip_handler = modeset_page_flip_event;

	/* wait 5s for VBLANK or input events */
	while (time(&cur) < start + 30) {
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		v.tv_sec = start + 30 - cur;

		ret = select(fd + 1, &fds, NULL, NULL, &v);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(fd, &fds)) {
			drmHandleEvent(fd, &ev);
		}
	}
}

/*
 * A short helper function to compute a changing color value. No need to
 * understand it.
 */

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

/*
 * modeset_draw_dev() is a new function that redraws the screen of a single
 * output. It takes the DRM-fd and the output devices as arguments, redraws a
 * new frame and schedules the page-flip for the next vsync.
 *
 * This function does the same as modeset_draw() did in the previous examples
 * but only for a single output device now.
 * After we are done rendering a frame, we have to swap the buffers. Instead of
 * calling drmModeSetCrtc() as we did previously, we now want to schedule this
 * page-flip for the next vertical-blank (vblank). We use drmModePageFlip() for
 * this. It takes the CRTC-id and FB-id and will asynchronously swap the buffers
 * when the next vblank occurs. Note that this is done by the kernel, so neither
 * a thread is started nor any other magic is done in libdrm.
 * The DRM_MODE_PAGE_FLIP_EVENT flag tells drmModePageFlip() to send us a
 * page-flip event on the DRM-fd when the page-flip happened. The last argument
 * is a data-pointer that is returned with this event.
 * If we wouldn't pass this flag, we would not get notified when the page-flip
 * happened.
 *
 * Note: If you called drmModePageFlip() and directly call it again, it will
 * return EBUSY if the page-flip hasn't happened in between. So you almost
 * always want to pass DRM_MODE_PAGE_FLIP_EVENT to get notified when the
 * page-flip happens so you know when to render the next frame.
 * If you scheduled a page-flip but call drmModeSetCrtc() before the next
 * vblank, then the scheduled page-flip will become a no-op. However, you will
 * still get notified when it happens and you still cannot call
 * drmModePageFlip() again until it finished. So to sum it up: there is no way
 * to effectively cancel a page-flip.
 *
 * If you wonder why drmModePageFlip() takes fewer arguments than
 * drmModeSetCrtc(), then you should take into account, that drmModePageFlip()
 * reuses the arguments from drmModeSetCrtc(). So things like connector-ids,
 * x/y-offsets and so on have to be set via drmModeSetCrtc() first before you
 * can use drmModePageFlip()! We do this in main() as all the previous examples
 * did, too.
 */

static void modeset_draw_dev(int fd, struct modeset_dev *dev)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;
	int ret;

	dev->r = next_color(&dev->r_up, dev->r, 5);
	dev->g = next_color(&dev->g_up, dev->g, 5);
	dev->b = next_color(&dev->b_up, dev->b, 5);

	buf = &dev->bufs[dev->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] =
				     (dev->r << 16) | (dev->g << 8) | dev->b;
		}
	}

	ret = modeset_atomic_commit(fd, buf, dev);
	if (ret) {
		fprintf(stderr, "atomic commit failed, %d\n", errno);
	} else {
		dev->front_buf ^= 1;
		dev->pflip_pending = true;
	}
}

/*
 * modeset_cleanup() stays mostly the same. However, before resetting a CRTC to
 * its previous state, we wait for any outstanding page-flip to complete. This
 * isn't strictly neccessary, however, some DRM drivers are known to be buggy if
 * we call drmModeSetCrtc() if there is a pending page-flip.
 * Furthermore, we don't want any pending page-flips when our application exist.
 * Because another application might pick up the DRM device and try to schedule
 * their own page-flips which might then fail as long as our page-flip is
 * pending.
 * So lets be safe here and simply wait for any page-flips to complete. This is
 * a blocking operation, but it's mostly just <16ms so we can ignore that.
 */

static void modeset_cleanup(int fd)
{
	struct modeset_dev *iter;
	drmEventContext ev;
	int ret;

	/* init variables */
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = modeset_page_flip_event;

	while (modeset_list) {
		/* remove from global list */
		iter = modeset_list;
		modeset_list = iter->next;

		/* if a pageflip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		/* restore saved CRTC configuration */
		if (!iter->pflip_pending)
			drmModeSetCrtc(fd,
				       iter->saved_crtc->crtc_id,
				       iter->saved_crtc->buffer_id,
				       iter->saved_crtc->x,
				       iter->saved_crtc->y,
				       &iter->conn_id,
				       1,
				       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* destroy framebuffers */
		modeset_destroy_fb(fd, &iter->bufs[1]);
		modeset_destroy_fb(fd, &iter->bufs[0]);

		/* free allocated memory */
		free(iter);
	}
}

/*
 * This example shows how to make the kernel handle page-flips and how to wait
 * for them in user-space. The select() example here should show you how you can
 * integrate these loops into your own applications without the need for a
 * separate modesetting thread.
 *
 * However, please note that vsync'ed double-buffering doesn't solve all
 * problems. Imagine that you cannot render a frame fast enough to satisfy all
 * vertical-blanks. In this situation, you don't want to wait after scheduling a
 * page-flip until the vblank happens to draw the next frame. A solution for
 * this is triple-buffering. It should be farily easy to extend this example to
 * use triple-buffering, but feel free to contact me if you have any questions
 * about it.
 * Also note that the DRM kernel API is quite limited if you want to reschedule
 * page-flips that haven't happened, yet. You cannot call drmModePageFlip()
 * twice in a single scanout-period. The behavior of drmModeSetCrtc() while a
 * page-flip is pending might also be unexpected.
 * Unfortunately, there is no ultimate solution to all modesetting problems.
 * This example shows the tools to do vsync'ed page-flips, however, it depends
 * on your use-case how you have to implement it.
 *
 * If you want more code, I can recommend reading the source-code of:
 *  - plymouth (which uses dumb-buffers like this example; very easy to understand)
 *  - kmscon (which uses libuterm to do this)
 *  - wayland (very sophisticated DRM renderer; hard to understand fully as it
 *             uses more complicated techniques like DRM planes)
 *  - xserver (very hard to understand as it is split across many files/projects)
 *
 * Any feedback is welcome. Feel free to use this code freely for your own
 * documentation or projects.
 *
 *  - Hosted on http://github.com/dvdhrm/docs
 *  - Written by David Herrmann <dh.herrmann@googlemail.com>
 */
