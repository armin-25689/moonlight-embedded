--- src/platform.c.orig	2024-02-20 04:01:31 UTC
+++ src/platform.c
@@ -74,9 +74,9 @@ enum platform platform_check(char* name) {
   }
   #endif
   #ifdef HAVE_X11
-  bool x11 = strcmp(name, "x11") == 0;
+  bool x11 = strcmp(name, "x11") == 0 || strcmp(name, "wayland") == 0;
   bool vdpau = strcmp(name, "x11_vdpau") == 0;
-  bool vaapi = strcmp(name, "x11_vaapi") == 0;
+  bool vaapi = strcmp(name, "x11_vaapi") == 0 || strcmp(name, "vaapi") == 0 || strcmp(name, "wayland_vaapi") == 0;
   if (std || x11 || vdpau || vaapi) {
     int init = x11_init(std || vdpau, std || vaapi);
     #ifdef HAVE_VAAPI
@@ -87,11 +87,7 @@ enum platform platform_check(char* name) {
     if (init == INIT_VDPAU)
       return X11_VDPAU;
     #endif
-    #ifdef HAVE_SDL
-    return SDL;
-    #else
     return X11;
-    #endif
   }
   #endif
   #ifdef HAVE_SDL
