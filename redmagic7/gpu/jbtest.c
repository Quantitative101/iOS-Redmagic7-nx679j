/*
 * RedMagic 7 jailbreak diagnostics (v180): a tiny program the kernel blob writes to /private/var/tmp in three
 * forms (ad-hoc signed, unsigned, ad-hoc signed with jailbreak-style entitlements) and ca-watch spawns from
 * SpringBoard (mobile) and wifid (root). It says who it is, what code-signing status the kernel gave it, whether
 * it may write the root and /private/var, and whether task_for_pid(launchd) works; then exits 42.
 *
 * Build: redmagic7/gpu/build-jbtest.sh
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

static const char *
try_write(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	static char why[64];

	if (fd < 0) {
		snprintf(why, sizeof(why), "NO (errno %d)", errno);
		return why;
	}
	write(fd, "rm\n", 3);
	close(fd);
	unlink(path);
	return "yes";
}

int
main(int argc, char **argv)
{
	uint32_t flags = 0;
	mach_port_t task = MACH_PORT_NULL;
	kern_return_t kr;

	(void)argc;
	csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof(flags));
	kr = task_for_pid(mach_task_self(), 1, &task);
	printf("JBTEST %s: uid %d euid %d gid %d; cs flags %#x (valid %d, platform %d, debugged %d, get-task-allow %d); "
	    "write / %s, /private/var/tmp %s, /private/var/mobile %s; task_for_pid(1) %#x%s\n",
	    argv[0], getuid(), geteuid(), getgid(), flags, !!(flags & 0x1), !!(flags & 0x4000000), !!(flags & 0x10000000),
	    !!(flags & 0x4), try_write("/rm-jbtest-root"), try_write("/private/var/tmp/rm-jbtest-w"),
	    try_write("/private/var/mobile/rm-jbtest-w"), kr, kr == KERN_SUCCESS ? " (GOT launchd's task port)" : "");
	fflush(stdout);
	return 42;
}
