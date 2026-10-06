#define _GNU_SOURCE
#include "qb_link.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <hybris/common/binding.h>

#define LOG(...) fprintf(stderr, "qb_link: " __VA_ARGS__)

/* Bionic's libnativewindow, loaded through libhybris. */
static int (*p_recv_handle)(int fd, struct AHardwareBuffer **out);
static void (*p_ahb_release)(struct AHardwareBuffer *buf);

static bool load_nativewindow(void)
{
	if (p_recv_handle)
		return true;
	void *h = android_dlopen("libnativewindow.so", RTLD_NOW);
	if (!h) {
		LOG("android_dlopen(libnativewindow.so) failed\n");
		return false;
	}
	p_recv_handle = android_dlsym(h, "AHardwareBuffer_recvHandleFromUnixSocket");
	p_ahb_release = android_dlsym(h, "AHardwareBuffer_release");
	return p_recv_handle && p_ahb_release;
}

int qb_link_init(struct qb_link *l)
{
	memset(l, 0, sizeof(*l));
	l->fd = -1;
	if (!load_nativewindow())
		return -ENOENT;
	l->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (l->listen_fd < 0)
		return -errno;
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	memcpy(addr.sun_path + 1, QB_SOCKET_NAME, strlen(QB_SOCKET_NAME));
	socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(QB_SOCKET_NAME);
	if (bind(l->listen_fd, (struct sockaddr *)&addr, len) || listen(l->listen_fd, 1)) {
		int e = -errno;
		close(l->listen_fd);
		return e;
	}
	return 0;
}

static bool recv_exact(int fd, void *buf, size_t len, uint32_t type)
{
	ssize_t n = recv(fd, buf, len, 0);
	if (n != (ssize_t)len || ((struct qb_header *)buf)->type != type) {
		LOG("handshake: expected msg %u (%zu bytes), got %zd\n", type, len, n);
		return false;
	}
	return true;
}

void qb_link_disconnect(struct qb_link *l)
{
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
	for (int i = 0; i < QB_MAX_SLOTS; i++) {
		if (l->ahb[i])
			p_ahb_release(l->ahb[i]);
		l->ahb[i] = NULL;
		l->slot_busy[i] = false;
	}
	l->have_pose = false;
}

bool qb_link_accept(struct qb_link *l, int timeout_ms)
{
	if (l->fd >= 0)
		return true;
	struct pollfd p = {l->listen_fd, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) <= 0)
		return false;
	int fd = accept4(l->listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return false;
	l->fd = fd;
	if (!recv_exact(fd, &l->hello, sizeof(l->hello), QB_MSG_HELLO) ||
	    !recv_exact(fd, &l->buffers, sizeof(l->buffers), QB_MSG_BUFFERS) ||
	    l->buffers.count != QB_MAX_SLOTS)
		goto fail;
	for (uint32_t i = 0; i < l->buffers.count; i++) {
		if (p_recv_handle(fd, &l->ahb[i]) != 0) {
			LOG("recvHandleFromUnixSocket failed\n");
			goto fail;
		}
	}
	l->generation++;
	LOG("app connected: eye %ux%u @ %.0f Hz, hands %u, %u buffers %ux%u\n", l->hello.eye_width,
	    l->hello.eye_height, l->hello.refresh_rate, l->hello.has_hand_tracking, l->buffers.count,
	    l->buffers.width, l->buffers.height);
	return true;
fail:
	qb_link_disconnect(l);
	return false;
}

static bool handle(struct qb_link *l, const uint8_t *buf, ssize_t n)
{
	const struct qb_header *h = (const void *)buf;
	if (h->type == QB_MSG_POSE && n >= (ssize_t)sizeof(struct qb_pose_msg)) {
		memcpy(&l->pose, buf, sizeof(l->pose));
		l->have_pose = true;
		return true;
	}
	if (h->type == QB_MSG_RELEASE && n >= (ssize_t)sizeof(struct qb_release)) {
		const struct qb_release *r = (const void *)buf;
		if (r->slot < QB_MAX_SLOTS)
			l->slot_busy[r->slot] = false;
	}
	return false;
}

static bool drain(struct qb_link *l, int timeout_ms, bool want_pose)
{
	_Alignas(16) uint8_t buf[sizeof(struct qb_pose_msg) + 64];
	bool got_pose = false;
	for (;;) {
		int flags = MSG_DONTWAIT;
		if (want_pose && !got_pose) {
			struct pollfd p = {l->fd, POLLIN, 0};
			if (poll(&p, 1, timeout_ms) <= 0)
				return true;
		}
		ssize_t n = recv(l->fd, buf, sizeof(buf), flags);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			LOG("app disconnected\n");
			qb_link_disconnect(l);
			return false;
		}
		if (n < 0)
			return true;
		if (n >= (ssize_t)sizeof(struct qb_header))
			got_pose |= handle(l, buf, n);
	}
}

bool qb_link_poll(struct qb_link *l)
{
	return l->fd >= 0 && drain(l, 0, false);
}

bool qb_link_wait_pose(struct qb_link *l, int timeout_ms)
{
	return l->fd >= 0 && drain(l, timeout_ms, true);
}

int qb_link_acquire(struct qb_link *l)
{
	if (l->fd < 0)
		return -1;
	for (int i = 0; i < QB_MAX_SLOTS; i++)
		if (!l->slot_busy[i])
			return i;
	return -1;
}

static bool send_msg(struct qb_link *l, const void *msg, size_t len)
{
	if (l->fd < 0)
		return false;
	if (send(l->fd, msg, len, MSG_NOSIGNAL) != (ssize_t)len) {
		qb_link_disconnect(l);
		return false;
	}
	return true;
}

bool qb_link_present(struct qb_link *l, int slot, uint64_t pose_seq)
{
	if (slot < 0 || slot >= QB_MAX_SLOTS)
		return false;
	struct qb_frame f = {{QB_MSG_FRAME, sizeof(f)}, (uint32_t)slot, 0, pose_seq};
	l->slot_busy[slot] = true;
	return send_msg(l, &f, sizeof(f));
}

bool qb_link_haptic(struct qb_link *l, int hand, float amplitude, float frequency, int64_t duration_ns)
{
	struct qb_haptic h = {{QB_MSG_HAPTIC, sizeof(h)}, (uint32_t)hand, amplitude, frequency, duration_ns};
	return send_msg(l, &h, sizeof(h));
}

void qb_link_fini(struct qb_link *l)
{
	qb_link_disconnect(l);
	if (l->listen_fd >= 0)
		close(l->listen_fd);
}
