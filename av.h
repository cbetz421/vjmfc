#include <libavformat/avformat.h>
#include <stdint.h>

AVFormatContext *av_context_new (const char *fname);
void av_context_free (AVFormatContext **fctxt);


uint32_t get_codec_id (AVFormatContext *ic);
