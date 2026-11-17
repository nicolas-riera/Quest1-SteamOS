// Offline evaluation of StereoDense (../steamvr-quest1/stereo_dense.cpp) against the old StereoDepth plane sweep.
//
//   synth [scene=0] [noise=2] [contrast=1]  rendered scenes through the factory calibration, exact ground truth
//   seq                                     moving head, temporal fusion on/off
//   real <ring file> [slot=0]               frames from a copy of the camera ring (kept in build/, never committed)
//   bench [n=200]                           timing of the rigs on synthetic frames
//
// Calibration: QUEST1_CAMERA_CALIB (a copy of camera_calibration_v2.json). Prints numbers; with DENSE_OUT=<dir> it also
// writes PGM/PPM images of the rectified view and the disparity.
// Build (WSL, native x86 or aarch64): see build_test.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../steamvr-quest1/quest1_camera_ring.h"
#include "../steamvr-quest1/stereo_dense.h"
#include "../steamvr-quest1/stereo_depth.h"
#include "camcalib.h"

static double NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

struct V3
{
	double x = 0, y = 0, z = 0;
};
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ---- camera helpers -------------------------------------------------------------------------------------------------

static StereoDepth::ProjectFn Projector(const CamModel &c)
{
	return [c](const double p[3], double &px, double &py) {
		double q[3] = {p[0] - c.t[0], p[1] - c.t[1], p[2] - c.t[2]}, d[3];
		for (int i = 0; i < 3; i++)
			d[i] = c.R[0][i] * q[0] + c.R[1][i] * q[1] + c.R[2][i] * q[2];
		return c.Project(d, px, py);
	};
}

// raw pixel -> raw camera direction (inverse of CamModel::Project)
static V3 Unproject(const CamModel &c, double px, double py)
{
	double ut = (px - c.cx) / c.f, vt = (py - c.cy) / c.f, u = ut, v = vt;
	for (int it = 0; it < 12; it++) {
		double r2 = u * u + v * v;
		u = ut - (2 * c.p[0] * u * v + c.p[1] * (r2 + 2 * u * u));
		v = vt - (c.p[1] * 2 * u * v + c.p[0] * (r2 + 2 * v * v));
		// (the second line uses the freshly updated u: fine for these tiny tangential terms)
	}
	double thd = sqrt(u * u + v * v), th = thd;
	for (int it = 0; it < 20; it++) {
		double t2 = th * th, poly = 1, dpoly = 1, tp = 1;
		for (int i = 0; i < 6; i++) {
			tp *= t2;
			poly += c.k[i] * tp;
			dpoly += c.k[i] * (2 * i + 3) * tp;
		}
		th -= (th * poly - thd) / dpoly;
	}
	double s = sin(th), co = cos(th);
	return thd > 1e-12 ? V3{s * u / thd, s * v / thd, co} : V3{0, 0, 1};
}

static V3 DevDir(const CamModel &c, V3 d) // raw camera direction -> device frame
{
	return {c.R[0][0] * d.x + c.R[0][1] * d.y + c.R[0][2] * d.z, c.R[1][0] * d.x + c.R[1][1] * d.y + c.R[1][2] * d.z,
	        c.R[2][0] * d.x + c.R[2][1] * d.y + c.R[2][2] * d.z};
}

// ---- synthetic world -----------------------------------------------------------------------------------------------

struct Pose // device -> world
{
	double R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	V3 t;
	V3 Rot(V3 v) const { return {R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z}; }
	V3 Apply(V3 p) const { return Rot(p) + t; }
	void M34(double m[12]) const
	{
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++)
				m[r * 4 + c] = R[r * 3 + c];
			m[r * 4 + 3] = r == 0 ? t.x : r == 1 ? t.y : t.z;
		}
	}
};

static Pose YawPose(double yawDeg, V3 t)
{
	Pose p;
	double a = yawDeg * M_PI / 180, c = cos(a), s = sin(a);
	p.R[0] = c, p.R[2] = s, p.R[6] = -s, p.R[8] = c;
	p.t = t;
	return p;
}

static uint32_t Hash3(int x, int y, int z, int seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u + (uint32_t)seed * 1274126177u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}
static double VNoise(V3 p, int seed) // value noise in [0,1]
{
	int ix = (int)floor(p.x), iy = (int)floor(p.y), iz = (int)floor(p.z);
	double fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
	fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy), fz = fz * fz * (3 - 2 * fz);
	double v = 0;
	for (int k = 0; k < 8; k++) {
		int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
		double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
		v += w * (Hash3(ix + dx, iy + dy, iz + dz, seed) & 0xFFFF) / 65535.0;
	}
	return v;
}
static double Tex(V3 p, double s0, double contrast, int seed)
{
	double v = 0.5 * VNoise(p * s0, seed) + 0.3 * VNoise(p * (s0 * 3.1), seed + 1) + 0.2 * VNoise(p * (s0 * 9.7), seed + 2);
	return 0.5 + (v - 0.5) * contrast;
}

struct Hit
{
	double t = 1e30;
	int obj = -1;
	V3 p;
};

struct Scene
{
	double contrast = 1;
	// returns the nearest hit of the ray o + t d (d unit), t > 1e-3
	Hit Cast(V3 o, V3 d) const
	{
		Hit h;
		auto plane = [&](int id, V3 n, double off) { // n . p = off
			double dn = Dot(n, d);
			if (fabs(dn) < 1e-9)
				return;
			double t = (off - Dot(n, o)) / dn;
			if (t > 1e-3 && t < h.t)
				h.t = t, h.obj = id, h.p = o + d * t;
		};
		auto box = [&](int id, V3 lo, V3 hi) {
			double t0 = 1e-3, t1 = h.t;
			const double od[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, l[3] = {lo.x, lo.y, lo.z}, u[3] = {hi.x, hi.y, hi.z};
			for (int a = 0; a < 3; a++) {
				if (fabs(dd[a]) < 1e-12) {
					if (od[a] < l[a] || od[a] > u[a])
						return;
					continue;
				}
				double ta = (l[a] - od[a]) / dd[a], tb = (u[a] - od[a]) / dd[a];
				if (ta > tb)
					std::swap(ta, tb);
				t0 = std::max(t0, ta), t1 = std::min(t1, tb);
				if (t0 > t1)
					return;
			}
			if (t0 < h.t)
				h.t = t0, h.obj = id, h.p = o + d * t0;
		};
		auto sphere = [&](int id, V3 c, double r) {
			V3 oc = o - c;
			double b = Dot(oc, d), cc = Dot(oc, oc) - r * r, disc = b * b - cc;
			if (disc < 0)
				return;
			double t = -b - sqrt(disc);
			if (t > 1e-3 && t < h.t)
				h.t = t, h.obj = id, h.p = o + d * t;
		};
		// world: x right, y up, -z forward; the head starts at the origin, 1.5 m above the floor
		plane(0, {0, 1, 0}, -1.5);               // floor
		plane(1, {0, 0, 1}, -3.5);               // far wall
		plane(2, {1, 0, 0}, -2.2);               // left wall
		plane(3, {1, 0, 0}, 2.6);                // right wall
		plane(4, {0, 0, 1}, 2.5);                // wall behind
		plane(5, {0, 1, 0}, 1.4);                // ceiling
		box(6, {-0.5, -0.80, -1.3}, {0.5, -0.74, -0.5});   // table top
		box(7, {-0.45, -1.5, -1.25}, {-0.39, -0.80, -1.19}); // table leg
		box(8, {-1.9, -1.5, -3.2}, {-1.3, 0.1, -2.5});     // cabinet
		sphere(9, {0.65, -0.45, -1.5}, 0.3);               // ball
		box(10, {1.2, -1.5, -2.2}, {1.7, -0.6, -1.7});     // box on the floor
		return h;
	}
	double Shade(const Hit &h) const
	{
		V3 p = h.p;
		double v;
		switch (h.obj) {
		case 0: v = Tex({p.x, 0, p.z}, 25, contrast, 1) * 0.55 + 0.15; break; // carpet
		case 1: v = Tex({p.x, p.y, 0}, 6, contrast * 0.5, 2) * 0.5 + 0.35; break; // painted wall
		case 2:
		case 3:
		case 4: v = Tex({p.z, p.y, p.x}, 5, contrast * 0.5, 3) * 0.5 + 0.3; break;
		case 5: v = 0.8 - 0.1 * Tex({p.x, 0, p.z}, 4, contrast, 4); break;
		case 6: v = Tex({p.x, 0, p.z}, 40, contrast, 5) * 0.6 + 0.25; break;
		case 7: v = 0.2 + 0.1 * Tex(p, 30, contrast, 6); break;
		case 8: v = Tex(p, 18, contrast, 7) * 0.5 + 0.2; break;
		case 9: v = Tex(p, 35, contrast, 8) * 0.7 + 0.2; break;
		case 10: v = Tex(p, 20, contrast, 9) * 0.6 + 0.25; break;
		default: v = 0.3;
		}
		// a poster on the far wall: strong texture
		if (h.obj == 1 && p.x > -0.8 && p.x < 0.4 && p.y > -0.2 && p.y < 0.8)
			v = Tex(p, 50, contrast * 1.6, 11) * 0.8 + 0.1;
		return std::max(0.0, std::min(1.0, v));
	}
};

struct Rng
{
	uint64_t s = 88172645463325252ull;
	double U()
	{
		s ^= s << 13, s ^= s >> 7, s ^= s << 17;
		return (s >> 11) * (1.0 / 9007199254740992.0);
	}
	double G() { return sqrt(-2 * log(U() + 1e-12)) * cos(6.2831853 * U()); }
};

static void RenderCam(const Scene &sc, const CamModel &c, const Pose &pose, double noise, double gain, std::vector<uint8_t> &img, Rng &rng)
{
	img.assign((size_t)c.w * c.h, 0);
	std::vector<float> lin((size_t)c.w * c.h, 0);
	V3 o = pose.Apply({c.t[0], c.t[1], c.t[2]});
	for (int y = 0; y < c.h; y++)
		for (int x = 0; x < c.w; x++) {
			double px = x, py = y; // centres at integers
			V3 dr = Unproject(c, px, py);
			double th = acos(std::max(-1.0, std::min(1.0, dr.z)));
			if (th > 1.35) { // lens edge: dark
				lin[(size_t)y * c.w + x] = 0.02f;
				continue;
			}
			V3 d = pose.Rot(DevDir(c, dr));
			Hit h = sc.Cast(o, d);
			float v = h.obj < 0 ? 0.3f : (float)sc.Shade(h);
			v *= (float)(1.0 - 0.45 * pow(th / 1.35, 2)); // vignetting
			lin[(size_t)y * c.w + x] = v * 230.0f * (float)gain;
		}
	// lens softness: [1 2 1] x [1 2 1], then sensor noise
	for (int y = 0; y < c.h; y++)
		for (int x = 0; x < c.w; x++) {
			float s = 0, w = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int xx = x + dx, yy = y + dy;
					if (xx < 0 || yy < 0 || xx >= c.w || yy >= c.h)
						continue;
					float k = (dx ? 1 : 2) * (dy ? 1 : 2);
					s += k * lin[(size_t)yy * c.w + xx], w += k;
				}
			double v = s / w + noise * rng.G();
			img[(size_t)y * c.w + x] = (uint8_t)std::max(0.0, std::min(255.0, v + 0.5));
		}
}

// ---- evaluation ----------------------------------------------------------------------------------------------------

struct Metrics
{
	int gtCells = 0, measured = 0, good = 0, all = 0, allGood = 0;
	std::vector<double> relErr, allErr;
	void Print(const char *name) const
	{
		auto pct = [](std::vector<double> v, double q) {
			if (v.empty())
				return 0.0;
			std::sort(v.begin(), v.end());
			return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
		};
		printf("  %-26s cells with GT %4d | measured %4d (%.0f%%): good %.0f%%, rel.err median %.1f%% p90 %.1f%% | all cells good %.0f%% median %.1f%% p90 %.1f%%\n",
		       name, gtCells, measured, gtCells ? 100.0 * measured / gtCells : 0.0, measured ? 100.0 * good / measured : 0.0,
		       100 * pct(relErr, 0.5), 100 * pct(relErr, 0.9), gtCells ? 100.0 * allGood / gtCells : 0.0, 100 * pct(allErr, 0.5),
		       100 * pct(allErr, 0.9));
	}
};

template <class GridT>
static void Score(Metrics &m, const Scene &sc, const Pose &pose, const GridT &g, int cells, std::function<bool(int)> measured)
{
	V3 o = pose.t;
	for (int i = 0; i < cells; i++) {
		const double *d = g.Direction(i);
		Hit h = sc.Cast(o, pose.Rot({d[0], d[1], d[2]}));
		if (h.obj < 0 || h.t < 0.3 || h.t > 6)
			continue;
		m.gtCells++;
		double rel = fabs(g.Depth(i) - h.t) / h.t;
		bool good = rel < 0.10;
		m.all++, m.allGood += good, m.allErr.push_back(rel);
		if (measured(i))
			m.measured++, m.good += good, m.relErr.push_back(rel);
	}
}

struct Rigs
{
	std::vector<CamModel> cams;
	StereoDenseRig bottom, top;
	bool haveTop = false;
	StereoDense grid;
	StereoDepth old;
	Rigs() : cams(4) {}
	bool Init(bool useTop)
	{
		for (int i = 0; i < 4; i++)
			cams[i].id = i;
		LoadCalibration(cams);
		for (auto &c : cams)
			if (!c.calibrated) {
				fprintf(stderr, "camera %d not calibrated (set QUEST1_CAMERA_CALIB)\n", c.id);
				return false;
			}
		StereoDenseRig::Params bp;
		if (getenv("DENSE_NEARCONF"))
			bp.nearMinConf = atoi(getenv("DENSE_NEARCONF"));
		if (getenv("DENSE_FARMAX"))
			bp.farDispMax = atoi(getenv("DENSE_FARMAX"));
		if (!ConfigureRig(bottom, 0, 2, bp))
			return false;
		if (useTop) {
			StereoDenseRig::Params tp;
			tp.rectW = 192, tp.focal = 100;
			haveTop = ConfigureRig(top, 1, 3, tp);
		}
		StereoDense::Params gp;
		grid.Configure(gp);
		grid.AddRig(&bottom);
		if (haveTop)
			grid.AddRig(&top);
		StereoDepth::Params op;
		return old.Configure(op, Projector(cams[0]), Projector(cams[2]), cams[0].w, cams[0].h);
	}
	bool ConfigureRig(StereoDenseRig &r, int l, int rr, const StereoDenseRig::Params &p)
	{
		if (cams[l].t[0] > cams[rr].t[0])
			std::swap(l, rr);
		const CamModel &a = cams[l], &b = cams[rr];
		double va[3] = {a.R[0][2], a.R[1][2], a.R[2][2]}, vb[3] = {b.R[0][2], b.R[1][2], b.R[2][2]};
		double v[3] = {va[0] + vb[0], va[1] + vb[1], va[2] + vb[2]};
		bool ok = r.Configure(p, Projector(a), Projector(b), a.t, b.t, v, a.w, a.h);
		fprintf(stderr, "rig %d/%d: %s baseline %.3f D %d\n", l, rr, ok ? "ok" : "FAILED", r.Baseline(), r.MaxDisparity());
		return ok;
	}
};

// the synthetic floor (world y = -1.5) as a plane in the device frame: n . p + d = 0
static void FloorPlane(const Pose &pose, double plane[4])
{
	plane[0] = pose.R[3], plane[1] = pose.R[4], plane[2] = pose.R[5], plane[3] = pose.t.y + 1.5;
}

static void WritePGM(const char *dir, const char *name, const uint8_t *d, int w, int h)
{
	if (!dir)
		return;
	std::string p = std::string(dir) + "/" + name + ".pgm";
	FILE *f = fopen(p.c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	fwrite(d, 1, (size_t)w * h, f);
	fclose(f);
}
static void WriteDisp(const char *dir, const char *name, const StereoDenseRig &r)
{
	if (!dir)
		return;
	int W = r.Width(), H = r.Height();
	std::vector<uint8_t> rgb((size_t)W * H * 3);
	for (int i = 0; i < W * H; i++) {
		uint16_t dq = r.Disparity()[i];
		uint8_t g = r.RectifiedLeft()[i];
		if (!dq) {
			rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = g / 2;
			continue;
		}
		double t = std::min(1.0, dq / 16.0 / std::max(1, r.MaxDisparity()));
		rgb[i * 3] = (uint8_t)(255 * std::min(1.0, 2 * t)), rgb[i * 3 + 1] = (uint8_t)(255 * (1 - fabs(2 * t - 1))),
		rgb[i * 3 + 2] = (uint8_t)(255 * std::max(0.0, 1 - 2 * t));
	}
	std::string p = std::string(dir) + "/" + name + "_disp.ppm";
	FILE *f = fopen(p.c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
	WritePGM(dir, (std::string(name) + "_rectL").c_str(), r.RectifiedLeft(), W, H);
	WritePGM(dir, (std::string(name) + "_rectR").c_str(), r.RectifiedRight(), W, H);
}

static void DiscardPrintRig(const char *tag, const StereoDenseRig &r)
{
	const StereoDenseRig::Stats &s = r.LastStats();
	printf("  %s: inside %d, matched %d (%.0f%% of inside), ms: rectify %.2f census %.2f match %.2f filter %.2f\n", tag, s.pixels, s.matched,
	       s.pixels ? 100.0 * s.matched / s.pixels : 0.0, s.msRectify, s.msFarMatch, s.msNearMatch, s.msFilter);
}

static int RunSynth(int sceneId, double noise, double contrast)
{
	Rigs R;
	if (!R.Init(true))
		return 1;
	const char *out = getenv("DENSE_OUT");
	Scene sc;
	sc.contrast = contrast;
	Rng rng;
	Pose pose;
	pose.t = {0, 0, 0};
	if (sceneId == 1)
		pose = YawPose(35, {0.3, -0.1, 0.2});
	std::vector<uint8_t> img[4];
	for (int i = 0; i < 4; i++)
		RenderCam(sc, R.cams[i], pose, noise, i == 2 || i == 3 ? 0.92 : 1.0, img[i], rng);
	if (out)
		for (int i = 0; i < 4; i++)
			WritePGM(out, ("cam" + std::to_string(i)).c_str(), img[i].data(), 640, 480);
	R.bottom.Compute(img[0].data(), img[2].data(), 640);
	R.top.Compute(img[1].data(), img[3].data(), 640);
	R.grid.SetPose(nullptr);
	double plane[4];
	FloorPlane(pose, plane);
	if (getenv("DENSE_FLOOR"))
		R.grid.SetFloor(plane);
	R.grid.Update();
	R.old.Compute(img[0].data(), img[2].data(), 640);
	printf("synthetic scene %d, noise %.1f, contrast %.2f\n", sceneId, noise, contrast);
	DiscardPrintRig("bottom rig", R.bottom);
	DiscardPrintRig("top rig   ", R.top);
	Metrics mo, mn, mb;
	Score(mo, sc, pose, R.old, R.old.Cols() * R.old.Rows(), [&](int i) { return R.old.Confidence(i) > 0; });
	Score(mn, sc, pose, R.grid, R.grid.Cols() * R.grid.Rows(), [&](int i) { return R.grid.CellSource(i) == StereoDense::kMeasured || R.grid.CellSource(i) == StereoDense::kFused; });
	mo.Print("old plane sweep (32x24)");
	mn.Print("dense, both rigs (64x48)");
	// bottom rig alone for comparison
	{
		StereoDense g1;
		StereoDense::Params gp;
		g1.Configure(gp);
		g1.AddRig(&R.bottom);
		if (getenv("DENSE_FLOOR"))
			g1.SetFloor(plane);
		g1.Update();
		Score(mb, sc, pose, g1, g1.Cols() * g1.Rows(), [&](int i) { return g1.CellSource(i) == StereoDense::kMeasured; });
		mb.Print("dense, bottom rig only");
	}
	WriteDisp(out, "bottom", R.bottom);
	WriteDisp(out, "top", R.top);
	return 0;
}

static int RunSeq(double noise, double contrast)
{
	Rigs R;
	if (!R.Init(true))
		return 1;
	Scene sc;
	sc.contrast = contrast;
	Rng rng;
	StereoDense fused, plain;
	StereoDense::Params gp;
	if (getenv("DENSE_MAXAGE"))
		gp.maxAge = atoi(getenv("DENSE_MAXAGE"));
	if (getenv("DENSE_GATE"))
		gp.fuseGate = atof(getenv("DENSE_GATE"));
	if (getenv("DENSE_PNOISE"))
		gp.processNoise = atof(getenv("DENSE_PNOISE"));
	fused.Configure(gp), plain.Configure(gp);
	fused.AddRig(&R.bottom), fused.AddRig(&R.top), plain.AddRig(&R.bottom), plain.AddRig(&R.top);
	Metrics mf, mp, m1f, m1p;
	Metrics mfm, mff; // fused run: measured-only cells, fused (carried or filtered) cells
	std::vector<uint8_t> img[4];
	for (int f = 0; f < 45; f++) {
		// 1.5 s at 30 Hz: the head moves 0.4 m/s sideways and turns 25 deg/s
		double t = f / 30.0;
		Pose pose = YawPose(25 * t, {0.4 * t, 0.02 * sin(6 * t), -0.2 * t});
		for (int i = 0; i < 4; i++)
			RenderCam(sc, R.cams[i], pose, noise, i == 2 || i == 3 ? 0.92 : 1.0, img[i], rng);
		R.bottom.Compute(img[0].data(), img[2].data(), 640);
		R.top.Compute(img[1].data(), img[3].data(), 640);
		double m[12];
		pose.M34(m);
		fused.SetPose(m);
		plain.SetPose(nullptr);
		if (getenv("DENSE_FLOOR")) {
			double plane[4];
			FloorPlane(pose, plane);
			fused.SetFloor(plane), plain.SetFloor(plane);
		}
		fused.Update();
		plain.Update();
		if (f >= 20) {
			auto ms = [&](const StereoDense &g, int i) { return g.CellSource(i) == StereoDense::kMeasured || g.CellSource(i) == StereoDense::kFused; };
			Score(mf, sc, pose, fused, fused.Cols() * fused.Rows(), [&](int i) { return ms(fused, i); });
			Score(mp, sc, pose, plain, plain.Cols() * plain.Rows(), [&](int i) { return ms(plain, i); });
			Score(mfm, sc, pose, fused, fused.Cols() * fused.Rows(), [&](int i) { return fused.CellSource(i) == StereoDense::kMeasured; });
			Score(mff, sc, pose, fused, fused.Cols() * fused.Rows(), [&](int i) { return fused.CellSource(i) == StereoDense::kFused; });
		}
	}
	printf("sequence (frames 20..44 of a moving head)\n");
	mp.Print("dense, per frame");
	mf.Print("dense + temporal fusion");
	mfm.Print("  of which fresh only");
	mff.Print("  of which fused/carried");
	return 0;
}

static int RunReal(const char *ringFile, int slotIdx)
{
	Rigs R;
	if (!R.Init(true))
		return 1;
	FILE *f = fopen(ringFile, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", ringFile);
		return 1;
	}
	std::vector<uint8_t> data;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	fclose(f);
	const q1cam_ring_header *hdr = (const q1cam_ring_header *)data.data();
	if (hdr->magic != Q1CAM_RING_MAGIC || slotIdx >= (int)hdr->slot_count)
		return 1;
	const q1cam_ring_slot &s = hdr->slot[slotIdx];
	const uint8_t *px = data.data() + hdr->header_size + (size_t)slotIdx * hdr->slot_bytes;
	std::vector<uint8_t> img[4];
	for (uint32_t c = 0; c < s.ncams && c < 4; c++)
		if (s.cam[c].present && s.cam[c].cam_id < 4)
			img[s.cam[c].cam_id].assign(px + s.cam[c].offset, px + s.cam[c].offset + 640 * 480);
	for (int i = 0; i < 4; i++)
		if (img[i].empty()) {
			fprintf(stderr, "camera %d missing in slot\n", i);
			return 1;
		}
	const char *out = getenv("DENSE_OUT");
	R.bottom.Compute(img[0].data(), img[2].data(), 640);
	R.top.Compute(img[1].data(), img[3].data(), 640);
	R.grid.Update();
	R.old.Compute(img[0].data(), img[2].data(), 640);
	printf("real frames, slot %d\n", slotIdx);
	DiscardPrintRig("bottom rig", R.bottom);
	DiscardPrintRig("top rig   ", R.top);
	int c = 0, cells = R.grid.Cols() * R.grid.Rows(), meas = 0;
	for (int i = 0; i < cells; i++)
		meas += R.grid.CellSource(i) == StereoDense::kMeasured, c += R.grid.Confidence(i) > 0;
	int oc = 0;
	for (int i = 0; i < R.old.Cols() * R.old.Rows(); i++)
		oc += R.old.Confidence(i) > 0;
	printf("  dense grid: %d of %d cells measured (%.0f%%); old sweep: %d of %d confident (%.0f%%)\n", meas, cells, 100.0 * meas / cells, oc,
	       R.old.Cols() * R.old.Rows(), 100.0 * oc / (R.old.Cols() * R.old.Rows()));
	// depth histogram of the dense grid's measured cells
	int hist[8] = {};
	static const double lim[7] = {0.4, 0.6, 0.9, 1.3, 2.0, 3.0, 5.0};
	for (int i = 0; i < cells; i++)
		if (R.grid.CellSource(i) == StereoDense::kMeasured) {
			int b = 0;
			while (b < 7 && R.grid.Depth(i) >= lim[b])
				b++;
			hist[b]++;
		}
	printf("  measured depth histogram (<0.4 .6 .9 1.3 2 3 5 >5 m): %d %d %d %d %d %d %d %d\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
	for (int i = 0; i < 4; i++)
		WritePGM(out, ("realcam" + std::to_string(i)).c_str(), img[i].data(), 640, 480);
	WriteDisp(out, "real_bottom", R.bottom);
	WriteDisp(out, "real_top", R.top);
	return 0;
}

// compute only: n x one rig on frames from a ring copy (for instruction counting under qemu: see the notes)
static int RunCount(const char *ringFile, int n, char which)
{
	Rigs R;
	if (!R.Init(true))
		return 1;
	FILE *f = fopen(ringFile, "rb");
	if (!f)
		return 1;
	std::vector<uint8_t> data;
	uint8_t buf[65536];
	size_t k;
	while ((k = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + k);
	fclose(f);
	const q1cam_ring_header *hdr = (const q1cam_ring_header *)data.data();
	const q1cam_ring_slot &s = hdr->slot[0];
	const uint8_t *px = data.data() + hdr->header_size;
	std::vector<uint8_t> img[4];
	for (uint32_t c = 0; c < s.ncams && c < 4; c++)
		img[s.cam[c].cam_id].assign(px + s.cam[c].offset, px + s.cam[c].offset + 640 * 480);
	fprintf(stderr, "START\n");
	for (int i = 0; i < n; i++) {
		if (which == 'b')
			R.bottom.Compute(img[0].data(), img[2].data(), 640);
		else if (which == 't')
			R.top.Compute(img[1].data(), img[3].data(), 640);
		else
			R.grid.Update();
	}
	fprintf(stderr, "DONE\n");
	return 0;
}

static int RunBench(int n)
{
	Rigs R;
	if (!R.Init(true))
		return 1;
	Scene sc;
	Rng rng;
	std::vector<uint8_t> img[4];
	Pose pose;
	for (int i = 0; i < 4; i++)
		RenderCam(sc, R.cams[i], pose, 2.0, 1.0, img[i], rng);
	double tb = 0, tt = 0, tg = 0, to = 0, st[4] = {0, 0, 0, 0};
	for (int i = 0; i < n + 5; i++) {
		double a = NowMs();
		R.bottom.Compute(img[0].data(), img[2].data(), 640);
		double b = NowMs();
		R.top.Compute(img[1].data(), img[3].data(), 640);
		double c = NowMs();
		R.grid.Update();
		double d = NowMs();
		R.old.Compute(img[0].data(), img[2].data(), 640);
		double e = NowMs();
		if (i >= 5) {
			tb += b - a, tt += c - b, tg += d - c, to += e - d;
			const StereoDenseRig::Stats &s = R.bottom.LastStats();
			st[0] += s.msRectify, st[1] += s.msFarMatch, st[2] += s.msNearMatch, st[3] += s.msFilter;
		}
	}
	printf("bench over %d frames (this machine, not the headset): bottom rig %.2f ms (rectify %.2f far %.2f near %.2f filter %.2f), top rig %.2f ms, "
	       "grid update %.2f ms, old plane sweep %.2f ms\n",
	       n, tb / n, st[0] / n, st[1] / n, st[2] / n, st[3] / n, tt / n, tg / n, to / n);
	return 0;
}

int main(int argc, char **argv)
{
	std::string mode = argc > 1 ? argv[1] : "synth";
	if (mode == "synth")
		return RunSynth(argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? atof(argv[3]) : 2.0, argc > 4 ? atof(argv[4]) : 1.0);
	if (mode == "seq")
		return RunSeq(argc > 2 ? atof(argv[2]) : 2.0, argc > 3 ? atof(argv[3]) : 1.0);
	if (mode == "real" && argc > 2)
		return RunReal(argv[2], argc > 3 ? atoi(argv[3]) : 0);
	if (mode == "count" && argc > 3)
		return RunCount(argv[2], atoi(argv[3]), argc > 4 ? argv[4][0] : 'b');
	if (mode == "bench")
		return RunBench(argc > 2 ? atoi(argv[2]) : 200);
	fprintf(stderr, "usage: dense_eval synth [scene noise contrast] | seq | real <ring> [slot] | bench [n]\n");
	return 1;
}
