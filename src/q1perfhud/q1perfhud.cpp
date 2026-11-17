// Performance overlay for Steam's quick menu ("Performance Overlay Level"), as a head-locked SteamVR overlay.
// On the Steam Frame that setting drives mangoapp inside gamescope (Steam writes <Steam>/config/mangohud.conf and runs
// `mangohudctl toggle reload_config`); neither exists here (gamescope needs Vulkan 1.3, docs/desktop.md), so this
// reads the same file and draws the numbers itself. Levels follow mangohud's presets, which Steam asks for when the
// client runs with STEAM_USE_MANGOAPP=1 STEAM_MANGOAPP_PRESETS_SUPPORTED=1 (device/holo-perfhud.sh sets them):
//   0 hidden   1 FPS   2 + frame times, reprojection   3 + GPU/CPU load, clocks, temperatures   4 + RAM, battery, drops
// Data: SteamVR's cumulative compositor stats (frames the application submitted, reprojected, GPU times) and the
// kernel's own files (KGSL, cpufreq, thermal zones, /proc). Text is drawn with a 5x7 bitmap font into a small
// texture pushed as a 32-bit TGA through SetOverlayFromFile (the path that does not leak, camoverlay.cpp).
// Usage: q1-perfhud (as steamos, SteamVR running; retried by systemd). Env: PERFHUD_CONF (mangohud.conf path),
// PERFHUD_DIST (m, 1.0), PERFHUD_X, PERFHUD_Y (m, -0.28 / 0.22: top left of the view), PERFHUD_WIDTH (m, 0.26),
// PERFHUD_LEVEL (fixed level, ignores the file), PERFHUD_DEBUG=1.
#include <openvr.h>

#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vr;

// 5x7 glyphs, one byte per row (bit 4 = leftmost column): digits, capitals and the few signs the lines use
struct Glyph {
	char c;
	unsigned char r[7];
};
static const Glyph kFont[] = {
	{'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
	{'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
	{'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
	{'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
	{'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
	{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
	{'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
	{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
	{'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
	{'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
	{'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
	{'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
	{'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
	{'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
	{'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
	{'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
	{'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
	{'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
	{'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
	{'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
};

static const unsigned char *GlyphFor(char c)
{
	if (c >= 'a' && c <= 'z')
		c -= 32;
	for (const Glyph &g : kFont)
		if (g.c == c)
			return g.r;
	return nullptr; // space and anything else: blank
}

static const int kScale = 2, kCols = 24, kCellW = 6 * kScale, kCellH = 9 * kScale, kPad = 4;

// BGRA, top-down
static void Render(const std::vector<std::string> &lines, int W, int H, std::vector<uint8_t> &px)
{
	px.assign((size_t)W * H * 4, 0);
	for (size_t i = 0; i < px.size(); i += 4) { // translucent black
		px[i + 3] = 150;
	}
	for (size_t li = 0; li < lines.size(); li++)
		for (size_t ci = 0; ci < lines[li].size() && (int)ci < kCols; ci++) {
			const unsigned char *g = GlyphFor(lines[li][ci]);
			if (!g)
				continue;
			for (int row = 0; row < 7; row++)
				for (int col = 0; col < 5; col++) {
					if (!(g[row] & (0x10 >> col)))
						continue;
					for (int sy = 0; sy < kScale; sy++)
						for (int sx = 0; sx < kScale; sx++) {
							int x = kPad + (int)ci * kCellW + col * kScale + sx;
							int y = kPad + (int)li * kCellH + row * kScale + sy;
							uint8_t *p = &px[((size_t)y * W + x) * 4];
							p[0] = p[1] = p[2] = p[3] = 255;
						}
				}
		}
}

static void WriteTga(const char *tmp, const char *dst, const std::vector<uint8_t> &px, int W, int H)
{
	uint8_t hdr[18] = {};
	hdr[2] = 2;
	hdr[12] = W & 255, hdr[13] = W >> 8, hdr[14] = H & 255, hdr[15] = H >> 8;
	hdr[16] = 32, hdr[17] = 8 | 0x20; // 8 alpha bits, top-left origin
	if (FILE *f = fopen(tmp, "wb")) {
		fwrite(hdr, 1, sizeof(hdr), f);
		fwrite(px.data(), 1, px.size(), f);
		fclose(f);
		rename(tmp, dst);
	}
}

static std::string ReadFile(const std::string &p)
{
	std::string s;
	if (FILE *f = fopen(p.c_str(), "r")) {
		char buf[256];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			s.append(buf, n);
		fclose(f);
	}
	return s;
}

static double ReadNum(const std::string &p, double def = -1)
{
	std::string s = ReadFile(p);
	return s.empty() ? def : atof(s.c_str());
}

// Steam's level out of mangohud.conf: "preset=N" (presets supported), else guessed from what it enables
static int LevelFromConf(const std::string &conf, bool *exists)
{
	if (conf.empty()) {
		*exists = false;
		return 0;
	}
	*exists = true;
	int level = -1;
	size_t pos = 0;
	while (pos < conf.size()) {
		size_t e = conf.find('\n', pos);
		std::string line = conf.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
		pos = e == std::string::npos ? conf.size() : e + 1;
		if (line.compare(0, 7, "preset=") == 0)
			level = std::max(-1, atoi(line.c_str() + 7)); // the last one wins
		else if (line == "no_display")
			return 0;
	}
	if (level >= 0)
		return std::min(level, 4);
	if (conf.find("cpu_temp") != std::string::npos)
		return 4;
	if (conf.find("gpu_stats") != std::string::npos || conf.find("frame_timing=1") != std::string::npos)
		return 3;
	if (conf.find("fps_only") != std::string::npos)
		return 1;
	return 2;
}

struct Cpu {
	double idle = 0, total = 0;
};

static bool ReadCpu(Cpu &c)
{
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return false;
	unsigned long long v[8] = {};
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
		       &v[7]);
	fclose(f);
	if (n < 4)
		return false;
	c.idle = v[3] + v[4];
	c.total = 0;
	for (int i = 0; i < 8; i++)
		c.total += v[i];
	return true;
}

static double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static std::string Fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string Fmt(const char *f, ...)
{
	char b[96];
	va_list ap;
	va_start(ap, f);
	vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	return b;
}

int main()
{
	// wait for SteamVR (a Background app starts nothing: VRApplication_Overlay would start vrserver in this cgroup)
	for (;;) {
		bool server = false, comp = false;
		if (DIR *d = opendir("/proc")) {
			while (dirent *de = readdir(d)) {
				char path[64], name[32] = "";
				snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
				if (FILE *f = fopen(path, "r")) {
					if (fscanf(f, "%31s", name) == 1) {
						server |= !strcmp(name, "vrserver");
						comp |= !strcmp(name, "vrcompositor");
					}
					fclose(f);
				}
			}
			closedir(d);
		}
		if (server && comp)
			break;
		sleep(3);
	}
	sleep(10);
	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Background);
	if (err != VRInitError_None) {
		fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	const bool debug = getenv("PERFHUD_DEBUG") != nullptr;
	const std::string confPath = getenv("PERFHUD_CONF") ? getenv("PERFHUD_CONF")
							    : "/home/steamos/.local/share/Steam/config/mangohud.conf";
	const int fixedLevel = getenv("PERFHUD_LEVEL") ? atoi(getenv("PERFHUD_LEVEL")) : -1;
	const float dist = getenv("PERFHUD_DIST") ? atof(getenv("PERFHUD_DIST")) : 1.0f;
	const float ox = getenv("PERFHUD_X") ? atof(getenv("PERFHUD_X")) : -0.28f;
	const float oy = getenv("PERFHUD_Y") ? atof(getenv("PERFHUD_Y")) : 0.22f;
	const float width = getenv("PERFHUD_WIDTH") ? atof(getenv("PERFHUD_WIDTH")) : 0.26f;

	VROverlayHandle_t ov;
	VROverlay()->CreateOverlay("quest1.perfhud", "Performance overlay", &ov);
	const int W = kCols * kCellW + 2 * kPad;
	VROverlay()->SetOverlayWidthInMeters(ov, width);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_SortWithNonSceneOverlays, true);
	VROverlay()->SetOverlaySortOrder(ov, 90);
	VROverlay()->SetOverlayAlpha(ov, 1.0f);

	// sensors: the SoC's tsens zones (0.1 C) and the GPU's own (0.1 C)
	std::vector<std::string> socZones;
	for (int z = 0; z < 64; z++) {
		std::string t = ReadFile("/sys/class/thermal/thermal_zone" + std::to_string(z) + "/type");
		if (t.compare(0, 14, "tsens_tz_senso") == 0)
			socZones.push_back("/sys/class/thermal/thermal_zone" + std::to_string(z) + "/temp");
	}

	bool shown = false;
	int level = 0;
	size_t shownLines = 0;
	Cpu cpu0;
	ReadCpu(cpu0);
	Compositor_CumulativeStats prev = {};
	bool havePrev = false;
	double prevT = Now();
	std::string lastText;
	std::vector<uint8_t> px;
	const char *tmp = "/dev/shm/q1-perfhud.tga.tmp", *dst = "/dev/shm/q1-perfhud.tga";
	for (;;) {
		usleep(500000);
		if (fixedLevel >= 0) {
			level = fixedLevel;
		} else {
			bool exists;
			std::string conf = ReadFile(confPath);
			int nl = LevelFromConf(conf, &exists);
			(void)exists;
			if (nl != level) {
				fprintf(stderr, "perfhud: %s -> level %d\n", confPath.c_str(), nl);
				if (debug)
					fprintf(stderr, "%s\n", conf.c_str());
			}
			level = nl;
		}
		if (level <= 0) {
			if (shown) {
				VROverlay()->HideOverlay(ov);
				shown = false;
				lastText.clear();
			}
			havePrev = false;
			continue;
		}

		std::vector<std::string> L;
		double t = Now(), dt = t - prevT;
		prevT = t;
		Compositor_CumulativeStats st = {};
		bool haveStats = false;
		if (VRCompositor()) {
			VRCompositor()->GetCumulativeStats(&st, sizeof(st));
			haveStats = true;
		}
		double fps = 0, appGpu = 0, cmpGpu = 0, reproj = 0, drops = 0;
		if (haveStats && havePrev && dt > 0 && st.m_nPid == prev.m_nPid) {
			double sub = (double)(st.m_nNumFrameSubmits - prev.m_nNumFrameSubmits);
			double pres = (double)(st.m_nNumFramePresents - prev.m_nNumFramePresents);
			fps = sub / dt;
			if (sub > 0) {
				appGpu = (st.m_flSumApplicationGPUTimeMS - prev.m_flSumApplicationGPUTimeMS) / sub;
				cmpGpu = (st.m_flSumCompositorGPUTimeMS - prev.m_flSumCompositorGPUTimeMS) / sub;
			}
			if (pres > 0)
				reproj = 100.0 * (st.m_nNumReprojectedFrames - prev.m_nNumReprojectedFrames) / pres;
			drops = (double)(st.m_nNumDroppedFrames - prev.m_nNumDroppedFrames);
		}
		prev = st;
		havePrev = haveStats;
		L.push_back(Fmt("FPS %.0f", fps));
		if (level >= 2) {
			L.push_back(Fmt("FRAME %.1f MS", fps > 0 ? 1000.0 / fps : 0.0));
			L.push_back(Fmt("GPU APP %.1f CMP %.1f MS", appGpu, cmpGpu));
			L.push_back(Fmt("REPROJ %.0f%%", reproj));
		}
		if (level >= 3) {
			Cpu c1;
			double load = 0;
			if (ReadCpu(c1) && c1.total > cpu0.total)
				load = 100.0 * (1.0 - (c1.idle - cpu0.idle) / (c1.total - cpu0.total));
			cpu0 = c1;
			double gpuTemp = ReadNum("/sys/class/kgsl/kgsl-3d0/temp") / 10.0;
			double socTemp = 0;
			for (const std::string &z : socZones)
				socTemp = std::max(socTemp, ReadNum(z, 0) / 10.0);
			L.push_back(Fmt("GPU %.0f%% %.0fMHZ %.0fC", ReadNum("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", 0),
					ReadNum("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", 0) / 1e6, gpuTemp));
			L.push_back(Fmt("CPU %.0f%% %.1f/%.1fGHZ %.0fC", load,
					ReadNum("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1e6,
					ReadNum("/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq", 0) / 1e6, socTemp));
		}
		if (level >= 4) {
			double total = 0, avail = 0;
			if (FILE *f = fopen("/proc/meminfo", "r")) {
				char k[64];
				unsigned long v;
				while (fscanf(f, "%63s %lu kB", k, &v) == 2) {
					if (!strcmp(k, "MemTotal:"))
						total = v;
					else if (!strcmp(k, "MemAvailable:"))
						avail = v;
				}
				fclose(f);
			}
			L.push_back(Fmt("RAM %.0f/%.0f MB", (total - avail) / 1024, total / 1024));
			L.push_back(Fmt("BAT %.0f%% %.0fC", ReadNum("/sys/class/power_supply/battery/capacity", 0),
					ReadNum("/sys/class/power_supply/battery/temp", 0) / 10.0));
			L.push_back(Fmt("DROPPED %.0f", drops));
		}
		std::string text;
		for (const std::string &s : L)
			text += s + "\n";
		if (text != lastText) {
			lastText = text;
			const int H = (int)L.size() * kCellH + 2 * kPad;
			Render(L, W, H, px);
			WriteTga(tmp, dst, px, W, H);
			VROverlay()->SetOverlayFromFile(ov, dst);
			if (L.size() != shownLines) { // the overlay's centre: its top left corner stays at (ox, oy)
				shownLines = L.size();
				HmdMatrix34_t m = {{{1, 0, 0, ox + width / 2}, {0, 1, 0, oy - width * H / W / 2}, {0, 0, 1, -dist}}};
				VROverlay()->SetOverlayTransformTrackedDeviceRelative(ov, k_unTrackedDeviceIndex_Hmd, &m);
			}
		}
		if (!shown) {
			VROverlay()->ShowOverlay(ov);
			shown = true;
		}
		if (debug)
			fprintf(stderr, "perfhud: level %d\n%s", level, text.c_str());
	}
}
