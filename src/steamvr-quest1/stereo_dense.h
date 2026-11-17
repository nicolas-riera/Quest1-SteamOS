// Dense stereo depth for the Quest 1's tracking cameras, as a direction grid in the device frame (the same shape of
// output as stereo_depth.h, which it is meant to replace). See docs/passthrough.md, "Dense stereo depth".
//
// Per camera pair (a "rig": the bottom pair 0/2, optionally the top pair 1/3):
//   1. 2x2 box downsample of both 640x480 frames, then rectification into a common virtual pinhole pair (320x240 by
//      default, one rotation for both so the epipolar lines are image rows), through maps built once from the factory
//      calibration (the caller supplies the projections);
//   2. 5x5 census transform (24 bits), Hamming cost for every disparity, 5x5 box aggregation, winner-takes-all in both
//      directions, parabola sub-pixel, uniqueness test, left-right check, speckle removal;
//   3. every valid pixel becomes a 3D point; the points go into a grid of view directions (device frame), each cell
//      keeps a foreground-favouring quantile of its ranges and a confidence.
// Over time (given the head pose) the cells are fused in inverse depth, and unmeasured cells take the floor plane
// when one is supplied, else a membrane between the measured ones.
// The inner loops are plain contiguous loops over x so the compiler's vectoriser (NEON on the headset) gets them; no
// intrinsics, same result on any machine.
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

class DenseMatcher;

class StereoDenseRig
{
public:
	StereoDenseRig();
	~StereoDenseRig();
	StereoDenseRig(const StereoDenseRig &) = delete;
	StereoDenseRig &operator=(const StereoDenseRig &) = delete;

	//! device-frame point (x right, y up, -z forward, metres) -> raw pixel of one camera; false if not visible
	using ProjectFn = std::function<bool(const double p_device[3], double &px, double &py)>;

	struct Params
	{
		int rectW = 320, rectH = 240;     //!< rectified image size (half of the 640x480 raw frames)
		double focal = 100;               //!< rectified pinhole focal length in rectified pixels
		double nearM = 0.25;              //!< closest depth (sets the disparity range), metres along the rectified axis
		double farM = 10;                 //!< depths beyond are not reported
		int minContrast = 3;              //!< gray levels of local range below which a pixel is not matched (flat, noise only)
		float maxCost = 0.30f;            //!< fraction of the 24 census bits x 25 window that may differ at the winner
		float uniqueness = 0.12f;         //!< (second best - best) / best must exceed this, the second one outside +-1 step
		int lrTolerance = 1;              //!< left-right consistency, in disparity steps
		int speckleMin = 20;              //!< connected pieces (disparity jumps < 1 px) smaller than this are dropped
		int nearMinConf = 60;             //!< 0..255: the near (half size) matcher's results need at least this confidence
		int farDispMax = 16;              //!< disparities below this (depths beyond focal*baseline/this) are matched at the rectified
		                                  //!< size, the closer ones at half size (a quarter of the work, they are blurry anyway)
	};

	//! tl/tr: camera centres in the device frame (left = the one with the smaller x); viewDir: where the rectified
	//! frames look (device frame, need not be a unit vector, must not be parallel to the baseline).
	bool Configure(const Params &p, ProjectFn left, ProjectFn right, const double tl[3], const double tr[3], const double viewDir[3],
	               int imageW, int imageH);

	//! Both frames 8-bit gray imageW x imageH with the same stride. Fills Disparity().
	bool Compute(const uint8_t *left, const uint8_t *right, int stride);

	int Width() const { return W_; }
	int Height() const { return H_; }
	int MaxDisparity() const { return D_; }
	//! Q4 disparity (1/16 px) of the left rectified image; 0 = no valid match
	const uint16_t *Disparity() const { return disp_.data(); }
	//! 0..255 per pixel, 0 where Disparity() is 0
	const uint8_t *Confidence() const { return conf_.data(); }
	const uint8_t *RectifiedLeft() const { return rl_.data(); }
	const uint8_t *RectifiedRight() const { return rr_.data(); }

	//! Rectified pixel (continuous, pixel centres at +0.5) and disparity in pixels -> device-frame point
	void Unproject(double x, double y, double disparity, double p_dev[3]) const;
	//! device-frame direction of a rectified pixel (unit vector)
	void Direction(double x, double y, double d_dev[3]) const;
	double Baseline() const { return B_; }
	double Focal() const { return params_.focal; }

	struct Stats
	{
		int pixels = 0;        //!< pixels inside both rectified views
		int matched = 0;       //!< valid disparities after all tests
		double msRectify = 0, msFarMatch = 0, msNearMatch = 0, msFilter = 0;
	};
	const Stats &LastStats() const { return stats_; }

private:
	Params params_;
	int W_ = 0, H_ = 0, D_ = 0, rawW_ = 0, rawH_ = 0;
	double B_ = 0, cx_ = 0, cy_ = 0;
	double ex_[3] = {1, 0, 0}, ey_[3] = {0, 1, 0}, ez_[3] = {0, 0, 1}, tl_[3] = {0, 0, 0};
	struct Map
	{
		std::vector<int32_t> idx; //!< top-left tap in the half-size raw image, -1 = outside
		std::vector<uint8_t> wx, wy;
	} mapL_, mapR_;
	std::vector<uint8_t> hl_, hr_;                      // half-size raw
	std::vector<uint8_t> rl_, rr_;                      // rectified
	std::vector<uint8_t> inside_, contrast_, valid_;    // both views see the pixel / enough texture / both
	std::unique_ptr<DenseMatcher> far_, near_;
	int farMax_ = 0;
	std::vector<uint8_t> nl_, nr_, nvalid_, nconf_;     // half-size images for the near matcher
	std::vector<uint16_t> ndisp_;
	std::vector<uint16_t> disp_;
	std::vector<uint8_t> conf_;
	std::vector<int32_t> label_, stack_, members_;
	Stats stats_;
};

//! Direction grid fed by one or more rigs, with temporal fusion and a floor prior.
class StereoDense
{
public:
	struct Params
	{
		int cols = 64, rows = 48;
		double azMinDeg = -70, azMaxDeg = 70; //!< horizontal, positive to the right
		double elMinDeg = -85, elMaxDeg = 60; //!< vertical, positive up
		int minSamples = 3;                   //!< valid pixels a cell needs
		double foregroundQuantile = 0.25;     //!< of the cell's ranges: low = favour the nearest surface (Meta: favor_foreground)
		double nearM = 0.25, farM = 10;
		double defaultDepthM = 4.0;           //!< where nothing is known and there is no floor
		double fuseGate = 0.18;               //!< relative inverse-depth change still taken as the same surface
		double fuseNoise = 0.04;              //!< relative inverse-depth noise of one measurement
		double processNoise = 0.02;           //!< relative inverse-depth change allowed per frame (variance growth)
		int maxAge = 4;                       //!< frames a cell is kept without a new measurement
	};

	void Configure(const Params &p);
	//! Rigs are not owned; add before Compute. Each must be configured.
	void AddRig(StereoDenseRig *rig) { rigs_.push_back(rig); }

	//! Head pose at this frame as a row-major 3x4 (device -> world); null = unknown: no temporal propagation.
	void SetPose(const double *world_from_device_3x4);
	//! Floor plane in the device frame: n . p + d = 0 with n pointing up (to the head); null = no floor prior.
	void SetFloor(const double plane_n_d[4]);

	//! Builds the grid from the rigs' current disparities (call after each rig's Compute).
	void Update();

	int Cols() const { return params_.cols; }
	int Rows() const { return params_.rows; }
	float Depth(int cell) const { return depth_[cell]; }       //!< metres along the cell's direction from the device origin
	float Confidence(int cell) const { return conf_[cell]; }   //!< 0 for cells that were filled in
	const double *Direction(int cell) const { return &dirs_[cell * 3]; }
	enum Source : uint8_t { kNone = 0, kMeasured = 1, kFused = 2, kFloor = 3, kFilled = 4 };
	uint8_t CellSource(int cell) const { return src_[cell]; }
	void ResetHistory();

	struct Stats { int measured = 0, fused = 0, floor = 0, filled = 0; };
	const Stats &LastStats() const { return stats_; }

private:
	Params params_;
	std::vector<StereoDenseRig *> rigs_;
	std::vector<double> dirs_;
	std::vector<float> depth_, conf_, inv_, var_, prevInv_, prevVar_, tmp_;
	std::vector<uint8_t> src_, age_, prevAge_, has_;
	std::vector<float> sampleR_;
	std::vector<int32_t> sampleCell_, cellStart_, cellCount_;
	double pose_[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, prevPose_[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
	bool havePose_ = false, havePrevPose_ = false, haveFloor_ = false, haveHistory_ = false;
	double floor_[4] = {0, 1, 0, 0};
	Stats stats_;

	int CellOf(const double dir[3]) const;
};
