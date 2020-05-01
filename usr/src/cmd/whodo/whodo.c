/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2013 Gary Mills
 *
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2020 Joyent, Inc.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/*	  All Rights Reserved	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

/*
 * This is the new whodo command which takes advantage of
 * the /proc interface to gain access to the information
 * of all the processes currently on the system.
 *
 * Maintenance note:
 *
 * Much of this code is replicated in w.c.  If you're
 * fixing bugs here, then you should probably fix 'em there too.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <err.h>
#include <errno.h>
#include <sys/types.h>
#include <utmpx.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include <dirent.h>
#include <procfs.h>		/* /proc header file */
#include <sys/wait.h>
#include <locale.h>
#include <unistd.h>
#include <limits.h>
#include <priv_utils.h>

/*
 * Use the full lengths from utmpx for user and line.
 */
#define	NMAX	(sizeof (((struct utmpx *)0)->ut_user))
#define	LMAX	(sizeof (((struct utmpx *)0)->ut_line))

/* Print minimum field widths. */
#define	LOGIN_WIDTH	8
#define	LINE_WIDTH	8

#define	DIV60(t)	((t+30)/60)    /* x/60 rounded */

#ifdef ERR
#undef ERR
#endif
#define	ERR		(-1)

#define	DEVNAMELEN	14
#define	HSIZE		256		/* size of process hash table */
#define	PROCDIR		"/proc"
#define	INITPROCESS	(pid_t)1	/* init process pid */
#define	NONE		'n'		/* no state */
#define	RUNNING		'r'		/* runnable process */
#define	ZOMBIE		'z'		/* zombie process */
#define	VISITED		'v'		/* marked node as visited */

static uint_t	ndevs;			/* number of configured devices */
static uint_t	maxdev;			/* slots for configured devices */
#define	DNINCR	100
static struct devl {			/* device list   */
	char	dname[DEVNAMELEN];	/* device name   */
	dev_t	ddev;			/* device number */
} *devl;

struct uproc {
	pid_t	p_upid;			/* user process id */
	char	p_state;		/* numeric value of process state */
	dev_t	p_ttyd;			/* controlling tty of process */
	time_t	p_time;			/* ticks of user & system time */
	time_t	p_ctime;		/* ticks of child user & system time */
	int	p_igintr;		/* 1=ignores SIGQUIT and SIGINT */
	char	p_comm[PRARGSZ+1];	/* command */
	char	p_args[PRARGSZ+1];	/* command line arguments */
	struct uproc	*p_child,	/* first child pointer */
			*p_sibling,	/* sibling pointer */
			*p_pgrplink,	/* pgrp link */
			*p_link;	/* hash table chain pointer */
};

/*
 *	define	hash table for struct uproc
 *	Hash function uses process id
 *	and the size of the hash table(HSIZE)
 *	to determine process index into the table.
 */
static struct uproc	pr_htbl[HSIZE];

static struct	uproc	*findhash(pid_t);
static time_t	findidle(char *);
static void	clnarglist(char *);
static void	showproc(struct uproc *);
static void	showtotals(struct uproc *);
static void	calctotals(struct uproc *);
static char	*getty(dev_t);
static void	prttime(time_t, int);
static void	prtat(time_t *);

static int	priv_proc_open(const char *, int);
static int	priv_proc_openat(int, const char *, int);
static boolean_t do_proc_read(int, void *, size_t);

static char	*prog;
static int	header = 1;	/* true if -h flag: don't print heading */
static int	lflag = 0;	/* true if -l flag: w command format */
static char	*sel_user;	/* login of particular user selected */
static time_t	now;		/* current time of day */
static time_t	uptime;		/* time of last reboot & elapsed time since */
static int	nusers;		/* number of users logged in now */
static time_t	idle;		/* number of minutes user is idle */
static time_t	jobtime;	/* total cpu time visible */
static char	doing[520];	/* process attached to terminal */
static time_t	proctime;	/* cpu time of process in doing */
static int	empty;
static pid_t	curpid;

/*
 * Basic privs we never need and can drop. This is likely not exhaustive,
 * but should significantly reduce any potential attack surfaces.
 */
static const char *drop_privs[] = {
	PRIV_FILE_WRITE,
	PRIV_NET_ACCESS,
	PRIV_PROC_EXEC,
	PRIV_PROC_FORK,
	PRIV_FILE_LINK_ANY
};

#if SIGQUIT > SIGINT
#define	ACTSIZE	SIGQUIT
#else
#define	ACTSIZE	SIGINT
#endif

int
main(int argc, char *argv[])
{
	struct utmpx	*ut;
	struct utmpx	*utmpbegin;
	struct utmpx	*utmpend;
	struct utmpx	*utp;
	struct tm		*tm;
	struct uproc	*up, *parent, *pgrp;
	struct psinfo	info;
	struct sigaction actinfo[ACTSIZE];
	struct pstatus	statinfo;
	struct stat	sbuf;
	struct utsname	uts;
	DIR		*dirp;
	struct	dirent	*dp;
	char		pname[PATH_MAX];
	int		procfd, dirfd;
	int		i;
	int		days, hrs, mins;
	int		entries;
	priv_set_t	*pset;

	if (__init_suid_priv(PU_CLEARLIMITSET, PRIV_PROC_OWNER, NULL) != 0) {
		err(EXIT_FAILURE, "failed to enable privilege bracketing");
	}

	/*
	 * After setting up privilege bracketing, we can further reduce the
	 * privileges in use. The effective set is set to the basic set minus
	 * the privs in drop_privs. The permitted set is the effective set
	 * plus PRIV_PROC_OWNER (i.e. the privilege being bracketed).
	 */
	pset = priv_allocset();
	if (pset == NULL)
		err(EXIT_FAILURE, "priv_allocset failed");

	priv_basicset(pset);
	for (i = 0; i < ARRAY_SIZE(drop_privs); i++) {
		if (priv_delset(pset, drop_privs[i]) != 0) {
			err(EXIT_FAILURE,
			    "failed to remove %s privilege from privilege set",
			    drop_privs[i]);
		}
	}

	if (setppriv(PRIV_SET, PRIV_EFFECTIVE, pset) < 0)
		err(EXIT_FAILURE, "failed setting effective privilege set");

	if (priv_addset(pset, PRIV_PROC_OWNER) != 0) {
		err(EXIT_FAILURE,
		    "failed to add PRIV_PROC_OWNER privilege to privilege set");
	}

	if (setppriv(PRIV_SET, PRIV_PERMITTED, pset) < 0)
		err(EXIT_FAILURE, "failed to set permitted privilege set");

	/*
	 * Unfortunately, when run as root, privilege bracketing is a no-op,
	 * so we have to add PRIV_PROC_OWNER into our effective set for things
	 * to work.
	 */
	if (getuid() == 0 && setppriv(PRIV_SET, PRIV_EFFECTIVE, pset) < 0) {
		err(EXIT_FAILURE, "failed to set effective privilege set");
	}

	priv_freeset(pset);
	pset = NULL;

	(void) setlocale(LC_ALL, "");
#if !defined(TEXT_DOMAIN)
#define	TEXT_DOMAIN "SYS_TEST"
#endif
	(void) textdomain(TEXT_DOMAIN);

	prog = argv[0];

	while (argc > 1) {
		if (argv[1][0] == '-') {
			for (i = 1; argv[1][i]; i++) {
				switch (argv[1][i]) {

				case 'h':
					header = 0;
					break;

				case 'l':
					lflag++;
					break;

				default:
					(void) printf(gettext(
					    "usage: %s [ -hl ] [ user ]\n"),
					    prog);
					exit(1);
				}
			}
		} else {
			if (!isalnum(argv[1][0]) || argc > 2) {
				(void) printf(gettext(
				    "usage: %s [ -hl ] [ user ]\n"), prog);
				exit(1);
			} else
				sel_user = argv[1];
		}
		argc--; argv++;
	}

	/*
	 * read the UTMPX_FILE (contains information about
	 * each logged in user)
	 */
	if (stat(UTMPX_FILE, &sbuf) < 0)
		err(EXIT_FAILURE, gettext("stat error of %s"), UTMPX_FILE);

	entries = sbuf.st_size / sizeof (struct futmpx);

	if ((ut = calloc(entries, sizeof (struct utmpx))) == NULL)
		err(EXIT_FAILURE, gettext("calloc error of %s"), UTMPX_FILE);

	(void) utmpxname(UTMPX_FILE);

	utmpbegin = ut;
	utmpend = utmpbegin + entries;

	setutxent();
	while ((ut < utmpend) && ((utp = getutxent()) != NULL))
		(void) memcpy(ut++, utp, sizeof (*ut));
	endutxent();

	(void) time(&now);	/* get current time */

	if (header) {	/* print a header */
		if (lflag) {	/* w command format header */
			prtat(&now);
			for (ut = utmpbegin; ut < utmpend; ut++) {
				if (ut->ut_type == USER_PROCESS) {
					nusers++;
				} else if (ut->ut_type == BOOT_TIME) {
					uptime = now - ut->ut_xtime;
					uptime += 30;
					days = uptime / (60*60*24);
					uptime %= (60*60*24);
					hrs = uptime / (60*60);
					uptime %= (60*60);
					mins = uptime / 60;

					(void) printf(dcgettext(NULL,
					    "up %d day(s), %d hr(s), "
					    "%d min(s)", LC_TIME),
					    days, hrs, mins);
				}
			}

			ut = utmpbegin; /* rewind utmp data */
			(void) printf(dcgettext(NULL,
			    "  %d user(s)\n", LC_TIME), nusers);
			(void) printf(dcgettext(NULL, "User     tty      "
			    "login@         idle    JCPU    PCPU what\n",
			    LC_TIME));
		} else {	/* standard whodo header */
			char date_buf[100];

			/*
			 * print current time and date
			 */
			(void) strftime(date_buf, sizeof (date_buf),
			    "%c", localtime(&now));
			(void) printf("%s\n", date_buf);

			/*
			 * print system name
			 */
			(void) uname(&uts);
			(void) printf("%s\n", uts.nodename);
		}
	}

	/*
	 * loop through /proc, reading info about each process
	 * and build the parent/child tree
	 */
	if ((dirp = opendir(PROCDIR)) == NULL)
		err(EXIT_FAILURE, gettext("could not open %s"), PROCDIR);

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.')
			continue;

		if (snprintf(pname, sizeof (pname), "%s/%s", PROCDIR,
		    dp->d_name) > sizeof (pname))
			continue;

		dirfd = priv_proc_open(pname, O_RDONLY);

		if (dirfd < 0)
			continue;

		procfd = priv_proc_openat(dirfd, "psinfo", O_RDONLY);
		if (procfd < 0) {
			(void) close(dirfd);
			continue;
		}

		if (!do_proc_read(procfd, &info, sizeof (info))) {
			warn(gettext("read() failed on %s"), pname);
			(void) close(dirfd);
			continue;
		}
		(void) close(procfd);

		up = findhash(info.pr_pid);
		up->p_ttyd = info.pr_ttydev;
		up->p_state = (info.pr_nlwp == 0? ZOMBIE : RUNNING);
		up->p_time = 0;
		up->p_ctime = 0;
		up->p_igintr = 0;
		(void) strlcpy(up->p_comm, info.pr_fname,
		    sizeof (up->p_comm));
		up->p_args[0] = 0;

		if (up->p_state != NONE && up->p_state != ZOMBIE) {
			procfd = priv_proc_openat(dirfd, "status", O_RDONLY);
			if (procfd < 0) {
				(void) close(dirfd);
				continue;
			}

			if (!do_proc_read(procfd, &statinfo,
			    sizeof (statinfo))) {
				warn(gettext("read() failed on %s/status"),
				    pname);

				(void) close(procfd);
				(void) close(dirfd);
				continue;
			}
			(void) close(procfd);

			up->p_time = statinfo.pr_utime.tv_sec +
			    statinfo.pr_stime.tv_sec;
			up->p_ctime = statinfo.pr_cutime.tv_sec +
			    statinfo.pr_cstime.tv_sec;

			procfd = priv_proc_openat(dirfd, "sigact", O_RDONLY);
			if (procfd < 0) {
				(void) close(dirfd);
				continue;
			}

			if (!do_proc_read(procfd, actinfo, sizeof (actinfo))) {
				warn(gettext("read() failed on %s/sigact"),
				    pname);

				(void) close(procfd);
				(void) close(dirfd);
				continue;
			}
			(void) close(procfd);
			(void) close(dirfd);

			up->p_igintr =
			    actinfo[SIGINT-1].sa_handler == SIG_IGN &&
			    actinfo[SIGQUIT-1].sa_handler == SIG_IGN;

			up->p_args[0] = 0;

			/*
			 * Process args if there's a chance we'll print it.
			 */
			if (lflag) { /* w command needs args */
				clnarglist(info.pr_psargs);
				(void) strlcpy(up->p_args, info.pr_psargs,
				    sizeof (up->p_args));
				if (up->p_args[0] == 0 ||
				    up->p_args[0] == '-' &&
				    up->p_args[1] <= ' ' ||
				    up->p_args[0] == '?') {
					(void) strlcat(up->p_args, " (",
					    sizeof (up->p_args));
					(void) strlcat(up->p_args, up->p_comm,
					    sizeof (up->p_args));
					(void) strlcat(up->p_args, ")",
					    sizeof (up->p_args));
				}
			}

		}

		/*
		 * link pgrp together in case parents go away
		 * Pgrp chain is a single linked list originating
		 * from the pgrp leader to its group member.
		 */
		if (info.pr_pgid != info.pr_pid) {	/* not pgrp leader */
			pgrp = findhash(info.pr_pgid);
			up->p_pgrplink = pgrp->p_pgrplink;
			pgrp->p_pgrplink = up;
		}
		parent = findhash(info.pr_ppid);

		/* if this is the new member, link it in */
		if (parent->p_upid != INITPROCESS) {
			if (parent->p_child) {
				up->p_sibling = parent->p_child;
				up->p_child = 0;
			}
			parent->p_child = up;
		}

	}

	/* revert to non-privileged user */
	__priv_relinquish();
	if (getuid() == 0) {
		/*
		 * Since the privilege bracketing functions are effectively
		 * no-ops when running as root, we must explicitly
		 * relinquish PRIV_PROC_OWNER ourselves.
		 */
		pset = priv_allocset();
		if (pset == NULL) {
			err(EXIT_FAILURE,
			    gettext("failed to allocate privilege set"));
		}

		priv_emptyset(pset);

		if (priv_addset(pset, PRIV_PROC_OWNER) != 0) {
			err(EXIT_FAILURE, gettext("failed to add "
			    "PRIV_PROC_OWNER to privilege set"));
		}

		if (setppriv(PRIV_OFF, PRIV_PERMITTED, pset) != 0) {
			err(EXIT_FAILURE,
			    gettext("failed to set permitted privilege set"));
		}

		priv_freeset(pset);
		pset = NULL;
	}

	(void) closedir(dirp);
	(void) time(&now);	/* get current time */

	/*
	 * loop through utmpx file, printing process info
	 * about each logged in user
	 */
	for (ut = utmpbegin; ut < utmpend; ut++) {
		time_t tim;

		if (ut->ut_type != USER_PROCESS)
			continue;
		if (sel_user && strncmp(ut->ut_name, sel_user, NMAX) != 0)
			continue;	/* we're looking for somebody else */
		if (lflag) {	/* -l flag format (w command) */
			/* print login name of the user */
			(void) printf("%-*.*s ", LOGIN_WIDTH, (int)NMAX,
			    ut->ut_name);

			/* print tty user is on */
			(void) printf("%-*.*s ", LINE_WIDTH, (int)LMAX,
			    ut->ut_line);

			/* print when the user logged in */
			tim = ut->ut_xtime;
			(void) prtat(&tim);

			/* print idle time */
			idle = findidle(ut->ut_line);
			prttime(idle, 8);
			showtotals(findhash((pid_t)ut->ut_pid));
		} else {	/* standard whodo format */
			tim = ut->ut_xtime;
			tm = localtime(&tim);
			(void) printf("\n%-*.*s %-*.*s %2.1d:%2.2d\n",
			    LINE_WIDTH, (int)LMAX, ut->ut_line,
			    LOGIN_WIDTH, (int)NMAX, ut->ut_name, tm->tm_hour,
			    tm->tm_min);
			showproc(findhash((pid_t)ut->ut_pid));
		}
	}

	return (0);
}

/*
 * Used for standard whodo format.
 * This is the recursive routine descending the process
 * tree starting from the given process pointer(up).
 * It used depth-first search strategy and also marked
 * each node as printed as it traversed down the tree.
 */
static void
showproc(struct uproc *up)
{
	struct	uproc	*zp;

	if (up->p_state == VISITED) /* we already been here */
		return;
	/* print the data for this process */
	if (up->p_state == ZOMBIE)
		(void) printf("    %-*.*s %5d %4.1ld:%2.2ld %s\n",
		    LINE_WIDTH, (int)LMAX, "  ?", (int)up->p_upid, 0L, 0L,
		    "<defunct>");
	else if (up->p_state != NONE) {
		(void) printf("    %-*.*s %5d %4.1ld:%2.2ld %s\n",
		    LINE_WIDTH, (int)LMAX, getty(up->p_ttyd), (int)up->p_upid,
		    up->p_time / 60L, up->p_time % 60L,
		    up->p_comm);
	}
	up->p_state = VISITED;

	/* descend for its children */
	if (up->p_child) {
		showproc(up->p_child);
		for (zp = up->p_child->p_sibling; zp; zp = zp->p_sibling) {
			showproc(zp);
		}
	}

	/* print the pgrp relation */
	if (up->p_pgrplink)
		showproc(up->p_pgrplink);
}


/*
 * Used for -l flag (w command) format.
 * Prints the CPU time for all processes & children,
 * and the cpu time for interesting process,
 * and what the user is doing.
 */
static void
showtotals(struct uproc *up)
{
	jobtime = 0;
	proctime = 0;
	empty = 1;
	curpid = -1;

	/* default act: normally never prints */
	(void) strlcpy(doing, "-", sizeof (doing));
	calctotals(up);

	/* print CPU time for all processes & children */
	/* and need to convert clock ticks to seconds first */
	prttime((time_t)jobtime, 8);

	/* print cpu time for interesting process */
	/* and need to convert clock ticks to seconds first */
	prttime((time_t)proctime, 8);

	/* what user is doing, current process */
	(void) printf("%-.32s\n", doing);
}

/*
 *  Used for -l flag (w command) format.
 *  This recursive routine descends the process
 *  tree starting from the given process pointer(up).
 *  It used depth-first search strategy and also marked
 *  each node as visited as it traversed down the tree.
 *  It calculates the process time for all processes &
 *  children.  It also finds the "interesting" process
 *  and determines its cpu time and command.
 */
static void
calctotals(struct uproc *up)
{
	struct uproc	*zp;

	if (up->p_state == VISITED)
		return;
	up->p_state = VISITED;
	if (up->p_state == NONE || up->p_state == ZOMBIE)
		return;
	jobtime += up->p_time + up->p_ctime;
	proctime += up->p_time;

	if (empty && !up->p_igintr) {
		empty = 0;
		curpid = -1;
	}

	if (up->p_upid > curpid && (!up->p_igintr || empty)) {
		curpid = up->p_upid;
		(void) strlcpy(doing, up->p_args, sizeof (doing));
	}

	/* descend for its children */
	if (up->p_child) {
		calctotals(up->p_child);
		for (zp = up->p_child->p_sibling; zp; zp = zp->p_sibling)
			calctotals(zp);
	}
}

static char *
devadd(const char *name, dev_t ddev)
{
	struct devl *dp;
	int leng, start, i;

	if (ndevs == maxdev) {
		uint_t newdev;

		newdev = maxdev + DNINCR;
		if (newdev < DNINCR)
			errx(EXIT_FAILURE, gettext("devadd overflow"));

		dp = recallocarray(devl, maxdev, newdev, sizeof (struct devl));
		if (dp == NULL)
			err(EXIT_FAILURE, gettext("out of memory!"));
		maxdev = newdev;
		devl = dp;
	}
	dp = &devl[ndevs++];

	dp->ddev = ddev;
	if (name == NULL) {
		(void) strlcpy(dp->dname, "  ?  ", sizeof (dp->dname));
		return (dp->dname);
	}

	leng = strlen(name);
	if (leng < DEVNAMELEN + 4) {
		/* strip off "/dev/" */
		(void) strlcpy(dp->dname, &name[5], sizeof (dp->dname));
	} else {
		/* strip enough off the front to fit */
		start = leng - DEVNAMELEN - 1;

		for (i = start; i < leng && name[i] != '/'; i++)
				;
		if (i == leng)
			(void) strlcpy(dp->dname, &name[start], DEVNAMELEN);
		else
			(void) strlcpy(dp->dname, &name[i+1], DEVNAMELEN);
	}
	return (dp->dname);
}

static char *
devlookup(dev_t ddev)
{
	struct devl *dp;
	int i;

	for (dp = devl, i = 0; i < ndevs; dp++, i++) {
		if (dp->ddev == ddev)
			return (dp->dname);
	}
	return (NULL);
}

/*
 * This routine gives back a corresponding device name
 * from the device number given.
 */
static char *
getty(dev_t dev)
{
	extern char *_ttyname_dev(dev_t, char *, size_t);
	char devname[TTYNAME_MAX];
	char *retval;

	if (dev == PRNODEV)
		return ("  ?  ");

	if ((retval = devlookup(dev)) != NULL)
		return (retval);

	retval = _ttyname_dev(dev, devname, sizeof (devname));
	return (devadd(retval, dev));
}

/*
 * Findhash  finds the appropriate entry in the process
 * hash table (pr_htbl) for the given pid in case that
 * pid exists on the hash chain. It returns back a pointer
 * to that uproc structure. If this is a new pid, it allocates
 * a new node, initializes it, links it into the chain (after
 * head) and returns a structure pointer.
 */
static struct uproc *
findhash(pid_t pid)
{
	struct uproc *up, *tp;

	tp = up = &pr_htbl[(int)pid % HSIZE];
	if (up->p_upid == 0) {			/* empty slot */
		up->p_upid = pid;
		up->p_state = NONE;
		up->p_child = up->p_sibling = up->p_pgrplink = up->p_link = 0;
		return (up);
	}
	if (up->p_upid == pid) {		/* found in hash table */
		return (up);
	}
	for (tp = up->p_link; tp; tp = tp->p_link) {	/* follow chain */
		if (tp->p_upid == pid) {
			return (tp);
		}
	}
	tp = malloc(sizeof (*tp));		/* add new node */
	if (tp == NULL)
		err(EXIT_FAILURE, gettext("out of memory!"));

	(void) memset((char *)tp, 0, sizeof (*tp));
	tp->p_upid = pid;
	tp->p_state = NONE;
	tp->p_child = tp->p_sibling = tp->p_pgrplink = (pid_t)0;
	tp->p_link = up->p_link;		/* insert after head */
	up->p_link = tp;
	return (tp);
}

#define	HR	(60 * 60)
#define	DAY	(24 * HR)
#define	MON	(30 * DAY)
#define	PRINTF(a)	(void) printf a

/*
 * Prttime prints an elapsed time in hours, minutes, or seconds,
 * right-justified with the rightmost column always blank.
 * The second argument is the minimum field width.
 */
static void
prttime(time_t tim, int width)
{
	char value[36];

	if (tim >= 36 * 60) {
		(void) snprintf(value, sizeof (value), "%d:%02d:%02d",
		    (int)tim / HR, (int)(tim % HR) / 60, (int)tim % 60);
	} else if (tim >= 60) {
		(void) snprintf(value, sizeof (value), "%d:%02d",
		    (int)tim / 60, (int)tim % 60);
	} else if (tim > 0) {
		(void) snprintf(value, sizeof (value), "%d", (int)tim);
	} else {
		(void) strcpy(value, "0");
	}
	width = (width > 2) ? width - 1 : 1;
	PRINTF(("%*s ", width, value));
}

/*
 * Prints the ISO date or time given a pointer to a time of day,
 * left-justfied in a 12-character expanding field with the
 * rightmost column always blank.
 * Includes a dcgettext() override in case a message catalog is needed.
 */
static void
prtat(time_t *time)
{
	struct tm	*p;

	p = localtime(time);
	if (now - *time <= 18 * HR) {
		char timestr[50];

		(void) strftime(timestr, sizeof (timestr),
		    dcgettext(NULL, "%T", LC_TIME), p);
		PRINTF(("%-11s ", timestr));
	} else if (now - *time <= 7 * DAY) {
		char weekdaytime[20];

		(void) strftime(weekdaytime, sizeof (weekdaytime),
		    dcgettext(NULL, "%a %H:%M", LC_TIME), p);
		PRINTF(("%-11s ", weekdaytime));
	} else {
		char monthtime[20];

		(void) strftime(monthtime, sizeof (monthtime),
		    dcgettext(NULL, "%F", LC_TIME), p);
		PRINTF(("%-11s ", monthtime));
	}
}

/*
 * find & return number of minutes current tty has been idle
 */
static time_t
findidle(char *devname)
{
	struct stat stbuf;
	time_t lastaction, diff;
	char ttyname[64];

	(void) strlcpy(ttyname, "/dev/", sizeof (ttyname));
	(void) strlcat(ttyname, devname, sizeof (ttyname));
	if (stat(ttyname, &stbuf) != -1) {
		lastaction = stbuf.st_atime;
		diff = now - lastaction;
		diff = DIV60(diff);
		if (diff < 0)
			diff = 0;
	} else
		diff = 0;
	return (diff);
}

/*
 * given a pointer to the argument string clean out unsavory characters.
 */
static void
clnarglist(char *arglist)
{
	char	*c;
	int	err = 0;

	/* get rid of unsavory characters */
	for (c = arglist; *c == '\0'; c++) {
		if ((*c < ' ') || (*c > 0176)) {
			if (err++ > 5) {
				*arglist = '\0';
				break;
			}
			*c = '?';
		}
	}
}

static int
priv_proc_open(const char *path, int oflag)
{
	int fd, errsave = 0;

	if (__priv_bracket(PRIV_ON) != 0)
		err(EXIT_FAILURE, gettext("privilege bracketing failed"));

	do {
		fd = open(path, oflag);
		if (fd < 0)
			errsave = errno;
	} while (fd < 0 && errno == EAGAIN);

	if (__priv_bracket(PRIV_OFF) != 0)
		err(EXIT_FAILURE, gettext("privilege bracketing failed"));

	if (fd < 0)
		errno = errsave;

	return (fd);
}

static int
priv_proc_openat(int dfd, const char *path, int mode)
{
	int fd, errsave = 0;

	if (__priv_bracket(PRIV_ON) != 0)
		err(EXIT_FAILURE, gettext("privilege bracketing failed"));

	do {
		fd = openat(dfd, path, mode);
		if (fd < 0)
			errsave = errno;
	} while (fd < 0 && errno == EAGAIN);

	if (__priv_bracket(PRIV_OFF) != 0)
		err(EXIT_FAILURE, gettext("privilege bracketing failed"));

	if (fd < 0)
		errno = errsave;

	return (fd);
}

static boolean_t
do_proc_read(int fd, void *buf, size_t bufsize)
{
	ssize_t n;

	do {
		n = pread(fd, buf, bufsize, 0);
		if (n == bufsize)
			return (B_TRUE);
		/*
		 * Retry on a partial read or EAGAIN, otherwise fail
		 */
	} while (n >= 0 || errno == EAGAIN);

	return (B_FALSE);
}
