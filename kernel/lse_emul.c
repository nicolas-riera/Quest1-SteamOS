/*
 * Emulate ARMv8.1 LSE atomics (and ARMv8.3 LDAPR) for EL0 on ARMv8.0 CPUs.
 *
 * The Kryo 280 (MSM8998) is ARMv8.0, while Valve's arm64 Steam client is built
 * for ARMv8.1+ and uses LSE atomics unconditionally. Those encodings are
 * unallocated on ARMv8.0 and trap as undefined instructions; this hook performs
 * the same operation with exclusive load/store (ldaxr/stlxr) on the user
 * address and skips the instruction. Every variant is given acquire+release
 * semantics, which is at least as strong as the original.
 *
 * Trapped instructions are counted in /sys/module/lse_emul/parameters/count.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <asm/system_misc.h>
#include <asm/traps.h>
#include <asm/uaccess.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lse_emul."

static unsigned long count;
module_param(count, ulong, 0444);

/*
 * One compare-and-swap on user memory: if *p == old, store new. *cur gets the
 * value seen. Returns 0 or -EFAULT. Retries only when the exclusive store fails.
 */
#define USER_CMPXCHG(name, sfx, w)						\
static int name(unsigned long addr, u64 old, u64 new, u64 *cur)		\
{										\
	u64 val = 0;								\
	unsigned int tmp;							\
	int ret = 0;								\
										\
	uaccess_enable();							\
	asm volatile(								\
	"1:	ldaxr" sfx "	%" w "[val], [%[addr]]\n"			\
	"	cmp	%" w "[val], %" w "[old]\n"				\
	"	b.ne	3f\n"							\
	"2:	stlxr" sfx "	%w[tmp], %" w "[new], [%[addr]]\n"		\
	"	cbnz	%w[tmp], 1b\n"						\
	"3:\n"									\
	"	.pushsection .fixup,\"ax\"\n"					\
	"	.align	2\n"							\
	"4:	mov	%w[ret], %w[efault]\n"					\
	"	b	3b\n"							\
	"	.popsection\n"							\
	_ASM_EXTABLE(1b, 4b)							\
	_ASM_EXTABLE(2b, 4b)							\
	: [val] "=&r" (val), [tmp] "=&r" (tmp), [ret] "+r" (ret)		\
	: [addr] "r" (addr), [old] "r" (old), [new] "r" (new),			\
	  [efault] "i" (-EFAULT)						\
	: "cc", "memory");							\
	uaccess_disable();							\
	*cur = val;								\
	return ret;								\
}

USER_CMPXCHG(user_cmpxchg8, "b", "w")
USER_CMPXCHG(user_cmpxchg16, "h", "w")
USER_CMPXCHG(user_cmpxchg32, "", "w")
USER_CMPXCHG(user_cmpxchg64, "", "x")

/* 128-bit compare-and-swap for CASP with 64-bit registers. */
static int user_cmpxchg128(unsigned long addr, u64 old_lo, u64 old_hi, u64 new_lo, u64 new_hi, u64 *cur_lo,
			   u64 *cur_hi)
{
	u64 lo = 0, hi = 0;
	unsigned int tmp;
	int ret = 0;

	uaccess_enable();
	asm volatile(
	"1:	ldaxp	%[lo], %[hi], [%[addr]]\n"
	"	cmp	%[lo], %[olo]\n"
	"	ccmp	%[hi], %[ohi], #0, eq\n"
	"	b.ne	3f\n"
	"2:	stlxp	%w[tmp], %[nlo], %[nhi], [%[addr]]\n"
	"	cbnz	%w[tmp], 1b\n"
	"3:\n"
	"	.pushsection .fixup,\"ax\"\n"
	"	.align	2\n"
	"4:	mov	%w[ret], %w[efault]\n"
	"	b	3b\n"
	"	.popsection\n"
	_ASM_EXTABLE(1b, 4b)
	_ASM_EXTABLE(2b, 4b)
	: [lo] "=&r" (lo), [hi] "=&r" (hi), [tmp] "=&r" (tmp), [ret] "+r" (ret)
	: [addr] "r" (addr), [olo] "r" (old_lo), [ohi] "r" (old_hi), [nlo] "r" (new_lo), [nhi] "r" (new_hi),
	  [efault] "i" (-EFAULT)
	: "cc", "memory");
	uaccess_disable();
	*cur_lo = lo;
	*cur_hi = hi;
	return ret;
}

static int user_cmpxchg(unsigned int size, unsigned long addr, u64 old, u64 new, u64 *cur)
{
	switch (size) {
	case 0: return user_cmpxchg8(addr, old, new, cur);
	case 1: return user_cmpxchg16(addr, old, new, cur);
	case 2: return user_cmpxchg32(addr, old, new, cur);
	default: return user_cmpxchg64(addr, old, new, cur);
	}
}

static inline u64 size_mask(unsigned int size)
{
	return size == 3 ? ~0ULL : (1ULL << (8 << size)) - 1;
}

static inline s64 sign_extend(u64 v, unsigned int size)
{
	unsigned int shift = 64 - (8 << size);

	return (s64)(v << shift) >> shift;
}

static inline u64 get_reg(struct pt_regs *regs, unsigned int n)
{
	return n == 31 ? 0 : regs->regs[n]; /* register 31 is XZR here */
}

static inline void set_reg(struct pt_regs *regs, unsigned int n, u64 v)
{
	if (n != 31)
		regs->regs[n] = v;
}

static inline unsigned long base_addr(struct pt_regs *regs, unsigned int n)
{
	return n == 31 ? regs->sp : regs->regs[n]; /* register 31 is SP as a base */
}

static int fault(struct pt_regs *regs, unsigned long addr, int bus)
{
	siginfo_t info;

	info.si_signo = bus ? SIGBUS : SIGSEGV;
	info.si_errno = 0;
	info.si_code = bus ? BUS_ADRALN : SEGV_ACCERR;
	info.si_addr = (void __user *)addr;
	arm64_notify_die("LSE emulation: bad user access", regs, &info, 0);
	return 0;
}

static u64 alu(unsigned int o3opc, u64 old, u64 v, unsigned int size)
{
	switch (o3opc) {
	case 0x0: return old + v;			/* LDADD */
	case 0x1: return old & ~v;			/* LDCLR */
	case 0x2: return old ^ v;			/* LDEOR */
	case 0x3: return old | v;			/* LDSET */
	case 0x4: return sign_extend(old, size) > sign_extend(v, size) ? old : v;	/* LDSMAX */
	case 0x5: return sign_extend(old, size) < sign_extend(v, size) ? old : v;	/* LDSMIN */
	case 0x6: return old > v ? old : v;		/* LDUMAX */
	case 0x7: return old < v ? old : v;		/* LDUMIN */
	default:  return v;				/* SWP (o3=1, opc=000) */
	}
}

/* LD<op>, ST<op> (Rt = XZR), SWP and LDAPR: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt */
static int emulate_atomic(struct pt_regs *regs, u32 insn)
{
	unsigned int size = insn >> 30, rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
	unsigned int o3opc = (insn >> 12) & 15;
	unsigned long addr = base_addr(regs, rn);
	u64 mask = size_mask(size), v = get_reg(regs, rs) & mask, guess = 0, cur;

	if (o3opc > 0x8 && o3opc != 0xc)
		return 1;			/* unallocated */
	if (addr & ((1UL << size) - 1))
		return fault(regs, addr, 1);

	if (o3opc == 0xc) {			/* LDAPR (ARMv8.3): a load-acquire */
		u8 b;
		u16 h;
		u32 w;
		int err;

		if (rs != 31)
			return 1;
		/* an aligned single load is single-copy atomic; the barrier gives acquire */
		switch (size) {
		case 0: err = get_user(b, (u8 __user *)addr); cur = b; break;
		case 1: err = get_user(h, (u16 __user *)addr); cur = h; break;
		case 2: err = get_user(w, (u32 __user *)addr); cur = w; break;
		default: err = get_user(cur, (u64 __user *)addr); break;
		}
		if (err)
			return fault(regs, addr, 0);
		smp_mb();
		set_reg(regs, rt, cur);
	} else {
		for (;;) {
			if (user_cmpxchg(size, addr, guess, alu(o3opc, guess, v, size) & mask, &cur))
				return fault(regs, addr, 0);
			if (cur == guess)
				break;
			guess = cur;
		}
		set_reg(regs, rt, cur);
	}
	count++;
	regs->pc += 4;
	return 0;
}

/* CAS{A,L,AL}{B,H}: size 001000 1 L 1 Rs o0 11111 Rn Rt */
static int emulate_cas(struct pt_regs *regs, u32 insn)
{
	unsigned int size = insn >> 30, rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
	unsigned long addr = base_addr(regs, rn);
	u64 mask = size_mask(size), cur;

	if (addr & ((1UL << size) - 1))
		return fault(regs, addr, 1);
	if (user_cmpxchg(size, addr, get_reg(regs, rs) & mask, get_reg(regs, rt) & mask, &cur))
		return fault(regs, addr, 0);
	set_reg(regs, rs, cur);
	count++;
	regs->pc += 4;
	return 0;
}

/* CASP{A,L,AL}: 0 sz 001000 0 L 1 Rs o0 11111 Rn Rt, Rs and Rt even (pairs) */
static int emulate_casp(struct pt_regs *regs, u32 insn)
{
	unsigned int sz = (insn >> 30) & 1, rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
	unsigned long addr = base_addr(regs, rn);
	u64 cur, hi;

	if ((rs & 1) || (rt & 1))
		return 1;
	if (addr & (sz ? 15 : 7))
		return fault(regs, addr, 1);
	if (sz) {
		if (user_cmpxchg128(addr, get_reg(regs, rs), get_reg(regs, rs + 1), get_reg(regs, rt),
				    get_reg(regs, rt + 1), &cur, &hi))
			return fault(regs, addr, 0);
		set_reg(regs, rs, cur);
		set_reg(regs, rs + 1, hi);
	} else {
		u64 old = (get_reg(regs, rs) & 0xffffffff) | get_reg(regs, rs + 1) << 32;
		u64 new = (get_reg(regs, rt) & 0xffffffff) | get_reg(regs, rt + 1) << 32;

		if (user_cmpxchg64(addr, old, new, &cur))
			return fault(regs, addr, 0);
		set_reg(regs, rs, cur & 0xffffffff);
		set_reg(regs, rs + 1, cur >> 32);
	}
	count++;
	regs->pc += 4;
	return 0;
}

#define EL0_A64_MASK (PSR_MODE32_BIT | PSR_MODE_MASK)

static struct undef_hook lse_hooks[] = {
	{
		.instr_mask = 0x3f200c00,
		.instr_val = 0x38200000,
		.pstate_mask = EL0_A64_MASK,
		.pstate_val = PSR_MODE_EL0t,
		.fn = emulate_atomic,
	},
	{
		.instr_mask = 0x3fa07c00,
		.instr_val = 0x08a07c00,
		.pstate_mask = EL0_A64_MASK,
		.pstate_val = PSR_MODE_EL0t,
		.fn = emulate_cas,
	},
	{
		.instr_mask = 0xbfa07c00,
		.instr_val = 0x08207c00,
		.pstate_mask = EL0_A64_MASK,
		.pstate_val = PSR_MODE_EL0t,
		.fn = emulate_casp,
	},
};

static int __init lse_emul_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lse_hooks); i++)
		register_undef_hook(&lse_hooks[i]);
	pr_info("lse_emul: emulating ARMv8.1 LSE atomics and LDAPR for user space\n");
	return 0;
}
core_initcall(lse_emul_init);
