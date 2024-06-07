--- third_party/moonlight-common-c/src/Limelight.h.orig	2024-02-18 00:20:24 UTC
+++ third_party/moonlight-common-c/src/Limelight.h
@@ -229,6 +229,7 @@ typedef struct _DECODE_UNIT {
 #define VIDEO_FORMAT_MASK_H265  0x0F00
 #define VIDEO_FORMAT_MASK_AV1   0xF000
 #define VIDEO_FORMAT_MASK_10BIT 0x2200
+#define VIDEO_FORMAT_MASK_YUV444 0x0020
 
 // If set in the renderer capabilities field, this flag will cause audio/video data to
 // be submitted directly from the receive thread. This should only be specified if the
