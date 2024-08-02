--- src/input/mapping.c.orig	2024-08-01 13:37:02 UTC
+++ src/input/mapping.c
@@ -46,9 +46,13 @@ struct mapping* mapping_parse(char* mapping) {
 
   char* option;
   while ((option = strtok_r(NULL, ",", &strpoint)) != NULL) {
-    char *key = NULL, *orig_value = NULL;
+    char key[255] = {'\0'}, orig_value[255] = {'\0'};
     int ret;
-    if ((ret = sscanf(option, "%m[^:]:%ms", &key, &orig_value)) == 2) {
+    if (strlen(option) > 255) {
+      fprintf(stderr, "Can't map (%s)\n", option);
+      continue;
+    }
+    if ((ret = sscanf(option, "%[^:]:%s", key, orig_value)) == 2) {
       int int_value, direction_value;
       char *value = orig_value;
       char flag = 0;
@@ -156,14 +160,12 @@ struct mapping* mapping_parse(char* mapping) {
         /* CRC is not supported */
       } else
         fprintf(stderr, "Can't map (%s)\n", option);
-    } else if (ret == 0 && option[0] != '\n')
+    } else if (ret == 0 && option[0] != '\n') {
       fprintf(stderr, "Can't map (%s)\n", option);
+    }
 
-    if (key != NULL)
-      free(key);
-
-    if (orig_value != NULL)
-      free(orig_value);
+    memset(key, 0, sizeof(key));
+    memset(orig_value, 0, sizeof(orig_value));
   }
   map->guid[32] = '\0';
   map->name[256] = '\0';
@@ -233,5 +235,5 @@ void mapping_print(struct mapping* map) {
   print_btn("paddle3", map->btn_paddle3);
   print_btn("paddle4", map->btn_paddle4);
   print_btn("touchpad", map->btn_touchpad);
-  printf("platform:Linux\n");
+  printf("platform:FreeBSD\n");
 }
