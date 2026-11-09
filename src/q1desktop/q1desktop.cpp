// q1desktop: an X display (an Xvfb) as a SteamVR dashboard overlay, with mouse, scroll and keyboard input.
// On the Steam Frame, desktop mode is a nested Plasma inside a gamescope whose OpenVR backend shows it as the
// dashboard overlay "valve.plasma.desktopmode". The Quest 1's Adreno 540 blob can't run gamescope (no dma-buf
// import / modifiers), so this plays that role for a Plasma X11 session on its own Xvfb (docs/desktop.md):
//  - pixels: Xvfb exports its framebuffer as an XWD file (-fbdir), mapped here; XDamage says what changed;
//    only the changed rows are copied (alpha forced opaque) into a Vulkan image given to SetOverlayTexture.
//    Nothing is copied while the dashboard is hidden or the screen is idle.
//  - input: the dashboard laser (overlay mouse events), the trackpad/stick scroll and the SteamVR keyboard
//    (control bar keyboard button) become XTest events on that Xvfb.
// Usage (as steamos, SteamVR running): DISPLAY=:2 q1desktop <xwd file>
// Env: Q1D_KEY (valve.plasma.desktopmode), Q1D_NAME (Desktop), Q1D_WIDTH (overlay width in m, 2.67 like the
// Frame), Q1D_ICON (dashboard thumbnail, png), Q1D_HZ (max uploads per second, 60), Q1D_SHOW=0 (don't open
// the dashboard on it at start), Q1D_DEBUG=1.
// Exits when the overlay is closed from its control bar (exit 0) or SteamVR quits (exit 1).
#include "q1overlay.h"

#include <X11/extensions/Xdamage.h>

#include <poll.h>
#include <signal.h>

static volatile sig_atomic_t g_show; // SIGUSR1: open the dashboard on the overlay (a second launch)

int main(int argc, char **argv)
{
	g_prog = "q1desktop";
	if (argc < 2) {
		fprintf(stderr, "usage: DISPLAY=:N %s <Xvfb framebuffer xwd file>\n", argv[0]);
		return 2;
	}
	g_debug = getenv("Q1D_DEBUG") && *getenv("Q1D_DEBUG") == '1';
	// gamescope's keys: a window of an app Steam runs is "valve.steam.desktopgame.<appid>" (its
	// --vr-app-overlay-key), which the dashboard ties to that running app; the Frame's desktop session (not
	// launched by Steam) is "valve.plasma.desktopmode"
	std::string key_s = "valve.plasma.desktopmode";
	unsigned long long appid = getenv("SteamAppId") ? strtoull(getenv("SteamAppId"), nullptr, 10) : 0;
	if (!appid && getenv("SteamGameId")) // non-Steam shortcuts: the 64-bit game id carries the app id on top
		appid = strtoull(getenv("SteamGameId"), nullptr, 10) >> 32;
	if (appid)
		key_s = "valve.steam.desktopgame." + std::to_string(appid);
	if (getenv("Q1D_KEY"))
		key_s = getenv("Q1D_KEY");
	const char *key = key_s.c_str();
	DBG("overlay key %s (SteamAppId %s, SteamGameId %s)\n", key, getenv("SteamAppId") ?: "-",
	    getenv("SteamGameId") ?: "-");
	const char *name = getenv("Q1D_NAME") ? getenv("Q1D_NAME") : "Desktop";
	float width_m = getenv("Q1D_WIDTH") ? atof(getenv("Q1D_WIDTH")) : 2.67f;
	double min_dt = 1.0 / (getenv("Q1D_HZ") ? std::max(1, atoi(getenv("Q1D_HZ"))) : 60);

	XInput xi;
	xi.dpy = XOpenDisplay(nullptr);
	if (!xi.dpy)
		die("cannot open the X display (DISPLAY)");
	int dmg_ev, dmg_err, xt_ev, xt_err, xt_maj, xt_min;
	if (!XDamageQueryExtension(xi.dpy, &dmg_ev, &dmg_err))
		die("no DAMAGE extension on the X server");
	if (!XTestQueryExtension(xi.dpy, &xt_ev, &xt_err, &xt_maj, &xt_min))
		die("no XTEST extension on the X server");
	Window root = DefaultRootWindow(xi.dpy);
	XDamageCreate(xi.dpy, root, XDamageReportRawRectangles);
	x_find_scratch(xi);

	Xwd fb = xwd_open(argv[1]);
	DBG("framebuffer %ux%u stride %u %s\n", fb.width, fb.height, fb.stride, fb.bgr ? "BGRX" : "RGBX");

	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Overlay);
	if (err != VRInitError_None) {
		fprintf(stderr, "q1desktop: VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}

	VROverlayHandle_t ov = k_ulOverlayHandleInvalid, thumb = k_ulOverlayHandleInvalid;
	EVROverlayError oe = VROverlay()->CreateDashboardOverlay(key, name, &ov, &thumb);
	if (oe == VROverlayError_KeyInUse && appid && !getenv("Q1D_KEY")) {
		// Steam's own game theater (steamui: "-legacygame ... -overlaykey valve.steam.desktopgame.<appid>")
		// may hold the app's key
		fprintf(stderr, "q1desktop: %s is taken, using valve.plasma.desktopmode\n", key);
		key_s = "valve.plasma.desktopmode";
		key = key_s.c_str();
		oe = VROverlay()->CreateDashboardOverlay(key, name, &ov, &thumb);
	}
	if (oe != VROverlayError_None) {
		fprintf(stderr, "q1desktop: CreateDashboardOverlay(%s): %s\n", key,
			VROverlay()->GetOverlayErrorNameFromEnum(oe));
		return 1;
	}
	if (getenv("Q1D_ICON") && thumb != k_ulOverlayHandleInvalid)
		VROverlay()->SetOverlayFromFile(thumb, getenv("Q1D_ICON"));
	overlay_setup(ov, width_m, fb.width, fb.height);

	Gfx g;
	gfx_init(g, (VkDeviceSize)fb.width * fb.height * 4);
	Surface surf;
	surface_init(g, surf, fb.width, fb.height, fb.bgr);
	VRVulkanTextureData_t vk;
	Texture_t tex = surface_texture(g, surf, vk);

	upload(g, surf, fb.pixels, fb.stride, 0, fb.height);
	VROverlay()->SetOverlayTexture(ov, &tex);
	if (!getenv("Q1D_SHOW") || *getenv("Q1D_SHOW") != '0')
		VROverlay()->ShowDashboard(key);

	signal(SIGUSR1, [](int) { g_show = 1; });

	// dirty rows from XDamage, flushed at most every min_dt while the overlay is seen
	uint32_t dirty0 = fb.height, dirty1 = 0;
	double last = 0;
	int xfd = ConnectionNumber(xi.dpy);
	unsigned uploads = 0;
	double stat_t = now_s(), up_s = 0, set_s = 0, rows = 0;
	for (;;) {
		while (XPending(xi.dpy)) {
			XEvent e;
			XNextEvent(xi.dpy, &e);
			if (e.type == dmg_ev + XDamageNotify) {
				auto *d = (XDamageNotifyEvent *)&e;
				int y0 = std::max(0, (int)d->area.y), y1 = std::min((int)fb.height, d->area.y + d->area.height);
				if (y1 > y0) {
					dirty0 = std::min(dirty0, (uint32_t)y0);
					dirty1 = std::max(dirty1, (uint32_t)y1);
				}
			}
		}

		VREvent_t ev;
		while (VROverlay()->PollNextOverlayEvent(ov, &ev, sizeof(ev))) {
			if (x_input_event(xi, ev, fb.width, fb.height))
				continue;
			switch (ev.eventType) {
			case VREvent_OverlayClosed:
				DBG("closed from the control bar\n");
				VR_Shutdown();
				return 0;
			case VREvent_Quit:
				VR_Shutdown();
				return 1;
			default:
				DBG("overlay event %u\n", ev.eventType);
				break;
			}
		}
		// SteamVR going away: the system-wide quit event comes to the app, not the overlay
		while (VRSystem()->PollNextEvent(&ev, sizeof(ev)))
			if (ev.eventType == VREvent_Quit) {
				VRSystem()->AcknowledgeQuit_Exiting();
				VR_Shutdown();
				return 1;
			}

		if (g_show) {
			g_show = 0;
			VROverlay()->ShowDashboard(key);
		}

		double t = now_s();
		if (dirty1 > dirty0 && t - last >= min_dt && VROverlay()->IsOverlayVisible(ov)) {
			rows += dirty1 - dirty0;
			if (upload(g, surf, fb.pixels, fb.stride, dirty0, dirty1)) {
				double t1 = now_s();
				VROverlay()->SetOverlayTexture(ov, &tex);
				up_s += t1 - t;
				set_s += now_s() - t1;
				uploads++;
			}
			dirty0 = fb.height;
			dirty1 = 0;
			last = t;
		}
		if (g_debug && t - stat_t >= 5) {
			DBG("%.1f uploads/s, %.0f rows damaged, %.0f uploaded, upload %.2f ms (CPU %.2f), SetOverlayTexture %.2f ms each\n",
			    uploads / (t - stat_t), uploads ? rows / uploads : 0, uploads ? g_changed_rows / uploads : 0, uploads ? up_s * 1e3 / uploads : 0, uploads ? g_cpu_s * 1e3 / uploads : 0,
			    uploads ? set_s * 1e3 / uploads : 0);
			uploads = 0;
			up_s = set_s = rows = g_changed_rows = g_cpu_s = 0;
			stat_t = t;
		}

		// sleep until X has something or the next upload slot (overlay events are polled at least at 60 Hz)
		pollfd p = {xfd, POLLIN, 0};
		poll(&p, 1, 16);
	}
}
