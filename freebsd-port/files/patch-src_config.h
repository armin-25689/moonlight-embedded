--- src/config.h.orig	2024-02-20 04:01:31 UTC
+++ src/config.h
@@ -48,9 +48,11 @@ typedef struct _CONFIGURATION {
   bool hdr;
   int pin;
   unsigned short port;
+  bool yuv444;
 } CONFIGURATION, *PCONFIGURATION;
 
 extern bool inputAdded;
 
 bool config_file_parse(char* filename, PCONFIGURATION config);
 void config_parse(int argc, char* argv[], PCONFIGURATION config);
+void config_clear();
