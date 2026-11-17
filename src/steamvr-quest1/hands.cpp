// Meta's hand tracking for driver_quest1: see hands.h and docs/hand-tracking.md.
#include "hands.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../q1track/q1hands_fk.h"
#include "q1json.h"

using namespace q1hands;

namespace q1hands {
namespace {

// OpenVR hand skeleton (eBone_*): 0 Root, 1 Wrist, 2-5 Thumb0-3, 6-10 Index0-4, 11-15 Middle0-4, 16-20 Ring0-4,
// 21-25 Pinky0-4, 26-30 the aux bones (thumb, index, middle, ring, pinky). The i-th bone is node i+1 of Valve's
// vr_glove_*_skeleton.glb, which gives the bind pose.
constexpr int B_ROOT = 0, B_WRIST = 1, B_AUX = 26;

// Meta joint behind each OpenVR bone (docs/hand-tracking.md): -1 = no joint of Meta's has it (the finger metacarpals
// of index, middle, ring and the root: they follow the wrist)
const int kMetaOf[kBones] = {-1, 0,                    // root, wrist
                             3, 4, 5, 19,              // thumb: metacarpal, proximal, distal, tip
                             -1, 6, 7, 8, 20,          // index
                             -1, 9, 10, 11, 21,        // middle
                             -1, 12, 13, 14, 22,       // ring
                             15, 16, 17, 18, 23,       // pinky
                             5, 8, 11, 14, 18};        // aux: the distal joints
// the end bone each aux bone sits at
const int kAuxEnd[5] = {5, 10, 15, 20, 25};

struct Valve
{
	bool loaded = false;
	int parent[kBones];
	M3 Rb[kBones];       // bind world rotation
	double Pb[kBones][3]; // bind world position
	M3 Lb[kBones];       // bind parent-local rotation
	double Lt[kBones][3]; // bind parent-local translation
	double palm[3];      // palm normal in the skeleton's model space
};

struct Retarget
{
	bool ready = false;
	M3 M; // Meta's wrist frame to the skeleton's model space
	M3 Mt;
};

Valve g_valve[2];
Retarget g_ret[2];

M3 Axis(const double *a, double ang)
{
	double c = cos(ang), s = sin(ang), t = 1 - c;
	return {{t * a[0] * a[0] + c, t * a[0] * a[1] - s * a[2], t * a[0] * a[2] + s * a[1], t * a[0] * a[1] + s * a[2],
	         t * a[1] * a[1] + c, t * a[1] * a[2] - s * a[0], t * a[0] * a[2] - s * a[1], t * a[1] * a[2] + s * a[0],
	         t * a[2] * a[2] + c}};
}

void Norm(double *v)
{
	double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (n > 1e-12)
		v[0] /= n, v[1] /= n, v[2] /= n;
}
void Cross(const double *a, const double *b, double *o)
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}
double Dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

//! Orthonormal frame (columns: fingers, thumb side, palm-out-or-back) from the wrist and three knuckles.
M3 HandFrame(const double *wrist, const double *index, const double *middle, const double *pinky)
{
	double f[3], s[3], n[3];
	for (int k = 0; k < 3; k++)
		f[k] = middle[k] - wrist[k], s[k] = index[k] - pinky[k];
	Norm(f);
	double d = Dot(s, f);
	for (int k = 0; k < 3; k++)
		s[k] -= d * f[k];
	Norm(s);
	Cross(f, s, n);
	return {{f[0], s[0], n[0], f[1], s[1], n[1], f[2], s[2], n[2]}};
}

bool LoadValve(int h)
{
	Valve &v = g_valve[h];
	if (v.loaded)
		return true;
	static double lastTry[2];
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = ts.tv_sec + ts.tv_nsec * 1e-9;
	if (now - lastTry[h] < 5)
		return false;
	lastTry[h] = now;
	const char *side = h ? "right" : "left";
	std::string path = std::string("/opt/steamvr/resources/skeletons/vr_glove_") + side + "_skeleton.glb";
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<char> buf(1 << 16);
	size_t n = fread(buf.data(), 1, buf.size(), f);
	fclose(f);
	uint32_t len = 0;
	if (n < 20 || memcmp(buf.data(), "glTF", 4) || (memcpy(&len, buf.data() + 12, 4), 20 + (size_t)len > n))
		return false;
	q1json::Json j;
	q1json::JsonParser jp{buf.data() + 20, buf.data() + 20 + len};
	const q1json::Json *nodes;
	if (!jp.Value(j) || !(nodes = j.Get("nodes")) || nodes->arr.size() < kBones + 1)
		return false;
	// glTF nodes: world transform of each, node i+1 is bone i
	size_t nn = nodes->arr.size();
	std::vector<int> par(nn, -1);
	for (size_t i = 0; i < nn; i++)
		if (const q1json::Json *c = nodes->arr[i].Get("children"))
			for (const auto &e : c->arr)
				if (e.num >= 0 && e.num < (double)nn)
					par[(size_t)e.num] = (int)i;
	std::vector<M3> R(nn);
	std::vector<double> S(nn, 1.0);
	std::vector<std::vector<double>> P(nn, std::vector<double>(3, 0.0));
	std::vector<bool> done(nn, false);
	for (size_t pass = 0; pass < nn; pass++)
		for (size_t i = 0; i < nn; i++) {
			if (done[i] || (par[i] >= 0 && !done[(size_t)par[i]]))
				continue;
			double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
			float qf[4];
			const q1json::Json *x;
			if ((x = nodes->arr[i].Get("translation")))
				x->Numbers(t, 3);
			if ((x = nodes->arr[i].Get("rotation")))
				x->Numbers(q, 4);
			double sc = 1;
			if ((x = nodes->arr[i].Get("scale")) && x->arr.size() == 3)
				sc = x->arr[0].num;
			for (int k = 0; k < 4; k++)
				qf[k] = (float)q[k];
			M3 lr = FromQuatXyzw(qf);
			if (par[i] < 0) {
				R[i] = lr;
				S[i] = sc;
				P[i] = {t[0], t[1], t[2]};
			} else {
				size_t p = (size_t)par[i];
				double tp[3] = {t[0] * S[p], t[1] * S[p], t[2] * S[p]}, d[3];
				MulV(R[p], tp, d);
				R[i] = Mul(R[p], lr);
				S[i] = S[p] * sc;
				P[i] = {P[p][0] + d[0], P[p][1] + d[1], P[p][2] + d[2]};
			}
			done[i] = true;
		}
	for (int b = 0; b < kBones; b++) {
		size_t node = (size_t)b + 1;
		v.parent[b] = par[node] >= 1 ? par[node] - 1 : -1;
		v.Rb[b] = R[node];
		for (int k = 0; k < 3; k++)
			v.Pb[b][k] = P[node][k];
	}
	for (int b = 0; b < kBones; b++) {
		int p = v.parent[b];
		if (p < 0) {
			v.Lb[b] = v.Rb[b];
			for (int k = 0; k < 3; k++)
				v.Lt[b][k] = v.Pb[b][k];
		} else {
			v.Lb[b] = Mul(Transpose(v.Rb[p]), v.Rb[b]);
			double d[3] = {v.Pb[b][0] - v.Pb[p][0], v.Pb[b][1] - v.Pb[p][1], v.Pb[b][2] - v.Pb[p][2]};
			MulV(Transpose(v.Rb[p]), d, v.Lt[b]);
		}
	}
	// the palm's side: n = -(fingers x thumb side) for a right hand, + for a left (docs/hand-tracking.md)
	M3 F = HandFrame(v.Pb[B_WRIST], v.Pb[7], v.Pb[12], v.Pb[22]);
	double fx[3] = {F.m[0], F.m[3], F.m[6]}, sx[3] = {F.m[1], F.m[4], F.m[7]};
	Cross(fx, sx, v.palm);
	double sg = h ? -1.0 : 1.0;
	for (int k = 0; k < 3; k++)
		v.palm[k] *= sg;
	v.loaded = true;
	fprintf(stderr, "quest1: hands: %s skeleton loaded from %s\n", side, path.c_str());
	return true;
}

//! the bones' parent-space transforms from their model-space (world) rotations and positions
void Assemble(const Valve &v, const M3 *Rv, double (*Pv)[3], vr::VRBoneTransform_t *out)
{
	for (int b = 0; b < kBones; b++) {
		int p = v.parent[b];
		double q[4], t[3] = {Pv[b][0], Pv[b][1], Pv[b][2]};
		M3 lr = Rv[b];
		if (p >= 0) {
			double d[3] = {Pv[b][0] - Pv[p][0], Pv[b][1] - Pv[p][1], Pv[b][2] - Pv[p][2]};
			MulV(Transpose(Rv[p]), d, t);
			lr = Mul(Transpose(Rv[p]), Rv[b]);
		}
		ToQuatWxyz(lr, q);
		out[b].position.v[0] = (float)t[0];
		out[b].position.v[1] = (float)t[1];
		out[b].position.v[2] = (float)t[2];
		out[b].position.v[3] = 1.0f;
		out[b].orientation.w = (float)q[0];
		out[b].orientation.x = (float)q[1];
		out[b].orientation.y = (float)q[2];
		out[b].orientation.z = (float)q[3];
	}
}

//! the aux bones sit at the finger ends and turn with the finger's last bone
void PlaceAux(const Valve &v, M3 *Rv, double (*Pv)[3])
{
	for (int a = 0; a < 5; a++) {
		int e = kAuxEnd[a], x = B_AUX + a;
		M3 D = Mul(Rv[e], Transpose(v.Rb[e]));
		double d[3] = {v.Pb[x][0] - v.Pb[e][0], v.Pb[x][1] - v.Pb[e][1], v.Pb[x][2] - v.Pb[e][2]}, r[3];
		MulV(D, d, r);
		Rv[x] = Mul(D, v.Rb[x]);
		for (int k = 0; k < 3; k++)
			Pv[x][k] = Pv[e][k] + r[k];
	}
}

// ---- the shared memory ----
const q1hands_shm *g_shm = nullptr;

double MonoNow()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

const q1hands_shm *Shm()
{
	static double lastTry;
	if (g_shm)
		return g_shm;
	double now = MonoNow();
	if (now - lastTry < 1.0)
		return nullptr;
	lastTry = now;
	int fd = open(Q1HANDS_SHM_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return nullptr;
	struct stat st;
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(q1hands_shm)) {
		close(fd);
		return nullptr;
	}
	void *m = mmap(nullptr, sizeof(q1hands_shm), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return nullptr;
	g_shm = (const q1hands_shm *)m;
	fprintf(stderr, "quest1: hands: %s mapped\n", Q1HANDS_SHM_PATH);
	return g_shm;
}

bool EnsureRetarget(int h, const HandPose &hp)
{
	Retarget &r = g_ret[h];
	if (r.ready)
		return true;
	if (!LoadValve(h))
		return false;
	const Valve &v = g_valve[h];
	// Meta's bind frame: wrist, index/middle/pinky knuckle joints 6, 9, 16
	M3 Fm = HandFrame(hp.Pbind[0], hp.Pbind[6], hp.Pbind[9], hp.Pbind[16]);
	M3 Fv = HandFrame(v.Pb[B_WRIST], v.Pb[7], v.Pb[12], v.Pb[22]);
	r.M = Mul(Fv, Transpose(Fm));
	r.Mt = Transpose(r.M);
	r.ready = true;
	fprintf(stderr, "quest1: hands: %s retargeting ready\n", h ? "right" : "left");
	return true;
}

} // namespace

bool Read(int h, Frame *f)
{
	*f = Frame();
	const q1hands_shm *m = Shm();
	if (!m)
		return false;
	static q1hands_shm s[2]; // one copy per hand (each hand has its own caller thread state in practice: one thread)
	q1hands_shm &c = s[h];
	if (!Copy(m, &c))
		return false;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
	if (now - c.update_ns > 300000000LL)
		return false;
	f->valid = true;
	f->handsMode = c.mode == Q1HANDS_MODE_HANDS && c.hands_enabled;
	f->controllerInHand = c.controller_in_hand[h] != 0;
	f->controllerConnected = c.controller_connected[h] != 0;
	f->headValid = c.head_valid != 0;
	f->headRot[0] = c.head_rot[3];
	for (int k = 0; k < 3; k++)
		f->headRot[k + 1] = c.head_rot[k], f->headPos[k] = c.head_pos[k];
	const q1hands_hand &hd = c.hand[h];
	f->sampleNs = hd.sample_ns;
	if (!f->handsMode || !hd.tracked || !c.head_valid || now - hd.sample_ns > 250000000LL)
		return true;
	double scale = c.hand_scale > 0.5f && c.hand_scale < 1.8f ? c.hand_scale : 1.0;
	HandPose hp;
	if (!Pose(c, h, scale, &hp) || !EnsureRetarget(h, hp))
		return true;
	const Valve &v = g_valve[h];
	const Retarget &r = g_ret[h];

	// per Meta joint, in the wrist's frame: rotation away from the bind pose and position
	M3 Rwt = Transpose(hp.R[0]);
	M3 Dv[J_COUNT];
	double Pm[J_COUNT][3];
	for (int j = 0; j < J_COUNT; j++) {
		M3 Rm = Mul(Rwt, hp.R[j]);
		Dv[j] = Mul(r.M, Mul(Mul(Rm, Transpose(hp.Rbind[j])), r.Mt)); // in the skeleton's model space
		double d[3] = {hp.P[j][0] - hp.P[0][0], hp.P[j][1] - hp.P[0][1], hp.P[j][2] - hp.P[0][2]}, w[3];
		MulV(Rwt, d, w);
		MulV(r.M, w, Pm[j]);
	}
	M3 Rv[kBones];
	double Pv[kBones][3];
	for (int b = 0; b < kBones; b++) {
		int j = kMetaOf[b];
		if (b >= B_AUX)
			continue; // placed below
		if (j >= 0) {
			Rv[b] = Mul(Dv[j], v.Rb[b]);
			for (int k = 0; k < 3; k++)
				Pv[b][k] = Pm[j][k];
		} else { // root and the metacarpals that Meta's skeleton does not have: with the wrist
			Rv[b] = Mul(Dv[0], v.Rb[b]);
			double d[3] = {v.Pb[b][0] - v.Pb[B_WRIST][0], v.Pb[b][1] - v.Pb[B_WRIST][1], v.Pb[b][2] - v.Pb[B_WRIST][2]};
			MulV(Dv[0], d, Pv[b]);
		}
	}
	// the wrist is the skeleton's origin
	for (int k = 0; k < 3; k++)
		Pv[B_ROOT][k] = Pv[B_WRIST][k] = 0;
	Rv[B_ROOT] = Identity();
	PlaceAux(v, Rv, Pv);
	Assemble(v, Rv, Pv, f->bones);

	// the skeleton's frame in the tracking space: the wrist's, turned by the model-space change
	M3 Rframe = Mul(hp.R[0], r.Mt);
	ToQuatWxyz(Rframe, f->rot);
	for (int k = 0; k < 3; k++) {
		f->pos[k] = hp.P[0][k];
		f->pinchPoint[k] = 0.5 * (hp.P[J_THUMB_TIP][k] + hp.P[J_INDEX_TIP][k]);
		f->palmNormal[k] = hp.palmNormal[k];
		f->palmCenter[k] = hp.palmCenter[k];
	}
	f->pinchDist = hp.pinchDist;
	f->tracked = true;
	return true;
}

bool CurledBones(int h, const float curl[5], vr::VRBoneTransform_t out[kBones])
{
	if (!LoadValve(h))
		return false;
	const Valve &v = g_valve[h];
	// finger chains, from the metacarpal on: bone, flexion at full curl (degrees); thumb, index, middle, ring, pinky
	struct Chain
	{
		int bone[4];
		float deg[4];
	};
	static const Chain chains[5] = {{{2, 3, 4, 5}, {10, 30, 45, 0}},
	                                {{6, 7, 8, 9}, {0, 75, 100, 55}},
	                                {{11, 12, 13, 14}, {0, 80, 100, 55}},
	                                {{16, 17, 18, 19}, {0, 80, 100, 55}},
	                                {{21, 22, 23, 24}, {8, 80, 100, 55}}};
	M3 Rv[kBones];
	double Pv[kBones][3];
	float ang[kBones] = {};
	for (int c = 0; c < 5; c++)
		for (int k = 0; k < 4; k++)
			ang[chains[c].bone[k]] = chains[c].deg[k] * 0.0174533f * (curl[c] < 0 ? 0 : curl[c] > 1 ? 1 : curl[c]);
	for (int b = 0; b < B_AUX; b++) {
		int p = v.parent[b];
		M3 rot = v.Lb[b];
		if (ang[b] != 0) { // flexion about the axis that turns the bone toward the palm
			int child = b + 1;
			double dir[3] = {v.Pb[child][0] - v.Pb[b][0], v.Pb[child][1] - v.Pb[b][1], v.Pb[child][2] - v.Pb[b][2]};
			Norm(dir);
			double axis[3], al[3];
			Cross(dir, v.palm, axis);
			Norm(axis);
			MulV(Transpose(v.Rb[b]), axis, al);
			rot = Mul(rot, Axis(al, ang[b]));
		}
		if (p < 0) {
			Rv[b] = rot;
			for (int k = 0; k < 3; k++)
				Pv[b][k] = v.Lt[b][k];
		} else {
			double d[3];
			MulV(Rv[p], v.Lt[b], d);
			Rv[b] = Mul(Rv[p], rot);
			for (int k = 0; k < 3; k++)
				Pv[b][k] = Pv[p][k] + d[k];
		}
	}
	PlaceAux(v, Rv, Pv);
	Assemble(v, Rv, Pv, out);
	return true;
}

} // namespace q1hands
