--- src/input/evdev.c.orig	2024-08-01 13:37:02 UTC
+++ src/input/evdev.c
@@ -45,6 +45,22 @@
 #endif
 #include <math.h>
 
+#define QUITCODE "quit"
+
+static int keyboardpipefd = -1;
+static const char *quitstate = QUITCODE;
+static const char *grabcode = GRABCODE;
+static const char *ungrabcode = UNGRABCODE;
+
+static bool waitingToSwitchGrabOnModifierUp = false;
+static bool isgrabkeyrelease = false;
+static bool isUseKbdmux = false;
+static bool fakeGrab = false;
+static bool fakeGrabKey = false;
+static bool iskeyboardgrab = false;
+static bool sdlgp = false;
+static bool swapXYAB = false;
+
 #if __BYTE_ORDER == __LITTLE_ENDIAN
 #define int16_to_le(val) val
 #else
@@ -77,6 +93,13 @@ struct input_device {
   int32_t mouseDeltaX, mouseDeltaY, mouseVScroll, mouseHScroll;
   int32_t touchDownX, touchDownY, touchX, touchY;
   #endif
+  int fingersNum;
+  int maxFingersNum;
+  int mtPalm;
+  int mtSlot;
+  bool isDraging;
+  bool isDraged;
+  bool isMoving;
   struct timeval touchDownTime;
   struct timeval btnDownTime;
   short controllerId;
@@ -136,6 +159,8 @@ int evdev_gamepads = 0;
 
 #define ACTION_MODIFIERS (MODIFIER_SHIFT|MODIFIER_ALT|MODIFIER_CTRL)
 #define QUIT_KEY KEY_Q
+#define GRAB_KEY KEY_Z
+#define FAKE_GRAB_KEY KEY_M
 #define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)
 
 static bool (*handler) (struct input_event*, struct input_device*);
@@ -148,6 +173,22 @@ static int evdev_get_map(int* map, int length, int val
   return -1;
 }
 
+static short keystatlist[0xFF];
+static void keyrelease(int keycode) {
+  keystatlist[keycode] = 0;
+}
+static void keypress(int keycode) {
+  keystatlist[keycode] = 1;
+}
+static void freeallkey () {
+  for (int i=0;i<0xFF;i++) {
+    if (keystatlist[i] == 1) {
+      keystatlist[i] = 0;
+      LiSendKeyboardEvent(0x80 << 8 | keyCodes[i], KEY_ACTION_UP, 0);
+    }
+  }
+}
+
 static bool evdev_init_parms(struct input_device *dev, struct input_abs_parms *parms, int code) {
   int abs = evdev_get_map(dev->abs_map, ABS_MAX, code);
 
@@ -352,7 +393,7 @@ static bool evdev_handle_event(struct input_event *ev,
     if (dev->mouseHScroll != 0) {
       LiSendHScrollEvent(dev->mouseHScroll);
       dev->mouseHScroll = 0;
-    }
+    } 
     if (dev->gamepadModified) {
       if (dev->controllerId < 0) {
         for (int i = 0; i < MAX_GAMEPADS; i++) {
@@ -375,6 +416,24 @@ static bool evdev_handle_event(struct input_event *ev,
         LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, dev->buttonFlags, dev->leftTrigger, dev->rightTrigger, dev->leftStickX, dev->leftStickY, dev->rightStickX, dev->rightStickY);
       dev->gamepadModified = false;
     }
+    if (dev->isDraging) {
+      int nowdeltax = dev->touchX - dev->touchDownX;
+      int nowdeltay = dev->touchY - dev->touchDownY;
+      if (nowdeltax * nowdeltax + nowdeltay * nowdeltay >= dev->mtPalm * dev->mtPalm) {
+        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
+        dev->isDraging = false;
+        dev->isDraged = true;
+      } else {
+        struct timeval elapsedTime;
+        timersub(&ev->time, &dev->touchDownTime, &elapsedTime);
+        int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
+        if (holdTimeMs >= TOUCH_RCLICK_TIME) {
+          LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
+          dev->isDraging = false;
+          dev->isDraged = true;
+        }
+      }
+    }
     break;
   case EV_KEY:
     if (ev->code > KEY_MAX)
@@ -407,16 +466,67 @@ static bool evdev_handle_event(struct input_event *ev,
       }
 
       // After the quit key combo is pressed, quit once all keys are raised
-      if ((dev->modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS &&
-          ev->code == QUIT_KEY && ev->value != 0) {
-        waitingToExitOnModifiersUp = true;
+      if ((dev->modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && ev->value != 0) {
+        if (ev->code == QUIT_KEY) {
+          waitingToExitOnModifiersUp = true;
+          return true;
+        } else if (ev->code == GRAB_KEY || ev->code == FAKE_GRAB_KEY) {
+          if (ev->code == GRAB_KEY) {
+            if (fakeGrab) {
+              fakeGrab = false;
+              fakeGrabKey = true;
+            }
+          }
+          else if (ev->code == FAKE_GRAB_KEY) {
+            fakeGrab = !fakeGrab;
+            fakeGrabKey = true;
+          }
+          waitingToSwitchGrabOnModifierUp = true;
+          return true;
+        }
+      }
+      if (waitingToSwitchGrabOnModifierUp) {
+        if ((ev->code == GRAB_KEY && ev->value == 0) || 
+            (ev->code == FAKE_GRAB_KEY && ev->value == 0)) {
+          isgrabkeyrelease = true;
+          if (dev->modifiers != 0)
+            return true;
+        }
+        if (dev->modifiers == 0 && isgrabkeyrelease) {
+          waitingToSwitchGrabOnModifierUp = false;
+          isgrabkeyrelease = false;
+          freeallkey();
+          if (fakeGrabKey && fakeGrab) {
+            grab_window(false);
+          }
+          else if (fakeGrabKey && !fakeGrab) {
+            grab_window(true);
+          }
+          else {
+            grab_window(!iskeyboardgrab);
+          }
+          fakeGrabKey = false;
+          return true;
+        }
         return true;
-      } else if (waitingToExitOnModifiersUp && dev->modifiers == 0)
+      } else if (waitingToExitOnModifiersUp && dev->modifiers == 0) {
+        freeallkey();
+        grab_window(false);
         return false;
+      }
 
+      if (!iskeyboardgrab)
+        break;
+      if (ev->value)
+        keypress(ev->code);
+      else
+        keyrelease(ev->code);
       short code = 0x80 << 8 | keyCodes[ev->code];
       LiSendKeyboardEvent(code, ev->value?KEY_ACTION_DOWN:KEY_ACTION_UP, dev->modifiers);
+
     } else {
+      if (!iskeyboardgrab)
+        break;
       int mouseCode = 0;
       int gamepadCode = 0;
       int index = dev->key_map[ev->code];
@@ -440,36 +550,137 @@ static bool evdev_handle_event(struct input_event *ev,
       case BTN_TOUCH:
         if (ev->value == 1) {
           dev->touchDownTime = ev->time;
+          if (dev->is_mouse && dev->mtSlot == -1) {
+            dev->fingersNum = 1;
+            dev->maxFingersNum = 1;
+          }
         } else {
           if (dev->touchDownX != TOUCH_UP && dev->touchDownY != TOUCH_UP) {
             int deltaX = dev->touchX - dev->touchDownX;
             int deltaY = dev->touchY - dev->touchDownY;
-            if (deltaX * deltaX + deltaY * deltaY < TOUCH_CLICK_RADIUS * TOUCH_CLICK_RADIUS) {
-              struct timeval elapsedTime;
-              timersub(&ev->time, &dev->touchDownTime, &elapsedTime);
-              int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
-              int button = holdTimeMs >= TOUCH_RCLICK_TIME ? BUTTON_RIGHT : BUTTON_LEFT;
+            int nowpalm = dev->is_touchscreen ? TOUCH_CLICK_RADIUS : dev->mtPalm;
+            struct timeval elapsedTime;
+            timersub(&ev->time, &dev->touchDownTime, &elapsedTime);
+            int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
+            int button;
+            if (dev->is_touchscreen && deltaX * deltaX + deltaY * deltaY < nowpalm * nowpalm) {
+              button = holdTimeMs >= TOUCH_RCLICK_TIME ? BUTTON_RIGHT : BUTTON_LEFT;
               LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
               usleep(TOUCH_CLICK_DELAY);
               LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
+            } else if (dev->mtPalm > 0 && !dev->isMoving) {
+              if (holdTimeMs < TOUCH_RCLICK_TIME) {
+                switch (dev->maxFingersNum) {
+                case 1:
+                  button = BUTTON_LEFT;
+                  break;
+                case 2:
+                  button = BUTTON_RIGHT;
+                  break;
+                case 3:
+                  button = BUTTON_MIDDLE;
+                  break;
+                default:
+                  dev->touchDownX = TOUCH_UP;
+                  dev->touchDownY = TOUCH_UP;
+                  dev->fingersNum = 0;
+                  dev->maxFingersNum = 0;
+                  dev->isMoving = false;
+                  dev->mtSlot = -1;
+                  return true;
+                }
+                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
+                usleep(TOUCH_CLICK_DELAY);
+                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
+              }
             }
           }
           dev->touchDownX = TOUCH_UP;
           dev->touchDownY = TOUCH_UP;
+          dev->fingersNum = 0;
+          dev->maxFingersNum = 0;
+          dev->isMoving = false;
+          dev->mtSlot = -1;
         }
         break;
+      case BTN_TOOL_FINGER:
+        if (dev->mtPalm <= 0)
+          break;
+        if (ev->value == 1) {
+          dev->fingersNum = 1;
+        }
+        break;
+      case BTN_TOOL_DOUBLETAP:
+        if (dev->mtPalm <= 0)
+          break;
+        if (ev->value == 1) {
+          if (dev->maxFingersNum < 2)
+            dev->maxFingersNum = 2;
+          dev->fingersNum = 2;
+        }
+        break;
+      case BTN_TOOL_TRIPLETAP:
+        if (dev->mtPalm <= 0)
+          break;
+        if (ev->value == 1) {
+          dev->isDraging = true;
+          if (dev->maxFingersNum < 3)
+            dev->maxFingersNum = 3;
+          dev->fingersNum = 3;
+        } else {
+          if (dev->isDraged) {
+            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
+          }
+          dev->isDraging = false;
+          dev->isDraged = false;
+        }
+        break;
+      case BTN_TOOL_QUADTAP:
+        if (dev->mtPalm <= 0)
+          break;
+        if (ev->value == 1) {
+          if (dev->maxFingersNum < 4)
+            dev->maxFingersNum = 4;
+          dev->fingersNum = 4;
+        }
+        break;
+      case BTN_TOOL_QUINTTAP:
+        if (dev->mtPalm <= 0)
+          break;
+        if (ev->value == 1) {
+          if (dev->maxFingersNum < 5)
+            dev->maxFingersNum = 5;
+          dev->fingersNum = 5;
+        }
+        break;
       default:
         gamepadModified = true;
         if (dev->map == NULL)
           break;
-        else if (index == dev->map->btn_a)
-          gamepadCode = A_FLAG;
-        else if (index == dev->map->btn_x)
-          gamepadCode = X_FLAG;
-        else if (index == dev->map->btn_y)
-          gamepadCode = Y_FLAG;
-        else if (index == dev->map->btn_b)
-          gamepadCode = B_FLAG;
+        else if (index == dev->map->btn_a) {
+          if (!swapXYAB)
+            gamepadCode = A_FLAG;
+          else
+            gamepadCode = B_FLAG;
+        }
+        else if (index == dev->map->btn_x) {
+          if (!swapXYAB)
+            gamepadCode = X_FLAG;
+          else
+            gamepadCode = Y_FLAG;
+        }
+        else if (index == dev->map->btn_y) {
+          if (!swapXYAB)
+            gamepadCode = Y_FLAG;
+          else
+            gamepadCode = X_FLAG;
+        }
+        else if (index == dev->map->btn_b) {
+          if (!swapXYAB)
+            gamepadCode = B_FLAG;
+          else
+            gamepadCode = A_FLAG;
+        }
         else if (index == dev->map->btn_dpup)
           gamepadCode = UP_FLAG;
         else if (index == dev->map->btn_dpdown)
@@ -608,6 +819,55 @@ static bool evdev_handle_event(struct input_event *ev,
       }
       break;
     }
+    if (dev->mtPalm > 0) {
+      int nowdistance = 0;
+      switch (ev->code) {
+      case ABS_MT_SLOT:
+        if (dev->maxFingersNum < ev->value + 1)
+          dev->maxFingersNum = ev->value + 1;
+        dev->mtSlot = ev->value;
+        break;
+      case ABS_MT_POSITION_X:
+        if (dev->mtSlot > 0 || ev->value < 0)
+          break;
+        if (dev->touchDownX == TOUCH_UP) {
+          dev->touchDownX = ev->value;
+          dev->touchX = ev->value;
+        } else {
+          nowdistance = ev->value - dev->touchX;
+          if (!dev->isMoving && abs(ev->value - dev->touchDownX) >= dev->mtPalm)
+            dev->isMoving = true;
+          if (dev->isMoving) {
+            if (dev->fingersNum == 2)
+              LiSendHighResHScrollEvent((short)nowdistance);
+            else
+              dev->mouseDeltaX += nowdistance;
+          }
+          dev->touchX = ev->value;
+        }
+        break;
+      case ABS_MT_POSITION_Y:
+        if (dev->mtSlot > 0 || ev->value < 0)
+          break;
+        if (dev->touchDownY == TOUCH_UP) {
+          dev->touchDownY = ev->value;
+          dev->touchY = ev->value;
+        } else {
+          nowdistance = ev->value - dev->touchY;
+          if (!dev->isMoving && abs(ev->value - dev->touchDownY) >= dev->mtPalm)
+            dev->isMoving = true;
+          if (dev->isMoving) {
+            if (dev->fingersNum == 2)
+              LiSendHighResScrollEvent((short)nowdistance);
+            else
+              dev->mouseDeltaY += nowdistance;
+          }
+          dev->touchY = ev->value;
+        }
+        break;
+      }
+      break;
+    }
 
     if (dev->map == NULL)
       break;
@@ -717,12 +977,14 @@ static bool evdev_handle_mapping_event(struct input_ev
   case EV_ABS:
     hat_index = (ev->code - ABS_HAT0X) / 2;
     if (hat_index >= 0 && hat_index < 4) {
+      dev->hats_state[hat_index][0] = 0;
+      dev->hats_state[hat_index][1] = 0;
       int hat_dir_index = (ev->code - ABS_HAT0X) % 2;
       dev->hats_state[hat_index][hat_dir_index] = ev->value < 0 ? -1 : (ev->value == 0 ? 0 : 1);
     }
     if (currentAbs != NULL) {
       struct input_abs_parms parms;
-      evdev_init_parms(dev, &parms, ev->code);
+      evdev_init_parms(dev, &parms, dev->abs_map[ev->code]);
 
       if (ev->value > parms.avg + parms.range/2) {
         *currentAbs = dev->abs_map[ev->code];
@@ -730,8 +992,9 @@ static bool evdev_handle_mapping_event(struct input_ev
       } else if (ev->value < parms.avg - parms.range/2) {
         *currentAbs = dev->abs_map[ev->code];
         *currentReverse = true;
-      } else if (ev->code == *currentAbs)
+      } else if (dev->abs_map[ev->code] == *currentAbs) {
         return false;
+      }
     } else if (currentHat != NULL) {
       if (hat_index >= 0 && hat_index < 4) {
         *currentHat = hat_index;
@@ -758,8 +1021,10 @@ static int evdev_handle(int fd) {
       struct input_event ev;
       while ((rc = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0) {
         if (rc == LIBEVDEV_READ_STATUS_SYNC)
-          fprintf(stderr, "Error: cannot keep up\n");
+          fprintf(stderr, "Error:%s(%d) cannot keep up\n", libevdev_get_name(devices[i].dev), i);
         else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
+          if (!iskeyboardgrab && ev.type != EV_KEY)
+            break;
           if (!handler(&ev, &devices[i]))
             return LOOP_RETURN;
         }
@@ -775,6 +1040,47 @@ static int evdev_handle(int fd) {
   return LOOP_OK;
 }
 
+void evdev_init_vars(bool isfakegrab, bool issdlgp, bool isswapxyab, bool isinputadded) {
+  fakeGrab = isfakegrab;
+  sdlgp = issdlgp;
+  swapXYAB = isswapxyab;
+  if (swapXYAB)
+
+  if (isinputadded)
+    return;
+
+  const char* tryFirstInput = "/dev/input/event0";
+  const char* trySecondInput = "/dev/input/event1";
+
+  int fdFirst = open(tryFirstInput, O_RDWR|O_NONBLOCK);
+  int fdSecond = open(trySecondInput, O_RDWR|O_NONBLOCK);
+  if (fdFirst <= 0 && fdSecond <= 0) {
+    //Suppose use kbdmux because of default behavior
+    isUseKbdmux = true;
+    return;
+  }
+
+  struct libevdev *evdevFirst = libevdev_new();
+  libevdev_set_fd(evdevFirst, fdFirst);
+  const char* nameFirst = libevdev_get_name(evdevFirst);
+  struct libevdev *evdevSecond = libevdev_new();
+  libevdev_set_fd(evdevSecond, fdSecond);
+  const char* nameSecond = libevdev_get_name(evdevSecond);
+
+  libevdev_free(evdevFirst);
+  libevdev_free(evdevSecond);
+  close(fdFirst);
+  close(fdSecond);
+
+  if (strcmp(nameFirst, "System keyboard multiplexer") == 0 ||
+      strcmp(nameSecond, "System keyboard multiplexer") == 0) {
+    isUseKbdmux = true;
+    return;
+  }
+
+  return;
+}
+
 void evdev_create(const char* device, struct mapping* mappings, bool verbose, int rotate) {
   int fd = open(device, O_RDWR|O_NONBLOCK);
   if (fd <= 0) {
@@ -823,8 +1129,9 @@ void evdev_create(const char* device, struct mapping* 
     mappings = xwc_mapping;
 
   bool is_keyboard = libevdev_has_event_code(evdev, EV_KEY, KEY_Q);
-  bool is_mouse = libevdev_has_event_type(evdev, EV_REL) || libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT);
-  bool is_touchscreen = libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH);
+  bool is_mouse = libevdev_has_event_type(evdev, EV_REL) || 
+                  libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT);
+  bool is_touchscreen = !is_mouse && libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH);
 
   // This classification logic comes from SDL
   bool is_accelerometer =
@@ -851,7 +1158,34 @@ void evdev_create(const char* device, struct mapping* 
      libevdev_has_event_code(evdev, EV_ABS, ABS_WHEEL) ||
      libevdev_has_event_code(evdev, EV_ABS, ABS_GAS) ||
      libevdev_has_event_code(evdev, EV_ABS, ABS_BRAKE));
+  bool is_acpibutton =
+    strcmp(name, "Sleep Button") == 0 ||
+    strcmp(name, "Power Button") == 0;
+  // Just use System keyboard multiplexer for FreeBSD,see kbdcontrol(1) and kbdmux(4)
+  // Trying to grab kbdmux0 and keyboard it's self at the same time results in
+  // the keyboard becoming unresponsive on FreeBSD.
+  bool is_likekeyboard =
+    is_keyboard && isUseKbdmux && strcmp(name, "System keyboard multiplexer") != 0;
+/*
+    (is_keyboard && guid[0] <= 3) ||
+    strcmp(name, "AT keyboard") == 0;
+*/
 
+  // In some cases,acpibutton can be mistaken for a keyboard and freeze the keyboard when tring grab.
+  if (is_acpibutton) {
+    if (verbose)
+      printf("Skip acpibutton: %s\n", name);
+    libevdev_free(evdev);
+    close(fd);
+    return;
+  }
+  // In some cases,Do not grab likekeyboard for avoiding keyboard unresponsive
+  if (is_likekeyboard) {
+    if (verbose)
+      printf("Do NOT grab like-keyboard: %s,version: %d,bustype: %d\n", name, guid[6], guid[0]);
+    is_keyboard = false;
+  }
+
   if (is_accelerometer) {
     if (verbose)
       printf("Ignoring accelerometer: %s\n", name);
@@ -861,12 +1195,22 @@ void evdev_create(const char* device, struct mapping* 
   }
 
   if (is_gamepad) {
-    evdev_gamepads++;
 
+    if (sdlgp) {
+      if (verbose)
+        printf("Ignoring gamepad by evdev,instead by using sdl: %s\n", name);
+      libevdev_free(evdev);
+      close(fd);
+      return;
+    }
+
     if (mappings == NULL) {
       fprintf(stderr, "No mapping available for %s (%s) on %s\n", name, str_guid, device);
+      fprintf(stderr, "Please use 'moonlight map -input %s >> ~/.config/moonlight/gamecontrollerdb.txt' for %s to create mapping\n", device, name);
       mappings = default_mapping;
     }
+
+    evdev_gamepads++;
   } else {
     if (verbose)
       printf("Not mapping %s as a gamepad\n", name);
@@ -900,7 +1244,15 @@ void evdev_create(const char* device, struct mapping* 
   devices[dev].rotate = rotate;
   devices[dev].touchDownX = TOUCH_UP;
   devices[dev].touchDownY = TOUCH_UP;
+  if (is_mouse && (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT) && 
+                   libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_DOUBLETAP) && 
+                   libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH) && 
+                   libevdev_has_event_code(evdev, EV_ABS, ABS_X))) {
+    devices[dev].mtPalm = 25;
+    devices[dev].mtSlot = -1;
+  }
 
+
   int nbuttons = 0;
   /* Count joystick buttons first like SDL does */
   for (int i = BTN_JOYSTICK; i < KEY_MAX; ++i) {
@@ -917,8 +1269,9 @@ void evdev_create(const char* device, struct mapping* 
     /* Skip hats */
     if (i == ABS_HAT0X)
       i = ABS_HAT3Y;
-    else if (libevdev_has_event_code(devices[dev].dev, EV_ABS, i))
+    else if (libevdev_has_event_code(devices[dev].dev, EV_ABS, i)) {
       devices[dev].abs_map[i] = naxes++;
+    }
   }
 
   devices[dev].controllerId = -1;
@@ -939,7 +1292,7 @@ void evdev_create(const char* device, struct mapping* 
       fprintf(stderr, "Mapping for %s (%s) on %s is incorrect\n", name, str_guid, device);
   }
 
-  if (grabbingDevices && (is_keyboard || is_mouse || is_touchscreen)) {
+  if (grabbingDevices && !fakeGrab && (is_keyboard || is_mouse || is_touchscreen)) {
     if (ioctl(fd, EVIOCGRAB, 1) < 0) {
       fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
     }
@@ -1009,6 +1362,7 @@ void evdev_map(char* device) {
   struct libevdev *evdev = libevdev_new();
   libevdev_set_fd(evdev, fd);
   const char* name = libevdev_get_name(evdev);
+  iskeyboardgrab = true;
 
   int16_t guid[8] = {0};
   guid[0] = int16_to_le(libevdev_get_id_bustype(evdev));
@@ -1027,6 +1381,8 @@ void evdev_map(char* device) {
   libevdev_free(evdev);
   close(fd);
 
+  if (ioctl(devices[0].fd, EVIOCGRAB, 1) < 0)
+    fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
   handler = evdev_handle_mapping_event;
 
   evdev_map_abs("Left Stick Right", &(map.abs_leftx), &(map.reverse_leftx));
@@ -1037,25 +1393,27 @@ void evdev_map(char* device) {
   evdev_map_abs("Right Stick Up", &(map.abs_righty), &(map.reverse_righty));
   evdev_map_key("Right Stick Button", &(map.btn_rightstick));
 
-  evdev_map_hatkey("D-Pad Right", &(map.hat_dpright), &(map.hat_dir_dpright), &(map.btn_dpright));
-  evdev_map_hatkey("D-Pad Left", &(map.hat_dpleft), &(map.hat_dir_dpleft), &(map.btn_dpleft));
-  evdev_map_hatkey("D-Pad Up", &(map.hat_dpup), &(map.hat_dir_dpup), &(map.btn_dpup));
-  evdev_map_hatkey("D-Pad Down", &(map.hat_dpdown), &(map.hat_dir_dpdown), &(map.btn_dpdown));
+  evdev_map_hatkey("D-Pad(Hat) Right", &(map.hat_dpright), &(map.hat_dir_dpright), &(map.btn_dpright));
+  evdev_map_hatkey("D-Pad(Hat) Left", &(map.hat_dpleft), &(map.hat_dir_dpleft), &(map.btn_dpleft));
+  evdev_map_hatkey("D-Pad(Hat) Up", &(map.hat_dpup), &(map.hat_dir_dpup), &(map.btn_dpup));
+  evdev_map_hatkey("D-Pad(Hat) Down", &(map.hat_dpdown), &(map.hat_dir_dpdown), &(map.btn_dpdown));
 
   evdev_map_key("Button X (1)", &(map.btn_x));
   evdev_map_key("Button A (2)", &(map.btn_a));
   evdev_map_key("Button B (3)", &(map.btn_b));
   evdev_map_key("Button Y (4)", &(map.btn_y));
-  evdev_map_key("Back Button", &(map.btn_back));
+  evdev_map_key("Back(Select) Button", &(map.btn_back));
   evdev_map_key("Start Button", &(map.btn_start));
-  evdev_map_key("Special Button", &(map.btn_guide));
+  evdev_map_key("Special(Home) Button", &(map.btn_guide));
 
   bool ignored;
   evdev_map_abskey("Left Trigger", &(map.abs_lefttrigger), &(map.btn_lefttrigger), &ignored);
   evdev_map_abskey("Right Trigger", &(map.abs_righttrigger), &(map.btn_righttrigger), &ignored);
 
-  evdev_map_key("Left Bumper", &(map.btn_leftshoulder));
-  evdev_map_key("Right Bumper", &(map.btn_rightshoulder));
+  evdev_map_key("Left Bumper(Shoulder)", &(map.btn_leftshoulder));
+  evdev_map_key("Right Bumper(Shoulder)", &(map.btn_rightshoulder));
+  if (ioctl(devices[0].fd, EVIOCGRAB, 0) < 0)
+    fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
   mapping_print(&map);
 }
 
@@ -1065,11 +1423,7 @@ void evdev_start() {
   // code looks for. For this reason, we wait to grab until
   // we're ready to take input events. Ctrl+C works up until
   // this point.
-  for (int i = 0; i < numDevices; i++) {
-    if ((devices[i].is_keyboard || devices[i].is_mouse || devices[i].is_touchscreen) && ioctl(devices[i].fd, EVIOCGRAB, 1) < 0) {
-      fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
-    }
-  }
+  grab_window(true);
 
   // Any new input devices detected after this point will be grabbed immediately
   grabbingDevices = true;
@@ -1123,3 +1477,446 @@ void evdev_rumble(unsigned short controller_id, unsign
   write(device->fd, (const void*) &event, sizeof(event));
   device->haptic_effect_id = effect.id;
 }
+
+void fake_grab_window(bool grabstat) {
+  freeallkey();
+  evdev_drain();
+#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
+  write(keyboardpipefd, !grabstat ? &ungrabcode : &grabcode, sizeof(char *));
+#endif
+  iskeyboardgrab = grabstat;
+}
+
+void grab_window(bool grabstat) {
+  int grabnum;
+
+  evdev_drain();
+
+  if (fakeGrab && fakeGrabKey) {
+    iskeyboardgrab = true;
+    grabnum = 0;
+    goto grab;
+  }
+  else if (!fakeGrab && fakeGrabKey) {
+    iskeyboardgrab = true;
+#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
+    write(keyboardpipefd, &grabcode, sizeof(char *));
+#endif
+    grabnum = 1;
+    goto grab;
+  }
+  else if (fakeGrab && !fakeGrabKey) {
+    iskeyboardgrab = true;
+#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
+    write(keyboardpipefd, &grabcode, sizeof(char *));
+#endif
+    return;
+  }
+
+  if (grabstat != iskeyboardgrab) {
+    if (!grabstat) {
+      grabnum = 0;
+    } else {
+      grabnum = 1;
+    }
+    iskeyboardgrab = grabstat;
+
+#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
+    write(keyboardpipefd, !grabstat ? &ungrabcode : &grabcode, sizeof(char *));
+#endif
+    goto grab;
+  }
+
+grab:
+  for (int i = 0; i < numDevices; i++) {
+    if (devices[i].is_keyboard || devices[i].is_mouse || devices[i].is_touchscreen) {
+      if (ioctl(devices[i].fd, EVIOCGRAB, grabnum) < 0)
+        fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
+    }
+  }
+}
+
+void evdev_trans_op_fd(int pipefd) {
+  keyboardpipefd = pipefd;
+}
+
+#ifdef HAVE_SDL
+
+#include <SDL.h>
+#include <SDL_thread.h>
+
+#define SDL_NOTHING 0
+#define SDL_QUIT_APPLICATION 1
+#define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)
+
+extern int sdl_gamepads;
+
+static SDL_Thread *thread = NULL;
+
+static const int SDL_TO_LI_BUTTON_MAP[] = {
+  A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
+  BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
+  LS_CLK_FLAG, RS_CLK_FLAG,
+  LB_FLAG, RB_FLAG,
+  UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
+  MISC_FLAG,
+  PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
+  TOUCHPAD_FLAG,
+};
+
+typedef struct _GAMEPAD_STATE {
+  unsigned char leftTrigger, rightTrigger;
+  short leftStickX, leftStickY;
+  short rightStickX, rightStickY;
+  int buttons;
+  SDL_JoystickID sdl_id;
+  SDL_GameController* controller;
+#if !SDL_VERSION_ATLEAST(2, 0, 9)
+  SDL_Haptic* haptic;
+  int haptic_effect_id;
+#endif
+  short id;
+  bool initialized;
+} GAMEPAD_STATE, *PGAMEPAD_STATE;
+
+static GAMEPAD_STATE gamepads[MAX_GAMEPADS];
+
+static int activeGamepadMask = 0;
+
+static void send_controller_arrival_sdl(PGAMEPAD_STATE state) {
+#if SDL_VERSION_ATLEAST(2, 0, 18)
+  unsigned int supportedButtonFlags = 0;
+  unsigned short capabilities = 0;
+  unsigned char type = LI_CTYPE_UNKNOWN;
+
+  for (int i = 0; i < SDL_arraysize(SDL_TO_LI_BUTTON_MAP); i++) {
+    if (SDL_GameControllerHasButton(state->controller, (SDL_GameControllerButton)i)) {
+        supportedButtonFlags |= SDL_TO_LI_BUTTON_MAP[i];
+    }
+  }
+
+  if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
+      SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS)
+    capabilities |= LI_CCAP_ANALOG_TRIGGERS;
+  if (SDL_GameControllerHasRumble(state->controller))
+    capabilities |= LI_CCAP_RUMBLE;
+  if (SDL_GameControllerHasRumbleTriggers(state->controller))
+    capabilities |= LI_CCAP_TRIGGER_RUMBLE;
+  if (SDL_GameControllerGetNumTouchpads(state->controller) > 0)
+    capabilities |= LI_CCAP_TOUCHPAD;
+  if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL))
+    capabilities |= LI_CCAP_ACCEL;
+  if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO))
+    capabilities |= LI_CCAP_GYRO;
+  if (SDL_GameControllerHasLED(state->controller))
+    capabilities |= LI_CCAP_RGB_LED;
+
+  switch (SDL_GameControllerGetType(state->controller)) {
+  case SDL_CONTROLLER_TYPE_XBOX360:
+  case SDL_CONTROLLER_TYPE_XBOXONE:
+    type = LI_CTYPE_XBOX;
+    break;
+  case SDL_CONTROLLER_TYPE_PS3:
+  case SDL_CONTROLLER_TYPE_PS4:
+  case SDL_CONTROLLER_TYPE_PS5:
+    type = LI_CTYPE_PS;
+    break;
+  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
+#if SDL_VERSION_ATLEAST(2, 24, 0)
+  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
+  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
+  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
+#endif
+    type = LI_CTYPE_NINTENDO;
+    break;
+  }
+
+  LiSendControllerArrivalEvent(state->id, activeGamepadMask, type, supportedButtonFlags, capabilities);
+#endif
+}
+
+static PGAMEPAD_STATE get_gamepad(SDL_JoystickID sdl_id, bool add) {
+  // See if a gamepad already exists
+  for (int i = 0;i<MAX_GAMEPADS;i++) {
+    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id)
+      return &gamepads[i];
+  }
+
+  if (!add)
+    return NULL;
+
+  for (int i = 0;i<MAX_GAMEPADS;i++) {
+    if (!gamepads[i].initialized) {
+      gamepads[i].sdl_id = sdl_id;
+      gamepads[i].id = i;
+      gamepads[i].initialized = true;
+
+      activeGamepadMask |= (1 << i);
+
+      return &gamepads[i];
+    }
+  }
+
+  return &gamepads[0];
+}
+
+static int x11_sdlinput_handle_event(SDL_Event* event);
+static void add_gamepad(int joystick_index) {
+  SDL_GameController* controller = SDL_GameControllerOpen(joystick_index);
+  if (!controller) {
+    fprintf(stderr, "Could not open gamecontroller %i: %s\n", joystick_index, SDL_GetError());
+    return;
+  }
+
+  SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
+  SDL_JoystickID joystick_id = SDL_JoystickInstanceID(joystick);
+
+  // Check if we have already set up a state for this gamepad
+  PGAMEPAD_STATE state = get_gamepad(joystick_id, false);
+  if (state) {
+    // This was probably a gamepad added during initialization, so we've already
+    // got state set up. However, we still need to inform the host about it, since
+    // we couldn't do that during initialization (since we weren't connected yet).
+    send_controller_arrival_sdl(state);
+
+    SDL_GameControllerClose(controller);
+    return;
+  }
+
+  // Create a new gamepad state
+  state = get_gamepad(joystick_id, true);
+  state->controller = controller;
+
+#if !SDL_VERSION_ATLEAST(2, 0, 9)
+  state->haptic = SDL_HapticOpenFromJoystick(joystick);
+  if (state->haptic && (SDL_HapticQuery(state->haptic) & SDL_HAPTIC_LEFTRIGHT) == 0) {
+    SDL_HapticClose(state->haptic);
+    state->haptic = NULL;
+  }
+  state->haptic_effect_id = -1;
+#endif
+
+  // Send the controller arrival event to the host
+  send_controller_arrival_sdl(state);
+
+  sdl_gamepads++;
+}
+
+static void remove_gamepad(SDL_JoystickID sdl_id) {
+  for (int i = 0;i<MAX_GAMEPADS;i++) {
+    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id) {
+#if !SDL_VERSION_ATLEAST(2, 0, 9)
+      if (gamepads[i].haptic_effect_id >= 0) {
+        SDL_HapticDestroyEffect(gamepads[i].haptic, gamepads[i].haptic_effect_id);
+      }
+
+      if (gamepads[i].haptic) {
+        SDL_HapticClose(gamepads[i].haptic);
+      }
+#endif
+
+      SDL_GameControllerClose(gamepads[i].controller);
+
+      // This will cause disconnection of the virtual controller on the host PC
+      activeGamepadMask &= ~(1 << i);
+      LiSendMultiControllerEvent(i, activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);
+
+      memset(&gamepads[i], 0, sizeof(*gamepads));
+      sdl_gamepads--;
+      break;
+    }
+  }
+}
+
+static void sdlinput_init(char* mappings) {
+  memset(gamepads, 0, sizeof(gamepads));
+
+  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
+#if !SDL_VERSION_ATLEAST(2, 0, 9)
+  SDL_InitSubSystem(SDL_INIT_HAPTIC);
+#endif
+  SDL_GameControllerAddMappingsFromFile(mappings);
+}
+
+static int x11_sdlinput_handle_event(SDL_Event* event) {
+  unsigned char touchEventType;
+  PGAMEPAD_STATE gamepad;
+  switch (event->type) {
+  case SDL_CONTROLLERAXISMOTION:
+    gamepad = get_gamepad(event->caxis.which, false);
+    if (!gamepad)
+      return SDL_NOTHING;
+    switch (event->caxis.axis) {
+    case SDL_CONTROLLER_AXIS_LEFTX:
+      gamepad->leftStickX = event->caxis.value;
+      break;
+    case SDL_CONTROLLER_AXIS_LEFTY:
+      gamepad->leftStickY = -SDL_max(event->caxis.value, (short)-32767);
+      break;
+    case SDL_CONTROLLER_AXIS_RIGHTX:
+      gamepad->rightStickX = event->caxis.value;
+      break;
+    case SDL_CONTROLLER_AXIS_RIGHTY:
+      gamepad->rightStickY = -SDL_max(event->caxis.value, (short)-32767);
+      break;
+    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
+      gamepad->leftTrigger = (unsigned char)(event->caxis.value * 255UL / 32767);
+      break;
+    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
+      gamepad->rightTrigger = (unsigned char)(event->caxis.value * 255UL / 32767);
+      break;
+    default:
+      return SDL_NOTHING;
+    }
+    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
+    break;
+  case SDL_CONTROLLERBUTTONDOWN:
+  case SDL_CONTROLLERBUTTONUP:
+    gamepad = get_gamepad(event->cbutton.which, false);
+    if (!gamepad)
+      return SDL_NOTHING;
+    if (event->cbutton.button >= SDL_arraysize(SDL_TO_LI_BUTTON_MAP))
+      return SDL_NOTHING;
+
+    int now_buttons = SDL_TO_LI_BUTTON_MAP[event->cbutton.button];
+    if (swapXYAB) {
+      switch (now_buttons) {
+      case A_FLAG:
+        now_buttons = B_FLAG;
+        break;
+      case B_FLAG:
+        now_buttons = A_FLAG;
+        break;
+      case X_FLAG:
+        now_buttons = Y_FLAG;
+        break;
+      case Y_FLAG:
+        now_buttons = X_FLAG;
+        break;
+      }
+    }
+    if (event->type == SDL_CONTROLLERBUTTONDOWN)
+      gamepad->buttons |= now_buttons;
+    else
+      gamepad->buttons &= ~now_buttons;
+
+    if ((gamepad->buttons & QUIT_BUTTONS) == QUIT_BUTTONS)
+      return SDL_QUIT_APPLICATION;
+
+    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
+    break;
+  case SDL_CONTROLLERDEVICEADDED:
+    add_gamepad(event->cdevice.which);
+    break;
+  case SDL_CONTROLLERDEVICEREMOVED:
+    remove_gamepad(event->cdevice.which);
+    break;
+#if SDL_VERSION_ATLEAST(2, 0, 14)
+  case SDL_CONTROLLERSENSORUPDATE:
+    gamepad = get_gamepad(event->csensor.which, false);
+    if (!gamepad)
+      return SDL_NOTHING;
+    switch (event->csensor.sensor) {
+    case SDL_SENSOR_ACCEL:
+      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_ACCEL, event->csensor.data[0], event->csensor.data[1], event->csensor.data[2]);
+      break;
+    case SDL_SENSOR_GYRO:
+      // Convert rad/s to deg/s
+      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_GYRO,
+                                  event->csensor.data[0] * 57.2957795f,
+                                  event->csensor.data[1] * 57.2957795f,
+                                  event->csensor.data[2] * 57.2957795f);
+      break;
+    }
+    break;
+  case SDL_CONTROLLERTOUCHPADDOWN:
+  case SDL_CONTROLLERTOUCHPADUP:
+  case SDL_CONTROLLERTOUCHPADMOTION:
+    gamepad = get_gamepad(event->ctouchpad.which, false);
+    if (!gamepad)
+      return SDL_NOTHING;
+    switch (event->type) {
+    case SDL_CONTROLLERTOUCHPADDOWN:
+      touchEventType = LI_TOUCH_EVENT_DOWN;
+      break;
+    case SDL_CONTROLLERTOUCHPADUP:
+      touchEventType = LI_TOUCH_EVENT_UP;
+      break;
+    case SDL_CONTROLLERTOUCHPADMOTION:
+      touchEventType = LI_TOUCH_EVENT_MOVE;
+      break;
+    default:
+      return SDL_NOTHING;
+    }
+    LiSendControllerTouchEvent(gamepad->id, touchEventType, event->ctouchpad.finger,
+                               event->ctouchpad.x, event->ctouchpad.y, event->ctouchpad.pressure);
+    break;
+#endif
+  }
+
+  return SDL_NOTHING;
+}
+
+static void x11_sdl_stop () {
+  for (int i=0;i<MAX_GAMEPADS;i++) {
+    if (gamepads[i].initialized) {
+      remove_gamepad(gamepads[i].sdl_id);
+    }
+  }
+
+  SDL_Quit();
+}
+
+static int x11_sdl_event_handle(void *pointer) {
+  SDL_Event event;
+  bool done = false;
+
+  while (!done && SDL_WaitEvent(&event)) {
+    switch (x11_sdlinput_handle_event(&event)) {
+    case SDL_QUIT_APPLICATION:
+#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
+      write(keyboardpipefd, &quitstate, sizeof(char *));
+#endif
+      done = true;
+      break;
+    default:
+      if (event.type == SDL_QUIT)
+        done = true;
+      break;
+    }
+  }
+
+  x11_sdl_stop();
+  return 0;
+}
+
+int x11_sdl_init (char* mappings) {
+  sdl_gamepads = 0;
+  sdlinput_init(mappings);
+
+  // Add game controllers here to ensure an accurate count
+  // goes to the host when starting a new session.
+  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
+    if (SDL_IsGameController(i)) {
+      add_gamepad(i);
+    }
+    else {
+      char guidStr[33];
+      SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
+                                guidStr, sizeof(guidStr));
+      const char* name = SDL_JoystickNameForIndex(i);
+      fprintf(stderr, "No mapping available for %s (%s).Use 'x11/antimicrox' or others to create mapping to ~/.config/moonlight/gamecontrollerdb.txt\n", name, guidStr);
+    }
+  }
+
+  thread = SDL_CreateThread(x11_sdl_event_handle, "sdl_event_handle", NULL);
+  if (thread == NULL) {
+    fprintf(stderr, "Can't create sdl poll event thread.\n");
+    SDL_Quit();
+    return -1;
+  }
+
+  return 0;
+}
+
+#endif /* HAVE_SDL */
