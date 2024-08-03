--- src/main.c.orig	2024-08-03 07:59:40 UTC
+++ src/main.c
@@ -42,6 +42,7 @@
 #include <client.h>
 #include <discover.h>
 
+#include <time.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <stdbool.h>
@@ -52,8 +53,7 @@
 #include <netinet/in.h>
 #include <netdb.h>
 #include <arpa/inet.h>
-#include <openssl/rand.h>
-
+ 
 static void applist(PSERVER_DATA server) {
   PAPP_LIST list = NULL;
   if (gs_applist(server, &list) != GS_OK) {
@@ -149,6 +149,7 @@ static void stream(PSERVER_DATA server, PCONFIGURATION
     if (!config->viewonly)
       evdev_start();
     loop_main();
+    loop_destroy();
     if (!config->viewonly)
       evdev_stop();
   }
@@ -166,6 +167,7 @@ static void stream(PSERVER_DATA server, PCONFIGURATION
   }
 
   platform_stop(system);
+  config_clear();
 }
 
 static void help() {
@@ -196,25 +198,28 @@ static void help() {
   printf("\t-width <width>\t\tHorizontal resolution (default 1280)\n");
   printf("\t-height <height>\tVertical resolution (default 720)\n");
   #ifdef HAVE_EMBEDDED
-  printf("\t-rotate <angle>\tRotate display: 0/90/180/270 (default 0)\n");
+  printf("\t-rotate <angle>\t\tRotate display: 0/90/180/270 (default 0)\n");
   #endif
   printf("\t-fps <fps>\t\tSpecify the fps to use (default 60)\n");
   printf("\t-bitrate <bitrate>\tSpecify the bitrate in Kbps\n");
   printf("\t-packetsize <size>\tSpecify the maximum packetsize in bytes\n");
   printf("\t-codec <codec>\t\tSelect used codec: auto/h264/h265/av1 (default auto)\n");
-  printf("\t-hdr\t\tEnable HDR streaming (experimental, requires host and device support)\n");
-  printf("\t-remote <yes/no/auto>\t\t\tEnable optimizations for WAN streaming (default auto)\n");
+  printf("\t-yuv444\t\t\tTry to use yuv444 format\n");
+  printf("\t-remote <yes/no/auto>\tEnable optimizations for WAN streaming (default auto)\n");
+  printf("\t-sdlgp\t\t\tForce to use sdl to drive gamepad\n");
+  printf("\t-swapxyab\t\tSwap X/Y and A/B for gamepad for embedded(not sdl) platform\n");
+  printf("\t-fakegrab\t\tDo not grab keyboard and mouse for embedded(not sdl) platform\n");
   printf("\t-app <app>\t\tName of app to stream\n");
   printf("\t-nosops\t\t\tDon't allow GFE to modify game settings\n");
   printf("\t-localaudio\t\tPlay audio locally on the host computer\n");
-  printf("\t-surround <5.1/7.1>\t\tStream 5.1 or 7.1 surround sound\n");
+  printf("\t-surround <5.1/7.1>\tStream 5.1 or 7.1 surround sound\n");
   printf("\t-keydir <directory>\tLoad encryption keys from directory\n");
   printf("\t-mapping <file>\t\tUse <file> as gamepad mappings configuration file\n");
-  printf("\t-platform <system>\tSpecify system used for audio, video and input: pi/imx/aml/rk/x11/x11_vdpau/sdl/fake (default auto)\n");
+  printf("\t-platform <system>\tSpecify system used for audio, video and input: rk/x11/x11_vaapi/sdl (default auto)\n");
   printf("\t-nounsupported\t\tDon't stream if resolution is not officially supported by the server\n");
   printf("\t-quitappafter\t\tSend quit app request to remote after quitting session\n");
   printf("\t-viewonly\t\tDisable all input processing (view-only mode)\n");
-  printf("\t-nomouseemulation\t\tDisable gamepad mouse emulation support (long pressing Start button)\n");
+  printf("\t-nomouseemulation\tDisable gamepad mouse emulation support (long pressing Start button)\n");
   #if defined(HAVE_SDL) || defined(HAVE_X11)
   printf("\n WM options (SDL and X11 only)\n\n");
   printf("\t-windowed\t\tDisplay screen in a window\n");
@@ -224,7 +229,9 @@ static void help() {
   printf("\t-input <device>\t\tUse <device> as input. Can be used multiple times\n");
   printf("\t-audio <device>\t\tUse <device> as audio output device\n");
   #endif
-  printf("\nUse Ctrl+Alt+Shift+Q or Play+Back+LeftShoulder+RightShoulder to exit streaming session\n\n");
+  printf("\nUse Ctrl+Alt+Shift+Q or Play+Back+LeftShoulder+RightShoulder to exit streaming session\n");
+  printf("\nUse Ctrl+Alt+Shift+Z to exit ungrab keyboard and mouse\n");
+  printf("\nUse Ctrl+Alt+Shift+M to enter fake grab mode(as ungrab keyboard and mouse)\n\n");
   exit(0);
 }
 
@@ -238,7 +245,7 @@ int main(int argc, char* argv[]) {
 int main(int argc, char* argv[]) {
   CONFIGURATION config;
   config_parse(argc, argv, &config);
-
+  
   if (config.action == NULL || strcmp("help", config.action) == 0)
     help();
 
@@ -251,6 +258,7 @@ int main(int argc, char* argv[]) {
       exit(-1);
     }
 
+    loop_create();
     evdev_create(config.inputs[0], NULL, config.debug_level > 0, config.rotate);
     evdev_map(config.inputs[0]);
     exit(0);
@@ -319,22 +327,59 @@ int main(int argc, char* argv[]) {
       exit(-1);
     }
 
+    if (IS_EMBEDDED(system)) {
+      loop_create();
+    }
+
     config.stream.supportedVideoFormats = VIDEO_FORMAT_H264;
     if (config.codec == CODEC_HEVC || (config.codec == CODEC_UNSPECIFIED && platform_prefers_codec(system, CODEC_HEVC))) {
       config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265;
-      if (config.hdr)
-        config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
+      //if (config.hdr)
+      //  config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
     }
     if (config.codec == CODEC_AV1 || (config.codec == CODEC_UNSPECIFIED && platform_prefers_codec(system, CODEC_AV1))) {
       config.stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
-      if (config.hdr)
-        config.stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
+      //if (config.hdr)
+      //  config.stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
     }
 
-    if (config.hdr && !(config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT)) {
-      fprintf(stderr, "HDR streaming requires HEVC or AV1 codec\n");
-      exit(-1);
+    //if (config.hdr && !(config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT)) {
+    //  fprintf(stderr, "HDR streaming requires HEVC or AV1 codec\n");
+    //  exit(-1);
+    //}
+
+    // set yuv444 depend on config
+    if (config.yuv444 && (system == X11_VAAPI || system == X11)) {
+      if (system == X11_VAAPI && isSupportYuv444) {
+        // some encoder dose not support yuv444 when using h264,so try use h265 instead of h264
+        if (config.stream.supportedVideoFormats != VIDEO_FORMAT_H265)
+          config.stream.supportedVideoFormats = VIDEO_FORMAT_H265;
+        config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265_444;
+      }
+      else if (system == X11_VAAPI) {
+        printf("YUV444 is not supported because of platform: %d .\n", (int)system);
+        config.yuv444 = false;
+      }
+      if (system == X11) {
+        config.stream.supportedVideoFormats |= VIDEO_FORMAT_H264_444;
+        if (config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_H265)
+          config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265_444;
+        if (config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_AV1)
+          config.stream.supportedVideoFormats |= VIDEO_FORMAT_AV1_444;
+        if (config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_H265 || 
+            config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_AV1)
+          printf("When using x11 platform with yuv444, codec 'h264' is more recommended!\n");
+      }
     }
+    if (config.yuv444) {
+      // pass var to ffmpeg
+      wantYuv444 = true;
+      if (!config.hdr) {
+        config.stream.colorSpace = COLORSPACE_REC_709;
+        config.stream.colorRange = COLOR_RANGE_FULL;
+      }
+      printf("Try to use yuv444 mode\n");
+    }
 
     #ifdef HAVE_SDL
     if (system == SDL)
@@ -362,6 +407,26 @@ int main(int argc, char* argv[]) {
           mappings = map;
         }
 
+        #ifdef HAVE_SDL
+        int sdl_err = -1;
+        if (config.sdlgp && config.inputsCount <= 0)
+          sdl_err = x11_sdl_init(config.mapping);
+        if (sdl_err < 0) {
+          if (config.inputsCount > 0)
+            printf("Using evdev to drive gamepads because of '-input device' option in command.\n");
+          else if (config.sdlgp)
+            printf("SDL gamepad module start faild.\n");
+          config.sdlgp = false;
+        } else {
+          rumble_handler = sdlinput_rumble;
+          rumble_triggers_handler = sdlinput_rumble_triggers;
+          set_motion_event_state_handler = sdlinput_set_motion_event_state;
+          set_controller_led_handler = sdlinput_set_controller_led;
+        }
+        #endif
+
+        evdev_init_vars(config.fakegrab, config.sdlgp, config.swapxyab, inputAdded);
+
         for (int i=0;i<config.inputsCount;i++) {
           if (config.debug_level > 0)
             printf("Adding input device %s...\n", config.inputs[i]);
@@ -371,7 +436,10 @@ int main(int argc, char* argv[]) {
 
         udev_init(!inputAdded, mappings, config.debug_level > 0, config.rotate);
         evdev_init(config.mouse_emulation);
-        rumble_handler = evdev_rumble;
+
+        if (!config.sdlgp)
+          rumble_handler = evdev_rumble;
+
         #ifdef HAVE_LIBCEC
         cec_init();
         #endif /* HAVE_LIBCEC */
@@ -398,7 +466,8 @@ int main(int argc, char* argv[]) {
     if (config.pin > 0 && config.pin <= 9999) {
       sprintf(pin, "%04d", config.pin);
     } else {
-      sprintf(pin, "%d%d%d%d", (unsigned)random() % 10, (unsigned)random() % 10, (unsigned)random() % 10, (unsigned)random() % 10);
+      srand((unsigned)time(NULL));
+      sprintf(pin, "%04d", (unsigned)rand() % 9999 + 1);
     }
     printf("Please enter the following PIN on the target PC: %s\n", pin);
     fflush(stdout);
@@ -406,6 +475,7 @@ int main(int argc, char* argv[]) {
       fprintf(stderr, "Failed to pair to server: %s\n", gs_error);
     } else {
       printf("Succesfully paired\n");
+      printf("Note: Use Ctrl+Alt+Shift+Q to quit streaming.\n");
     }
   } else if (strcmp("unpair", config.action) == 0) {
     if (gs_unpair(&server) != GS_OK) {
