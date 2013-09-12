#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <linux/videodev2.h>

int decoder_handler;
int converter_handler;
int video_handler;

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


int
main (int argc, char **argv)
{
	int ret = EXIT_SUCCESS;

	if (!open_devices ())
		ret = EXIT_FAILURE;

	close_devices ();

	return ret;
}
