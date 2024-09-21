#ifdef HAVE_GBM
extern struct DISPLAY_CALLBACK display_callback_gbm;
#endif
void export_bo(struct Source_Buffer_Info buffers[MAX_FB_NUM], int *buffer_num, int *plane_num);
