// quest1_overlayview: the Steam client's VR stream (Remote Play spectator) fed with the headset's VR view.
//
// A Remote Play session that asks for the spectator view (k_EStreamControlSetSpectatorMode) makes the Steam client
// stream the VR view itself (CVRStreamThread): every frame it calls IVROverlayView::AcquireOverlayView on
// SteamVR's spectator overlay with no native device, i.e. for an OpenGL texture in its current context, and encodes
// that. vrclient serves such a view by importing the overlay's Vulkan image into OpenGL with GL_EXT_memory_object_fd
// (CSharedImageLinux), and calls glCreateMemoryObjectsEXT without checking the extension exists. steam.service
// hides GL_EXT_memory_object* and GL_EXT_semaphore* (Steam's GL is llvmpipe, which cannot import the Adreno's
// memory; for its own overlays vrclient then copies through the CPU), so the pointer was null and Steam died with
// SIGSEGV at address 0, taking vrcompositor with it (SteamVR 2.17 and 2.19 alike). The other ways to that overlay
// fail here too: a Vulkan view crashes the same way (vrclient never loaded its Vulkan entry points in this process:
// null vkGetDeviceProcAddr), and IVROverlay::GetOverlayImageData only reads raw or file overlays
// (VROverlayError_InvalidTexture).
//
// Steam reaches vrclient through its own libopenvr_api.so (dlsym "VR_GetGenericInterface"). dlsym is wrapped here,
// and the IVROverlayView_003 it hands out is a proxy. An OpenGL view is a constant 1280x720 texture of the caller's
// context, uploaded once: the picture itself goes into the encoder's input without Steam's OpenGL, by
// libquest1_v4l2.so (quest1_v4l2.c "Steam's VR stream"), which this proxy tells that the spectator is on (each
// view) and which then copies Monado's NV12 spectator frames (/dev/shm/quest1-spectator, made while
// /tmp/quest1-spectator exists) into each raw frame, or dark blue after 0.5 s without one, as q1screencast did. The
// trigger is made on the first request and removed 3 s after the last one, if made here. A first version converted
// every frame to RGBA and uploaded it into llvmpipe, where Steam converted it back to NV12: ~5 frames a second.
// Vulkan and D3D11 requests go through untouched.
#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define LOG(...) fprintf(stderr, "quest1_overlayview: " __VA_ARGS__)

// openvr.h
enum { DeviceType_DirectX11 = 0, DeviceType_Vulkan = 1 };
enum { TextureType_OpenGL = 1 };
enum { VROverlayError_None = 0, VROverlayError_RequestFailed = 23 };
typedef struct {
	void *handle;
	int32_t eType;
} VRNativeDevice_t;
typedef struct {
	void *handle;
	int32_t eType, eColorSpace;
} Texture_t;
typedef struct {
	uint64_t overlayHandle;
	Texture_t texture;
	float bounds[4];
} VROverlayView_t;

typedef int acquire_fn(void *self, uint64_t overlay, VRNativeDevice_t *dev, void *view, uint32_t size);
typedef int release_fn(void *self, void *view);
typedef void post_fn(void *self, uint64_t overlay, const void *event);
typedef bool permitted_fn(void *self, uint64_t overlay);
// the C table ("FnTable:IVROverlayView_003") has the same functions without the object
typedef int fnt_acquire_fn(uint64_t overlay, VRNativeDevice_t *dev, void *view, uint32_t size);
typedef int fnt_release_fn(void *view);

static void *(*real_dlsym)(void *, const char *);

static void *real_dlsym_get(void)
{
	if (!real_dlsym) {
		real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
		if (!real_dlsym)
			real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
	}
	return (void *)real_dlsym;
}

static void *lib_sym(const char *lib, const char *name)
{
	void *h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
	if (!h)
		h = dlopen(lib, RTLD_NOW);
	return h && real_dlsym_get() ? real_dlsym(h, name) : NULL;
}

// --- OpenGL: one texture per overlay in the caller's context ---
static struct {
	bool ok;
#define GLF(n, r, a) r(*n) a;
	GLF(GenTextures, void, (GLsizei, GLuint *))
	GLF(BindTexture, void, (GLenum, GLuint))
	GLF(TexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
	GLF(TexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
	GLF(TexParameteri, void, (GLenum, GLenum, GLint))
	GLF(PixelStorei, void, (GLenum, GLint))
	GLF(GetIntegerv, void, (GLenum, GLint *))
	GLF(BindBuffer, void, (GLenum, GLuint))
#undef GLF
} gl;

static bool gl_init(void)
{
	if (gl.ok)
		return true;
#define G(n) gl.n = lib_sym("libGL.so.1", "gl" #n)
	G(GenTextures);
	G(BindTexture);
	G(TexImage2D);
	G(TexSubImage2D);
	G(TexParameteri);
	G(PixelStorei);
	G(GetIntegerv);
	G(BindBuffer);
#undef G
	gl.ok = gl.GenTextures && gl.BindTexture && gl.TexImage2D && gl.TexSubImage2D && gl.TexParameteri &&
	        gl.PixelStorei && gl.GetIntegerv && gl.BindBuffer;
	if (!gl.ok)
		LOG("no OpenGL entry points\n");
	return gl.ok;
}

struct gltex {
	uint64_t overlay;
	GLuint tex;
	uint32_t w, h;
};

static void gl_upload(struct gltex *g, const void *rgba, uint32_t w, uint32_t h)
{
	GLint prev_tex = 0, prev_align = 4, prev_row = 0, prev_pbo = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
	gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
	gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
	gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
	if (prev_pbo)
		gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	if (!g->tex)
		gl.GenTextures(1, &g->tex);
	gl.BindTexture(GL_TEXTURE_2D, g->tex);
	if (g->w != w || g->h != h) {
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		// overlay pixels are sRGB-encoded, as the texture vrclient would import (R8G8B8A8_SRGB)
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		g->w = w;
		g->h = h;
	} else {
		gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}
	gl.BindTexture(GL_TEXTURE_2D, prev_tex);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
	if (prev_pbo)
		gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_pbo);
}

static int refuse(uint64_t overlay, void *view, uint32_t size, const char *why)
{
	static int logged;
	if (logged++ < 4)
		LOG("OpenGL view of overlay %#llx refused: %s\n", (unsigned long long)overlay, why);
	if (view && size)
		memset(view, 0, size);
	return VROverlayError_RequestFailed;
}

static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;

typedef void *get_generic_fn(const char *version, int *error);
static get_generic_fn *real_get_generic;

#define SPECT_TRIGGER "/tmp/quest1-spectator"
#define VIEW_W 1280
#define VIEW_H 720

static struct {
	bool made_trigger;
	struct timespec last; // the last request
	bool reaper;
	void (*shim_spectator)(void);
	bool shim_looked;
} sp;

static double since(const struct timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) + (now.tv_nsec - t->tv_nsec) / 1e9;
}

// removes the trigger made here 3 s after the last request (Monado frees its side 5 s later)
static void *reaper(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(1);
		pthread_mutex_lock(&view_lock);
		if (sp.made_trigger && since(&sp.last) > 3) {
			unlink(SPECT_TRIGGER);
			sp.made_trigger = false;
			LOG("spectator view no longer asked for: %s removed\n", SPECT_TRIGGER);
		}
		pthread_mutex_unlock(&view_lock);
	}
	return NULL;
}

//! An OpenGL view (no native device) of the spectator overlay: see the top of the file
static int gl_view(uint64_t overlay, void *view, uint32_t size)
{
	static struct gltex tex;
	static unsigned long served;
	if (!view || size < sizeof(VROverlayView_t))
		return refuse(overlay, view, size, "view too small");
	pthread_mutex_lock(&view_lock);
	const char *why = NULL;
	if (!gl_init()) {
		why = "no OpenGL";
		goto out;
	}
	if (!sp.shim_looked) {
		sp.shim_looked = true;
		*(void **)&sp.shim_spectator = real_dlsym_get() ? real_dlsym(RTLD_DEFAULT, "quest1_v4l2_spectator") : NULL;
		if (!sp.shim_spectator)
			LOG("libquest1_v4l2.so has no quest1_v4l2_spectator: the stream shows the constant texture\n");
	}
	if (sp.shim_spectator)
		sp.shim_spectator();
	clock_gettime(CLOCK_MONOTONIC, &sp.last);
	if (!sp.reaper) {
		pthread_t t;
		sp.reaper = pthread_create(&t, NULL, reaper, NULL) == 0;
		if (sp.reaper)
			pthread_detach(t);
	}
	if (access(SPECT_TRIGGER, F_OK) != 0) {
		int fd = open(SPECT_TRIGGER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0) {
			close(fd);
			sp.made_trigger = true;
			LOG("spectator view asked for: %s made\n", SPECT_TRIGGER);
		}
	}
	if (!tex.tex) {
		// dark blue (#1b2838): what the encoder input would show if the shim ever left it alone
		uint8_t *px = malloc((size_t)VIEW_W * VIEW_H * 4);
		if (!px) {
			why = "out of memory";
			goto out;
		}
		for (size_t i = 0; i < (size_t)VIEW_W * VIEW_H; i++)
			memcpy(px + i * 4, "\x1b\x28\x38\xff", 4);
		tex.overlay = overlay;
		gl_upload(&tex, px, VIEW_W, VIEW_H);
		free(px);
	}
	VROverlayView_t o = {overlay, {(void *)(uintptr_t)tex.tex, TextureType_OpenGL, 0}, {0, 0, 1, 1}};
	if (served++ % 1800 == 0)
		LOG("spectator view served to Steam's VR stream (%lu views)\n", served);
	memset(view, 0, size);
	memcpy(view, &o, sizeof(o));
out:
	pthread_mutex_unlock(&view_lock);
	return why ? refuse(overlay, view, size, why) : VROverlayError_None;
}

static bool is_gl(const VRNativeDevice_t *dev)
{
	return !dev || (dev->eType != DeviceType_Vulkan && dev->eType != DeviceType_DirectX11);
}

//! The release of a view gl_view made: nothing left to release in vrclient
static bool is_our_view(const void *view)
{
	const VROverlayView_t *v = view;
	return v && v->texture.eType == TextureType_OpenGL;
}

// --- the C++ interface: a proxy object whose first word is its own vtable ---
struct proxy {
	void **vtbl;
	void *real;
};

#define REAL_FN(p, i, type) ((type *)(*(void ***)(p)->real)[i])

static int p_acquire(struct proxy *p, uint64_t overlay, VRNativeDevice_t *dev, void *view, uint32_t size)
{
	if (is_gl(dev))
		return gl_view(overlay, view, size);
	return REAL_FN(p, 0, acquire_fn)(p->real, overlay, dev, view, size);
}

static int p_release(struct proxy *p, void *view)
{
	if (is_our_view(view))
		return VROverlayError_None;
	return REAL_FN(p, 1, release_fn)(p->real, view);
}

static void p_post(struct proxy *p, uint64_t overlay, const void *event)
{
	REAL_FN(p, 2, post_fn)(p->real, overlay, event);
}

static bool p_permitted(struct proxy *p, uint64_t overlay)
{
	return REAL_FN(p, 3, permitted_fn)(p->real, overlay);
}

static void *proxy_vtbl[] = {(void *)p_acquire, (void *)p_release, (void *)p_post, (void *)p_permitted};

// --- the C table ---
static fnt_acquire_fn *fnt_real_acquire;
static fnt_release_fn *fnt_real_release;

static int fnt_acquire(uint64_t overlay, VRNativeDevice_t *dev, void *view, uint32_t size)
{
	if (is_gl(dev))
		return gl_view(overlay, view, size);
	return fnt_real_acquire(overlay, dev, view, size);
}

static int fnt_release(void *view)
{
	if (is_our_view(view))
		return VROverlayError_None;
	return fnt_real_release(view);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *wrap(const char *version, void *iface)
{
	static struct proxy *proxies[8];
	static void *tables[8][4];
	static int nproxies, ntables;
	void *out = iface;
	if (!iface || !version)
		return iface;
	pthread_mutex_lock(&lock);
	if (!strcmp(version, "IVROverlayView_003")) {
		int i;
		for (i = 0; i < nproxies && proxies[i]->real != iface; i++)
			;
		if (i == nproxies && nproxies < 8) {
			struct proxy *p = calloc(1, sizeof(*p));
			if (p) {
				p->vtbl = proxy_vtbl;
				p->real = iface;
				proxies[nproxies++] = p;
				LOG("IVROverlayView_003 wrapped (OpenGL views: the headset's VR view)\n");
			}
		}
		if (i < nproxies)
			out = proxies[i];
	} else if (!strcmp(version, "FnTable:IVROverlayView_003")) {
		void **real = iface;
		int i;
		for (i = 0; i < ntables && tables[i][1] != real[1]; i++)
			;
		if (i == ntables && ntables < 8) {
			memcpy(tables[ntables], real, sizeof(tables[0]));
			fnt_real_acquire = (fnt_acquire_fn *)real[0];
			fnt_real_release = (fnt_release_fn *)real[1];
			tables[ntables][0] = (void *)fnt_acquire;
			tables[ntables][1] = (void *)fnt_release;
			ntables++;
			LOG("FnTable:IVROverlayView_003 wrapped (OpenGL views: the headset's VR view)\n");
		}
		if (i < ntables)
			out = tables[i];
	}
	pthread_mutex_unlock(&lock);
	return out;
}

static void *get_generic(const char *version, int *error)
{
	return wrap(version, real_get_generic(version, error));
}

// Steam's dlsym. Anything but VR_GetGenericInterface goes straight on as a tail call, so that the real dlsym sees
// Steam's return address (RTLD_NEXT is resolved from the caller).
EXPORT void *dlsym(void *h, const char *name)
{
	if (!real_dlsym_get())
		return NULL;
	// Steam looks it up in libopenvr_api.so's handle. RTLD_NEXT/RTLD_DEFAULT lookups go on untouched: resolved from
	// here, RTLD_NEXT would find the caller itself (libquest1_v4l2.so's own VR_GetGenericInterface for vrlink looks
	// up the next one that way: endless recursion, vrlink crashed at start)
	if (h != RTLD_NEXT && h != RTLD_DEFAULT && !strcmp(name, "VR_GetGenericInterface")) {
		void *f = real_dlsym(h, name);
		if (!f)
			return NULL;
		real_get_generic = (get_generic_fn *)f;
		return (void *)get_generic;
	}
#if defined(__has_attribute) && __has_attribute(musttail)
	__attribute__((musttail))
#endif
	return real_dlsym(h, name);
}
