--- src/config.h.orig	2023-11-03 06:08:34 UTC
+++ src/config.h
@@ -54,3 +54,4 @@ void config_parse(int argc, char* argv[], PCONFIGURATI
 
 bool config_file_parse(char* filename, PCONFIGURATION config);
 void config_parse(int argc, char* argv[], PCONFIGURATION config);
+void config_clear();
