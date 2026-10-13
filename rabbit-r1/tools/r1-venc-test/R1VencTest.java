// SPDX-License-Identifier: Apache-2.0
// Encodes a synthetic moving pattern with one H.264 encoder and muxes it to MP4,
// so encoder output can be checked without a camera scene:
//   adb push r1-venc-test.jar /data/local/tmp/
//   adb shell CLASSPATH=/data/local/tmp/r1-venc-test.jar app_process / R1VencTest \
//       [codec] [yuv|surface] [width] [height] [frames]
// yuv feeds YUV 4:2:0 through the codec's input images, which needs no colour
// conversion. surface draws RGBA with the GPU into the codec's input surface,
// which the MediaTek encoder converts first, as for camera apps that render.
// Writes /data/local/tmp/venc-<codec>-<mode>.mp4, and for yuv also .yuv, the
// I420 frames that went in, for a PSNR check on the host:
//   ffmpeg -i venc.mp4 -f rawvideo -s WxH -pix_fmt yuv420p -framerate 30 \
//       -i venc.yuv -lavfi psnr -f null -
// (the frame rate matters: ffmpeg pairs frames by time).
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.view.Surface;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;

public class R1VencTest {
    static final int FPS = 30;

    // Frame n of the pattern, as I420: a diagonal luma ramp scrolling right, a
    // bright square moving down, chroma ramps across and down.
    static byte[] frame(int w, int h, int n) {
        byte[] f = new byte[w * h * 3 / 2];
        int bx = (n * 7) % (w - 64), by = (n * 5) % (h - 64);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v = ((x + y) / 2 + n * 4) & 0xff;
                if (x >= bx && x < bx + 64 && y >= by && y < by + 64)
                    v = 235;
                f[y * w + x] = (byte) v;
            }
        int u = w * h, cw = w / 2, ch = h / 2;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                f[u + y * cw + x] = (byte) (64 + x * 128 / cw);
                f[u + cw * ch + y * cw + x] = (byte) (64 + y * 128 / ch);
            }
        return f;
    }

    // Copies an I420 frame into whatever YUV 4:2:0 layout the codec's image has.
    static void fill(Image img, byte[] f, int w, int h) {
        Image.Plane[] p = img.getPlanes();
        for (int i = 0; i < 3; i++) {
            ByteBuffer b = p[i].getBuffer();
            int pw = i == 0 ? w : w / 2, ph = i == 0 ? h : h / 2;
            int off = i == 0 ? 0 : i == 1 ? w * h : w * h + pw * ph;
            int rs = p[i].getRowStride(), ps = p[i].getPixelStride();
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    b.put(y * rs + x * ps, f[off + y * pw + x]);
        }
    }

    public static void main(String[] args) throws Exception {
        String name = args.length > 0 ? args[0] : "c2.mtk.avc.encoder";
        boolean surface = args.length > 1 && args[1].equals("surface");
        int w = args.length > 2 ? Integer.parseInt(args[2]) : 1280;
        int h = args.length > 3 ? Integer.parseInt(args[3]) : 720;
        int frames = args.length > 4 ? Integer.parseInt(args[4]) : 90;
        String base = "/data/local/tmp/venc-" + name + "-" + (surface ? "surface" : "yuv");

        MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h);
        fmt.setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000);
        fmt.setInteger(MediaFormat.KEY_FRAME_RATE, FPS);
        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, surface
                ? MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
                : MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);

        MediaCodec codec = MediaCodec.createByCodecName(name);
        codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        Surface input = surface ? codec.createInputSurface() : null;
        codec.start();
        System.out.println("codec " + codec.getName() + ", " + w + "x" + h + ", "
                + (surface ? "RGBA surface" : "YUV images") + " input");

        MediaMuxer mux = new MediaMuxer(base + ".mp4", MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
        FileOutputStream ref = surface ? null : new FileOutputStream(base + ".yuv");
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        Paint paint = new Paint();
        int track = -1, in = 0, out = 0, empty = 0;
        long bytes = 0, start = System.nanoTime();
        boolean inputDone = false;

        while (true) {
            if (!inputDone) {
                if (surface) {
                    Canvas c = input.lockHardwareCanvas();
                    c.drawColor(Color.rgb((in * 4) & 0xff, 96, 160));
                    paint.setColor(Color.WHITE);
                    float x = (in * 7) % (w - 64), y = (in * 5) % (h - 64);
                    c.drawRect(x, y, x + 64, y + 64, paint);
                    input.unlockCanvasAndPost(c);
                    if (++in == frames) {
                        codec.signalEndOfInputStream();
                        inputDone = true;
                    }
                } else {
                    int idx = codec.dequeueInputBuffer(10_000);
                    if (idx >= 0) {
                        long pts = in * 1_000_000L / FPS;
                        if (in == frames) {
                            codec.queueInputBuffer(idx, 0, 0, pts, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            byte[] f = frame(w, h, in);
                            fill(codec.getInputImage(idx), f, w, h);
                            ref.write(f);
                            codec.queueInputBuffer(idx, 0, f.length, pts, 0);
                            in++;
                        }
                    }
                }
            }
            int idx = codec.dequeueOutputBuffer(info, 10_000);
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                track = mux.addTrack(codec.getOutputFormat());
                mux.start();
            } else if (idx >= 0) {
                boolean config = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                if (!config && info.size > 0) {
                    mux.writeSampleData(track, codec.getOutputBuffer(idx), info);
                    out++;
                    bytes += info.size;
                } else if (!config) {
                    empty++;
                }
                codec.releaseOutputBuffer(idx, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0)
                    break;
            }
        }
        double secs = (System.nanoTime() - start) / 1e9;
        codec.stop();
        codec.release();
        mux.stop();
        mux.release();
        if (ref != null)
            ref.close();
        System.out.printf("%d frames in, %d out (%d empty), %d bytes, %.2f s, %.1f fps%n",
                in, out, empty, bytes, secs, out / secs);
        System.out.println(base + ".mp4");
    }
}
