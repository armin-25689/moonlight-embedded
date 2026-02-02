int convert_init(AVFrame *frame, int width, int height);
int convert_frame(AVFrame * src_frame, uint8_t *dst_buffer[4], uint32_t pitch[4], int dst_fmt);
void convert_destroy();
