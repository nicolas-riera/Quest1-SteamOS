/*
 * qb_link: Linux (glibc + libhybris) end of the qbridge socket.
 * Accepts the Android app, receives its shared AHardwareBuffers and tracking
 * samples, hands out free image slots and announces finished frames.
 * Not thread-safe: drive it from one thread (or lock around it).
 */
#ifndef QB_LINK_H
#define QB_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "qbridge_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AHardwareBuffer;

struct qb_link {
	int listen_fd;
	int fd;                        /* connected app, -1 if none */
	struct qb_hello hello;
	struct qb_buffers buffers;
	struct AHardwareBuffer *ahb[QB_MAX_SLOTS];
	bool slot_busy[QB_MAX_SLOTS];  /* owned by the app (being displayed) */
	struct qb_pose_msg pose;       /* latest tracking sample */
	bool have_pose;
	uint64_t generation;           /* bumped on every (re)connection */
};

/* Create the abstract listening socket. Returns 0 or -errno. */
int qb_link_init(struct qb_link *l);

/* Accept an app if one is waiting and run the handshake (HELLO, BUFFERS and
 * the AHardwareBuffers). timeout_ms < 0 blocks. Returns true when connected. */
bool qb_link_accept(struct qb_link *l, int timeout_ms);

/* Drain pending messages (POSE, RELEASE) without blocking. Returns false if
 * the app went away (buffers are then released). */
bool qb_link_poll(struct qb_link *l);

/* Block until a new POSE arrives (or timeout). Returns false on disconnect. */
bool qb_link_wait_pose(struct qb_link *l, int timeout_ms);

/* A slot the app doesn't hold, or -1. Linux owns it until qb_link_present. */
int qb_link_acquire(struct qb_link *l);

/* Rendering into `slot` is complete (GPU work finished); give it to the app. */
bool qb_link_present(struct qb_link *l, int slot, uint64_t pose_seq);

bool qb_link_haptic(struct qb_link *l, int hand, float amplitude, float frequency, int64_t duration_ns);

void qb_link_disconnect(struct qb_link *l);
void qb_link_fini(struct qb_link *l);

#ifdef __cplusplus
}
#endif

#endif
