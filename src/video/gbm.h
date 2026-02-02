#include "render.h"

int generate_gbm_bo(int gbm_fd, struct _drm_buf gbm_buf[], int buffer_num, void *display, int width, int height, int src_fmt, uint64_t size[MAX_PLANE_NUM]);
int generate_gbm_buffer(int gbm_fd, struct _drm_buf gbm_buf[], int buffer_num, void *display, int width, int height, int src_fmt);
void gbm_close_display (int gbm_fd, void *gbm_buf, int buffer_num, void *display, void *window);
void* gbm_get_window(int gbm_fd, void * display, int width, int height, uint32_t format);
void* gbm_get_display(int *gbm_fd);
int gbm_convert_image(struct Render_Image *image, struct _drm_buf *drm_buf, int gbm_fd, int handle_num, int plane_num, int dst_fmt, uint64_t size[MAX_PLANE_NUM], uint64_t map_offset[MAX_PLANE_NUM]);
