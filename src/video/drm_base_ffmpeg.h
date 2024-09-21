// for ffmpeg
// 0 is success,-1 is failed
int ffmpeg_init_drm_hw_ctx(const char *device, const enum AVPixelFormat pixel_format);
// init ls behined to prepare_drm_decoder_context
int ffmpeg_bind_drm_ctx(AVCodecContext* decoder_ctx, AVDictionary** dict);
