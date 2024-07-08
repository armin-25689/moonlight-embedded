--- src/config.c.orig	2024-02-20 04:01:31 UTC
+++ src/config.c
@@ -37,12 +37,14 @@
 #define USER_PATHS "."
 #define DEFAULT_CONFIG_DIR "/.config"
 #define DEFAULT_CACHE_DIR "/.cache"
+#define MAX_FILE_LINES 255
 
 #define write_config_string(fd, key, value) fprintf(fd, "%s = %s\n", key, value)
 #define write_config_int(fd, key, value) fprintf(fd, "%s = %d\n", key, value)
 #define write_config_bool(fd, key, value) fprintf(fd, "%s = %s\n", key, value ? "true":"false")
 
 bool inputAdded = false;
+char *fileConfigs[MAX_FILE_LINES] = {0};
 
 static struct option long_options[] = {
   {"720", no_argument, NULL, 'a'},
@@ -50,6 +52,7 @@ static struct option long_options[] = {
   {"4k", no_argument, NULL, '0'},
   {"width", required_argument, NULL, 'c'},
   {"height", required_argument, NULL, 'd'},
+  {"yuv444", no_argument, NULL, 'f'},
   {"bitrate", required_argument, NULL, 'g'},
   {"packetsize", required_argument, NULL, 'h'},
   {"app", required_argument, NULL, 'i'},
@@ -66,6 +69,8 @@ static struct option long_options[] = {
   {"windowed", no_argument, NULL, 't'},
   {"surround", required_argument, NULL, 'u'},
   {"fps", required_argument, NULL, 'v'},
+  {"fakegrab", no_argument, NULL, 'w'},
+  {"nograb", no_argument, NULL, 'W'},
   {"codec", required_argument, NULL, 'x'},
   {"nounsupported", no_argument, NULL, 'y'},
   {"quitappafter", no_argument, NULL, '1'},
@@ -151,6 +156,9 @@ static void parse_argument(int c, char* value, PCONFIG
   case 'd':
     config->stream.height = atoi(value);
     break;
+  case 'f':
+    config->yuv444 = true;
+    break;
   case 'g':
     config->stream.bitrate = atoi(value);
     break;
@@ -220,6 +228,10 @@ static void parse_argument(int c, char* value, PCONFIG
   case 'v':
     config->stream.fps = atoi(value);
     break;
+  case 'w':
+  case 'W':
+    config->fakegrab = true;
+    break;
   case 'x':
     if (strcasecmp(value, "auto") == 0)
       config->codec = CODEC_UNSPECIFIED;
@@ -281,19 +293,34 @@ bool config_file_parse(char* filename, PCONFIGURATION 
 
   char *line = NULL;
   size_t len = 0;
+  int lines = 0;
+  int pairline = 0;
+  int keylen = 0, valuelen = 0;
 
   while (getline(&line, &len, fd) != -1) {
-    char *key = NULL, *value = NULL;
-    if (sscanf(line, "%ms = %m[^\n]", &key, &value) == 2) {
+    lines++;
+    if (lines > (int)(MAX_FILE_LINES / 2) || len > 255) {
+      fprintf(stderr, "Can't read config when line number > %d or length > 255: %s\n", MAX_FILE_LINES / 2, filename);
+      break;
+    }
+    char key[255] = {'\0'}, value[255] = {'\0'};
+    if (sscanf(line, "%s = %[^\n]", key, value) == 2) {
+      keylen = strlen(key);
+      valuelen = strlen(value);
+      pairline = lines * 2;
+      fileConfigs[pairline - 1] = (char *)malloc(1 + keylen * sizeof(char));
+      fileConfigs[pairline] = (char *)malloc(1 + valuelen * sizeof(char));
+      memcpy(fileConfigs[pairline - 1], key, keylen + 1);
+      memcpy(fileConfigs[pairline], value, valuelen + 1);
       if (strcmp(key, "address") == 0) {
-        config->address = value;
+        config->address = fileConfigs[pairline];
       } else if (strcmp(key, "sops") == 0) {
         config->sops = strcmp("true", value) == 0;
       } else {
         for (int i=0;long_options[i].name != NULL;i++) {
           if (strcmp(long_options[i].name, key) == 0) {
             if (long_options[i].has_arg == required_argument)
-              parse_argument(long_options[i].val, value, config);
+              parse_argument(long_options[i].val, fileConfigs[pairline], config);
             else if (strcmp("true", value) == 0)
               parse_argument(long_options[i].val, NULL, config);
           }
@@ -387,7 +414,7 @@ void config_parse(int argc, char* argv[], PCONFIGURATI
   config->mapping = get_path("gamecontrollerdb.txt", getenv("XDG_DATA_DIRS"));
   config->key_dir[0] = 0;
 
-  char* config_file = get_path("moonlight.conf", "/etc");
+  char* config_file = get_path("moonlight.conf", "/usr/local/etc");
   if (config_file)
     config_file_parse(config_file, config);
 
@@ -438,5 +465,14 @@ void config_parse(int argc, char* argv[], PCONFIGURATI
     } else /* if (config->stream.width * config->stream.height <= 3840 * 2160) */ {
       config->stream.bitrate = (int)(40000 * (config->stream.fps / 30.0));
     }
+    if (config->yuv444)
+      config->stream.bitrate = config->stream.bitrate * 2;
+  }
+}
+
+void config_clear() {
+  for (int i = 0; i < MAX_FILE_LINES; i++) {
+    if (fileConfigs[i] != 0)
+      free(fileConfigs[i]);
   }
 }
