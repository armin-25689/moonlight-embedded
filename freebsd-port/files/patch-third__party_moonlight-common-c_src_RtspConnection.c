--- third_party/moonlight-common-c/src/RtspConnection.c.orig	2024-08-01 13:37:02 UTC
+++ third_party/moonlight-common-c/src/RtspConnection.c
@@ -1090,6 +1090,12 @@ int performRtspHandshake(PSERVER_INFORMATION serverInf
                 Limelog("WARNING: Host PC doesn't support HEVC. Streaming at resolutions above 4K using H.264 will likely fail!\n");
             }
         }
+        if ((serverInfo->serverCodecModeSupport & SCM_AV1_444_10) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_AV1_444))
+          NegotiatedVideoFormat |= VIDEO_FORMAT_AV1_444;
+        else if ((serverInfo->serverCodecModeSupport & SCM_HEVC_444_10) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H265_444))
+          NegotiatedVideoFormat |= VIDEO_FORMAT_H265_444;
+        else if ((serverInfo->serverCodecModeSupport & SCM_H264_444) && (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_H264_444))
+          NegotiatedVideoFormat |= VIDEO_FORMAT_H264_444;
 
         // Look for the SDP attribute that indicates we're dealing with a server that supports RFI
         ReferenceFrameInvalidationSupported = strstr(response.payload, "x-nv-video[0].refPicInvalidation") != NULL;
