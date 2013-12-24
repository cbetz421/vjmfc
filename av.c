#include <libavcodec/avcodec.h>
#include <linux/videodev2.h>

#include "av.h"

AVFormatContext *
av_context_new (const char *fname)
{
	AVFormatContext *ic = NULL;

	av_register_all ();

	if (avformat_open_input (&ic, fname, NULL, NULL) < 0)
		return NULL;

	if (avformat_find_stream_info (ic, NULL) < 0) {
		avformat_close_input (&ic);
		return NULL;
	}

	return ic;
}

void
av_context_free (AVFormatContext **fctxt)
{
	avformat_close_input (fctxt);
}

uint32_t
get_codec_id (AVFormatContext *ic)
{
	unsigned int i;
	AVCodecContext *cc;
	enum AVCodecID codec = AV_CODEC_ID_NONE;

	for (i = 0; i < ic->nb_streams; i++) {
		cc = ic->streams[i]->codec;
		if (cc->codec_type == AVMEDIA_TYPE_VIDEO) {
			codec = cc->codec_id;
			break;
		}
        }

	switch (codec) {
	case AV_CODEC_ID_H264:
		return V4L2_PIX_FMT_H264;
	case AV_CODEC_ID_MPEG4:
		return V4L2_PIX_FMT_MPEG4;
	case AV_CODEC_ID_H263:
		return V4L2_PIX_FMT_H263;
	case AV_CODEC_ID_MPEG2VIDEO:
		return V4L2_PIX_FMT_MPEG2;
	case AV_CODEC_ID_MPEG1VIDEO:
		return V4L2_PIX_FMT_MPEG1;
	default:
		break;
	}

	return 0;
}
