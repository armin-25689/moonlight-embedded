struct SwsContext* sws_init(int src_w, int src_h, int src_fmt, int dst_w, int dst_h, int dst_fmt, int threads_count);
int convert_frame_to_packet_format(struct SwsContext *sws_ctx, AVFrame *dst_frame, AVFrame *src_frame, uint8_t *data, int data_size, int pitch);
