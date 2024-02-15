--- src/input/x11.c.orig	2023-11-03 06:08:34 UTC
+++ src/input/x11.c
@@ -18,7 +18,6 @@
  */
 
 #include "x11.h"
-#include "keyboard.h"
 
 #include "../loop.h"
 
@@ -31,116 +30,29 @@
 #include <stdlib.h>
 #include <poll.h>
 
-#define ACTION_MODIFIERS (MODIFIER_SHIFT|MODIFIER_ALT|MODIFIER_CTRL)
-#define QUIT_KEY 0x18  /* KEY_Q */
-
 static Display *display;
 static Window window;
 
 static Atom wm_deletemessage;
 
-static int last_x = -1, last_y = -1;
-static int keyboard_modifiers;
-
 static const char data[1] = {0};
 static Cursor cursor;
 static bool grabbed = True;
 
 static int x11_handler(int fd) {
   XEvent event;
-  int button = 0;
-  int motion_x, motion_y;
 
   while (XPending(display)) {
     XNextEvent(display, &event);
     switch (event.type) {
-    case KeyPress:
-    case KeyRelease:
-      if (event.xkey.keycode >= 8 && event.xkey.keycode < (sizeof(keyCodes)/sizeof(keyCodes[0]) + 8)) {
-        if ((keyboard_modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && event.type == KeyRelease) {
-          if (event.xkey.keycode == QUIT_KEY)
-            return LOOP_RETURN;
-          else {
-            grabbed = !grabbed;
-            XDefineCursor(display, window, grabbed ? cursor : 0);
-          }
-        }
-
-        int modifier = 0;
-        switch (event.xkey.keycode) {
-        case 0x32:
-        case 0x3e:
-          modifier = MODIFIER_SHIFT;
-          break;
-        case 0x40:
-        case 0x6c:
-          modifier = MODIFIER_ALT;
-          break;
-        case 0x25:
-        case 0x69:
-          modifier = MODIFIER_CTRL;
-          break;
-        }
-
-        if (modifier != 0) {
-          if (event.type == KeyPress)
-            keyboard_modifiers |= modifier;
-          else
-            keyboard_modifiers &= ~modifier;
-        }
-
-        short code = 0x80 << 8 | keyCodes[event.xkey.keycode - 8];
-        LiSendKeyboardEvent(code, event.type == KeyPress ? KEY_ACTION_DOWN : KEY_ACTION_UP, keyboard_modifiers);
+    case FocusIn:
+    case FocusOut:
+      if (event.type == FocusIn) {
+        grabbed = true;
+      } else {
+        grabbed = false;
       }
-      break;
-    case ButtonPress:
-    case ButtonRelease:
-      switch (event.xbutton.button) {
-      case Button1:
-        button = BUTTON_LEFT;
-        break;
-      case Button2:
-        button = BUTTON_MIDDLE;
-        break;
-      case Button3:
-        button = BUTTON_RIGHT;
-        break;
-      case Button4:
-        LiSendScrollEvent(1);
-        break;
-      case Button5:
-        LiSendScrollEvent(-1);
-        break;
-      case 6:
-        LiSendHScrollEvent(-1);
-        break;
-      case 7:
-        LiSendHScrollEvent(1);
-        break;
-      case 8:
-        button = BUTTON_X1;
-        break;
-      case 9:
-        button = BUTTON_X2;
-        break;
-      }
-
-      if (button != 0)
-        LiSendMouseButtonEvent(event.type==ButtonPress ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
-      break;
-    case MotionNotify:
-      motion_x = event.xmotion.x - last_x;
-      motion_y = event.xmotion.y - last_y;
-      if (abs(motion_x) > 0 || abs(motion_y) > 0) {
-        if (last_x >= 0 && last_y >= 0)
-          LiSendMouseMoveEvent(motion_x, motion_y);
-
-        if (grabbed)
-          XWarpPointer(display, None, window, 0, 0, 0, 0, 640, 360);
-      }
-
-      last_x = grabbed ? 640 : event.xmotion.x;
-      last_y = grabbed ? 360 : event.xmotion.y;
+      XDefineCursor(display, window, grabbed ? cursor : 0);
       break;
     case ClientMessage:
       if (event.xclient.data.l[0] == wm_deletemessage)
