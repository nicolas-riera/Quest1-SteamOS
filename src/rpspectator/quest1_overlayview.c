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
// and the IVROverlayView_003 it hands out is a proxy. An OpenGL view is the headset's VR view as Monado makes it
// for Remote Play anyway (comp_window_mdp.c "Remote Play spectator": the left eye, 1280x720 NV12 in
// /dev/shm/quest1-spectator while /tmp/quest1-spectator exists), converted to RGBA and uploaded into an OpenGL
// texture of the caller's context: the view Steam gets (TextureType_OpenGL, like vrclient's own). The trigger is
// made on the first request and removed 3 s after the last one, if made here. Without a new frame for 0.5 s (the
// headset asleep or taken off: Monado renders nothing) the view is dark blue (#1b2838), as the Frame's stream and
// q1screencast's.
// Vulkan and D3D11 requests go through untouched.
#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
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

// Monado's spectator frames (comp_window_mdp.c, spect_thread; read as q1screencast does)
#define SPECT_TRIGGER "/tmp/quest1-spectator"
#define SPECT_FILE "/dev/shm/quest1-spectator"
#define SPECT_MAGIC 0x32535051u // "QPS2": NV12, BT.709 full range
#define SPECT_HEADER 4096
#define SPECT_W 1280
#define SPECT_H 720
struct spect_header {
	uint32_t magic, width, height, stride, frame_bytes;
	uint32_t index, seq;
};

static struct {
	bool made_trigger;
	struct timespec last; // the last request
	bool reaper;
	const struct spect_header *map;
	size_t size;
	ino_t ino;
	uint32_t seq;
	struct timespec fresh; // the last new frame
	bool blue;
	uint8_t *rgba;
	uint32_t w, h;
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

static void spect_unmap(void)
{
	if (sp.map)
		munmap((void *)sp.map, sp.size);
	sp.map = NULL;
	sp.ino = 0;
}

static const struct spect_header *spect_frames(void)
{
	struct stat st;
	if (stat(SPECT_FILE, &st) != 0) {
		spect_unmap();
		return NULL;
	}
	if (!sp.map || st.st_ino != sp.ino) {
		spect_unmap();
		int fd = open(SPECT_FILE, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return NULL;
		void *m = st.st_size >= SPECT_HEADER ? mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
		close(fd);
		if (m == MAP_FAILED)
			return NULL;
		const struct spect_header *h = m;
		if (h->magic != SPECT_MAGIC || !h->width || !h->height || h->width > 4096 || h->height > 4096 ||
		    h->stride < h->width || h->frame_bytes < h->stride * h->height * 3 / 2 ||
		    SPECT_HEADER + 2 * (uint64_t)h->frame_bytes > (uint64_t)st.st_size) {
			munmap(m, st.st_size);
			return NULL;
		}
		sp.map = h;
		sp.size = st.st_size;
		sp.ino = st.st_ino;
		sp.seq = 0;
	}
	return sp.map;
}

static uint8_t clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

//! NV12 (BT.709 full range) to RGBA
static void nv12_to_rgba(const uint8_t *y, const uint8_t *uv, uint32_t stride, uint32_t w, uint32_t h, uint8_t *out)
{
	// 16.16 fixed point: R = Y + 1.5748 V, G = Y - 0.1873 U - 0.4681 V, B = Y + 1.8556 U
	const int rv = 103206, gu = 12276, gv = 30679, bu = 121609;
	for (uint32_t j = 0; j < h; j++) {
		const uint8_t *yr = y + (size_t)j * stride, *uvr = uv + (size_t)(j / 2) * stride;
		uint8_t *o = out + (size_t)j * w * 4;
		for (uint32_t i = 0; i < w; i++) {
			int Y = yr[i] << 16, U = uvr[i & ~1u] - 128, V = uvr[i | 1u] - 128;
			o[0] = clamp8((Y + rv * V + 32768) >> 16);
			o[1] = clamp8((Y - gu * U - gv * V + 32768) >> 16);
			o[2] = clamp8((Y + bu * U + 32768) >> 16);
			o[3] = 255;
			o += 4;
		}
	}
}

//! An OpenGL view (no native device) of the spectator overlay: the headset's VR view
static int gl_view(uint64_t overlay, void *view, uint32_t size)
{
	static struct gltex tex;
	static unsigned long served, fresh;
	if (!view || size < sizeof(VROverlayView_t))
		return refuse(overlay, view, size, "view too small");
	pthread_mutex_lock(&view_lock);
	const char *why = NULL;
	if (!gl_init()) {
		why = "no OpenGL";
		goto out;
	}
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
	const struct spect_header *h = spect_frames();
	uint32_t seq = h ? __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) : 0;
	uint32_t w = h ? h->width : SPECT_W, hh = h ? h->height : SPECT_H;
	bool changed = !sp.rgba || sp.w != w || sp.h != hh;
	if (changed) {
		uint8_t *n = realloc(sp.rgba, (size_t)w * hh * 4);
		if (!n) {
			why = "out of memory";
			goto out;
		}
		sp.rgba = n;
		sp.w = w;
		sp.h = hh;
		sp.blue = false;
	}
	if (h && seq && seq != sp.seq) {
		uint32_t idx = h->index & 1;
		const uint8_t *f = (const uint8_t *)h + SPECT_HEADER + (size_t)idx * h->frame_bytes;
		nv12_to_rgba(f, f + (size_t)h->stride * hh, h->stride, w, hh, sp.rgba);
		sp.seq = seq;
		sp.fresh = sp.last;
		sp.blue = false;
		changed = true;
		fresh++;
	} else if (!sp.blue && (!sp.fresh.tv_sec || since(&sp.fresh) > 0.5)) {
		for (size_t i = 0; i < (size_t)w * hh; i++)
			memcpy(sp.rgba + i * 4, "\x1b\x28\x38\xff", 4);
		sp.blue = true;
		changed = true;
	}
	if (changed || !tex.tex) {
		tex.overlay = overlay;
		gl_upload(&tex, sp.rgba, w, hh);
	}
	VROverlayView_t o = {overlay, {(void *)(uintptr_t)tex.tex, TextureType_OpenGL, 0}, {0, 0, 1, 1}};
	if (served++ % 1800 == 0)
		LOG("spectator view %ux%u served to Steam's VR stream (%lu views, %lu new frames)\n", w, hh, served, fresh);
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
	if (!strcmp(name, "VR_GetGenericInterface")) {
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
