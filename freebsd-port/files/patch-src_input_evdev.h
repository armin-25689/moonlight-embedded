--- src/input/evdev.h.orig	2023-11-03 06:08:34 UTC
+++ src/input/evdev.h
@@ -19,6 +19,9 @@
 
 #include "mapping.h"
 
+#define GRABCODE "grab"
+#define UNGRABCODE "ungrab"
+
 extern int evdev_gamepads;
 
 void evdev_create(const char* device, struct mapping* mappings, bool verbose, int rotate);
@@ -29,3 +32,6 @@ void evdev_rumble(unsigned short controller_id, unsign
 void evdev_stop();
 void evdev_map(char* device);
 void evdev_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor);
+void evdev_trans_op_fd(int fd);
+void is_use_kbdmux();
+int x11_sdl_init(char* mappings);
