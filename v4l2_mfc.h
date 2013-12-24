#ifndef MFC_H_
#define MFC_H_

#include <linux/videodev2.h>
#include <stdint.h>

int v4l2_mfc_querycap (int fd);

int v4l2_mfc_s_fmt (int fd,
		    uint32_t pfmt,
		    unsigned int size);

int v4l2_mfc_reqbufs (int fd,
		      enum v4l2_buf_type type,
		      enum v4l2_memory memory,
		      int *buf_cnt);

int v4l2_mfc_querybuf (int fd,
		       int index,
		       enum v4l2_memory memory,
		       struct v4l2_plane *planes,
		       struct v4l2_buffer *buf);

int v4l2_mfc_streamon (int fd, enum v4l2_buf_type type);

int v4l2_mfc_streamoff (int fd, enum v4l2_buf_type type);

int v4l2_mfc_s_ctrl (int fd, int id, int value);

int v4l2_mfc_g_ctrl (int fd, int id, int *value);

int v4l2_mfc_qbuf (int fd,
		   struct v4l2_buffer *qbuf,
		   enum v4l2_buf_type type,
		   enum v4l2_memory memory,
		   int index,
		   struct v4l2_plane *planes,
		   int frame_length);

int v4l2_mfc_dqbuf (int fd,
		    struct v4l2_buffer *dqbuf,
		    enum v4l2_buf_type type,
		    enum v4l2_memory memory);

int v4l2_mfc_g_fmt (int fd,
		    struct v4l2_format *fmt);

int v4l2_mfc_g_crop (int fd,
		     struct v4l2_crop *crop,
		     enum v4l2_buf_type type);

int v4l2_mfc_poll (int fd,
		   int *revents,
		   int timeout);

#endif
