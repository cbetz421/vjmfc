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
#include <unistd.h>
#include <sys/mman.h>

#include "v4l2_mfc.h"
#include "dev.h"
#include "av.h"

/* compressed frame size. 1080p mpeg4 10Mb/s can be >256k in size, so
 * this is to make sure frame fits into buffer */
#define STREAM_BUFFER_SIZE 512000

struct mfc_buffer {
       int size[2];
       int offset[2];
       int bytesused[2];
       void *plane[2];
       int numplanes;
       int index;
       bool queue;
};

struct mfc_ctxt {
	int handler;
	AVFormatContext *fc;
	struct mfc_buffer *out;
	int oc;
};

static struct mfc_ctxt *
mfc_ctxt_new ()
{
	struct mfc_ctxt *ctxt = calloc (1, sizeof (struct mfc_ctxt));
	ctxt->handler = -1;
	return ctxt;
}

static bool
mfc_ctxt_open (struct mfc_ctxt *ctxt, const char *filename)
{
	ctxt->fc = av_context_new (filename);
	return ctxt->fc != NULL;
}

static void
mfc_ctxt_close (struct mfc_ctxt *ctxt)
{
	if (ctxt->fc)
		av_context_free (&ctxt->fc);
}

static void
unmap_buffers (struct mfc_ctxt *ctxt)
{
	int i, j;

	for (i = 0; i < ctxt->oc; i++) {
		struct mfc_buffer *b = &ctxt->out[i];

		for (j = 0; j < b->numplanes; j++) {
			if (b->plane[j] && b->plane[j] != MAP_FAILED)
				munmap (b->plane[j], b->size[j]);
		}
	}
}

static void
mfc_ctxt_free (struct mfc_ctxt *ctxt)
{
	unmap_buffers (ctxt);
	free (ctxt->out);
	free (ctxt);
}


static bool
map_buffer (int fd, struct v4l2_buffer *buf, struct mfc_buffer *out)
{
	int i;

	for (i = 0; i < 2; i++) {
		out->size[i] = buf->m.planes[i].length;
		out->bytesused[i] = buf->m.planes[i].bytesused;

		if (out->size[i] == 0)
			continue;

		out->plane[i] = mmap (NULL,
				      buf->m.planes[i].length,
				      PROT_READ | PROT_WRITE,
				      MAP_SHARED,
				      fd,
				      buf->m.planes[i].m.mem_offset);

		if (out->plane[i] == MAP_FAILED)
			return false;

		memset (out->plane[i], 0, out->size[i]);
		out->numplanes++;
	}

	return true;
}

static bool
queue_buffers (struct mfc_ctxt *ctxt)
{
	int i;
	struct v4l2_plane planes[2];
	struct v4l2_buffer buf;
	struct mfc_buffer *b;

	for (i = 0; i < ctxt->oc; i++) {
		if (v4l2_mfc_querybuf (ctxt->handler,
				       &buf,
				       V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				       V4L2_MEMORY_MMAP,
				       i, planes) != 0) {
			perror ("query output buffers failed: ");
			return false;
		}

		b = &ctxt->out[i];
		if (!map_buffer (ctxt->handler, &buf, b)) {
			perror ("mapping output buffers failed: ");
			return false;
		}
		b->index = i;

		for (i = 0; i < b->numplanes; i++) {
			planes[i].m.userptr = (unsigned long) b->plane[i];
			planes[i].length = b->size[i];
			planes[i].bytesused = b->bytesused[i];
		}

		int frame_length = b->bytesused[0]; /* 0 */
		if (v4l2_mfc_qbuf (ctxt->handler,
				   &buf,
				   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				   V4L2_MEMORY_MMAP,
				   i, planes, frame_length) != 0) {
			perror ("queue input buffer");
			return false;
		}
	}

	return true;
}

static bool
mfc_ctxt_init (struct mfc_ctxt *ctxt, uint32_t codec)
{
	ctxt->handler = v4l2_find_device("s5p-mfc-dec");
	if (ctxt->handler == -1)
		return false;

	if (v4l2_mfc_s_fmt (ctxt->handler, codec, 1024 * 3072) != 0) {
		perror ("Couldn't set format: ");
		return false;
	}

	int count = 2;
	if (v4l2_mfc_reqbufs (ctxt->handler,
			      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			      V4L2_MEMORY_MMAP,
			      &count) != 0) {
		perror ("Couldn't request buffers: ");
		return false;
	}

	ctxt->oc = count; /* output buffers count */
	ctxt->out = (struct mfc_buffer *) calloc (count, sizeof (struct mfc_buffer));

	if (!queue_buffers (ctxt))
		return false;

	if (v4l2_mfc_streamon (ctxt->handler,
			       V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) != 0) {
		perror ("Couldn't set stream on: ");
		return false;
	}

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

int
main (int argc, char **argv)
{
	int ret = EXIT_FAILURE;

	if (argc != 2) {
		fprintf (stderr, "Missing video path argument.\n");
		return ret;
	}

	struct mfc_ctxt *ctxt = mfc_ctxt_new ();
	if (!mfc_ctxt_open (ctxt, argv[1]))
		goto bail;

	uint32_t codec = get_codec_id (ctxt->fc);
	if (codec == 0)
		goto bail;

	if (!mfc_ctxt_init (ctxt, codec))
		goto bail;

	ret = EXIT_SUCCESS;

bail:
	mfc_ctxt_close (ctxt);
	mfc_ctxt_deinit (ctxt);
	mfc_ctxt_free (ctxt);
	return ret;
}
