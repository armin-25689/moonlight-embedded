int ffmpeg_init_filter(AVFrame *frame, AVCodecContext *decoder_ctx, bool usehdr, uint16_t *hdr_metadata, int (*decode_frame) (AVFrame *frame, bool native));
int ffmpeg_filter_destroy();
int ffmpeg_filte_frame(AVFrame *frame, AVCodecContext *decoder_ctx, int (*decode_frame) (AVFrame *frame, bool native));
int ffmpeg_modify_filter_action (int action);
int ffmpeg_reject_filter_action (int action);
void ffmpeg_filter_stop_filte ();
