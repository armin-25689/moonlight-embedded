--- src/input/udev.c.orig	2024-02-20 04:01:31 UTC
+++ src/input/udev.c
@@ -29,7 +29,6 @@
 #include <stdio.h>
 #include <string.h>
 #include <stdlib.h>
-#include <poll.h>
 
 static bool autoadd, debug;
 static struct mapping* defaultMappings;
@@ -38,7 +37,7 @@ static int inputRotate;
 static struct udev_monitor *udev_mon;
 static int inputRotate;
 
-static int udev_handle(int fd) {
+static int udev_handle(int fd, void *data) {
   struct udev_device *dev = udev_monitor_receive_device(udev_mon);
   const char *action = udev_device_get_action(dev);
   if (action != NULL) {
@@ -91,7 +90,7 @@ void udev_init(bool autoload, struct mapping* mappings
   defaultMappings = mappings;
   inputRotate = rotate;
 
-  loop_add_fd(udev_monitor_get_fd(udev_mon), &udev_handle, POLLIN);
+  loop_add_fd(udev_monitor_get_fd(udev_mon), &udev_handle, EPOLLIN);
 }
 
 void udev_destroy() {
