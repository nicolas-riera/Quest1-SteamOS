// quest1_display_frametrace: debug: a trace of vrcompositor's frame loop.
// Part of quest1_display.c (included by it, in this order: one translation unit).

// --- debug: `runuser -u steamos -- touch /tmp/quest1-frametrace` records 3 s of vrcompositor's frame loop (vblank
// ticks, display events, fence waits, acquires, submits, presents) into /tmp/quest1-frametrace.txt
#define FT_MAX 30000
static struct
{
	uint64_t t;
	const char *what;
	long tid;
	int64_t arg;
} ft_ev[FT_MAX];
static _Atomic unsigned ft_n;
static _Atomic int ft_on; //!< 0 idle, 1 recording, 2 dumping
static uint64_t ft_until;

static void ft(const char *what, int64_t arg)
{
	if (ft_on != 1)
		return;
	unsigned i = atomic_fetch_add(&ft_n, 1);
	if (i >= FT_MAX)
		return;
	ft_ev[i].t = now_ns();
	ft_ev[i].what = what;
	ft_ev[i].tid = (long)syscall(SYS_gettid);
	ft_ev[i].arg = arg;
}

//! From the vblank thread, once a tick.
static void ft_tick(uint64_t t)
{
	if (ft_on == 0 && t % 1000000000ull < 14000000ull && unlink("/tmp/quest1-frametrace") == 0) {
		ft_n = 0;
		ft_until = t + 3000000000ull;
		ft_on = 1;
	} else if (ft_on == 1 && t > ft_until) {
		ft_on = 2;
		FILE *f = fopen("/tmp/quest1-frametrace.txt.tmp", "w");
		unsigned n = ft_n < FT_MAX ? ft_n : FT_MAX;
		for (unsigned i = 0; f && i < n; i++)
			fprintf(f, "%llu %s %ld %lld\n", (unsigned long long)ft_ev[i].t, ft_ev[i].what, ft_ev[i].tid,
			        (long long)ft_ev[i].arg);
		if (f) {
			fclose(f);
			rename("/tmp/quest1-frametrace.txt.tmp", "/tmp/quest1-frametrace.txt");
		}
		ft_on = 0;
	}
}
