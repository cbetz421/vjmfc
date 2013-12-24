#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "dev.h"
#include "v4l2_mfc.h"

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
get_device (const char *fname)
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

int
v4l2_find_device (const char *drivername)
{
	DIR *dir;
	struct dirent *ent;
	char *driver, *device;
	int handler;

	handler = -1;
	dir = opendir ("/sys/class/video4linux/");
	if (!dir)
		return handler;

	while ((ent = readdir (dir)) != NULL) {
		if (strncmp (ent->d_name, "video", 5) != 0)
			continue;

		driver = get_driver (ent->d_name);
		if (!driver)
			continue;

		device = get_device (ent->d_name);
		if (device && handler == -1 && strstr (driver, drivername)) {
			open_and_query (device, &handler);
		}

		free (driver);
		free (device);
	}

	closedir (dir);
	return handler;
}
