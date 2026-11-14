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
	extern const void *kCFRunLoopDefaultMode;
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
		if (type != 2 && (late || type == 16 || type == 17) && oslog_lines < 6000) {
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
typedef void (*send_inv_fn)(id, SEL, id, void *, unsigned long, id, SEL, id);
static send_inv_fn send_inv_orig;
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

static void *
locd_thread(void *arg)
{
	(void)arg;
	sleep(60);
	dump_threads(60);
	sleep(60);
	dump_threads(120);
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
		return;
	}
	/* v148: the apps that do not draw: os_log, and the main thread's stacks when it stops answering */
	{
		static const char *const apps[] = { "MobileNotes", "FaceTime", "AppleTV", "Passbook", "Podcasts", "News", "Maps", "MobilePhone", "Measure", "Magnifier", "MobileSafari", "VoiceMemos", "Compass" };
		for (unsigned a = 0; a < sizeof(apps) / sizeof(apps[0]); a++) {
			if (strcmp(name, apps[a]) == 0) {
				say("in %s, cache slide %#lx: os_log hook and main-thread hang watchdog\n", name, (unsigned long)slide);
				oslog_install();
				hang_main = mach_thread_self();
				pthread_t t;
				pthread_create(&t, NULL, hang_thread, NULL);
				pthread_detach(t);
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
	if (strcmp(name, "accountsd") == 0 || strcmp(name, "wifid") == 0) {   /* v170: wifid too (never answers Settings queryPower) */
		/* v149: Notes waits on accountsd for ever (ACAccountStore tetheredSyncSourceTypeForDataclass:): its threads */
		say("in %s, cache slide %#lx: threads at 60 and 120 s\n", name, (unsigned long)slide);
		oslog_install();
		xpc_send_install();
		pthread_t t;
		pthread_create(&t, NULL, locd_thread, NULL);
		pthread_detach(t);
		return;
	}
	if (strcmp(name, "locationd") == 0) {
		/* v140: locationd never answers Settings' Date & Time (CoreLocation sync XPC): its threads at 60 s and 120 s */
		say("in locationd, cache slide %#lx: threads at 60 and 120 s\n", (unsigned long)slide);
		oslog_install();
		xpc_send_install();
		pthread_t t;
		pthread_create(&t, NULL, locd_thread, NULL);
		pthread_detach(t);
		return;
	}
	if (strcmp(name, "Preferences") == 0) {
		say("in Settings, cache slide %#lx: os_log hook and main-thread hang watchdog\n", (unsigned long)slide);
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
