// quest1_streamclient: part of libquest1_v4l2.so, for Steam's stream client (Steam Link: a PC's game on the headset).
//
// streaming_client draws with SDL (its OpenVR video driver: the picture is a SteamVR overlay) on OpenGL, and
// vrclient shares that OpenGL texture with vrcompositor through GL_EXT_memory_object_fd, with no fallback: on the
// OpenGL it finds by itself here (Mesa's llvmpipe, on the CPU) it stops on
//   ASSERT "s_OpenGLFunctions.pGetUnsignedBytevEXT != nullptr" at sharedresource_linux.cpp:208
// Steam ships ANGLE (libEGL.so and libGLESv2.so next to the client: OpenGL ES on Vulkan, what its own browser
// draws with), and on the Adreno's Vulkan ANGLE has GL_EXT_memory_object_fd and GL_EXT_semaphore_fd.
//
// So the client, and no other process, gets libquest1_angle_egl.so as its libEGL.so.1 and libGL.so.1: the client
// starts itself again once with /usr/local/lib/quest1-streamclient first in its library path. Nothing of Valve's
// is changed.
#define _GNU_SOURCE
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GLDIR "/usr/local/lib/quest1-streamclient"

__attribute__((constructor)) static void quest1_streamclient_init(int argc, char **argv, char **envp)
{
	(void)argc;
	(void)envp;
	char exe[PATH_MAX], dir[PATH_MAX], lib[PATH_MAX + 32];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0)
		return;
	exe[n] = 0;
	const char *base = strrchr(exe, '/');
	if (!base || strcmp(base + 1, "streaming_client") != 0 || getenv("QUEST1_ANGLE_EGL"))
		return;
	strcpy(dir, exe);
	snprintf(lib, sizeof(lib), "%s/libEGL.so", dirname(dir));
	if (access(lib, R_OK) != 0 || access(GLDIR "/libEGL.so.1", R_OK) != 0 || access(GLDIR "/libGL.so.1", R_OK) != 0)
		return;
	setenv("QUEST1_ANGLE_EGL", lib, 1);
	setenv("ANGLE_DEFAULT_PLATFORM", "vulkan", 1);
	setenv("SDL_RENDER_DRIVER", "opengles2", 1); // SDL's renderer on that context
	// the Adreno driver's dynamic vertex-binding stride draws nothing: without this every draw of the client is
	// lost with no error (a clear still works), and the stream's window is transparent
	setenv("ANGLE_FEATURE_OVERRIDES_DISABLED", "supportsExtendedDynamicState", 1);
	const char *old = getenv("LD_LIBRARY_PATH");
	char *path;
	if (asprintf(&path, GLDIR "%s%s", old ? ":" : "", old ? old : "") < 0)
		return;
	setenv("LD_LIBRARY_PATH", path, 1);
	fprintf(stderr, "quest1_v4l2: stream client: OpenGL from ANGLE (%s)\n", lib);
	execv(exe, argv);
	fprintf(stderr, "quest1_v4l2: stream client: could not start again\n");
}
