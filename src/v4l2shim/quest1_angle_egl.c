// libquest1_angle_egl.so: libEGL.so.1 and libGL.so.1 for Steam's stream client, and for it only: they are found
// under those names in /usr/local/lib/quest1-streamclient, a directory quest1_streamclient.c puts first in that
// process's library path. Everything goes to Steam's ANGLE (QUEST1_ANGLE_EGL, its libEGL.so: OpenGL ES on the
// Adreno's Vulkan).
//
// Who calls what in the client:
//  - SDL's OpenVR video driver calls EGL directly (libEGL.so.1 is linked into libSDL3) and asks for a desktop
//    OpenGL context: it gets an OpenGL ES 3.1 one, and eglGetProcAddress adds the desktop-only things the driver
//    uses (glCheckNamedFramebufferStatus, a depth renderbuffer of no stated size);
//  - vrclient (SteamVR's library, loaded so that it sees its own libGL.so.1 and libEGL.so.1 first) calls a few
//    OpenGL functions by name, looks for a GLX context then an EGL one, and shares the overlay's texture with
//    vrcompositor through GL_EXT_memory_object_fd and GL_EXT_semaphore_fd, which ANGLE has on this Vulkan.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

static void *angle;
static int trace = -1; // QUEST1_ANGLE_EGL_TRACE: every call on stderr

static void *real(const char *name)
{
	if (!angle) {
		const char *p = getenv("QUEST1_ANGLE_EGL");
		angle = p ? dlopen(p, RTLD_NOW | RTLD_GLOBAL) : NULL;
		if (!angle) {
			fprintf(stderr, "quest1_angle_egl: %s: %s\n", p ? p : "QUEST1_ANGLE_EGL is not set", p ? dlerror() : "");
			abort();
		}
		trace = getenv("QUEST1_ANGLE_EGL_TRACE") != NULL;
	}
	return dlsym(angle, name);
}

static void *(*real_gpa)(const char *);

static void *gl(const char *name)
{
	if (!real_gpa && !(*(void **)&real_gpa = real("eglGetProcAddress")))
		return NULL;
	return real_gpa(name);
}

// the functions of EGL and OpenGL forwarded here take integers and pointers only and return one or nothing, so
// the arguments go on as they came (ten: glTexImage2D has nine)
typedef long (*fn10)(long, long, long, long, long, long, long, long, long, long);
#define FWD(name, lookup, target) \
	EXPORT long name(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) \
	{ \
		static fn10 p; \
		if (!p && !(p = (fn10)lookup(target))) { \
			fprintf(stderr, "quest1_angle_egl: ANGLE has no %s\n", target); \
			return 0; \
		} \
		long r = p(a, b, c, d, e, f, g, h, i, j); \
		if (trace > 0) \
			fprintf(stderr, "quest1_angle_egl: %s(%#lx, %#lx, %#lx, %#lx) = %#lx\n", #name, a, b, c, d, r); \
		return r; \
	}
#define EGL(name) FWD(name, real, #name)
#define GL(name) FWD(name, gl, #name)

EGL(eglGetDisplay) EGL(eglGetPlatformDisplay) EGL(eglGetPlatformDisplayEXT) EGL(eglInitialize) EGL(eglTerminate)
EGL(eglGetConfigs) EGL(eglGetConfigAttrib) EGL(eglDestroyContext) EGL(eglCreatePbufferSurface)
EGL(eglCreateWindowSurface) EGL(eglCreatePlatformWindowSurface) EGL(eglDestroySurface) EGL(eglMakeCurrent)
EGL(eglGetCurrentContext) EGL(eglGetCurrentDisplay) EGL(eglGetCurrentSurface) EGL(eglSwapBuffers)
EGL(eglSwapInterval) EGL(eglWaitNative) EGL(eglWaitGL) EGL(eglQueryAPI) EGL(eglQueryString) EGL(eglQuerySurface)
EGL(eglQueryContext) EGL(eglGetError) EGL(eglReleaseThread) EGL(eglSurfaceAttrib) EGL(eglQueryDeviceStringEXT)
EGL(eglQueryDisplayAttribEXT) EGL(eglCreateImage) EGL(eglDestroyImage) EGL(eglCreateImageKHR)
EGL(eglDestroyImageKHR) EGL(eglCreateSync) EGL(eglDestroySync) EGL(eglClientWaitSync) EGL(eglCreateSyncKHR)
EGL(eglDestroySyncKHR) EGL(eglClientWaitSyncKHR) EGL(eglWaitSyncKHR) EGL(eglDupNativeFenceFDANDROID)
EGL(eglBindTexImage) EGL(eglReleaseTexImage) EGL(eglCopyBuffers)

GL(glBindRenderbuffer) GL(glBindTexture) GL(glCheckFramebufferStatus) GL(glDeleteTextures) GL(glFinish) GL(glFlush)
GL(glGenTextures) GL(glGetError) GL(glGetIntegerv) GL(glGetRenderbufferParameteriv) GL(glGetStringi)
GL(glGetTexLevelParameteriv) GL(glPixelStorei) GL(glTexImage2D) GL(glTexParameteri)
FWD(glGetTexImage, gl, "glGetTexImageANGLE")

// no list of devices in this ANGLE; SDL then takes the default display
EXPORT unsigned int eglQueryDevicesEXT(int max, void **devices, int *num)
{
	(void)max;
	(void)devices;
	if (num)
		*num = 0;
	return 0;
}

// there is no GLX here: no context and no visual (the client's windowed mode is not this one). vrclient still asks
// GLX for the functions of the extensions, whatever the context is from: ANGLE's
#define NOGLX(name) \
	EXPORT long name(void) \
	{ \
		if (trace > 0) \
			fprintf(stderr, "quest1_angle_egl: %s: no GLX\n", #name); \
		return 0; \
	}
NOGLX(glXGetCurrentContext) NOGLX(glXGetCurrentDisplay) NOGLX(glXGetCurrentDrawable) NOGLX(glXMakeCurrent)
NOGLX(glXChooseFBConfig) NOGLX(glXGetVisualFromFBConfig)

EXPORT void *eglGetProcAddress(const char *name);
EXPORT void *glXGetProcAddress(const char *name)
{
	return eglGetProcAddress(name);
}
EXPORT void *glXGetProcAddressARB(const char *name)
{
	return eglGetProcAddress(name);
}

// --- a desktop OpenGL context asked for: an OpenGL ES 3.1 one
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x40
#define EGL_OPENGL_BIT 0x8
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD

EXPORT unsigned int eglBindAPI(unsigned int api)
{
	static unsigned int (*p)(unsigned int);
	if (!p && !(*(void **)&p = real("eglBindAPI")))
		return 0;
	unsigned int r = p(api == EGL_OPENGL_API ? EGL_OPENGL_ES_API : api);
	if (trace > 0)
		fprintf(stderr, "quest1_angle_egl: eglBindAPI(%#x) = %u\n", api, r);
	return r;
}

EXPORT unsigned int eglChooseConfig(void *dpy, const int *attr, void *configs, int size, int *num)
{
	static unsigned int (*p)(void *, const int *, void *, int, int *);
	if (!p && !(*(void **)&p = real("eglChooseConfig")))
		return 0;
	int a[64], n = 0;
	if (attr) {
		for (; attr[n] != EGL_NONE && n < 60; n += 2) {
			a[n] = attr[n];
			a[n + 1] = attr[n] == EGL_RENDERABLE_TYPE && (attr[n + 1] & EGL_OPENGL_BIT) ? EGL_OPENGL_ES3_BIT : attr[n + 1];
		}
		a[n] = EGL_NONE;
		attr = a;
	}
	unsigned int r = p(dpy, attr, configs, size, num);
	if (trace > 0)
		fprintf(stderr, "quest1_angle_egl: eglChooseConfig = %u, %d configs\n", r, num ? *num : -1);
	return r;
}

EXPORT void *eglCreateContext(void *dpy, void *config, void *share, const int *attr)
{
	static void *(*p)(void *, void *, void *, const int *);
	if (!p && !(*(void **)&p = real("eglCreateContext")))
		return NULL;
	int a[64], n = 0;
	a[n++] = EGL_CONTEXT_MAJOR_VERSION;
	a[n++] = 3;
	a[n++] = EGL_CONTEXT_MINOR_VERSION;
	a[n++] = 1;
	for (int i = 0; attr && attr[i] != EGL_NONE && n < 60; i += 2) {
		if (attr[i] == EGL_CONTEXT_MAJOR_VERSION || attr[i] == EGL_CONTEXT_MINOR_VERSION ||
		    attr[i] == EGL_CONTEXT_OPENGL_PROFILE_MASK)
			continue;
		a[n++] = attr[i];
		a[n++] = attr[i + 1];
	}
	a[n] = EGL_NONE;
	void *r = p(dpy, config, share, a);
	if (trace > 0)
		fprintf(stderr, "quest1_angle_egl: eglCreateContext = %p\n", r);
	return r;
}

// --- the desktop-only things SDL's OpenVR driver uses
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6

// OpenGL 4.5's check of a framebuffer by name: bound for the check, the binding put back
static unsigned int q1_glCheckNamedFramebufferStatus(unsigned int fb, unsigned int target)
{
	static void (*get)(unsigned int, int *);
	static void (*bind)(unsigned int, unsigned int);
	static unsigned int (*check)(unsigned int);
	if (!check) {
		*(void **)&get = gl("glGetIntegerv");
		*(void **)&bind = gl("glBindFramebuffer");
		*(void **)&check = gl("glCheckFramebufferStatus");
		if (!get || !bind || !check)
			return 0;
	}
	int was = 0;
	get(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &was);
	bind(target, fb);
	unsigned int r = check(target);
	bind(target, (unsigned int)was);
	if (trace > 0)
		fprintf(stderr, "quest1_angle_egl: glCheckNamedFramebufferStatus(%u, %#x) = %#x\n", fb, target, r);
	return r;
}

// a depth buffer of no stated size: 24 bits
static void q1_glRenderbufferStorage(unsigned int target, unsigned int fmt, int w, int h)
{
	static void (*p)(unsigned int, unsigned int, int, int);
	if (!p && !(*(void **)&p = gl("glRenderbufferStorage")))
		return;
	p(target, fmt == GL_DEPTH_COMPONENT ? GL_DEPTH_COMPONENT24 : fmt, w, h);
}

// the trace of vrclient's copy into the texture it shares with vrcompositor (its functions are ANGLE's own, got by
// name: this one is put in between when tracing), with the state of both framebuffers and the error it left
static void q1_trace_glBlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                                       unsigned int mask, unsigned int filter)
{
	static void (*blit)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);
	static unsigned int (*check)(unsigned int);
	static unsigned int (*error)(void);
	static int logged;
	if (!blit) {
		*(void **)&blit = gl("glBlitFramebuffer");
		*(void **)&check = gl("glCheckFramebufferStatus");
		*(void **)&error = gl("glGetError");
	}
	static void (*get)(unsigned int, int *);
	static void (*bind)(unsigned int, unsigned int);
	static void (*readpx)(int, int, int, int, unsigned int, unsigned int, void *);
	if (!get) {
		*(void **)&get = gl("glGetIntegerv");
		*(void **)&bind = gl("glBindFramebuffer");
		*(void **)&readpx = gl("glReadPixels");
	}
	unsigned int before = error(), rd = check(GL_READ_FRAMEBUFFER), dr = check(0x8CA9);
	unsigned char src[4] = {0}, dst[4] = {0};
	int rfb = 0, dfb = 0;
	get(GL_READ_FRAMEBUFFER_BINDING, &rfb);
	get(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
	readpx((sx0 + sx1) / 2, (sy0 + sy1) / 2, 1, 1, 0x1908, 0x1401, src);
	blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	unsigned int after = error();
	bind(GL_READ_FRAMEBUFFER, (unsigned int)dfb);
	readpx((dx0 + dx1) / 2, (dy0 + dy1) / 2, 1, 1, 0x1908, 0x1401, dst);
	bind(GL_READ_FRAMEBUFFER, (unsigned int)rfb);
	if (logged < 40 || logged % 60 == 0)
		fprintf(stderr, "quest1_angle_egl: middle pixel: source %u,%u,%u,%u, shared texture after the copy %u,%u,%u,%u (%#x)\n",
		        src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], error());
	if (logged++ < 8 || after)
		fprintf(stderr,
		        "quest1_angle_egl: glBlitFramebuffer(%d,%d,%d,%d -> %d,%d,%d,%d, %#x, %#x): read fb %#x, draw fb %#x, "
		        "error before %#x, after %#x\n",
		        sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter, rd, dr, before, after);
}

EXPORT void *eglGetProcAddress(const char *name)
{
	// vrclient asks for "pglFramebufferRenderbuffer" (its own name for the pointer): the function meant
	if (!strcmp(name, "pglFramebufferRenderbuffer"))
		name++;
	void *p = !strcmp(name, "glRenderbufferStorage") ? (void *)q1_glRenderbufferStorage : gl(name);
	if (p && trace > 0 && !strcmp(name, "glBlitFramebuffer"))
		p = (void *)q1_trace_glBlitFramebuffer;
	if (!p && !strcmp(name, "glCheckNamedFramebufferStatus"))
		p = (void *)q1_glCheckNamedFramebufferStatus;
	if (!p || trace > 0)
		fprintf(stderr, "quest1_angle_egl: eglGetProcAddress(%s) = %p\n", name, p);
	return p;
}
