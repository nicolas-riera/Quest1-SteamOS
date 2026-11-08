#include "stereo_depth.h"

#include <math.h>
#include <stdlib.h>

#include <algorithm>

namespace
{
const int kPatch = 2; // 5x5

void Downsample(const uint8_t *src, int stride, int dw, int dh, uint8_t *dst)
{
	for (int y = 0; y < dh; y++) {
		const uint8_t *a = src + (size_t)2 * y * stride, *b = a + stride;
		uint8_t *o = dst + (size_t)y * dw;
		for (int x = 0; x < dw; x++)
			o[x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
	}
}

float Median(float *v, int n)
{
	std::sort(v, v + n);
	return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

float Clamp01(float x) { return std::max(0.0f, std::min(1.0f, x)); }
} // namespace

bool StereoDepth::Configure(const Params &p, ProjectFn left, ProjectFn right, int imageW, int imageH)
{
	params_ = p;
	haveHistory_ = false;
	if (p.cols < 3 || p.rows < 3 || p.hypotheses < 4 || p.nearM <= 0 || p.farM <= p.nearM || imageW < 32 || imageH < 32 ||
	    !left || !right)
		return false;
	w_ = imageW / 2, h_ = imageH / 2;
	hyp_ = p.hypotheses;
	const int cells = p.cols * p.rows;
	dirs_.assign(cells * 3, 0);
	for (int r = 0; r < p.rows; r++)
		for (int c = 0; c < p.cols; c++) {
			double az = (p.azMinDeg + (p.azMaxDeg - p.azMinDeg) * (c + 0.5) / p.cols) * M_PI / 180;
			double el = (p.elMaxDeg - (p.elMaxDeg - p.elMinDeg) * (r + 0.5) / p.rows) * M_PI / 180;
			double *d = &dirs_[(r * p.cols + c) * 3];
			d[0] = sin(az) * cos(el), d[1] = sin(el), d[2] = -cos(az) * cos(el);
		}
	invDepth_.resize(hyp_);
	for (int k = 0; k < hyp_; k++)
		invDepth_[k] = (float)(1 / p.farM + (1 / p.nearM - 1 / p.farM) * k / (hyp_ - 1));

	offL_.assign((size_t)cells * hyp_, -1);
	offR_.assign((size_t)cells * hyp_, -1);
	// Raw pixel x lies in downsampled pixel (x - 0.5) / 2 (pixel centres at integers); a 5x5 patch needs a margin.
	auto toOff = [&](const ProjectFn &fn, const double pt[3]) -> int32_t {
		double px, py;
		if (!fn(pt, px, py))
			return -1;
		int x = (int)floor((px - 0.5) / 2 + 0.5), y = (int)floor((py - 0.5) / 2 + 0.5);
		if (x < kPatch || y < kPatch || x >= w_ - kPatch || y >= h_ - kPatch)
			return -1;
		return y * w_ + x;
	};
	for (int i = 0; i < cells; i++)
		for (int k = 0; k < hyp_; k++) {
			double d = 1.0 / invDepth_[k], pt[3] = {dirs_[i * 3] * d, dirs_[i * 3 + 1] * d, dirs_[i * 3 + 2] * d};
			offL_[(size_t)i * hyp_ + k] = toOff(left, pt);
			offR_[(size_t)i * hyp_ + k] = toOff(right, pt);
		}
	dsL_.assign((size_t)w_ * h_, 0);
	dsR_.assign((size_t)w_ * h_, 0);
	depth_.assign(cells, (float)p.defaultDepthM);
	rawDepth_.assign(cells, 0);
	conf_.assign(cells, 0);
	rawConf_.assign(cells, 0);
	med_.assign(cells, 0);
	cost_.assign(hyp_, 0);
	valid_.assign(cells, 0);
	have_.assign(cells, 0), h2_.assign(cells, 0), o2_.assign(cells, 0);
	return true;
}

bool StereoDepth::Compute(const uint8_t *left, const uint8_t *right, int stride)
{
	if (!left || !right || w_ == 0)
		return false;
	Downsample(left, stride, w_, h_, dsL_.data());
	Downsample(right, stride, w_, h_, dsR_.data());

	const int cells = params_.cols * params_.rows, C = params_.cols, Rr = params_.rows;
	int po[25], n = 0;
	for (int dy = -kPatch; dy <= kPatch; dy++)
		for (int dx = -kPatch; dx <= kPatch; dx++)
			po[n++] = dy * w_ + dx;
	const uint8_t *L = dsL_.data(), *R = dsR_.data();
	float *cost = cost_.data();
	stats_ = Stats();

	for (int i = 0; i < cells; i++) {
		const int32_t *oL = &offL_[(size_t)i * hyp_], *oR = &offR_[(size_t)i * hyp_];
		int nvalid = 0, bi = -1, nvis = 0;
		float best = 2, varBest = 0;
		for (int k = 0; k < hyp_; k++) {
			float c = 2; // worse than any real cost (1 - ZNCC is in [0, 2])
			if (oL[k] >= 0 && oR[k] >= 0) {
				nvis++;
				const uint8_t *a = L + oL[k], *b = R + oR[k];
				int sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
				for (int j = 0; j < 25; j++) {
					int x = a[po[j]], y = b[po[j]];
					sa += x, sb += y, saa += x * x, sbb += y * y, sab += x * y;
				}
				int va = 25 * saa - sa * sa, vb = 25 * sbb - sb * sb; // 625 x variance
				// flat patches (sensor noise only) carry no information
				if (va > 625 * 12 && vb > 625 * 12) {
					c = 1.0f - (float)(25 * sab - sa * sb) / sqrtf((float)va * (float)vb);
					nvalid++;
					if (c < best)
						best = c, bi = k, varBest = (float)va / 625;
				}
			}
			cost[k] = c;
		}
		rawConf_[i] = 0;
		rawDepth_[i] = 0;
		stats_.visible += nvis > 0;
		if (bi < 0 || nvalid < 4)
			continue;
		stats_.textured++, stats_.meanZncc += 1 - best;
		// uniqueness: the winner against the best hypothesis outside its neighbourhood
		float second = 2;
		for (int k = 0; k < hyp_; k++)
			if (abs(k - bi) > 2 && cost[k] < second)
				second = cost[k];
		if (second >= 2)
			second = 1;
		// parabola through the winner and its neighbours, in inverse depth
		float pos = (float)bi;
		if (bi > 0 && bi < hyp_ - 1 && cost[bi - 1] < 2 && cost[bi + 1] < 2) {
			float a = cost[bi - 1], b = cost[bi], c = cost[bi + 1], den = a - 2 * b + c;
			if (den > 1e-6f)
				pos += std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den));
		}
		rawDepth_[i] = 1.0f / (invDepth_[0] + (invDepth_[hyp_ - 1] - invDepth_[0]) * pos / (hyp_ - 1));
		float zn = Clamp01((0.35f - best) / 0.25f); // ZNCC 0.65 -> 0, 0.9 -> 1
		float uq = Clamp01((second - best) / 0.15f);
		float tx = Clamp01(varBest / 150);
		rawConf_[i] = zn * uq * (0.5f + 0.5f * tx);
	}

	if (stats_.textured)
		stats_.meanZncc /= stats_.textured;
	// 3x3 median over the confident cells (a confident cell with no confident neighbour is an outlier)
	const float minc = (float)params_.minConfidence;
	for (int i = 0; i < cells; i++)
		valid_[i] = rawConf_[i] >= minc;
	for (int r = 0; r < Rr; r++)
		for (int c = 0; c < C; c++) {
			float v[9];
			int m = 0, i = r * C + c;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int rr = r + dy, cc = c + dx;
					if (rr >= 0 && rr < Rr && cc >= 0 && cc < C && valid_[rr * C + cc])
						v[m++] = rawDepth_[rr * C + cc];
				}
			bool ok = valid_[i] ? m >= 2 : m >= 3;
			med_[i] = ok ? Median(v, m) : 0;
			have_[i] = ok;
			if (!ok)
				valid_[i] = 0;
		}
	// Unknown cells: a membrane in inverse depth between the measured ones (Jacobi passes of the 4-neighbour mean, the
	// measured cells held), then one 3x3 blur. A hole filled with the default depth next to a near measured surface (the
	// desk right under the cameras, 2026-11-08) made a jump of metres between neighbouring cells: long stretched
	// triangles, smears across the passthrough. With nothing measured at all, everything is at the default depth.
	int known = 0;
	double meanInv = 0;
	for (int i = 0; i < cells; i++)
		if (have_[i]) {
			float d = std::max((float)params_.nearM, std::min((float)params_.farM, med_[i]));
			o2_[i] = 1.0f / d, known++, meanInv += o2_[i];
		}
	const float defInv = 1.0f / (float)params_.defaultDepthM;
	const float startInv = known ? (float)(meanInv / known) : defInv;
	for (int i = 0; i < cells; i++)
		if (!have_[i])
			o2_[i] = known ? startInv : defInv;
	for (int pass = 0; known && pass < 60; pass++) {
		for (int r = 0; r < Rr; r++)
			for (int c = 0; c < C; c++) {
				int i = r * C + c;
				if (have_[i]) {
					med_[i] = o2_[i];
					continue;
				}
				float sum = 0;
				int m = 0;
				if (r > 0)
					sum += o2_[i - C], m++;
				if (r < Rr - 1)
					sum += o2_[i + C], m++;
				if (c > 0)
					sum += o2_[i - 1], m++;
				if (c < C - 1)
					sum += o2_[i + 1], m++;
				med_[i] = sum / m;
			}
		o2_.swap(med_);
	}
	for (int r = 0; r < Rr; r++)
		for (int c = 0; c < C; c++) {
			float sum = 0;
			int m = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int rr = r + dy, cc = c + dx;
					if (rr >= 0 && rr < Rr && cc >= 0 && cc < C)
						sum += o2_[rr * C + cc], m++;
				}
			med_[r * C + c] = sum / m;
		}
	o2_.swap(med_);
	const float a = (float)params_.smoothing, ratio = (float)params_.resetRatio;
	for (int i = 0; i < cells; i++) {
		float d = 1.0f / o2_[i];
		if (haveHistory_) {
			float q = d / depth_[i];
			if (q < ratio && q > 1 / ratio) // blend in inverse depth: disparity noise is what is averaged out
				d = 1.0f / ((1 - a) / depth_[i] + a / d);
		}
		depth_[i] = d;
		conf_[i] = valid_[i] ? rawConf_[i] : 0;
	}
	haveHistory_ = true;
	return true;
}
