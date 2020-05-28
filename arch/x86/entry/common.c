// SPDX-License-Identifier: GPL-2.0-only
/*
 * common.c - C code for kernel entry and exit
 * Copyright (c) 2015 Andrew Lutomirski
 *
 * Based on asm and ptrace code by many authors.  The code here originated
 * in ptrace.c and signal.c.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/tracehook.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/signal.h>
#include <linux/export.h>
#include <linux/context_tracking.h>
#include <linux/user-return-notifier.h>
#include <linux/nospec.h>
#include <linux/uprobes.h>
#include <linux/livepatch.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include <asm/desc.h>
#include <asm/traps.h>
#include <asm/vdso.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include <asm/nospec-branch.h>
#include <asm/io_bitmap.h>
#include <asm/syscall.h>

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

#ifdef CONFIG_CONTEXT_TRACKING
/**
 * enter_from_user_mode - Establish state when coming from user mode
 *
 * Syscall entry disables interrupts, but user mode is traced as interrupts
 * enabled. Also with NO_HZ_FULL RCU might be idle.
 *
 * 1) Tell lockdep that interrupts are disabled
 * 2) Invoke context tracking if enabled to reactivate RCU
 * 3) Trace interrupts off state
 */
__visible noinstr void enter_from_user_mode(void)
{
	enum ctx_state state = ct_state();

	lockdep_hardirqs_off(CALLER_ADDR0);
	user_exit_irqoff();

	instrumentation_begin();
	CT_WARN_ON(state != CONTEXT_USER);
	trace_hardirqs_off_prepare();
	instrumentation_end();
}
#else
static __always_inline void enter_from_user_mode(void)
{
	lockdep_hardirqs_off(CALLER_ADDR0);
	instrumentation_begin();
	trace_hardirqs_off_prepare();
	instrumentation_end();
}
#endif

/**
 * exit_to_user_mode - Fixup state when exiting to user mode
 *
 * Syscall exit enables interrupts, but the kernel state is interrupts
 * disabled when this is invoked. Also tell RCU about it.
 *
 * 1) Trace interrupts on state
 * 2) Invoke context tracking if enabled to adjust RCU state
 * 3) Clear CPU buffers if CPU is affected by MDS and the migitation is on.
 * 4) Tell lockdep that interrupts are enabled
 */
static __always_inline void exit_to_user_mode(void)
{
	instrumentation_begin();
	trace_hardirqs_on_prepare();
	lockdep_hardirqs_on_prepare(CALLER_ADDR0);
	instrumentation_end();

	user_enter_irqoff();
	mds_user_clear_cpu_buffers();
	lockdep_hardirqs_on(CALLER_ADDR0);
}

static void do_audit_syscall_entry(struct pt_regs *regs, u32 arch)
{
#ifdef CONFIG_X86_64
	if (arch == AUDIT_ARCH_X86_64) {
		audit_syscall_entry(regs->orig_ax, regs->di,
				    regs->si, regs->dx, regs->r10);
	} else
#endif
	{
		audit_syscall_entry(regs->orig_ax, regs->bx,
				    regs->cx, regs->dx, regs->si);
	}
}

/*
 * Returns the syscall nr to run (which should match regs->orig_ax) or -1
 * to skip the syscall.
 */
static long syscall_trace_enter(struct pt_regs *regs)
{
	u32 arch = in_ia32_syscall() ? AUDIT_ARCH_I386 : AUDIT_ARCH_X86_64;

	struct thread_info *ti = current_thread_info();
	unsigned long ret = 0;
	u32 work;

	if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
		BUG_ON(regs != task_pt_regs(current));

	work = READ_ONCE(ti->flags);

	if (work & (_TIF_SYSCALL_TRACE | _TIF_SYSCALL_EMU)) {
		ret = tracehook_report_syscall_entry(regs);
		if (ret || (work & _TIF_SYSCALL_EMU))
			return -1L;
	}

#ifdef CONFIG_SECCOMP
	/*
	 * Do seccomp after ptrace, to catch any tracer changes.
	 */
	if (work & _TIF_SECCOMP) {
		struct seccomp_data sd;

		sd.arch = arch;
		sd.nr = regs->orig_ax;
		sd.instruction_pointer = regs->ip;
#ifdef CONFIG_X86_64
		if (arch == AUDIT_ARCH_X86_64) {
			sd.args[0] = regs->di;
			sd.args[1] = regs->si;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->r10;
			sd.args[4] = regs->r8;
			sd.args[5] = regs->r9;
		} else
#endif
		{
			sd.args[0] = regs->bx;
			sd.args[1] = regs->cx;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->si;
			sd.args[4] = regs->di;
			sd.args[5] = regs->bp;
		}

		ret = __secure_computing(&sd);
		if (ret == -1)
			return ret;
	}
#endif

	if (unlikely(test_thread_flag(TIF_SYSCALL_TRACEPOINT)))
		trace_sys_enter(regs, regs->orig_ax);

	do_audit_syscall_entry(regs, arch);

	return ret ?: regs->orig_ax;
}

#define EXIT_TO_USERMODE_LOOP_FLAGS				\
	(_TIF_SIGPENDING | _TIF_NOTIFY_RESUME | _TIF_UPROBE |	\
	 _TIF_NEED_RESCHED | _TIF_USER_RETURN_NOTIFY | _TIF_PATCH_PENDING)

static void exit_to_usermode_loop(struct pt_regs *regs, u32 cached_flags)
{
	/*
	 * In order to return to user mode, we need to have IRQs off with
	 * none of EXIT_TO_USERMODE_LOOP_FLAGS set.  Several of these flags
	 * can be set at any time on preemptible kernels if we have IRQs on,
	 * so we need to loop.  Disabling preemption wouldn't help: doing the
	 * work to clear some of the flags can sleep.
	 */
	while (true) {
		/* We have work to do. */
		local_irq_enable();

		if (cached_flags & _TIF_NEED_RESCHED)
			schedule();

		if (cached_flags & _TIF_UPROBE)
			uprobe_notify_resume(regs);

		if (cached_flags & _TIF_PATCH_PENDING)
			klp_update_patch_state(current);

		/* deal with pending signal delivery */
		if (cached_flags & _TIF_SIGPENDING)
			do_signal(regs);

		if (cached_flags & _TIF_NOTIFY_RESUME) {
			clear_thread_flag(TIF_NOTIFY_RESUME);
			tracehook_notify_resume(regs);
			rseq_handle_notify_resume(NULL, regs);
		}

		if (cached_flags & _TIF_USER_RETURN_NOTIFY)
			fire_user_return_notifiers();

		/* Disable IRQs and retry */
		local_irq_disable();

		cached_flags = READ_ONCE(current_thread_info()->flags);

		if (!(cached_flags & EXIT_TO_USERMODE_LOOP_FLAGS))
			break;
	}
}

static void __prepare_exit_to_usermode(struct pt_regs *regs)
{
	struct thread_info *ti = current_thread_info();
	u32 cached_flags;

	addr_limit_user_check();

	lockdep_assert_irqs_disabled();
	lockdep_sys_exit();

	cached_flags = READ_ONCE(ti->flags);

	if (unlikely(cached_flags & EXIT_TO_USERMODE_LOOP_FLAGS))
		exit_to_usermode_loop(regs, cached_flags);

	/* Reload ti->flags; we may have rescheduled above. */
	cached_flags = READ_ONCE(ti->flags);

	if (unlikely(cached_flags & _TIF_IO_BITMAP))
		tss_update_io_bitmap();

	fpregs_assert_state_consistent();
	if (unlikely(cached_flags & _TIF_NEED_FPU_LOAD))
		switch_fpu_return();

#ifdef CONFIG_COMPAT
	/*
	 * Compat syscalls set TS_COMPAT.  Make sure we clear it before
	 * returning to user mode.  We need to clear it *after* signal
	 * handling, because syscall restart has a fixup for compat
	 * syscalls.  The fixup is exercised by the ptrace_syscall_32
	 * selftest.
	 *
	 * We also need to clear TS_REGS_POKED_I386: the 32-bit tracer
	 * special case only applies after poking regs and before the
	 * very next return to user mode.
	 */
	ti->status &= ~(TS_COMPAT|TS_I386_REGS_POKED);
#endif
}

__visible noinstr void prepare_exit_to_usermode(struct pt_regs *regs)
{
	instrumentation_begin();
	__prepare_exit_to_usermode(regs);
	instrumentation_end();
	exit_to_user_mode();
}

#define SYSCALL_EXIT_WORK_FLAGS				\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT |	\
	 _TIF_SINGLESTEP | _TIF_SYSCALL_TRACEPOINT)

static void syscall_slow_exit_work(struct pt_regs *regs, u32 cached_flags)
{
	bool step;

	audit_syscall_exit(regs);

	if (cached_flags & _TIF_SYSCALL_TRACEPOINT)
		trace_sys_exit(regs, regs->ax);

	/*
	 * If TIF_SYSCALL_EMU is set, we only get here because of
	 * TIF_SINGLESTEP (i.e. this is PTRACE_SYSEMU_SINGLESTEP).
	 * We already reported this syscall instruction in
	 * syscall_trace_enter().
	 */
	step = unlikely(
		(cached_flags & (_TIF_SINGLESTEP | _TIF_SYSCALL_EMU))
		== _TIF_SINGLESTEP);
	if (step || cached_flags & _TIF_SYSCALL_TRACE)
		tracehook_report_syscall_exit(regs, step);
}

static void __syscall_return_slowpath(struct pt_regs *regs)
{
	struct thread_info *ti = current_thread_info();
	u32 cached_flags = READ_ONCE(ti->flags);

	CT_WARN_ON(ct_state() != CONTEXT_KERNEL);

	if (IS_ENABLED(CONFIG_PROVE_LOCKING) &&
	    WARN(irqs_disabled(), "syscall %ld left IRQs disabled", regs->orig_ax))
		local_irq_enable();

	rseq_syscall(regs);

	/*
	 * First do one-time work.  If these work items are enabled, we
	 * want to run them exactly once per syscall exit with IRQs on.
	 */
	if (unlikely(cached_flags & SYSCALL_EXIT_WORK_FLAGS))
		syscall_slow_exit_work(regs, cached_flags);

	local_irq_disable();
	__prepare_exit_to_usermode(regs);
}

/*
 * Called with IRQs on and fully valid regs.  Returns with IRQs off in a
 * state such that we can immediately switch to user mode.
 */
__visible noinstr void syscall_return_slowpath(struct pt_regs *regs)
{
	instrumentation_begin();
	__syscall_return_slowpath(regs);
	instrumentation_end();
	exit_to_user_mode();
}

#ifdef CONFIG_X86_64
__visible noinstr void do_syscall_64(unsigned long nr, struct pt_regs *regs)
{
	struct thread_info *ti;

	enter_from_user_mode();
	instrumentation_begin();

	local_irq_enable();
	ti = current_thread_info();
	if (READ_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY)
		nr = syscall_trace_enter(regs);

	if (likely(nr < NR_syscalls)) {
		nr = array_index_nospec(nr, NR_syscalls);
		regs->ax = sys_call_table[nr](regs);
#ifdef CONFIG_X86_X32_ABI
	} else if (likely((nr & __X32_SYSCALL_BIT) &&
			  (nr & ~__X32_SYSCALL_BIT) < X32_NR_syscalls)) {
		nr = array_index_nospec(nr & ~__X32_SYSCALL_BIT,
					X32_NR_syscalls);
		regs->ax = x32_sys_call_table[nr](regs);
#endif
	}
	__syscall_return_slowpath(regs);

	instrumentation_end();
	exit_to_user_mode();
}
#endif

#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
/*
 * Does a 32-bit syscall.  Called with IRQs on in CONTEXT_KERNEL.  Does
 * all entry and exit work and returns with IRQs off.  This function is
 * extremely hot in workloads that use it, and it's usually called from
 * do_fast_syscall_32, so forcibly inline it to improve performance.
 */
static void do_syscall_32_irqs_on(struct pt_regs *regs)
{
	struct thread_info *ti = current_thread_info();
	unsigned int nr = (unsigned int)regs->orig_ax;

#ifdef CONFIG_IA32_EMULATION
	ti->status |= TS_COMPAT;
#endif

	if (READ_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY) {
		/*
		 * Subtlety here: if ptrace pokes something larger than
		 * 2^32-1 into orig_ax, this truncates it.  This may or
		 * may not be necessary, but it matches the old asm
		 * behavior.
		 */
		nr = syscall_trace_enter(regs);
	}

	if (likely(nr < IA32_NR_syscalls)) {
		nr = array_index_nospec(nr, IA32_NR_syscalls);
		regs->ax = ia32_sys_call_table[nr](regs);
	}

	__syscall_return_slowpath(regs);
}

/* Handles int $0x80 */
__visible noinstr void do_int80_syscall_32(struct pt_regs *regs)
{
	enter_from_user_mode();
	instrumentation_begin();

	local_irq_enable();
	do_syscall_32_irqs_on(regs);

	instrumentation_end();
	exit_to_user_mode();
}

static bool __do_fast_syscall_32(struct pt_regs *regs)
{
	int res;

	/* Fetch EBP from where the vDSO stashed it. */
	if (IS_ENABLED(CONFIG_X86_64)) {
		/*
		 * Micro-optimization: the pointer we're following is
		 * explicitly 32 bits, so it can't be out of range.
		 */
		res = __get_user(*(u32 *)&regs->bp,
			 (u32 __user __force *)(unsigned long)(u32)regs->sp);
	} else {
		res = get_user(*(u32 *)&regs->bp,
		       (u32 __user __force *)(unsigned long)(u32)regs->sp);
	}

	if (res) {
		/* User code screwed up. */
		regs->ax = -EFAULT;
		local_irq_disable();
		__prepare_exit_to_usermode(regs);
		return false;
	}

	/* Now this is just like a normal syscall. */
	do_syscall_32_irqs_on(regs);
	return true;
}

/* Returns 0 to return using IRET or 1 to return using SYSEXIT/SYSRETL. */
__visible noinstr long do_fast_syscall_32(struct pt_regs *regs)
{
	/*
	 * Called using the internal vDSO SYSENTER/SYSCALL32 calling
	 * convention.  Adjust regs so it looks like we entered using int80.
	 */
	unsigned long landing_pad = (unsigned long)current->mm->context.vdso +
					vdso_image_32.sym_int80_landing_pad;
	bool success;

	/*
	 * SYSENTER loses EIP, and even SYSCALL32 needs us to skip forward
	 * so that 'regs->ip -= 2' lands back on an int $0x80 instruction.
	 * Fix it up.
	 */
	regs->ip = landing_pad;

	enter_from_user_mode();
	instrumentation_begin();

	local_irq_enable();
	success = __do_fast_syscall_32(regs);

	instrumentation_end();
	exit_to_user_mode();

	/* If it failed, keep it simple: use IRET. */
	if (!success)
		return 0;

#ifdef CONFIG_X86_64
	/*
	 * Opportunistic SYSRETL: if possible, try to return using SYSRETL.
	 * SYSRETL is available on all 64-bit CPUs, so we don't need to
	 * bother with SYSEXIT.
	 *
	 * Unlike 64-bit opportunistic SYSRET, we can't check that CX == IP,
	 * because the ECX fixup above will ensure that this is essentially
	 * never the case.
	 */
	return regs->cs == __USER32_CS && regs->ss == __USER_DS &&
		regs->ip == landing_pad &&
		(regs->flags & (X86_EFLAGS_RF | X86_EFLAGS_TF)) == 0;
#else
	/*
	 * Opportunistic SYSEXIT: if possible, try to return using SYSEXIT.
	 *
	 * Unlike 64-bit opportunistic SYSRET, we can't check that CX == IP,
	 * because the ECX fixup above will ensure that this is essentially
	 * never the case.
	 *
	 * We don't allow syscalls at all from VM86 mode, but we still
	 * need to check VM, because we might be returning from sys_vm86.
	 */
	return static_cpu_has(X86_FEATURE_SEP) &&
		regs->cs == __USER_CS && regs->ss == __USER_DS &&
		regs->ip == landing_pad &&
		(regs->flags & (X86_EFLAGS_RF | X86_EFLAGS_TF | X86_EFLAGS_VM)) == 0;
#endif
}
#endif

SYSCALL_DEFINE0(ni_syscall)
{
	return -ENOSYS;
}

/**
 * idtentry_enter - Handle state tracking on idtentry
 * @regs:	Pointer to pt_regs of interrupted context
 *
 * Invokes:
 *  - lockdep irqflag state tracking as low level ASM entry disabled
 *    interrupts.
 *
 *  - Context tracking if the exception hit user mode.
 *
 *  - RCU notification if the exception hit kernel mode.
 *
 *  - The hardirq tracer to keep the state consistent as low level ASM
 *    entry disabled interrupts.
 */
void noinstr idtentry_enter(struct pt_regs *regs)
{
	if (user_mode(regs)) {
		enter_from_user_mode();
	} else {
		lockdep_hardirqs_off(CALLER_ADDR0);
		rcu_irq_enter();
		instrumentation_begin();
		trace_hardirqs_off_prepare();
		instrumentation_end();
	}
}

/**
 * idtentry_exit - Common code to handle return from exceptions
 * @regs:	Pointer to pt_regs (exception entry regs)
 *
 * Depending on the return target (kernel/user) this runs the necessary
 * preemption and work checks if possible and required and returns to
 * the caller with interrupts disabled and no further work pending.
 *
 * This is the last action before returning to the low level ASM code which
 * just needs to return to the appropriate context.
 *
 * Invoked by all exception/interrupt IDTENTRY handlers which are not
 * returning through the paranoid exit path (all except NMI, #DF and the IST
 * variants of #MC and #DB) and are therefore on the thread stack.
 */
void noinstr idtentry_exit(struct pt_regs *regs)
{
	lockdep_assert_irqs_disabled();

	/* Check whether this returns to user mode */
	if (user_mode(regs)) {
		prepare_exit_to_usermode(regs);
	} else if (regs->flags & X86_EFLAGS_IF) {
		/* Check kernel preemption, if enabled */
		if (IS_ENABLED(CONFIG_PREEMPTION)) {
			/*
			 * This needs to be done very carefully.
			 * idtentry_enter() invoked rcu_irq_enter(). This
			 * needs to be undone before scheduling.
			 *
			 * Preemption is disabled inside of RCU idle
			 * sections. When the task returns from
			 * preempt_schedule_irq(), RCU is still watching.
			 *
			 * rcu_irq_exit_preempt() has additional state
			 * checking if CONFIG_PROVE_RCU=y
			 */
			if (!preempt_count()) {
				if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
					WARN_ON_ONCE(!on_thread_stack());
				instrumentation_begin();
				rcu_irq_exit_preempt();
				if (need_resched())
					preempt_schedule_irq();
				/* Covers both tracing and lockdep */
				trace_hardirqs_on();
				instrumentation_end();
				return;
			}
		}
		/*
		 * If preemption is disabled then this needs to be done
		 * carefully with respect to RCU. The exception might come
		 * from a RCU idle section in the idle task due to the fact
		 * that safe_halt() enables interrupts. So this needs the
		 * same ordering of lockdep/tracing and RCU as the return
		 * to user mode path.
		 */
		instrumentation_begin();
		/* Tell the tracer that IRET will enable interrupts */
		trace_hardirqs_on_prepare();
		lockdep_hardirqs_on_prepare(CALLER_ADDR0);
		instrumentation_end();
		rcu_irq_exit();
		lockdep_hardirqs_on(CALLER_ADDR0);
	} else {
		/* IRQ flags state is correct already. Just tell RCU */
		rcu_irq_exit();
	}
}
