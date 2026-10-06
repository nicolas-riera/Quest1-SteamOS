// lsetest: check the kernel's ARMv8.1 LSE / ARMv8.3 LDAPR emulation (kernel/lse_emul.c).
// Build: gcc -O2 -march=armv8.3-a -pthread -o lsetest lsetest.c
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static int fails;
#define CHECK(name, got, want)                                                                              \
	do {                                                                                                \
		unsigned long long g_ = (got), w_ = (want);                                                 \
		if (g_ != w_) {                                                                             \
			printf("FAIL %-28s got %#llx want %#llx\n", name, g_, w_);                          \
			fails++;                                                                            \
		}                                                                                           \
	} while (0)

static uint64_t counter;

static void *adder(void *arg)
{
	for (int i = 0; i < 250000; i++) {
		uint64_t one = 1, old;
		asm volatile("ldaddal %[v], %[o], [%[p]]" : [o] "=r"(old) : [v] "r"(one), [p] "r"(&counter) : "memory");
	}
	return NULL;
}

int main(void)
{
	uint64_t m64, o;
	uint32_t m32, o32;
	uint16_t m16, o16;
	uint8_t m8, o8;

	m64 = 40;
	asm volatile("ldadd %[v], %[o], [%[p]]" : [o] "=r"(o) : [v] "r"(2ULL), [p] "r"(&m64) : "memory");
	CHECK("ldadd x", o, 40);
	CHECK("ldadd x mem", m64, 42);

	m32 = 0xff;
	asm volatile("ldclral %w[v], %w[o], [%[p]]" : [o] "=r"(o32) : [v] "r"(0x0f), [p] "r"(&m32) : "memory");
	CHECK("ldclral w", o32, 0xff);
	CHECK("ldclral w mem", m32, 0xf0);

	m16 = 0x1234;
	asm volatile("ldeorh %w[v], %w[o], [%[p]]" : [o] "=r"(o16) : [v] "r"(0xffff), [p] "r"(&m16) : "memory");
	CHECK("ldeorh", o16, 0x1234);
	CHECK("ldeorh mem", m16, 0xedcb);

	m8 = 0x10;
	asm volatile("ldsetb %w[v], %w[o], [%[p]]" : [o] "=r"(o8) : [v] "r"(0x01), [p] "r"(&m8) : "memory");
	CHECK("ldsetb", o8, 0x10);
	CHECK("ldsetb mem", m8, 0x11);

	m32 = (uint32_t)-5;
	asm volatile("ldsmax %w[v], %w[o], [%[p]]" : [o] "=r"(o32) : [v] "r"(3), [p] "r"(&m32) : "memory");
	CHECK("ldsmax w (signed)", m32, 3);
	m32 = (uint32_t)-5;
	asm volatile("ldumax %w[v], %w[o], [%[p]]" : [o] "=r"(o32) : [v] "r"(3), [p] "r"(&m32) : "memory");
	CHECK("ldumax w (unsigned)", m32, (uint32_t)-5);
	m8 = 0x80;
	asm volatile("ldsminb %w[v], %w[o], [%[p]]" : [o] "=r"(o8) : [v] "r"(1), [p] "r"(&m8) : "memory");
	CHECK("ldsminb", m8, 0x80);

	m64 = 7;
	asm volatile("swpal %[v], %[o], [%[p]]" : [o] "=r"(o) : [v] "r"(9ULL), [p] "r"(&m64) : "memory");
	CHECK("swpal", o, 7);
	CHECK("swpal mem", m64, 9);

	m64 = 5;
	asm volatile("stadd %[v], [%[p]]" ::[v] "r"(3ULL), [p] "r"(&m64) : "memory");
	CHECK("stadd", m64, 8);

	// CAS: success and failure
	m64 = 100;
	o = 100;
	asm volatile("casal %[c], %[n], [%[p]]" : [c] "+r"(o) : [n] "r"(200ULL), [p] "r"(&m64) : "memory");
	CHECK("casal ok old", o, 100);
	CHECK("casal ok mem", m64, 200);
	o = 1;
	asm volatile("casal %[c], %[n], [%[p]]" : [c] "+r"(o) : [n] "r"(300ULL), [p] "r"(&m64) : "memory");
	CHECK("casal fail old", o, 200);
	CHECK("casal fail mem", m64, 200);
	m16 = 0xbeef;
	o16 = 0xbeef;
	asm volatile("casah %w[c], %w[n], [%[p]]" : [c] "+r"(o16) : [n] "r"(0x1), [p] "r"(&m16) : "memory");
	CHECK("casah mem", m16, 1);

	// CASP 2x64
	{
		__attribute__((aligned(16))) uint64_t pair[2] = {11, 22};
		register uint64_t c0 asm("x4") = 11, c1 asm("x5") = 22, n0 asm("x6") = 33, n1 asm("x7") = 44;
		asm volatile("caspal x4, x5, x6, x7, [%[p]]" : "+r"(c0), "+r"(c1) : "r"(n0), "r"(n1), [p] "r"(pair)
			     : "memory");
		CHECK("caspal old lo", c0, 11);
		CHECK("caspal old hi", c1, 22);
		CHECK("caspal mem lo", pair[0], 33);
		CHECK("caspal mem hi", pair[1], 44);
	}
	// CASP 2x32
	{
		__attribute__((aligned(8))) uint32_t pair[2] = {1, 2};
		register uint32_t c0 asm("w8") = 1, c1 asm("w9") = 2, n0 asm("w10") = 3, n1 asm("w11") = 4;
		asm volatile("casp w8, w9, w10, w11, [%[p]]" : "+r"(c0), "+r"(c1) : "r"(n0), "r"(n1), [p] "r"(pair)
			     : "memory");
		CHECK("casp w mem", ((uint64_t)pair[1] << 32) | pair[0], (4ULL << 32) | 3);
	}

	// LDAPR (ARMv8.3)
	m64 = 0xabcdef;
	asm volatile("ldapr %[o], [%[p]]" : [o] "=r"(o) : [p] "r"(&m64) : "memory");
	CHECK("ldapr", o, 0xabcdef);

	// Atomicity under contention
	pthread_t t[4];
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, adder, NULL);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	CHECK("4 threads x 250000 ldaddal", counter, 1000000);
	double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e6;
	printf("contended ldaddal: %.0f ns per op (4 threads, wall / ops)\n", ns);

	printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
	return fails != 0;
}
