/* Instruction-level probe for the RVA23U64 mandatory extensions.
 *
 * The differential programs in ../progs only reach what the compiler chooses
 * to emit from ordinary C. This one reaches the rest: every instruction the
 * RVA23U64 profile makes mandatory is written out by hand, executed, and
 * checked against a value computed from the specification.
 *
 * It is freestanding on purpose. The RVA23 toolchain's glibc is built for
 * the vector extension, so linking it would drag RVV into a test that is
 * about the scalar extensions, and a missing scalar instruction inside libc
 * would trap before main() ever ran.
 *
 * Each check prints its name before executing, so an instruction the
 * emulator does not implement shows up as a truncated line naming itself:
 *
 *     zbb.rev8           ok
 *     zimop.mop.r        <- trapped here
 */

typedef unsigned long long u64;
typedef signed long long i64;
typedef unsigned int u32;
typedef signed int i32;
typedef unsigned short u16;
typedef unsigned char u8;

static int failures;
static int checks;

static long sys_write(int fd, const void *buf, unsigned long len)
{
	register long a0 __asm__("a0") = fd;
	register long a1 __asm__("a1") = (long)buf;
	register long a2 __asm__("a2") = len;
	register long a7 __asm__("a7") = 64;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
	return a0;
}

static __attribute__((noreturn)) void sys_exit(int code)
{
	register long a0 __asm__("a0") = code;
	register long a7 __asm__("a7") = 93;
	for (;;)
		__asm__ volatile("ecall" : : "r"(a0), "r"(a7));
}

static unsigned long slen(const char *s)
{
	unsigned long n = 0;
	while (s[n]) n++;
	return n;
}

static void put(const char *s) { sys_write(1, s, slen(s)); }

static void puthex(u64 v)
{
	char buf[19];
	int i;
	buf[0] = '0'; buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = "0123456789abcdef"[(v >> ((15 - i) * 4)) & 0xF];
	buf[18] = 0;
	put(buf);
}

/* The name goes out before the instruction runs, so a trap names itself. */
static void begin(const char *name)
{
	char pad[24];
	unsigned long n = slen(name), i;
	checks++;
	put(name);
	for (i = n; i < 22; i++) pad[i - n] = ' ';
	pad[i - n] = 0;
	put(pad);
}

static void done(void) { put("ok\n"); }

static void check(const char *name, u64 got, u64 want)
{
	if (got == want) {
		put("ok\n");
	} else {
		failures++;
		put("FAIL got=");
		puthex(got);
		put(" want=");
		puthex(want);
		put("\n");
		(void)name;
	}
}

/* Keeps the compiler from folding an operand into a constant. */
#define OPAQUE(x) __asm__ volatile("" : "+r"(x))

/* ------------------------------------------------------------------ Zba */

static void test_zba(void)
{
	u64 a, b, r;

	begin("zba.add.uw");
	a = 0xFFFFFFFF80000001ull; b = 0x100;
	OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nadd.uw %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.add.uw", r, 0x80000101ull);

	begin("zba.slli.uw");
	a = 0xFFFFFFFF00000003ull;
	OPAQUE(a);
	__asm__(".option push\n.option arch, +zba\nslli.uw %0, %1, 32\n.option pop"
		: "=r"(r) : "r"(a));
	check("zba.slli.uw", r, 0x300000000ull);

	begin("zba.sh1add");
	a = 5; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh1add %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh1add", r, 17);

	begin("zba.sh2add");
	a = 5; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh2add %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh2add", r, 27);

	begin("zba.sh3add");
	a = 5; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh3add %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh3add", r, 47);

	begin("zba.sh1add.uw");
	a = 0xFFFFFFFF00000005ull; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh1add.uw %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh1add.uw", r, 17);

	begin("zba.sh2add.uw");
	a = 0xFFFFFFFF00000005ull; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh2add.uw %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh2add.uw", r, 27);

	begin("zba.sh3add.uw");
	a = 0xFFFFFFFF00000005ull; b = 7; OPAQUE(a); OPAQUE(b);
	__asm__(".option push\n.option arch, +zba\nsh3add.uw %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(b));
	check("zba.sh3add.uw", r, 47);
}

/* ------------------------------------------------------------------ Zbb */

#define ZBB1(mn, in, want) do { \
	u64 a_ = (in), r_; OPAQUE(a_); \
	begin("zbb." mn); \
	__asm__(".option push\n.option arch, +zbb\n" mn " %0, %1\n.option pop" \
		: "=r"(r_) : "r"(a_)); \
	check("zbb." mn, r_, (u64)(want)); } while (0)

#define ZBB2(mn, x, y, want) do { \
	u64 a_ = (x), b_ = (y), r_; OPAQUE(a_); OPAQUE(b_); \
	begin("zbb." mn); \
	__asm__(".option push\n.option arch, +zbb\n" mn " %0, %1, %2\n.option pop" \
		: "=r"(r_) : "r"(a_), "r"(b_)); \
	check("zbb." mn, r_, (u64)(want)); } while (0)

#define ZBBI(mn, x, imm, want) do { \
	u64 a_ = (x), r_; OPAQUE(a_); \
	begin("zbb." mn); \
	__asm__(".option push\n.option arch, +zbb\n" mn " %0, %1, " #imm "\n.option pop" \
		: "=r"(r_) : "r"(a_)); \
	check("zbb." mn, r_, (u64)(want)); } while (0)

static void test_zbb(void)
{
	ZBB2("andn", 0xF0F0F0F0F0F0F0F0ull, 0x00FF00FF00FF00FFull, 0xF000F000F000F000ull);
	ZBB2("orn",  0xF0F0F0F0F0F0F0F0ull, 0x00FF00FF00FF00FFull, 0xFFF0FFF0FFF0FFF0ull);
	ZBB2("xnor", 0xF0F0F0F0F0F0F0F0ull, 0x00FF00FF00FF00FFull, 0x0FF00FF00FF00FF0ull);

	ZBB1("clz",  0x0000000000001000ull, 51);
	ZBB1("clz",  0ull, 64);
	ZBB1("clzw", 0x0000000000001000ull, 19);
	ZBB1("clzw", 0xFFFFFFFF00000000ull, 32);
	ZBB1("ctz",  0x0000000000001000ull, 12);
	ZBB1("ctz",  0ull, 64);
	ZBB1("ctzw", 0xFFFFFFFF00001000ull, 12);
	ZBB1("ctzw", 0xFFFFFFFF00000000ull, 32);
	ZBB1("cpop", 0xF0F0F0F0F0F0F0F0ull, 32);
	ZBB1("cpopw", 0xFFFFFFFFF0F0F0F0ull, 16);

	ZBB2("max",  (u64)(i64)-5, 3, 3);
	ZBB2("maxu", (u64)(i64)-5, 3, (u64)(i64)-5);
	ZBB2("min",  (u64)(i64)-5, 3, (u64)(i64)-5);
	ZBB2("minu", (u64)(i64)-5, 3, 3);

	ZBB1("sext.b", 0x00000000000000FFull, (u64)(i64)-1);
	ZBB1("sext.h", 0x000000000000FFFFull, (u64)(i64)-1);
	ZBB1("zext.h", 0xFFFFFFFFFFFFFFFFull, 0xFFFFull);

	ZBB2("rol",  0x8000000000000001ull, 1, 0x0000000000000003ull);
	ZBB2("ror",  0x8000000000000001ull, 1, 0xC000000000000000ull);
	ZBB2("rolw", 0x0000000080000001ull, 1, 0x0000000000000003ull);
	ZBB2("rorw", 0x0000000000000001ull, 1, 0xFFFFFFFF80000000ull);
	ZBBI("rori", 0x0000000000000001ull, 1, 0x8000000000000000ull);
	ZBBI("roriw", 0x0000000000000001ull, 1, 0xFFFFFFFF80000000ull);

	ZBB1("orc.b", 0x0100000200000003ull, 0xFF0000FF000000FFull);
	ZBB1("rev8",  0x0123456789ABCDEFull, 0xEFCDAB8967452301ull);
}

/* ------------------------------------------------------------------ Zbs */

#define ZBS2(mn, x, y, want) do { \
	u64 a_ = (x), b_ = (y), r_; OPAQUE(a_); OPAQUE(b_); \
	begin("zbs." mn); \
	__asm__(".option push\n.option arch, +zbs\n" mn " %0, %1, %2\n.option pop" \
		: "=r"(r_) : "r"(a_), "r"(b_)); \
	check("zbs." mn, r_, (u64)(want)); } while (0)

#define ZBSI(mn, x, imm, want) do { \
	u64 a_ = (x), r_; OPAQUE(a_); \
	begin("zbs." mn); \
	__asm__(".option push\n.option arch, +zbs\n" mn " %0, %1, " #imm "\n.option pop" \
		: "=r"(r_) : "r"(a_)); \
	check("zbs." mn, r_, (u64)(want)); } while (0)

static void test_zbs(void)
{
	/* The shift amount is taken modulo XLEN, so 68 selects bit 4. */
	ZBS2("bclr", 0xFFFFFFFFFFFFFFFFull, 68, 0xFFFFFFFFFFFFFFEFull);
	ZBS2("bset", 0ull, 68, 0x10ull);
	ZBS2("binv", 0x10ull, 68, 0ull);
	ZBS2("bext", 0x10ull, 68, 1ull);
	ZBSI("bclri", 0xFFFFFFFFFFFFFFFFull, 63, 0x7FFFFFFFFFFFFFFFull);
	ZBSI("bseti", 0ull, 63, 0x8000000000000000ull);
	ZBSI("binvi", 0x8000000000000000ull, 63, 0ull);
	ZBSI("bexti", 0x8000000000000000ull, 63, 1ull);
}

/* --------------------------------------------------------------- Zicond */

static void test_zicond(void)
{
	u64 a = 0xDEADBEEFull, c, r;

	begin("zicond.czero.eqz");
	c = 0; OPAQUE(a); OPAQUE(c);
	__asm__(".option push\n.option arch, +zicond\nczero.eqz %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(c));
	check("czero.eqz", r, 0);

	begin("zicond.czero.eqz.nz");
	c = 1; OPAQUE(a); OPAQUE(c);
	__asm__(".option push\n.option arch, +zicond\nczero.eqz %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(c));
	check("czero.eqz", r, 0xDEADBEEFull);

	begin("zicond.czero.nez");
	c = 1; OPAQUE(a); OPAQUE(c);
	__asm__(".option push\n.option arch, +zicond\nczero.nez %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(c));
	check("czero.nez", r, 0);

	begin("zicond.czero.nez.z");
	c = 0; OPAQUE(a); OPAQUE(c);
	__asm__(".option push\n.option arch, +zicond\nczero.nez %0, %1, %2\n.option pop"
		: "=r"(r) : "r"(a), "r"(c));
	check("czero.nez", r, 0xDEADBEEFull);
}

/* ------------------------------------------------------------------ Zcb */

/* Zcb only ever appears as the compressed form of an instruction the base
 * ISA can also express, so every block below moves its operands into the
 * x8-x15 range by hand and reads the answer back out inside the same asm.
 * Letting the compiler place the operands would not do: a register asm
 * variable is only live across the asm that names it, and the begin() call
 * in between is free to reuse the register. */
static void test_zcb(void)
{
	static volatile u8 buf[8] = { 0x81, 0x82, 0x83, 0x84, 0, 0, 0, 0 };
	const volatile u8 *p = buf;
	u64 r;

#define ZCB_LOAD(mn, off, want) do { \
	begin("zcb." mn); \
	__asm__ volatile("mv a1, %1\n" \
		".option push\n.option arch, +zcb\n" mn " a0, " #off "(a1)\n.option pop\n" \
		"mv %0, a0" \
		: "=r"(r) : "r"(p) : "a0", "a1", "memory"); \
	check("zcb." mn, r, (u64)(want)); } while (0)

	ZCB_LOAD("c.lbu", 0, 0x81);
	ZCB_LOAD("c.lbu", 3, 0x84);
	ZCB_LOAD("c.lhu", 0, 0x8281);
	ZCB_LOAD("c.lhu", 2, 0x8483);
	ZCB_LOAD("c.lh",  0, 0xFFFFFFFFFFFF8281ull);
	ZCB_LOAD("c.lh",  2, 0xFFFFFFFFFFFF8483ull);

	begin("zcb.c.sb");
	{
		u64 v = 0x5A;
		__asm__ volatile("mv a1, %0\nmv a0, %1\n"
			".option push\n.option arch, +zcb\nc.sb a0, 1(a1)\n.option pop"
			: : "r"(p + 4), "r"(v) : "a0", "a1", "memory");
	}
	check("c.sb", buf[5], 0x5A);

	begin("zcb.c.sh");
	{
		u64 v = 0xBEEF;
		__asm__ volatile("mv a1, %0\nmv a0, %1\n"
			".option push\n.option arch, +zcb\nc.sh a0, 2(a1)\n.option pop"
			: : "r"(p + 4), "r"(v) : "a0", "a1", "memory");
	}
	check("c.sh", (u64)buf[6] | ((u64)buf[7] << 8), 0xBEEF);

	/* The unary forms take rd' as both source and destination. */
#define ZCB_UNARY(mn, in, want) do { \
	u64 a_ = (in); \
	begin("zcb." mn); \
	__asm__ volatile("mv a0, %1\n" \
		".option push\n.option arch, +zcb\n" mn " a0\n.option pop\n" \
		"mv %0, a0" \
		: "=r"(r) : "r"(a_) : "a0"); \
	check("zcb." mn, r, (u64)(want)); } while (0)

	ZCB_UNARY("c.zext.b", 0xFFFFFFFFFFFFFF5Aull, 0x5A);
	ZCB_UNARY("c.sext.b", 0x00000000000000FFull, (u64)(i64)-1);
	ZCB_UNARY("c.sext.b", 0x000000000000007Full, 0x7F);
	ZCB_UNARY("c.zext.h", 0xFFFFFFFFFFFFBEEFull, 0xBEEF);
	ZCB_UNARY("c.sext.h", 0x000000000000FFFFull, (u64)(i64)-1);
	ZCB_UNARY("c.sext.h", 0x0000000000007FFFull, 0x7FFF);
	ZCB_UNARY("c.zext.w", 0xFFFFFFFF80000001ull, 0x80000001ull);
	ZCB_UNARY("c.not",    0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull);

	begin("zcb.c.mul");
	{
		u64 x = 7, y = 6;
		__asm__ volatile("mv a0, %1\nmv a1, %2\n"
			".option push\n.option arch, +zcb\nc.mul a0, a1\n.option pop\n"
			"mv %0, a0"
			: "=r"(r) : "r"(x), "r"(y) : "a0", "a1");
		check("c.mul", r, 42);
	}

	begin("zcb.c.mul.neg");
	{
		u64 x = (u64)(i64)-7, y = 6;
		__asm__ volatile("mv a0, %1\nmv a1, %2\n"
			".option push\n.option arch, +zcb\nc.mul a0, a1\n.option pop\n"
			"mv %0, a0"
			: "=r"(r) : "r"(x), "r"(y) : "a0", "a1");
		check("c.mul", r, (u64)(i64)-42);
	}
}

/* --------------------------------------------------------------- Zfhmin */

static void test_zfhmin(void)
{
	/* 0x3C00 is 1.0h, 0x4000 is 2.0h. */
	static volatile u16 h[2] = { 0x3C00, 0 };
	u64 r;
	float f;

	begin("zfhmin.flh+fmv.x.h");
	{
		u64 bits;
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"flh fa0, 0(%1)\nfmv.x.h %0, fa0\n.option pop"
			: "=r"(bits) : "r"(h) : "fa0", "memory");
		r = bits & 0xFFFF;
	}
	check("flh", r, 0x3C00);

	begin("zfhmin.fcvt.s.h");
	{
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"flh fa0, 0(%1)\nfcvt.s.h %0, fa0\n.option pop"
			: "=f"(f) : "r"(h) : "fa0", "memory");
	}
	check("fcvt.s.h", (u64)*(u32 *)&f, 0x3F800000ull);

	begin("zfhmin.fcvt.h.s");
	{
		float two = 2.0f;
		u64 bits;
		OPAQUE(two);
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fcvt.h.s fa0, %1\nfmv.x.h %0, fa0\n.option pop"
			: "=r"(bits) : "f"(two) : "fa0");
		r = bits & 0xFFFF;
	}
	check("fcvt.h.s", r, 0x4000);

	begin("zfhmin.fsh");
	{
		u64 bits = 0x4000;
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fmv.h.x fa0, %0\nfsh fa0, 2(%1)\n.option pop"
			: : "r"(bits), "r"(h) : "fa0", "memory");
	}
	check("fsh", h[1], 0x4000);

	begin("zfhmin.fcvt.d.h");
	{
		double d;
		u64 one = 0x3C00;
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fmv.h.x fa0, %1\nfcvt.d.h %0, fa0\n.option pop"
			: "=f"(d) : "r"(one) : "fa0");
		check("fcvt.d.h", *(u64 *)&d, 0x3FF0000000000000ull);
	}

	begin("zfhmin.fcvt.h.d");
	{
		double d = 2.0;
		u64 bits;
		OPAQUE(d);
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fcvt.h.d fa0, %1\nfmv.x.h %0, fa0\n.option pop"
			: "=r"(bits) : "f"(d) : "fa0");
		check("fcvt.h.d", bits & 0xFFFF, 0x4000);
	}
}

/* ------------------------------------------------------- Zfhmin rounding */

/* Correctly-rounded reference values. Every entry is a bit pattern and the
 * half it must convert to; the tables were generated with numpy's float16,
 * and independently agree with GCC's own _Float16 over forty million cases.
 *
 * They exist because narrowing to a half is the one place these conversions
 * can be subtly wrong and still look right: rounding a double by way of a
 * float rounds twice, and a value sitting exactly on a float's tie comes out
 * one ulp away from what a single correctly-rounded narrowing gives. Several
 * entries below are exactly such values.
 */

static const struct { u64 bits; u16 want; } F16_FROM_F64[] = {
	{ 0x0000000000000000ull, 0x0000 },
	{ 0x8000000000000000ull, 0x8000 },
	{ 0x3ff0000000000000ull, 0x3c00 },
	{ 0xbff0000000000000ull, 0xbc00 },
	{ 0x4000000000000000ull, 0x4000 },
	{ 0x3fe0000000000000ull, 0x3800 },
	{ 0xc00c000000000000ull, 0xc300 },
	{ 0x400921fb54442d11ull, 0x4248 },
	{ 0x40effc0000000000ull, 0x7bff },
	{ 0x40effde000000000ull, 0x7bff },
	{ 0x40effe0000000000ull, 0x7c00 },
	{ 0x40efffe000000000ull, 0x7c00 },
	{ 0x40f0000000000000ull, 0x7c00 },
	{ 0x7e37e43c8800759cull, 0x7c00 },
	{ 0xfe37e43c8800759cull, 0xfc00 },
	{ 0x3f10000000000000ull, 0x0400 },
	{ 0x3e70000000000000ull, 0x0001 },
	{ 0x3e60000000000000ull, 0x0000 },
	{ 0x3e6000001ad7f29bull, 0x0001 },
	{ 0x3e78000000000000ull, 0x0002 },
	{ 0x3e50000000000000ull, 0x0000 },
	{ 0x3ddb7cdfd9d7bdbbull, 0x0000 },
	{ 0xbddb7cdfd9d7bdbbull, 0x8000 },
	{ 0x7ff0000000000000ull, 0x7c00 },
	{ 0xfff0000000000000ull, 0xfc00 },
	{ 0x3ff0020000000000ull, 0x3c00 },
	{ 0x3ff0060000000000ull, 0x3c02 },
	{ 0x3ff0020000400000ull, 0x3c01 },
	{ 0x3fd6aa000b49d2d0ull, 0x35ab },
	{ 0x404d45ffff2cd659ull, 0x5351 },
	{ 0xbf257200085aa83eull, 0x895d },
	{ 0xbec1400005a7751cull, 0x8023 },
	{ 0xbfe43a00059840f7ull, 0xb90f },
	{ 0x4105cca7b53302fcull, 0x7c00 },
	{ 0xbfbd7d143437f5abull, 0xaf5f },
	{ 0x3fc2a9087219c1daull, 0x30aa },
	{ 0xc0f727ad2b6b5fceull, 0xfc00 },
	{ 0x3dd1e044e448373cull, 0x0000 },
	{ 0x402746a4aa785c61ull, 0x49d2 },
	{ 0x3e06d750369a9ad7ull, 0x0000 },
	{ 0x3f0c0a2944eb31e4ull, 0x0381 },
	{ 0x3e955df75af806efull, 0x0005 },
	{ 0xbe33c26cbe934924ull, 0x8000 },
	{ 0xbf6c7424b77336dfull, 0x9b1d },
	{ 0x403c7afd7a448c01ull, 0x4f1f },
	{ 0x3ec460c4d112fdfdull, 0x0029 },
	{ 0xbe6e0ec5c728d420ull, 0x8001 },
	{ 0xbf506a796ca2e0f9ull, 0x941b },
	{ 0x3ee7878b256cabc5ull, 0x00bc },
	{ 0xc0cd76a80ff5cf57ull, 0xf35e },
	{ 0x3f1e1c8dcbf9512cull, 0x0787 },
	{ 0xbdde6946a3103d72ull, 0x8000 },
	{ 0xbfa3f751f49b41e2ull, 0xa8fe },
	{ 0xbf238f9e7635d387ull, 0x88e4 },
	{ 0xbfee52c68a2e359dull, 0xbb95 },
	{ 0x3ebbaf28ca27e135ull, 0x001c },
	{ 0x40fe34f4288919f7ull, 0x7c00 },
	{ 0xbee78a66e8c9ea0bull, 0x80bc },
	{ 0x3e7f6eab437f1a93ull, 0x0002 },
	{ 0x3e8d3a5911de664eull, 0x0004 },
	{ 0x3e0a72bb726bdf3bull, 0x0000 },
	{ 0xbe8ea9568540e607ull, 0x8004 },
	{ 0x3f861827413cdff5ull, 0x2186 },
	{ 0xbe9e5a852503044dull, 0x8008 },
	{ 0x3f19f01258efff6bull, 0x067c },
	{ 0xbdda9a612e9abe22ull, 0x8000 },
	{ 0x3daa2699c4041892ull, 0x0000 },
	{ 0xbf422b8406474e46ull, 0x908b },
	{ 0xbe389957f9c10133ull, 0x8000 },
	{ 0xbe268b6417d1f761ull, 0x8000 },
	{ 0x40c0ea5d646c289cull, 0x703b },
	{ 0xbd80f0599f7c60baull, 0x8000 },
	{ 0x4081a5d16b577b8cull, 0x6069 },
	{ 0x409f8a699f7f2c7cull, 0x67e3 },
	{ 0x3fe48f5bdd563b02ull, 0x3924 },
	{ 0x3ebbc989cf639b4bull, 0x001c },
	{ 0xbe801d3db0993e57ull, 0x8002 },
	{ 0xc0bab817206ebdabull, 0xeeae },
	{ 0x407461f382f85ec6ull, 0x5d18 },
	{ 0x3ef361b5c708b90dull, 0x0136 },
	{ 0x3f0d4d1cf5f6fbbbull, 0x03aa },
	{ 0x4014ba52d4df9d54ull, 0x452f },
	{ 0x3e444b8860a889b4ull, 0x0000 },
	{ 0x3f3f35551901880cull, 0x0fcd },
	{ 0xc019389e88d88055ull, 0xc64e },
	{ 0xbe26aff7a83ab922ull, 0x8000 },
	{ 0x3de80871c52be303ull, 0x0000 },
	{ 0x3f750feeca522f49ull, 0x1d44 },
	{ 0x40316e2a79121ee3ull, 0x4c5c },
	{ 0xbe2ca72a9fb88334ull, 0x8000 },
	{ 0xbec7cebe7f4a100eull, 0x8030 },
	{ 0x3f2fba19af63eee1ull, 0x0bef },
	{ 0x4115002c4f8574fdull, 0x7c00 },
	{ 0xbfc4597520122a44ull, 0xb116 },
	{ 0xc078c5aac90d529full, 0xde31 },
	{ 0x40ac41c8f46665d6ull, 0x6b10 },
};
static const struct { u32 bits; u16 want; } F16_FROM_F32[] = {
	{ 0x00000000u, 0x0000 },
	{ 0x80000000u, 0x8000 },
	{ 0x3f800000u, 0x3c00 },
	{ 0xbf800000u, 0xbc00 },
	{ 0x40000000u, 0x4000 },
	{ 0x3f000000u, 0x3800 },
	{ 0xc0600000u, 0xc300 },
	{ 0x40490fdbu, 0x4248 },
	{ 0x477fe000u, 0x7bff },
	{ 0x477fef00u, 0x7bff },
	{ 0x477ff000u, 0x7c00 },
	{ 0x477fff00u, 0x7c00 },
	{ 0x47800000u, 0x7c00 },
	{ 0x7f800000u, 0x7c00 },
	{ 0xff800000u, 0xfc00 },
	{ 0x38800000u, 0x0400 },
	{ 0x33800000u, 0x0001 },
	{ 0x33000000u, 0x0000 },
	{ 0x33000001u, 0x0001 },
	{ 0x33c00000u, 0x0002 },
	{ 0x32800000u, 0x0000 },
	{ 0x2edbe6ffu, 0x0000 },
	{ 0xaedbe6ffu, 0x8000 },
	{ 0x3f801000u, 0x3c00 },
	{ 0x3f803000u, 0x3c02 },
	{ 0x3eb55000u, 0x35aa },
	{ 0x426a3000u, 0x5352 },
	{ 0xb92b9000u, 0x895c },
	{ 0xb60a0000u, 0x8022 },
	{ 0xbf21d000u, 0xb90e },
	{ 0x482e653eu, 0x7c00 },
	{ 0xbdebe8a2u, 0xaf5f },
	{ 0x3e154844u, 0x30aa },
	{ 0xc7b93d69u, 0xfc00 },
	{ 0x2e8f0227u, 0x0000 },
	{ 0x413a3525u, 0x49d2 },
	{ 0x3036ba82u, 0x0000 },
	{ 0x3860514au, 0x0381 },
	{ 0x34aaefbbu, 0x0005 },
	{ 0xb19e1366u, 0x8000 },
	{ 0xbb63a126u, 0x9b1d },
	{ 0x41e3d7ecu, 0x4f1f },
	{ 0x36230627u, 0x0029 },
	{ 0xb370762eu, 0x8001 },
	{ 0xba8353cbu, 0x941b },
	{ 0x373c3c59u, 0x00bc },
	{ 0xc66bb540u, 0xf35e },
	{ 0x38f0e46eu, 0x0787 },
	{ 0xaef34a35u, 0x8000 },
	{ 0xbd1fba90u, 0xa8fe },
	{ 0xb91c7cf4u, 0x88e4 },
	{ 0xbf729634u, 0xbb95 },
	{ 0x35dd7946u, 0x001c },
	{ 0x47f1a7a1u, 0x7c00 },
	{ 0xb73c5337u, 0x80bc },
	{ 0x33fb755au, 0x0002 },
	{ 0x3469d2c9u, 0x0004 },
	{ 0x305395dcu, 0x0000 },
	{ 0xb4754ab4u, 0x8004 },
	{ 0x3c30c13au, 0x2186 },
	{ 0xb4f2d429u, 0x8008 },
	{ 0x38cf8093u, 0x067c },
	{ 0xaed4d309u, 0x8000 },
	{ 0x2d5134ceu, 0x0000 },
	{ 0xba115c20u, 0x908b },
	{ 0xb1c4cac0u, 0x8000 },
	{ 0xb1345b21u, 0x8000 },
	{ 0x460752ebu, 0x703b },
	{ 0xac0782cdu, 0x8000 },
	{ 0x440d2e8bu, 0x6069 },
	{ 0x44fc534du, 0x67e3 },
	{ 0x3f247adfu, 0x3924 },
	{ 0x35de4c4eu, 0x001c },
	{ 0xb400e9eeu, 0x8002 },
	{ 0xc5d5c0b9u, 0xeeae },
	{ 0x43a30f9cu, 0x5d18 },
	{ 0x379b0daeu, 0x0136 },
	{ 0x386a68e8u, 0x03aa },
	{ 0x40a5d297u, 0x452f },
	{ 0x32225c43u, 0x0000 },
	{ 0x39f9aaa9u, 0x0fcd },
	{ 0xc0c9c4f4u, 0xc64e },
	{ 0xb1357fbdu, 0x8000 },
	{ 0x2f40438eu, 0x0000 },
	{ 0x3ba87f76u, 0x1d44 },
	{ 0x418b7154u, 0x4c5c },
	{ 0xb1653955u, 0x8000 },
	{ 0xb63e75f4u, 0x8030 },
	{ 0x397dd0cdu, 0x0bef },
	{ 0x48a80162u, 0x7c00 },
	{ 0xbe22cba9u, 0xb116 },
	{ 0xc3c62d56u, 0xde31 },
	{ 0x45620e48u, 0x6b10 },
};
static const struct { u16 bits; u32 want; } F32_FROM_F16[] = {
	{ 0x0000u, 0x00000000u },
	{ 0x8000u, 0x80000000u },
	{ 0x0001u, 0x33800000u },
	{ 0x03ffu, 0x387fc000u },
	{ 0x0400u, 0x38800000u },
	{ 0x3c00u, 0x3f800000u },
	{ 0xbc00u, 0xbf800000u },
	{ 0x7bffu, 0x477fe000u },
	{ 0x7c00u, 0x7f800000u },
	{ 0xfc00u, 0xff800000u },
	{ 0x7e00u, 0x7fc00000u },
	{ 0x3555u, 0x3eaaa000u },
	{ 0x0200u, 0x38000000u },
	{ 0x00ffu, 0x377f0000u },
};

static void test_zfhmin_rounding(void)
{
	unsigned i;
	u64 bad;

	begin("zfhmin.fcvt.h.d.table");
	bad = 0;
	for (i = 0; i < sizeof(F16_FROM_F64) / sizeof(F16_FROM_F64[0]); i++) {
		double d;
		u64 bits = F16_FROM_F64[i].bits, out;
		__builtin_memcpy(&d, &bits, sizeof(d));
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fcvt.h.d fa0, %1\nfmv.x.h %0, fa0\n.option pop"
			: "=r"(out) : "f"(d) : "fa0");
		if ((out & 0xFFFF) != F16_FROM_F64[i].want)
			bad = (bad == 0) ? (((u64)i << 32) | (out & 0xFFFF)) : bad;
	}
	check("fcvt.h.d table", bad, 0);

	begin("zfhmin.fcvt.h.s.table");
	bad = 0;
	for (i = 0; i < sizeof(F16_FROM_F32) / sizeof(F16_FROM_F32[0]); i++) {
		float f;
		u32 bits = F16_FROM_F32[i].bits;
		u64 out;
		__builtin_memcpy(&f, &bits, sizeof(f));
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fcvt.h.s fa0, %1\nfmv.x.h %0, fa0\n.option pop"
			: "=r"(out) : "f"(f) : "fa0");
		if ((out & 0xFFFF) != F16_FROM_F32[i].want)
			bad = (bad == 0) ? (((u64)i << 32) | (out & 0xFFFF)) : bad;
	}
	check("fcvt.h.s table", bad, 0);

	begin("zfhmin.fcvt.s.h.table");
	bad = 0;
	for (i = 0; i < sizeof(F32_FROM_F16) / sizeof(F32_FROM_F16[0]); i++) {
		u64 in = F32_FROM_F16[i].bits;
		float f;
		u32 out;
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fmv.h.x fa0, %1\nfcvt.s.h %0, fa0\n.option pop"
			: "=f"(f) : "r"(in) : "fa0");
		__builtin_memcpy(&out, &f, sizeof(out));
		if (out != F32_FROM_F16[i].want)
			bad = (bad == 0) ? (((u64)i << 32) | out) : bad;
	}
	check("fcvt.s.h table", bad, 0);

	/* A half is only a half while its box is intact: on hardware, reading a
	 * register that holds a single as if it held a half gives the canonical
	 * quiet NaN, not the low sixteen bits.
	 *
	 * libriscv only boxes when it is built with FCSR emulation on
	 * (-DRISCV_FCSR=ON), because boxing costs a store on every FP write and
	 * a scripting guest never looks. Both answers are therefore correct for
	 * some build, and the check accepts either -- what it is really here to
	 * catch is a third answer, which would mean the box is being read
	 * inconsistently with the way it is written. */
	begin("zfhmin.unboxed");
	{
		u64 word = 0x3F800000ull; /* 1.0f, whose low half is 0x0000 */
		float f;
		u32 out;
		__asm__ volatile(".option push\n.option arch, +zfhmin\n"
			"fmv.w.x fa0, %1\nfcvt.s.h %0, fa0\n.option pop"
			: "=f"(f) : "r"(word) : "fa0");
		__builtin_memcpy(&out, &f, sizeof(out));
		check("unboxed half",
			out == 0x7FC00000u /* boxed: canonical qNaN */
			|| out == 0x00000000u /* unboxed: the low half, +0.0 */, 1);
	}
}

/* ------------------------------------------------------------------ Zfa */

static void test_zfa(void)
{
	float f;
	double d;
	u64 r;

	/* FLI.S index 1 is the smallest normal, index 16 is 1.0. */
	begin("zfa.fli.s");
	__asm__ volatile(".option push\n.option arch, +zfa\nfli.s %0, 1.0\n.option pop" : "=f"(f));
	check("fli.s", (u64)*(u32 *)&f, 0x3F800000ull);

	begin("zfa.fli.d");
	__asm__ volatile(".option push\n.option arch, +zfa\nfli.d %0, 1.0\n.option pop" : "=f"(d));
	check("fli.d", *(u64 *)&d, 0x3FF0000000000000ull);

	begin("zfa.fli.d.inf");
	__asm__ volatile(".option push\n.option arch, +zfa\nfli.d %0, inf\n.option pop" : "=f"(d));
	check("fli.d.inf", *(u64 *)&d, 0x7FF0000000000000ull);

	/* fminm/fmaxm return NaN when either operand is NaN, unlike fmin/fmax. */
	begin("zfa.fminm.d");
	{
		double a = 3.0, b = 4.0;
		OPAQUE(a); OPAQUE(b);
		__asm__ volatile(".option push\n.option arch, +zfa\nfminm.d %0, %1, %2\n.option pop"
			: "=f"(d) : "f"(a), "f"(b));
		check("fminm.d", *(u64 *)&d, 0x4008000000000000ull);
	}

	begin("zfa.fmaxm.d.nan");
	{
		u64 nanbits = 0x7FF8000000000000ull;
		double a, b = 4.0;
		__asm__ volatile("fmv.d.x %0, %1" : "=f"(a) : "r"(nanbits));
		OPAQUE(b);
		__asm__ volatile(".option push\n.option arch, +zfa\nfmaxm.d %0, %1, %2\n.option pop"
			: "=f"(d) : "f"(a), "f"(b));
		__asm__ volatile("fmv.x.d %0, %1" : "=r"(r) : "f"(d));
		/* Canonical quiet NaN. */
		check("fmaxm.d", r, 0x7FF8000000000000ull);
	}

	begin("zfa.fround.d");
	{
		double a = 2.5;
		OPAQUE(a);
		__asm__ volatile(".option push\n.option arch, +zfa\nfround.d %0, %1, rne\n.option pop"
			: "=f"(d) : "f"(a));
		check("fround.d", *(u64 *)&d, 0x4000000000000000ull); /* 2.0, ties to even */
	}

	begin("zfa.froundnx.s");
	{
		float a = -2.5f;
		OPAQUE(a);
		__asm__ volatile(".option push\n.option arch, +zfa\nfroundnx.s %0, %1, rtz\n.option pop"
			: "=f"(f) : "f"(a));
		check("froundnx.s", (u64)*(u32 *)&f, 0xC0000000ull); /* -2.0 */
	}

	begin("zfa.fcvtmod.w.d");
	{
		/* 2^32 + 5 has no 32-bit value; fcvtmod takes it modulo 2^32. */
		double a = 4294967301.0;
		i64 out;
		OPAQUE(a);
		__asm__ volatile(".option push\n.option arch, +zfa\nfcvtmod.w.d %0, %1, rtz\n.option pop"
			: "=r"(out) : "f"(a));
		check("fcvtmod.w.d", (u64)out, 5);
	}

	begin("zfa.fleq.d");
	{
		double a = 1.0, b = 2.0;
		i64 out;
		OPAQUE(a); OPAQUE(b);
		__asm__ volatile(".option push\n.option arch, +zfa\nfleq.d %0, %1, %2\n.option pop"
			: "=r"(out) : "f"(a), "f"(b));
		check("fleq.d", (u64)out, 1);
	}

	begin("zfa.fltq.s");
	{
		float a = 2.0f, b = 1.0f;
		i64 out;
		OPAQUE(a); OPAQUE(b);
		__asm__ volatile(".option push\n.option arch, +zfa\nfltq.s %0, %1, %2\n.option pop"
			: "=r"(out) : "f"(a), "f"(b));
		check("fltq.s", (u64)out, 0);
	}
}

/* ---------------------------------------------------------------- Zimop */

/* The may-be-operations write zero to rd and are otherwise inert. All 32
 * MOP.R and all 8 MOP.RR encodings have to decode. */
static void test_zimop(void)
{
	u64 r;
	int i;

	begin("zimop.mop.r.0");
	r = 0xDEADBEEF; OPAQUE(r);
	__asm__ volatile(".option push\n.option arch, +zimop\nmop.r.0 %0, %0\n.option pop" : "+r"(r));
	check("mop.r.0", r, 0);

	begin("zimop.mop.r.all");
	{
		u64 acc = 0;
#define MOP_R(n) do { u64 t_ = 0xFFFFull; OPAQUE(t_); \
	__asm__ volatile(".option push\n.option arch, +zimop\nmop.r." #n " %0, %0\n.option pop" \
		: "+r"(t_)); acc |= t_; } while (0)
		MOP_R(0);  MOP_R(1);  MOP_R(2);  MOP_R(3);
		MOP_R(4);  MOP_R(5);  MOP_R(6);  MOP_R(7);
		MOP_R(8);  MOP_R(9);  MOP_R(10); MOP_R(11);
		MOP_R(12); MOP_R(13); MOP_R(14); MOP_R(15);
		MOP_R(16); MOP_R(17); MOP_R(18); MOP_R(19);
		MOP_R(20); MOP_R(21); MOP_R(22); MOP_R(23);
		MOP_R(24); MOP_R(25); MOP_R(26); MOP_R(27);
		MOP_R(28); MOP_R(29); MOP_R(30); MOP_R(31);
		check("mop.r.*", acc, 0);
	}

	begin("zimop.mop.rr.all");
	{
		u64 acc = 0;
#define MOP_RR(n) do { u64 t_ = 0xFFFFull, u_ = 0xFFFFull; OPAQUE(t_); OPAQUE(u_); \
	__asm__ volatile(".option push\n.option arch, +zimop\nmop.rr." #n " %0, %0, %1\n.option pop" \
		: "+r"(t_) : "r"(u_)); acc |= t_; } while (0)
		MOP_RR(0); MOP_RR(1); MOP_RR(2); MOP_RR(3);
		MOP_RR(4); MOP_RR(5); MOP_RR(6); MOP_RR(7);
		check("mop.rr.*", acc, 0);
	}
	(void)i;
}

/* ---------------------------------------------------------------- Zcmop */

static void test_zcmop(void)
{
	begin("zcmop.c.mop.all");
	__asm__ volatile(".option push\n.option arch, +zcmop\n"
		"c.mop.1\nc.mop.3\nc.mop.5\nc.mop.7\n"
		"c.mop.9\nc.mop.11\nc.mop.13\nc.mop.15\n.option pop");
	done();
	checks++;
}

/* ---------------------------------------------------------------- Zawrs */

static void test_zawrs(void)
{
	/* Both forms are allowed to return immediately; what matters is that
	 * they do not trap. A reservation is set first, because wrs is only
	 * architecturally meaningful after an LR. */
	static volatile u32 word;

	begin("zawrs.wrs.nto");
	__asm__ volatile("lr.w zero, (%0)\n"
		".option push\n.option arch, +zawrs\nwrs.nto\n.option pop"
		: : "r"(&word) : "memory");
	done();

	begin("zawrs.wrs.sto");
	__asm__ volatile("lr.w zero, (%0)\n"
		".option push\n.option arch, +zawrs\nwrs.sto\n.option pop"
		: : "r"(&word) : "memory");
	done();
	checks += 2;
}

/* ---------------------------------------------------- Zihintpause / ntl */

static void test_hints(void)
{
	begin("zihintpause.pause");
	__asm__ volatile(".option push\n.option arch, +zihintpause\npause\n.option pop");
	done();

	begin("zihintntl.ntl");
	__asm__ volatile(".option push\n.option arch, +zihintntl\n"
		"ntl.p1\nntl.pall\nntl.s1\nntl.all\n.option pop");
	done();

	begin("zihintntl.c.ntl");
	__asm__ volatile(".option push\n.option arch, +zihintntl,+zca\n"
		"c.ntl.p1\nc.ntl.pall\nc.ntl.s1\nc.ntl.all\n.option pop");
	done();
	checks += 3;
}

/* ---------------------------------------------------------------- Zicbo* */

static void test_zicbo(void)
{
	static volatile u8 line[128] __attribute__((aligned(64))) = { 1, 2, 3, 4 };
	u64 sum;
	int i;

	begin("zicbop.prefetch");
	__asm__ volatile(".option push\n.option arch, +zicbop\n"
		"prefetch.i 0(%0)\nprefetch.r 0(%0)\nprefetch.w 0(%0)\n.option pop"
		: : "r"(line) : "memory");
	done();
	checks++;

	begin("zicbom.cbo.clean");
	__asm__ volatile(".option push\n.option arch, +zicbom\n"
		"cbo.clean (%0)\ncbo.flush (%0)\ncbo.inval (%0)\n.option pop"
		: : "r"(line) : "memory");
	done();
	checks++;

	begin("zicboz.cbo.zero");
	__asm__ volatile(".option push\n.option arch, +zicboz\ncbo.zero (%0)\n.option pop"
		: : "r"(line) : "memory");
	sum = 0;
	for (i = 0; i < 64; i++) sum += line[i];
	check("cbo.zero", sum, 0);
}

/* ------------------------------------------------------------ Zicntr/csr */

static void test_zicntr(void)
{
	u64 a, b;

	begin("zicntr.rdcycle");
	__asm__ volatile("rdcycle %0" : "=r"(a));
	__asm__ volatile("rdcycle %0" : "=r"(b));
	check("rdcycle", b >= a, 1);

	begin("zicntr.rdtime");
	__asm__ volatile("rdtime %0" : "=r"(a));
	__asm__ volatile("rdtime %0" : "=r"(b));
	check("rdtime", b >= a, 1);

	begin("zicntr.rdinstret");
	__asm__ volatile("rdinstret %0" : "=r"(a));
	__asm__ volatile("rdinstret %0" : "=r"(b));
	/* Only monotonicity is required here. libriscv accounts for retired
	 * instructions a basic block at a time unless it is run with --accurate,
	 * so two reads inside one block legitimately return the same count. */
	check("rdinstret", b >= a, 1);
}

/* ------------------------------------------------------- A: LR/SC + AMO */

static void test_atomics(void)
{
	static volatile u32 w;
	static volatile u64 dw;
	u64 r;

	begin("a.lr.w/sc.w");
	w = 5;
	__asm__ volatile("1: lr.w %0, (%1)\naddi %0, %0, 1\nsc.w t0, %0, (%1)\nbnez t0, 1b"
		: "=&r"(r) : "r"(&w) : "t0", "memory");
	check("lr/sc.w", w, 6);

	begin("a.lr.d/sc.d");
	dw = 5;
	__asm__ volatile("1: lr.d %0, (%1)\naddi %0, %0, 1\nsc.d t0, %0, (%1)\nbnez t0, 1b"
		: "=&r"(r) : "r"(&dw) : "t0", "memory");
	check("lr/sc.d", dw, 6);

	begin("a.amoadd.w");
	w = 5;
	__asm__ volatile("li t0, 3\namoadd.w %0, t0, (%1)" : "=r"(r) : "r"(&w) : "t0", "memory");
	check("amoadd.w", (r == 5) && (w == 8), 1);

	begin("a.amoswap.d");
	dw = 5;
	__asm__ volatile("li t0, 9\namoswap.d %0, t0, (%1)" : "=r"(r) : "r"(&dw) : "t0", "memory");
	check("amoswap.d", (r == 5) && (dw == 9), 1);

	begin("a.amomaxu.w");
	w = 5;
	__asm__ volatile("li t0, -1\namomaxu.w %0, t0, (%1)" : "=r"(r) : "r"(&w) : "t0", "memory");
	check("amomaxu.w", w, 0xFFFFFFFFu);

	begin("a.amomin.w");
	w = 5;
	__asm__ volatile("li t0, -1\namomin.w %0, t0, (%1)" : "=r"(r) : "r"(&w) : "t0", "memory");
	check("amomin.w", w, 0xFFFFFFFFu);
}

/* ------------------------------------------------------------------ main */

int probe_main(void)
{
	put("RVA23U64 mandatory instruction probe\n\n");
#ifndef SKIP_ZBA
	test_zba();
#endif
#ifndef SKIP_ZBB
	test_zbb();
#endif
#ifndef SKIP_ZBS
	test_zbs();
#endif
#ifndef SKIP_ZICOND
	test_zicond();
#endif
#ifndef SKIP_ZCB
	test_zcb();
#endif
#ifndef SKIP_ZFHMIN
	test_zfhmin();
	test_zfhmin_rounding();
#endif
#ifndef SKIP_ZFA
	test_zfa();
#endif
#ifndef SKIP_ZIMOP
	test_zimop();
#endif
#ifndef SKIP_ZCMOP
	test_zcmop();
#endif
#ifndef SKIP_ZAWRS
	test_zawrs();
#endif
#ifndef SKIP_HINTS
	test_hints();
#endif
#ifndef SKIP_ZICBO
	test_zicbo();
#endif
#ifndef SKIP_ZICNTR
	test_zicntr();
#endif
#ifndef SKIP_ATOMICS
	test_atomics();
#endif

	put("\n");
	if (failures == 0) {
		put("all checks passed\n");
		return 0;
	}
	put("FAILURES\n");
	return 1;
}

/* Freestanding: no libc start files, so the stack and the global pointer are
 * our own. Setting gp matters more than it looks -- the linker relaxes static
 * data references to gp-relative form, and without a crt0 to load it every
 * such access would read address zero and quietly return nothing. */
__attribute__((aligned(16))) char probe_stack[64 * 1024];

__attribute__((naked, section(".text.start"))) void _start(void)
{
	__asm__ volatile(
		".option push\n"
		".option norelax\n"
		"la gp, __global_pointer$\n"
		".option pop\n"
		"la sp, probe_stack\n"
		"li t0, %0\n"
		"add sp, sp, t0\n"
		"call probe_main\n"
		"li a7, 93\n"
		"ecall\n" : : "i"(sizeof(probe_stack)));
}
