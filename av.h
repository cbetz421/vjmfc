#ifndef AV_H_
#define AV_H_

#include <libavformat/avformat.h>
#include <stdint.h>

AVFormatContext *av_context_new (const char *fname);
void av_context_free (AVFormatContext **fctxt);


uint32_t get_codec_id (AVFormatContext *ic);
uint8_t *get_codec_extradata (AVFormatContext *ic, int *size);

#endif
