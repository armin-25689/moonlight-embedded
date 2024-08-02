#define WAYLAND 0x04
#define QUITCODE "quit"

void* wl_get_display(const char *deivce);
void* wl_get_window();
void wl_close_display();
int wayland_setup(int width, int height, int drFlags);
void wl_setup_post();
void wl_show_picture();
void wl_dispatch_event();
void wl_get_resolution(int *width, int *height);
void wl_trans_op_fd(int fd);
void wl_change_cursor(const char *op);
