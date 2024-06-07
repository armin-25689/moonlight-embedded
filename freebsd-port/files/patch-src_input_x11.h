--- src/input/x11.h.orig	2024-02-20 04:01:31 UTC
+++ src/input/x11.h
@@ -20,3 +20,4 @@ void x11_input_init(Display* display, Window window);
 #include <X11/Xlib.h>
 
 void x11_input_init(Display* display, Window window);
+void x11_input_remove();
