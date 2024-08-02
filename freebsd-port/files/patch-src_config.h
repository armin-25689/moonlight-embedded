--- src/config.h.orig	2024-08-01 13:37:02 UTC
+++ src/config.h
@@ -48,9 +48,14 @@ typedef struct _CONFIGURATION {
   bool hdr;
   int pin;
   unsigned short port;
+  bool sdlgp;
+  bool swapxyab;
+  bool yuv444;
+  bool fakegrab;
 } CONFIGURATION, *PCONFIGURATION;
 
 extern bool inputAdded;
 
 bool config_file_parse(char* filename, PCONFIGURATION config);
 void config_parse(int argc, char* argv[], PCONFIGURATION config);
+void config_clear();
