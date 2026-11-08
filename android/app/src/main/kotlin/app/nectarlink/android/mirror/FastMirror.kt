// SPDX-License-Identifier: GPL-3.0-or-later
package app.nectarlink.android.mirror

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build

/** The same low-delay H.264 I/P mode for Wi-Fi and USB. */
internal object FastMirror {
    fun encoder(width: Int, height: Int, fps: Int): MediaCodec {
        val encoder = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { info ->
            if (!info.isEncoder || !info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) }) return@firstOrNull false
            val hardware = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) info.isHardwareAccelerated
                else !info.name.startsWith("OMX.google.") && !info.name.startsWith("c2.android.")
            if (!hardware) return@firstOrNull false
            val video = requireNotNull(info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities) {
                "H.264 encoder ${info.name} did not report video capabilities"
            }
            video.areSizeAndRateSupported(width, height, fps.toDouble())
        } ?: error("No hardware H.264 encoder supports ${width}x$height at $fps fps")
        return MediaCodec.createByCodecName(encoder.name)
    }

    fun configure(codec: MediaCodec, format: MediaFormat, width: Int, height: Int, fps: Int, requested: Int): Int {
        // Keep the same high-quality budget on Wi-Fi and USB.
        val floor = when {
            maxOf(width, height) <= 1280 -> 20_000_000
            maxOf(width, height) <= 1920 -> 60_000_000
            else -> 100_000_000
        }
        val caps = codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
        val video = requireNotNull(caps.videoCapabilities) { "${codec.name}: missing H.264 video capabilities" }
        val encoder = requireNotNull(caps.encoderCapabilities) { "${codec.name}: missing encoder capabilities" }
        val bitrate = video.bitrateRange.clamp(maxOf(floor, requested))
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        format.setInteger(MediaFormat.KEY_LATENCY, 0)
        format.setFloat(MediaFormat.KEY_OPERATING_RATE, fps.toFloat())
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            format.setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, fps.toFloat())
        } else {
            // Before KEY_MAX_B_FRAMES, Baseline guarantees no B pictures.
            format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
        }
        check(encoder.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) { "Hardware H.264 encoder ${codec.name} does not support VBR" }
        format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
        return bitrate
    }
}
