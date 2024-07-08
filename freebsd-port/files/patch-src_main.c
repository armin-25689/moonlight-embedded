--- src/main.c.orig	2024-02-20 04:01:31 UTC
+++ src/main.c
@@ -42,6 +42,7 @@
 #include <client.h>
 #include <discover.h>
 
+#include <time.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <stdbool.h>
@@ -52,7 +53,8 @@
 #include <netinet/in.h>
 #include <netdb.h>
 #include <arpa/inet.h>
-#include <openssl/rand.h>
+ 
+bool isNoSdl = false;
 
 static void applist(PSERVER_DATA server) {
   PAPP_LIST list = NULL;
@@ -166,6 +168,7 @@ static void stream(PSERVER_DATA server, PCONFIGURATION
   }
 
   platform_stop(system);
+  config_clear();
 }
 
 static void help() {
@@ -202,7 +205,7 @@ static void help() {
   printf("\t-bitrate <bitrate>\tSpecify the bitrate in Kbps\n");
   printf("\t-packetsize <size>\tSpecify the maximum packetsize in bytes\n");
   printf("\t-codec <codec>\t\tSelect used codec: auto/h264/h265/av1 (default auto)\n");
-  printf("\t-hdr\t\tEnable HDR streaming (experimental, requires host and device support)\n");
+  printf("\t-yuv444\t\t\tTry to use yuv444 format\n");
   printf("\t-remote <yes/no/auto>\t\t\tEnable optimizations for WAN streaming (default auto)\n");
   printf("\t-app <app>\t\tName of app to stream\n");
   printf("\t-nosops\t\t\tDon't allow GFE to modify game settings\n");
@@ -210,7 +213,7 @@ static void help() {
   printf("\t-surround <5.1/7.1>\t\tStream 5.1 or 7.1 surround sound\n");
   printf("\t-keydir <directory>\tLoad encryption keys from directory\n");
   printf("\t-mapping <file>\t\tUse <file> as gamepad mappings configuration file\n");
-  printf("\t-platform <system>\tSpecify system used for audio, video and input: pi/imx/aml/rk/x11/x11_vdpau/sdl/fake (default auto)\n");
+  printf("\t-platform <system>\tSpecify system used for audio, video and input: rk/x11/x11_vaapi/sdl (default auto)\n");
   printf("\t-nounsupported\t\tDon't stream if resolution is not officially supported by the server\n");
   printf("\t-quitappafter\t\tSend quit app request to remote after quitting session\n");
   printf("\t-viewonly\t\tDisable all input processing (view-only mode)\n");
@@ -224,7 +227,9 @@ static void help() {
   printf("\t-input <device>\t\tUse <device> as input. Can be used multiple times\n");
   printf("\t-audio <device>\t\tUse <device> as audio output device\n");
   #endif
-  printf("\nUse Ctrl+Alt+Shift+Q or Play+Back+LeftShoulder+RightShoulder to exit streaming session\n\n");
+  printf("\nUse Ctrl+Alt+Shift+Q or Play+Back+LeftShoulder+RightShoulder to exit streaming session\n");
+  printf("\nUse Ctrl+Alt+Shift+Z to exit ungrab keyboard and mouse\n");
+  printf("\nUse Ctrl+Alt+Shift+M to enter fake grab mode(as ungrab keyboard and mouse)\n\n");
   exit(0);
 }
 
@@ -238,7 +243,10 @@ int main(int argc, char* argv[]) {
 int main(int argc, char* argv[]) {
   CONFIGURATION config;
   config_parse(argc, argv, &config);
-
+  #ifndef HAVE_SDL
+  isNoSdl = true;
+  #endif
+  
   if (config.action == NULL || strcmp("help", config.action) == 0)
     help();
 
@@ -251,6 +259,7 @@ int main(int argc, char* argv[]) {
       exit(-1);
     }
 
+    isNoSdl = true;
     evdev_create(config.inputs[0], NULL, config.debug_level > 0, config.rotate);
     evdev_map(config.inputs[0]);
     exit(0);
@@ -322,19 +331,31 @@ int main(int argc, char* argv[]) {
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
+    if (config.yuv444 && (system == X11_VAAPI || system == X11_VDPAU || system == X11)) {
+      if (config.stream.supportedVideoFormats == VIDEO_FORMAT_H264 && (system == X11_VAAPI || system == X11_VDPAU)) {
+	// some encoder dose not support yuv444 when using h264,so try use h265 instead of h264
+	config.stream.supportedVideoFormats |= VIDEO_FORMAT_H265;
+      }
+      config.stream.supportedVideoFormats |= VIDEO_FORMAT_MASK_YUV444;
+      printf("Try to use yuv444 mode\n");
     }
+    else if (config.yuv444)
+      printf("YUV444 is not supported because of platform: %d .\n", (int)system);
 
     #ifdef HAVE_SDL
     if (system == SDL)
@@ -362,16 +383,39 @@ int main(int argc, char* argv[]) {
           mappings = map;
         }
 
+        bool storeIsNoSdl = isNoSdl;
+        if (config.inputsCount <= 0)
+          is_use_kbdmux(config.fakegrab);
+        // Use evdev to drive gamepad listed in command
+        isNoSdl = true;
         for (int i=0;i<config.inputsCount;i++) {
           if (config.debug_level > 0)
             printf("Adding input device %s...\n", config.inputs[i]);
 
           evdev_create(config.inputs[i], mappings, config.debug_level > 0, config.rotate);
         }
+        isNoSdl = storeIsNoSdl;
 
         udev_init(!inputAdded, mappings, config.debug_level > 0, config.rotate);
         evdev_init(config.mouse_emulation);
+        #ifdef HAVE_SDL
+	if (config.inputsCount > 0 || x11_sdl_init(config.mapping) != 0) {
+          if (config.inputsCount > 0)
+            printf("Using evdev to drive gamepads because of '-input device' option in command.\n");
+          else
+            printf("SDL module start faild,please using gamepads by '-input gamepad(/dev/input/eventx) -input keyboard ...' option in command.Or using '-platform sdl' instead.\n");
+          isNoSdl = true;
+          rumble_handler = evdev_rumble;
+        } else {
+          rumble_handler = sdlinput_rumble;
+          rumble_triggers_handler = sdlinput_rumble_triggers;
+          set_motion_event_state_handler = sdlinput_set_motion_event_state;
+          set_controller_led_handler = sdlinput_set_controller_led;
+        }
+        #else
         rumble_handler = evdev_rumble;
+        #endif
+
         #ifdef HAVE_LIBCEC
         cec_init();
         #endif /* HAVE_LIBCEC */
@@ -398,7 +442,8 @@ int main(int argc, char* argv[]) {
     if (config.pin > 0 && config.pin <= 9999) {
       sprintf(pin, "%04d", config.pin);
     } else {
-      sprintf(pin, "%d%d%d%d", (unsigned)random() % 10, (unsigned)random() % 10, (unsigned)random() % 10, (unsigned)random() % 10);
+      srand((unsigned)time(NULL));
+      sprintf(pin, "%04d", (unsigned)rand() % 9999 + 1);
     }
     printf("Please enter the following PIN on the target PC: %s\n", pin);
     fflush(stdout);
@@ -406,6 +451,7 @@ int main(int argc, char* argv[]) {
       fprintf(stderr, "Failed to pair to server: %s\n", gs_error);
     } else {
       printf("Succesfully paired\n");
+      printf("Note: Use Ctrl+Alt+Shift+Q to quit streaming.\n");
     }
   } else if (strcmp("unpair", config.action) == 0) {
     if (gs_unpair(&server) != GS_OK) {
