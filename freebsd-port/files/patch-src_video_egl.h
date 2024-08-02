--- src/video/egl.h.orig	2024-08-01 13:37:02 UTC
+++ src/video/egl.h
@@ -17,8 +17,10 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
-#include <EGL/egl.h>
+enum WindowType {X11_WINDOW=1, WAYLAND_WINDOW=2, GBM_WINDOW=4};
+// 1 is x11 ;2 is wayland ;4 is gbm
+extern enum WindowType windowType;
 
-void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height);
-void egl_draw(uint8_t* image[3]);
+void egl_init(void *native_display, void *native_window, int frame_width, int frame_height, int screen_width, int screen_height, int dcFlag);
+void egl_draw(AVFrame* frame);
 void egl_destroy();
