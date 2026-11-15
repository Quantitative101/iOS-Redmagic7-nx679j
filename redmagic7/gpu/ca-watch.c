/*
 * RedMagic 7: ca-watch, what CoreAnimation's window server decides each frame, for the Metal bring-up.
 *
 * Installed as /usr/libexec/mondrian-inject.dylib, the path ios-mondrian's UI hook dlopens in every daemon it lands in;
 * does nothing outside backboardd. There it replaces virtual functions of CA::WindowServer in the dyld shared cache of
 * THIS build (iOS 16.7.16, 20H392, iPhone10,3: addresses from `ipsw dyld symaddr ... --image QuartzCore`), found by
 * value in every WindowServer vtable (no slot offsets or concrete display class assumed), and counts the calls and
 * the answers. After the boot logo's fade the server stops drawing (redmagic7/gpu/METAL-PORT.md, v31-v39); this says
 * which decision stops it.
 *
 * v40: v38/v39 replaced six slots of AppleCLCDDisplay/EAGLServer and printed nothing after: the lines went to
 * /dev/console from backboardd, which the ring does not see, while the plug-in's stderr lines arrive. Now stderr, one
 * write a line; every WindowServer vtable; and the display's power, idle and readiness calls as well.
 * The technique is HackiOS's (userland/src/inject.c: replace_virtual), MIT.
 * SPDX-License-Identifier: MIT
 */
#include <mach-o/dyld.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>   /* v195: the names check reads launchd.plist */

extern const char *getprogname(void);
extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;

static void
say(const char *fmt, ...)
{
	char line[600];
	va_list ap;
	int n = snprintf(line, sizeof(line), "RM CAWATCH: ");

	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
	write(2, line, (size_t)n);
}

/* 20H392 QuartzCore: every CA::WindowServer vtable */
static const uintptr_t vtables[] = {
	0x1d5b29c18, 0x1d5b29df0, 0x1d5b2a5c0, 0x1d5b2ad90, 0x1d5b2af00, 0x1d5b2afa8, 0x1d5b2b090, 0x1d5b2b268,
	0x1d5b2b9e0, 0x1d5b2c200, 0x1d5b2c3d8, 0x1d5b2cb40, 0x1d5b2cbe8, 0x1d5b37f58, 0x1d5b38788, 0x1d5b39ed0,
};

typedef uintptr_t (*fn6)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct watched {
	const char *name;
	uintptr_t address;           /* unslid */
	int boolean;                 /* answer is a bool: report its changes */
	fn6 original;
	unsigned slots;
	volatile unsigned calls, nonzero;
	volatile uintptr_t last, first_arg1;
};

static struct watched w[] = {
	{ "IOMFBDisplay::is_ready", 0x1882f9318, 1 },
	{ "IOMFBDisplay::can_update", 0x188325804, 1 },
	{ "IOMFBDisplay::ignore_update_p", 0x1882e9684, 1 },
	{ "IOMFBDisplay::begin_update", 0x1882e6798, 0 },
	{ "IOMFBDisplay::finish_update", 0x1882d0f88, 0 },
	{ "IOMFBDisplay::idle", 0x188394534, 1 },
	{ "IOMFBDisplay::will_unblank", 0x188383460, 0 },
	{ "IOMFBDisplay::set_power_state", 0x1883966c4, 0 },
	{ "IOMFBDisplay::framebuffer_available", 0x18853c980, 1 },
	{ "Display::is_ready", 0x1883bc310, 1 },
	{ "Display::idle", 0x1883a8938, 1 },
	{ "Display::will_unblank", 0x1883bc33c, 0 },
	{ "Display::ignore_update_p", 0x1883bc384, 1 },
	{ "Display::begin_update", 0x18841b4a8, 0 },
	{ "Display::finish_update", 0x18841b4ac, 0 },
	{ "EAGLServer::render_update", 0x1883601dc, 0 },
	{ "Server::render_update", 0x1884f4b58, 0 },
	{ "EAGLServer::renderer", 0x1882eaf3c, 0 },
};
#define NW (sizeof(w) / sizeof(w[0]))

static uintptr_t
hook(unsigned i, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	struct watched *x = &w[i];
	uintptr_t r = x->original(a0, a1, a2, a3, a4, a5);
	unsigned n = ++x->calls;
	if (x->boolean) r &= 0xff;
	x->nonzero += r != 0;
	if (n <= 3 || (x->boolean && r != x->last) || (n <= 50 && !x->boolean && r == 0 && x->last != 0))
		say("%s(this %#lx, %#lx) -> %#lx (call %u)\n", x->name, (unsigned long)a0, (unsigned long)a1, (unsigned long)r, n);
	if (n == 1) x->first_arg1 = a1;
	x->last = r;
	return r;
}

#define H(i) static uintptr_t h##i(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) \
	{ return hook(i, a0, a1, a2, a3, a4, a5); }
H(0) H(1) H(2) H(3) H(4) H(5) H(6) H(7) H(8) H(9) H(10) H(11) H(12) H(13) H(14) H(15) H(16) H(17)
static const fn6 hooks[] = { h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11, h12, h13, h14, h15, h16, h17 };
_Static_assert(sizeof(hooks) / sizeof(hooks[0]) == NW, "one hook a watched function");

static int
patch(uintptr_t *entry, uintptr_t value)
{
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
		return -1;
	*entry = value;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return 0;
}

static void
install(void)
{
	for (unsigned v = 0; v < sizeof(vtables) / sizeof(vtables[0]); v++) {
		uintptr_t *table = (uintptr_t *)(vtables[v] + slide);
		for (unsigned s = 0; s < 200; s++) {
			for (unsigned i = 0; i < NW; i++) {
				if (table[s] != w[i].address + slide) continue;
				if (patch(&table[s], (uintptr_t)hooks[i]) != 0) {
					say("vtable %#lx slot %u (%s): not writable\n", (unsigned long)vtables[v], s, w[i].name);
					break;
				}
				w[i].original = (fn6)(w[i].address + slide);
				w[i].slots++;
				break;
			}
		}
	}
	char line[500];
	int n = 0;
	for (unsigned i = 0; i < NW; i++)
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s:%u", w[i].name, w[i].slots);
	say("slots replaced:%s\n", line);
}

/*
 * v43: every thread of backboardd, its name and its frame-pointer chain, at t+40 s and t+70 s. On v41 the Metal window
 * server stopped after 10 frames with one command buffer made and never committed; software kept drawing 8 frames a
 * second from t+28 s. Addresses inside the shared cache are printed unslid (symbolicate with ipsw dyld a2s).
 */
static unsigned long
unslide(uintptr_t a)
{
	size_t length = 0;
	uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
	return (a >= base && a < base + length) ? (unsigned long)(a - slide) : (unsigned long)a;
}

static void
dump_threads(int when)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	thread_t self = mach_thread_self();

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
	say("threads at t+%ds: %u\n", when, count);
	for (mach_msg_type_number_t t = 0; t < count; t++) {
		char name[64] = "";
		uintptr_t frames[20];
		unsigned n = 0;

		if (threads[t] == self) continue;
		pthread_t p = pthread_from_mach_thread_np(threads[t]);
		if (p) pthread_getname_np(p, name, sizeof(name));
		if (thread_suspend(threads[t]) == KERN_SUCCESS) {
			arm_thread_state64_t st;
			mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
			if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
				frames[n++] = (uintptr_t)st.__pc;
				frames[n++] = (uintptr_t)st.__lr;
				uintptr_t fp = (uintptr_t)st.__fp;
				while (fp && (fp & 7) == 0 && n < 20) {
					vm_size_t got = 0;
					uintptr_t pair[2];
					if (vm_read_overwrite(mach_task_self(), fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS)
						break;
					if (pair[1] == 0) break;
					frames[n++] = pair[1];
					if (pair[0] <= fp) break;
					fp = pair[0];
				}
			}
			thread_resume(threads[t]);
		}
		char line[520];
		int k = snprintf(line, sizeof(line), "T%u '%s':", t, name);
		for (unsigned f = 0; f < n && k < (int)sizeof(line) - 20; f++)
			k += snprintf(line + k, sizeof(line) - (size_t)k, " %lx", unslide(frames[f]));
		say("%s\n", line);
	}
}

/*
 * v45: a sampling profiler on com.apple.coreanimation.display.primary, 50 samples a second from t+20 s to t+80 s.
 * Each sample keeps the first QuartzCore frame and the first plug-in frame of its chain (plug-in as an offset in the
 * HakiosMetal image, for atos -l 0); every 10 s the commonest of each are printed. v44: no GPU buffer took over 5 ms
 * to make, so the time between frames is somewhere else, or the thread is idle.
 */
#include <dlfcn.h>
#define NKEYS 64
struct key { unsigned long a; unsigned n; };
static struct key qk[NKEYS], pk[NKEYS];
static unsigned samples;

static void
bump(struct key *k, unsigned long a)
{
	for (unsigned i = 0; i < NKEYS; i++) {
		if (k[i].a == a) { k[i].n++; return; }
		if (k[i].a == 0) { k[i].a = a; k[i].n = 1; return; }
	}
}

static void
report(const char *what, struct key *k)
{
	char line[560];
	int n = 0;
	for (int r = 0; r < 10; r++) {
		unsigned best = 0, bi = 0;
		for (unsigned i = 0; i < NKEYS; i++)
			if (k[i].n > best) { best = k[i].n; bi = i; }
		if (best == 0) break;
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %lx:%u", k[bi].a, best);
		k[bi].n = 0;
		if (n > (int)sizeof(line) - 30) break;
	}
	say("prof %s (of %u):%s\n", what, samples, line);
	memset(k, 0, sizeof(struct key) * NKEYS);
}

static void *
profiler(void *arg)
{
	(void)arg;
	size_t length = 0;
	uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
	sleep(20);
	for (int round = 0; round < 6; round++) {
		thread_t target = MACH_PORT_NULL;
		thread_act_array_t threads;
		mach_msg_type_number_t count = 0;
		if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
			for (unsigned t = 0; t < count; t++) {
				char name[64] = "";
				pthread_t p = pthread_from_mach_thread_np(threads[t]);
				if (p) pthread_getname_np(p, name, sizeof(name));
				if (strcmp(name, "com.apple.coreanimation.display.primary") == 0) target = threads[t];
			}
		}
		if (target == MACH_PORT_NULL) { say("prof: no display.primary thread\n"); sleep(10); continue; }
		samples = 0;
		for (int s = 0; s < 500; s++) {
			usleep(20000);
			uintptr_t frames[24];
			unsigned n = 0;
			if (thread_suspend(target) != KERN_SUCCESS) continue;
			arm_thread_state64_t st;
			mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
			if (thread_get_state(target, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
				frames[n++] = (uintptr_t)st.__pc;
				frames[n++] = (uintptr_t)st.__lr;
				uintptr_t fp = (uintptr_t)st.__fp;
				while (fp && (fp & 7) == 0 && n < 24) {
					vm_size_t got = 0;
					uintptr_t pair[2];
					if (vm_read_overwrite(mach_task_self(), fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS) break;
					if (pair[1] == 0) break;
					frames[n++] = pair[1];
					if (pair[0] <= fp) break;
					fp = pair[0];
				}
			}
			thread_resume(target);
			samples++;
			unsigned long q = 0, pl = 0;
			for (unsigned f = 0; f < n; f++) {
				uintptr_t a = frames[f];
				if (!q && a >= base && a < base + length) {
					unsigned long u = (unsigned long)(a - slide);
					if (u >= 0x188280000UL && u < 0x188600000UL) q = u;   /* QuartzCore __TEXT */
				}
				if (!pl && !(a >= base && a < base + length)) {
					Dl_info info;
					if (dladdr((void *)a, &info) && info.dli_fname && strstr(info.dli_fname, "HakiosMetal"))
						pl = (unsigned long)(a - (uintptr_t)info.dli_fbase);
				}
			}
			bump(qk, q ? q : 1);
			bump(pk, pl ? pl : 1);
		}
		report("quartzcore", qk);
		report("plugin", pk);
	}
	return NULL;
}

/*
 * v78: SpringBoard's main thread, 50 samples a second from t+15 s, 12 rounds of 500 (to t+135 s): the commonest leaf
 * pc and the commonest caller two frames up, unslid (ipsw dyld a2s ADDR). On v77 the unlock animation crawled while
 * backboardd's display thread sat idle: is SpringBoard the one that is slow to give it frames?
 */
static struct key sk[NKEYS], ck[NKEYS];

static void *
main_profiler(void *arg)
{
	thread_t target = (thread_t)(uintptr_t)arg;
	size_t length = 0;
	uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
	sleep(15);
	for (int round = 0; round < 12; round++) {
		unsigned n_samples = 0, busy = 0;
		for (int s = 0; s < 500; s++) {
			usleep(20000);
			if (thread_suspend(target) != KERN_SUCCESS) continue;
			arm_thread_state64_t st;
			mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
			uintptr_t pc = 0, up = 0;
			if (thread_get_state(target, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
				pc = (uintptr_t)st.__pc;
				uintptr_t fp = (uintptr_t)st.__fp, pair[2];
				vm_size_t got = 0;
				for (int d = 0; d < 2 && fp && (fp & 7) == 0; d++) {
					if (vm_read_overwrite(mach_task_self(), fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS) break;
					up = pair[1];
					if (pair[0] <= fp) break;
					fp = pair[0];
				}
			}
			thread_resume(target);
			n_samples++;
			unsigned long a = pc >= base && pc < base + length ? (unsigned long)(pc - slide) : (unsigned long)pc;
			unsigned long b = up >= base && up < base + length ? (unsigned long)(up - slide) : (unsigned long)up;
			/* (mach_msg_trap's pc, in libsystem_kernel, is a thread waiting) */
			bump(sk, a ? a : 1);
			bump(ck, b ? b : 1);
			(void)busy;
		}
		samples = n_samples;
		report("main leaf", sk);
		report("main caller+2", ck);
	}
	return NULL;
}

/*
 * v103: CoreAnimation's frame scheduling, read live. CA::WindowServer::IOMFBServer::vsync_callback (20H392 0x1882ba1ec)
 * renders at a vsync only when the frame's time (stamp + interval) has reached [srv+0x2b0] (the next update; +inf:
 * nothing scheduled, and then [srv+0x308], the vsync refcount, runs down and vsync and CA's timer are switched off);
 * [srv+0x2d8] is a deadline a vsync's stamp must have passed, [srv+0x2e0] the last stamp. The server is the vsync
 * target IOMobileFramebuffer keeps at [fb+0x5c8], its function at [fb+0x5e8]. Every 250 ms for 4 minutes, a line
 * when anything changed (and each 5 s): what stops the unlock's animation half way.
 */
static int
peek(uintptr_t at, void *out, size_t n)
{
	vm_size_t got = 0;
	return vm_read_overwrite(mach_task_self(), at, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}


/*
 * v106: what a display link in SpringBoard gets. The unlock's zoom is a UIKit spring stepped by SpringBoard's display
 * link and it stops half way while the server has nothing of its own to animate: a CADisplayLink of our own, on a
 * thread of its own, logs once a second how many times it fired and its timestamp, targetTimestamp and duration --
 * does an application's display link keep its 60 a second, and with time that moves?
 */
#include <objc/runtime.h>
#include <objc/message.h>

/*
 * RedMagic v208: this process's display links may run above 60 Hz, as on a ProMotion iPhone whose app opted in with
 * CADisableMinimumFrameDurationOnPhone. QuartzCore keeps that answer in a dispatch_once flag
 * (CADeviceDisableMinimumFrameDurationOnPhone, read by CA::Display::DisplayLinkItem::set_preferred_fps_range_locked):
 * its once token (cache 0x1dcdfbee0) and its byte (0x1dcdfbee8) are set here before QuartzCore first asks. v207 ran
 * the panel and the render server at 90 Hz, but SpringBoard's display links stayed at 60 ticks a second.
 */
static void
ca_high_rate_optin(const char *who)
{
	volatile long *once = (volatile long *)(0x1dcdfbee0UL + slide);
	volatile uint8_t *disabled = (volatile uint8_t *)(0x1dcdfbee8UL + slide);
	if (*once == 0) {
		*disabled = 1;
		*once = -1L;
		say("%s: display links may run above 60 Hz (CADisableMinimumFrameDurationOnPhone set)\n", who);
	} else {
		say("%s: QuartzCore had already decided (token %#lx, value %u): left alone\n", who, (unsigned long)*once,
		    (unsigned)*disabled);
	}
}

static volatile int dl_ticks;
static double dl_ts, dl_target, dl_dur, dl_first;

static void
dl_tick(id self, SEL cmd, id link)
{
	(void)self; (void)cmd;
	dl_ts = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("timestamp"));
	dl_target = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("targetTimestamp"));
	dl_dur = ((double (*)(id, SEL))objc_msgSend)(link, sel_registerName("duration"));
	if (dl_first == 0) dl_first = dl_ts;
	dl_ticks++;
}

static void *
dl_reporter(void *arg)
{
	(void)arg;
	void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	double (*now)(void) = qc ? (double (*)(void))dlsym(qc, "CACurrentMediaTime") : NULL;
	int last = 0;
	for (int i = 0; i < 240; i++) {
		sleep(1);
		int n = dl_ticks;
		say("displaylink: %d ticks in the last second (%d in all); now %.3f timestamp %.3f target %.3f duration %.4f\n",
		    n - last, n, now ? now() : 0.0, dl_ts, dl_target, dl_dur);
		last = n;
	}
	return NULL;
}

static void *
dl_thread(void *arg)
{
	(void)arg;
	sleep(20);   /* (after SpringBoard's own start-up) */
	void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	(void)qc;
	Class base = objc_getClass("NSObject");
	Class cls = objc_allocateClassPair(base, "RMDisplayLinkProbe", 0);
	if (cls == Nil) { say("displaylink: no probe class\n"); return NULL; }
	class_addMethod(cls, sel_registerName("tick:"), (IMP)dl_tick, "v@:@");
	objc_registerClassPair(cls);
	id obj = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc")), sel_registerName("init"));
	Class dlc = objc_getClass("CADisplayLink");
	if (dlc == Nil) { say("displaylink: no CADisplayLink class\n"); return NULL; }
	id link = ((id (*)(Class, SEL, id, SEL))objc_msgSend)(dlc, sel_registerName("displayLinkWithTarget:selector:"), obj, sel_registerName("tick:"));
	id loop = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSRunLoop"), sel_registerName("currentRunLoop"));
	{
		/* v208: ask for up to 120 a second (CAFrameRateRange min, max, preferred) and say what the display offers */
		struct rm_rate_range { float minimum, maximum, preferred; } range = { 60.0f, 120.0f, 120.0f };
		SEL sr = sel_registerName("setPreferredFrameRateRange:");
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(link, sel_registerName("respondsToSelector:"), sr)) {
			((void (*)(id, SEL, struct rm_rate_range))objc_msgSend)(link, sr, range);
		}
		Class cad = objc_getClass("CADisplay");
		id main = cad ? ((id (*)(Class, SEL))objc_msgSend)(cad, sel_registerName("mainDisplay")) : NULL;
		if (main != NULL) {
			/* v208b: only selectors CADisplay answers (v208 asked for UIScreen's maximumFramesPerSecond and crashed) */
			static const char *const names[] = { "refreshRate", "hardwareRefreshRate", "minimumFrameDuration",
			                                     "heartbeatRate" };
			double v[4] = { -1, -1, -1, -1 };
			for (int i = 0; i < 4; i++) {
				SEL s = sel_registerName(names[i]);
				if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(main, sel_registerName("respondsToSelector:"), s)) {
					v[i] = ((double (*)(id, SEL))objc_msgSend)(main, s);
				}
			}
			say("displaylink: main display refreshRate %.6f hardwareRefreshRate %.6f minimumFrameDuration %.6f "
			    "heartbeatRate %.6f\n", v[0], v[1], v[2], v[3]);
			/*
			 * v210: where 1/60 comes from. -[CADisplay refreshRate] reads the CA::Display::Display behind ivar +0x8:
			 * periods at +0x1b8 (refresh), +0x1c0 (hardware), +0x1c8 (heartbeat), or +0x1d8 for all three when
			 * bit 2 of +0x250 is set. Also the modes the server sent (CADisplayMode refreshRate).
			 */
			const uint8_t *d = *(const uint8_t *const *)((const uint8_t *)main + 8);
			if (d != NULL) {
				say("displaylink: Display %p periods 1b8 %.6f 1c0 %.6f 1c8 %.6f 1d8 %.6f flags250 %#x\n", (void *)d,
				    *(const double *)(d + 0x1b8), *(const double *)(d + 0x1c0), *(const double *)(d + 0x1c8),
				    *(const double *)(d + 0x1d8), (unsigned)d[0x250]);
			}
			static const char *const modesel[] = { "currentMode", "preferredMode", "nativeMode" };
			for (int i = 0; i < 3; i++) {
				SEL s = sel_registerName(modesel[i]);
				if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(main, sel_registerName("respondsToSelector:"), s)) continue;
				id m = ((id (*)(id, SEL))objc_msgSend)(main, s);
				if (m == NULL) {
					say("displaylink: %s nil\n", modesel[i]);
					continue;
				}
				SEL rr = sel_registerName("refreshRate");
				double r = ((BOOL (*)(id, SEL, SEL))objc_msgSend)(m, sel_registerName("respondsToSelector:"), rr) ?
				    ((double (*)(id, SEL))objc_msgSend)(m, rr) : -1;
				unsigned long w = ((unsigned long (*)(id, SEL))objc_msgSend)(m, sel_registerName("width"));
				unsigned long h = ((unsigned long (*)(id, SEL))objc_msgSend)(m, sel_registerName("height"));
				say("displaylink: %s %lux%lu refreshRate %.6f\n", modesel[i], w, h, r);
			}
			id modes = ((id (*)(id, SEL))objc_msgSend)(main, sel_registerName("availableModes"));
			long n = modes ? ((long (*)(id, SEL))objc_msgSend)(modes, sel_registerName("count")) : -1;
			say("displaylink: %ld available modes\n", n);
			for (long i = 0; i < n && i < 6; i++) {
				id m = ((id (*)(id, SEL, unsigned long))objc_msgSend)(modes, sel_registerName("objectAtIndex:"), (unsigned long)i);
				double r = ((double (*)(id, SEL))objc_msgSend)(m, sel_registerName("refreshRate"));
				say("displaylink: mode %ld refreshRate %.6f\n", i, r);
			}
		}
	}
	((void (*)(id, SEL, id, id))objc_msgSend)(link, sel_registerName("addToRunLoop:forMode:"), loop, (id)kCFRunLoopDefaultMode);
	say("displaylink: probe added (%p) on its own thread\n", (void *)link);
	pthread_t r;
	pthread_create(&r, NULL, dl_reporter, NULL);
	pthread_detach(r);
	extern void CFRunLoopRun(void);
	CFRunLoopRun();
	return NULL;
}


/*
 * v109: a process's os_log, printed: libsystem_trace's os_log_set_hook calls this block for every message (on the
 * logging thread), os_log_copy_message_string formats it. To stderr (the kernel's /dev/null tee puts it in the log),
 * debug messages left out, 8000 lines at most a process. What SpringBoard and PosterBoard say while the unlock waits.
 */
typedef void (^rm_os_log_hook_t)(uint8_t type, void *msg);
static volatile int oslog_lines;
/*
 * RedMagic v192: errors and faults only in the apps and daemons watched for hangs (their info lines filled the 4 MB
 * ring in a few minutes of opening apps: ring207 lost the daemons' thread dumps). backboardd, SpringBoard, Settings,
 * the posters and configd keep everything.
 */
static int oslog_errors_only;

static void
oslog_install(void)
{
	void *t = dlopen("/usr/lib/system/libsystem_trace.dylib", RTLD_NOW);
	rm_os_log_hook_t (*set_hook)(uint8_t, rm_os_log_hook_t) = t ? (rm_os_log_hook_t (*)(uint8_t, rm_os_log_hook_t))dlsym(t, "os_log_set_hook") : NULL;
	char *(*copy)(void *) = t ? (char *(*)(void *))dlsym(t, "os_log_copy_message_string") : NULL;
	if (set_hook == NULL || copy == NULL) {
		say("oslog: no hook (%p %p)\n", (void *)set_hook, (void *)copy);
		return;
	}
	static rm_os_log_hook_t previous;
	previous = set_hook(0, ^(uint8_t type, void *msg) {
		/* v111: errors and faults only, and whatever mentions CoreGraphics (its backtraces): the 4 MB ring kept
		 * the boot's chatter and lost the unlock */
		/* v115: everything but debug from 50 s after the hook went in (the boot's chatter out, the unlock in), errors
		 * from the start; 6000 lines */
		static uint64_t from;
		if (from == 0) from = mach_absolute_time() + (strcmp(getprogname(), "configd") == 0 ? 0 : (uint64_t)50 * 19200000ull);   /* v145: configd from the start */
		const int late = mach_absolute_time() >= from;
		if (type != 2 && (late || type == 16 || type == 17) && oslog_lines < 6000 &&
		    (!oslog_errors_only || type == 16 || type == 17)) {
			char *text = copy(msg);
			if (text != NULL && strstr(text, "invalid context") != NULL && strstr(text, "CGBitmapContextCreateImage") == NULL) {
				free(text);
				text = NULL;
			}
			/* v113: CoreGraphics redacts its backtraces' names: the raw stack, cache addresses unslid, symbolised
			 * on the Mac (ipsw dyld a2s); the hook runs on the thread that logged */
			static volatile int stacks;
			if (text != NULL && stacks < 12 && strstr(text, "invalid context") != NULL) {
				stacks++;
				void *frames[48];
				int nf = backtrace(frames, 48);
				char st[1400];
				int at = snprintf(st, sizeof(st), "RM CGSTACK %d:", stacks);
				for (int i = 1; i < nf && at < (int)sizeof(st) - 20; i++) {
					at += snprintf(st + at, sizeof(st) - (size_t)at, " %lx", (unsigned long)((uintptr_t)frames[i] - slide));
				}
				st[at < (int)sizeof(st) - 1 ? at : (int)sizeof(st) - 2] = '\n';
				write(2, st, (size_t)(at < (int)sizeof(st) - 1 ? at + 1 : (int)sizeof(st) - 1));
			}
			if (text != NULL) {
				oslog_lines++;
				char line[4000];
				int n = snprintf(line, sizeof(line), "RM OSLOG %u: %s\n", type, text);
				if (n > (int)sizeof(line) - 1) { n = (int)sizeof(line) - 1; line[n - 1] = '\n'; }
				write(2, line, (size_t)n);
				free(text);
			}
		}
		if (previous) previous(type, msg);
	});
	say("oslog: hook installed\n");
}

static void *
iomfb_watch(void *arg)
{
	(void)arg;
	void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	double (*now)(void) = qc ? (double (*)(void))dlsym(qc, "CACurrentMediaTime") : NULL;
	const uintptr_t cb = 0x1882ba1ecUL + slide;
	uintptr_t srv = 0;
	char last[400] = "";
	for (int i = 0; i < 960; i++) {
		usleep(250000);
		if (srv == 0) {
			/* v104: the object CA registered for vsync, from the kernel (GetMainDisplay made a new one each call) */
			uint64_t fbv = 0;
			size_t fbn = sizeof(fbv);
			if (sysctlbyname("kern.mondrian_vsync_refcon", &fbv, &fbn, NULL, 0) != 0 || fbv == 0) continue;
			void *fb = (void *)(uintptr_t)fbv;
			uintptr_t ref = 0, fn = 0;
			if (!peek((uintptr_t)fb + 0x5c8, &ref, 8) || !peek((uintptr_t)fb + 0x5e8, &fn, 8)) continue;
			if (fn != cb) {
				if (i % 40 == 0) say("iomfb: fb %p vsync fn %#lx (CA's %#lx), target %#lx: not CA's yet\n", fb, (unsigned long)fn, (unsigned long)cb, (unsigned long)ref);
				continue;
			}
			srv = ref;
			say("iomfb: fb %p, CA's IOMFBServer %#lx\n", fb, (unsigned long)srv);
		}
		double next = 0;
		uint64_t q2b8 = 0, q2c0 = 0, dl = 0, lastv = 0;
		uint32_t refs = 0;
		uint8_t flags = 0;
		if (!peek(srv + 0x2b0, &next, 8) || !peek(srv + 0x2b8, &q2b8, 8) || !peek(srv + 0x2c0, &q2c0, 8) ||
		    !peek(srv + 0x2d8, &dl, 8) || !peek(srv + 0x2e0, &lastv, 8) || !peek(srv + 0x308, &refs, 4) || !peek(srv + 0x30d, &flags, 1)) {
			say("iomfb: server %#lx unreadable\n", (unsigned long)srv);
			srv = 0;
			continue;
		}
		char line[400];
		snprintf(line, sizeof(line), "next_update %.4f 2b8 %#llx 2c0 %#llx deadline %llu last_vbl %llu refs %u flags %#x",
			 next, (unsigned long long)q2b8, (unsigned long long)q2c0, (unsigned long long)dl, (unsigned long long)lastv, refs, flags);
		/* (the timestamps change each vsync: compare the rest) */
		char key[400];
		snprintf(key, sizeof(key), "%.4f %u %#x %llu", next, refs, flags, (unsigned long long)q2b8);
		if (strcmp(key, last) != 0 || i % 20 == 0) {
			strcpy(last, key);
			say("iomfb t %.3f abs %llu: %s\n", now ? now() : 0.0, (unsigned long long)mach_absolute_time(), line);
		}
	}
	return NULL;
}

static void *
summary(void *arg)
{
	(void)arg;
	unsigned last = ~0u;
	for (int i = 0; i < 150; i++) {
		sleep(2);
		if (i == 19 || i == 34) dump_threads(2 * (i + 1));
		unsigned now = 0;
		for (unsigned k = 0; k < NW; k++) now += w[k].calls;
		if (now == last && i % 10 != 0) continue;   /* quiet while nothing moves, but say so every 20 s */
		last = now;
		char line[560];
		int n = 0;
		for (unsigned k = 0; k < NW && n < (int)sizeof(line) - 40; k++)
			if (w[k].calls)
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %u/%u", strstr(w[k].name, "::") + 2, w[k].calls,
				    w[k].nonzero);
		say("t+%ds calls/nonzero:%s\n", 2 * (i + 1), n ? line : " none");
	}
	return NULL;
}

/*
 * v136: a hang watchdog for Settings (its Date & Time and About pages hang). Every second the main queue is pinged;
 * when a ping has waited 3 s, the main thread's stack is printed (frames minus the cache slide: ipsw dyld a2s), every
 * 3 s while it stays stuck, 12 stacks at most.
 */
static volatile int hang_pong;
static mach_port_t hang_main;

static void
hang_stack(int stuckFor)
{
	uintptr_t frames[32];
	unsigned n = 0;

	if (thread_suspend(hang_main) != KERN_SUCCESS) return;
	arm_thread_state64_t st;
	mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state(hang_main, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
		frames[n++] = (uintptr_t)st.__pc;
		frames[n++] = (uintptr_t)st.__lr;
		uintptr_t fp = (uintptr_t)st.__fp;
		while (fp && (fp & 7) == 0 && n < 32) {
			vm_size_t got = 0;
			uintptr_t pair[2];
			if (vm_read_overwrite(mach_task_self(), fp, sizeof(pair), (vm_address_t)pair, &got) != KERN_SUCCESS) break;
			if (pair[1] == 0) break;
			frames[n++] = pair[1];
			if (pair[0] <= fp) break;
			fp = pair[0];
		}
	}
	thread_resume(hang_main);
	char line[900];
	int k = snprintf(line, sizeof(line), "RM HANG main thread stuck %d s:", stuckFor);
	for (unsigned f = 0; f < n && k < (int)sizeof(line) - 20; f++)
		k += snprintf(line + k, sizeof(line) - (size_t)k, " %lx", unslide(frames[f]));
	say("%s\n", line);
}

static void *
hang_thread(void *arg)
{
	int stacks = 0, waited = 0;

	(void)arg;
	for (;;) {
		hang_pong = 0;
		dispatch_async(dispatch_get_main_queue(), ^{ hang_pong = 1; });
		for (waited = 0; waited < 3000 && !hang_pong; waited += 100) usleep(100000);
		int stuck = 3;
		while (!hang_pong && stacks < 12) {
			hang_stack(stuck);
			if (stacks == 0) {
				dump_threads(stuck);   /* v137: who holds what the main thread waits for */
			}
			stacks++;
			for (int w = 0; w < 30 && !hang_pong; w++) usleep(100000);
			stuck += 3;
		}
		sleep(1);
	}
	return NULL;
}

/*
 * v141: which service a synchronous NSXPC call waits on (locationd deadlocks inside one; Settings' Date & Time waits
 * on locationd): -[NSXPCConnection _sendInvocation:...] logged once per service and selector, 80 at most.
 */
extern void xpc_connection_cancel(void *connection);    /* v195: libxpc (the iOS SDK keeps its header from us) */
typedef void (*send_inv_fn)(id, SEL, id, void *, unsigned long, id, SEL, id);
static send_inv_fn send_inv_orig;
/*
 * RedMagic v192: in locationd, a call to locationd's own services that has not returned in 2 s has its connection
 * invalidated, so a synchronous one comes back with an error instead of waiting for ever. v191 answered one such
 * self-call (CoreLocation's authorization query, cache 0x191dbde88); ring207 showed another (cache 0x191d99328 via
 * NSInvocation) holding the LocationSupport queue that locationd's main thread, its XPC activities and its
 * tracking-avoidance service all wait on, and Maps, Compass and News still waited 36 s. A call that returns in time
 * is untouched; CoreLocation makes a new connection the next time.
 */
static int in_locationd;

/*
 * RedMagic v193: locationd's own synchronous Mach calls give up after 3 s. Its main queue's holder (rings 206-208,
 * the same frames every time) sat in a MIG call of its own code (locationd 0x1000c74f0: mach_msg SEND|RCV, no
 * timeout, msgh_id 1203) made from FairPlay's obfuscated code: fairplayd.H2 is started and exits without ever
 * taking the request, so the reply never comes, and locationd's main thread, its XPC activities and every client
 * (Maps, Compass, News) waited behind it. Its import slot for mach_msg (__got at locationd 0x101b40098, this build)
 * now points here: a call that sends and receives with no timeout gets MACH_RCV_TIMEOUT 3000 ms, and on
 * MACH_RCV_TIMED_OUT the MIG stub gives its reply port back and reports the error. Only calls from locationd's own
 * code pass here (the system libraries keep their own bindings).
 */
typedef mach_msg_return_t (*mach_msg_fn)(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t, mach_msg_size_t,
    mach_port_name_t, mach_msg_timeout_t, mach_port_name_t);
static mach_msg_fn locd_mach_msg_real;

static mach_msg_return_t
locd_mach_msg(mach_msg_header_t *msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size,
    mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify)
{
	if ((option & MACH_SEND_MSG) && (option & MACH_RCV_MSG) && !(option & MACH_RCV_TIMEOUT)) {
		const mach_msg_id_t id = msg->msgh_id;
		mach_msg_return_t r = locd_mach_msg_real(msg, option | MACH_RCV_TIMEOUT, send_size, rcv_size, rcv_name, 3000,
		    notify);
		if (r == MACH_RCV_TIMED_OUT) {
			static volatile int told;
			if (told++ < 20) say("locationd: Mach call id %d unanswered for 3 s: given up\n", (int)id);
		}
		return r;
	}
	return locd_mach_msg_real(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
}

static void
locd_mach_msg_install(void)
{
	/* v194: the image by its path (index 0 was not locationd: ring209 read code at the slot) */
	intptr_t image_slide = -1;
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		if (n != NULL && strcmp(n, "/usr/libexec/locationd") == 0) {
			image_slide = _dyld_get_image_vmaddr_slide(i);
			break;
		}
	}
	if (image_slide == -1) {
		say("locationd: /usr/libexec/locationd not among the images: mach_msg left alone\n");
		return;
	}
	uintptr_t *slot = (uintptr_t *)(0x101b40098UL + (uintptr_t)image_slide);
	const uintptr_t want = (uintptr_t)&mach_msg;
	if (*slot != want) {
		say("locationd: __got slot at %p holds %#lx, not mach_msg (%#lx): not this build, left alone\n", (void *)slot,
		    (unsigned long)*slot, (unsigned long)want);
		return;
	}
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("locationd: __got page not writable (%d): mach_msg left alone\n", kr);
		return;
	}
	locd_mach_msg_real = (mach_msg_fn)want;
	*slot = (uintptr_t)locd_mach_msg;
	(void)vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("locationd: its own mach_msg calls now time out after 3 s (slot %p)\n", (void *)slot);
}
static unsigned send_keys[96];
static unsigned send_nkeys;

static void
send_inv_hook(id self, SEL cmd, id inv, void *args, unsigned long count, id sig, SEL sel, id proxy)
{
	id service = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("serviceName"));
	const char *svc = service ? ((const char *(*)(id, SEL))objc_msgSend)(service, sel_registerName("UTF8String")) : "(endpoint)";
	const char *name = sel ? sel_getName(sel) : "?";
	unsigned key = 5381;
	for (const char *p = svc; p && *p; p++) key = key * 33 + (unsigned char)*p;
	for (const char *p = name; *p; p++) key = key * 33 + (unsigned char)*p;
	unsigned i;
	for (i = 0; i < send_nkeys && send_keys[i] != key; i++) {
	}
	if (i == send_nkeys && send_nkeys < 80) {
		send_keys[send_nkeys++] = key;
		say("XPC send: %s -[%s] (thread %p)\n", svc ? svc : "(null)", name, (void *)pthread_self());
	}
	/*
	 * RedMagic v194: services whose daemon never runs on this phone (launchd keeps the name, so a call waits for
	 * ever instead of failing): CommCenter (no modem; vmd's CoreTelephonyClient getActiveContexts held its queue and
	 * Phone waited on vmd, ring209). The connection is invalidated before the send, so the caller gets
	 * NSXPCConnectionInvalid at once.
	 */
	if (svc != NULL && strcmp(svc, "com.apple.commcenter.coretelephony.xpc") == 0) {
		static volatile int told;
		if (told++ < 5) say("XPC: %s -[%s]: CommCenter never runs here, failed at once\n", svc, name);
		/* v195: the xpc connection cancelled here and now: -invalidate is asynchronous, and vmd's next synchronous
		 * call (getSubscriptionInfoWithError:) still went out before it took effect and waited for ever (ring210) */
		void *x = ((void *(*)(id, SEL))objc_msgSend)(self, sel_registerName("_xpcConnection"));
		if (x != NULL) xpc_connection_cancel(x);
		((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("invalidate"));
		/* v200: a caller that tries again at once spun vmd at 85-90 % of a core (ring217): past 10 such failures in
		 * a second, each one waits 200 ms first (5 a second at most); an ordinary caller never waits */
		static uint64_t window;
		static unsigned inWindow;
		const uint64_t t = mach_absolute_time();
		if (t - window > 19200000ull) {
			window = t;
			inWindow = 0;
		}
		if (++inWindow > 10) {
			static volatile int throttled;
			if (throttled++ < 3) say("XPC: CommCenter calls failing more than 10 a second: each now waits 200 ms\n");
			usleep(200000);
		}
	}
	/* v192: every service, not only its own: locationd also calls CommCenter (never started here), mobileactivationd,
	 * healthd... and any of them that never answers holds the same queue */
	if (in_locationd) {
		__block volatile int done = 0;
		id conn = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("retain"));
		char what[160];
		snprintf(what, sizeof(what), "%s -[%s]", svc, name);
		char *label = strdup(what);
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
			if (!done) {
				static volatile int broken;
				if (broken++ < 20) say("locationd: a call waited 2 s, connection invalidated: %s\n", label);
				((void (*)(id, SEL))objc_msgSend)(conn, sel_registerName("invalidate"));
			}
			((void (*)(id, SEL))objc_msgSend)(conn, sel_registerName("release"));
			free(label);
		});
		send_inv_orig(self, cmd, inv, args, count, sig, sel, proxy);
		done = 1;
		return;
	}
	send_inv_orig(self, cmd, inv, args, count, sig, sel, proxy);
}

static void
xpc_send_install(void)
{
	Class c = objc_getClass("NSXPCConnection");
	SEL s = sel_registerName("_sendInvocation:orArguments:count:methodSignature:selector:withProxy:");
	Method m = c ? class_getInstanceMethod(c, s) : NULL;
	if (m == NULL) {
		say("XPC send: no -[NSXPCConnection _sendInvocation:...]\n");
		return;
	}
	send_inv_orig = (send_inv_fn)method_setImplementation(m, (IMP)send_inv_hook);
	say("XPC send: hook installed\n");
}

/*
 * v142: Settings' Date & Time no longer asks locationd anything for the "Set Automatically" footer: locationd
 * deadlocks on a synchronous call into itself (v141), and -[PSGDateTimeController setAutomaticTimeFooter] waited on
 * it for ever. The class lives in a settings bundle loaded when General opens, so it is looked for twice a second.
 */
static void
dt_footer_noop(id self, SEL cmd)
{
	(void)self; (void)cmd;
}

static void *
dt_patch_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 2400; i++) {
		Class c = objc_getClass("PSGDateTimeController");
		if (c != NULL) {
			Method m = class_getInstanceMethod(c, sel_registerName("setAutomaticTimeFooter"));
			if (m != NULL) {
				method_setImplementation(m, (IMP)dt_footer_noop);
				say("Date & Time: automatic-time footer no longer asks locationd\n");
			} else {
				say("Date & Time: PSGDateTimeController has no setAutomaticTimeFooter\n");
			}
			return NULL;
		}
		usleep(500000);
	}
	return NULL;
}

/*
 * RedMagic v190: accountsd answers "no tethered sync" (@0) itself. -[ACDAccountStore
 * tetheredSyncSourceTypeForDataclass:completion:] asks lockdownd (_tetheredSyncSourceTypeForDataType ->
 * lockdown_copy_value), lockdownd never answers, and the recvfrom has no timeout: that accountsd queue stopped for good,
 * and Notes waited on it from launch for ever (-[NoteContext setUpLocalAccountAndStore] -> ACAccountStore
 * tetheredSyncSourceTypeForDataclass:, ring205). The reply is completion(NSNumber type, NSError error); the client keeps
 * the number. Tethered sync is iTunes/Finder syncing: none here.
 */
static void
acd_tethered_none(id self, SEL cmd, id dataclass, void (^completion)(id, id))
{
	(void)self; (void)cmd; (void)dataclass;
	static id zero;
	if (zero == NULL) {
		zero = ((id (*)(Class, SEL, int))objc_msgSend)(objc_getClass("NSNumber"), sel_registerName("numberWithInt:"), 0);
		zero = ((id (*)(id, SEL))objc_msgSend)(zero, sel_registerName("retain"));
	}
	static volatile int told;
	if (told++ < 5) {
		say("accountsd: tethered sync source asked: none (lockdownd not asked)\n");
	}
	if (completion != NULL) {
		completion(zero, NULL);
	}
}

static void *
acd_patch_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 120; i++) {
		Class c = objc_getClass("ACDAccountStore");
		if (c != NULL) {
			Method m = class_getInstanceMethod(c, sel_registerName("tetheredSyncSourceTypeForDataclass:completion:"));
			if (m != NULL) {
				method_setImplementation(m, (IMP)acd_tethered_none);
				say("accountsd: tetheredSyncSourceTypeForDataclass: answered here, lockdownd left alone\n");
			} else {
				say("accountsd: ACDAccountStore has no tetheredSyncSourceTypeForDataclass:completion:\n");
			}
			return NULL;
		}
		usleep(250000);
	}
	say("accountsd: no ACDAccountStore class after 30 s\n");
	return NULL;
}

/*
 * RedMagic v191: locationd no longer asks itself for an app's location authorization. Its thread dump (ring206):
 * one thread inside CoreLocation's authorization query (cache 0x191dbde88 -> CLInternalGetAuthorizationStatus ->
 * a synchronous NSXPC call to locationd's own com.apple.locationd.synchronous), the main thread and another in
 * dispatch_sync on the LocationSupport queue that call holds: a self-deadlock, and every client (Maps, Compass,
 * News, Settings' Date & Time) waited on locationd for ever. The query's own failure answer is 0 (not determined),
 * so that is what it answers inside locationd, without the call. The method is found by its implementation's
 * address (this build's cache) among CoreLocation's classes, and named in the log.
 */
static int
locd_auth_none(id self, SEL cmd, id bundleID, id bundlePath)
{
	(void)self; (void)cmd; (void)bundleID; (void)bundlePath;
	static volatile int told;
	if (told++ < 5) {
		say("locationd: an authorization status asked of itself: 0 (not determined), no call to itself\n");
	}
	return 0;
}

static int
locd_patch_auth(void)
{
	const uintptr_t target = 0x191dbde88UL + slide;
	unsigned count = 0;
	const char **names = objc_copyClassNamesForImage("/System/Library/Frameworks/CoreLocation.framework/CoreLocation",
	    &count);
	if (names == NULL || count == 0) {
		return 0;
	}
	for (unsigned i = 0; i < count; i++) {
		Class c = objc_getClass(names[i]);
		if (c == NULL) continue;
		Class both[2] = { object_getClass((id)c), c };
		for (int k = 0; k < 2; k++) {
			unsigned n = 0;
			Method *ms = class_copyMethodList(both[k], &n);
			for (unsigned j = 0; j < n; j++) {
				if ((uintptr_t)method_getImplementation(ms[j]) == target) {
					method_setImplementation(ms[j], (IMP)locd_auth_none);
					say("locationd: %c[%s %s] (cache %#lx) answers 0 here: the self-deadlock is gone\n", k == 0 ? '+' : '-',
					    names[i], sel_getName(method_getName(ms[j])), (unsigned long)(target - slide));
					free(ms);
					free(names);
					return 1;
				}
			}
			free(ms);
		}
	}
	free(names);
	return -1;
}

static void *
locd_patch_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 120; i++) {
		int r = locd_patch_auth();
		if (r != 0) {
			if (r < 0) say("locationd: no CoreLocation method at cache 0x191dbde88: the deadlock is left as it was\n");
			return NULL;
		}
		usleep(250000);
	}
	return NULL;
}

/*
 * RedMagic v191: apps no longer wait on FairPlay for the user agent's device type. Podcasts' main thread sat in
 * AMSFairPlayGetDeviceType from -[AMSUserAgent _sharedComponentFairPlayDeviceType] (ring206) while fairplayd.H2 was
 * started and exited 19 times without an answer (it needs the real secure hardware). The method's own failure
 * answer is nil (the user agent goes without that component); that is what it answers now.
 */
static id
ams_fp_none(id self, SEL cmd)
{
	(void)self; (void)cmd;
	static volatile int told;
	if (told++ < 3) say("AMSUserAgent: FairPlay device type: none (fairplayd not asked)\n");
	return NULL;
}

/*
 * RedMagic v194: the camera apps (FaceTime, Measure, Magnifier) no longer wait at launch on the capture server.
 * -[AVCaptureProprietaryDefaultsSingleton init] asks com.apple.coremedia.capturesession for its defaults source
 * (FigCaptureSourceRemoteCopyCaptureSources, a synchronous FigXPC call); the server, cameracaptured, is never started
 * here (no camera), so the call waited for ever (ring206). The source is left unset, as if the server had none.
 */
static void
avcap_defaults_none(id self, SEL cmd, bool source)
{
	(void)self; (void)cmd; (void)source;
	static volatile int told;
	if (told++ < 3) say("AVCapture: proprietary defaults source not asked of the capture server (no camera)\n");
}

static void *
avcap_patch_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 240; i++) {
		Class c = objc_getClass("AVCaptureProprietaryDefaultsSingleton");
		if (c != NULL) {
			Method m = class_getInstanceMethod(c, sel_registerName("_updateProprietaryDefaultsSource:"));
			if (m != NULL) {
				method_setImplementation(m, (IMP)avcap_defaults_none);
				say("AVCapture: _updateProprietaryDefaultsSource: answered here\n");
			}
			return NULL;
		}
		usleep(250000);
	}
	return NULL;
}

static void *
ams_patch_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 240; i++) {
		Class c = objc_getClass("AMSUserAgent");
		if (c != NULL) {
			Method m = class_getInstanceMethod(c, sel_registerName("_sharedComponentFairPlayDeviceType"));
			if (m != NULL) {
				method_setImplementation(m, (IMP)ams_fp_none);
				say("AMSUserAgent: _sharedComponentFairPlayDeviceType answered here\n");
			}
			return NULL;
		}
		usleep(250000);
	}
	return NULL;
}

/*
 * RedMagic v195: which of the services apps wait on launchd actually has, and which jobs in this phone's own
 * /System/Library/xpc/launchd.plist publish them. The root build removes CommCenter's jobs (ios-root-tweaks.py), yet
 * calls to com.apple.commcenter.coretelephony.xpc waited instead of failing (rings 209-210).
 */
extern mach_port_t bootstrap_port;     /* libxpc/liblaunch: the SDK has no <servers/bootstrap.h> */
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
static const char *const names_watched[] = {
	"com.apple.commcenter.coretelephony.xpc", "com.apple.commcenter.xpc", "com.apple.coremedia.capturesession",
	"com.apple.locationd.synchronous", "com.apple.locationd.registration", "com.apple.fairplayd.versioned",
	"com.apple.fairplayd", "com.apple.fairplayd.H2", "com.apple.visualvoicemail", "com.apple.passd.library",
	"com.apple.lockdown.host_watcher", NULL,
};

struct names_ctx {
	unsigned jobs, hits;
};

static void
names_job(const void *key, const void *value, void *context)
{
	struct names_ctx *c = context;
	c->jobs++;
	if (CFGetTypeID(value) != CFDictionaryGetTypeID()) return;
	CFDictionaryRef job = value;
	CFDictionaryRef ms = CFDictionaryGetValue(job, CFSTR("MachServices"));
	if (ms == NULL || CFGetTypeID(ms) != CFDictionaryGetTypeID()) return;
	for (int i = 0; names_watched[i] != NULL; i++) {
		CFStringRef n = CFStringCreateWithCString(NULL, names_watched[i], kCFStringEncodingUTF8);
		const bool has = CFDictionaryContainsKey(ms, n);
		CFRelease(n);
		if (has) {
			char path[300] = "?", label[160] = "?";
			if (CFGetTypeID(key) == CFStringGetTypeID()) CFStringGetCString(key, path, sizeof(path), kCFStringEncodingUTF8);
			CFStringRef l = CFDictionaryGetValue(job, CFSTR("Label"));
			if (l != NULL && CFGetTypeID(l) == CFStringGetTypeID()) CFStringGetCString(l, label, sizeof(label), kCFStringEncodingUTF8);
			say("NAMES: %s is published by %s (%s)%s%s\n", names_watched[i], label, path,
			    CFDictionaryContainsKey(job, CFSTR("_LimitLoadToDeviceTree")) ? " [_LimitLoadToDeviceTree]" : "",
			    CFDictionaryContainsKey(job, CFSTR("_LimitLoadFromDeviceTree")) ? " [_LimitLoadFromDeviceTree]" : "");
			c->hits++;
		}
	}
}

/*
 * RedMagic v196: services this phone can never answer leave launchd's table, so a call to them fails at once
 * (as CommCenter's do since the root build removed its jobs) instead of waiting for ever. ring211's names check:
 * com.apple.fairplayd(.versioned) are held for fairplayd.H2, which exits on every start (no FairPlay hardware) --
 * locationd's FairPlay call blocked its main queue and with it Maps, Compass, News and passd (Wallet); and
 * mediaserverd publishes the capture services but never serves them (no camera) -- FaceTime, Measure and
 * Magnifier waited on com.apple.coremedia.capturesession. The edit is made once, on this phone's own
 * /System/Library/xpc/launchd.plist (kept first as launchd.plist.rm195), and launchd reads it at the next boot.
 */
static const char *const lp_capture[] = {
	"com.apple.coremedia.capturesession", "com.apple.coremedia.capturesource", "com.apple.videoconference.camera",
	"com.apple.coremedia.capture.deferred.containermanager.xpc", "com.apple.coremedia.figvirtualcapturecard.xpc",
	"com.apple.coremedia.cameraviewfinder", NULL,
};

static int
lp_copy_file(const char *from, const char *to)
{
	int in = open(from, O_RDONLY), out = -1, ok = 0;
	if (in < 0) return 0;
	out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (out >= 0) {
		char buf[65536];
		ssize_t r;
		ok = 1;
		while ((r = read(in, buf, sizeof(buf))) > 0) {
			if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
		}
		if (r < 0) ok = 0;
		fsync(out);
		close(out);
	}
	close(in);
	return ok;
}

static void
launchd_plist_fix(void)
{
	const char *path = "/System/Library/xpc/launchd.plist", *backup = "/System/Library/xpc/launchd.plist.rm195";
	const char *tmp = "/System/Library/xpc/launchd.plist.rm196-new";
	CFURLRef url = CFURLCreateWithFileSystemPath(NULL, CFSTR("/System/Library/xpc/launchd.plist"), kCFURLPOSIXPathStyle, false);
	CFReadStreamRef rs = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	CFPropertyListRef pl = NULL;
	if (rs != NULL && CFReadStreamOpen(rs)) {
		pl = CFPropertyListCreateWithStream(NULL, rs, 0, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
		CFReadStreamClose(rs);
	}
	if (rs != NULL) CFRelease(rs);
	if (url != NULL) CFRelease(url);
	if (pl == NULL || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
		say("LAUNCHD: launchd.plist not read: left alone\n");
		if (pl != NULL) CFRelease(pl);
		return;
	}
	CFMutableDictionaryRef daemons = (CFMutableDictionaryRef)CFDictionaryGetValue(pl, CFSTR("LaunchDaemons"));
	if (daemons == NULL || CFGetTypeID(daemons) != CFDictionaryGetTypeID()) {
		say("LAUNCHD: no LaunchDaemons: left alone\n");
		CFRelease(pl);
		return;
	}
	const CFIndex before = CFDictionaryGetCount(daemons);
	int removedJobs = 0, removedNames = 0;
	CFStringRef fp = CFSTR("/System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist");
	if (CFDictionaryContainsKey(daemons, fp)) {
		CFDictionaryRemoveValue(daemons, fp);
		removedJobs++;
	}
	CFMutableDictionaryRef media = (CFMutableDictionaryRef)CFDictionaryGetValue(daemons,
	    CFSTR("/System/Library/LaunchDaemons/com.apple.mediaserverd.plist"));
	if (media != NULL && CFGetTypeID(media) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef ms = (CFMutableDictionaryRef)CFDictionaryGetValue(media, CFSTR("MachServices"));
		if (ms != NULL && CFGetTypeID(ms) == CFDictionaryGetTypeID()) {
			for (int i = 0; lp_capture[i] != NULL; i++) {
				CFStringRef n = CFStringCreateWithCString(NULL, lp_capture[i], kCFStringEncodingUTF8);
				if (CFDictionaryContainsKey(ms, n)) {
					CFDictionaryRemoveValue(ms, n);
					removedNames++;
				}
				CFRelease(n);
			}
		}
	}
	if (removedJobs == 0 && removedNames == 0) {
		say("LAUNCHD: launchd.plist already without fairplayd.H2 and mediaserverd's capture names (%ld daemons)\n", (long)before);
		CFRelease(pl);
		return;
	}
	struct stat sb;
	if (stat(backup, &sb) != 0 && !lp_copy_file(path, backup)) {
		say("LAUNCHD: no backup could be written (errno %d): left alone\n", errno);
		CFRelease(pl);
		return;
	}
	CFErrorRef err = NULL;
	CFDataRef data = CFPropertyListCreateData(NULL, pl, kCFPropertyListBinaryFormat_v1_0, 0, &err);
	CFRelease(pl);
	if (data == NULL) {
		say("LAUNCHD: the edited plist did not serialise: left alone\n");
		if (err != NULL) CFRelease(err);
		return;
	}
	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
	int ok = fd >= 0;
	if (ok) {
		const CFIndex len = CFDataGetLength(data);
		ok = write(fd, CFDataGetBytePtr(data), (size_t)len) == (ssize_t)len;
		fsync(fd);
		close(fd);
		(void)chown(tmp, 0, 0);
	}
	CFRelease(data);
	if (ok && rename(tmp, path) == 0) {
		sync();
		say("LAUNCHD: launchd.plist edited for the next boot: %d job(s) removed (fairplayd.H2), %d capture name(s) taken "
		    "from mediaserverd; %ld daemons before; the original is launchd.plist.rm195\n", removedJobs, removedNames, (long)before);
	} else {
		say("LAUNCHD: launchd.plist not replaced (errno %d): left as it was\n", errno);
		unlink(tmp);
	}
}

static void *
names_thread(void *arg)
{
	(void)arg;
	sleep(70);
	for (int i = 0; names_watched[i] != NULL; i++) {
		mach_port_t port = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_look_up(bootstrap_port, names_watched[i], &port);
		say("NAMES: look up %s: %s (kr %d, port %#x)\n", names_watched[i], kr == KERN_SUCCESS ? "FOUND" : "absent", kr, port);
		if (port != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), port);
	}
	CFURLRef url = CFURLCreateWithFileSystemPath(NULL, CFSTR("/System/Library/xpc/launchd.plist"), kCFURLPOSIXPathStyle, false);
	CFReadStreamRef rs = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	if (rs != NULL && CFReadStreamOpen(rs)) {
		CFPropertyListRef pl = CFPropertyListCreateWithStream(NULL, rs, 0, kCFPropertyListImmutable, NULL, NULL);
		CFReadStreamClose(rs);
		if (pl != NULL && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
			CFDictionaryRef daemons = CFDictionaryGetValue(pl, CFSTR("LaunchDaemons"));
			struct names_ctx c = { 0, 0 };
			if (daemons != NULL && CFGetTypeID(daemons) == CFDictionaryGetTypeID()) {
				CFDictionaryApplyFunction(daemons, names_job, &c);
			}
			say("NAMES: this phone's launchd.plist: %u daemons, %u publish a watched name\n", c.jobs, c.hits);
		} else {
			say("NAMES: launchd.plist not read as a dictionary\n");
		}
		if (pl != NULL) CFRelease(pl);
	} else {
		say("NAMES: launchd.plist not opened\n");
	}
	if (rs != NULL) CFRelease(rs);
	if (url != NULL) CFRelease(url);
	return NULL;
}

/*
 * RedMagic v197: the v196 launchd.plist edit undone. Without fairplayd.H2's job lskdd failed at once and launchd
 * restarted it 188 times in 14 minutes, the screen stopped drawing and the SoC ran at 74-84 C (ring213). The
 * original kept by v196 (launchd.plist.rm195) goes back in place, as soon as the root is writable; launchd reads
 * it at the next boot. Nothing is done when the two are the same or there is no backup.
 */
static int
lp_same_file(const char *a, const char *b)
{
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), same = fa >= 0 && fb >= 0;
	while (same) {
		char x[16384], y[16384];
		ssize_t ra = read(fa, x, sizeof(x)), rb = read(fb, y, sizeof(y));
		if (ra != rb || ra < 0) { same = 0; break; }
		if (ra == 0) break;
		if (memcmp(x, y, (size_t)ra) != 0) same = 0;
	}
	if (fa >= 0) close(fa);
	if (fb >= 0) close(fb);
	return same;
}

static void *
launchd_restore_thread(void *arg)
{
	const char *path = "/System/Library/xpc/launchd.plist", *backup = "/System/Library/xpc/launchd.plist.rm195";
	const char *tmp = "/System/Library/xpc/launchd.plist.rm197-restore";
	struct stat sb;
	(void)arg;
	if (stat(backup, &sb) != 0) {
		say("LAUNCHD: no launchd.plist.rm195 backup: nothing to restore\n");
		return NULL;
	}
	if (lp_same_file(path, backup)) {
		say("LAUNCHD: launchd.plist is already the original (same as launchd.plist.rm195)\n");
		return NULL;
	}
	for (int i = 0; i < 90; i++) {         /* the root becomes writable once launchd remounts it */
		unlink(tmp);
		if (lp_copy_file(backup, tmp)) {
			(void)chown(tmp, 0, 0);
			(void)chmod(tmp, 0644);
			if (rename(tmp, path) == 0) {
				sync();
				say("LAUNCHD: launchd.plist restored from launchd.plist.rm195 (v196 edit undone, next boot) after %d s\n", i * 2);
				return NULL;
			}
		}
		sleep(2);
	}
	say("LAUNCHD: launchd.plist could NOT be restored (errno %d)\n", errno);
	unlink(tmp);
	return NULL;
}

/*
 * RedMagic v180: jailbreak diagnostics, 75 s after the process starts (SpringBoard: mobile, wifid: root). What this
 * process is (uid, code-signing flags, sandbox, task_for_pid of launchd), the security sysctls and boot line, where
 * it may write, whether /private/var survives a reboot (a counter), and three test programs the kernel blob put in
 * /private/var/tmp (redmagic7/gpu/jbtest.c: ad-hoc signed, unsigned, ad-hoc with jailbreak entitlements) spawned.
 */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);
extern char **environ;

static const char *
jb_can_write(const char *path)
{
	static char why[4][48];
	static unsigned turn;
	char *w = why[turn++ & 3];
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);

	if (fd < 0) {
		snprintf(w, 48, "NO (errno %d)", errno);
		return w;
	}
	write(fd, "rm\n", 3);
	close(fd);
	unlink(path);
	return "yes";
}

static void
jb_spawn(const char *path)
{
	int fds[2], status = 0, done = 0, e;
	pid_t pid = 0;
	posix_spawn_file_actions_t fa;
	char out[700];
	ssize_t n = 0, r;
	struct stat st;
	char *argv[2] = { (char *)path, NULL };

	if (stat(path, &st) != 0) {
		say("JB %s: not there (errno %d)\n", path, errno);
		return;
	}
	if (pipe(fds) != 0) return;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	e = posix_spawn(&pid, path, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (e != 0) {
		say("JB %s (mode %o, %lld bytes): posix_spawn REFUSED, errno %d\n", path, st.st_mode & 07777, (long long)st.st_size, e);
		close(fds[0]);
		return;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	for (int i = 0; i < 50 && !done; i++) {
		r = read(fds[0], out + n, sizeof(out) - 1 - (size_t)n);
		if (r > 0) n += r;
		if (waitpid(pid, &status, WNOHANG) == pid) done = 1;
		else usleep(100000);
	}
	while (n < (ssize_t)sizeof(out) - 1 && (r = read(fds[0], out + n, sizeof(out) - 1 - (size_t)n)) > 0) n += r;
	out[n] = 0;
	close(fds[0]);
	if (!done) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	say("JB %s: %s, %s %d; it said: %s%s", path, done ? "ran" : "TIMED OUT (killed)",
	    WIFSIGNALED(status) ? "KILLED by signal" : "exit", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status),
	    n ? out : "(nothing)", n && out[n - 1] == '\n' ? "" : "\n");
}

static void *
jb_thread(void *arg)
{
	uint32_t flags = 0;
	mach_port_t task = MACH_PORT_NULL;
	kern_return_t kr;
	char args[1024] = { 0 };
	size_t size = sizeof(args) - 1;
	static const char *const names[] = { "kern.secure_kernel", "security.mac.proc_enforce", "security.mac.vnode_enforce",
	    "vm.cs_debug", "vm.cs_process_enforcement", "vm.cs_system_enforcement", "vm.cs_library_validation",
	    "vm.cs_force_kill", "vm.cs_force_hard", "kern.development", "kern.bootsessionuuid" };
	struct stat st;

	(void)arg;
	sleep(75);
	csops(getpid(), 0, &flags, sizeof(flags));
	kr = task_for_pid(mach_task_self(), 1, &task);
	say("JB in %s (pid %d): uid %d euid %d; cs flags %#x; sandbox_check(any) %d; task_for_pid(launchd) %#x\n",
	    getprogname(), getpid(), getuid(), geteuid(), flags, sandbox_check(getpid(), NULL, 0), kr);
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) say("JB boot line: %s\n", args);
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char value[64] = { 0 };
		size_t vsize = sizeof(value) - 1;
		if (sysctlbyname(names[i], value, &vsize, NULL, 0) != 0) {
			say("JB sysctl %s: absent (errno %d)\n", names[i], errno);
		} else if (vsize == 4) {
			say("JB sysctl %s = %d\n", names[i], *(int *)value);
		} else if (vsize == 8) {
			say("JB sysctl %s = %lld\n", names[i], *(long long *)value);
		} else {
			say("JB sysctl %s = \"%s\"\n", names[i], value);
		}
	}
	say("JB write: / %s, /private/var/tmp %s, /private/var/mobile %s, /private/preboot %s; /var/jb %s\n",
	    jb_can_write("/rm-jb-w"), jb_can_write("/private/var/tmp/rm-jb-w"), jb_can_write("/private/var/mobile/rm-jb-w"),
	    jb_can_write("/private/preboot/rm-jb-w"), stat("/var/jb", &st) == 0 ? "exists" : "absent");
	if (getuid() == 501 || geteuid() == 501 || strcmp(getprogname(), "SpringBoard") == 0) {
		/* /private/var across reboots: a counter of SpringBoard starts */
		const char *path = "/private/var/mobile/Library/rm-boot-count";
		int count = 0, fd = open(path, O_RDONLY);
		if (fd >= 0) {
			char b[16] = { 0 };
			read(fd, b, sizeof(b) - 1);
			count = atoi(b);
			close(fd);
		}
		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd >= 0) {
			char b[16];
			int l = snprintf(b, sizeof(b), "%d\n", count + 1);
			write(fd, b, (size_t)l);
			close(fd);
		}
		say("JB /private/var persistence: this is SpringBoard start number %d on this /var (%s)\n", count + 1,
		    fd >= 0 ? "counted" : "COULD NOT WRITE");
	}
	jb_spawn("/private/var/tmp/rm-jbtest-adhoc");
	jb_spawn("/private/var/tmp/rm-jbtest-unsigned");
	jb_spawn("/private/var/tmp/rm-jbtest-ent");
	return NULL;
}

/*
 * RedMagic v184: root SSH over Wi-Fi. wifid runs as root; once the kernel blob has written dropbear
 * (/usr/local/bin/dropbearmulti, redmagic7/ssh/build-ssh.sh) it is started: host keys made on first start (-R),
 * no password logins (-s): only the key in /var/root/.ssh/authorized_keys. Then en0's address is said.
 */
static void *
ssh_address_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 120; i++) {
		struct ifaddrs *list = NULL, *a;
		char found[64] = "";
		if (getifaddrs(&list) == 0) {
			for (a = list; a != NULL; a = a->ifa_next) {
				if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, "en0") == 0) {
					inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, found, sizeof(found));
				}
			}
			freeifaddrs(list);
		}
		if (found[0]) {
			say("SSH: ssh root@%s (en0)\n", found);
			return NULL;
		}
		sleep(5);
	}
	say("SSH: en0 has no IPv4 address after 10 min\n");
	return NULL;
}

static void *
ssh_thread(void *arg)
{
	const char *db = "/usr/local/bin/dropbearmulti";
	struct stat st;
	pid_t pid = 0;
	int e, fds[2];
	posix_spawn_file_actions_t fa;
	pthread_t t;
	/* -F -E: in the foreground, its log on stderr, which comes back here into the ring */
	char *argv[] = { "dropbearmulti", "dropbear", "-F", "-E", "-R", "-s", "-p", "22", NULL };
	/* a clean environment: wifid's has DYLD_INSERT_LIBRARIES (the kernel's UI hook), which dropbear must not carry */
	char *envp[] = { "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root", NULL };

	(void)arg;
	for (int i = 0; i < 60 && stat(db, &st) != 0; i++) sleep(5);
	if (stat(db, &st) != 0) {
		say("SSH: %s never appeared\n", db);
		return NULL;
	}
	sleep(2);
	if (pipe(fds) != 0) return NULL;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	e = posix_spawn(&pid, db, &fa, NULL, argv, envp);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (e != 0) {
		say("SSH: dropbear posix_spawn errno %d\n", e);
		close(fds[0]);
		return NULL;
	}
	say("SSH: dropbear started, pid %d (key logins only)\n", pid);
	pthread_create(&t, NULL, ssh_address_thread, NULL);
	pthread_detach(t);
	/* its log, line by line, as long as it runs (at most 2000 lines) */
	char line[300];
	size_t n = 0;
	unsigned lines = 0;
	for (;;) {
		char c;
		ssize_t r = read(fds[0], &c, 1);
		if (r <= 0) break;
		if (c == '\n' || n == sizeof(line) - 1) {
			line[n] = 0;
			if (lines++ < 2000) say("SSH: dropbear: %s\n", line);
			n = 0;
		} else {
			line[n++] = c;
		}
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	say("SSH: dropbear ended: %s %d\n", WIFSIGNALED(status) ? "signal" : "exit",
	    WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
	return NULL;
}

static void *
locd_thread(void *arg)
{
	(void)arg;
	sleep(60);
	dump_threads(60);
	sleep(60);
	dump_threads(120);
	/* v192: then every 90 s, 8 more: the first two were gone from a session's ring by the time apps were opened */
	for (int i = 1; i <= 8; i++) {
		sleep(90);
		dump_threads(120 + 90 * i);
	}
	return NULL;
}

__attribute__((constructor)) static void
ca_watch(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) return;
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "SpringBoard") == 0) {
		/* v78: this thread is the main thread (the constructor runs on it) */
		say("in SpringBoard, cache slide %#lx: sampling the main thread\n", (unsigned long)slide);
		ca_high_rate_optin("SpringBoard");      /* v208 */
		/* v108: SpringBoard's os_log mirrored to its stderr (the kernel tees that into the log): what the unlock's
		 * transition is waiting for. Boot-arg sb_oslog=0 leaves it out. */
		{
			char args[1024] = { 0 };
			size_t size = sizeof(args) - 1;
			if (!(sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sb_oslog=0") != NULL)) {
				oslog_install();
			}
			/* v111: where SpringBoard's CGContext calls get an invalid context (read when the error happens) */
			setenv("CG_CONTEXT_SHOW_BACKTRACE", "1", 1);
		}
		pthread_t t;
		pthread_create(&t, NULL, main_profiler, (void *)(uintptr_t)mach_thread_self());
		pthread_detach(t);
		pthread_create(&t, NULL, dl_thread, NULL);
		pthread_detach(t);
		pthread_create(&t, NULL, jb_thread, NULL);      /* v180 */
		pthread_detach(t);
		return;
	}
	/* v148: the apps that do not draw: os_log, and the main thread's stacks when it stops answering */
	{
		static const char *const apps[] = { "MobileNotes", "FaceTime", "AppleTV", "Passbook", "Podcasts", "News", "Maps", "MobilePhone", "Measure", "Magnifier", "MobileSafari", "VoiceMemos", "Compass" };
		for (unsigned a = 0; a < sizeof(apps) / sizeof(apps[0]); a++) {
			if (strcmp(name, apps[a]) == 0) {
				say("in %s, cache slide %#lx: os_log hook and main-thread hang watchdog\n", name, (unsigned long)slide);
				ca_high_rate_optin(name);           /* v208 */
				oslog_errors_only = 1;      /* v192 */
				oslog_install();
				hang_main = mach_thread_self();
				pthread_t t;
				pthread_create(&t, NULL, hang_thread, NULL);
				pthread_detach(t);
				pthread_create(&t, NULL, ams_patch_thread, NULL);    /* v191 */
				pthread_detach(t);
				pthread_create(&t, NULL, avcap_patch_thread, NULL);  /* v194 */
				pthread_detach(t);
				xpc_send_install();                                  /* v194: CommCenter calls fail at once */
				return;
			}
		}
	}
	if (strcmp(name, "configd") == 0) {
		/* v145: InterfaceNamer's own words: why it never posts that IOKit is quiet */
		say("in configd: os_log hook\n");
		oslog_install();
		return;
	}
	if (strcmp(name, "accountsd") == 0 || strcmp(name, "wifid") == 0 ||
	    strcmp(name, "lockdownd") == 0 ||   /* v170: wifid too (never answers Settings queryPower); v190: lockdownd */
	    /* v191: what Wallet (passd), Phone (vmd: VMVoicemailManager fetchAccounts) and the camera apps
	     * (FigCaptureSourceRemote, the capture server) wait on */
	    strcmp(name, "passd") == 0 || strcmp(name, "vmd") == 0 || strcmp(name, "mediaserverd") == 0) {
		/* v149: Notes waits on accountsd for ever (ACAccountStore tetheredSyncSourceTypeForDataclass:): its threads */
		say("in %s, cache slide %#lx: threads at 60 and 120 s\n", name, (unsigned long)slide);
		oslog_errors_only = 1;              /* v192 */
		oslog_install();
		xpc_send_install();
		pthread_t t;
		pthread_create(&t, NULL, locd_thread, NULL);
		pthread_detach(t);
		if (strcmp(name, "accountsd") == 0) {
			pthread_create(&t, NULL, acd_patch_thread, NULL);     /* v190 */
			pthread_detach(t);
		}
		pthread_create(&t, NULL, ams_patch_thread, NULL);     /* v194: passd waited on FairPlay too (ring209) */
		pthread_detach(t);
		if (strcmp(name, "passd") == 0 || strcmp(name, "vmd") == 0 || strcmp(name, "mediaserverd") == 0) {
			/* v195: their main thread's stack every 3 s it stays stuck (the 90 s dumps can miss the moment) */
			hang_main = mach_thread_self();
			pthread_create(&t, NULL, hang_thread, NULL);
			pthread_detach(t);
		}
		if (strcmp(name, "wifid") == 0) {
			pthread_create(&t, NULL, launchd_restore_thread, NULL);   /* v197: first, at once */
			pthread_detach(t);
			pthread_create(&t, NULL, names_thread, NULL);     /* v195 */
			pthread_detach(t);
		}
		if (strcmp(name, "wifid") == 0) {
			pthread_create(&t, NULL, jb_thread, NULL);      /* v180: the same, as root */
			pthread_detach(t);
			pthread_create(&t, NULL, ssh_thread, NULL);     /* v184: root SSH */
			pthread_detach(t);
		}
		return;
	}
	if (strcmp(name, "locationd") == 0) {
		/* v140: locationd never answers Settings' Date & Time (CoreLocation sync XPC): its threads at 60 s and 120 s */
		say("in locationd, cache slide %#lx: threads at 60 and 120 s\n", (unsigned long)slide);
		in_locationd = 1;                   /* v192 */
		hang_main = mach_thread_self();     /* v195: its main thread's stack when it stops answering */
		{
			pthread_t ht;
			pthread_create(&ht, NULL, hang_thread, NULL);
			pthread_detach(ht);
		}
		locd_mach_msg_install();            /* v193 */
		{
			pthread_t ft;
			pthread_create(&ft, NULL, ams_patch_thread, NULL);    /* v194 */
			pthread_detach(ft);
		}
		oslog_errors_only = 1;
		pthread_t t;
		if (locd_patch_auth() == 0) {       /* v191: at once if CoreLocation's classes are there, else polled */
			pthread_create(&t, NULL, locd_patch_thread, NULL);
			pthread_detach(t);
		}
		oslog_install();
		xpc_send_install();
		pthread_create(&t, NULL, locd_thread, NULL);
		pthread_detach(t);
		return;
	}
	if (strcmp(name, "Preferences") == 0) {
		say("in Settings, cache slide %#lx: os_log hook and main-thread hang watchdog\n", (unsigned long)slide);
		ca_high_rate_optin("Settings");         /* v208 */
		oslog_install();
		xpc_send_install();
		{
			pthread_t dt;
			pthread_create(&dt, NULL, dt_patch_thread, NULL);
			pthread_detach(dt);
		}
		hang_main = mach_thread_self();
		pthread_t t;
		pthread_create(&t, NULL, hang_thread, NULL);
		pthread_detach(t);
		return;
	}
	if (strcmp(name, "PosterBoard") == 0 || strcmp(name, "GradientPosterExtension") == 0) {
		say("in %s: os_log hook\n", name);
		oslog_install();
		return;
	}
	if (strcmp(name, "backboardd") != 0) return;
	/* v38: CoreAnimation's frame clock from the display's refresh notifications (HackiOS ca_vbl=1).
	 * v78: boot-arg ca_timer=0 leaves it out, to see what the server's frame pace is without it. */
	{
		char args[1024] = { 0 };
		size_t size = sizeof(args) - 1;
		int timer = !(sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "ca_timer=0") != NULL);
		if (timer) setenv("CA_FORCE_TIMER", "1", 1);
		say("in backboardd, CA_FORCE_TIMER %s, cache slide %#lx\n", timer ? "=1" : "not set (ca_timer=0)", (unsigned long)slide);
		/*
		 * v211: CoreAnimation's heartbeat (the period display links and the server's frames run at) from the panel's
		 * measured vsync period (kern.mondrian_fb_period_ns). The built-in panel's 91 Hz timing (TimingElements)
		 * is dropped by IOMFBDisplay::update_digital_modes (only IsVirtual or digital-out timings count), so the
		 * display has no mode and AppleCLCDDisplay::heartbeat_rate takes CA_HEARTBEAT_RATE, else 1/60 s.
		 * Boot-arg ca_hb=0 leaves it out.
		 */
		{
			uint32_t ns = 0;
			size_t nn = sizeof(ns);
			if (strstr(args, "ca_hb=0") == NULL &&
			    sysctlbyname("kern.mondrian_fb_period_ns", &ns, &nn, NULL, 0) == 0 && ns >= 5000000u && ns <= 20000000u) {
				char hb[32];
				snprintf(hb, sizeof(hb), "%.4f", 1e9 / (double)ns);
				setenv("CA_HEARTBEAT_RATE", hb, 1);
				say("in backboardd, CA_HEARTBEAT_RATE=%s (vsync period %u ns)\n", hb, ns);
			} else {
				say("in backboardd, CA_HEARTBEAT_RATE not set (period %u ns)\n", ns);
			}
		}
		/* v97: every frame drawn whole (QuartzCore's CA_DISABLE_DIRTY_REGIONS): the surface dumps showed content of
		 * the unlock zoom left in place by every frame after it, each redrawing only what CoreAnimation counted as
		 * changed. Boot-arg ca_dirty=1 leaves CoreAnimation's own dirty regions. */
		if (strstr(args, "ca_dirty=1") == NULL) setenv("CA_DISABLE_DIRTY_REGIONS", "1", 1);
		say("in backboardd, CA_DISABLE_DIRTY_REGIONS %s\n", strstr(args, "ca_dirty=1") == NULL ? "=1" : "not set (ca_dirty=1)");
	}
	/* v116: the render server's own os_log (commits held, fences, frames): SpringBoard finishes the unlock, the
	 * screen keeps its middle */
	oslog_install();
	install();
	pthread_t t;
	pthread_create(&t, NULL, summary, NULL);
	pthread_detach(t);
	pthread_create(&t, NULL, profiler, NULL);
	pthread_detach(t);
	pthread_create(&t, NULL, iomfb_watch, NULL);
	pthread_detach(t);
}
