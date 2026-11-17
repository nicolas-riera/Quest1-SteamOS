// SPDX-License-Identifier: MIT
/*
 * The room tied to spatial anchors (docs/boundary.md "The room tied to anchors, as on Meta's OS").
 *
 * The engine locates every anchor from the landmarks around it (libossdk MapManager v13 getAnchorStates, a
 * seqlock copy of shared memory, no binder). q1anchord places and persists anchors around the room and lists their
 * uuids in ROOM_UUIDS. Here, each listed anchor that is located gets a reference pose the first time it is seen:
 * T_room_anchor, the anchor in the frame the room lives in. Afterwards, whenever anchors are located, the correction
 * C = T_room_world, estimated from the anchors (nearest weighted most), is applied to every pose q1trackd
 * publishes, so SteamVR, Monado, the chaperone, passthrough and the stream all see the room where it was.
 * C moves slowly toward its target (a small error is imperceptible), snaps only for a large one, and is held while
 * the engine relocalizes or no anchor is located.
 *
 * Frames (docs/boundary.md): the engine's world frame W is the frame of the persisted world anchor a1, so
 * T_W_anchor = inverse(T_odometry_a1) * T_odometry_submap * T_submap_anchor [G, to verify worn].
 * Pose7 = quaternion x,y,z,w then position x,y,z (OVR::Pose<float> layout).
 */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

struct pose7 {
	float q[4];
	float p[3];
};

static void q_mul(const float *a, const float *b, float *o)
{
	float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}

static void q_rot(const float *q, const float *v, float *o)
{
	float u[3] = {q[0], q[1], q[2]}, w = q[3];
	float c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
	float d[3] = {u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0]};
	for (int i = 0; i < 3; i++)
		o[i] = v[i] + 2 * (w * c[i] + d[i]);
}

static void q_norm(float *q)
{
	float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n < 1e-6f) {
		q[0] = q[1] = q[2] = 0, q[3] = 1;
		return;
	}
	for (int i = 0; i < 4; i++)
		q[i] /= n;
}

static struct pose7 p_ident(void)
{
	struct pose7 r;
	memset(&r, 0, sizeof(r));
	r.q[3] = 1;
	return r;
}

static struct pose7 p_mul(struct pose7 a, struct pose7 b)
{
	struct pose7 r;
	float t[3];
	q_mul(a.q, b.q, r.q);
	q_norm(r.q);
	q_rot(a.q, b.p, t);
	for (int i = 0; i < 3; i++)
		r.p[i] = a.p[i] + t[i];
	return r;
}

static struct pose7 p_inv(struct pose7 a)
{
	struct pose7 r;
	float c[4] = {-a.q[0], -a.q[1], -a.q[2], a.q[3]}, t[3];
	memcpy(r.q, c, sizeof(c));
	q_rot(c, a.p, t);
	for (int i = 0; i < 3; i++)
		r.p[i] = -t[i];
	return r;
}

static float q_angle_deg(const float *a, const float *b) /* angle between two rotations */
{
	float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	if (d > 1)
		d = 1;
	return 2 * acosf(d) * 57.29578f;
}

/* ---- engine anchor states (AnchorStates v13, 2000 bytes) ---- */
#define AS_SUBMAPS 0x28
#define AS_ANCHORS 0x258
#define AS_MAXANCH 25
#define AS_MAXSUB 10

struct eng_anchor {
	int64_t handle;
	uint8_t uuid[16];
	int located;
	struct pose7 odo; /* T_odometry_anchor */
};

struct eng_state {
	int ok;
	uint64_t nsub;
	int reloc;
	int have_odo_uuid;
	struct eng_anchor a[AS_MAXANCH];
};

static void parse_anchor_states(const uint8_t *b, struct eng_state *s)
{
	memset(s, 0, sizeof(*s));
	uint64_t nsub;
	memcpy(&nsub, b + 0x10, 8);
	if (nsub > AS_MAXSUB)
		return;
	s->nsub = nsub;
	s->reloc = b[0x20] != 0;
	for (int i = 0; i < AS_MAXANCH; i++) {
		const uint8_t *a = b + AS_ANCHORS + 0x38 * i;
		struct eng_anchor *o = &s->a[i];
		memcpy(&o->handle, a, 8);
		memcpy(o->uuid, a + 8, 16);
		uint8_t si = a[0x34];
		if (o->handle == -1 || si >= nsub)
			continue;
		struct pose7 ts, os;
		memcpy(&ts, a + 0x18, 28);
		memcpy(&os, b + AS_SUBMAPS + 0x38 * si + 0x18, 28);
		o->odo = p_mul(os, ts);
		o->located = 1;
	}
	s->ok = 1;
}

/* ---- reference poses (room frame), persisted next to the map files ---- */
#define ROOM_MAX 16
#define ROOM_REFS "/data/misc/tracking/room-anchors.bin"
#define ROOM_UUIDS "/data/misc/tracking/room-anchors.uuid"
#define WORLD_UUID "/data/misc/tracking/world-anchor.uuid"
#define ROOM_MAGIC 0x31524d51u

struct room_ref {
	uint8_t uuid[16];
	struct pose7 t_room_anchor;
};

struct room {
	struct room_ref ref[ROOM_MAX];
	int nref;
	struct pose7 c;     /* applied T_room_world */
	int have_c;
	int used;           /* anchors used by the last update */
	float err_m, err_deg; /* applied vs target at the last update */
	uint8_t world[16];
	int have_world;
	int64_t last_ns;
};

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "e565e1bf-..." -> 16 bytes in text order; returns 0 on success */
static int uuid_parse(const char *s, uint8_t *out)
{
	int n = 0;
	for (; *s && n < 32; s++) {
		if (*s == '-')
			continue;
		int h = hexval(s[0]), l = s[1] ? hexval(s[1]) : -1;
		if (h < 0 || l < 0)
			return -1;
		out[n / 2] = (uint8_t)(h * 16 + l);
		n += 2;
		s++;
	}
	return n == 32 ? 0 : -1;
}

/* reads a text file of uuids, one per line; returns the count */
static int uuids_read(const char *path, uint8_t (*out)[16], int max)
{
	char buf[2048];
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	int c = 0;
	for (char *p = buf; *p && c < max;) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (uuid_parse(p, out[c]) == 0)
			c++;
		if (!e)
			break;
		p = e + 1;
	}
	return c;
}

static void room_load(struct room *r)
{
	memset(r, 0, sizeof(*r));
	r->c = p_ident();
	FILE *f = fopen(ROOM_REFS, "rb");
	if (!f)
		return;
	uint32_t hdr[2];
	if (fread(hdr, 4, 2, f) == 2 && hdr[0] == ROOM_MAGIC && hdr[1] <= ROOM_MAX)
		r->nref = (int)fread(r->ref, sizeof(struct room_ref), hdr[1], f);
	fclose(f);
}

static void room_save(const struct room *r)
{
	FILE *f = fopen(ROOM_REFS ".tmp", "wb");
	if (!f)
		return;
	uint32_t hdr[2] = {ROOM_MAGIC, (uint32_t)r->nref};
	fwrite(hdr, 4, 2, f);
	fwrite(r->ref, sizeof(struct room_ref), r->nref, f);
	fclose(f);
	rename(ROOM_REFS ".tmp", ROOM_REFS);
}

static const struct pose7 *room_ref_find(const struct room *r, const uint8_t *uuid)
{
	for (int i = 0; i < r->nref; i++)
		if (!memcmp(r->ref[i].uuid, uuid, 16))
			return &r->ref[i].t_room_anchor;
	return NULL;
}

/* Correction limits: below SNAP the applied correction slews, above it jumps (a relocalization that moved the
 * world a lot; the driver hides the room meanwhile). */
#define ROOM_SLEW_M_S 0.02f
#define ROOM_SLEW_DEG_S 2.0f
#define ROOM_SNAP_M 0.25f
#define ROOM_SNAP_DEG 12.0f

/*
 * One update (about 20 Hz). head = the head position in the engine's world frame W (before the correction).
 * Returns 1 when the correction changed.
 */
static int room_update(struct room *r, const struct eng_state *s, const float *head, int64_t now_ns)
{
	float dt = r->last_ns ? (float)(now_ns - r->last_ns) * 1e-9f : 0;
	r->last_ns = now_ns;
	if (dt > 1)
		dt = 1;
	r->used = 0;
	if (!s->ok || s->reloc || !r->have_world)
		return 0;
	struct pose7 odo_w = p_ident();
	int found = 0;
	for (int i = 0; i < AS_MAXANCH; i++)
		if (s->a[i].located && !memcmp(s->a[i].uuid, r->world, 16)) {
			odo_w = s->a[i].odo;
			found = 1;
		}
	if (!found)
		return 0;
	struct pose7 w_odo = p_inv(odo_w);

	/* new references: a listed anchor seen located, stored in the room frame through the applied correction */
	uint8_t list[ROOM_MAX][16];
	int nl = uuids_read(ROOM_UUIDS, list, ROOM_MAX);
	int added = 0;
	for (int k = 0; k < nl && r->nref < ROOM_MAX; k++) {
		if (!memcmp(list[k], r->world, 16) || room_ref_find(r, list[k]))
			continue;
		for (int i = 0; i < AS_MAXANCH; i++)
			if (s->a[i].located && !memcmp(s->a[i].uuid, list[k], 16)) {
				memcpy(r->ref[r->nref].uuid, list[k], 16);
				r->ref[r->nref].t_room_anchor = p_mul(r->c, p_mul(w_odo, s->a[i].odo));
				r->nref++;
				added = 1;
			}
	}
	if (added)
		room_save(r);

	/* target: weighted blend of the per-anchor corrections C_i = T_room_anchor * inverse(T_W_anchor) */
	float qs[4] = {0, 0, 0, 0}, ps[3] = {0, 0, 0}, wsum = 0, qref[4] = {0, 0, 0, 1};
	int n = 0;
	for (int i = 0; i < AS_MAXANCH; i++) {
		if (!s->a[i].located)
			continue;
		const struct pose7 *ref = room_ref_find(r, s->a[i].uuid);
		if (!ref)
			continue;
		struct pose7 t_w_a = p_mul(w_odo, s->a[i].odo);
		struct pose7 ci = p_mul(*ref, p_inv(t_w_a));
		float d[3] = {t_w_a.p[0] - head[0], t_w_a.p[1] - head[1], t_w_a.p[2] - head[2]};
		float w = 1.0f / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 0.25f);
		if (n == 0)
			memcpy(qref, ci.q, sizeof(qref));
		float sg = (ci.q[0] * qref[0] + ci.q[1] * qref[1] + ci.q[2] * qref[2] + ci.q[3] * qref[3]) < 0 ? -1.f : 1.f;
		for (int k = 0; k < 4; k++)
			qs[k] += w * sg * ci.q[k];
		for (int k = 0; k < 3; k++)
			ps[k] += w * ci.p[k];
		wsum += w;
		n++;
	}
	r->used = n;
	if (n == 0)
		return 0;
	struct pose7 tgt;
	memcpy(tgt.q, qs, sizeof(qs));
	q_norm(tgt.q);
	for (int k = 0; k < 3; k++)
		tgt.p[k] = ps[k] / wsum;

	float dp[3] = {tgt.p[0] - r->c.p[0], tgt.p[1] - r->c.p[1], tgt.p[2] - r->c.p[2]};
	float dm = sqrtf(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
	float dd = q_angle_deg(tgt.q, r->c.q);
	r->err_m = dm, r->err_deg = dd;
	if (!r->have_c || dm > ROOM_SNAP_M || dd > ROOM_SNAP_DEG) {
		r->c = tgt;
		r->have_c = 1;
		return 1;
	}
	if (dm < 1e-4f && dd < 0.01f)
		return 0;
	float fm = dm > 0 ? fminf(1.f, ROOM_SLEW_M_S * dt / dm) : 1.f;
	float fd = dd > 0 ? fminf(1.f, ROOM_SLEW_DEG_S * dt / dd) : 1.f;
	float f = fminf(fm, fd); /* neither limit is exceeded */
	float sg = (tgt.q[0] * r->c.q[0] + tgt.q[1] * r->c.q[1] + tgt.q[2] * r->c.q[2] + tgt.q[3] * r->c.q[3]) < 0 ? -1.f : 1.f;
	for (int k = 0; k < 4; k++)
		r->c.q[k] = r->c.q[k] * (1 - f) + sg * tgt.q[k] * f;
	q_norm(r->c.q);
	for (int k = 0; k < 3; k++)
		r->c.p[k] += dp[k] * f;
	return 1;
}

/* applies the correction to a published pose (position, orientation, and the vectors in tracking space) */
static void room_apply(const struct pose7 *c, float *orient, float *pos, float *vecs[], int nvec)
{
	float t[3], q[4];
	q_rot(c->q, pos, t);
	for (int i = 0; i < 3; i++)
		pos[i] = t[i] + c->p[i];
	q_mul(c->q, orient, q);
	q_norm(q);
	memcpy(orient, q, sizeof(q));
	for (int k = 0; k < nvec; k++) {
		q_rot(c->q, vecs[k], t);
		memcpy(vecs[k], t, sizeof(t));
	}
}
