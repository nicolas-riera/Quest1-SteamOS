// Runs StereoDepth (../steamvr-quest1/stereo_depth.cpp) on the live camera ring and prints numbers only: compute time,
// centre depth, a coarse ASCII map of the grid and the confident-cell ratio, once a second. No image content is ever
// written, logged or sent anywhere. Usage: depth_test [seconds=5]   (see build_test.sh for the build)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "../steamvr-quest1/quest1_camera_ring.h"
#include "../steamvr-quest1/stereo_depth.h"
#include "camcalib.h"

static double NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// device frame -> raw camera direction R^T (p - t) -> raw pixel
static StereoDepth::ProjectFn Projector(const CamModel &c)
{
	return [c](const double p[3], double &px, double &py) {
		double q[3] = {p[0] - c.t[0], p[1] - c.t[1], p[2] - c.t[2]}, d[3];
		for (int i = 0; i < 3; i++)
			d[i] = c.R[0][i] * q[0] + c.R[1][i] * q[1] + c.R[2][i] * q[2];
		return c.Project(d, px, py);
	};
}

static char Bucket(float d, bool filled)
{
	static const float lim[] = {0.4f, 0.6f, 0.8f, 1.0f, 1.3f, 1.6f, 2.0f, 2.5f, 3.5f, 5.0f};
	int b = 0;
	while (b < 10 && d >= lim[b])
		b++;
	if (b == 10)
		return filled ? '-' : '+';
	return (char)((filled ? 'a' : '0') + b); // digit = measured, letter = filled in; 0/a < 0.4 m ... 9/j 3.5-5 m
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 5;
	std::vector<CamModel> cams(2);
	cams[0].id = 0, cams[1].id = 2;
	LoadCalibration(cams);
	if (!cams[0].calibrated || !cams[1].calibrated) {
		fprintf(stderr, "calibration for cameras 0/2 not readable\n");
		return 1;
	}
	{
		double bx = cams[1].t[0] - cams[0].t[0], by = cams[1].t[1] - cams[0].t[1], bz = cams[1].t[2] - cams[0].t[2];
		printf("baseline %.3f m, cam0 t (%.3f %.3f %.3f), cam2 t (%.3f %.3f %.3f)\n", sqrt(bx * bx + by * by + bz * bz),
		       cams[0].t[0], cams[0].t[1], cams[0].t[2], cams[1].t[0], cams[1].t[1], cams[1].t[2]);
	}

	int fd = open(Q1CAM_RING_DEFAULT_PATH, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)sizeof(q1cam_ring_header)) {
		fprintf(stderr, "ring not available\n");
		return 1;
	}
	const uint8_t *map = (const uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		return 1;
	const q1cam_ring_header *hdr = (const q1cam_ring_header *)map;
	if (hdr->magic != Q1CAM_RING_MAGIC || hdr->version != Q1CAM_RING_VERSION ||
	    (uint64_t)st.st_size < q1cam_ring_file_size(hdr->slot_bytes)) {
		fprintf(stderr, "bad ring header\n");
		return 1;
	}

	StereoDepth sd;
	StereoDepth::Params prm;
	if (!sd.Configure(prm, Projector(cams[0]), Projector(cams[1]), cams[0].w, cams[0].h)) {
		fprintf(stderr, "Configure failed\n");
		return 1;
	}

	std::vector<uint8_t> img[2];
	uint32_t last = hdr->write_count, first = last;
	printf("ring write_count %u, producer pid %d\n", last, hdr->producer_pid);
	double t0 = NowMs(), tReport = t0;
	int frames = 0, torn = 0;
	double sumMs = 0, maxMs = 0;
	while (NowMs() - t0 < seconds * 1000) {
		uint32_t n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
		if (n == last) {
			usleep(2000);
		} else {
			last = n;
			const q1cam_ring_slot &s = hdr->slot[(n - 1) % hdr->slot_count];
			uint32_t s0 = s.seq;
			bool ok = !(s0 & 1) && s.frame_type == Q1CAM_FRAME_HEADSET;
			const uint8_t *px = map + hdr->header_size + (size_t)((n - 1) % hdr->slot_count) * hdr->slot_bytes;
			if (ok) {
				static const uint32_t ids[2] = {0, 2};
				for (int k = 0; k < 2 && ok; k++) {
					const q1cam_ring_cam *rc = nullptr;
					for (uint32_t c = 0; c < s.ncams && c < Q1CAM_MAX_CAMS; c++)
						if (s.cam[c].present && s.cam[c].cam_id == ids[k])
							rc = &s.cam[c];
					if (!rc || (int)rc->width != cams[0].w || (int)rc->height != cams[0].h ||
					    (uint64_t)rc->offset + (uint64_t)rc->stride * rc->height > hdr->slot_bytes ||
					    rc->stride != rc->width) {
						ok = false;
						break;
					}
					img[k].assign(px + rc->offset, px + rc->offset + (size_t)rc->stride * rc->height);
				}
				if (ok && s.seq != s0) {
					torn++;
					ok = false;
				}
			}
			if (ok) {
				double a = NowMs();
				sd.Compute(img[0].data(), img[1].data(), cams[0].w);
				double ms = NowMs() - a;
				sumMs += ms, maxMs = std::max(maxMs, ms), frames++;
			}
		}
		double now = NowMs();
		if (now - tReport >= 1000) {
			tReport = now;
			int C = sd.Cols(), R = sd.Rows(), conf = 0;
			for (int i = 0; i < C * R; i++)
				conf += sd.Confidence(i) > 0;
			const StereoDepth::Stats &stt = sd.LastStats();
			double mean[2] = {0, 0}, sd2[2] = {0, 0};
			for (int k = 0; k < 2; k++) {
				double s1 = 0, s2 = 0;
				for (size_t j = 0; j < img[k].size(); j += 7)
					s1 += img[k][j], s2 += (double)img[k][j] * img[k][j];
				size_t cnt = (img[k].size() + 6) / 7;
				mean[k] = cnt ? s1 / cnt : 0, sd2[k] = cnt ? sqrt(std::max(0.0, s2 / cnt - mean[k] * mean[k])) : 0;
			}
			printf("images: left mean %.0f sd %.0f, right mean %.0f sd %.0f; visible cells %d, textured %d, mean best ZNCC %.2f\n",
			       mean[0], sd2[0], mean[1], sd2[1], stt.visible, stt.textured, stt.meanZncc);
			printf("--- ring +%u frame sets, %d computed (%d torn), compute avg %.2f ms max %.2f ms, confident %.0f%%, "
			       "centre depth %.2f m (conf %.2f)\n",
			       last - first, frames, torn, frames ? sumMs / frames : 0.0, maxMs, 100.0 * conf / (C * R),
			       sd.Depth((R / 2) * C + C / 2), sd.Confidence((R / 2) * C + C / 2));
			for (int r = 0; r < R; r += 2) {
				std::string line;
				for (int c = 0; c < C; c++)
					line += Bucket(sd.Depth(r * C + c), sd.Confidence(r * C + c) <= 0);
				printf("%s\n", line.c_str());
			}
			fflush(stdout);
			frames = 0, sumMs = 0, maxMs = 0;
		}
	}
	if (last == first)
		printf("write_count did not advance: the ring is not being written\n");
	return 0;
}
