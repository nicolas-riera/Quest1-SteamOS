/*
 * hybris_ahb: the NDK AHardwareBuffer API for glibc programs, forwarded to the
 * Android 10 libnativewindow through libhybris. Lets Monado use its Android
 * (AHardwareBuffer) swapchain path on Linux, the only image sharing the
 * Quest's Vulkan driver supports.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <android/hardware_buffer.h>
#include <hybris/common/binding.h>

static struct {
	int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
	void (*acquire)(AHardwareBuffer *);
	void (*release)(AHardwareBuffer *);
	void (*describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
	int (*is_supported)(const AHardwareBuffer_Desc *);
	int (*send)(const AHardwareBuffer *, int);
	int (*recv)(int, AHardwareBuffer **);
} nw;

static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
	void *h = android_dlopen("libnativewindow.so", RTLD_NOW);
	if (!h) {
		fprintf(stderr, "hybris_ahb: cannot load libnativewindow.so through libhybris\n");
		abort();
	}
	nw.allocate = android_dlsym(h, "AHardwareBuffer_allocate");
	nw.acquire = android_dlsym(h, "AHardwareBuffer_acquire");
	nw.release = android_dlsym(h, "AHardwareBuffer_release");
	nw.describe = android_dlsym(h, "AHardwareBuffer_describe");
	nw.is_supported = android_dlsym(h, "AHardwareBuffer_isSupported");
	nw.send = android_dlsym(h, "AHardwareBuffer_sendHandleToUnixSocket");
	nw.recv = android_dlsym(h, "AHardwareBuffer_recvHandleFromUnixSocket");
}

#define NW() pthread_once(&once, load)

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out)
{
	NW();
	return nw.allocate(desc, out);
}

void AHardwareBuffer_acquire(AHardwareBuffer *buffer)
{
	NW();
	nw.acquire(buffer);
}

void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
	NW();
	nw.release(buffer);
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *out)
{
	NW();
	nw.describe(buffer, out);
}

int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc *desc)
{
	NW();
	return nw.is_supported ? nw.is_supported(desc) : 1;
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int fd)
{
	NW();
	return nw.send(buffer, fd);
}

int AHardwareBuffer_recvHandleFromUnixSocket(int fd, AHardwareBuffer **out)
{
	NW();
	return nw.recv(fd, out);
}
