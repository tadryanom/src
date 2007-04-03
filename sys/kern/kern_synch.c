/*	$OpenBSD: kern_synch.c,v 1.79 2007/04/03 08:05:43 art Exp $	*/
/*	$NetBSD: kern_synch.c,v 1.37 1996/04/22 01:38:37 christos Exp $	*/

/*
 * Copyright (c) 1982, 1986, 1990, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *
 *	@(#)kern_synch.c	8.6 (Berkeley) 1/21/94
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/signalvar.h>
#include <sys/resourcevar.h>
#include <uvm/uvm_extern.h>
#include <sys/sched.h>
#include <sys/timeout.h>
#include <sys/mount.h>
#include <sys/syscallargs.h>
#include <sys/pool.h>

#include <machine/spinlock.h>

#ifdef KTRACE
#include <sys/ktrace.h>
#endif

void updatepri(struct proc *);
void endtsleep(void *);

/*
 * We're only looking at 7 bits of the address; everything is
 * aligned to 4, lots of things are aligned to greater powers
 * of 2.  Shift right by 8, i.e. drop the bottom 256 worth.
 */
#define TABLESIZE	128
#define LOOKUP(x)	(((long)(x) >> 8) & (TABLESIZE - 1))
struct slpque {
	struct proc *sq_head;
	struct proc **sq_tailp;
} slpque[TABLESIZE];

/*
 * During autoconfiguration or after a panic, a sleep will simply
 * lower the priority briefly to allow interrupts, then return.
 * The priority to be used (safepri) is machine-dependent, thus this
 * value is initialized and maintained in the machine-dependent layers.
 * This priority will typically be 0, or the lowest priority
 * that is safe for use on the interrupt stack; it can be made
 * higher to block network software interrupts after panics.
 */
int safepri;

/*
 * General sleep call.  Suspends the current process until a wakeup is
 * performed on the specified identifier.  The process will then be made
 * runnable with the specified priority.  Sleeps at most timo/hz seconds
 * (0 means no timeout).  If pri includes PCATCH flag, signals are checked
 * before and after sleeping, else signals are not checked.  Returns 0 if
 * awakened, EWOULDBLOCK if the timeout expires.  If PCATCH is set and a
 * signal needs to be delivered, ERESTART is returned if the current system
 * call should be restarted if possible, and EINTR is returned if the system
 * call should be interrupted by the signal (return EINTR).
 */
int
tsleep(void *ident, int priority, const char *wmesg, int timo)
{
	struct sleep_state sls;
	int error, error1;

	if (cold || panicstr) {
		int s;
		/*
		 * After a panic, or during autoconfiguration,
		 * just give interrupts a chance, then just return;
		 * don't run any other procs or panic below,
		 * in case this is the idle process and already asleep.
		 */
		s = splhigh();
		splx(safepri);
		splx(s);
		return (0);
	}

	sleep_setup(&sls, ident, priority, wmesg);
	sleep_setup_timeout(&sls, timo);
	sleep_setup_signal(&sls, priority);

	sleep_finish(&sls, 1);
	error1 = sleep_finish_timeout(&sls);
	error = sleep_finish_signal(&sls);

	/* Signal errors are higher priority than timeouts. */
	if (error == 0 && error1 != 0)
		error = error1;

	return (error);
}

void
sleep_setup(struct sleep_state *sls, void *ident, int prio, const char *wmesg)
{
	struct proc *p = curproc;
	struct slpque *qp;

#ifdef DIAGNOSTIC
	if (ident == NULL)
		panic("tsleep: no ident");
	if (p->p_stat != SONPROC)
		panic("tsleep: not SONPROC");
	if (p->p_back != NULL)
		panic("tsleep: p_back not NULL");
#endif

#ifdef KTRACE
	if (KTRPOINT(p, KTR_CSW))
		ktrcsw(p, 1, 0);
#endif

	sls->sls_catch = 0;
	sls->sls_do_sleep = 1;
	sls->sls_sig = 1;

	SCHED_LOCK(sls->sls_s);

	p->p_wchan = ident;
	p->p_wmesg = wmesg;
	p->p_slptime = 0;
	p->p_priority = prio & PRIMASK;
	qp = &slpque[LOOKUP(ident)];
	if (qp->sq_head == 0)
		qp->sq_head = p;
	else
		*qp->sq_tailp = p;
	*(qp->sq_tailp = &p->p_forw) = NULL;
}

void
sleep_finish(struct sleep_state *sls, int do_sleep)
{
	struct proc *p = curproc;

	if (sls->sls_do_sleep && do_sleep) {
		p->p_stat = SSLEEP;
		p->p_stats->p_ru.ru_nvcsw++;
		SCHED_ASSERT_LOCKED();
		mi_switch();
	} else if (!do_sleep) {
		unsleep(p);
#ifdef DIAGNOSTIC
		if (p->p_stat != SONPROC)
			panic("sleep_finish !SONPROC");
#endif
	}

#ifdef __HAVE_CPUINFO
	p->p_cpu->ci_schedstate.spc_curpriority = p->p_usrpri;
#else
	curpriority = p->p_usrpri;
#endif
	SCHED_UNLOCK(sls->sls_s);

	/*
	 * Even though this belongs to the signal handling part of sleep,
	 * we need to clear it before the ktrace.
	 */
	atomic_clearbits_int(&p->p_flag, P_SINTR);

#ifdef KTRACE
	if (KTRPOINT(p, KTR_CSW))
		ktrcsw(p, 0, 0);
#endif
}

void
sleep_setup_timeout(struct sleep_state *sls, int timo)
{
	if (timo)
		timeout_add(&curproc->p_sleep_to, timo);
}

int
sleep_finish_timeout(struct sleep_state *sls)
{
	struct proc *p = curproc;

	if (p->p_flag & P_TIMEOUT) {
		atomic_clearbits_int(&p->p_flag, P_TIMEOUT);
		return (EWOULDBLOCK);
	} else if (timeout_pending(&p->p_sleep_to)) {
		timeout_del(&p->p_sleep_to);
	}

	return (0);
}

void
sleep_setup_signal(struct sleep_state *sls, int prio)
{
	struct proc *p = curproc;

	if ((sls->sls_catch = (prio & PCATCH)) == 0)
		return;

	/*
	 * We put ourselves on the sleep queue and start our timeout
	 * before calling CURSIG, as we could stop there, and a wakeup
	 * or a SIGCONT (or both) could occur while we were stopped.
	 * A SIGCONT would cause us to be marked as SSLEEP
	 * without resuming us, thus we must be ready for sleep
	 * when CURSIG is called.  If the wakeup happens while we're
	 * stopped, p->p_wchan will be 0 upon return from CURSIG.
	 */
	atomic_setbits_int(&p->p_flag, P_SINTR);
	if ((sls->sls_sig = CURSIG(p)) != 0) {
		if (p->p_wchan)
			unsleep(p);
		p->p_stat = SONPROC;
		sls->sls_do_sleep = 0;
	} else if (p->p_wchan == 0) {
		sls->sls_catch = 0;
		sls->sls_do_sleep = 0;
	}
}

int
sleep_finish_signal(struct sleep_state *sls)
{
	struct proc *p = curproc;

	if (sls->sls_catch != 0) {
		if (sls->sls_sig != 0 || (sls->sls_sig = CURSIG(p)) != 0) {
			if (p->p_sigacts->ps_sigintr & sigmask(sls->sls_sig))
				return (EINTR);
			return (ERESTART);
		}
	}

	return (0);
}

/*
 * Implement timeout for tsleep.
 * If process hasn't been awakened (wchan non-zero),
 * set timeout flag and undo the sleep.  If proc
 * is stopped, just unsleep so it will remain stopped.
 */
void
endtsleep(void *arg)
{
	struct proc *p = arg;
	int s;

	SCHED_LOCK(s);
	if (p->p_wchan) {
		if (p->p_stat == SSLEEP)
			setrunnable(p);
		else
			unsleep(p);
		atomic_setbits_int(&p->p_flag, P_TIMEOUT);
	}
	SCHED_UNLOCK(s);
}

/*
 * Remove a process from its wait queue
 */
void
unsleep(struct proc *p)
{
	struct slpque *qp;
	struct proc **hp;

	if (p->p_wchan) {
		hp = &(qp = &slpque[LOOKUP(p->p_wchan)])->sq_head;
		while (*hp != p)
			hp = &(*hp)->p_forw;
		*hp = p->p_forw;
		if (qp->sq_tailp == &p->p_forw)
			qp->sq_tailp = hp;
		p->p_wchan = 0;
	}
}

/*
 * Make a number of processes sleeping on the specified identifier runnable.
 */
void
wakeup_n(void *ident, int n)
{
	struct slpque *qp;
	struct proc *p, **q;
	int s;

	SCHED_LOCK(s);
	qp = &slpque[LOOKUP(ident)];
restart:
	for (q = &qp->sq_head; (p = *q) != NULL; ) {
#ifdef DIAGNOSTIC
		if (p->p_back)
			panic("wakeup: p_back not NULL");
		if (p->p_stat != SSLEEP && p->p_stat != SSTOP)
			panic("wakeup: p_stat is %d", (int)p->p_stat);
#endif
		if (p->p_wchan == ident) {
			--n;
			p->p_wchan = 0;
			*q = p->p_forw;
			if (qp->sq_tailp == &p->p_forw)
				qp->sq_tailp = q;
			if (p->p_stat == SSLEEP) {
				/* OPTIMIZED EXPANSION OF setrunnable(p); */
				if (p->p_slptime > 1)
					updatepri(p);
				p->p_slptime = 0;
				p->p_stat = SRUN;

				/*
				 * Since curpriority is a user priority,
				 * p->p_priority is always better than
				 * curpriority on the last CPU on
				 * which it ran.
				 *
				 * XXXSMP See affinity comment in
				 * resched_proc().
				 */
				setrunqueue(p);
#ifdef __HAVE_CPUINFO
				KASSERT(p->p_cpu != NULL);
				need_resched(p->p_cpu);
#else
				need_resched(NULL);
#endif
				/* END INLINE EXPANSION */

				if (n != 0)
					goto restart;
				else
					break;
			}
		} else
			q = &p->p_forw;
	}
	SCHED_UNLOCK(s);
}

/*
 * Make all processes sleeping on the specified identifier runnable.
 */
void
wakeup(void *chan)
{
	wakeup_n(chan, -1);
}

int
sys_sched_yield(struct proc *p, void *v, register_t *retval)
{
	yield();
	return (0);
}

#ifdef RTHREADS

int
sys_thrsleep(struct proc *p, void *v, register_t *revtal)
{
	struct sys_thrsleep_args *uap = v;
	long ident = (long)SCARG(uap, ident);
	int timo = SCARG(uap, timeout);
	_spinlock_lock_t *lock = SCARG(uap, lock);
	_spinlock_lock_t unlocked = _SPINLOCK_UNLOCKED;
	int error;

	p->p_thrslpid = ident;

	if (lock)
		copyout(&unlocked, lock, sizeof(unlocked));
	if (hz > 1000)
		timo = timo * (hz / 1000);
	else
		timo = timo / (1000 / hz);
	if (timo < 0)
		timo = 0;
	error = tsleep(&p->p_thrslpid, PUSER | PCATCH, "thrsleep", timo);

	if (error == ERESTART)
		error = EINTR;

	return (error);

}

int
sys_thrwakeup(struct proc *p, void *v, register_t *retval)
{
	struct sys_thrwakeup_args *uap = v;
	long ident = (long)SCARG(uap, ident);
	int n = SCARG(uap, n);
	struct proc *q;
	int found = 0;
	
	TAILQ_FOREACH(q, &p->p_p->ps_threads, p_thr_link) {
		if (q->p_thrslpid == ident) {
			wakeup(&q->p_thrslpid);
			q->p_thrslpid = 0;
			if (++found == n)
				return (0);
		}
	}
	if (!found)
		return (ESRCH);

	return (0);
}
#endif
