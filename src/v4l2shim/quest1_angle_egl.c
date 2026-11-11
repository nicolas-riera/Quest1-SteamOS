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
//    vrcompositor through GL_EXT_memory_object_fd and GL_EXT_semaphore_fd, which ANGLE has on this Vulkan;
//  - the client's video frames (decoded by venus into dma-bufs) are imported with EGL_EXT_image_dma_buf_import,
//    which this file adds (below: ANGLE has it only on a Vulkan with VK_EXT_image_drm_format_modifier).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
EGL(eglSwapInterval) EGL(eglWaitNative) EGL(eglWaitGL) EGL(eglQueryAPI) EGL(eglQuerySurface)
EGL(eglQueryContext) EGL(eglGetError) EGL(eglReleaseThread) EGL(eglSurfaceAttrib) EGL(eglQueryDeviceStringEXT)
EGL(eglQueryDisplayAttribEXT) EGL(eglCreateImage) EGL(eglDestroyImage)
EGL(eglCreateSync) EGL(eglDestroySync) EGL(eglClientWaitSync) EGL(eglCreateSyncKHR)
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

// --- the decoder's frames, with no copy
//
// The client shows a decoded frame through an SDL NV12 texture (two OpenGL textures: Y, and UV sampled as .rg)
// made once per decoder buffer: each plane is a dma-buf image (EGL_LINUX_DMA_BUF_EXT: DRM_FORMAT_R8 for Y,
// DRM_FORMAT_GR88 for UV, its fd, offset and pitch), given to SDL's texture with glEGLImageTargetTexture2DOES.
// Without the EGL extension it copies every frame by CPU (an mmap of the dma-buf, then glTexSubImage2D), which
// does not keep up as soon as anything else runs on the headset.
//
// ANGLE has no dma-buf import on this Vulkan, but it has GL_EXT_memory_object_fd: a plane is imported as an
// OpenGL texture with linear tiling, and that texture is the source of a real ANGLE EGL image (EGL_GL_TEXTURE_2D)
// that the client then gives to SDL. The Adreno blob refuses an ION dma-buf as OPAQUE_FD memory: the Vulkan layer
// imports it again as a gralloc buffer (quest1_dmabuf.c). ANGLE binds the image at offset 0 whatever offset it is
// given, so the plane's offset goes to the layer in a structure of ours chained to ANGLE's VkImageCreateInfo
// (glTexStorageMemFlags2DANGLE takes such a chain), and the layer binds the image there.
// A plane whose rows are not what a linear texture of that width has (pitch beyond width x pixel size: the stream at
// an odd size) is refused: the client then copies by CPU, as before.
#define EGL_EXTENSIONS 0x3055
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_GL_TEXTURE_LEVEL_KHR 0x30BC
#define DRM_FORMAT_R8 0x20203852
#define DRM_FORMAT_GR88 0x38385247
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_LINEAR_TILING_EXT 0x9585
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586

// VK_IMAGE_USAGE_TRANSFER_SRC, TRANSFER_DST, SAMPLED and COLOR_ATTACHMENT
#define VK_USAGE_SAMPLED_COPY_DRAWN 0x17u

// the plane's offset, for the Vulkan layer (quest1_dmabuf.c, same values): it binds the image there
#define QUEST1_STRUCTURE_TYPE_PLANE_OFFSET 0x51310001
struct quest1_plane_offset {
	int sType;
	const void *pNext;
	uint64_t offset;
};

static const char dmabuf_ext[] = "EGL_EXT_image_dma_buf_import";

EXPORT const char *eglQueryString(void *dpy, int name)
{
	static const char *(*p)(void *, int);
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static const char *from;
	static char *with;
	if (!p && !(*(void **)&p = real("eglQueryString")))
		return NULL;
	const char *r = p(dpy, name);
	if (name != EGL_EXTENSIONS || !dpy || !r || strstr(r, dmabuf_ext))
		return r;
	pthread_mutex_lock(&lock);
	if (from != r) { // the string a display gives stays the same: made once
		char *s;
		if (asprintf(&s, "%s %s", r, dmabuf_ext) >= 0) {
			free(with);
			with = s;
			from = r;
		}
	}
	const char *out = from == r ? with : r;
	pthread_mutex_unlock(&lock);
	return out;
}

// the images made here, with the texture that is their source (deleted with the image)
enum { NIMG = 128 };
static struct {
	void *image;
	unsigned int tex;
} imgs[NIMG];
static pthread_mutex_t imgs_lock = PTHREAD_MUTEX_INITIALIZER;

static void *(*real_create_image)(void *, void *, unsigned int, void *, const int *);

static void *import_plane(void *dpy, const int *attr)
{
	long w = 0, h = 0, fourcc = 0, fd = -1, offset = 0, pitch = 0;
	unsigned long long modifier = 0;
	for (int i = 0; attr && attr[i] != EGL_NONE; i += 2) {
		switch (attr[i]) {
		case EGL_WIDTH: w = attr[i + 1]; break;
		case EGL_HEIGHT: h = attr[i + 1]; break;
		case EGL_LINUX_DRM_FOURCC_EXT: fourcc = (unsigned int)attr[i + 1]; break;
		case EGL_DMA_BUF_PLANE0_FD_EXT: fd = attr[i + 1]; break;
		case EGL_DMA_BUF_PLANE0_OFFSET_EXT: offset = (unsigned int)attr[i + 1]; break;
		case EGL_DMA_BUF_PLANE0_PITCH_EXT: pitch = (unsigned int)attr[i + 1]; break;
		case EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT: modifier |= (unsigned int)attr[i + 1]; break;
		case EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT: modifier |= (unsigned long long)(unsigned int)attr[i + 1] << 32; break;
		default:
			if (attr[i] >= EGL_DMA_BUF_PLANE1_FD_EXT && attr[i] <= EGL_DMA_BUF_PLANE1_FD_EXT + 5)
				fourcc = 0; // a second plane: one image for the whole frame, not something made here
			break; // colour space hints: the client's shader does the conversion
		}
	}
	unsigned int fmt = fourcc == DRM_FORMAT_R8 ? GL_R8 : fourcc == DRM_FORMAT_GR88 ? GL_RG8 : 0;
	long bpp = fourcc == DRM_FORMAT_R8 ? 1 : 2;
	off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
	void *ctx = ((void *(*)(void))real("eglGetCurrentContext"))();
	if (!fmt || w <= 0 || h <= 0 || pitch != w * bpp || modifier || size < offset + pitch * h || !ctx) {
		if (trace)
			fprintf(stderr, "quest1_angle_egl: dma-buf image %ldx%ld %.4s, pitch %ld, offset %ld, %lld bytes: not made\n",
			        w, h, (const char *)&fourcc, pitch, offset, (long long)size);
		return NULL;
	}

	static void (*create_mo)(int, unsigned int *), (*delete_mo)(int, const unsigned int *);
	static void (*import_fd)(unsigned int, uint64_t, unsigned int, int);
	static void (*storage)(unsigned int, int, unsigned int, int, int, unsigned int, uint64_t, unsigned int,
	                       unsigned int, const void *);
	static void (*gen)(int, unsigned int *), (*del)(int, const unsigned int *), (*bind)(unsigned int, unsigned int);
	static void (*param)(unsigned int, unsigned int, int), (*get)(unsigned int, int *);
	static unsigned int (*error)(void);
	if (!storage) {
		*(void **)&create_mo = gl("glCreateMemoryObjectsEXT");
		*(void **)&delete_mo = gl("glDeleteMemoryObjectsEXT");
		*(void **)&import_fd = gl("glImportMemoryFdEXT");
		*(void **)&gen = gl("glGenTextures");
		*(void **)&del = gl("glDeleteTextures");
		*(void **)&bind = gl("glBindTexture");
		*(void **)&param = gl("glTexParameteri");
		*(void **)&get = gl("glGetIntegerv");
		*(void **)&error = gl("glGetError");
		*(void **)&storage = gl("glTexStorageMemFlags2DANGLE");
	}
	if (!create_mo || !delete_mo || !import_fd || !gen || !del || !bind || !param || !get || !error || !storage)
		return NULL;
	int dfd = fcntl((int)fd, F_DUPFD_CLOEXEC, 0); // the import owns the fd it is given
	if (dfd < 0)
		return NULL;
	unsigned int mo = 0, tex = 0, before = error();
	int was = 0;
	get(GL_TEXTURE_BINDING_2D, &was);
	create_mo(1, &mo);
	import_fd(mo, (uint64_t)size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dfd);
	gen(1, &tex);
	bind(GL_TEXTURE_2D, tex);
	param(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_LINEAR_TILING_EXT);
	// sampled, copied, and drawn into (a framebuffer on it is complete: it can be read back)
	struct quest1_plane_offset plane = {QUEST1_STRUCTURE_TYPE_PLANE_OFFSET, NULL, (uint64_t)offset};
	storage(GL_TEXTURE_2D, 1, fmt, (int)w, (int)h, mo, 0, 0, VK_USAGE_SAMPLED_COPY_DRAWN, &plane);
	unsigned int err = error();
	bind(GL_TEXTURE_2D, (unsigned int)was);
	delete_mo(1, &mo); // the texture keeps the memory
	void *img = NULL;
	if (!err) {
		static const int level0[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
		img = real_create_image(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (void *)(uintptr_t)tex, level0);
	}
	if (img) {
		pthread_mutex_lock(&imgs_lock);
		int i = 0;
		while (i < NIMG && imgs[i].image)
			i++;
		if (i < NIMG)
			imgs[i].image = img, imgs[i].tex = tex;
		pthread_mutex_unlock(&imgs_lock);
		if (i == NIMG)
			fprintf(stderr, "quest1_angle_egl: more than %d dma-buf images: a texture will stay\n", NIMG);
	} else {
		del(1, &tex);
	}
	if (trace || !img)
		fprintf(stderr, "quest1_angle_egl: dma-buf image %ldx%ld %.4s, pitch %ld, offset %ld: %p (OpenGL error %#x%s)\n",
		        w, h, (const char *)&fourcc, pitch, offset, img, err, before ? ", one was pending before" : "");
	return img;
}

EXPORT void *eglCreateImageKHR(void *dpy, void *ctx, unsigned int target, void *buffer, const int *attr)
{
	if (!real_create_image && !(*(void **)&real_create_image = real("eglCreateImageKHR")))
		return NULL;
	if (target == EGL_LINUX_DMA_BUF_EXT)
		return import_plane(dpy, attr);
	return real_create_image(dpy, ctx, target, buffer, attr);
}

EXPORT unsigned int eglDestroyImageKHR(void *dpy, void *img)
{
	static unsigned int (*p)(void *, void *);
	if (!p && !(*(void **)&p = real("eglDestroyImageKHR")))
		return 0;
	unsigned int r = p(dpy, img), tex = 0;
	pthread_mutex_lock(&imgs_lock);
	for (int i = 0; img && i < NIMG; i++)
		if (imgs[i].image == img) {
			tex = imgs[i].tex;
			imgs[i].image = NULL;
		}
	pthread_mutex_unlock(&imgs_lock);
	static void (*del)(int, const unsigned int *);
	if (tex && ((void *(*)(void))real("eglGetCurrentContext"))() && (del || (*(void **)&del = gl("glDeleteTextures"))))
		del(1, &tex);
	return r;
}

EXPORT void *eglGetProcAddress(const char *name)
{
	// vrclient asks for "pglFramebufferRenderbuffer" (its own name for the pointer): the function meant
	if (!strcmp(name, "pglFramebufferRenderbuffer"))
		name++;
	if (!strcmp(name, "eglCreateImageKHR"))
		return (void *)eglCreateImageKHR;
	if (!strcmp(name, "eglDestroyImageKHR"))
		return (void *)eglDestroyImageKHR;
	if (!strcmp(name, "eglQueryString"))
		return (void *)eglQueryString;
	void *p = !strcmp(name, "glRenderbufferStorage") ? (void *)q1_glRenderbufferStorage : gl(name);
	if (p && trace > 0 && !strcmp(name, "glBlitFramebuffer"))
		p = (void *)q1_trace_glBlitFramebuffer;
	if (!p && !strcmp(name, "glCheckNamedFramebufferStatus"))
		p = (void *)q1_glCheckNamedFramebufferStatus;
	// desktop OpenGL's ARB names of what OpenGL ES has in core (the client's frame import asks for
	// glActiveTextureARB, and calls it even when it was not found)
	size_t len = strlen(name);
	if (!p && len > 5 && len < 96 && !strcmp(name + len - 3, "ARB")) {
		char core[96];
		memcpy(core, name, len - 3);
		core[len - 3] = 0;
		p = gl(core);
	}
	if (!p || trace > 0)
		fprintf(stderr, "quest1_angle_egl: eglGetProcAddress(%s) = %p\n", name, p);
	return p;
}
