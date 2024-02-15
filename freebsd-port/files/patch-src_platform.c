--- src/platform.c.orig	2023-11-03 06:08:34 UTC
+++ src/platform.c
@@ -76,7 +76,7 @@ enum platform platform_check(char* name) {
   #ifdef HAVE_X11
   bool x11 = strcmp(name, "x11") == 0;
   bool vdpau = strcmp(name, "x11_vdpau") == 0;
-  bool vaapi = strcmp(name, "x11_vaapi") == 0;
+  bool vaapi = strcmp(name, "x11_vaapi") == 0 || strcmp(name, "wayland_vaapi") == 0 || strcmp(name, "vaapi") == 0;
   if (std || x11 || vdpau || vaapi) {
     int init = x11_init(std || vdpau, std || vaapi);
     #ifdef HAVE_VAAPI
@@ -201,6 +201,9 @@ AUDIO_RENDERER_CALLBACKS* platform_get_audio(enum plat
     #endif
     #ifdef HAVE_ALSA
     return &audio_callbacks_alsa;
+    #endif
+    #ifdef __FreeBSD__
+    return &audio_callbacks_oss;
     #endif
   }
   return NULL;
