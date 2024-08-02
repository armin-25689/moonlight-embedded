--- third_party/moonlight-common-c/src/SdpGenerator.c.orig	2024-08-01 13:37:02 UTC
+++ third_party/moonlight-common-c/src/SdpGenerator.c
@@ -343,7 +343,9 @@ static PSDP_OPTION getAttributesList(char*urlSafeAddr)
     // GFE currently imposes a limit of 100 Mbps for the video bitrate. It will automatically
     // impose that on maximumBitrateKbps but not on initialBitrateKbps. We will impose the cap
     // ourselves so initialBitrateKbps does not exceed maximumBitrateKbps.
-    adjustedBitrate = adjustedBitrate > 100000 ? 100000 : adjustedBitrate;
+    if (!IS_SUNSHINE()) {
+      adjustedBitrate = adjustedBitrate > 100000 ? 100000 : adjustedBitrate;
+    }
 
     // We don't support dynamic bitrate scaling properly (it tends to bounce between min and max and never
     // settle on the optimal bitrate if it's somewhere in the middle), so we'll just latch the bitrate
@@ -443,6 +445,14 @@ static PSDP_OPTION getAttributesList(char*urlSafeAddr)
         }
 
         if (AppVersionQuad[0] >= 7) {
+            if (IS_SUNSHINE()) {
+                if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_YUV444) {
+                    err |= addAttributeString(&optionHead, "x-nv-video[0].yuv444Mode", "1");
+                }
+                else {
+                    err |= addAttributeString(&optionHead, "x-nv-video[0].yuv444Mode", "0");
+                }
+            }
             // Enable HDR if requested
             if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_10BIT) {
                 err |= addAttributeString(&optionHead, "x-nv-video[0].dynamicRangeMode", "1");
