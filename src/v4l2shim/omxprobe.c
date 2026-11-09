// omxprobe: encode a synthetic clip with Android's own encoder (the vendor libOmxVenc, through libOmxCore and
// libhybris), to see what a working venus encode session looks like: with the msm_vidc firmware log on, its
// SET_PROPERTY list and buffer registration (BufferChk) show up in dmesg next to ours. This is how quest1_v4l2's
// PREPARE_BUF of every buffer was found (2026-11-09). Nothing of the vendor's is built in: it is loaded at run time.
//   gcc -O2 -I<OpenMAX IL headers> -I/opt/hybris/include -o omxprobe omxprobe.c -L/opt/hybris/lib
//       -Wl,-rpath,/opt/hybris/lib -lhybris-common -lpthread
//   (headers: AOSP frameworks/native/headers/media_plugin/media/openmax, Khronos OpenMAX IL 1.1)
//   D=/sys/kernel/debug/msm_vidc; echo 0x3f > $D/fw_level; echo 0x1003 > $D/debug_level   (firmware log; it takes
//   effect when the firmware loads again, 15 s after the last session; put back 0x18 and 0x3 afterwards)
//   LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./omxprobe [OMX.qcom.video.encoder.avc|.hevc]
#include <OMX_Component.h>
#include <OMX_Core.h>
#include <OMX_Video.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void *android_dlopen(const char *filename, int flag);
extern void *android_dlsym(void *handle, const char *symbol);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int cmd_done = -1, filled, emptied, errors;
static OMX_BUFFERHEADERTYPE *inb[32], *outb[32];
static int nin, nout;
static int free_in[32];

static OMX_ERRORTYPE on_event(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE e, OMX_U32 d1, OMX_U32 d2, OMX_PTR data)
{
	printf("event %d %u %u\n", e, d1, d2);
	pthread_mutex_lock(&mu);
	if (e == OMX_EventCmdComplete)
		cmd_done = (int)d2;
	if (e == OMX_EventError)
		errors++;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	return OMX_ErrorNone;
}

static OMX_ERRORTYPE on_empty(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
	pthread_mutex_lock(&mu);
	emptied++;
	for (int i = 0; i < nin; i++)
		if (inb[i] == b)
			free_in[i] = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	return OMX_ErrorNone;
}

static OMX_HANDLETYPE comp;
static OMX_ERRORTYPE on_fill(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
	pthread_mutex_lock(&mu);
	filled++;
	if (filled <= 3 || filled % 30 == 0)
		printf("fill %d: %u bytes flags 0x%x\n", filled, b->nFilledLen, b->nFlags);
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	b->nFilledLen = 0;
	OMX_FillThisBuffer(h, b);
	return OMX_ErrorNone;
}

static void wait_cmd(int state)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 3;
	pthread_mutex_lock(&mu);
	while (cmd_done != state && !errors)
		if (pthread_cond_timedwait(&cv, &mu, &ts)) {
			printf("timeout waiting for state %d\n", state);
			break;
		}
	pthread_mutex_unlock(&mu);
}

#define INIT(x) (memset(&(x), 0, sizeof(x)), (x).nSize = sizeof(x), (x).nVersion.s.nVersionMajor = 1, (x).nVersion.s.nVersionMinor = 1)

int main(int argc, char **argv)
{
	int W = 1280, H = 720, N = 90;
	void *core = android_dlopen("libOmxCore.so", RTLD_NOW);
	if (!core) {
		printf("no libOmxCore\n");
		return 1;
	}
	OMX_ERRORTYPE (*init)(void) = android_dlsym(core, "OMX_Init");
	OMX_ERRORTYPE (*gethandle)(OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE *) =
		android_dlsym(core, "OMX_GetHandle");
	OMX_ERRORTYPE (*freehandle)(OMX_HANDLETYPE) = android_dlsym(core, "OMX_FreeHandle");
	printf("init %x\n", init());
	OMX_CALLBACKTYPE cb = {on_event, on_empty, on_fill};
	OMX_ERRORTYPE r = gethandle(&comp, (OMX_STRING)(argc > 1 ? argv[1] : "OMX.qcom.video.encoder.avc"), NULL, &cb);
	printf("gethandle %x\n", r);
	if (r)
		return 1;
	OMX_PARAM_PORTDEFINITIONTYPE pd;
	INIT(pd);
	pd.nPortIndex = 0;
	r = OMX_GetParameter(comp, OMX_IndexParamPortDefinition, &pd);
	pd.format.video.nFrameWidth = W;
	pd.format.video.nFrameHeight = H;
	pd.format.video.nStride = W;
	pd.format.video.nSliceHeight = H;
	pd.format.video.xFramerate = 60 << 16;
	pd.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
	printf("in port set %x\n", OMX_SetParameter(comp, OMX_IndexParamPortDefinition, &pd));
	INIT(pd);
	pd.nPortIndex = 1;
	OMX_GetParameter(comp, OMX_IndexParamPortDefinition, &pd);
	pd.format.video.nFrameWidth = W;
	pd.format.video.nFrameHeight = H;
	pd.format.video.nBitrate = 20000000;
	pd.format.video.xFramerate = 60 << 16;
	pd.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
	printf("out port set %x\n", OMX_SetParameter(comp, OMX_IndexParamPortDefinition, &pd));
	OMX_VIDEO_PARAM_BITRATETYPE br;
	INIT(br);
	br.nPortIndex = 1;
	br.eControlRate = OMX_Video_ControlRateVariable;
	br.nTargetBitrate = 20000000;
	printf("bitrate %x\n", OMX_SetParameter(comp, OMX_IndexParamVideoBitrate, &br));

	OMX_PARAM_PORTDEFINITIONTYPE ip, op;
	INIT(ip);
	ip.nPortIndex = 0;
	OMX_GetParameter(comp, OMX_IndexParamPortDefinition, &ip);
	INIT(op);
	op.nPortIndex = 1;
	OMX_GetParameter(comp, OMX_IndexParamPortDefinition, &op);
	printf("in: %u x %u bytes, out: %u x %u bytes\n", ip.nBufferCountActual, ip.nBufferSize, op.nBufferCountActual,
	       op.nBufferSize);
	nin = ip.nBufferCountActual;
	nout = op.nBufferCountActual;
	OMX_SendCommand(comp, OMX_CommandStateSet, OMX_StateIdle, NULL);
	for (int i = 0; i < nin; i++) {
		r = OMX_AllocateBuffer(comp, &inb[i], 0, NULL, ip.nBufferSize);
		if (r)
			printf("alloc in %d: %x\n", i, r);
		free_in[i] = 1;
	}
	for (int i = 0; i < nout; i++) {
		r = OMX_AllocateBuffer(comp, &outb[i], 1, NULL, op.nBufferSize);
		if (r)
			printf("alloc out %d: %x\n", i, r);
	}
	wait_cmd(OMX_StateIdle);
	printf("idle\n");
	OMX_SendCommand(comp, OMX_CommandStateSet, OMX_StateExecuting, NULL);
	wait_cmd(OMX_StateExecuting);
	printf("executing\n");
	for (int i = 0; i < nout; i++)
		OMX_FillThisBuffer(comp, outb[i]);
	for (int n = 0; n < N && !errors; n++) {
		pthread_mutex_lock(&mu);
		int k = -1;
		while (k < 0 && !errors) {
			for (int i = 0; i < nin; i++)
				if (free_in[i]) {
					k = i;
					break;
				}
			if (k < 0)
				pthread_cond_wait(&cv, &mu);
		}
		if (k >= 0)
			free_in[k] = 0;
		pthread_mutex_unlock(&mu);
		if (k < 0)
			break;
		OMX_BUFFERHEADERTYPE *b = inb[k];
		memset(b->pBuffer, (n * 7) & 0xff, b->nAllocLen);
		b->nFilledLen = W * H * 3 / 2;
		b->nOffset = 0;
		b->nTimeStamp = (OMX_TICKS)n * 1000000 / 60;
		b->nFlags = 0;
		r = OMX_EmptyThisBuffer(comp, b);
		if (r)
			printf("etb %x\n", r);
		usleep(16000);
	}
	usleep(200000);
	printf("filled %d emptied %d errors %d\n", filled, emptied, errors);
	OMX_SendCommand(comp, OMX_CommandStateSet, OMX_StateIdle, NULL);
	wait_cmd(OMX_StateIdle);
	OMX_SendCommand(comp, OMX_CommandStateSet, OMX_StateLoaded, NULL);
	for (int i = 0; i < nin; i++)
		OMX_FreeBuffer(comp, 0, inb[i]);
	for (int i = 0; i < nout; i++)
		OMX_FreeBuffer(comp, 1, outb[i]);
	wait_cmd(OMX_StateLoaded);
	freehandle(comp);
	return 0;
}
