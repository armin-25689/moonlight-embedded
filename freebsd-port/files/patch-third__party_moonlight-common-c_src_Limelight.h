--- third_party/moonlight-common-c/src/Limelight.h.orig	2024-08-01 13:37:02 UTC
+++ third_party/moonlight-common-c/src/Limelight.h
@@ -219,16 +219,20 @@ typedef struct _DECODE_UNIT {
 // Passed in StreamConfiguration.supportedVideoFormats to specify supported codecs
 // and to DecoderRendererSetup() to specify selected codec.
 #define VIDEO_FORMAT_H264        0x0001 // H.264 High Profile
+#define VIDEO_FORMAT_H264_444    0x0004 // H.264 High444 Profile
 #define VIDEO_FORMAT_H265        0x0100 // HEVC Main Profile
-#define VIDEO_FORMAT_H265_MAIN10 0x0200 // HEVC Main10 Profile
+#define VIDEO_FORMAT_H265_MAIN10 0x0200 // HEVC Main10 Profil
+#define VIDEO_FORMAT_H265_444    0x0400 // HEVC REXT Profile
 #define VIDEO_FORMAT_AV1_MAIN8   0x1000 // AV1 Main 8-bit profile
 #define VIDEO_FORMAT_AV1_MAIN10  0x2000 // AV1 Main 10-bit profile
+#define VIDEO_FORMAT_AV1_444     0x4000 // AV1 444 profile,not supported
 
 // Masks for clients to use to match video codecs without profile-specific details.
-#define VIDEO_FORMAT_MASK_H264  0x000F
-#define VIDEO_FORMAT_MASK_H265  0x0F00
-#define VIDEO_FORMAT_MASK_AV1   0xF000
-#define VIDEO_FORMAT_MASK_10BIT 0x2200
+#define VIDEO_FORMAT_MASK_H264   0x000F
+#define VIDEO_FORMAT_MASK_H265   0x0F00
+#define VIDEO_FORMAT_MASK_AV1    0xF000
+#define VIDEO_FORMAT_MASK_10BIT  0x2200
+#define VIDEO_FORMAT_MASK_YUV444 0x4404
 
 // If set in the renderer capabilities field, this flag will cause audio/video data to
 // be submitted directly from the receive thread. This should only be specified if the
@@ -486,16 +490,20 @@ void LiInitializeConnectionCallbacks(PCONNECTION_LISTE
 
 // ServerCodecModeSupport values
 #define SCM_H264        0x00001
+#define SCM_H264_444    0x00002 // Sunshine extension
 #define SCM_HEVC        0x00100
 #define SCM_HEVC_MAIN10 0x00200
+#define SCM_HEVC_444_10 0x00400 // Sunshine extension
 #define SCM_AV1_MAIN8   0x10000 // Sunshine extension
 #define SCM_AV1_MAIN10  0x20000 // Sunshine extension
+#define SCM_AV1_444_10  0x40000 // Sunshine extension
 
 // SCM masks to identify various codec capabilities
-#define SCM_MASK_H264   SCM_H264
-#define SCM_MASK_HEVC   (SCM_HEVC | SCM_HEVC_MAIN10)
-#define SCM_MASK_AV1    (SCM_AV1_MAIN8 | SCM_AV1_MAIN10)
-#define SCM_MASK_10BIT  (SCM_HEVC_MAIN10 | SCM_AV1_MAIN10)
+#define SCM_MASK_H264   (SCM_H264 | SCM_H264_444)
+#define SCM_MASK_HEVC   (SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_444_10)
+#define SCM_MASK_AV1    (SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_444_10)
+#define SCM_MASK_10BIT  (SCM_HEVC_MAIN10 | SCM_AV1_MAIN10 | SCM_HEVC_444_10 | SCM_AV1_444_10)
+#define SCM_MASK_444    (SCM_H264_444 | SCM_HEVC_444_10 | SCM_AV1_444_10)
 
 typedef struct _SERVER_INFORMATION {
     // Server host name or IP address in text form
