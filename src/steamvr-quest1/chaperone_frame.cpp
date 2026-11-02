// See chaperone_frame.h.
#include "chaperone_frame.h"

#include <math.h>
#include <time.h>
#include <unistd.h>

#include <mutex>

namespace
{
std::mutex mtx;
float eyeHeight = 1.65f;
bool anchored = false;   //!< the frame below is valid for the current wearing
bool wasWorn = false;
double lastWornS = 0;    //!< monotonic seconds of the last time the headset was worn
double offSinceS = 0;    //!< when it was taken off (0 = worn)
double yaw = 0;          //!< raw yaw correction: raw = Ry(yaw) * local + t
double t[3] = {0, 0, 0};
bool sixDof = false;

constexpr double kReanchorOffS = 20.0;

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

//! Panels off = headset not worn (Monado creates the file before blanking, removes it at unblank).
bool Worn() { return access("/tmp/quest1-panels-off", F_OK) != 0; }

void Rot(const XrQuaternionf &q, double x, double y, double z, double out[3])
{
	// v + 2w(u x v) + 2 u x (u x v)
	double ux = q.x, uy = q.y, uz = q.z, w = q.w;
	double cx = uy * z - uz * y, cy = uz * x - ux * z, cz = ux * y - uy * x;
	out[0] = x + 2 * (w * cx + uy * cz - uz * cy);
	out[1] = y + 2 * (w * cy + uz * cx - ux * cz);
	out[2] = z + 2 * (w * cz + ux * cy - uy * cx);
}
} // namespace

void Quest1FrameInit(float h)
{
	std::lock_guard<std::mutex> l(mtx);
	eyeHeight = h;
}

void Quest1FrameUpdate(const XrPosef &head, bool positionTracked)
{
	std::lock_guard<std::mutex> l(mtx);
	sixDof = positionTracked;
	double now = Now();
	bool worn = Worn();
	if (worn) {
		if (!wasWorn && offSinceS != 0 && now - offSinceS > kReanchorOffS)
			anchored = false;
		offSinceS = 0;
		lastWornS = now;
	} else if (wasWorn || offSinceS == 0) {
		offSinceS = now;
	}
	wasWorn = worn;
	if (anchored || !positionTracked || !worn || access("/tmp/quest1-no-anchor", F_OK) == 0)
		return;

	// forward direction on the floor plane, also when looking up or down (then the up vector
	// points forward / backward)
	double f[3], u[3];
	Rot(head.orientation, 0, 0, -1, f);
	Rot(head.orientation, 0, 1, 0, u);
	double s = f[1] < 0 ? 1 : -1;
	double hx = f[0] + s * u[0], hz = f[2] + s * u[2];
	if (hx * hx + hz * hz < 1e-4)
		return;
	// Ry(a) maps (0,0,-1) to (-sin a, 0, -cos a): a0 = atan2(-hx, -hz); raw yaw = -a0
	yaw = -atan2(-hx, -hz);
	double c = cos(yaw), sn = sin(yaw);
	const double px = head.position.x, pz = head.position.z;
	// Ry(yaw) * (x, y, z) = (c x + sn z, y, -sn x + c z)
	t[0] = -(c * px + sn * pz);
	t[1] = eyeHeight - head.position.y;
	t[2] = -(-sn * px + c * pz);
	anchored = true;
}

void Quest1FrameApply(vr::DriverPose_t &pose)
{
	std::lock_guard<std::mutex> l(mtx);
	double a = sixDof && anchored ? yaw : 0;
	pose.qWorldFromDriverRotation.w = cos(a / 2);
	pose.qWorldFromDriverRotation.x = 0;
	pose.qWorldFromDriverRotation.y = sin(a / 2);
	pose.qWorldFromDriverRotation.z = 0;
	if (sixDof && anchored) {
		for (int i = 0; i < 3; i++)
			pose.vecWorldFromDriverTranslation[i] = t[i];
	} else {
		pose.vecWorldFromDriverTranslation[0] = pose.vecWorldFromDriverTranslation[2] = 0;
		pose.vecWorldFromDriverTranslation[1] = eyeHeight;
	}
}
