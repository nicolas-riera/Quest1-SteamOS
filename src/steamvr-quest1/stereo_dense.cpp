#include "stereo_dense.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace
{
double NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross(const double a[3], const double b[3], double o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool Normalize(double v[3])
{
	double n = sqrt(Dot(v, v));
	if (n < 1e-12)
		return false;
	v[0] /= n, v[1] /= n, v[2] /= n;
	return true;
}

// atan2 to about 1e-5 rad, no libm call (the splat runs it a few times per valid pixel)
inline float FastAtan2(float y, float x)
{
	float ax = fabsf(x), ay = fabsf(y);
	float mx = std::max(ax, ay), mn = std::min(ax, ay);
	float z = mx > 0 ? mn / mx : 0, z2 = z * z;
	float a = z * (0.9998660f + z2 * (-0.3302995f + z2 * (0.1801410f + z2 * (-0.0851330f + 0.0208351f * z2))));
	if (ay > ax)
		a = 1.57079633f - a;
	if (x < 0)
		a = 3.14159265f - a;
	return y < 0 ? -a : a;
}

const int kRing = 5; // vertical window of the aggregation (5x5 box)
const int kPlanes = 3; // the 24 census bits as three byte planes

// 24-bit census, planar bytes: bit k of the window of row y, column x set if neighbour k is darker than the centre
void Census(const uint8_t *img, int W, int H, uint8_t *planes /* [3][H][WP] */, int WP, uint8_t *scratch)
{
	static const int8_t off[24][2] = {{-2, -2}, {-1, -2}, {0, -2}, {1, -2}, {2, -2}, {-2, -1}, {-1, -1}, {0, -1},
	                                  {1, -1},  {2, -1},  {-2, 0}, {-1, 0}, {1, 0},   {2, 0},   {-2, 1},  {-1, 1},
	                                  {0, 1},   {1, 1},   {2, 1},  {-2, 2}, {-1, 2},  {0, 2},   {1, 2},   {2, 2}};
	for (int p = 0; p < kPlanes; p++)
		memset(planes + (size_t)p * H * WP, 0, (size_t)H * WP);
	for (int y = 2; y < H - 2; y++) {
		const uint8_t *c = img + (size_t)y * W;
		for (int p = 0; p < kPlanes; p++) {
			uint8_t *acc = scratch;
			memset(acc, 0, W);
			for (int b = 0; b < 8; b++) {
				const int k = p * 8 + b;
				const uint8_t *n = img + (size_t)(y + off[k][1]) * W + off[k][0];
				for (int x = 2; x < W - 2; x++)
					acc[x] |= (uint8_t)((n[x] < c[x]) << b);
			}
			memcpy(planes + ((size_t)p * H + y) * WP, acc, W);
		}
	}
}

// raw matching cost of one row for the disparities dmin .. dmin+D-1, x in [xa, xb):
// cost[i][x] = number of the 24 census bits that differ between left x and right x - (dmin + i)
void RowCost(const uint8_t *lp[3], const uint8_t *rp[3], int xa, int xb, int dmin, int D, int WP, uint8_t *cost)
{
	for (int i = 0; i < D; i++) {
		const int d = dmin + i;
		uint8_t *o = cost + (size_t)i * WP;
		const int x0 = std::max(xa, std::min(xb, d + 2)); // right census is only valid from x = 2
		if (x0 > xa)
			memset(o + xa, 24, x0 - xa);
		int x = x0;
#if defined(__ARM_NEON)
		for (; x + 16 <= xb; x += 16) {
			uint8x16_t a = vcntq_u8(veorq_u8(vld1q_u8(lp[0] + x), vld1q_u8(rp[0] + x - d)));
			uint8x16_t b = vcntq_u8(veorq_u8(vld1q_u8(lp[1] + x), vld1q_u8(rp[1] + x - d)));
			uint8x16_t c = vcntq_u8(veorq_u8(vld1q_u8(lp[2] + x), vld1q_u8(rp[2] + x - d)));
			vst1q_u8(o + x, vaddq_u8(vaddq_u8(a, b), c));
		}
#endif
		for (; x < xb; x++)
			o[x] = (uint8_t)(__builtin_popcount(lp[0][x] ^ rp[0][x - d]) + __builtin_popcount(lp[1][x] ^ rp[1][x - d]) +
			                 __builtin_popcount(lp[2][x] ^ rp[2][x - d]));
	}
}

// local gray-level range of the 3x3 neighbourhood at least `minContrast`
void Contrast(const uint8_t *img, int W, int H, int minContrast, uint8_t *out)
{
	std::vector<uint8_t> mx(W), mn(W);
	memset(out, 0, (size_t)W * H);
	for (int y = 1; y < H - 1; y++) {
		const uint8_t *r0 = img + (size_t)(y - 1) * W, *r1 = r0 + W, *r2 = r1 + W;
		for (int x = 0; x < W; x++) {
			mx[x] = std::max(r0[x], std::max(r1[x], r2[x]));
			mn[x] = std::min(r0[x], std::min(r1[x], r2[x]));
		}
		uint8_t *o = out + (size_t)y * W;
		for (int x = 1; x < W - 1; x++) {
			int hi = std::max(mx[x - 1], std::max(mx[x], mx[x + 1])), lo = std::min(mn[x - 1], std::min(mn[x], mn[x + 1]));
			o[x] = hi - lo >= minContrast;
		}
	}
}
} // namespace

// ---- DenseMatcher: census block matching at one resolution over the disparities [dmin, dmin + D) ----------------------

class DenseMatcher
{
public:
	struct Tests
	{
		float maxCost, uniqueness;
		int lrTolerance;
	};

	//! box: rows/columns where results are wanted (the rest of the image is outside both cameras' views)
	void Configure(int W, int H, int dmin, int D, int xlo, int xhi, int ylo, int yhi)
	{
		W_ = W, H_ = H, dmin_ = dmin, D_ = D, WP_ = W + 32;
		xlo_ = std::max(4, xlo), xhi_ = std::min(W - 4, xhi), ylo_ = std::max(4, ylo), yhi_ = std::min(H - 4, yhi);
		// columns the cost sums are computed on: the output columns plus the 5-wide window
		xa_ = std::max(2, xlo_ - 2), xb_ = std::min(W - 2, xhi_ + 2);
		cl_.assign((size_t)kPlanes * H * WP_, 0), cr_.assign((size_t)kPlanes * H * WP_, 0);
		c8_.assign((size_t)D * WP_, 0), ring_.assign((size_t)kRing * D * WP_, 0), S_.assign((size_t)D * WP_, 0);
		bestL_.assign(WP_, 0), secL_.assign(WP_, 0), bestR_.assign(WP_, 0), dL_.assign(WP_, 0), dR_.assign(WP_, 0);
		scratch_.assign(WP_, 0);
	}
	bool Empty() const { return xhi_ <= xlo_ || yhi_ <= ylo_; }
	int Width() const { return W_; }
	int Height() const { return H_; }

	//! left/right: W x H 8-bit; valid: W x H, nonzero where a match may be attempted. Results: Q4 disparity (0 = none), confidence.
	void Run(const uint8_t *left, const uint8_t *right, const uint8_t *valid, const Tests &t, uint16_t *disp, uint8_t *conf)
	{
		const int W = W_, H = H_, D = D_, WP = WP_;
		memset(disp, 0, (size_t)W * H * sizeof(uint16_t));
		memset(conf, 0, (size_t)W * H);
		if (Empty())
			return;
		Census(left, W, H, cl_.data(), WP, scratch_.data());
		Census(right, W, H, cr_.data(), WP, scratch_.data());
		const uint32_t maxCost = (uint32_t)(t.maxCost * 24 * 25);
		std::fill(S_.begin(), S_.end(), 0);
		std::fill(ring_.begin(), ring_.end(), 0);
		const size_t plane = (size_t)H * WP;
		const uint8_t *cl = cl_.data(), *cr = cr_.data();
		const int ya = std::max(0, ylo_ - 4), yb = std::min(H, yhi_ + 4);
		uint16_t *bl = bestL_.data(), *sl = secL_.data(), *dl = dL_.data(), *br = bestR_.data(), *dr = dR_.data();
		for (int y = ya; y < yb; y++) {
			uint16_t *slot = ring_.data() + (size_t)(y % kRing) * D * WP;
			// drop the row that leaves the window, add the new one
			if (y >= ya + kRing)
				for (int i = 0; i < D; i++) {
					uint16_t *s = S_.data() + (size_t)i * WP, *old = slot + (size_t)i * WP;
					for (int x = xa_; x < xb_; x++)
						s[x] = (uint16_t)(s[x] - old[x]);
				}
			const uint8_t *lp[3] = {cl + y * WP, cl + plane + y * WP, cl + 2 * plane + y * WP};
			const uint8_t *rp[3] = {cr + y * WP, cr + plane + y * WP, cr + 2 * plane + y * WP};
			RowCost(lp, rp, xa_ - 2 > 0 ? xa_ - 2 : 0, std::min(W, xb_ + 2), dmin_, D, WP, c8_.data());
			for (int i = 0; i < D; i++) {
				const uint8_t *c = c8_.data() + (size_t)i * WP;
				uint16_t *h = slot + (size_t)i * WP, *s = S_.data() + (size_t)i * WP;
				for (int x = xa_; x < xb_; x++) {
					uint16_t v = (uint16_t)(c[x - 2] + c[x - 1] + c[x] + c[x + 1] + c[x + 2]);
					h[x] = v;
					s[x] = (uint16_t)(s[x] + v);
				}
			}
			if (y < ya + kRing - 1)
				continue;
			const int yc = y - 2; // window rows yc-2 .. yc+2
			if (yc < ylo_ || yc >= yhi_)
				continue;
			for (int x = xa_; x < xb_; x++)
				bl[x] = 0xFFFF, sl[x] = 0xFFFF, dl[x] = 0;
			for (int x = 2; x < xb_; x++)
				br[x] = 0xFFFF, dr[x] = 0;
			// left-referenced winner, then the second best outside +-1 step
			for (int i = 0; i < D; i++) {
				const uint16_t *s = S_.data() + (size_t)i * WP;
				const uint16_t i16 = (uint16_t)i;
				for (int x = xa_; x < xb_; x++) {
					uint16_t v = s[x];
					bool lt = v < bl[x];
					bl[x] = lt ? v : bl[x];
					dl[x] = lt ? i16 : dl[x];
				}
			}
			for (int i = 0; i < D; i++) {
				const uint16_t *s = S_.data() + (size_t)i * WP;
				const uint16_t i16 = (uint16_t)i;
				for (int x = xa_; x < xb_; x++) {
					uint16_t d = (uint16_t)(i16 - dl[x] + 1); // 0..2 = within one step of the winner (wraps for negative)
					uint16_t sv = s[x], v = d > 2 ? sv : (uint16_t)0xFFFF;
					sl[x] = v < sl[x] ? v : sl[x];
				}
			}
			// right-referenced winner: right pixel xr sees left xr + d
			for (int i = 0; i < D; i++) {
				const int d = dmin_ + i;
				const uint16_t *s = S_.data() + (size_t)i * WP + d;
				const uint16_t i16 = (uint16_t)i;
				const int xs = 2, xe = std::min(xb_ - d, W - 2);
				for (int xr = xs; xr < xe; xr++) {
					uint16_t v = s[xr];
					bool lt = v < br[xr];
					br[xr] = lt ? v : br[xr];
					dr[xr] = lt ? i16 : dr[xr];
				}
			}
			const uint8_t *val = valid + (size_t)yc * W;
			uint16_t *out = disp + (size_t)yc * W;
			uint8_t *cf = conf + (size_t)yc * W;
			for (int x = xlo_; x < xhi_; x++) {
				if (!val[x] || bl[x] > maxCost)
					continue;
				const int i = dl[x], d = dmin_ + i;
				if (x - d < 4)
					continue;
				if (sl[x] != 0xFFFF && (float)(sl[x] - bl[x]) <= t.uniqueness * bl[x])
					continue;
				if (abs((int)dr[x - d] - i) > t.lrTolerance)
					continue;
				float pos = (float)d;
				if (i > 0 && i < D - 1) {
					float a = S_[(size_t)(i - 1) * WP + x], b = S_[(size_t)i * WP + x], c = S_[(size_t)(i + 1) * WP + x];
					float den = a - 2 * b + c;
					if (den > 1e-3f)
						pos += std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den));
				}
				if (pos < 0.5f)
					continue; // at or beyond infinity
				out[x] = (uint16_t)std::max(1.0f, pos * 16 + 0.5f);
				float q1 = 1.0f - (float)bl[x] / (float)(maxCost + 1);
				float q2 = sl[x] == 0xFFFF ? 1.0f : std::min(1.0f, (float)(sl[x] - bl[x]) / (0.5f * bl[x] + 8));
				cf[x] = (uint8_t)std::max(1.0f, 255.0f * q1 * q2);
			}
		}
	}

private:
	int W_ = 0, H_ = 0, dmin_ = 0, D_ = 0, WP_ = 0, xlo_ = 0, xhi_ = 0, ylo_ = 0, yhi_ = 0, xa_ = 0, xb_ = 0;
	std::vector<uint8_t> cl_, cr_, c8_, scratch_;
	std::vector<uint16_t> ring_, S_, bestL_, secL_, bestR_, dL_, dR_;
};

// ---- StereoDenseRig ------------------------------------------------------------------------------------------------

StereoDenseRig::StereoDenseRig() {}
StereoDenseRig::~StereoDenseRig() {}

bool StereoDenseRig::Configure(const Params &p, ProjectFn left, ProjectFn right, const double tl[3], const double tr[3],
                               const double viewDir[3], int imageW, int imageH)
{
	params_ = p;
	if (!left || !right || p.rectW < 64 || p.rectH < 64 || (p.rectW & 1) || (p.rectH & 1) || p.focal <= 1 || p.nearM <= 0 || imageW < 64 ||
	    imageH < 64)
		return false;
	W_ = p.rectW, H_ = p.rectH, rawW_ = imageW, rawH_ = imageH;
	double b[3] = {tr[0] - tl[0], tr[1] - tl[1], tr[2] - tl[2]};
	B_ = sqrt(Dot(b, b));
	if (B_ < 0.01)
		return false;
	memcpy(tl_, tl, sizeof(tl_));
	for (int i = 0; i < 3; i++)
		ex_[i] = b[i] / B_;
	double z[3] = {viewDir[0], viewDir[1], viewDir[2]};
	double zb = Dot(z, ex_);
	for (int i = 0; i < 3; i++)
		z[i] -= zb * ex_[i];
	if (!Normalize(z))
		return false;
	memcpy(ez_, z, sizeof(ez_));
	Cross(ez_, ex_, ey_); // x right, y down, z forward
	cx_ = W_ / 2.0, cy_ = H_ / 2.0;
	D_ = (int)ceil(p.focal * B_ / p.nearM) + 2;
	D_ = std::min(128, (D_ + 7) & ~7);

	const int hw = rawW_ / 2, hh = rawH_ / 2;
	for (int side = 0; side < 2; side++) {
		Map &m = side ? mapR_ : mapL_;
		const ProjectFn &fn = side ? right : left;
		const double *t = side ? tr : tl;
		m.idx.assign((size_t)W_ * H_, -1);
		m.wx.assign((size_t)W_ * H_, 0);
		m.wy.assign((size_t)W_ * H_, 0);
		for (int y = 0; y < H_; y++)
			for (int x = 0; x < W_; x++) {
				double u = (x + 0.5 - cx_) / p.focal, v = (y + 0.5 - cy_) / p.focal, dir[3], pt[3];
				for (int i = 0; i < 3; i++)
					dir[i] = u * ex_[i] + v * ey_[i] + ez_[i];
				Normalize(dir);
				for (int i = 0; i < 3; i++)
					pt[i] = t[i] + 1000 * dir[i]; // a far point: the direction as seen from this camera
				double px, py;
				if (!fn(pt, px, py))
					continue;
				// raw pixel centres at integers; the half-size image's pixel k covers raw 2k and 2k+1
				double hx = (px - 0.5) / 2, hy = (py - 0.5) / 2;
				int x0 = (int)floor(hx), y0 = (int)floor(hy);
				if (x0 < 0 || y0 < 0 || x0 + 1 >= hw || y0 + 1 >= hh)
					continue;
				size_t i = (size_t)y * W_ + x;
				m.idx[i] = y0 * hw + x0;
				m.wx[i] = (uint8_t)std::min(255.0, (hx - x0) * 256);
				m.wy[i] = (uint8_t)std::min(255.0, (hy - y0) * 256);
			}
	}
	hl_.assign((size_t)hw * hh, 0), hr_.assign((size_t)hw * hh, 0);
	rl_.assign((size_t)W_ * H_, 0), rr_.assign((size_t)W_ * H_, 0);
	inside_.assign((size_t)W_ * H_, 0), contrast_.assign((size_t)W_ * H_, 0), valid_.assign((size_t)W_ * H_, 0);
	disp_.assign((size_t)W_ * H_, 0), conf_.assign((size_t)W_ * H_, 0);
	int xlo = W_, xhi = 0, ylo = H_, yhi = 0;
	for (int y = 0; y < H_; y++)
		for (int x = 0; x < W_; x++) {
			size_t i = (size_t)y * W_ + x;
			inside_[i] = mapL_.idx[i] >= 0 && mapR_.idx[i] >= 0;
			if (inside_[i])
				xlo = std::min(xlo, x), xhi = std::max(xhi, x + 1), ylo = std::min(ylo, y), yhi = std::max(yhi, y + 1);
		}

	// the far matcher covers disparities [0, farMax) at the rectified size; the near one [farMax - 8, D) at half size
	farMax_ = std::min(D_, std::max(8, params_.farDispMax));
	far_.reset(new DenseMatcher);
	far_->Configure(W_, H_, 0, farMax_, xlo, xhi, ylo, yhi);
	near_.reset();
	if (D_ > farMax_) {
		const int hW = W_ / 2, hH = H_ / 2, dlo = (farMax_ - 8) / 2, dhi = (D_ + 1) / 2;
		near_.reset(new DenseMatcher);
		near_->Configure(hW, hH, dlo, dhi - dlo, xlo / 2, (xhi + 1) / 2, ylo / 2, (yhi + 1) / 2);
		nl_.assign((size_t)hW * hH, 0), nr_.assign((size_t)hW * hH, 0);
		nvalid_.assign((size_t)hW * hH, 0), ndisp_.assign((size_t)hW * hH, 0), nconf_.assign((size_t)hW * hH, 0);
		for (int y = 0; y < hH; y++)
			for (int x = 0; x < hW; x++)
				nvalid_[(size_t)y * hW + x] = inside_[(size_t)2 * y * W_ + 2 * x] && inside_[(size_t)(2 * y + 1) * W_ + 2 * x + 1];
	}
	return true;
}

void StereoDenseRig::Direction(double x, double y, double d[3]) const
{
	double u = (x - cx_) / params_.focal, v = (y - cy_) / params_.focal;
	for (int i = 0; i < 3; i++)
		d[i] = u * ex_[i] + v * ey_[i] + ez_[i];
	Normalize(d);
}

void StereoDenseRig::Unproject(double x, double y, double disparity, double p[3]) const
{
	double Z = params_.focal * B_ / disparity, X = (x - cx_) / params_.focal * Z, Y = (y - cy_) / params_.focal * Z;
	for (int i = 0; i < 3; i++)
		p[i] = tl_[i] + ex_[i] * X + ey_[i] * Y + ez_[i] * Z;
}

bool StereoDenseRig::Compute(const uint8_t *left, const uint8_t *right, int stride)
{
	if (!left || !right || W_ == 0)
		return false;
	stats_ = Stats();
	const int hw = rawW_ / 2, hh = rawH_ / 2;
	double t0 = NowMs();
	for (int side = 0; side < 2; side++) {
		const uint8_t *src = side ? right : left;
		uint8_t *h = side ? hr_.data() : hl_.data();
		for (int y = 0; y < hh; y++) {
			const uint8_t *a = src + (size_t)2 * y * stride, *b = a + stride;
			uint8_t *o = h + (size_t)y * hw;
			for (int x = 0; x < hw; x++)
				o[x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
		}
		const Map &m = side ? mapR_ : mapL_;
		uint8_t *out = side ? rr_.data() : rl_.data();
		for (size_t i = 0, n = (size_t)W_ * H_; i < n; i++) {
			int32_t k = m.idx[i];
			if (k < 0) {
				out[i] = 0;
				continue;
			}
			const uint8_t *q = h + k;
			int wx = m.wx[i], wy = m.wy[i];
			int top = q[0] * (256 - wx) + q[1] * wx, bot = q[hw] * (256 - wx) + q[hw + 1] * wx;
			out[i] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
		}
	}
	double t1 = NowMs();

	Contrast(rl_.data(), W_, H_, params_.minContrast, contrast_.data());
	for (size_t i = 0, n = (size_t)W_ * H_; i < n; i++)
		valid_[i] = inside_[i] & contrast_[i];
	const DenseMatcher::Tests tests = {params_.maxCost, params_.uniqueness, params_.lrTolerance};
	far_->Run(rl_.data(), rr_.data(), valid_.data(), tests, disp_.data(), conf_.data());
	double t2 = NowMs();
	if (near_) {
		const int hW = W_ / 2, hH = H_ / 2;
		for (int side = 0; side < 2; side++) {
			const uint8_t *src = side ? rr_.data() : rl_.data();
			uint8_t *o = side ? nr_.data() : nl_.data();
			for (int y = 0; y < hH; y++) {
				const uint8_t *a = src + (size_t)2 * y * W_, *b = a + W_;
				for (int x = 0; x < hW; x++)
					o[(size_t)y * hW + x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
			}
		}
		std::vector<uint8_t> nc((size_t)hW * hH), nv((size_t)hW * hH);
		Contrast(nl_.data(), hW, hH, params_.minContrast, nc.data());
		for (size_t i = 0; i < nv.size(); i++)
			nv[i] = nvalid_[i] & nc[i];
		near_->Run(nl_.data(), nr_.data(), nv.data(), tests, ndisp_.data(), nconf_.data());
		// merge: the far result where it is a real match below the hand-over disparity, else the near one (x2 in both axes)
		const int handOver = (farMax_ - 4) * 16;
		for (int y = 0; y < H_; y++)
			for (int x = 0; x < W_; x++) {
				const size_t i = (size_t)y * W_ + x;
				if (disp_[i] && disp_[i] < handOver)
					continue;
				const size_t j = (size_t)(y / 2) * hW + x / 2;
				if (ndisp_[j] && nconf_[j] >= params_.nearMinConf) {
					disp_[i] = (uint16_t)std::min(65535, 2 * (int)ndisp_[j]);
					conf_[i] = nconf_[j];
				}
			}
	}
	double t3 = NowMs();

	// speckle removal: connected pieces of similar disparity (<= 1 px apart) that are too small
	{
		const int W = W_, H = H_;
		label_.assign((size_t)W * H, 0);
		int next = 1;
		for (int y0 = 0; y0 < H; y0++)
			for (int x0 = 0; x0 < W; x0++) {
				size_t i0 = (size_t)y0 * W + x0;
				if (!disp_[i0] || label_[i0])
					continue;
				stack_.clear(), members_.clear();
				stack_.push_back((int32_t)i0);
				label_[i0] = next;
				while (!stack_.empty()) {
					int32_t i = stack_.back();
					stack_.pop_back();
					members_.push_back(i);
					int x = i % W, y = i / W;
					static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
					for (int k = 0; k < 4; k++) {
						int nx = x + dx[k], ny = y + dy[k];
						if (nx < 0 || ny < 0 || nx >= W || ny >= H)
							continue;
						size_t j = (size_t)ny * W + nx;
						if (!disp_[j] || label_[j] || abs((int)disp_[j] - (int)disp_[i]) > 16)
							continue;
						label_[j] = next;
						stack_.push_back((int32_t)j);
					}
				}
				if ((int)members_.size() < params_.speckleMin)
					for (int32_t i : members_)
						disp_[i] = 0, conf_[i] = 0;
				next++;
			}
	}
	double t4 = NowMs();
	for (size_t i = 0, n = (size_t)W_ * H_; i < n; i++) {
		stats_.pixels += inside_[i];
		stats_.matched += disp_[i] != 0;
	}
	stats_.msRectify = t1 - t0, stats_.msFarMatch = t2 - t1, stats_.msNearMatch = t3 - t2, stats_.msFilter = t4 - t3;
	return true;
}

// ---- StereoDense ---------------------------------------------------------------------------------------------------

void StereoDense::Configure(const Params &p)
{
	params_ = p;
	const int cells = p.cols * p.rows;
	dirs_.assign(cells * 3, 0);
	for (int r = 0; r < p.rows; r++)
		for (int c = 0; c < p.cols; c++) {
			double az = (p.azMinDeg + (p.azMaxDeg - p.azMinDeg) * (c + 0.5) / p.cols) * M_PI / 180;
			double el = (p.elMaxDeg - (p.elMaxDeg - p.elMinDeg) * (r + 0.5) / p.rows) * M_PI / 180;
			double *d = &dirs_[(r * p.cols + c) * 3];
			d[0] = sin(az) * cos(el), d[1] = sin(el), d[2] = -cos(az) * cos(el);
		}
	depth_.assign(cells, (float)p.defaultDepthM), conf_.assign(cells, 0);
	inv_.assign(cells, 0), var_.assign(cells, 0), prevInv_.assign(cells, 0), prevVar_.assign(cells, 0), tmp_.assign(cells, 0), prevFill_.assign(cells, 0);
	src_.assign(cells, 0), age_.assign(cells, 0), prevAge_.assign(cells, 0), has_.assign(cells, 0);
	cellStart_.assign(cells + 1, 0), cellCount_.assign(cells, 0);
	haveHistory_ = false;
}

void StereoDense::ResetHistory() { haveHistory_ = false; }

void StereoDense::SetPose(const double *m)
{
	if (!m) {
		havePose_ = false;
		return;
	}
	memcpy(pose_, m, sizeof(pose_));
	havePose_ = true;
}

void StereoDense::SetFloor(const double *plane)
{
	haveFloor_ = plane != nullptr;
	if (plane)
		memcpy(floor_, plane, sizeof(floor_));
}

int StereoDense::CellOf(const double dir[3]) const
{
	const Params &p = params_;
	float az = FastAtan2((float)dir[0], (float)-dir[2]) * (180.0f / 3.14159265f);
	float hz = sqrtf((float)(dir[0] * dir[0] + dir[2] * dir[2]));
	float el = FastAtan2((float)dir[1], hz) * (180.0f / 3.14159265f);
	int c = (int)floorf((az - (float)p.azMinDeg) / (float)(p.azMaxDeg - p.azMinDeg) * p.cols);
	int r = (int)floorf(((float)p.elMaxDeg - el) / (float)(p.elMaxDeg - p.elMinDeg) * p.rows);
	if (c < 0 || c >= p.cols || r < 0 || r >= p.rows)
		return -1;
	return r * p.cols + c;
}

void StereoDense::Update()
{
	const Params &p = params_;
	const int cells = p.cols * p.rows;
	stats_ = Stats();

	// 1. every valid pixel of every rig -> (cell, range)
	sampleCell_.clear(), sampleR_.clear();
	for (size_t ri = 0; ri < rigs_.size(); ri++) {
		StereoDenseRig *rig = rigs_[ri];
		if (!rigFresh_[ri])
			continue;
		const uint16_t *disp = rig->Disparity();
		const int W = rig->Width(), H = rig->Height();
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				uint16_t dq = disp[(size_t)y * W + x];
				if (!dq)
					continue;
				double pt[3];
				rig->Unproject(x + 0.5, y + 0.5, dq / 16.0, pt);
				double r = sqrt(Dot(pt, pt));
				if (r < p.nearM || r > p.farM)
					continue;
				double dir[3] = {pt[0] / r, pt[1] / r, pt[2] / r};
				int cell = CellOf(dir);
				if (cell < 0)
					continue;
				sampleCell_.push_back(cell), sampleR_.push_back((float)r);
			}
	}
	std::fill(cellCount_.begin(), cellCount_.end(), 0);
	for (int32_t c : sampleCell_)
		cellCount_[c]++;
	cellStart_[0] = 0;
	for (int i = 0; i < cells; i++)
		cellStart_[i + 1] = cellStart_[i] + cellCount_[i];
	std::vector<float> sorted(sampleR_.size());
	{
		std::vector<int32_t> fill(cellStart_.begin(), cellStart_.end() - 1);
		for (size_t i = 0; i < sampleCell_.size(); i++)
			sorted[fill[sampleCell_[i]]++] = sampleR_[i];
	}

	// 2. prediction from the previous frame (inverse warp: rotation and translation of the head between the frames)
	std::vector<float> predInv(cells, 0), predVar(cells, 0), predFill(cells, 0);
	std::vector<uint8_t> predAge(cells, 0);
	const bool predict = haveHistory_ && havePose_ && havePrevPose_;
	float motion = 1; // 0 = the head has not moved since the previous frame, 1 = it has
	if (predict) {
		// M = cur^-1 * prev maps a previous device-frame point to the current device frame
		double Rc[9], Rp[9];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				Rc[r * 3 + c] = pose_[r * 4 + c], Rp[r * 3 + c] = prevPose_[r * 4 + c];
		double M[9], tM[3];
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				double s = 0;
				for (int k = 0; k < 3; k++)
					s += Rc[k * 3 + r] * Rp[k * 3 + c]; // Rc^T Rp
				M[r * 3 + c] = s;
			}
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += Rc[k * 3 + r] * (prevPose_[k * 4 + 3] - pose_[k * 4 + 3]);
			tM[r] = s;
		}
		{
			double tr = M[0] + M[4] + M[8], ang = acos(std::max(-1.0, std::min(1.0, (tr - 1) / 2))); // rotation angle, radians
			double tl = sqrt(tM[0] * tM[0] + tM[1] * tM[1] + tM[2] * tM[2]);
			motion = (float)std::min(1.0, std::max(ang / 0.012, tl / 0.02)); // 0.7 degrees or 2 cm = moving
		}
		// prediction of current cell j: its direction seen from the previous frame is R_prev^-1 R_cur d = M^T d (rotation part)
		for (int j = 0; j < cells; j++) {
			const double *d = &dirs_[j * 3];
			double dp[3] = {M[0] * d[0] + M[3] * d[1] + M[6] * d[2], M[1] * d[0] + M[4] * d[1] + M[7] * d[2],
			                M[2] * d[0] + M[5] * d[1] + M[8] * d[2]};
			int i = CellOf(dp);
			if (i < 0)
				continue;
			const bool fillOnly = prevInv_[i] <= 0 || prevAge_[i] > p.maxAge; // a carried membrane value, not a measurement
			const float carried = fillOnly ? prevFill_[i] : prevInv_[i];
			if (carried <= 0)
				continue;
			double r = 1.0 / carried, pp[3] = {dp[0] * r, dp[1] * r, dp[2] * r};
			double pc[3] = {M[0] * pp[0] + M[1] * pp[1] + M[2] * pp[2] + tM[0], M[3] * pp[0] + M[4] * pp[1] + M[5] * pp[2] + tM[1],
			                M[6] * pp[0] + M[7] * pp[1] + M[8] * pp[2] + tM[2]};
			double rc = sqrt(Dot(pc, pc));
			if (rc < p.nearM || rc > p.farM * 1.5)
				continue;
			if (fillOnly) {
				predFill[j] = (float)(1.0 / rc);
				continue;
			}
			predInv[j] = (float)(1.0 / rc);
			double q = p.processNoise * predInv[j];
			predVar[j] = prevVar_[i] + (float)(q * q);
			predAge[j] = (uint8_t)std::min<int>(255, prevAge_[i] + 1);
		}
	}

	// 3. measure, fuse
	std::fill(has_.begin(), has_.end(), 0);
	for (int j = 0; j < cells; j++) {
		const int n = cellCount_[j];
		bool meas = false;
		float z = 0, vz = 0, cq = 0;
		if (n >= p.minSamples) {
			float *s = &sorted[cellStart_[j]];
			std::sort(s, s + n);
			float rlow = s[std::min(n - 1, (int)(p.foregroundQuantile * n))];
			float r25 = s[n / 4], r75 = s[std::min(n - 1, (3 * n) / 4)], med = s[n / 2];
			// the low quantile is biased by the sample count and the noise (a few noisy samples always have a low one): follow it
			// only as far as it sits clearly below the median (a real nearer surface in the cell), else take the median
			float w = std::min(1.0f, std::max(0.0f, ((med - rlow) / med - (float)p.edgeBiasMin) / (float)p.edgeBiasRamp));
			float rq = med + (rlow - med) * w;
			float spread = (r75 - r25) / std::max(0.1f, med);
			float cn = std::min(1.0f, (n - p.minSamples + 1) / 6.0f), cs = std::max(0.0f, 1.0f - spread / 0.5f);
			cq = cn * (0.4f + 0.6f * cs);
			z = 1.0f / rq;
			float sig = (float)p.fuseNoise * z / (0.3f + 0.7f * cq);
			vz = sig * sig;
			meas = true;
		}
		const bool pred = predInv[j] > 0;
		if (meas && pred && fabsf(z - predInv[j]) < (float)p.fuseGate * std::max(z, predInv[j])) {
			float K = predVar[j] / (predVar[j] + vz);
			// deadband: an innovation within the measurement's own noise is mostly noise; let it through quadratically
			float e = z - predInv[j], sg = sqrtf(vz) * (float)p.deadband;
			if (fabsf(e) < sg)
				e *= fabsf(e) / sg;
			inv_[j] = predInv[j] + K * e;
			var_[j] = (1 - K) * predVar[j];
			src_[j] = kFused, age_[j] = 0, conf_[j] = std::min(1.0f, cq + 0.2f);
			has_[j] = 1, stats_.fused++;
		} else if (meas) {
			inv_[j] = z, var_[j] = vz, src_[j] = kMeasured, age_[j] = 0, conf_[j] = cq;
			has_[j] = 1, stats_.measured++;
		} else if (pred) {
			inv_[j] = predInv[j], var_[j] = predVar[j], src_[j] = kFused, age_[j] = predAge[j];
			conf_[j] = 0.3f * (1.0f - (float)predAge[j] / (float)p.maxAge);
			has_[j] = 1, stats_.fused++;
		} else {
			src_[j] = kNone, conf_[j] = 0, age_[j] = 255;
		}
	}

	// 4. the floor: a measurement cannot be behind it; unmeasured downward cells take it
	if (haveFloor_) {
		for (int j = 0; j < cells; j++) {
			const double *d = &dirs_[j * 3];
			double nd = floor_[0] * d[0] + floor_[1] * d[1] + floor_[2] * d[2];
			if (nd > -0.05)
				continue;
			double rf = -floor_[3] / nd;
			if (rf < p.nearM || rf > p.farM)
				continue;
			if (has_[j]) {
				if (1.0 / inv_[j] > rf * 1.08)
					inv_[j] = (float)(1.0 / rf), var_[j] = std::max(var_[j], 1e-4f);
			} else {
				inv_[j] = (float)(1.0 / rf), var_[j] = 1e-2f, src_[j] = kFloor, age_[j] = 0, conf_[j] = 0;
				has_[j] = 1, stats_.floor++;
			}
		}
	}

	// 5. whatever is still unknown: a membrane in inverse depth between the known cells (as stereo_depth.cpp)
	{
		const int C = p.cols, R = p.rows;
		int known = 0;
		double mean = 0;
		for (int j = 0; j < cells; j++)
			if (has_[j])
				known++, mean += inv_[j];
		const float defInv = 1.0f / (float)p.defaultDepthM;
		const float start = known ? (float)(mean / known) : defInv;
		std::vector<float> a(cells), b(cells);
		for (int j = 0; j < cells; j++)
			a[j] = has_[j] ? inv_[j] : start; // no warm start from last frame's fill: 80 passes would leave it stale
		for (int pass = 0; known && known < cells && pass < 80; pass++) {
			for (int r = 0; r < R; r++)
				for (int c = 0; c < C; c++) {
					int j = r * C + c;
					if (has_[j]) {
						b[j] = a[j];
						continue;
					}
					float sum = 0;
					int m = 0;
					if (r > 0)
						sum += a[j - C], m++;
					if (r < R - 1)
						sum += a[j + C], m++;
					if (c > 0)
						sum += a[j - 1], m++;
					if (c < C - 1)
						sum += a[j + 1], m++;
					b[j] = sum / m;
				}
			a.swap(b);
		}
		for (int j = 0; j < cells; j++) {
			if (!has_[j]) {
				float v = a[j];
				// the membrane re-solves a little differently every frame: blend it into last frame's, unless it jumped
				if (predFill[j] > 0 && fabsf(v - predFill[j]) < 0.35f * std::max(v, predFill[j]))
					v = predFill[j] + (float)(p.fillBlend + (1 - p.fillBlend) * motion) * (v - predFill[j]);
				inv_[j] = v, var_[j] = 1.0f, src_[j] = kFilled, conf_[j] = 0, age_[j] = 255;
				stats_.filled++;
			}
		}
		// output: an edge-preserving 3x3 average in inverse depth (the state above stays unsmoothed)
		for (int r = 0; r < R; r++)
			for (int c = 0; c < C; c++) {
				const int j = r * C + c;
				const float v0 = inv_[j];
				float sum = 2 * v0, wsum = 2;
				if (p.smoothRel > 0) {
					const float tol = (float)p.smoothRel * v0;
					for (int dr = -1; dr <= 1; dr++)
						for (int dc = -1; dc <= 1; dc++) {
							const int rr = r + dr, cc = c + dc;
							if ((!dr && !dc) || rr < 0 || rr >= R || cc < 0 || cc >= C)
								continue;
							const float v = inv_[rr * C + cc], d = (v - v0) / tol, wk = (dr && dc ? 0.5f : 1.0f) / (1.0f + d * d);
							sum += wk * v, wsum += wk;
						}
				}
				tmp_[j] = sum / wsum;
			}
		for (int j = 0; j < cells; j++)
			depth_[j] = 1.0f / std::max(tmp_[j], 1e-3f);
	}

	// 6. remember (only measured/fused/floor cells carry over, not the membrane)
	for (int j = 0; j < cells; j++) {
		bool keep = src_[j] == kMeasured || src_[j] == kFused || src_[j] == kFloor;
		prevInv_[j] = keep ? inv_[j] : 0, prevVar_[j] = var_[j], prevAge_[j] = keep ? age_[j] : 255;
		prevFill_[j] = src_[j] == kFilled ? inv_[j] : 0;
	}
	memcpy(prevPose_, pose_, sizeof(prevPose_));
	havePrevPose_ = havePose_;
	haveHistory_ = true;
}
