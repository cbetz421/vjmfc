#include "v4l2_mfc.h"

#include <string.h>
#include <stdio.h>
#include <poll.h>

#include <sys/ioctl.h>

int
v4l2_mfc_querycap (int fd)
{
	int ret;
	struct v4l2_capability cap;

	ret = ioctl (fd, VIDIOC_QUERYCAP, &cap);
	if (ret != 0) {
		perror ("VIDIOC_QUERYCAP failed: ");
		return ret;
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
		fprintf (stderr, "Device does not support capture\n");
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE)) {
		fprintf (stderr, "Device does not support output\n");
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf (stderr, "Device does not support streaming\n");
		return -1;
	}

	return 0;
}

int
v4l2_mfc_s_fmt (int fd,
		uint32_t pfmt,
		unsigned int size)
{
	int ret;
	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.fmt.pix_mp = {
			.num_planes = 1,
			.pixelformat = pfmt,
			.plane_fmt = {
				[0] = {
					.sizeimage = size,
				},
			},
		},
	};

	ret = ioctl (fd, VIDIOC_S_FMT, &fmt);
	return ret;
}

int
v4l2_mfc_reqbufs (int fd,
		  enum v4l2_buf_type type,
		  enum v4l2_memory memory,
		  uint32_t *buf_cnt)
{
	int ret;
	struct v4l2_requestbuffers reqbuf = {
		.type = type,
		.memory = memory,
		.count = *buf_cnt,
	};

	ret = ioctl (fd, VIDIOC_REQBUFS, &reqbuf);
	*buf_cnt = reqbuf.count;

	return ret;
}

int
v4l2_mfc_querybuf (int fd,
		   int index,
		   enum v4l2_buf_type type,
		   enum v4l2_memory memory,
		   struct v4l2_plane *planes,
		   struct v4l2_buffer *buf)
{
	int ret;

	struct v4l2_buffer b = {
		.type = type,
		.memory = memory,
		.index = index,
		.m.planes = planes,
	};

	ret = ioctl (fd, VIDIOC_QUERYBUF, &b);

	if (buf)
		memcpy(buf, &b, sizeof (struct v4l2_buffer));

	return ret;
}

int
v4l2_mfc_streamon (int fd,
		   enum v4l2_buf_type type)
{
    int ret;

    ret = ioctl (fd, VIDIOC_STREAMON, &type);
    return ret;
}

int
v4l2_mfc_streamoff (int fd,
		    enum v4l2_buf_type type)
{
    int ret;

    ret = ioctl (fd, VIDIOC_STREAMOFF, &type);
    return ret;
}

int
v4l2_mfc_s_ctrl (int fd,
		 int id,
		 int value)
{
    int ret;
    struct v4l2_control ctrl = {
	    .id = id,
	    .value = value,
    };

    ret = ioctl (fd, VIDIOC_S_CTRL, &ctrl);
    return ret;
}

int
v4l2_mfc_g_ctrl (int fd,
		 int id,
		 int *value)
{
    int ret;
    struct v4l2_control ctrl = {
	    .id = id,
    };

    ret = ioctl (fd, VIDIOC_G_CTRL, &ctrl);
    *value = ctrl.value;

    return ret;
}

int
v4l2_mfc_qbuf (int fd,
	       struct v4l2_buffer *buf)
{
	int ret;

	ret = ioctl (fd, VIDIOC_QBUF, buf);
	return ret;
}

int
v4l2_mfc_dqbuf (int fd,
		struct v4l2_buffer *dqbuf,
		enum v4l2_buf_type type,
		enum v4l2_memory memory)
{
	struct v4l2_plane planes[2];
	int ret, length = 0;

	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		length = 1;
	else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		length = 2;

	dqbuf->type = type;
	dqbuf->memory = memory;
	dqbuf->m.planes = planes;
	dqbuf->length = length;

	ret = ioctl (fd, VIDIOC_DQBUF, dqbuf);
	return ret;
}

int
v4l2_mfc_g_fmt (int fd,
		struct v4l2_format *fmt)
{
    int ret;

    struct v4l2_format f = {
	    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    ret = ioctl (fd, VIDIOC_G_FMT, &f);

    if (fmt)
	    memcpy (fmt, &f, sizeof (struct v4l2_format));

    return ret;
}

int
v4l2_mfc_g_crop (int fd,
		 struct v4l2_crop *crop,
		 enum v4l2_buf_type type)
{
    int ret;

    crop->type = type;
    ret = ioctl (fd, VIDIOC_G_CROP, crop);
    return ret;
}

int
v4l2_mfc_poll (int fd,
	       int *revents,
	       int timeout)
{
    int ret;
    struct pollfd poll_events = {
	    .fd = fd,
	    .events = POLLOUT | POLLERR,
	    .revents = 0,
    };

    ret = poll ((struct pollfd*) &poll_events, 1, timeout);
    *revents = poll_events.revents;

    return ret;
}
