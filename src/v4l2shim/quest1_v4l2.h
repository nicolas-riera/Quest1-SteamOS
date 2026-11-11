// quest1_v4l2: the two halves of libquest1_v4l2.so. quest1_v4l2.c has the hooks (ioctl, mmap, close) and the
// encoder; quest1_vdec.c the decoder, reached through these.
#pragma once
#include <stddef.h>
#include <sys/types.h>

struct q1dec;

struct q1dec *q1dec_find(int fd);  // a decoder fd already tracked
struct q1dec *q1dec_adopt(int fd); // an fd not seen yet: tracked if it is the venus decoder
int q1dec_ioctl(struct q1dec *d, unsigned long req, void *arg);
void *q1dec_mmap(struct q1dec *d, void *addr, size_t len, int prot, int flags, off_t off);
void q1dec_closing(struct q1dec *d); // before close()
void q1dec_forget(struct q1dec *d);  // after close()
struct pollfd;
int q1dec_poll(struct q1dec *d, struct pollfd *fd, int timeout_ms); // poll() on a decoder fd alone
int q1_sys_poll(struct pollfd *fds, unsigned long n, int timeout_ms); // the system's poll, past the hook
