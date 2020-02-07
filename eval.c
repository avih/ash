/*	$NetBSD: eval.c,v 1.178 2020/02/04 16:06:59 kre Exp $	*/

/*-
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#ifndef lint
#if 0
static char sccsid[] = "@(#)eval.c	8.9 (Berkeley) 6/8/95";
#else
__RCSID("$NetBSD: eval.c,v 1.178 2020/02/04 16:06:59 kre Exp $");
#endif
#endif /* not lint */

#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysctl.h>

/*
 * Evaluate a command.
 */

#include "shell.h"
#include "nodes.h"
#include "syntax.h"
#include "expand.h"
#include "parser.h"
#include "jobs.h"
#include "eval.h"
#include "builtins.h"
#include "options.h"
#include "exec.h"
#include "redir.h"
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "show.h"
#include "mystring.h"
#include "main.h"
#ifndef SMALL
#include "nodenames.h"
#include "myhistedit.h"
#endif


STATIC struct skipsave s_k_i_p;
#define	evalskip	(s_k_i_p.state)
#define	skipcount	(s_k_i_p.count)

STATIC int loopnest;		/* current loop nesting level */
STATIC int funcnest;		/* depth of function calls */
STATIC int builtin_flags;	/* evalcommand flags for builtins */
/*
 * Base function nesting level inside a dot command.  Set to 0 initially
 * and to (funcnest + 1) before every dot command to enable 
 *   1) detection of being in a file sourced by a dot command and
 *   2) counting of function nesting in that file for the implementation
 *      of the return command.
 * The value is reset to its previous value after the dot command.
 */
STATIC int dot_funcnest;


const char *commandname;
struct strlist *cmdenviron;
int exitstatus;			/* exit status of last command */
int back_exitstatus;		/* exit status of backquoted command */


STATIC void evalloop(union node *, int);
STATIC void evalfor(union node *, int);
STATIC void evalcase(union node *, int);
STATIC void evalsubshell(union node *, int);
STATIC void expredir(union node *);
STATIC void evalredir(union node *, int);
STATIC void evalpipe(union node *);
STATIC void evalcommand(union node *, int, struct backcmd *);
STATIC void prehash(union node *);

STATIC char *find_dot_file(char *);

/*
 * Called to reset things after an exception.
 */

#ifdef mkinit
INCLUDE "eval.h"

RESET {
	reset_eval();
}

SHELLPROC {
	exitstatus = 0;
}
#endif

void
reset_eval(void)
{
	evalskip = SKIPNONE;
	dot_funcnest = 0;
	loopnest = 0;
	funcnest = 0;
}

static int
sh_pipe(int fds[2])
{
	int nfd;

	if (pipe(fds))
		return -1;

	if (fds[0] < 3) {
		nfd = fcntl(fds[0], F_DUPFD, 3);
		if (nfd != -1) {
			close(fds[0]);
			fds[0] = nfd;
		}
	}

	if (fds[1] < 3) {
		nfd = fcntl(fds[1], F_DUPFD, 3);
		if (nfd != -1) {
			close(fds[1]);
			fds[1] = nfd;
		}
	}
	return 0;
}


/*
 * The eval commmand.
 */

int
evalcmd(int argc, char **argv)
{
	char *p;
	char *concat;
	char **ap;

	if (argc > 1) {
		p = argv[1];
		if (argc > 2) {
			STARTSTACKSTR(concat);
			ap = argv + 2;
			for (;;) {
				while (*p)
					STPUTC(*p++, concat);
				if ((p = *ap++) == NULL)
					break;
				STPUTC(' ', concat);
			}
			STPUTC('\0', concat);
			p = grabstackstr(concat);
		}
		evalstring(p, builtin_flags & EV_TESTED);
	} else
		exitstatus = 0;
	return exitstatus;
}


/*
 * Execute a command or commands contained in a string.
 */

void
evalstring(char *s, int flag)
{
	union node *n;
	struct stackmark smark;
	int last;
	int any;

	last = flag & EV_EXIT;
	flag &= ~EV_EXIT;

	setstackmark(&smark);
	setinputstring(s, 1, line_number);

	any = 0;	/* to determine if exitstatus will have been set */
	while ((n = parsecmd(0)) != NEOF) {
		XTRACE(DBG_EVAL, ("evalstring: "), showtree(n));
		if (n && nflag == 0) {
			if (last && at_eof())
				evaltree(n, flag | EV_EXIT);
			else
				evaltree(n, flag);
			any = 1;
			if (evalskip)
				break;
		}
		rststackmark(&smark);
	}
	popfile();
	popstackmark(&smark);
	if (!any)
		exitstatus = 0;
	if (last)
		exraise(EXEXIT);
}



/*
 * Evaluate a parse tree.  The value is left in the global variable
 * exitstatus.
 */

void
evaltree(union node *n, int flags)
{
	bool do_etest;
	int sflags = flags & ~EV_EXIT;
	union node *next;
	struct stackmark smark;

	do_etest = false;
	if (n == NULL || nflag) {
		VTRACE(DBG_EVAL, ("evaltree(%s) called\n",
		    n == NULL ? "NULL" : "-n"));
		if (nflag == 0)
			exitstatus = 0;
		goto out2;
	}

	setstackmark(&smark);
	do {
#ifndef SMALL
		displayhist = 1; /* show history substitutions done with fc */
#endif
		next = NULL;
		CTRACE(DBG_EVAL, ("pid %d, evaltree(%p: %s(%d), %#x) called\n",
		    getpid(), n, NODETYPENAME(n->type), n->type, flags));
		if (n->type != NCMD && traps_invalid)
			free_traps();
		switch (n->type) {
		case NSEMI:
			evaltree(n->nbinary.ch1, sflags);
			if (nflag || evalskip)
				goto out1;
			next = n->nbinary.ch2;
			break;
		case NAND:
			evaltree(n->nbinary.ch1, EV_TESTED);
			if (nflag || evalskip || exitstatus != 0)
				goto out1;
			next = n->nbinary.ch2;
			break;
		case NOR:
			evaltree(n->nbinary.ch1, EV_TESTED);
			if (nflag || evalskip || exitstatus == 0)
				goto out1;
			next = n->nbinary.ch2;
			break;
		case NREDIR:
			evalredir(n, flags);
			break;
		case NSUBSHELL:
			evalsubshell(n, flags);
			do_etest = !(flags & EV_TESTED);
			break;
		case NBACKGND:
			evalsubshell(n, flags);
			break;
		case NIF: {
			evaltree(n->nif.test, EV_TESTED);
			if (nflag || evalskip)
				goto out1;
			if (exitstatus == 0)
				next = n->nif.ifpart;
			else if (n->nif.elsepart)
				next = n->nif.elsepart;
			else
				exitstatus = 0;
			break;
		}
		case NWHILE:
		case NUNTIL:
			evalloop(n, sflags);
			break;
		case NFOR:
			evalfor(n, sflags);
			break;
		case NCASE:
			evalcase(n, sflags);
			break;
		case NDEFUN:
			CTRACE(DBG_EVAL, ("Defining fn %s @%d%s\n",
			    n->narg.text, n->narg.lineno,
			    fnline1 ? " LINENO=1" : ""));
			defun(n->narg.text, n->narg.next, n->narg.lineno);
			exitstatus = 0;
			break;
		case NNOT:
			evaltree(n->nnot.com, EV_TESTED);
			exitstatus = !exitstatus;
			break;
		case NDNOT:
			evaltree(n->nnot.com, EV_TESTED);
			if (exitstatus != 0)
				exitstatus = 1;
			break;
		case NPIPE:
			evalpipe(n);
			do_etest = !(flags & EV_TESTED);
			break;
		case NCMD:
			evalcommand(n, flags, NULL);
			do_etest = !(flags & EV_TESTED);
			break;
		default:
#ifdef NODETYPENAME
			out1fmt("Node type = %d(%s)\n",
				n->type, NODETYPENAME(n->type));
#else
			out1fmt("Node type = %d\n", n->type);
#endif
			flushout(&output);
			break;
		}
		n = next;
		rststackmark(&smark);
	} while(n != NULL);
 out1:
	popstackmark(&smark);
 out2:
	if (pendingsigs)
		dotrap();
	if (eflag && exitstatus != 0 && do_etest)
		exitshell(exitstatus);
	if (flags & EV_EXIT)
		exraise(EXEXIT);
}


STATIC void
evalloop(union node *n, int flags)
{
	int status;

	loopnest++;
	status = 0;

	CTRACE(DBG_EVAL,  ("evalloop %s:", NODETYPENAME(n->type)));
	VXTRACE(DBG_EVAL, (" "), showtree(n->nbinary.ch1));
	VXTRACE(DBG_EVAL, ("evalloop    do: "), showtree(n->nbinary.ch2));
	VTRACE(DBG_EVAL,  ("evalloop  done\n"));
	CTRACE(DBG_EVAL,  ("\n"));

	for (;;) {
		evaltree(n->nbinary.ch1, EV_TESTED);
		if (nflag)
			break;
		if (evalskip) {
 skipping:		if (evalskip == SKIPCONT && --skipcount <= 0) {
				evalskip = SKIPNONE;
				continue;
			}
			if (evalskip == SKIPBREAK && --skipcount <= 0)
				evalskip = SKIPNONE;
			if (evalskip == SKIPFUNC || evalskip == SKIPFILE)
				status = exitstatus;
			break;
		}
		if (n->type == NWHILE) {
			if (exitstatus != 0)
				break;
		} else {
			if (exitstatus == 0)
				break;
		}
		evaltree(n->nbinary.ch2, flags & EV_TESTED);
		status = exitstatus;
		if (evalskip)
			goto skipping;
	}
	loopnest--;
	exitstatus = status;
}



STATIC void
evalfor(union node *n, int flags)
{
	struct arglist arglist;
	union node *argp;
	struct strlist *sp;
	struct stackmark smark;
	int status;

	status = nflag ? exitstatus : 0;

	setstackmark(&smark);
	arglist.lastp = &arglist.list;
	for (argp = n->nfor.args ; argp ; argp = argp->narg.next) {
		expandarg(argp, &arglist, EXP_FULL | EXP_TILDE);
		if (evalskip)
			goto out;
	}
	*arglist.lastp = NULL;

	loopnest++;
	for (sp = arglist.list ; sp ; sp = sp->next) {
		if (xflag) {
			outxstr(expandstr(ps4val(), line_number));
			outxstr("for ");
			outxstr(n->nfor.var);
			outxc('=');
			outxshstr(sp->text);
			outxc('\n');
			flushout(outx);
		}

		setvar(n->nfor.var, sp->text, 0);
		evaltree(n->nfor.body, flags & EV_TESTED);
		status = exitstatus;
		if (nflag)
			break;
		if (evalskip) {
			if (evalskip == SKIPCONT && --skipcount <= 0) {
				evalskip = SKIPNONE;
				continue;
			}
			if (evalskip == SKIPBREAK && --skipcount <= 0)
				evalskip = SKIPNONE;
			break;
		}
	}
	loopnest--;
	exitstatus = status;
 out:
	popstackmark(&smark);
}



STATIC void
evalcase(union node *n, int flags)
{
	union node *cp, *ncp;
	union node *patp;
	struct arglist arglist;
	struct stackmark smark;
	int status = 0;

	setstackmark(&smark);
	arglist.lastp = &arglist.list;
	line_number = n->ncase.lineno;
	expandarg(n->ncase.expr, &arglist, EXP_TILDE);
	for (cp = n->ncase.cases; cp && evalskip == 0; cp = cp->nclist.next) {
		for (patp = cp->nclist.pattern; patp; patp = patp->narg.next) {
			line_number = patp->narg.lineno;
			if (casematch(patp, arglist.list->text)) {
				while (cp != NULL && evalskip == 0 &&
				    nflag == 0) {
					if (cp->type == NCLISTCONT)
						ncp = cp->nclist.next;
					else
						ncp = NULL;
					line_number = cp->nclist.lineno;
					evaltree(cp->nclist.body, flags);
					status = exitstatus;
					cp = ncp;
				}
				goto out;
			}
		}
	}
 out:
	exitstatus = status;
	popstackmark(&smark);
}



/*
 * Kick off a subshell to evaluate a tree.
 */

STATIC void
evalsubshell(union node *n, int flags)
{
	struct job *jp= NULL;
	int backgnd = (n->type == NBACKGND);

	expredir(n->nredir.redirect);
	if (xflag && n->nredir.redirect) {
		union node *rn;

		outxstr(expandstr(ps4val(), line_number));
		outxstr("using redirections:");
		for (rn = n->nredir.redirect; rn; rn = rn->nfile.next)
			(void) outredir(outx, rn, ' ');
		outxstr(" do subshell ("/*)*/);
		if (backgnd)
			outxstr(/*(*/") &");
		outxc('\n');
		flushout(outx);
	}
	INTOFF;
	if ((!backgnd && flags & EV_EXIT && !have_traps()) ||
	    forkshell(jp = makejob(n, 1), n, backgnd?FORK_BG:FORK_FG) == 0) {
		if (backgnd)
			flags &=~ EV_TESTED;
		INTON;
		redirect(n->nredir.redirect, REDIR_KEEP);
		evaltree(n->nredir.n, flags | EV_EXIT);   /* never returns */
	} else if (backgnd)
		exitstatus = 0;
	else
		exitstatus = waitforjob(jp);
	INTON;

	if (!backgnd && xflag && n->nredir.redirect) {
		outxstr(expandstr(ps4val(), line_number));
		outxstr(/*(*/") done subshell\n");
		flushout(outx);
	}
}



/*
 * Compute the names of the files in a redirection list.
 */

STATIC void
expredir(union node *n)
{
	union node *redir;

	for (redir = n ; redir ; redir = redir->nfile.next) {
		struct arglist fn;

		fn.lastp = &fn.list;
		switch (redir->type) {
		case NFROMTO:
		case NFROM:
		case NTO:
		case NCLOBBER:
		case NAPPEND:
			expandarg(redir->nfile.fname, &fn, EXP_TILDE | EXP_REDIR);
			redir->nfile.expfname = fn.list->text;
			break;
		case NFROMFD:
		case NTOFD:
			if (redir->ndup.vname) {
				expandarg(redir->ndup.vname, &fn, EXP_TILDE | EXP_REDIR);
				fixredir(redir, fn.list->text, 1);
			}
			break;
		}
	}
}

/*
 * Perform redirections for a compound command, and then do it (and restore)
 */
STATIC void
evalredir(union node *n, int flags)
{
	struct jmploc jmploc;
	struct jmploc * const savehandler = handler;
	volatile int in_redirect = 1;
	const char * volatile PS4 = NULL;

	expredir(n->nredir.redirect);

	if (xflag && n->nredir.redirect) {
		union node *rn;

		outxstr(PS4 = expandstr(ps4val(), line_number));
		outxstr("using redirections:");
		for (rn = n->nredir.redirect; rn != NULL; rn = rn->nfile.next)
			(void) outredir(outx, rn, ' ');
		outxstr(" do {\n");	/* } */
		flushout(outx);
	}

	if (setjmp(jmploc.loc)) {
		int e;

		handler = savehandler;
		e = exception;
		popredir();
		if (PS4 != NULL) {
			outxstr(PS4);
			/* { */ outxstr("} failed\n");
			flushout(outx);
		}
		if (e == EXERROR || e == EXEXEC) {
			if (in_redirect) {
				exitstatus = 2;
				return;
			}
		}
		longjmp(handler->loc, 1);
	} else {
		INTOFF;
		handler = &jmploc;
		redirect(n->nredir.redirect, REDIR_PUSH | REDIR_KEEP);
		in_redirect = 0;
		INTON;
		evaltree(n->nredir.n, flags);
	}
	INTOFF;
	handler = savehandler;
	popredir();
	INTON;

	if (PS4 != NULL) {
		outxstr(PS4);
		/* { */ outxstr("} done\n");
		flushout(outx);
	}
}


/*
 * Evaluate a pipeline.  All the processes in the pipeline are children
 * of the process creating the pipeline.  (This differs from some versions
 * of the shell, which make the last process in a pipeline the parent
 * of all the rest.)
 */

STATIC void
evalpipe(union node *n)
{
	struct job *jp;
	struct nodelist *lp;
	int pipelen;
	int prevfd;
	int pip[2];

	CTRACE(DBG_EVAL, ("evalpipe(%p) called\n", n));
	pipelen = 0;
	for (lp = n->npipe.cmdlist ; lp ; lp = lp->next)
		pipelen++;
	INTOFF;
	jp = makejob(n, pipelen);
	prevfd = -1;
	for (lp = n->npipe.cmdlist ; lp ; lp = lp->next) {
		prehash(lp->n);
		pip[1] = -1;
		if (lp->next) {
			if (sh_pipe(pip) < 0) {
				if (prevfd >= 0)
					close(prevfd);
				error("Pipe call failed: %s", strerror(errno));
			}
		}
		if (forkshell(jp, lp->n,
		    n->npipe.backgnd ? FORK_BG : FORK_FG) == 0) {
			INTON;
			if (prevfd > 0)
				movefd(prevfd, 0);
			if (pip[1] >= 0) {
				close(pip[0]);
				movefd(pip[1], 1);
			}
			evaltree(lp->n, EV_EXIT);
		}
		if (prevfd >= 0)
			close(prevfd);
		prevfd = pip[0];
		close(pip[1]);
	}
	if (n->npipe.backgnd == 0) {
		exitstatus = waitforjob(jp);
		CTRACE(DBG_EVAL, ("evalpipe:  job done exit status %d\n",
		    exitstatus));
	} else
		exitstatus = 0;
	INTON;
}



/*
 * Execute a command inside back quotes.  If it's a builtin command, we
 * want to save its output in a block obtained from malloc.  Otherwise
 * we fork off a subprocess and get the output of the command via a pipe.
 * Should be called with interrupts off.
 */

void
evalbackcmd(union node *n, struct backcmd *result)
{
	int pip[2];
	struct job *jp;
	struct stackmark smark;		/* unnecessary (because we fork) */

	result->fd = -1;
	result->buf = NULL;
	result->nleft = 0;
	result->jp = NULL;

	if (nflag || n == NULL)
		goto out;

	setstackmark(&smark);

#ifdef notyet
	/*
	 * For now we disable executing builtins in the same
	 * context as the shell, because we are not keeping
	 * enough state to recover from changes that are
	 * supposed only to affect subshells. eg. echo "`cd /`"
	 */
	if (n->type == NCMD) {
		exitstatus = oexitstatus;	/* XXX o... no longer exists */
		evalcommand(n, EV_BACKCMD, result);
	} else
#endif
	{
		INTOFF;
		if (sh_pipe(pip) < 0)
			error("Pipe call failed");
		jp = makejob(n, 1);
		if (forkshell(jp, n, FORK_NOJOB) == 0) {
			FORCEINTON;
			close(pip[0]);
			movefd(pip[1], 1);
			eflag = 0;
			evaltree(n, EV_EXIT);
			/* NOTREACHED */
		}
		close(pip[1]);
		result->fd = pip[0];
		result->jp = jp;
		INTON;
	}
	popstackmark(&smark);
 out:
	CTRACE(DBG_EVAL, ("evalbackcmd done: fd=%d buf=0x%x nleft=%d jp=0x%x\n",
		result->fd, result->buf, result->nleft, result->jp));
}

const char *
syspath(void)
{
	static char *sys_path = NULL;
	static char def_path[] = "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/X11R6/bin:/usr/local/bin:/usr/local/sbin:/usr/games";

	sys_path = def_path;

	return sys_path;
}

static int
parse_command_args(int argc, char **argv, int *use_syspath)
{
	int sv_argc = argc;
	char *cp, c;

	*use_syspath = 0;

	for (;;) {
		argv++;
		if (--argc == 0)
			break;
		cp = *argv;
		if (*cp++ != '-')
			break;
		if (*cp == '-' && cp[1] == 0) {
			argv++;
			argc--;
			break;
		}
		while ((c = *cp++)) {
			switch (c) {
			case 'p':
				*use_syspath = 1;
				break;
			default:
				/* run 'typecmd' for other options */
				return 0;
			}
		}
	}
	return sv_argc - argc;
}

int vforked = 0;

/*
 * Execute a simple command.
 */

STATIC void
evalcommand(union node *cmd, int flgs, struct backcmd *backcmd)
{
	struct stackmark smark;
	union node *argp;
	struct arglist arglist;
	struct arglist varlist;
	volatile int flags = flgs;
	char ** volatile argv;
	volatile int argc;
	char **envp;
	int varflag;
	struct strlist *sp;
	volatile int mode;
	int pip[2];
	struct cmdentry cmdentry;
	struct job * volatile jp;
	struct jmploc jmploc;
	struct jmploc *volatile savehandler = NULL;
	const char *volatile savecmdname;
	volatile struct shparam saveparam;
	struct localvar *volatile savelocalvars;
	struct parsefile *volatile savetopfile;
	volatile int e;
	char * volatile lastarg;
	const char * volatile path = pathval();
	volatile int temp_path;
	const int savefuncline = funclinebase;
	const int savefuncabs = funclineabs;
	volatile int cmd_flags = 0;

	vforked = 0;
	/* First expand the arguments. */
	CTRACE(DBG_EVAL, ("evalcommand(%p, %d) called [%s]\n", cmd, flags,
	    cmd->ncmd.args ? cmd->ncmd.args->narg.text : ""));
	setstackmark(&smark);
	back_exitstatus = 0;

	line_number = cmd->ncmd.lineno;

	arglist.lastp = &arglist.list;
	varflag = 1;
	/* Expand arguments, ignoring the initial 'name=value' ones */
	for (argp = cmd->ncmd.args ; argp ; argp = argp->narg.next) {
		if (varflag && isassignment(argp->narg.text))
			continue;
		varflag = 0;
		line_number = argp->narg.lineno;
		expandarg(argp, &arglist, EXP_FULL | EXP_TILDE);
	}
	*arglist.lastp = NULL;

	expredir(cmd->ncmd.redirect);

	/* Now do the initial 'name=value' ones we skipped above */
	varlist.lastp = &varlist.list;
	for (argp = cmd->ncmd.args ; argp ; argp = argp->narg.next) {
		line_number = argp->narg.lineno;
		if (!isassignment(argp->narg.text))
			break;
		expandarg(argp, &varlist, EXP_VARTILDE);
	}
	*varlist.lastp = NULL;

	argc = 0;
	for (sp = arglist.list ; sp ; sp = sp->next)
		argc++;
	argv = stalloc(sizeof (char *) * (argc + 1));

	for (sp = arglist.list ; sp ; sp = sp->next) {
		VTRACE(DBG_EVAL, ("evalcommand arg: %s\n", sp->text));
		*argv++ = sp->text;
	}
	*argv = NULL;
	lastarg = NULL;
	if (iflag && funcnest == 0 && argc > 0)
		lastarg = argv[-1];
	argv -= argc;

	/* Print the command if xflag is set. */
	if (xflag) {
		char sep = 0;
		union node *rn;

		outxstr(expandstr(ps4val(), line_number));
		for (sp = varlist.list ; sp ; sp = sp->next) {
			char *p;

			if (sep != 0)
				outxc(sep);

			/*
			 * The "var=" part should not be quoted, regardless
			 * of the value, or it would not represent an
			 * assignment, but rather a command
			 */
			p = strchr(sp->text, '=');
			if (p != NULL) {
				*p = '\0';	/*XXX*/
				outxshstr(sp->text);
				outxc('=');
				*p++ = '=';	/*XXX*/
			} else
				p = sp->text;
			outxshstr(p);
			sep = ' ';
		}
		for (sp = arglist.list ; sp ; sp = sp->next) {
			if (sep != 0)
				outxc(sep);
			outxshstr(sp->text);
			sep = ' ';
		}
		for (rn = cmd->ncmd.redirect; rn; rn = rn->nfile.next)
			if (outredir(outx, rn, sep))
				sep = ' ';
		outxc('\n');
		flushout(outx);
	}

	/* Now locate the command. */
	if (argc == 0) {
		/*
		 * the empty command begins as a normal builtin, and
		 * remains that way while redirects are processed, then
		 * will become special before we get to doing the
		 * var assigns.
		 */
		cmdentry.cmdtype = CMDBUILTIN;
		cmdentry.u.bltin = bltincmd;
		VTRACE(DBG_CMDS, ("No command name, assume \"comamnd\"\n"));
	} else {
		static const char PATH[] = "PATH=";

		/*
		 * Modify the command lookup path, if a PATH= assignment
		 * is present
		 */
		for (sp = varlist.list; sp; sp = sp->next)
			if (strncmp(sp->text, PATH, sizeof(PATH) - 1) == 0)
				path = sp->text + sizeof(PATH) - 1;

		do {
			int argsused, use_syspath;

			find_command(argv[0], &cmdentry, cmd_flags, path);
			VTRACE(DBG_CMDS, ("Command %s type %d\n", argv[0],
			    cmdentry.cmdtype));
#if 0
			/*
			 * This short circuits all of the processing that
			 * should be done (including processing the
			 * redirects), so just don't ...
			 *
			 * (eventually this whole #if'd block will vanish)
			 */
			if (cmdentry.cmdtype == CMDUNKNOWN) {
				exitstatus = 127;
				flushout(&errout);
				goto out;
			}
#endif

			/* implement the 'command' builtin here */
			if (cmdentry.cmdtype != CMDBUILTIN ||
			    cmdentry.u.bltin != bltincmd)
				break;
			VTRACE(DBG_CMDS, ("Command \"command\"\n"));
			cmd_flags |= DO_NOFUNC;
			argsused = parse_command_args(argc, argv, &use_syspath);
			if (argsused == 0) {
				/* use 'type' builtin to display info */
				VTRACE(DBG_CMDS,
				    ("Command \"command\" -> \"type\"\n"));
				cmdentry.u.bltin = typecmd;
				break;
			}
			argc -= argsused;
			argv += argsused;
			if (use_syspath)
				path = syspath() + 5;
		} while (argc != 0);
		if (cmdentry.cmdtype == CMDSPLBLTIN && cmd_flags & DO_NOFUNC)
			/* posix mandates that 'command <splbltin>' act as if
			   <splbltin> was a normal builtin */
			cmdentry.cmdtype = CMDBUILTIN;
	}

	/*
	 * When traps are invalid, we permit the following:
	 *	trap
	 *	command trap
	 *	eval trap
	 *	command eval trap
	 *	eval command trap
	 * without zapping the traps completely, in all other cases we do.
	 *
	 * The test here permits eval "anything" but when evalstring() comes
	 * back here again, the "anything" will be validated.
	 * This means we can actually do:
	 *	eval eval eval command eval eval command trap
	 * as long as we end up with just "trap"
	 *
	 * We permit "command" by allowing CMDBUILTIN as well as CMDSPLBLTIN
	 *
	 * trapcmd() takes care of doing free_traps() if it is needed there.
	 */
	if (traps_invalid &&
	    ((cmdentry.cmdtype!=CMDSPLBLTIN && cmdentry.cmdtype!=CMDBUILTIN) ||
	     (cmdentry.u.bltin != trapcmd && cmdentry.u.bltin != evalcmd)))
		free_traps();

	/* Fork off a child process if necessary. */
	if (cmd->ncmd.backgnd
	  || ((cmdentry.cmdtype == CMDNORMAL || cmdentry.cmdtype == CMDUNKNOWN)
	     && (have_traps() || (flags & EV_EXIT) == 0))
#ifdef notyet			/* EV_BACKCMD is never set currently */
			/* this will need more work if/when it gets used */
	  || ((flags & EV_BACKCMD) != 0
	     && (cmdentry.cmdtype != CMDBUILTIN
	         && cmdentry.cmdtype != CMDSPLBLTIN)
	       || cmdentry.u.bltin == dotcmd
	       || cmdentry.u.bltin == evalcmd)
#endif
	 ) {
		INTOFF;
		jp = makejob(cmd, 1);
		mode = cmd->ncmd.backgnd;
		if (flags & EV_BACKCMD) {
			mode = FORK_NOJOB;
			if (sh_pipe(pip) < 0)
				error("Pipe call failed");
		}
#ifdef DO_SHAREDVFORK
		/* It is essential that if DO_SHAREDVFORK is defined that the
		 * child's address space is actually shared with the parent as
		 * we rely on this.
		 */
		if (usefork == 0 && cmdentry.cmdtype == CMDNORMAL &&
		    (!cmd->ncmd.backgnd || cmd->ncmd.redirect == NULL)) {
			pid_t	pid;
			int serrno;

			savelocalvars = localvars;
			localvars = NULL;
			vforked = 1;
	VFORK_BLOCK
			switch (pid = vfork()) {
			case -1:
				serrno = errno;
				VTRACE(DBG_EVAL, ("vfork() failed, errno=%d\n",
				    serrno));
				INTON;
				error("Cannot vfork (%s)", strerror(serrno));
				break;
			case 0:
				/* Make sure that exceptions only unwind to
				 * after the vfork(2)
				 */
				SHELL_FORKED();
				if (setjmp(jmploc.loc)) {
					if (exception == EXSHELLPROC) {
						/*
						 * We can't progress with the
						 * vfork, so, set vforked = 2
						 * so the parent knows,
						 * and _exit();
						 */
						vforked = 2;
						_exit(0);
					} else {
						_exit(exception == EXEXIT ?
						    exitstatus : exerrno);
					}
				}
				savehandler = handler;
				handler = &jmploc;
				listmklocal(varlist.list,
				    VDOEXPORT | VEXPORT | VNOFUNC);
				forkchild(jp, cmd, mode, vforked);
				break;
			default:
				VFORK_UNDO();
						/* restore from vfork(2) */
				CTRACE(DBG_PROCS|DBG_CMDS,
				    ("parent after vfork - vforked=%d\n",
				      vforked));
				handler = savehandler;
				poplocalvars();
				localvars = savelocalvars;
				if (vforked == 2) {
					vforked = 0;

					(void)waitpid(pid, NULL, 0);
					/*
					 * We need to progress in a
					 * normal fork fashion
					 */
					goto normal_fork;
				}
				/*
				 * Here the child has left home,
				 * getting on with its life, so
				 * so must we...
				 */
				vforked = 0;
				forkparent(jp, cmd, mode, pid);
				goto parent;
			}
	VFORK_END
		} else {
 normal_fork:
#endif
			if (forkshell(jp, cmd, mode) != 0)
				goto parent;	/* at end of routine */
			CTRACE(DBG_PROCS|DBG_CMDS, ("Child sets EV_EXIT\n"));
			flags |= EV_EXIT;
			FORCEINTON;
#ifdef DO_SHAREDVFORK
		}
#endif
		if (flags & EV_BACKCMD) {
			if (!vforked) {
				FORCEINTON;
			}
			close(pip[0]);
			movefd(pip[1], 1);
		}
		flags |= EV_EXIT;
	}

	/* This is the child process if a fork occurred. */
	/* Execute the command. */
	switch (cmdentry.cmdtype) {
		volatile int saved;

	case CMDFUNCTION:
		VXTRACE(DBG_EVAL, ("Shell function%s:  ",vforked?" VF":""),
		    trargs(argv));
		redirect(cmd->ncmd.redirect, saved =
			!(flags & EV_EXIT) || have_traps() ? REDIR_PUSH : 0);
		saveparam = shellparam;
		shellparam.malloc = 0;
		shellparam.reset = 1;
		shellparam.nparam = argc - 1;
		shellparam.p = argv + 1;
		shellparam.optnext = NULL;
		INTOFF;
		savelocalvars = localvars;
		localvars = NULL;
		reffunc(cmdentry.u.func);
		INTON;
		if (setjmp(jmploc.loc)) {
			if (exception == EXSHELLPROC) {
				freeparam((volatile struct shparam *)
				    &saveparam);
			} else {
				freeparam(&shellparam);
				shellparam = saveparam;
			}
			if (saved)
				popredir();;
			unreffunc(cmdentry.u.func);
			poplocalvars();
			localvars = savelocalvars;
			funclinebase = savefuncline;
			funclineabs = savefuncabs;
			handler = savehandler;
			longjmp(handler->loc, 1);
		}
		savehandler = handler;
		handler = &jmploc;
		if (cmdentry.u.func) {
			if (cmdentry.lno_frel)
				funclinebase = cmdentry.lineno - 1;
			else
				funclinebase = 0;
			funclineabs = cmdentry.lineno;

			VTRACE(DBG_EVAL,
			  ("function: node: %d '%s' # %d%s; funclinebase=%d\n",
			    getfuncnode(cmdentry.u.func)->type,
			    NODETYPENAME(getfuncnode(cmdentry.u.func)->type),
			    cmdentry.lineno, cmdentry.lno_frel?" (=1)":"",
			    funclinebase));
		}
		listmklocal(varlist.list, VDOEXPORT | VEXPORT);
		/* stop shell blowing its stack */
		if (++funcnest > 1000)
			error("too many nested function calls");
		evaltree(getfuncnode(cmdentry.u.func),
		    flags & (EV_TESTED|EV_EXIT));
		funcnest--;
		INTOFF;
		unreffunc(cmdentry.u.func);
		poplocalvars();
		localvars = savelocalvars;
		funclinebase = savefuncline;
		funclineabs = savefuncabs;
		freeparam(&shellparam);
		shellparam = saveparam;
		handler = savehandler;
		if (saved)
			popredir();
		INTON;
		if (evalskip == SKIPFUNC) {
			evalskip = SKIPNONE;
			skipcount = 0;
		}
		if (flags & EV_EXIT)
			exitshell(exitstatus);
		break;

	case CMDSPLBLTIN:
		VTRACE(DBG_EVAL, ("special "));
	case CMDBUILTIN:
		VXTRACE(DBG_EVAL, ("builtin command [%d]%s:  ", argc,
		    vforked ? " VF" : ""), trargs(argv));
		mode = (cmdentry.u.bltin == execcmd) ? 0 : REDIR_PUSH;
		if (flags == EV_BACKCMD) {
			memout.nleft = 0;
			memout.nextc = memout.buf;
			memout.bufsize = 64;
			mode |= REDIR_BACKQ;
		}
		e = -1;
		savecmdname = commandname;
		savetopfile = getcurrentfile();
		savehandler = handler;
		temp_path = 0;
		if (!setjmp(jmploc.loc)) {
			handler = &jmploc;

			/*
			 * We need to ensure the command hash table isn't
			 * corrupted by temporary PATH assignments.
			 * However we must ensure the 'local' command works!
			 */
			if (path != pathval() && (cmdentry.u.bltin == hashcmd ||
			    cmdentry.u.bltin == typecmd)) {
				savelocalvars = localvars;
				localvars = 0;
				temp_path = 1;
				mklocal(path - 5 /* PATH= */, 0);
			}
			redirect(cmd->ncmd.redirect, mode);

			/*
			 * the empty command is regarded as a normal
			 * builtin for the purposes of redirects, but
			 * is a special builtin for var assigns.
			 * (unless we are the "command" command.)
			 */
			if (argc == 0 && !(cmd_flags & DO_NOFUNC))
				cmdentry.cmdtype = CMDSPLBLTIN;

			/* exec is a special builtin, but needs this list... */
			cmdenviron = varlist.list;
			/* we must check 'readonly' flag for all builtins */
			listsetvar(varlist.list,
				cmdentry.cmdtype == CMDSPLBLTIN ? 0 : VNOSET);
			commandname = argv[0];
			/* initialize nextopt */
			argptr = argv + 1;
			optptr = NULL;
			/* and getopt */
			optreset = 1;
			optind = 1;
			builtin_flags = flags;
			exitstatus = cmdentry.u.bltin(argc, argv);
		} else {
			e = exception;
			if (e == EXINT)
				exitstatus = SIGINT + 128;
			else if (e == EXEXEC)
				exitstatus = exerrno;
			else if (e != EXEXIT)
				exitstatus = 2;
		}
		handler = savehandler;
		flushall();
		out1 = &output;
		out2 = &errout;
		freestdout();
		if (temp_path) {
			poplocalvars();
			localvars = savelocalvars;
		}
		cmdenviron = NULL;
		if (e != EXSHELLPROC) {
			commandname = savecmdname;
			if (flags & EV_EXIT)
				exitshell(exitstatus);
		}
		if (e != -1) {
			if ((e != EXERROR && e != EXEXEC)
			    || cmdentry.cmdtype == CMDSPLBLTIN)
				exraise(e);
			popfilesupto(savetopfile);
			FORCEINTON;
		}
		if (cmdentry.u.bltin != execcmd)
			popredir();
		if (flags == EV_BACKCMD) {
			backcmd->buf = memout.buf;
			backcmd->nleft = memout.nextc - memout.buf;
			memout.buf = NULL;
		}
		break;

	default:
		VXTRACE(DBG_EVAL, ("normal command%s:  ", vforked?" VF":""),
		    trargs(argv));
		redirect(cmd->ncmd.redirect, 
		    (vforked ? REDIR_VFORK : 0) | REDIR_KEEP);
		if (!vforked)
			for (sp = varlist.list ; sp ; sp = sp->next)
				setvareq(sp->text, VDOEXPORT|VEXPORT|VSTACK);
		envp = environment();
		shellexec(argv, envp, path, cmdentry.u.index, vforked);
		break;
	}
	goto out;

 parent:			/* parent process gets here (if we forked) */

	exitstatus = 0;		/* if not altered just below */
	if (mode == FORK_FG) {	/* argument to fork */
		exitstatus = waitforjob(jp);
	} else if (mode == FORK_NOJOB) {
		backcmd->fd = pip[0];
		close(pip[1]);
		backcmd->jp = jp;
	}
	FORCEINTON;

 out:
	if (lastarg)
		/* implement $_ for whatever use that really is */
		(void) setvarsafe("_", lastarg, VNOERROR);
	popstackmark(&smark);
}


/*
 * Search for a command.  This is called before we fork so that the
 * location of the command will be available in the parent as well as
 * the child.  The check for "goodname" is an overly conservative
 * check that the name will not be subject to expansion.
 */

STATIC void
prehash(union node *n)
{
	struct cmdentry entry;

	if (n && n->type == NCMD && n->ncmd.args)
		if (goodname(n->ncmd.args->narg.text))
			find_command(n->ncmd.args->narg.text, &entry, 0,
				     pathval());
}

int
in_function(void)
{
	return funcnest;
}

enum skipstate
current_skipstate(void)
{
	return evalskip;
}

void
save_skipstate(struct skipsave *p)
{
	*p = s_k_i_p;
}

void
restore_skipstate(const struct skipsave *p)
{
	s_k_i_p = *p;
}

void
stop_skipping(void)
{
	evalskip = SKIPNONE;
	skipcount = 0;
}

/*
 * Builtin commands.  Builtin commands whose functions are closely
 * tied to evaluation are implemented here.
 */

/*
 * No command given.
 */

int
bltincmd(int argc, char **argv)
{
	/*
	 * Preserve exitstatus of a previous possible redirection
	 * as POSIX mandates
	 */
	return back_exitstatus;
}


/*
 * Handle break and continue commands.  Break, continue, and return are
 * all handled by setting the evalskip flag.  The evaluation routines
 * above all check this flag, and if it is set they start skipping
 * commands rather than executing them.  The variable skipcount is
 * the number of loops to break/continue, or the number of function
 * levels to return.  (The latter is always 1.)  It should probably
 * be an error to break out of more loops than exist, but it isn't
 * in the standard shell so we don't make it one here.
 */

int
breakcmd(int argc, char **argv)
{
	int n = argc > 1 ? number(argv[1]) : 1;

	if (n <= 0)
		error("invalid count: %d", n);
	if (n > loopnest)
		n = loopnest;
	if (n > 0) {
		evalskip = (**argv == 'c')? SKIPCONT : SKIPBREAK;
		skipcount = n;
	}
	return 0;
}

int
dotcmd(int argc, char **argv)
{
	exitstatus = 0;

	(void) nextopt(NULL);		/* ignore a leading "--" */

	if (*argptr != NULL) {		/* That's what SVR2 does */
		char *fullname;
		/*
		 * dot_funcnest needs to be 0 when not in a dotcmd, so it
		 * cannot be restored with (funcnest + 1).
		 */
		int dot_funcnest_old;
		struct stackmark smark;

		setstackmark(&smark);
		fullname = find_dot_file(*argptr);
		setinputfile(fullname, 1);
		commandname = fullname;
		dot_funcnest_old = dot_funcnest;
		dot_funcnest = funcnest + 1;
		cmdloop(0);
		dot_funcnest = dot_funcnest_old;
		popfile();
		popstackmark(&smark);
	}
	return exitstatus;
}

/*
 * allow dotfile function nesting to be manipulated
 * (for read_profile).  This allows profile files to
 * be treated as if they were used as '.' commands,
 * (approximately) and in particular, for "return" to work.
 */
int
set_dot_funcnest(int new)
{
	int rv = dot_funcnest;

	if (new >= 0)
		dot_funcnest = new;

	return rv;
}

/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */

STATIC char *
find_dot_file(char *basename)
{
	char *fullname;
	const char *path = pathval();
	struct stat statb;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/')) {
		if (stat(basename, &statb) == 0) {
			if (S_ISDIR(statb.st_mode))
				error("%s: is a directory", basename);
			if (S_ISBLK(statb.st_mode))
				error("%s: is a block device", basename);
			return basename;
		}
	} else while ((fullname = padvance(&path, basename, 1)) != NULL) {
		if ((stat(fullname, &statb) == 0)) {
			/* weird format is to ease future code... */
			if (S_ISDIR(statb.st_mode) || S_ISBLK(statb.st_mode))
				;
#if notyet
			else if (unreadable()) {
				/*
				 * testing this via st_mode is ugly to get
				 * correct (and would ignore ACLs).
				 * better way is just to open the file.
				 * But doing that here would (currently)
				 * mean opening the file twice, which
				 * might not be safe.  So, defer this
				 * test until code is restructures so
				 * we can return a fd.   Then we also
				 * get to fix the mem leak just below...
				 */
			}
#endif
			else {
				/*
				 * Don't bother freeing here, since
				 * it will be freed by the caller.
				 * XXX no it won't - a bug for later.
				 */
				return fullname;
			}
		}
		stunalloc(fullname);
	}

	/* not found in the PATH */
	error("%s: not found", basename);
	/* NOTREACHED */
}



/*
 * The return command.
 *
 * Quoth the POSIX standard:
 *   The return utility shall cause the shell to stop executing the current
 *   function or dot script. If the shell is not currently executing
 *   a function or dot script, the results are unspecified.
 *
 * As for the unspecified part, there seems to be no de-facto standard: bash
 * ignores the return with a warning, zsh ignores the return in interactive
 * mode but seems to liken it to exit in a script.  (checked May 2014)
 *
 * We choose to silently ignore the return.  Older versions of this shell
 * set evalskip to SKIPFILE causing the shell to (indirectly) exit.  This
 * had at least the problem of circumventing the check for stopped jobs,
 * which would occur for exit or ^D.
 */

int
returncmd(int argc, char **argv)
{
	int ret = argc > 1 ? number(argv[1]) : exitstatus;

	if ((dot_funcnest == 0 && funcnest)
	    || (dot_funcnest > 0 && funcnest - (dot_funcnest - 1) > 0)) {
		evalskip = SKIPFUNC;
		skipcount = 1;
	} else if (dot_funcnest > 0) {
		evalskip = SKIPFILE;
		skipcount = 1;
	} else {
		/* XXX: should a warning be issued? */
		ret = 0;
	}

	return ret;
}


int
falsecmd(int argc, char **argv)
{
	return 1;
}


int
truecmd(int argc, char **argv)
{
	return 0;
}


int
execcmd(int argc, char **argv)
{
	(void) nextopt(NULL);		/* ignore a leading "--" */

	if (*argptr) {
		struct strlist *sp;

		iflag = 0;		/* exit on error */
		mflag = 0;
		optschanged();
		for (sp = cmdenviron; sp; sp = sp->next)
			setvareq(sp->text, VDOEXPORT|VEXPORT|VSTACK);
		shellexec(argptr, environment(), pathval(), 0, 0);
	}
	return 0;
}

static int
conv_time(clock_t ticks, char *seconds, size_t l)
{
	static clock_t tpm = 0;
	clock_t mins;
	int i;

	if (!tpm)
		tpm = sysconf(_SC_CLK_TCK) * 60;

	mins = ticks / tpm;
	snprintf(seconds, l, "%.4f", (ticks - mins * tpm) * 60.0 / tpm );

	if (seconds[0] == '6' && seconds[1] == '0') {
		/* 59.99995 got rounded up... */
		mins++;
		strlcpy(seconds, "0.0", l);
		return mins;
	}

	/* suppress trailing zeros */
	i = strlen(seconds) - 1;
	for (; seconds[i] == '0' && seconds[i - 1] != '.'; i--)
		seconds[i] = 0;
	return mins;
}

int
timescmd(int argc, char **argv)
{
	struct tms tms;
	int u, s, cu, cs;
	char us[8], ss[8], cus[8], css[8];

	nextopt("");

	times(&tms);

	u = conv_time(tms.tms_utime, us, sizeof(us));
	s = conv_time(tms.tms_stime, ss, sizeof(ss));
	cu = conv_time(tms.tms_cutime, cus, sizeof(cus));
	cs = conv_time(tms.tms_cstime, css, sizeof(css));

	outfmt(out1, "%dm%ss %dm%ss\n%dm%ss %dm%ss\n",
		u, us, s, ss, cu, cus, cs, css);

	return 0;
}
