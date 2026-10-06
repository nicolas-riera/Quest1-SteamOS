// test_angle_x11: ANGLE (Vulkan backend) on an X11 window through quest1_vkshim, like the Steam
// webhelper GPU process. Renders, swaps, reads the window back (top and bottom halves).
//   TEST=clear  clear to red (default)
//   TEST=draw   shader-drawn red triangle (needs ANGLE_FEATURE_OVERRIDES_DISABLED=supportsExtendedDynamicState
//               on the Adreno blob: its vertex-binding-stride dynamic state draws nothing)
//   TEST=copy   top half red / bottom blue, glCopyTexImage2D from the window, drawn back
//   TEST=blit   same through glBlitFramebuffer into a texture (ES3)
//   TEST=backdrop  copy a region crossing the middle (glCopyTexSubImage2D) and draw it back in place
//   TEST=flipblit  a blit flipping Y (as Skia does between surfaces of different origins): halves swapped
//   ORIENT=1    EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE on the window surface, as Chromium does
// Run with the steam-webhelper-gpu environment and ANGLE_DIR=~/.local/share/Steam/steamrtarm64.
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef intptr_t EGLAttrib;
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#define EGL_SURFACE_ORIENTATION_ANGLE 0x33A8
#define EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE 0x0002
#define EGL_NONE 0x3038

#define W 320
#define H 200

int main(int argc, char **argv)
{
	const char *dir = getenv("ANGLE_DIR");
	const char *test = getenv("TEST") ? getenv("TEST") : "clear";
	char path[512];
	snprintf(path, sizeof(path), "%s/libEGL.so", dir ? dir : ".");
	void *egl = dlopen(path, RTLD_NOW);
	snprintf(path, sizeof(path), "%s/libGLESv2.so", dir ? dir : ".");
	void *gles = dlopen(path, RTLD_NOW);
	if (!egl || !gles) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
#define F(lib, ret, name, ...) ret (*name)(__VA_ARGS__) = (ret(*)(__VA_ARGS__))dlsym(lib, #name)
	F(egl, EGLDisplay, eglGetPlatformDisplay, int, void *, const EGLAttrib *);
	F(egl, int, eglInitialize, EGLDisplay, EGLint *, EGLint *);
	F(egl, int, eglChooseConfig, EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	F(egl, EGLSurface, eglCreateWindowSurface, EGLDisplay, EGLConfig, unsigned long, const EGLint *);
	F(egl, EGLContext, eglCreateContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	F(egl, int, eglMakeCurrent, EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	F(egl, int, eglSwapBuffers, EGLDisplay, EGLSurface);
	F(egl, EGLint, eglGetError, void);
	F(gles, void, glClearColor, float, float, float, float);
	F(gles, void, glClear, unsigned);
	F(gles, void, glFinish, void);
	F(gles, const char *, glGetString, unsigned);
	F(gles, unsigned, glCreateShader, unsigned);
	F(gles, void, glShaderSource, unsigned, int, const char *const *, const int *);
	F(gles, void, glCompileShader, unsigned);
	F(gles, unsigned, glCreateProgram, void);
	F(gles, void, glAttachShader, unsigned, unsigned);
	F(gles, void, glLinkProgram, unsigned);
	F(gles, void, glUseProgram, unsigned);
	F(gles, void, glBindAttribLocation, unsigned, unsigned, const char *);
	F(gles, void, glVertexAttribPointer, unsigned, int, unsigned, unsigned char, int, const void *);
	F(gles, void, glEnableVertexAttribArray, unsigned);
	F(gles, void, glDrawArrays, unsigned, int, int);
	F(gles, void, glViewport, int, int, int, int);
	F(gles, void, glScissor, int, int, int, int);
	F(gles, void, glEnable, unsigned);
	F(gles, void, glDisable, unsigned);
	F(gles, unsigned, glGetError, void);
	F(gles, void, glGenTextures, int, unsigned *);
	F(gles, void, glBindTexture, unsigned, unsigned);
	F(gles, void, glTexParameteri, unsigned, unsigned, int);
	F(gles, void, glTexImage2D, unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
	F(gles, void, glCopyTexImage2D, unsigned, int, unsigned, int, int, int, int, int);
	F(gles, void, glGenFramebuffers, int, unsigned *);
	F(gles, void, glBindFramebuffer, unsigned, unsigned);
	F(gles, void, glFramebufferTexture2D, unsigned, unsigned, unsigned, unsigned, int);
	F(gles, void, glBlitFramebuffer, int, int, int, int, int, int, int, int, unsigned, unsigned);
	F(gles, void, glCopyTexSubImage2D, unsigned, int, int, int, int, int, int, int);
	F(gles, int, glGetUniformLocation, unsigned, const char *);
	F(gles, void, glUniform2f, int, float, float);
	enum { RX = 60, RY = 40, RW = 200, RH = 120 };

	Display *x = XOpenDisplay(NULL);
	Window w = XCreateSimpleWindow(x, DefaultRootWindow(x), 0, 0, W, H, 0, 0, 0);
	XMapWindow(x, w);
	XSync(x, False);

	EGLAttrib da[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, EGL_NONE};
	EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, x, da);
	EGLint maj, min;
	if (!eglInitialize(d, &maj, &min)) {
		fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError());
		return 1;
	}
	EGLint ca[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3033, 4 /* window */, 0x3040, 0x40 /* ES3 */, EGL_NONE};
	EGLConfig cfg;
	EGLint n = 0;
	eglChooseConfig(d, ca, &cfg, 1, &n);
	EGLint sa[] = {EGL_SURFACE_ORIENTATION_ANGLE, EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE, EGL_NONE};
	EGLSurface s = eglCreateWindowSurface(d, cfg, w, getenv("ORIENT") ? sa : NULL);
	EGLint cx[] = {0x3098, 3, EGL_NONE};
	EGLContext c = eglCreateContext(d, cfg, NULL, cx);
	if (!s || !c || !eglMakeCurrent(d, s, s, c)) {
		fprintf(stderr, "surface/context failed 0x%x\n", eglGetError());
		return 1;
	}
	printf("GL_RENDERER %s, test %s%s\n", glGetString(0x1F01), test, getenv("ORIENT") ? ", inverted surface" : "");
	if (getenv("SHOWEXT")) {
		F(egl, const char *, eglQueryString, EGLDisplay, EGLint);
		F(egl, int, eglQuerySurface, EGLDisplay, EGLSurface, EGLint, EGLint *);
		EGLint o = -1;
		eglQuerySurface(d, s, EGL_SURFACE_ORIENTATION_ANGLE, &o);
		printf("EGL extensions: %s\nsurface orientation %d\n", eglQueryString(d, 0x3055), o);
	}

	static const float tri[] = {-1, -1, 3, -1, -1, 3};
	const char *vs = "attribute vec2 p; varying vec2 uv; void main() { uv = (p + 1.0) * 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
	const char *fs_red = "precision mediump float; varying vec2 uv; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }";
	const char *fs_tex = "precision mediump float; varying vec2 uv; uniform sampler2D t; uniform vec2 sc; void main() { gl_FragColor = texture2D(t, uv * sc); }";
	int draw = !strcmp(test, "draw"), copy = !strcmp(test, "copy"), blit = !strcmp(test, "blit"), flip = !strcmp(test, "flipblit");
	int backdrop = !strcmp(test, "backdrop");
	blit |= flip;
	copy |= backdrop;
	unsigned prog = 0;
	if (draw || copy || blit) {
		unsigned v = glCreateShader(0x8B31), f = glCreateShader(0x8B30);
		const char *fs = draw ? fs_red : fs_tex;
		glShaderSource(v, 1, &vs, NULL);
		glCompileShader(v);
		glShaderSource(f, 1, &fs, NULL);
		glCompileShader(f);
		prog = glCreateProgram();
		glAttachShader(prog, v);
		glAttachShader(prog, f);
		glBindAttribLocation(prog, 0, "p");
		glLinkProgram(prog);
	}
	unsigned tex = 0, fbo = 0;
	if (copy || blit) {
		glGenTextures(1, &tex);
		glBindTexture(0x0DE1, tex);
		glTexParameteri(0x0DE1, 0x2801, 0x2600); // nearest
		glTexParameteri(0x0DE1, 0x2800, 0x2600);
		glTexImage2D(0x0DE1, 0, 0x1908, W, H, 0, 0x1908, 0x1401, NULL);
		if (blit) {
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(0x8D40, fbo);
			glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
			glBindFramebuffer(0x8D40, 0);
		}
	}

	int frames = argc > 1 ? atoi(argv[1]) : 10;
	for (int i = 0; i < frames; i++) {
		glViewport(0, 0, W, H);
		if (copy || blit) {
			// GL coordinates: y = 0 is the bottom. Top half red, bottom half blue
			glEnable(0x0C11);
			glScissor(0, H / 2, W, H / 2);
			glClearColor(1, 0, 0, 1);
			glClear(0x4000);
			glScissor(0, 0, W, H / 2);
			glClearColor(0, 0, 1, 1);
			glClear(0x4000);
			glDisable(0x0C11);
			if (backdrop) {
				// like a CSS backdrop-filter: read a region crossing the middle, draw it back in place
				glBindTexture(0x0DE1, tex);
				glCopyTexSubImage2D(0x0DE1, 0, 0, 0, RX, RY, RW, RH);
			} else if (copy) {
				glBindTexture(0x0DE1, tex);
				glCopyTexImage2D(0x0DE1, 0, 0x1908, 0, 0, W, H, 0);
			} else {
				glBindFramebuffer(0x8CA9, fbo); // draw framebuffer
				if (flip)
					glBlitFramebuffer(0, 0, W, H, 0, H, W, 0, 0x4000, 0x2600);
				else
					glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, 0x4000, 0x2600);
				glBindFramebuffer(0x8CA9, 0);
			}
			if (i == 0) {
				// GL-coordinate row 10 of the window and of the copy must agree
				F(gles, void, glReadPixels, int, int, int, int, unsigned, unsigned, void *);
				unsigned char a[4], b[4];
				glReadPixels(W / 2, 10, 1, 1, 0x1908, 0x1401, a);
				unsigned rf;
				glGenFramebuffers(1, &rf);
				glBindFramebuffer(0x8D40, rf);
				glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
				glReadPixels(W / 2, 10, 1, 1, 0x1908, 0x1401, b);
				glBindFramebuffer(0x8D40, 0);
				printf("row 10: window %02x%02x%02x, copy %02x%02x%02x: %s\n", a[0], a[1], a[2], b[0], b[1], b[2],
				       memcmp(a, b, 3) ? "MISMATCH" : "match");
			}
			if (!backdrop) {
				glClearColor(0, 1, 0, 1);
				glClear(0x4000);
			}
		} else {
			glClearColor(draw ? 0 : 1, 0, 0, 1);
			glClear(0x4000);
		}
		if (prog) {
			glUseProgram(prog);
			glUniform2f(glGetUniformLocation(prog, "sc"), backdrop ? (float)RW / W : 1, backdrop ? (float)RH / H : 1);
			if (backdrop)
				glViewport(RX, RY, RW, RH);
			glBindTexture(0x0DE1, tex);
			glVertexAttribPointer(0, 2, 0x1406, 0, 0, tri);
			glEnableVertexAttribArray(0);
			glDrawArrays(4, 0, 3);
		}
		if (i == 0 && glGetError())
			printf("GL error in frame 0\n");
		if (!eglSwapBuffers(d, s))
			fprintf(stderr, "swap failed 0x%x\n", eglGetError());
	}
	glFinish();
	eglMakeCurrent(d, NULL, NULL, NULL);
	XSync(x, False);
	usleep(200000);
	unsigned long top, bottom;
	XImage *img = XGetImage(x, w, W / 2, H / 4, 1, 1, ~0ul, ZPixmap);
	top = img ? XGetPixel(img, 0, 0) & 0xffffff : 0xdeadbeef;
	img = XGetImage(x, w, W / 2, 3 * H / 4, 1, 1, ~0ul, ZPixmap);
	bottom = img ? XGetPixel(img, 0, 0) & 0xffffff : 0xdeadbeef;
	unsigned long want_top = flip ? 0x0000ff : 0xff0000;
	unsigned long want_bottom = flip ? 0xff0000 : (copy || blit) ? 0x0000ff : 0xff0000;
	int pass = top == want_top && bottom == want_bottom;
	printf("top 0x%06lx bottom 0x%06lx: %s\n", top, bottom, pass ? "PASS" : "FAIL");
	return !pass;
}
