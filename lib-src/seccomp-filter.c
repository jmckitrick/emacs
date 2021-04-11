/* Generate a Secure Computing filter definition file.

Copyright (C) 2020-2021 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

GNU Emacs is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see
<https://www.gnu.org/licenses/>.  */

/* This program creates a small Secure Computing filter usable for a
typical minimal Emacs sandbox.  See the man page for `seccomp' for
details about Secure Computing filters.  This program requires the
`libseccomp' library.  However, the resulting filter file requires
only a Linux kernel supporting the Secure Computing extension.

Usage:

  seccomp-filter out.bpf out.pfc

This writes the raw `struct sock_filter' array to out.bpf and a
human-readable representation to out.pfc.  */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/futex.h>
#include <fcntl.h>
#include <sched.h>
#include <seccomp.h>
#include <unistd.h>

#include "verify.h"

static ATTRIBUTE_FORMAT_PRINTF (2, 3) _Noreturn void
fail (int error, const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  if (error == 0)
    {
      vfprintf (stderr, format, ap);
      fputc ('\n', stderr);
    }
  else
    {
      char buffer[1000];
      vsnprintf (buffer, sizeof buffer, format, ap);
      errno = error;
      perror (buffer);
    }
  va_end (ap);
  fflush (NULL);
  exit (EXIT_FAILURE);
}

/* This binary is trivial, so we use a single global filter context
   object that we release using `atexit'.  */

static scmp_filter_ctx ctx;

static void
release_context (void)
{
  seccomp_release (ctx);
}

/* Wrapper functions and macros for libseccomp functions.  We exit
   immediately upon any error to avoid error checking noise.  */

static void
set_attribute (enum scmp_filter_attr attr, uint32_t value)
{
  int status = seccomp_attr_set (ctx, attr, value);
  if (status < 0)
    fail (-status, "seccomp_attr_set (ctx, %u, %u)", attr, value);
}

/* Like `seccomp_rule_add (ACTION, SYSCALL, ...)', except that you
   don't have to specify the number of comparator arguments, and any
   failure will exit the process.  */

#define RULE(action, syscall, ...)                                   \
  do                                                                 \
    {                                                                \
      const struct scmp_arg_cmp arg_array[] = {__VA_ARGS__};         \
      enum { arg_cnt = sizeof arg_array / sizeof *arg_array };       \
      int status = seccomp_rule_add_array (ctx, (action), (syscall), \
                                           arg_cnt, arg_array);      \
      if (status < 0)                                                \
        fail (-status, "seccomp_rule_add_array (%s, %s, %d, {%s})",  \
              #action, #syscall, arg_cnt, #__VA_ARGS__);             \
    }                                                                \
  while (false)

static void
export_filter (const char *file,
               int (*function) (const scmp_filter_ctx, int),
               const char *name)
{
  int fd = TEMP_FAILURE_RETRY (
    open (file, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY | O_CLOEXEC,
          0644));
  if (fd < 0)
    fail (errno, "open %s", file);
  int status = function (ctx, fd);
  if (status < 0)
    fail (-status, "%s", name);
  if (close (fd) != 0)
    fail (errno, "close");
}

#define EXPORT_FILTER(file, function) \
  export_filter ((file), (function), #function)

int
main (int argc, char **argv)
{
  if (argc != 3)
    fail (0, "usage: %s out.bpf out.pfc", argv[0]);

  /* Any unhandled syscall should abort the Emacs process.  */
  ctx = seccomp_init (SCMP_ACT_KILL_PROCESS);
  if (ctx == NULL)
    fail (0, "seccomp_init");
  atexit (release_context);

  /* We want to abort immediately if the architecture is unknown.  */
  set_attribute (SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_KILL_PROCESS);
  set_attribute (SCMP_FLTATR_CTL_NNP, 1);
  set_attribute (SCMP_FLTATR_CTL_TSYNC, 1);

  verify (CHAR_BIT == 8);
  verify (sizeof (int) == 4 && INT_MIN == INT32_MIN
          && INT_MAX == INT32_MAX);
  verify (sizeof (void *) == 8);
  verify ((uintptr_t) NULL == 0);

  /* Allow a clean exit.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (exit));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (exit_group));

  /* Allow `mmap' and friends.  This is necessary for dynamic loading,
     reading the portable dump file, and thread creation.  We don't
     allow pages to be both writable and executable.  */
  verify (MAP_PRIVATE != 0);
  verify (MAP_SHARED != 0);
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (mmap),
        SCMP_A2_32 (SCMP_CMP_MASKED_EQ,
                    ~(PROT_NONE | PROT_READ | PROT_WRITE)),
        /* Only support known flags.  MAP_DENYWRITE is ignored, but
           some versions of the dynamic loader still use it.  Also
           allow allocating thread stacks.  */
        SCMP_A3_32 (SCMP_CMP_MASKED_EQ,
                    ~(MAP_PRIVATE | MAP_FILE | MAP_ANONYMOUS
                      | MAP_FIXED | MAP_DENYWRITE | MAP_STACK
                      | MAP_NORESERVE),
                    0));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (mmap),
        SCMP_A2_32 (SCMP_CMP_MASKED_EQ,
                    ~(PROT_NONE | PROT_READ | PROT_EXEC)),
        /* Only support known flags.  MAP_DENYWRITE is ignored, but
           some versions of the dynamic loader still use it. */
        SCMP_A3_32 (SCMP_CMP_MASKED_EQ,
                    ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED
                      | MAP_DENYWRITE),
                    0));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (munmap));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (mprotect),
        /* Don't allow making pages executable.  */
        SCMP_A2_32 (SCMP_CMP_MASKED_EQ,
                    ~(PROT_NONE | PROT_READ | PROT_WRITE), 0));

  /* Futexes are used everywhere.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (futex),
        SCMP_A1_32 (SCMP_CMP_EQ, FUTEX_WAKE_PRIVATE));

  /* Allow basic dynamic memory management.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (brk));

  /* Allow some status inquiries.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (uname));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getuid));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (geteuid));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getpid));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getpgrp));

  /* Allow operations on open file descriptors.  File descriptors are
     capabilities, and operating on them shouldn't cause security
     issues.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (read));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (write));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (close));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (lseek));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (dup));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (dup2));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (fstat));

  /* Allow read operations on the filesystem.  If necessary, these
     should be further restricted using mount namespaces.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (access));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (faccessat));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (stat));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (stat64));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (lstat));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (lstat64));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (fstatat64));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (newfstatat));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (readlink));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (readlinkat));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getcwd));

  /* Allow opening files, assuming they are only opened for
     reading.  */
  verify (O_WRONLY != 0);
  verify (O_RDWR != 0);
  verify (O_CREAT != 0);
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (open),
        SCMP_A1_32 (SCMP_CMP_MASKED_EQ,
                    ~(O_RDONLY | O_BINARY | O_CLOEXEC | O_PATH
                      | O_DIRECTORY),
                    0));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (openat),
        SCMP_A2_32 (SCMP_CMP_MASKED_EQ,
                    ~(O_RDONLY | O_BINARY | O_CLOEXEC | O_PATH
                      | O_DIRECTORY),
                    0));

  /* Allow `tcgetpgrp'.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (ioctl),
        SCMP_A0_32 (SCMP_CMP_EQ, STDIN_FILENO),
        SCMP_A1_32 (SCMP_CMP_EQ, TIOCGPGRP));

  /* Allow reading (but not setting) file flags.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (fcntl),
        SCMP_A1_32 (SCMP_CMP_EQ, F_GETFL));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (fcntl64),
        SCMP_A1_32 (SCMP_CMP_EQ, F_GETFL));

  /* Allow reading random numbers from the kernel.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getrandom));

  /* Changing the umask is uncritical.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (umask));

  /* Allow creation of pipes.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (pipe));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (pipe2));

  /* Allow reading (but not changing) resource limits.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (getrlimit));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (prlimit64),
	SCMP_A0_32 (SCMP_CMP_EQ, 0) /* pid == 0 (current process) */,
        SCMP_A2_64 (SCMP_CMP_EQ, 0) /* new_limit == NULL */);

  /* Block changing resource limits, but don't crash.  */
  RULE (SCMP_ACT_ERRNO (EPERM), SCMP_SYS (prlimit64),
        SCMP_A0_32 (SCMP_CMP_EQ, 0) /* pid == 0 (current process) */,
        SCMP_A2_64 (SCMP_CMP_NE, 0) /* new_limit != NULL */);

  /* Emacs installs signal handlers, which is harmless.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (sigaction));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (rt_sigaction));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (sigprocmask));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (rt_sigprocmask));

  /* Allow reading the current time.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (clock_gettime),
        SCMP_A0_32 (SCMP_CMP_EQ, CLOCK_REALTIME));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (time));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (gettimeofday));

  /* Allow timer support.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (timer_create));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (timerfd_create));

  /* Allow thread creation.  See the NOTES section in the manual page
     for the `clone' function.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (clone),
        SCMP_A0_64 (SCMP_CMP_MASKED_EQ,
                    /* Flags needed to create threads.  See
                       create_thread in libc.  */
                    ~(CLONE_VM | CLONE_FS | CLONE_FILES
                      | CLONE_SYSVSEM | CLONE_SIGHAND | CLONE_THREAD
                      | CLONE_SETTLS | CLONE_PARENT_SETTID
                      | CLONE_CHILD_CLEARTID),
                    0));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (sigaltstack));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (set_robust_list));

  /* Allow setting the process name for new threads.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (prctl),
        SCMP_A0_32 (SCMP_CMP_EQ, PR_SET_NAME));

  /* Allow some event handling functions used by glib.  */
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (eventfd));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (eventfd2));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (wait4));
  RULE (SCMP_ACT_ALLOW, SCMP_SYS (poll));

  /* Don't allow creating sockets (network access would be extremely
     dangerous), but also don't crash.  */
  RULE (SCMP_ACT_ERRNO (EACCES), SCMP_SYS (socket));

  EXPORT_FILTER (argv[1], seccomp_export_bpf);
  EXPORT_FILTER (argv[2], seccomp_export_pfc);
}
