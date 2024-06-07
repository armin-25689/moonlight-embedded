--- third_party/moonlight-common-c/src/RtspConnection.c.orig	2024-02-18 00:20:24 UTC
+++ third_party/moonlight-common-c/src/RtspConnection.c
@@ -1090,6 +1090,9 @@ int performRtspHandshake(PSERVER_INFORMATION serverInf
                 Limelog("WARNING: Host PC doesn't support HEVC. Streaming at resolutions above 4K using H.264 will likely fail!\n");
             }
         }
+	if (StreamConfig.supportedVideoFormats & VIDEO_FORMAT_MASK_YUV444) {
+	  NegotiatedVideoFormat |= VIDEO_FORMAT_MASK_YUV444;
+	}
 
         // Look for the SDP attribute that indicates we're dealing with a server that supports RFI
         ReferenceFrameInvalidationSupported = strstr(response.payload, "x-nv-video[0].refPicInvalidation") != NULL;
