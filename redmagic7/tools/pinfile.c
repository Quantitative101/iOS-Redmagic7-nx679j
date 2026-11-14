/*
 * pinfile: make (or inspect) a pinned, preallocated file on Android's f2fs and print where its blocks are.
 *
 *   pinfile create PATH GIB   new file: F2FS_IOC_SET_PIN_FILE, then fallocate GIB GiB, fsync
 *   pinfile map PATH          print the file's extents (FS_IOC_FIEMAP): logical, physical (bytes in the
 *                             filesystem's block device, which on this phone maps 1:1 onto the userdata partition)
 *
 * A pinned f2fs file is never moved by the cleaner, so its blocks stay where the iOS kernel expects them.
 * Freestanding aarch64 Linux binary (raw syscalls, no libc): there is no NDK on this Mac.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef long s64;

#define SYS_openat 56
#define SYS_close 57
#define SYS_ioctl 29
#define SYS_fallocate 47
#define SYS_fsync 82
#define SYS_write 64
#define SYS_exit 93
#define AT_FDCWD -100
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200

#define F2FS_IOC_SET_PIN_FILE 0x4004f50dUL   /* _IOW(0xf5, 13, __u32) */
#define F2FS_IOC_GET_PIN_FILE 0x8004f50eUL   /* _IOR(0xf5, 14, __u32) */
#define FS_IOC_FIEMAP 0xc020660bUL            /* _IOWR('f', 11, struct fiemap) */
#define FIEMAP_FLAG_SYNC 1

static s64
sys(s64 n, s64 a, s64 b, s64 c, s64 d)
{
	register s64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
	    x3 __asm__("x3") = d;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static void
out(const char *s)
{
	u64 n = 0;
	while (s[n])
		n++;
	sys(SYS_write, 1, (s64)s, (s64)n, 0);
}

static void
outn(const char *label, u64 v)
{
	char buf[32];
	int i = 31;
	buf[i] = 0;
	if (!v)
		buf[--i] = '0';
	while (v) {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	}
	out(label);
	out(buf + i);
}

static void
die(const char *why, s64 rc)
{
	out("pinfile: ");
	out(why);
	outn(" (errno ", (u64)-rc);
	out(")\n");
	sys(SYS_exit, 1, 0, 0, 0);
}

static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static u64
num(const char *s)
{
	u64 v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (u64)(*s++ - '0');
	return v;
}

struct fiemap_extent {
	u64 fe_logical, fe_physical, fe_length, fe_reserved64[2];
	u32 fe_flags, fe_reserved[3];
};
struct fiemap {
	u64 fm_start, fm_length;
	u32 fm_flags, fm_mapped_extents, fm_extent_count, fm_reserved;
	struct fiemap_extent fm_extents[64];
};

static void
map(s64 fd)
{
	static struct fiemap fm;
	u64 start = 0, total = 0;
	unsigned n = 0;
	for (;;) {
		fm.fm_start = start;
		fm.fm_length = ~0UL - start;
		fm.fm_flags = FIEMAP_FLAG_SYNC;
		fm.fm_extent_count = 64;
		fm.fm_mapped_extents = 0;
		s64 rc = sys(SYS_ioctl, fd, (s64)FS_IOC_FIEMAP, (s64)&fm, 0);
		if (rc < 0)
			die("FIEMAP", rc);
		if (!fm.fm_mapped_extents)
			break;
		for (u32 i = 0; i < fm.fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm.fm_extents[i];
			outn("extent ", n++);
			outn(" logical ", e->fe_logical);
			outn(" physical ", e->fe_physical);
			outn(" length ", e->fe_length);
			outn(" flags ", e->fe_flags);
			out("\n");
			total += e->fe_length;
			start = e->fe_logical + e->fe_length;
			if (e->fe_flags & 1) {        /* FIEMAP_EXTENT_LAST */
				outn("extents ", n);
				outn(" bytes ", total);
				out("\n");
				return;
			}
		}
	}
	outn("extents ", n);
	outn(" bytes ", total);
	out("\n");
}

void
main_c(long argc, char **argv)
{
	if (argc >= 4 && streq(argv[1], "create")) {
		s64 fd = sys(SYS_openat, AT_FDCWD, (s64)argv[2], O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
			die("create", fd);
		u32 pin = 1;
		s64 rc = sys(SYS_ioctl, fd, (s64)F2FS_IOC_SET_PIN_FILE, (s64)&pin, 0);
		if (rc < 0)
			die("pin", rc);
		u64 bytes = num(argv[3]) << 30;
		rc = sys(SYS_fallocate, fd, 0, 0, (s64)bytes);
		if (rc < 0)
			die("fallocate", rc);
		sys(SYS_fsync, fd, 0, 0, 0);
		outn("created, GiB ", num(argv[3]));
		out("\n");
		map(fd);
		sys(SYS_close, fd, 0, 0, 0);
	} else if (argc >= 3 && streq(argv[1], "map")) {
		s64 fd = sys(SYS_openat, AT_FDCWD, (s64)argv[2], O_RDWR, 0);
		if (fd < 0)
			die("open", fd);
		u32 pinned = 0;
		s64 rc = sys(SYS_ioctl, fd, (s64)F2FS_IOC_GET_PIN_FILE, (s64)&pinned, 0);
		outn("pinned ", rc < 0 ? 0 : 1);
		outn(" (pin count ", pinned);
		out(")\n");
		map(fd);
	} else {
		out("usage: pinfile create PATH GIB | pinfile map PATH\n");
	}
	sys(SYS_exit, 0, 0, 0, 0);
}

__asm__(".global _start\n_start:\n ldr x0, [sp]\n add x1, sp, #8\n and sp, x1, #~15\n bl main_c\n");
