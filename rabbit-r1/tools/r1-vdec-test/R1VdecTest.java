// SPDX-License-Identifier: Apache-2.0
// Decodes the video track of a file with one decoder and writes the frames as
// I420, so decoder output can be compared with a reference decode:
//   adb push r1-vdec-test.jar /data/local/tmp/
//   adb shell CLASSPATH=/data/local/tmp/r1-vdec-test.jar app_process / R1VdecTest \
//       codec file [buffer|surface|display]
// buffer reads the codec's output images; surface has it render into a YUV
// ImageReader; display renders into a layer on screen at the clip's frame
// rate, as players do, and holds the last frame for 5 s (take a screencap).
// display uses SurfaceControl APIs apps cannot, which app_process can.
// Writes <file>-<codec>-<mode>.yuv; on the host:
//   ffmpeg -i file -f rawvideo -pix_fmt yuv420p ref.yuv
//   ffmpeg -f rawvideo -s WxH -pix_fmt yuv420p -i out.yuv \
//       -f rawvideo -s WxH -pix_fmt yuv420p -i ref.yuv -lavfi psnr -f null -
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.view.Surface;
import android.view.SurfaceControl;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;

public class R1VdecTest {
    // Writes the visible w x h of a YUV 4:2:0 image as I420.
    static void dump(Image img, int w, int h, FileOutputStream out) throws Exception {
        Image.Plane[] p = img.getPlanes();
        byte[] row = new byte[w];
        for (int i = 0; i < 3; i++) {
            ByteBuffer b = p[i].getBuffer();
            int pw = i == 0 ? w : (w + 1) / 2, ph = i == 0 ? h : (h + 1) / 2;
            int rs = p[i].getRowStride(), ps = p[i].getPixelStride();
            for (int y = 0; y < ph; y++) {
                if (ps == 1) {
                    b.position(y * rs);
                    b.get(row, 0, pw);
                } else {
                    for (int x = 0; x < pw; x++)
                        row[x] = b.get(y * rs + x * ps);
                }
                out.write(row, 0, pw);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        String name = args[0], file = args[1];
        String mode = args.length > 2 ? args[2] : "buffer";
        boolean surface = mode.equals("surface"), display = mode.equals("display");
        String outName = file + "-" + name + "-" + mode + ".yuv";

        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(file);
        MediaFormat fmt = null;
        for (int i = 0; i < ex.getTrackCount(); i++) {
            MediaFormat f = ex.getTrackFormat(i);
            if (f.getString(MediaFormat.KEY_MIME).startsWith("video/")) {
                ex.selectTrack(i);
                fmt = f;
                break;
            }
        }
        int w = fmt.getInteger(MediaFormat.KEY_WIDTH), h = fmt.getInteger(MediaFormat.KEY_HEIGHT);

        ImageReader reader = null;
        SurfaceControl layer = null;
        Surface target = null;
        MediaCodec codec = MediaCodec.createByCodecName(name);
        if (surface) {
            reader = ImageReader.newInstance(w, h, ImageFormat.YUV_420_888, 4);
            target = reader.getSurface();
        } else if (display) {
            layer = new SurfaceControl.Builder().setName("r1-vdec-test").setBufferSize(w, h)
                    .build();
            SurfaceControl.Transaction t = new SurfaceControl.Transaction();
            SurfaceControl.Transaction.class.getMethod("setLayerStack", SurfaceControl.class,
                    int.class).invoke(t, layer, 0);
            t.setLayer(layer, Integer.MAX_VALUE).setVisibility(layer, true)
                    .setGeometry(layer, new Rect(0, 0, w, h), new Rect(0, 0, w, h), 0).apply();
            target = new Surface(layer);
        } else {
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        }
        codec.configure(fmt, target, null, 0);
        codec.start();
        System.out.println("codec " + codec.getName() + ", " + fmt.getString(MediaFormat.KEY_MIME)
                + " " + w + "x" + h + ", " + mode + " output");

        FileOutputStream out = new FileOutputStream(outName);
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int in = 0, frames = 0;
        boolean inputDone = false;
        long start = System.nanoTime();
        while (true) {
            if (!inputDone) {
                int idx = codec.dequeueInputBuffer(10_000);
                if (idx >= 0) {
                    int n = ex.readSampleData(codec.getInputBuffer(idx), 0);
                    if (n < 0) {
                        codec.queueInputBuffer(idx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(idx, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                        in++;
                    }
                }
            }
            int idx = codec.dequeueOutputBuffer(info, 10_000);
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                System.out.println("output format " + codec.getOutputFormat());
            } else if (idx >= 0) {
                boolean eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                if (info.size > 0 || (surface || display) && !eos) {
                    if (display) {
                        // At the clip's pace from the first frame.
                        long due = start + info.presentationTimeUs * 1000;
                        long wait = (due - System.nanoTime()) / 1_000_000;
                        if (wait > 0)
                            Thread.sleep(wait);
                        codec.releaseOutputBuffer(idx, true);
                    } else if (surface) {
                        codec.releaseOutputBuffer(idx, true);
                        Image img = null;
                        for (int t = 0; t < 200 && img == null; t++) {
                            img = reader.acquireNextImage();
                            if (img == null)
                                Thread.sleep(5);
                        }
                        if (img == null)
                            throw new IllegalStateException("no image for frame " + frames);
                        dump(img, w, h, out);
                        img.close();
                    } else {
                        dump(codec.getOutputImage(idx), w, h, out);
                        codec.releaseOutputBuffer(idx, false);
                    }
                    frames++;
                } else {
                    codec.releaseOutputBuffer(idx, false);
                }
                if (eos)
                    break;
            }
        }
        double secs = (System.nanoTime() - start) / 1e9;
        if (display)
            Thread.sleep(5000);
        codec.stop();
        codec.release();
        out.close();
        System.out.printf("%d samples in, %d frames out, %.2f s, %.1f fps%n", in, frames, secs,
                frames / secs);
        System.out.println(outName);
    }
}
