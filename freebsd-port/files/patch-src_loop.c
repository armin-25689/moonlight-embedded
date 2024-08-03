--- src/loop.c.orig	2024-02-20 04:01:31 UTC
+++ src/loop.c
@@ -20,23 +20,20 @@
 #include "loop.h"
 
 #include "connection.h"
-
 #include <sys/stat.h>
 #include <sys/signalfd.h>
 #include <stdlib.h>
 #include <stdio.h>
 #include <unistd.h>
-#include <poll.h>
 #include <signal.h>
 #include <string.h>
+#include <errno.h>
 
-static struct pollfd* fds = NULL;
-static FdHandler* fdHandlers = NULL;
-static int numFds = 0;
-
+static struct FD_Function fd_functions[maxEpollFds] = {};
+static int epoll_fd = -1;
 static int sigFd;
 
-static int loop_sig_handler(int fd) {
+static int loop_sig_handler(int fd, void *data) {
   struct signalfd_siginfo info;
   if (read(fd, &info, sizeof(info)) != sizeof(info))
     return LOOP_RETURN;
@@ -50,42 +47,88 @@ static int loop_sig_handler(int fd) {
   return LOOP_OK;
 }
 
-void loop_add_fd(int fd, FdHandler handler, int events) {
-  int fdindex = numFds;
-  numFds++;
+static inline int create_epoll_data (struct epoll_event *eventsi, int fd, void *data, Fd_Handler handler, int events, int opt) {
+  if (!handler || fd < 0 || events <= 0) {
+    fprintf(stderr, "Can not add fd to epoll because of null handler\n");
+    return -1;
+  }
 
-  if (fds == NULL) {
-    fds = malloc(sizeof(struct pollfd));
-    fdHandlers = malloc(sizeof(FdHandler*));
-  } else {
-    fds = realloc(fds, sizeof(struct pollfd)*numFds);
-    fdHandlers = realloc(fdHandlers, sizeof(FdHandler*)*numFds);
+  int index = -1;
+  if (opt == EPOLL_CTL_MOD) {
+    for (int i = 0; i < maxEpollFds; i++) {
+      if (fd_functions[i].fd == fd) {
+        index = i;
+        break;
+      }
+    }
   }
+  else {
+    for (int i = 0; i < maxEpollFds; i++) {
+      if (!fd_functions[i].func) {
+        index = i;
+        break;
+      }
+    }
+  }
+  if (index < 0) {
+    fprintf(stderr, "Can not add fd to epoll because of max fd numbers\n");
+    return -1;
+  }
+  fd_functions[index].fd = fd;
+  fd_functions[index].data = data;
+  fd_functions[index].func = handler;
+  fd_functions[index].events = events;
+  eventsi->events = events;
+  eventsi->data.fd = fd;
+  eventsi->data.ptr = (void *)(&fd_functions[index]);
+  return 0;
+}
 
-  if (fds == NULL || fdHandlers == NULL) {
-    fprintf(stderr, "Not enough memory\n");
+static inline void fd_ctl(int fd, void *data, Fd_Handler handler, int events, int opt) {
+  struct epoll_event event_data = {0};
+
+  if (create_epoll_data(&event_data, fd, data, handler, events, opt) < 0)
+    return;
+  int err = epoll_ctl(epoll_fd, opt, fd, &event_data);
+  if (err < 0) {
+    fprintf(stderr, "Can not add fd to epoll:%d\n", errno);
     exit(EXIT_FAILURE);
   }
+  return;
+}
 
-  fds[fdindex].fd = fd;
-  fds[fdindex].events = events;
-  fdHandlers[fdindex] = handler;
+void loop_add_fd(int fd, Fd_Handler handler, int events) {
+  return fd_ctl(fd, NULL, handler, events, EPOLL_CTL_ADD);
 }
 
-void loop_remove_fd(int fd) {
-  numFds--;
-  int fdindex = numFds;
+void loop_add_fd1(int fd, Fd_Handler handler, int events, void *data) {
+  return fd_ctl(fd, data, handler, events, EPOLL_CTL_ADD);
+}
 
-  for (int i=0;i<=numFds;i++) {
-    if (fds[i].fd == fd) {
-      fdindex = i;
+void loop_mod_fd(int fd, Fd_Handler handler, int events, void *data) {
+  return fd_ctl(fd, data, handler, events, EPOLL_CTL_MOD);
+}
+
+void loop_remove_fd(int fd) {
+  int err = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
+  if (err < 0) {
+    fprintf(stderr, "Can not delelte fd from epoll:%d\n", errno);
+    return;
+  }
+  for (int i = 0; i < maxEpollFds; i++) {
+    if (fd_functions[i].fd == fd) {
+      memset(&fd_functions[i], 0, sizeof(struct FD_Function));
       break;
     }
   }
+  return;
+}
 
-  if (fdindex != numFds && numFds > 0) {
-    memcpy(&fds[fdindex], &fds[numFds], sizeof(struct pollfd));
-    memcpy(&fdHandlers[fdindex], &fdHandlers[numFds], sizeof(FdHandler));
+void loop_create() {
+  epoll_fd = epoll_create(maxEpollFds);
+  if (epoll_fd < 0) {
+    fprintf(stderr, "Can not create epoll fd: %d\n", errno);
+    exit(EXIT_FAILURE);
   }
 }
 
@@ -99,18 +142,29 @@ void loop_init() {
   sigaddset(&sigset, SIGQUIT);
   sigprocmask(SIG_BLOCK, &sigset, NULL);
   sigFd = signalfd(-1, &sigset, 0);
-  loop_add_fd(sigFd, loop_sig_handler, POLLIN | POLLERR | POLLHUP);
+  loop_add_fd(sigFd, &loop_sig_handler, EPOLLIN | EPOLLERR | EPOLLHUP);
 }
 
 void loop_main() {
-  while (poll(fds, numFds, -1)) {
-    for (int i=0;i<numFds;i++) {
-      if (fds[i].revents > 0) {
-        int ret = fdHandlers[i](fds[i].fd);
-        if (ret == LOOP_RETURN) {
-          return;
-        }
+  bool done = false;
+  while (!done) {
+    struct epoll_event events[maxEpollFds] = {0};
+    int fd_events = epoll_wait(epoll_fd, events, maxEpollFds, -1);
+    if (fd_events < 0) {
+      done = true;
+    }
+    for (int i = 0 ;i < fd_events; i++) {
+      struct FD_Function *function = (struct FD_Function *)events[i].data.ptr;
+      int ret = function->func(function->fd, function->data);
+      if (ret == LOOP_RETURN) {
+        done = true;
       }
     }
   }
+}
+
+void loop_destroy() {
+  if (epoll_fd >= 0)
+    close(epoll_fd);
+  epoll_fd = -1;
 }
