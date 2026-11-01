// Copy of camera.cpp's calibration reader and camera model (Json, CamModel, LoadCalibration), for
// client programs that cannot include the driver headers. Keep in sync with camera.cpp.
#pragma once
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <openvr.h>
using vr::HmdMatrix34_t;
#define CLog(...) fprintf(stderr, __VA_ARGS__)
static const char *Env(const char *name, const char *def = nullptr)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

// --- Minimal JSON reader (enough for camera_calibration_v2.json) ------------------------------------------

struct Json
{
	enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
	double num = 0;
	std::string str;
	std::vector<Json> arr;
	std::map<std::string, Json> obj;

	const Json *Get(const char *key) const
	{
		if (type != Obj)
			return nullptr;
		auto it = obj.find(key);
		return it == obj.end() ? nullptr : &it->second;
	}
	bool Numbers(double *out, size_t n) const
	{
		if (type != Arr || arr.size() < n)
			return false;
		for (size_t i = 0; i < n; i++) {
			if (arr[i].type != Num)
				return false;
			out[i] = arr[i].num;
		}
		return true;
	}
};

struct JsonParser
{
	const char *p, *end;
	int depth = 0;

	void Ws()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}
	bool String(std::string &out)
	{
		if (p >= end || *p != '"')
			return false;
		p++;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end) {
				p++;
				char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; // \uXXXX kept as 'u' + digits
				out += c;
			} else {
				out += *p;
			}
			p++;
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}
	bool Value(Json &v)
	{
		if (++depth > 32)
			return false;
		Ws();
		if (p >= end)
			return false;
		bool ok = true;
		if (*p == '{') {
			v.type = Json::Obj;
			p++;
			Ws();
			if (p < end && *p == '}') {
				p++;
			} else {
				for (;;) {
					Ws();
					std::string key;
					if (!String(key))
						return false;
					Ws();
					if (p >= end || *p != ':')
						return false;
					p++;
					if (!Value(v.obj[key]))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == '}') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '[') {
			v.type = Json::Arr;
			p++;
			Ws();
			if (p < end && *p == ']') {
				p++;
			} else {
				for (;;) {
					v.arr.emplace_back();
					if (!Value(v.arr.back()))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == ']') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '"') {
			v.type = Json::Str;
			ok = String(v.str);
		} else if (!strncmp(p, "true", std::min<size_t>(4, end - p)) && end - p >= 4) {
			v.type = Json::Bool, v.num = 1, p += 4;
		} else if (!strncmp(p, "false", std::min<size_t>(5, end - p)) && end - p >= 5) {
			v.type = Json::Bool, p += 5;
		} else if (!strncmp(p, "null", std::min<size_t>(4, end - p)) && end - p >= 4) {
			p += 4;
		} else {
			std::string tmp(p, std::min<size_t>(64, end - p));
			char *e = nullptr;
			v.type = Json::Num;
			v.num = strtod(tmp.c_str(), &e);
			if (e == tmp.c_str())
				return false;
			p += e - tmp.c_str();
		}
		depth--;
		return ok;
	}
};

bool ParseJsonFile(const char *path, Json &out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (1u << 20))
		s.append(buf, n);
	fclose(f);
	JsonParser jp{s.data(), s.data() + s.size()};
	return jp.Value(out);
}

// --- Camera model ------------------------------------------------------------------------------------------

//! One tracking camera: factory calibration (raw sensor orientation, OpenCV axes: x right, y down,
//! z forward) and the quarter turns that make its image upright.
struct CamModel
{
	int id = 0;
	bool calibrated = false;
	int w = 640, h = 480;                  // raw image
	double f = 191, cx = 319.5, cy = 239.5; // PinholeSymmetric
	double k[6] = {0.29, 0, -0.15, 0.09, -0.02, 0}; // Fisheye62 radial (typical values when uncalibrated)
	double p[2] = {0, 0};                  // Fisheye62 tangential
	double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // DeviceFromCamera rotation (raw camera axes)
	double t[3] = {0, 0, 0};
	int rot = 3; // quarter turns clockwise applied to the raw image (3 = 90° counter-clockwise)

	int RotW() const { return rot & 1 ? h : w; }
	int RotH() const { return rot & 1 ? w : h; }

	//! rotated-camera direction -> raw-camera direction (inverse of the image rotation)
	void RotToRaw(const double in[3], double out[3]) const
	{
		double x = in[0], y = in[1];
		switch (rot & 3) {
		case 0: out[0] = x, out[1] = y; break;
		case 1: out[0] = y, out[1] = -x; break;  // rotated (xr, yr) = (-y, x)
		case 2: out[0] = -x, out[1] = -y; break;
		case 3: out[0] = -y, out[1] = x; break;  // rotated (xr, yr) = (y, -x)
		}
		out[2] = in[2];
	}
	//! raw pixel -> rotated pixel (continuous coordinates, pixel centres at +0.5)
	void RawToRotPx(double x, double y, double &xr, double &yr) const
	{
		switch (rot & 3) {
		case 0: xr = x, yr = y; break;
		case 1: xr = h - y, yr = x; break;
		case 2: xr = w - x, yr = h - y; break;
		case 3: xr = y, yr = w - x; break;
		}
	}
	//! principal point in the rotated image
	void RotCenter(double &rcx, double &rcy) const { RawToRotPx(cx + 0.5, cy + 0.5, rcx, rcy), rcx -= 0.5, rcy -= 0.5; }

	//! Fisheye62 projection of a raw-camera direction to a raw pixel. False behind the camera.
	bool Project(const double d[3], double &px, double &py) const
	{
		double r = sqrt(d[0] * d[0] + d[1] * d[1]);
		double th = atan2(r, d[2]);
		if (th > 1.6) // ~92°: outside any useful field of view
			return false;
		double t2 = th * th, poly = 1, tp = 1;
		for (int i = 0; i < 6; i++) {
			tp *= t2;
			poly += k[i] * tp;
		}
		double thd = th * poly;
		double u = r > 1e-12 ? thd * d[0] / r : 0, v = r > 1e-12 ? thd * d[1] / r : 0;
		double r2 = u * u + v * v;
		double ut = u + 2 * p[0] * u * v + p[1] * (r2 + 2 * u * u);
		double vt = v + 2 * p[1] * u * v + p[0] * (r2 + 2 * v * v);
		px = f * ut + cx;
		py = f * vt + cy;
		return true;
	}

	//! HeadFromCamera with OpenVR camera axes (x right, y up, looking down -z) of the rotated image.
	HmdMatrix34_t HeadFromCamera() const
	{
		// columns: rotated camera axes expressed in raw camera axes, then in the device frame
		HmdMatrix34_t m{};
		const double axes[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}; // OpenVR camera axes in OpenCV terms
		for (int c = 0; c < 3; c++) {
			double raw[3];
			RotToRaw(axes[c], raw);
			for (int r = 0; r < 3; r++)
				m.m[r][c] = (float)(R[r][0] * raw[0] + R[r][1] * raw[1] + R[r][2] * raw[2]);
		}
		for (int r = 0; r < 3; r++)
			m.m[r][3] = (float)t[r];
		return m;
	}
};

//! Chooses the quarter turn whose image "up" points closest to the device's +y (up).
int UprightRotation(const CamModel &c)
{
	int best = 0;
	double bestUp = -2;
	CamModel tmp = c;
	for (int r = 0; r < 4; r++) {
		tmp.rot = r;
		double up[3] = {0, -1, 0}, raw[3];
		tmp.RotToRaw(up, raw);
		double devUpY = c.R[1][0] * raw[0] + c.R[1][1] * raw[1] + c.R[1][2] * raw[2];
		if (devUpY > bestUp)
			bestUp = devUpY, best = r;
	}
	return best;
}

void LoadCalibration(std::vector<CamModel> &cams)
{
	const char *paths[] = {Env("QUEST1_CAMERA_CALIB"), "/persist/calibration/camera_calibration_v2.json",
	                       "/run/q1track/calibration/camera_calibration_v2.json"};
	Json root;
	const char *used = nullptr;
	for (const char *path : paths)
		if (path && ParseJsonFile(path, root)) {
			used = path;
			break;
		}
	const Json *list = used ? root.Get("CameraCalibration") : nullptr;
	if (!list || list->type != Json::Arr) {
		CLog("quest1 camera: no factory calibration readable, using defaults\n");
		return;
	}
	for (CamModel &c : cams) {
		for (const Json &e : list->arr) {
			const Json *id = e.Get("Id");
			int eid = !id ? -1 : id->type == Json::Num ? (int)id->num : id->type == Json::Str ? atoi(id->str.c_str()) : -1;
			if (eid != c.id)
				continue;
			double size[2], m[16], proj[3], dist[8];
			const Json *pj = e.Get("Projection"), *dj = e.Get("Distortion");
			const Json *pc = pj ? pj->Get("Coefficients") : nullptr, *dc = dj ? dj->Get("Coefficients") : nullptr;
			const Json *sz = e.Get("ImageSize"), *dfc = e.Get("DeviceFromCamera");
			if (!sz || !dfc || !pc || !dc || !sz->Numbers(size, 2) || !dfc->Numbers(m, 16) || !pc->Numbers(proj, 3) ||
			    !dc->Numbers(dist, 8))
				break;
			c.w = (int)size[0], c.h = (int)size[1];
			c.f = proj[0], c.cx = proj[1], c.cy = proj[2];
			for (int i = 0; i < 6; i++)
				c.k[i] = dist[i];
			c.p[0] = dist[6], c.p[1] = dist[7];
			for (int r = 0; r < 3; r++) {
				for (int col = 0; col < 3; col++)
					c.R[r][col] = m[r * 4 + col];
				c.t[r] = m[r * 4 + 3];
			}
			c.calibrated = true;
			c.rot = UprightRotation(c);
			break;
		}
		CLog("quest1 camera: cam %d %s, %dx%d f %.1f, image turned %d x 90 deg clockwise\n", c.id,
		     c.calibrated ? "calibrated" : "NOT in the calibration file", c.w, c.h, c.f, c.rot);
	}
}

