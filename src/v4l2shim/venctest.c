// venctest: encode a synthetic NV12 clip through the mainline V4L2 encoder API, the way Steam's Remote Play does
// (CGameStreamVideoStageV4L2, see quest1_v4l2.c), to check quest1_v4l2 on the venus encoder without Steam.
//
//   LD_PRELOAD=libquest1_v4l2.so venctest [-d /dev/video-enc0] [-c h264|hevc] [-s 1280x720] [-r 60] [-b kbit/s]
//                                         [-n frames] [-k keyframe-at] [-o out.264]
//
// Per frame: moving bars (luma) over a chroma ramp, written with the S_FMT bytesperline and the UV plane right
// under the S_FMT height, as Steam writes them. Reports encode latency (QBUF of the frame to DQBUF of its
// bitstream) and checks that the first frame and the forced keyframe start with the sequence headers.
// Build: gcc -std=gnu11 -O2 -Wall -o venctest venctest.c. Measured 2026-11-09, 1280x720 at 60 fps, 20 Mbit/s:
// H.264 12 ms per frame (QBUF to bitstream), HEVC 9 ms; both decode cleanly in ffmpeg, colours and UV plane right.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fd;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void must(int r, const char *what)
{
	if (r < 0) {
		fprintf(stderr, "venctest: %s: %s\n", what, strerror(errno));
		exit(1);
	}
}

static void ctrl(uint32_t id, int32_t v, const char *what)
{
	struct v4l2_control c = {.id = id, .value = v};
	must(ioctl(fd, VIDIOC_S_CTRL, &c), what);
}

static int nal_type(int hevc, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i + 3 < n; i++)
		if (!p[i] && !p[i + 1] && p[i + 2] == 1)
			return hevc ? (p[i + 3] >> 1) & 0x3f : p[i + 3] & 0x1f;
	return -1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video-enc0", *out = NULL;
	int hevc = 0, fps = 60, kbps = 20000, frames = 300, key_at = 100;
	unsigned w = 1280, h = 720;
	for (int o; (o = getopt(argc, argv, "d:c:s:r:b:n:k:o:")) != -1;) {
		switch (o) {
		case 'd': dev = optarg; break;
		case 'c': hevc = !strcmp(optarg, "hevc"); break;
		case 's': sscanf(optarg, "%ux%u", &w, &h); break;
		case 'r': fps = atoi(optarg); break;
		case 'b': kbps = atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		case 'k': key_at = atoi(optarg); break;
		case 'o': out = optarg; break;
		default: return 2;
		}
	}
	fd = open(dev, O_RDWR);
	must(fd, dev);

	// Steam's order: the raw format, the coded format, the crop, the frame rate, the controls
	struct v4l2_format of = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	of.fmt.pix_mp.width = w, of.fmt.pix_mp.height = h, of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	must(ioctl(fd, VIDIOC_S_FMT, &of), "S_FMT OUTPUT");
	uint32_t stride = of.fmt.pix_mp.plane_fmt[0].bytesperline, frame = of.fmt.pix_mp.plane_fmt[0].sizeimage;
	struct v4l2_format cf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
	cf.fmt.pix_mp.width = w, cf.fmt.pix_mp.height = h;
	cf.fmt.pix_mp.pixelformat = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
	must(ioctl(fd, VIDIOC_S_FMT, &cf), "S_FMT CAPTURE");
	printf("raw: stride %u, sizeimage %u; bitstream buffers %u bytes\n", stride, frame,
	       cf.fmt.pix_mp.plane_fmt[0].sizeimage);
	if (!stride) {
		fprintf(stderr, "venctest: bytesperline 0\n");
		return 1;
	}
	struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .target = V4L2_SEL_TGT_CROP};
	sel.r.width = w, sel.r.height = h;
	must(ioctl(fd, VIDIOC_S_SELECTION, &sel), "S_SELECTION");
	struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = fps;
	must(ioctl(fd, VIDIOC_S_PARM, &parm), "S_PARM");
	ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "FRAME_RC_ENABLE");
	ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "BITRATE_MODE");
	ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0x7fffffff, "GOP_SIZE");
	ctrl(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "PREPEND_SPSPPS_TO_IDR");
	if (hevc) {
		ctrl(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "HEVC_PROFILE");
		ctrl(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1, "HEVC_LEVEL");
	} else {
		ctrl(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H264_PROFILE");
		ctrl(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_5_1, "H264_LEVEL");
	}
	ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, kbps * 1000, "BITRATE");
	ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, kbps * 1200, "BITRATE_PEAK");

	// buffers: MMAP on both queues, every bitstream buffer queued
	enum { MAXB = 32 };
	uint8_t *omap[MAXB], *cmap[MAXB];
	uint32_t clen[MAXB];
	struct v4l2_requestbuffers rb = {.count = 4, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
	must(ioctl(fd, VIDIOC_REQBUFS, &rb), "REQBUFS OUTPUT");
	uint32_t nout = rb.count;
	for (uint32_t i = 0; i < nout; i++) {
		struct v4l2_plane p = {0};
		struct v4l2_buffer b = {.index = i, .type = rb.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
		must(ioctl(fd, VIDIOC_QUERYBUF, &b), "QUERYBUF OUTPUT");
		omap[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
		if (omap[i] == MAP_FAILED)
			must(-1, "mmap OUTPUT");
	}
	rb = (struct v4l2_requestbuffers){.count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP};
	must(ioctl(fd, VIDIOC_REQBUFS, &rb), "REQBUFS CAPTURE");
	uint32_t ncap = rb.count;
	for (uint32_t i = 0; i < ncap; i++) {
		struct v4l2_plane p = {0};
		struct v4l2_buffer b = {.index = i, .type = rb.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
		must(ioctl(fd, VIDIOC_QUERYBUF, &b), "QUERYBUF CAPTURE");
		clen[i] = p.length;
		cmap[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
		if (cmap[i] == MAP_FAILED)
			must(-1, "mmap CAPTURE");
		must(ioctl(fd, VIDIOC_QBUF, &b), "QBUF CAPTURE");
	}
	printf("%u raw buffers, %u bitstream buffers\n", nout, ncap);
	int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	must(ioctl(fd, VIDIOC_STREAMON, &t), "STREAMON OUTPUT");
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	must(ioctl(fd, VIDIOC_STREAMON, &t), "STREAMON CAPTURE");

	FILE *f = out ? fopen(out, "wb") : NULL;
	double lat_sum = 0, lat_max = 0, t0 = now_ms();
	uint64_t bytes = 0;
	int keys = 0, bad = 0;
	for (int n = 0; n < frames; n++) {
		uint32_t i = n % nout;
		if (n >= (int)nout) { // take a raw buffer back
			struct v4l2_plane p = {0};
			struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
			must(ioctl(fd, VIDIOC_DQBUF, &b), "DQBUF OUTPUT");
			i = b.index;
		}
		uint8_t *y = omap[i], *uv = omap[i] + stride * h; // Steam's layout
		for (unsigned r = 0; r < h; r++)
			for (unsigned c = 0; c < w; c++)
				y[r * stride + c] = (uint8_t)(((c + n * 8) / 64 % 2) ? 200 : 40) + (uint8_t)(r * 32 / h);
		for (unsigned r = 0; r < h / 2; r++)
			for (unsigned c = 0; c < w; c += 2) {
				uv[r * stride + c] = (uint8_t)(c * 255 / w);
				uv[r * stride + c + 1] = (uint8_t)(r * 2 * 255 / h);
			}
		if (n == key_at)
			ctrl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "FORCE_KEY_FRAME");
		struct v4l2_plane p = {.bytesused = frame};
		struct v4l2_buffer b = {.index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
		b.timestamp.tv_sec = n / fps;
		b.timestamp.tv_usec = (n % fps) * 1000000 / fps;
		double tq = now_ms();
		must(ioctl(fd, VIDIOC_QBUF, &b), "QBUF OUTPUT");
		struct pollfd pf = {.fd = fd, .events = POLLIN};
		if (poll(&pf, 1, 1000) <= 0) {
			fprintf(stderr, "venctest: frame %d: no bitstream after 1 s\n", n);
			return 1;
		}
		struct v4l2_plane cp = {0};
		struct v4l2_buffer cb = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP, .m.planes = &cp, .length = 1};
		must(ioctl(fd, VIDIOC_DQBUF, &cb), "DQBUF CAPTURE");
		double lat = now_ms() - tq;
		lat_sum += lat;
		if (lat > lat_max)
			lat_max = lat;
		int key = !!(cb.flags & V4L2_BUF_FLAG_KEYFRAME);
		int nt = nal_type(hevc, cmap[cb.index], cp.bytesused);
		int hdr = hevc ? nt == 32 : nt == 7;
		if (key) {
			keys++;
			printf("frame %d: keyframe, %u bytes, starts with %s (NAL %d), %.1f ms\n", n, cp.bytesused,
			       hdr ? "the sequence headers" : "NO HEADERS", nt, lat);
			bad += !hdr;
		}
		if ((n == 0 || n == key_at) && !key) {
			printf("frame %d: not a keyframe (flags 0x%x)\n", n, cb.flags);
			bad++;
		}
		if (cb.flags & 0xfff00000u) {
			printf("frame %d: private flags left: 0x%x\n", n, cb.flags);
			bad++;
		}
		if (f)
			fwrite(cmap[cb.index], 1, cp.bytesused, f);
		bytes += cp.bytesused;
		cp = (struct v4l2_plane){0};
		cb.m.planes = &cp;
		cb.length = 1;
		must(ioctl(fd, VIDIOC_QBUF, &cb), "QBUF CAPTURE");
	}
	double el = now_ms() - t0;
	printf("%d frames in %.0f ms (%.0f fps paced by us), encode latency avg %.2f ms max %.2f ms, %.1f kB/frame, "
	       "%d keyframes%s\n",
	       frames, el, frames * 1000.0 / el, lat_sum / frames, lat_max, bytes / 1024.0 / frames, keys,
	       bad ? ", ERRORS" : "");
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(fd, VIDIOC_STREAMOFF, &t);
	for (uint32_t i = 0; i < nout; i++)
		munmap(omap[i], frame);
	for (uint32_t i = 0; i < ncap; i++)
		munmap(cmap[i], clen[i]);
	close(fd);
	if (f)
		fclose(f);
	return bad ? 1 : 0;
}
