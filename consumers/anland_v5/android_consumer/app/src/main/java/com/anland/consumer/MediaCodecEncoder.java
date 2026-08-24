package com.anland.consumer;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaCodecList;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Arrays;

final class MediaCodecEncoder implements AutoCloseable {
    interface FrameSink {
        boolean onFrame(byte[] annexB, long timestampMs, boolean keyFrame) throws IOException;
    }

    private static final String TAG = "AnlandEncoder";
    private static final String MIME = MediaFormat.MIMETYPE_VIDEO_AVC;
    private static final int DEQUEUE_TIMEOUT_US = 10_000;

    private final MediaCodec codec;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Surface inputSurface;
    private final FrameSink sink;
    private final Runnable failureHandler;
    private final Thread outputThread;
    private volatile boolean running = true;
    private boolean codecStopped;
    private boolean released;
    private byte[] codecConfig = new byte[0];
    private final ByteArrayOutputStream partialAccessUnit = new ByteArrayOutputStream();
    private int partialFlags;
    private long partialPresentationTimeUs;

    MediaCodecEncoder(int width, int height, int fps, int bitRate, FrameSink sink,
                      Runnable failureHandler) throws IOException {
        this.sink = sink;
        this.failureHandler = failureHandler;
        EncoderSelection selection = findHardwareEncoder(width, height, fps);
        if (selection == null) {
            throw new IOException("No compatible hardware H.264 Surface encoder is available");
        }
        int selectedBitRate = Math.max(selection.minBitRate,
                Math.min(selection.maxBitRate, bitRate));
        InitializedCodec initialized = initializeCodec(selection, width, height, fps,
                selectedBitRate);
        codec = initialized.codec;
        inputSurface = initialized.surface;

        outputThread = new Thread(this::drainOutput, "anland-codec-output");
        outputThread.start();
        Log.i(TAG, "Started " + codec.getName() + " at " + width + "x" + height
                + " " + fps + "fps " + selectedBitRate + "bps");
    }

    private static final class InitializedCodec {
        final MediaCodec codec;
        final Surface surface;

        InitializedCodec(MediaCodec codec, Surface surface) {
            this.codec = codec;
            this.surface = surface;
        }
    }

    private static final class EncoderSelection {
        final String codecName;
        final int bitRateMode;
        final boolean supportsMainProfile;
        final int minBitRate;
        final int maxBitRate;

        EncoderSelection(String codecName, int bitRateMode, boolean supportsMainProfile,
                         int minBitRate, int maxBitRate) {
            this.codecName = codecName;
            this.bitRateMode = bitRateMode;
            this.supportsMainProfile = supportsMainProfile;
            this.minBitRate = minBitRate;
            this.maxBitRate = maxBitRate;
        }
    }

    private static InitializedCodec initializeCodec(EncoderSelection selection,
                                                     int width, int height, int fps,
                                                     int bitRate) throws IOException {
        MediaCodec codec = MediaCodec.createByCodecName(selection.codecName);
        Surface surface = null;
        boolean started = false;
        try {
            MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitRate);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
            format.setInteger(MediaFormat.KEY_BITRATE_MODE, selection.bitRateMode);
            if (selection.supportsMainProfile) {
                format.setInteger(MediaFormat.KEY_PROFILE,
                        MediaCodecInfo.CodecProfileLevel.AVCProfileMain);
            }
            format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);

            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            surface = codec.createInputSurface();
            codec.start();
            started = true;
            return new InitializedCodec(codec, surface);
        } finally {
            if (!started) {
                if (surface != null) surface.release();
                codec.release();
            }
        }
    }

    private static EncoderSelection findHardwareEncoder(int width, int height, int fps) {
        MediaCodecList codecs = new MediaCodecList(MediaCodecList.ALL_CODECS);
        for (MediaCodecInfo info : codecs.getCodecInfos()) {
            if (!info.isEncoder() || !info.isHardwareAccelerated()) continue;
            MediaCodecInfo.CodecCapabilities capabilities;
            try {
                capabilities = info.getCapabilitiesForType(MIME);
            } catch (IllegalArgumentException e) {
                continue;
            }
            boolean surfaceInput = false;
            for (int colorFormat : capabilities.colorFormats) {
                if (colorFormat == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) {
                    surfaceInput = true;
                    break;
                }
            }
            if (!surfaceInput
                    || !capabilities.getVideoCapabilities()
                            .areSizeAndRateSupported(width, height, fps)) {
                continue;
            }

            MediaCodecInfo.EncoderCapabilities encoderCapabilities =
                    capabilities.getEncoderCapabilities();
            int bitRateMode;
            if (encoderCapabilities.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
                bitRateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
            } else if (encoderCapabilities.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) {
                bitRateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR;
            } else {
                continue;
            }

            boolean mainProfile = false;
            for (MediaCodecInfo.CodecProfileLevel profileLevel : capabilities.profileLevels) {
                if (profileLevel.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileMain) {
                    mainProfile = true;
                    break;
                }
            }
            return new EncoderSelection(info.getName(), bitRateMode, mainProfile,
                    capabilities.getVideoCapabilities().getBitrateRange().getLower(),
                    capabilities.getVideoCapabilities().getBitrateRange().getUpper());
        }
        return null;
    }

    Surface getInputSurface() {
        return inputSurface;
    }

    void requestIdr() {
        if (!running) return;
        Bundle parameters = new Bundle();
        parameters.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
        try {
            codec.setParameters(parameters);
        } catch (IllegalStateException e) {
            Log.w(TAG, "Unable to request IDR", e);
        }
    }

    private void drainOutput() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            while (running) {
                int index = codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US);
                if (index == MediaCodec.INFO_TRY_AGAIN_LATER) continue;
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    updateCodecConfig(codec.getOutputFormat());
                    continue;
                }
                if (index < 0) continue;

                try {
                    ByteBuffer output = codec.getOutputBuffer(index);
                    if (output == null || info.size <= 0) continue;
                    output.position(info.offset);
                    output.limit(info.offset + info.size);
                    byte[] chunk = new byte[info.size];
                    output.get(chunk);

                    boolean partial = (info.flags & MediaCodec.BUFFER_FLAG_PARTIAL_FRAME) != 0;
                    if (partialAccessUnit.size() > 0 || partial) {
                        if (partialAccessUnit.size() == 0) {
                            partialPresentationTimeUs = info.presentationTimeUs;
                            partialFlags = 0;
                        }
                        partialAccessUnit.write(chunk, 0, chunk.length);
                        partialFlags |= info.flags;
                        if (partial) continue;

                        byte[] accessUnit = partialAccessUnit.toByteArray();
                        long presentationTimeUs = partialPresentationTimeUs;
                        int flags = partialFlags;
                        partialAccessUnit.reset();
                        partialFlags = 0;
                        processAccessUnit(accessUnit, presentationTimeUs, flags);
                    } else {
                        processAccessUnit(chunk, info.presentationTimeUs, info.flags);
                    }
                } finally {
                    codec.releaseOutputBuffer(index, false);
                }
            }
        } catch (Exception e) {
            if (running) {
                Log.e(TAG, "Encoder output failed", e);
                if (failureHandler != null) mainHandler.post(failureHandler);
            }
        }
    }

    private void processAccessUnit(byte[] accessUnit, long presentationTimeUs, int flags)
            throws IOException {
        byte[] annexB = toAnnexB(accessUnit);
        boolean config = (flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
        boolean keyFrame = (flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
        if (config) {
            codecConfig = annexB;
            return;
        }
        if (keyFrame && codecConfig.length > 0 && !containsParameterSets(annexB)) {
            byte[] withConfig = Arrays.copyOf(codecConfig, codecConfig.length + annexB.length);
            System.arraycopy(annexB, 0, withConfig, codecConfig.length, annexB.length);
            annexB = withConfig;
        }
        if (sink.onFrame(annexB, presentationTimeUs / 1000L, keyFrame)) requestIdr();
    }

    private void updateCodecConfig(MediaFormat format) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        appendCsd(out, format.getByteBuffer("csd-0"));
        appendCsd(out, format.getByteBuffer("csd-1"));
        if (out.size() > 0) codecConfig = out.toByteArray();
    }

    private static void appendCsd(ByteArrayOutputStream out, ByteBuffer source) {
        if (source == null) return;
        ByteBuffer data = source.duplicate();
        byte[] bytes = new byte[data.remaining()];
        data.get(bytes);
        byte[] annexB = toAnnexB(bytes);
        out.write(annexB, 0, annexB.length);
    }

    private static byte[] toAnnexB(byte[] data) {
        if (startsWithStartCode(data)) return data;

        ByteBuffer input = ByteBuffer.wrap(data);
        ByteArrayOutputStream output = new ByteArrayOutputStream(data.length + 16);
        while (input.remaining() >= 4) {
            int length = input.getInt();
            if (length <= 0 || length > input.remaining()) return data;
            output.write(0);
            output.write(0);
            output.write(0);
            output.write(1);
            output.write(data, input.position(), length);
            input.position(input.position() + length);
        }
        return input.hasRemaining() ? data : output.toByteArray();
    }

    private static boolean startsWithStartCode(byte[] data) {
        return data.length >= 4 && data[0] == 0 && data[1] == 0
                && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
    }

    private static boolean containsParameterSets(byte[] data) {
        for (int i = 0; i + 4 < data.length; i++) {
            int start = -1;
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                start = i + 3;
            } else if (i + 5 < data.length && data[i] == 0 && data[i + 1] == 0
                    && data[i + 2] == 0 && data[i + 3] == 1) {
                start = i + 4;
            }
            if (start >= 0) {
                int type = data[start] & 0x1f;
                if (type == 7 || type == 8) return true;
            }
        }
        return false;
    }

    synchronized void beginClose() {
        if (codecStopped) return;
        running = false;
        codecStopped = true;
        try {
            codec.stop();
        } catch (IllegalStateException ignored) {
        }
    }

    synchronized void finishClose() {
        if (released) return;
        beginClose();
        boolean interrupted = false;
        while (outputThread.isAlive()) {
            try {
                outputThread.join();
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        inputSurface.release();
        codec.release();
        released = true;
        if (interrupted) Thread.currentThread().interrupt();
    }

    @Override
    public void close() {
        beginClose();
        finishClose();
    }
}
