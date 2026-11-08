// Coarse per-frame depth from a calibrated stereo pair, as a grid of view directions in the device frame.
// Plane sweep in inverse depth: every grid direction is projected into both images at each depth hypothesis
// (positions precomputed at Configure time), 5x5 ZNCC patches are compared on 2x-downsampled images, the best
// hypothesis is refined to a sub-step, tested for texture and uniqueness, median-filtered, filled from its
// neighbours and smoothed over time. No OpenVR / camera model dependency: the caller supplies the projections.
#pragma once

#include <stdint.h>

#include <functional>
#include <vector>

class StereoDepth
{
public:
	//! device-frame point (x right, y up, -z forward, metres) -> raw pixel of one camera; false if not visible
	using ProjectFn = std::function<bool(const double p_device[3], double &px, double &py)>;

	struct Params
	{
		int cols = 32, rows = 24;
		double azMinDeg = -50, azMaxDeg = 50; //!< horizontal, positive to the right
		double elMinDeg = -50, elMaxDeg = 35; //!< vertical, positive up
		double nearM = 0.3, farM = 6.0;       //!< sweep range (hypotheses are uniform in 1/depth)
		int hypotheses = 48;
		double minConfidence = 0.25;          //!< cells below this are filled from their neighbours
		double defaultDepthM = 3.0;           //!< where nothing confident is around
		double smoothing = 0.5;               //!< temporal blend of the new frame (1 = no smoothing)
		double resetRatio = 1.4;              //!< a cell whose depth changes by more than this factor jumps
	};

	//! imageW/H: size of both raw images. False if the grid or the sweep is unusable.
	bool Configure(const Params &p, ProjectFn left, ProjectFn right, int imageW, int imageH);

	//! Both images 8-bit grayscale, imageW x imageH, same stride. Updates Depth()/Confidence().
	bool Compute(const uint8_t *left, const uint8_t *right, int stride);

	int Cols() const { return params_.cols; }
	int Rows() const { return params_.rows; }
	//! cell index = row * Cols() + col, row 0 = top (highest elevation)
	float Depth(int cell) const { return depth_[cell]; }                 //!< metres along the cell's direction
	float Confidence(int cell) const { return conf_[cell]; }             //!< 0 for cells that were filled in
	const double *Direction(int cell) const { return &dirs_[cell * 3]; } //!< unit vector, device frame
	void ResetHistory() { haveHistory_ = false; }

	//! last Compute(): cells whose sweep was visible in both images, cells with texture, mean best ZNCC of the latter
	struct Stats { int visible = 0, textured = 0; float meanZncc = 0; };
	const Stats &LastStats() const { return stats_; }

private:
	Params params_;
	int w_ = 0, h_ = 0; // downsampled size
	int hyp_ = 0;
	std::vector<double> dirs_;
	std::vector<float> invDepth_;      // per hypothesis
	std::vector<int32_t> offL_, offR_; // [cell * hyp_ + k] centre offset in the downsampled image, -1 = outside
	std::vector<uint8_t> dsL_, dsR_;
	std::vector<float> depth_, conf_, rawDepth_, rawConf_, med_, cost_;
	std::vector<uint8_t> valid_, have_, h2_;
	std::vector<float> o2_;
	bool haveHistory_ = false;
	Stats stats_;
};
