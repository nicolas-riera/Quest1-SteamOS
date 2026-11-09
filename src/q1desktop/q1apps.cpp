// q1apps: the apps Steam starts from its "+" menu, each in its own SteamVR dashboard overlay, as gamescope's
// OpenVR backend does on the Steam Frame. Steam runs them as they are, on its own X display (:1, an Xvfb):
// that display is also the screen Remote Play streams from the headset, which must not show them (on the Frame
// it shows the VR view). So (docs/desktop.md):
//  - a black window covers the whole display, above every app, letting the input through (empty input shape);
//    the X cursor is hidden;
//  - every top-level window of an app is redirected (Composite, automatic): its pixels stay in its own pixmap
//    even under that cover. Steam's own windows are left alone: its VR UI reads them through Composite itself,
//    and their pixmaps changing when q1apps came and went left Steam reading stale ones (no dashboard bar).
//    The apps' pixels are read with MIT-SHM where XDamage says they changed;
//  - windows are grouped by X client (an app's main window, its dialogs and its menus come from one
//    connection): each app gets a dashboard overlay showing its windows on a black screen-sized canvas, its
//    name and icon from its main window. Main windows fill the screen (as gamescope's), dialogs are centered;
//  - the overlay's mouse, scroll and keyboard events become XTest input on :1: pointing at an app's overlay (or
//    the dashboard showing it) raises that app's windows under the cover and gives it the keyboard focus;
//  - closing the overlay from its control bar asks the app's windows to close (WM_DELETE_WINDOW), then kills
//    the client if it is still there 5 s later;
//  - during a Remote Play session (Steam's stream threads exist) it makes SPECT_TRIGGER: Monado then writes the VR
//    view (NV12) to /dev/shm/quest1-spectator and q1screencast streams it from there, as the Frame streams what the
//    headset shows (comp_window_mdp.c spect_tick, src/q1screencast). The cover turns dark blue for the session: that
//    is what the stream shows when no VR frame comes (headset taken off and asleep), as the Frame's stream does. Its
//    cursor is empty, and it takes the pointer 1 s after the last laser input, so no app's cursor is streamed either.
// Usage (as steamos, SteamVR running): DISPLAY=:1 q1apps. Env: Q1A_HZ (max uploads per second per app, 60),
// Q1A_WIDTH (overlay width in m, 2.67), Q1A_DEBUG=1. Exits when SteamVR quits (systemd starts it again).
#include "q1overlay.h"

#include <X11/Xatom.h>
#include <X11/Xlib-xcb.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>


#include <dirent.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <map>
#include <set>

struct Rect {
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // [x0, x1) x [y0, y1)
	bool empty() const { return x1 <= x0 || y1 <= y0; }
	void add(const Rect &r)
	{
		if (r.empty())
			return;
		if (empty()) {
			*this = r;
			return;
		}
		x0 = std::min(x0, r.x0), y0 = std::min(y0, r.y0), x1 = std::max(x1, r.x1), y1 = std::max(y1, r.y1);
	}
	Rect cut(const Rect &r) const
	{
		return {std::max(x0, r.x0), std::max(y0, r.y0), std::min(x1, r.x1), std::min(y1, r.y1)};
	}
};

struct Win {
	XID client = 0;
	bool override_redirect = false;
	int x = 0, y = 0, w = 0, h = 0, depth = 24;
	Damage damage = None;
	Pixmap pixmap = None; // Composite's backing pixmap, named when first read, dropped on resize/unmap
	Rect rect() const { return {x, y, x + w, y + h}; }
};

struct App {
	VROverlayHandle_t ov = k_ulOverlayHandleInvalid, thumb = k_ulOverlayHandleInvalid;
	std::string key, name;
	Surface surf;
	std::vector<uint8_t> canvas; // the app's windows on black, screen-sized, B,G,R,X
	Rect dirty;
	bool icon = false;
	double gone_since = 0, close_at = 0, last_upload = 0, last_check = 0;
};

static Display *dpy;
static Window root, cover;
static int W, H;
static XID client_mask, own_client;
static int dmg_ev, dmg_err;
static std::map<Window, Win> wins; // mapped top-level windows but the cover
static std::map<XID, App> apps;
static std::set<XID> steam_clients; // X clients of Steam itself
static XID front; // the app raised under the cover, with the keyboard focus
static Gfx gfx;
static bool gfx_ready;
static float width_m = 2.67f;
static double min_dt = 1.0 / 60;
static Atom a_pid, a_net_name, a_utf8, a_icon, a_protocols, a_delete;

struct Shm {
	XShmSegmentInfo info{};
	XImage *img = nullptr;
};
static Shm shm24, shm32;

// --- Remote Play spectator ---------------------------------------------------------------------------------

#define SPECT_TRIGGER "/tmp/quest1-spectator"

static struct {
	bool on = false;    // a Remote Play session runs
	double checked = 0; // last look at Steam's threads
	pid_t steam = 0;
	bool cover_catches = false; // the cover takes the pointer (its empty cursor is what gets streamed)
	double last_input = 0;
} spect;

// the headset asleep (taken off: Monado renders nothing) shows as this dark blue in the stream, as on the Frame
#define SPECT_IDLE_RGB 0x1b2838

static bool read_comm(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, (int)n, f) != nullptr;
	fclose(f);
	return ok;
}

// Steam's Remote Play session: its video stream threads exist only while one runs (made when it starts)
static bool remote_play_session()
{
	char path[300], comm[32];
	if (spect.steam) {
		snprintf(path, sizeof(path), "/proc/%d/comm", spect.steam);
		if (!read_comm(path, comm, sizeof(comm)) || strcmp(comm, "steam\n"))
			spect.steam = 0;
	}
	if (!spect.steam) {
		DIR *d = opendir("/proc");
		for (dirent *e; d && (e = readdir(d));) {
			if (e->d_name[0] < '1' || e->d_name[0] > '9')
				continue;
			snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
			struct stat st;
			if (read_comm(path, comm, sizeof(comm)) && !strcmp(comm, "steam\n") && stat(path, &st) == 0 &&
			    st.st_uid == getuid()) {
				spect.steam = atoi(e->d_name);
				break;
			}
		}
		if (d)
			closedir(d);
	}
	if (!spect.steam)
		return false;
	snprintf(path, sizeof(path), "/proc/%d/task", spect.steam);
	DIR *d = opendir(path);
	bool found = false;
	for (dirent *e; d && !found && (e = readdir(d));) {
		if (e->d_name[0] == '.')
			continue;
		char p2[300];
		snprintf(p2, sizeof(p2), "/proc/%d/task/%s/comm", spect.steam, e->d_name);
		found = read_comm(p2, comm, sizeof(comm)) && !strncmp(comm, "CGameStreamVide", 15);
	}
	if (d)
		closedir(d);
	return found;
}

static void cover_input(bool catches)
{
	if (spect.cover_catches == catches)
		return;
	spect.cover_catches = catches;
	if (catches) {
		XFixesSetWindowShapeRegion(dpy, cover, ShapeInput, 0, 0, None); // its whole area
	} else {
		XserverRegion none = XFixesCreateRegion(dpy, nullptr, 0);
		XFixesSetWindowShapeRegion(dpy, cover, ShapeInput, 0, 0, none);
		XFixesDestroyRegion(dpy, none);
	}
	XFlush(dpy);
}

// once a loop: the session's start and end (checked every second)
static void spect_poll(double t)
{
	if (t - spect.checked >= 1) {
		spect.checked = t;
		bool on = remote_play_session();
		if (on != spect.on) {
			spect.on = on;
			fprintf(stderr, "q1apps: Remote Play session %s\n", on ? "started: the VR view is streamed" : "ended");
			if (on) {
				int fd = open(SPECT_TRIGGER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
				if (fd >= 0)
					close(fd);
			} else {
				unlink(SPECT_TRIGGER);
				cover_input(false);
			}
			XSetWindowBackground(dpy, cover, on ? SPECT_IDLE_RGB : BlackPixel(dpy, DefaultScreen(dpy)));
			XClearWindow(dpy, cover);
			XFlush(dpy);
		}
	}
	if (spect.on && !spect.cover_catches && t - spect.last_input > 1)
		cover_input(true);
}

// laser input for an app: the pointer must reach the apps again
static void spect_input(double t)
{
	spect.last_input = t;
	cover_input(false);
}

static int on_x_error(Display *, XErrorEvent *e)
{
	// windows come and go between an event and our requests about them
	DBG("X error %d (request %d.%d) on 0x%lx\n", e->error_code, e->request_code, e->minor_code, e->resourceid);
	return 0;
}

static XID client_of(Window w) { return w & ~client_mask; }

static void shm_init(Shm &s, int depth)
{
	XVisualInfo vi;
	if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), depth, TrueColor, &vi))
		return; // no such visual: no window of that depth either
	s.img = XShmCreateImage(dpy, vi.visual, depth, ZPixmap, nullptr, &s.info, W, H);
	if (!s.img)
		die("XShmCreateImage failed");
	s.info.shmid = shmget(IPC_PRIVATE, (size_t)s.img->bytes_per_line * H, IPC_CREAT | 0600);
	if (s.info.shmid < 0)
		die("shmget failed");
	s.info.shmaddr = s.img->data = (char *)shmat(s.info.shmid, nullptr, 0);
	s.info.readOnly = False;
	if (!XShmAttach(dpy, &s.info))
		die("XShmAttach failed");
	XSync(dpy, False);
	shmctl(s.info.shmid, IPC_RMID, nullptr); // gone once both sides detach
}

static std::string window_name(Window w)
{
	std::string name;
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = nullptr;
	if (XGetWindowProperty(dpy, w, a_net_name, 0, 256, False, a_utf8, &type, &format, &n, &after, &data) ==
	        Success && data && n) {
		name.assign((char *)data, n);
	}
	if (data)
		XFree(data);
	if (name.empty()) {
		XClassHint ch{};
		if (XGetClassHint(dpy, w, &ch)) {
			if (ch.res_class)
				name = ch.res_class;
			XFree(ch.res_name);
			XFree(ch.res_class);
		}
	}
	return name.empty() ? "App" : name;
}

// _NET_WM_ICON (32-bit ARGB, in longs) as the overlay's dashboard thumbnail: the largest up to 256 px
static bool set_icon(App &a, Window w)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = nullptr;
	if (XGetWindowProperty(dpy, w, a_icon, 0, 1 << 20, False, XA_CARDINAL, &type, &format, &n, &after, &data) !=
	        Success || !data) {
		return false;
	}
	const unsigned long *v = (const unsigned long *)data;
	unsigned long best = n, best_w = 0;
	for (unsigned long i = 0; i + 2 <= n;) {
		unsigned long iw = v[i], ih = v[i + 1];
		if (!iw || !ih || i + 2 + iw * ih > n)
			break;
		if (iw <= 256 && iw > best_w) {
			best = i;
			best_w = iw;
		}
		i += 2 + iw * ih;
	}
	bool ok = false;
	if (best < n) {
		unsigned long iw = v[best], ih = v[best + 1];
		std::vector<uint8_t> rgba(iw * ih * 4);
		for (unsigned long p = 0; p < iw * ih; p++) {
			uint32_t c = (uint32_t)v[best + 2 + p];
			rgba[p * 4 + 0] = c >> 16, rgba[p * 4 + 1] = c >> 8, rgba[p * 4 + 2] = c, rgba[p * 4 + 3] = c >> 24;
		}
		ok = VROverlay()->SetOverlayRaw(a.thumb, rgba.data(), iw, ih, 4) == VROverlayError_None;
	}
	XFree(data);
	return ok;
}

static bool is_transient(Window w)
{
	Window t = None;
	return XGetTransientForHint(dpy, w, &t) && t != None;
}

// the app's windows, bottom to top
static std::vector<Window> app_windows(XID client)
{
	std::vector<Window> v;
	Window r, p, *kids = nullptr;
	unsigned n = 0;
	if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {
		for (unsigned i = 0; i < n; i++)
			if (wins.count(kids[i]) && wins[kids[i]].client == client)
				v.push_back(kids[i]);
		XFree(kids);
	}
	return v;
}

static Window main_window(XID client)
{
	Window best = None;
	for (Window w : app_windows(client))
		if (!wins[w].override_redirect && !is_transient(w))
			best = w; // the topmost
	return best;
}

static void raise_cover()
{
	XRaiseWindow(dpy, cover);
}

static void bring_front(XID client)
{
	if (front == client)
		return;
	front = client;
	for (Window w : app_windows(client))
		XRaiseWindow(dpy, w);
	raise_cover();
	Window m = main_window(client);
	if (m)
		XSetInputFocus(dpy, m, RevertToPointerRoot, CurrentTime);
	XFlush(dpy);
	DBG("front: client 0x%lx\n", client);
}

static void mark(XID client, const Rect &r)
{
	auto it = apps.find(client);
	if (it != apps.end())
		it->second.dirty.add(r.cut({0, 0, W, H}));
}

static void app_create(XID client, Window w)
{
	App &a = apps[client];
	char key[64];
	snprintf(key, sizeof(key), "quest1.app.%lx", client);
	a.key = key;
	a.name = window_name(w);
	EVROverlayError oe = VROverlay()->CreateDashboardOverlay(a.key.c_str(), a.name.c_str(), &a.ov, &a.thumb);
	if (oe != VROverlayError_None) {
		fprintf(stderr, "q1apps: CreateDashboardOverlay(%s): %s\n", key, VROverlay()->GetOverlayErrorNameFromEnum(oe));
		a.ov = k_ulOverlayHandleInvalid;
		return;
	}
	overlay_setup(a.ov, width_m, W, H);
	if (!gfx_ready) {
		gfx_init(gfx, (VkDeviceSize)W * H * 4);
		gfx_ready = true;
	}
	surface_init(gfx, a.surf, W, H, true);
	a.canvas.assign((size_t)W * H * 4, 0);
	a.dirty = {0, 0, W, H};
	fprintf(stderr, "q1apps: \"%s\" (client 0x%lx) shown as %s\n", a.name.c_str(), client, key);
	VROverlay()->ShowDashboard(a.key.c_str());
}

static void app_destroy(XID client)
{
	App &a = apps[client];
	fprintf(stderr, "q1apps: \"%s\" gone\n", a.name.c_str());
	if (a.ov != k_ulOverlayHandleInvalid)
		VROverlay()->DestroyOverlay(a.ov);
	if (gfx_ready)
		surface_destroy(gfx, a.surf);
	apps.erase(client);
	if (front == client)
		front = 0;
}

// Steam's own windows (its client and webhelper also draw on :1, e.g. Big Picture's window while it starts): not
// apps it launched, left alone (Steam's UI reaches the headset as its own overlays)
static bool is_steam_window(Window w)
{
	std::string cls;
	XClassHint ch{};
	if (XGetClassHint(dpy, w, &ch)) {
		cls = std::string(ch.res_name ? ch.res_name : "") + "/" + (ch.res_class ? ch.res_class : "");
		XFree(ch.res_name);
		XFree(ch.res_class);
	}
	for (auto &c : cls)
		c = tolower(c);
	if (cls == "steam/steam" || cls.rfind("steamwebhelper/", 0) == 0)
		return true;
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = nullptr;
	bool steam = false;
	if (XGetWindowProperty(dpy, w, a_pid, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after, &data) == Success &&
	    data && n == 1) {
		char path[64], comm[32] = "";
		snprintf(path, sizeof(path), "/proc/%lu/comm", *(unsigned long *)data);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f))
				steam = !strcmp(comm, "steam\n") || !strcmp(comm, "steamwebhelper\n");
			fclose(f);
		}
	}
	if (data)
		XFree(data);
	return steam;
}

static void app_destroy(XID client);

static void win_mapped(Window w)
{
	XWindowAttributes at;
	if (w == cover || client_of(w) == own_client || steam_clients.count(client_of(w)) ||
	    !XGetWindowAttributes(dpy, w, &at) || at.c_class == InputOnly)
		return;
	if (is_steam_window(w)) {
		XID client = client_of(w);
		steam_clients.insert(client);
		for (auto it = wins.begin(); it != wins.end();)
			it = it->second.client == client ? wins.erase(it) : std::next(it);
		if (apps.count(client))
			app_destroy(client);
		DBG("client 0x%lx is Steam's: ignored\n", client);
		return;
	}
	Win &v = wins[w];
	v.client = client_of(w);
	v.override_redirect = at.override_redirect;
	v.depth = at.depth;
	v.x = at.x, v.y = at.y, v.w = at.width, v.h = at.height;
	if (!v.override_redirect) {
		// no window manager here: main windows fill the screen as in gamescope, dialogs are centered
		if (is_transient(w))
			XMoveWindow(dpy, w, std::max(0, (W - v.w) / 2), std::max(0, (H - v.h) / 2));
		else
			XMoveResizeWindow(dpy, w, 0, 0, W, H);
	}
	if (v.damage == None) {
		XCompositeRedirectWindow(dpy, w, CompositeRedirectAutomatic);
		v.damage = XDamageCreate(dpy, w, XDamageReportRawRectangles);
	}
	if (!apps.count(v.client))
		app_create(v.client, w);
	App &a = apps[v.client];
	a.gone_since = 0;
	mark(v.client, v.rect());
	if (!v.override_redirect) {
		front = 0; // a new main window or dialog is on top of its app: raise it and give it the focus
		bring_front(v.client);
	}
	raise_cover();
}

static void win_gone(Window w)
{
	auto it = wins.find(w);
	if (it == wins.end())
		return;
	Win v = it->second;
	if (v.damage != None)
		XDamageDestroy(dpy, v.damage);
	if (v.pixmap != None)
		XFreePixmap(dpy, v.pixmap);
	wins.erase(it);
	mark(v.client, v.rect());
	bool more = false;
	for (auto &e : wins)
		more = more || e.second.client == v.client;
	if (!more && apps.count(v.client))
		apps[v.client].gone_since = now_s();
}

static void win_configured(const XConfigureEvent &c)
{
	auto it = wins.find(c.window);
	if (it == wins.end())
		return;
	Win &v = it->second;
	mark(v.client, v.rect());
	if (c.width != v.w || c.height != v.h) {
		if (v.pixmap != None)
			XFreePixmap(dpy, v.pixmap);
		v.pixmap = None;
	}
	v.x = c.x, v.y = c.y, v.w = c.width, v.h = c.height;
	mark(v.client, v.rect());
	if (c.window != cover && c.above != None)
		raise_cover(); // something went above it
}

// the app's windows inside r, bottom to top, on black
static void composite(XID client, App &a, const Rect &r)
{
	for (int y = r.y0; y < r.y1; y++)
		memset(a.canvas.data() + ((size_t)y * W + r.x0) * 4, 0, (size_t)(r.x1 - r.x0) * 4);
	for (Window w : app_windows(client)) {
		Win &v = wins[w];
		Rect c = r.cut(v.rect());
		if (c.empty())
			continue;
		Shm &s = v.depth == 32 ? shm32 : shm24;
		if (!s.img)
			continue;
		if (v.pixmap == None)
			v.pixmap = XCompositeNameWindowPixmap(dpy, w);
		int cw = c.x1 - c.x0, ch = c.y1 - c.y0;
		s.img->width = cw;
		s.img->height = ch;
		s.img->bytes_per_line = cw * 4;
		if (!XShmGetImage(dpy, v.pixmap, s.img, c.x0 - v.x, c.y0 - v.y, AllPlanes))
			continue;
		for (int y = 0; y < ch; y++)
			memcpy(a.canvas.data() + ((size_t)(c.y0 + y) * W + c.x0) * 4, s.img->data + (size_t)y * cw * 4,
			       (size_t)cw * 4);
	}
}

static void close_app(XID client, App &a)
{
	for (Window w : app_windows(client)) {
		if (wins[w].override_redirect)
			continue;
		Atom *protos = nullptr;
		int n = 0;
		bool del = false;
		if (XGetWMProtocols(dpy, w, &protos, &n)) {
			for (int i = 0; i < n; i++)
				del = del || protos[i] == a_delete;
			XFree(protos);
		}
		if (del) {
			XEvent e{};
			e.xclient.type = ClientMessage;
			e.xclient.window = w;
			e.xclient.message_type = a_protocols;
			e.xclient.format = 32;
			e.xclient.data.l[0] = a_delete;
			e.xclient.data.l[1] = CurrentTime;
			XSendEvent(dpy, w, False, NoEventMask, &e);
		} else {
			XKillClient(dpy, w);
		}
	}
	XFlush(dpy);
	a.close_at = now_s() + 5;
}

int main()
{
	g_prog = "q1apps";
	g_debug = getenv("Q1A_DEBUG") && *getenv("Q1A_DEBUG") == '1';
	if (getenv("Q1A_WIDTH"))
		width_m = atof(getenv("Q1A_WIDTH"));
	if (getenv("Q1A_HZ"))
		min_dt = 1.0 / std::max(1, atoi(getenv("Q1A_HZ")));

	dpy = XOpenDisplay(nullptr);
	if (!dpy)
		die("cannot open the X display (DISPLAY)");
	XSetErrorHandler(on_x_error);
	int ev, err, maj = 0, min = 4;
	if (!XCompositeQueryExtension(dpy, &ev, &err) || !XCompositeQueryVersion(dpy, &maj, &min) || (maj == 0 && min < 2))
		die("no Composite 0.2 on the X server");
	if (!XDamageQueryExtension(dpy, &dmg_ev, &dmg_err))
		die("no DAMAGE extension on the X server");
	maj = 2, min = 0;
	if (!XFixesQueryExtension(dpy, &ev, &err) || !XFixesQueryVersion(dpy, &maj, &min) || maj < 2)
		die("no XFixes 2 on the X server");
	if (!XShmQueryExtension(dpy))
		die("no MIT-SHM on the X server");
	int xt_maj, xt_min;
	if (!XTestQueryExtension(dpy, &ev, &err, &xt_maj, &xt_min))
		die("no XTEST extension on the X server");
	root = DefaultRootWindow(dpy);
	W = DisplayWidth(dpy, DefaultScreen(dpy));
	H = DisplayHeight(dpy, DefaultScreen(dpy));
	client_mask = xcb_get_setup(XGetXCBConnection(dpy))->resource_id_mask;
	a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
	a_net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
	a_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	shm_init(shm24, 24);
	shm_init(shm32, 32);

	EVRInitError verr = VRInitError_None;
	VR_Init(&verr, VRApplication_Overlay);
	if (verr != VRInitError_None) {
		fprintf(stderr, "q1apps: VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(verr));
		return 1;
	}

	// the cover: the streamed screen shows it, not the apps; input goes through it to them
	XSetWindowAttributes sa{};
	sa.override_redirect = True;
	sa.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
	cover = XCreateWindow(dpy, root, 0, 0, W, H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                      CWOverrideRedirect | CWBackPixel, &sa);
	own_client = client_of(cover);
	XserverRegion none = XFixesCreateRegion(dpy, nullptr, 0);
	XFixesSetWindowShapeRegion(dpy, cover, ShapeInput, 0, 0, none);
	XFixesDestroyRegion(dpy, none);
	XStoreName(dpy, cover, "q1apps");
	XMapRaised(dpy, cover);
	XFixesHideCursor(dpy, root);
	// an empty cursor: Remote Play draws the X cursor into the stream (XFixes' cursor image, hidden or not)
	{
		static char zero = 0;
		Pixmap pm = XCreateBitmapFromData(dpy, root, &zero, 1, 1);
		XColor black{};
		Cursor none = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);
		XDefineCursor(dpy, cover, none);
		XDefineCursor(dpy, root, none);
		XFreePixmap(dpy, pm);
	}
	unlink(SPECT_TRIGGER); // a session is looked for at once

	XGrabServer(dpy); // no window missed between the listing and the event selection
	XSelectInput(dpy, root, SubstructureNotifyMask);
	{
		Window r, p, *kids = nullptr;
		unsigned n = 0;
		if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {
			for (unsigned i = 0; i < n; i++) {
				XWindowAttributes at;
				if (XGetWindowAttributes(dpy, kids[i], &at) && at.map_state == IsViewable)
					win_mapped(kids[i]);
			}
			XFree(kids);
		}
	}
	XUngrabServer(dpy);
	raise_cover();
	XFlush(dpy);
	fprintf(stderr, "q1apps: %dx%d, %zu app(s) at start\n", W, H, apps.size());

	XInput xi;
	xi.dpy = dpy;
	x_find_scratch(xi);
	int xfd = ConnectionNumber(dpy);
	for (;;) {
		while (XPending(dpy)) {
			XEvent e;
			XNextEvent(dpy, &e);
			if (e.type == dmg_ev + XDamageNotify) {
				auto *d = (XDamageNotifyEvent *)&e;
				auto it = wins.find(d->drawable);
				if (it != wins.end()) {
					Win &v = it->second;
					mark(v.client, {v.x + d->area.x, v.y + d->area.y, v.x + d->area.x + d->area.width,
					                v.y + d->area.y + d->area.height});
				}
				continue;
			}
			switch (e.type) {
			case MapNotify:
				if (e.xmap.event == root)
					win_mapped(e.xmap.window);
				break;
			case UnmapNotify:
				if (e.xunmap.event == root)
					win_gone(e.xunmap.window);
				break;
			case DestroyNotify:
				win_gone(e.xdestroywindow.window);
				break;
			case ReparentNotify:
				if (e.xreparent.parent != root)
					win_gone(e.xreparent.window);
				break;
			case ConfigureNotify:
				if (e.xconfigure.event == root)
					win_configured(e.xconfigure);
				break;
			default:
				break;
			}
		}

		double t = now_s();
		std::vector<XID> done;
		for (auto &it : apps) {
			XID client = it.first;
			App &a = it.second;
			if (a.ov == k_ulOverlayHandleInvalid || (a.gone_since && t - a.gone_since > 2)) {
				done.push_back(client);
				continue;
			}
			VREvent_t ev;
			while (VROverlay()->PollNextOverlayEvent(a.ov, &ev, sizeof(ev))) {
				if (ev.eventType == VREvent_MouseMove || ev.eventType == VREvent_MouseButtonDown ||
				    ev.eventType == VREvent_KeyboardCharInput)
					bring_front(client);
				if (ev.eventType == VREvent_MouseMove || ev.eventType == VREvent_MouseButtonDown ||
				    ev.eventType == VREvent_MouseButtonUp || ev.eventType == VREvent_ScrollDiscrete)
					spect_input(t);
				if (x_input_event(xi, ev, W, H))
					continue;
				if (ev.eventType == VREvent_OverlayClosed) {
					DBG("\"%s\" closed from the control bar\n", a.name.c_str());
					close_app(client, a);
				}
			}
			if (a.close_at && t > a.close_at && !a.gone_since) {
				Window m = main_window(client);
				if (m)
					XKillClient(dpy, m);
				a.close_at = 0;
			}
			bool visible = VROverlay()->IsOverlayVisible(a.ov);
			if (visible)
				bring_front(client);
			if (t - a.last_check > 2) { // the main window's title and icon come and change after it maps
				a.last_check = t;
				Window m = main_window(client);
				if (m) {
					std::string n = window_name(m);
					if (n != a.name) {
						a.name = n;
						VROverlay()->SetOverlayName(a.ov, n.c_str());
					}
					if (!a.icon)
						a.icon = set_icon(a, m);
				}
			}
			if (!a.dirty.empty() && visible && t - a.last_upload >= min_dt) {
				Rect r = a.dirty;
				a.dirty = Rect();
				composite(client, a, r);
				if (upload(gfx, a.surf, a.canvas.data(), W * 4, r.y0, r.y1)) {
					VRVulkanTextureData_t vk;
					Texture_t tex = surface_texture(gfx, a.surf, vk);
					VROverlay()->SetOverlayTexture(a.ov, &tex);
				}
				a.last_upload = t;
			}
		}
		for (XID c : done)
			app_destroy(c);
		spect_poll(t);

		VREvent_t ev;
		while (VRSystem()->PollNextEvent(&ev, sizeof(ev)))
			if (ev.eventType == VREvent_Quit) {
				VRSystem()->AcknowledgeQuit_Exiting();
				VR_Shutdown();
				return 1;
			}

		pollfd p = {xfd, POLLIN, 0};
		poll(&p, 1, 16);
	}
}
