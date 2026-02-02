#include "drm_base.h"

int drm_import_hw_buffer (int drm_fd, struct _drm_buf *drm_buf, struct Source_Buffer_Info *buffer, int planes, int composeOrSeperate, void* *image, int index);
