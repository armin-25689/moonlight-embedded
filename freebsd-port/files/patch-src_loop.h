--- src/loop.h.orig	2024-02-20 04:01:31 UTC
+++ src/loop.h
@@ -17,13 +17,27 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
+#include <sys/epoll.h>
+
+#define maxEpollFds 150
 #define LOOP_RETURN 1
 #define LOOP_OK 0
 
-typedef int(*FdHandler)(int fd);
+typedef int(*Fd_Handler)(int fd, void *data);
 
-void loop_add_fd(int fd, FdHandler handler, int events);
+struct FD_Function {
+  Fd_Handler func;
+  void*   data;
+  int     fd;
+  int     events;
+};
+
+void loop_add_fd(int fd, Fd_Handler handler, int events);
+void loop_add_fd1(int fd, Fd_Handler handler, int events, void *data);
+void loop_mod_fd(int fd, Fd_Handler handler, int events, void *data);
 void loop_remove_fd(int fd);
 
+void loop_create();
 void loop_init();
 void loop_main();
+void loop_destroy();
