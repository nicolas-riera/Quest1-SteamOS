// SPDX-License-Identifier: MIT
/*
 * Forward kinematics of Meta's hand skeleton from the q1handsd shared memory (q1hands_shm.h): the world pose of the
 * 24 joints, the palm and the thumb-index pinch. Header-only C++, shared by driver_quest1 (hands.cpp) and q1handviz.
 *
 * Meta's hand (docs/hand-tracking.md): 19 local rotations per hand replace the bind rotations (they are absolute,
 * not deltas: the state of an untracked hand is the bind pose), the joint translations are the bind pose's; the root
 * pose is the wrist's. Right hand: the bones run along -X, the thumb is on +Z, the palm faces -Y. The left hand is
 * the same skeleton mirrored through the origin (fingers +X, thumb -Z, palm +Y).
 */
#pragma once

#include <cmath>
#include <cstdint>

#include "q1hands_shm.h"

namespace q1hands {

struct M3
{
	double m[9]; // row-major
};

inline M3 Mul(const M3 &a, const M3 &b)
{
	M3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
	return r;
}
inline M3 Transpose(const M3 &a)
{
	M3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i * 3 + j] = a.m[j * 3 + i];
	return r;
}
inline void MulV(const M3 &a, const double *v, double *o)
{
	o[0] = a.m[0] * v[0] + a.m[1] * v[1] + a.m[2] * v[2];
	o[1] = a.m[3] * v[0] + a.m[4] * v[1] + a.m[5] * v[2];
	o[2] = a.m[6] * v[0] + a.m[7] * v[1] + a.m[8] * v[2];
}
inline M3 Identity() { return {{1, 0, 0, 0, 1, 0, 0, 0, 1}}; }
//! x, y, z, w quaternion (libossdk order) to a rotation matrix
inline M3 FromQuatXyzw(const float *q)
{
	double x = q[0], y = q[1], z = q[2], w = q[3];
	double n = sqrt(x * x + y * y + z * z + w * w);
	if (n < 1e-9)
		return Identity();
	x /= n, y /= n, z /= n, w /= n;
	return {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w),
	         1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w),
	         1 - 2 * (x * x + y * y)}};
}
//! rotation matrix to w, x, y, z
inline void ToQuatWxyz(const M3 &r, double *q)
{
	const double *m = r.m;
	double tr = m[0] + m[4] + m[8];
	if (tr > 0) {
		double s = sqrt(tr + 1.0) * 2;
		q[0] = 0.25 * s, q[1] = (m[7] - m[5]) / s, q[2] = (m[2] - m[6]) / s, q[3] = (m[3] - m[1]) / s;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		double s = sqrt(1.0 + m[0] - m[4] - m[8]) * 2;
		q[0] = (m[7] - m[5]) / s, q[1] = 0.25 * s, q[2] = (m[1] + m[3]) / s, q[3] = (m[2] + m[6]) / s;
	} else if (m[4] > m[8]) {
		double s = sqrt(1.0 + m[4] - m[0] - m[8]) * 2;
		q[0] = (m[2] - m[6]) / s, q[1] = (m[1] + m[3]) / s, q[2] = 0.25 * s, q[3] = (m[5] + m[7]) / s;
	} else {
		double s = sqrt(1.0 + m[8] - m[0] - m[4]) * 2;
		q[0] = (m[3] - m[1]) / s, q[1] = (m[2] + m[6]) / s, q[2] = (m[5] + m[7]) / s, q[3] = 0.25 * s;
	}
	double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int i = 0; i < 4; i++)
		q[i] /= n;
}

//! the joints of Meta's skeleton that the rest of the code names
enum Joint
{
	J_WRIST = 0,
	J_THUMB0 = 2,
	J_THUMB3 = 5,
	J_INDEX1 = 6,
	J_MIDDLE1 = 9,
	J_PINKY0 = 15,
	J_THUMB_TIP = 19,
	J_INDEX_TIP = 20,
	J_COUNT = Q1HANDS_SKEL_BONES
};

struct HandPose
{
	M3 R[J_COUNT];     //!< joint world rotations, tracking space
	double P[J_COUNT][3]; //!< joint world positions
	M3 Rbind[J_COUNT]; //!< the bind pose's world rotations, in the hand's own model space (wrist at identity)
	double Pbind[J_COUNT][3]; //!< the bind pose's positions, model space
	double palmNormal[3]; //!< out of the palm, tracking space
	double palmCenter[3];
	double pinchDist;    //!< thumb tip to index tip, meters
	double scale;
};

//! Meta's bind skeleton's palm normal in the hand's model space: right -Y, left +Y (see the file comment)
inline void BindPalmNormal(int hand, double *n)
{
	n[0] = 0, n[1] = hand ? -1 : 1, n[2] = 0;
}

//! The pose of hand `h` (0 left, 1 right) in the tracking space, from a consistent copy of the shared memory.
//! `scale` multiplies the bind pose's bone lengths (libossdk's hand scale, 1 when unknown).
inline bool Pose(const q1hands_shm &s, int h, double scale, HandPose *o)
{
	if (!s.skeleton_valid)
		return false;
	const q1hands_hand &hd = s.hand[h];
	o->scale = scale;
	M3 Rroot = FromQuatXyzw(hd.root_rot);
	for (int j = 0; j < J_COUNT; j++) {
		int p = s.bind_parent[j];
		M3 local = j < Q1HANDS_BONES ? FromQuatXyzw(hd.bone_rot[j]) : FromQuatXyzw(s.bind_rot[h][j]);
		M3 lbind = FromQuatXyzw(s.bind_rot[h][j]);
		double t[3] = {s.bind_pos[h][j][0] * scale, s.bind_pos[h][j][1] * scale, s.bind_pos[h][j][2] * scale};
		double tb[3] = {s.bind_pos[h][j][0], s.bind_pos[h][j][1], s.bind_pos[h][j][2]};
		if (p < 0 || p >= j) {
			o->R[j] = Mul(Rroot, local);
			o->Rbind[j] = lbind;
			for (int k = 0; k < 3; k++)
				o->P[j][k] = hd.root_pos[k], o->Pbind[j][k] = 0;
		} else {
			double d[3], db[3];
			MulV(o->R[p], t, d);
			MulV(o->Rbind[p], tb, db);
			o->R[j] = Mul(o->R[p], local);
			o->Rbind[j] = Mul(o->Rbind[p], lbind);
			for (int k = 0; k < 3; k++)
				o->P[j][k] = o->P[p][k] + d[k], o->Pbind[j][k] = o->Pbind[p][k] + db[k];
		}
	}
	double nb[3];
	BindPalmNormal(h, nb);
	MulV(o->R[J_WRIST], nb, o->palmNormal);
	for (int k = 0; k < 3; k++)
		o->palmCenter[k] = 0.5 * (o->P[J_WRIST][k] + o->P[J_MIDDLE1][k]);
	double dx = o->P[J_THUMB_TIP][0] - o->P[J_INDEX_TIP][0], dy = o->P[J_THUMB_TIP][1] - o->P[J_INDEX_TIP][1],
	       dz = o->P[J_THUMB_TIP][2] - o->P[J_INDEX_TIP][2];
	o->pinchDist = sqrt(dx * dx + dy * dy + dz * dz);
	return true;
}

//! A consistent copy of the shared memory (seqlock), false while the writer is busy for good or the daemon is gone.
inline bool Copy(const volatile q1hands_shm *m, q1hands_shm *out)
{
	for (int i = 0; i < 8; i++) {
		uint32_t a = m->seq;
		if (a & 1)
			continue;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		__builtin_memcpy(out, (const void *)m, sizeof(*out));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (m->seq == a)
			return out->magic == Q1HANDS_MAGIC && out->version == Q1HANDS_VERSION;
	}
	return false;
}

} // namespace q1hands
