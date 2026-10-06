/*
 * bionictls: LD_PRELOAD shim letting stock Android 10 bionic code run inside
 * glibc processes through libhybris.
 *
 * Bionic reads its per-thread state from TLS_SLOT_BIONIC_TLS = [tpidr_el0 - 8].
 * Under glibc 2.42 on aarch64 that word is the last member of struct pthread,
 * `getrandom_buf`, which stays NULL and unused when the kernel has no vDSO
 * getrandom (true for the Quest's 4.4 kernel). We park a zeroed fake
 * bionic_tls there for each thread, and clear it again before glibc's thread
 * teardown looks at getrandom_buf.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/auxv.h>

#define FAKE_BIONIC_TLS_SIZE (64 * 1024) /* bionic_tls is ~20 KiB on Q */

static inline void **bionic_tls_slot(void)
{
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return (void **)(tp - sizeof(void *));
}

static pthread_key_t cleanup_key;

static void release(void *area)
{
    void **slot = bionic_tls_slot();
    if (*slot == area)
        *slot = NULL;
    free(area);
}

static void install(void)
{
    void **slot = bionic_tls_slot();
    if (*slot)
        return;
    void *area = calloc(1, FAKE_BIONIC_TLS_SIZE);
    *slot = area;
    pthread_setspecific(cleanup_key, area); /* destructor runs on pthread_exit too */
}

struct start { void *(*fn)(void *); void *arg; };

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    install();
    void *ret = s.fn(s.arg);
    void **slot = bionic_tls_slot();
    void *area = *slot;
    *slot = NULL;
    pthread_setspecific(cleanup_key, NULL);
    free(area);
    return ret;
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real)
        real = dlsym(RTLD_NEXT, "pthread_create");
    struct start *s = malloc(sizeof(*s));
    if (!s)
        return real(t, attr, fn, arg);
    s->fn = fn;
    s->arg = arg;
    int r = real(t, attr, trampoline, s);
    if (r)
        free(s);
    return r;
}

__attribute__((constructor)) static void init(void)
{
    /* Only safe while glibc never uses getrandom_buf (no vDSO getrandom). */
    pthread_key_create(&cleanup_key, release);
    install();
}
