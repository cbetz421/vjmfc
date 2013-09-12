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
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* compressed frame size. 1080p mpeg4 10Mb/s can be >256k in size, so
 * this is to make sure frame fits into buffer */
#define STREAM_BUFFER_SIZE 512000

int decoder_handler;
int converter_handler;
int video_handler;

struct mfc_buffer {
	int size[3];
	int offset[3];
	int bytesused[3];
	void *plane[3];
	int numplanes;
	int index;
	bool queue;
};

struct mfc_buffer *out_buffers = NULL;
int out_buffers_count = 0;

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

inline static uint32_t
query_caps (int fd)
{
	struct v4l2_capability cap;
	int ret;

	ret = ioctl (fd, VIDIOC_QUERYCAP, &cap);
	if (ret != 0)
		return 0;

	return cap.capabilities;
}

static bool
check_m2m_caps (int fd)
{
	uint32_t c;

	c = query_caps (fd);
	return ((c & V4L2_CAP_VIDEO_M2M_MPLANE) ||
		(((c & V4L2_CAP_VIDEO_CAPTURE_MPLANE) &&
		  (c & V4L2_CAP_VIDEO_OUTPUT_MPLANE)) &&
		 (c & V4L2_CAP_STREAMING)));

}

static bool
check_output_caps (int fd)
{
	uint32_t c;

	c = query_caps (fd);
	return (c & V4L2_CAP_VIDEO_OUTPUT_MPLANE &&
		c & V4L2_CAP_STREAMING);
}

static void
check_and_open (char *device, int *handler, bool (*check_func)(int))
{
	int fd;

	fd = open (device, O_RDWR | O_NONBLOCK, 0);
	if (fd < 0)
		return;

	if (check_func (fd)) {
		printf ("Found %s\n", device);
		*handler = fd;
		return;
	}

	close (fd);
}

static bool
open_devices ()
{
	DIR *dir;
	struct dirent *ent;
	char *driver, *device;

	decoder_handler = converter_handler = video_handler = -1;

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
		if (!device) {
			free (driver);
			continue;
		}

		if (decoder_handler == -1 && strstr (driver, "s5p-mfc-dec")) {
			check_and_open (device, &decoder_handler, check_m2m_caps);
		} else if (converter_handler ==  -1 &&
			   (strstr (driver, "fimc") && strstr (driver, "m2m"))) {
			check_and_open (device, &converter_handler, check_m2m_caps);
		} else if (strstr (driver, "video0")) {
			check_and_open (device, &video_handler, check_output_caps);
		}

		free (driver);
		free (device);
	}

	closedir (dir);
	return decoder_handler != -1 &&
		converter_handler != -1 &&
		video_handler != -1;
}

static void
close_devices ()
{
	close (decoder_handler);
	decoder_handler = -1;
	close (converter_handler);
	converter_handler = -1;
	close (video_handler);
	video_handler = -1;
}

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
mfc_map_output_buffers (int count)
{
	int i, j, ret;
	struct v4l2_plane planes[3];
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes,
		.length = 3,
	};

	for (i = 0; i < count; i++) {
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

	if (!mfc_map_output_buffers (out_buffers_count))
		return false;

	return true;
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
mfc_free_buffers ()
{
	if (out_buffers)
		mfc_free_buffer (out_buffers_count, out_buffers);
}

int
main (int argc, char **argv)
{
	int ret = EXIT_SUCCESS;

	if (!open_devices ()) {
		ret = EXIT_FAILURE;
		goto bail;
	}

	if (!setup_mfc_output ()) {
		ret = EXIT_FAILURE;
		goto bail;
	}

bail:
	mfc_free_buffers ();
	close_devices ();
	return ret;
}
