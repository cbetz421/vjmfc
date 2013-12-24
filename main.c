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
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>

#include "v4l2_mfc.h"
#include "dev.h"
#include "av.h"

enum dir { IN, OUT };

struct mfc_buffer {
	void *paddr[2];
	struct v4l2_plane planes[2];
	struct v4l2_buffer buf;
};

struct mfc_ctxt {
	int handler;
	AVFormatContext *fc;
	struct mfc_buffer *in, *out;
	uint32_t ic, oc;
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
	if (!ctxt->fc)
		return false;

	char *dev = v4l2_find_device("s5p-mfc-dec");
	if (!dev)
		return false;

	ctxt->handler = open (dev, O_RDWR | O_NONBLOCK, 0);
	free (dev);
	if (ctxt->handler < 0)
		return false;

	if (v4l2_mfc_querycap (ctxt->handler) != 0)
		return false;

	return true;
}

static void
mfc_ctxt_close (struct mfc_ctxt *ctxt)
{
	if (ctxt->fc)
		av_context_free (&ctxt->fc);

	if (ctxt->handler != -1) {
		close (ctxt->handler);
		ctxt->handler = -1;
	}
}

static void
unmap_buffers (struct mfc_ctxt *ctxt)
{
	uint32_t i, j;

	for (i = 0; i < ctxt->oc; i++) {
		struct mfc_buffer *b = &ctxt->out[i];

		for (j = 0; j < b->buf.length; j++) {
			if (b->paddr[j] && b->paddr[j] != MAP_FAILED)
				munmap (b->paddr[j], b->planes[j].length);
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

inline static bool
map_planes (int fd, struct mfc_buffer *b)
{
	uint32_t i;
	struct v4l2_buffer *buf = &b->buf;

	for (i = 0; i < buf->length; i++) {
		if (buf->m.planes[i].length == 0)
			continue;

		b->paddr[i] = mmap (NULL,
				    buf->m.planes[i].length,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED,
				    fd,
				    buf->m.planes[i].m.mem_offset);

		if (b->paddr[i] == MAP_FAILED)
			return false;

		memset (b->paddr[i], 0, buf->m.planes[i].length);
	}

	return true;
}

static bool
create_buffers (struct mfc_ctxt *ctxt, enum dir d)
{
	uint32_t i, c;
	struct mfc_buffer *b;

	c = (d == IN) ? ctxt->ic : ctxt->oc;
	b = (d == IN) ? ctxt->in : ctxt->out;

	for (i = 0; i < c; i++) {
		if (v4l2_mfc_querybuf (ctxt->handler,
				       i,
				       V4L2_MEMORY_MMAP,
				       b[i].planes,
				       &b[i].buf) != 0) {
			perror ("query buffers failed: ");
			return false;
		}

		printf ("> %s buffer %d has %d plane(s)\n",
			(d == IN) ? "input" : "output", i, b[i].buf.length);
		assert (b[i].buf.length < 2);

		if (!map_planes (ctxt->handler, b)) {
			perror ("mapping buffers failed: ");
			return false;
		}
	}

	return true;
}

static bool
queue_buffers (struct mfc_ctxt *ctxt, enum dir d)
{
	uint32_t i, c;
	struct mfc_buffer *b;

	c = (d == IN) ? ctxt->ic : ctxt->oc;
	b = (d == IN) ? ctxt->in : ctxt->out;

	for (i = 0; i < c; i++) {
		if (v4l2_mfc_qbuf (ctxt->handler, &b[i].buf) != 0) {
			perror ("queue input buffer");
			return false;
		}
	}

	return true;
}

static void
fill_first_input_buffer (struct mfc_ctxt *ctxt)
{
	struct mfc_buffer *b;
	uint8_t *data;
	int size;

	b = &ctxt->in[0];

	data = get_codec_extradata (ctxt->fc, &size);
	if (size > 0)
		memcpy (b->paddr, data, size);
}

static bool
mfc_ctxt_init (struct mfc_ctxt *ctxt)
{
	uint32_t codec = get_codec_id (ctxt->fc);
	if (codec == 0) {
		perror ("Couldn't recognize the codec: ");
		return false;
	}

	if (v4l2_mfc_s_fmt (ctxt->handler, codec, 1024 * 3072) != 0) {
		perror ("Couldn't set format: ");
		return false;
	}

	uint32_t count = 2; /* because I want */
	if (v4l2_mfc_reqbufs (ctxt->handler,
			      V4L2_MEMORY_MMAP,
			      &count) != 0) {
		perror ("Couldn't request buffers: ");
		return false;
	}

	ctxt->ic = count; /* input buffers count */
	ctxt->in = (struct mfc_buffer *) calloc (count, sizeof (struct mfc_buffer));

	if (!create_buffers (ctxt, IN))
		return false;

	return true;
}

static bool
mfc_ctxt_deinit (struct mfc_ctxt *ctxt)
{
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
	if (!mfc_ctxt_open (ctxt, argv[1])) {
		perror ("Couldn't open input file: ");
		goto bail;
	}

	if (!mfc_ctxt_init (ctxt))
		goto bail;

	fill_first_input_buffer (ctxt);

	if (!queue_buffers (ctxt, IN)) {
		perror ("Couldn't queue input buffers");
		goto bail;
	}

	if (v4l2_mfc_streamon (ctxt->handler) != 0) {
		perror ("Couldn't set stream on: ");
		goto bail;
	}

	ret = EXIT_SUCCESS;

bail:
	mfc_ctxt_close (ctxt);
	mfc_ctxt_deinit (ctxt);
	mfc_ctxt_free (ctxt);
	return ret;
}
