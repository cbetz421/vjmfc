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

static int
open_devices ()
{
	DIR *dir;
	struct dirent *ent;
	char *driver, *device;
	int fd;
	bool ret;

	dir = opendir ("/sys/class/video4linux/");
	if (!dir)
		return -1;

	ret = false;
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

		fd = open (device, O_RDWR | O_NONBLOCK, 0);
		if (fd < 0) {
			free (driver);
			free (device);
			continue;
		}

		if (strstr (driver, "s5p-mfc-dec")) {
			if (check_m2m_caps (fd)) {
				printf ("Found %s in %s\n", driver, device);
			}
		} else if (strstr (driver, "fimc") && strstr (driver, "m2m")) {
			if (check_m2m_caps (fd)) {
				printf ("Found %s in %s\n", driver, device);
			}
		} else if (strstr (driver, "video0")) {
			if (check_output_caps (fd)) {
				printf ("Found %s in %s\n", driver, device);
			}
		}

		close (fd);
		free (driver);
		free (device);
		fd = -1;
	}

	closedir (dir);
	return ret;
}

int
main (int argc, char **argv)
{
	open_devices ();

	return 0;
}
