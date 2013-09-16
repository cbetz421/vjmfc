/*******************************************************************************
 * ==============================
 * Decoding initialization path
 * ==============================
 *
 * First the OUTPUT queue is initialized. With S_FMT the application
 * chooses which video format to decode and what size should be the
 * input buffer. Fourcc values have been defined for different codecs
 * e.g.  V4L2_PIX_FMT_H264 for h264. Then the OUTPUT buffers are
 * requested and mmaped. The stream header frame is loaded into the
 * first buffer, queued and streaming is enabled. At this point the
 * hardware is able to start processing the stream header and
 * afterwards it will have information about the video dimensions and
 * the size of the buffers with raw video data.
 *
 * The next step is setting up the CAPTURE queue and buffers. The
 * width, height, buffer size and minimum number of buffers can be
 * read with G_FMT call. The application can request more output
 * buffer if necessary. After requesting and mmaping buffers the
 * device is ready to decode video stream.
 *
 * The stream frames (ES frames) are written to the OUTPUT buffers,
 * and decoded video frames can be read from the CAPTURE buffers. When
 * no more source frames are present a single buffer with bytesused
 * set to 0 should be queued. This will inform the driver that
 * processing should be finished and it can dequeue all video frames
 * that are still left. The number of such frames is dependent on the
 * stream and its internal structure (how many frames had to be kept
 * as reference frames for decoding, etc).
 *
 * ===============
 *  Usage summary
 * ===============
 *
 * This is a step by step summary of the video decoding (from user
 * application point of view, with 2 treads and blocking api):
 *
 * 01. S_FMT(OUTPUT, V4L2_PIX_FMT_H264, ...)
 * 02. REQ_BUFS(OUTPUT, n)
 * 03. for i=1..n MMAP(OUTPUT, i)
 * 04. put stream header to buffer #1
 * 05. QBUF(OUTPUT, #1)
 * 06. STREAM_ON(OUTPUT)
 * 07. G_FMT(CAPTURE)
 * 08. REQ_BUFS(CAPTURE, m)
 * 09. for j=1..m MMAP(CAPTURE, j)
 * 10. for j=1..m QBUF(CAPTURE, #j)
 * 11. STREAM_ON(CAPTURE)
 *
 * display thread:
 * 12. DQBUF(CAPTURE) -> got decoded video data in buffer #j
 * 13. display buffer #j
 * 14. QBUF(CAPTURE, #j)
 * 15. goto 12
 *
 * parser thread:
 * 16. put next ES frame to buffer #i
 * 17. QBUF(OUTPUT, #i)
 * 18. DQBUF(OUTPUT) -> get next empty buffer #i 19. goto 16
 *
 * ...
 *
 * Similar usage sequence can be achieved with single threaded
 * application and non-blocking api with poll() call.
 *
 * https://lwn.net/Articles/419695/
 ******************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "v4l2_mfc.h"

/* compressed frame size. 1080p mpeg4 10Mb/s can be >256k in size, so
 * this is to make sure frame fits into buffer */
#define STREAM_BUFFER_SIZE 512000

struct mfc_buffer {
	int size[3];
	int offset[3];
	int bytesused[3];
	void *plane[3];
	int numplanes;
	int index;
	bool queue;
};

struct mfc_ctxt {
	int handler;
	struct mfc_buffer *out_buffers;
	int out_buffers_count;
};

static struct mfc_ctxt *
mfc_ctxt_new ()
{
	struct mfc_ctxt *ctxt = calloc (1, sizeof (struct mfc_ctxt));

	ctxt->handler = -1;

	return ctxt;
}

static void
mfc_free_buffer (int count, struct mfc_buffer *buffers)
{
	int i, j;

	for (i = 0; i < count; i++) {
		struct mfc_buffer *b = &buffers[i];

		for (j = 0; j < b->numplanes; j++) {
			if (b->plane[j] && b->plane != MAP_FAILED)
				munmap (b->plane[j], b->size[j]);
		}
	}

	free (buffers);
}

static void
mfc_ctxt_free (struct mfc_ctxt *ctxt)
{
	mfc_free_buffer (ctxt->out_buffers_count, ctxt->out_buffers);
	free (ctxt);
}

static char *
get_driver (char *fname)
{
	FILE *fp;
	char path[BUFSIZ];
	char *p, *driver = NULL;
	int s;
	size_t n = 0;

	s = snprintf (path, BUFSIZ, "/sys/class/video4linux/%s/name", fname);
	if (s >= BUFSIZ)
		return NULL;

	fp = fopen (path, "r");
	if (!fp)
		return NULL;

	s = getline (&driver, &n, fp);
	if (s < 0) {
		free (driver);
		return NULL;
	}

	p = strchr (driver, '\n');
	if (p)
		*p = '\0';

	return driver;
}

static char *
get_device (char *fname)
{
	char path[BUFSIZ], target[BUFSIZ];
	int s, ret;
	char *bname, *device;

	errno = 0;
	snprintf (path, BUFSIZ, "/sys/class/video4linux/%s", fname);
	ret = readlink (path, target, BUFSIZ);
	if (ret < 0 || ret == BUFSIZ || errno)
		return NULL;
	target[ret] = '\0'; /* why do we need this? */

	bname = basename (target);
	device = (char *) malloc (1024 * sizeof (char));
	s = snprintf (device, 1024, "/dev/%s", bname);
	if (s >= 1024)
		return NULL;

	return device;
}

static void
open_and_query (char *device, int *handler)
{
	int fd;

	fd = open (device, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
		return;

	if (v4l2_mfc_querycap (fd) == 0) {
		*handler = fd;
		return;
	}

	close (fd);
}

static bool
open_device (struct mfc_ctxt *ctxt)
{
	DIR *dir;
	struct dirent *ent;
	char *driver, *device;

	dir = opendir ("/sys/class/video4linux/");
	if (!dir)
		return false;

	while ((ent = readdir (dir)) != NULL) {
		if (strncmp (ent->d_name, "video", 5) != 0)
			continue;

		driver = get_driver (ent->d_name);
		if (!driver)
			continue;

		device = get_device (ent->d_name);
		if (device && ctxt->handler == -1 && strstr (driver, "s5p-mfc-dec")) {
			open_and_query (device, &ctxt->handler);
		}

		free (driver);
		free (device);
	}

	closedir (dir);
	return ctxt->handler != -1;
}

static bool
mfc_ctxt_init (struct mfc_ctxt *ctxt)
{
	if (!open_device (ctxt))
		return false;

	return true;
}

static void
close_device (struct mfc_ctxt *ctxt)
{
	close (ctxt->handler);
	ctxt->handler = -1;
}

static bool
mfc_ctxt_deinit (struct mfc_ctxt *ctxt)
{
	close_device (ctxt);
	return true;
}

#if 0
static bool
mfc_output_set_format ()
{
	int ret;
	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.fmt.pix_mp = {
			.pixelformat = V4L2_PIX_FMT_H264,
			.num_planes = 3,
		}
	};
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = STREAM_BUFFER_SIZE;

	ret = ioctl (decoder_handler, VIDIOC_S_FMT, &fmt);
	if (ret != 0) {
		perror ("video set format failed: ");
		return false;
	}

	return true;
}

static bool
mfc_output_get_format ()
{
	int ret;
	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	};

	/* get format */
	ret = ioctl (decoder_handler, VIDIOC_G_FMT, &fmt);
	if (ret != 0) {
		perror ("video get format failed: ");
		return false;
	}

	return fmt.fmt.pix_mp.plane_fmt[0].sizeimage == STREAM_BUFFER_SIZE;
}

static int
mfc_request_output_buffers ()
{
	int ret;
	struct v4l2_requestbuffers reqbuf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.count = 2,
	};

	ret = ioctl (decoder_handler, VIDIOC_REQBUFS, &reqbuf);
	if (ret != 0) {
		perror ("request output buffers failed: ");
		return -1;
	}

	return reqbuf.count;
}

static bool
mfc_map_output_buffers ()
{
	int i, j, ret;
	struct v4l2_plane planes[3];
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes,
		.length = 3,
	};

	for (i = 0; i < out_buffers_count; i++) {
		buf.index = i;

		ret = ioctl(decoder_handler, VIDIOC_QUERYBUF, &buf);
		if (ret != 0) {
			perror ("query output buffers failed: ");
			return false;
		}

		struct mfc_buffer *b = &out_buffers[i];
		b->numplanes = 0;
		for (j = 0; j < 3; j++) {
			b->size[j] = buf.m.planes[j].length;
			b->bytesused[j] = buf.m.planes[j].bytesused;

			if (b->size[j] == 0)
				continue;

			b->plane[j] = mmap (NULL,
					    buf.m.planes[j].length,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED,
					    decoder_handler,
					    buf.m.planes[j].m.mem_offset);

			if (b->plane[j] == MAP_FAILED) {
				perror ("mapping output buffers failed: ");
				return false;
			}

			memset (b->plane[j], 0, b->size[j]);
			b->numplanes++;
		}

		b->index = i;
	}

	return true;
}

static bool
mfc_queue_output_buffer (struct mfc_buffer *b)
{
	int i, ret;
	struct v4l2_plane planes[3];
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes,
		.length = b->numplanes,
	};

	for (i = 0; i < b->numplanes; i++) {
		planes[i].m.userptr = (unsigned long) b->plane[i];
		planes[i].length = b->size[i];
		planes[i].bytesused = b->bytesused[i];
	}

	ret = ioctl (decoder_handler, VIDIOC_QBUF, &buf);
	if (ret != 0) {
		perror ("queue input buffer");
		return false;
	}

	return true;
}

static bool
setup_mfc_output ()
{
	if (!(mfc_output_set_format () && mfc_output_get_format ()))
		return false;

	out_buffers_count = mfc_request_output_buffers ();
	if (out_buffers_count == -1)
		return false;

	out_buffers = (struct mfc_buffer *) calloc (out_buffers_count,
						    sizeof (struct mfc_buffer));
	if (!out_buffers)
		return false;

	if (!mfc_map_output_buffers ())
		return false;

	struct mfc_buffer *b = &out_buffers[0];
	if (!mfc_queue_output_buffer (b))
		return false;

	return true;
}

static void
mfc_free_buffers ()
{
	if (out_buffers)
		mfc_free_buffer (out_buffers_count, out_buffers);
}
#endif

int
main (int argc, char **argv)
{
	int ret = EXIT_FAILURE;
	struct mfc_ctxt *ctxt = mfc_ctxt_new ();

	if (!mfc_ctxt_init (ctxt))
		goto bail;

	ret = EXIT_SUCCESS;

bail:
	mfc_ctxt_deinit (ctxt);
	mfc_ctxt_free (ctxt);
	return ret;
}
