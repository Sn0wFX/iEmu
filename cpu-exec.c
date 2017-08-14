/*
 *  i386 emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include "exec.h"
#include "disas.h"
#include "tcg.h"
#include "kvm.h"
#include "qemu-barrier.h"

#if !defined(CONFIG_SOFTMMU)
#undef EAX
#undef ECX
#undef EDX
#undef EBX
#undef ESP
#undef EBP
#undef ESI
#undef EDI
#undef EIP
#include <signal.h>
#ifdef __linux__
#include <sys/ucontext.h>
#endif
#endif

#if defined(__sparc__) && !defined(CONFIG_SOLARIS)
// Work around ugly bugs in glibc that mangle global register contents
#undef env
#define env cpu_single_env
#endif

int tb_invalidated_flag;

//#define CONFIG_DEBUG_EXEC
//#define DEBUG_SIGNAL

int qemu_cpu_has_work(struct CPUARMState *env)
{
    return cpu_has_work(env);
}

void cpu_loop_exit(void)
{
    env->current_tb = NULL;
    longjmp(env->jmp_env, 1);
}

/* exit the current TB from a signal handler. The host registers are
   restored in a state compatible with the CPU emulator
 */
void cpu_resume_from_signal(struct CPUARMState *env1, void *puc)
{
#if !defined(CONFIG_SOFTMMU)
#ifdef __linux__
    struct ucontext *uc = puc;
#elif defined(__OpenBSD__)
    struct sigcontext *uc = puc;
#endif
#endif

    env = env1;

    /* XXX: restore cpu registers saved in host registers */

#if !defined(CONFIG_SOFTMMU)
    if (puc) {
        /* XXX: use siglongjmp ? */
#ifdef __linux__
#ifdef __ia64
        sigprocmask(SIG_SETMASK, (sigset_t *)&uc->uc_sigmask, NULL);
#else
        sigprocmask(SIG_SETMASK, &uc->uc_sigmask, NULL);
#endif
#elif defined(__OpenBSD__)
        sigprocmask(SIG_SETMASK, &uc->sc_mask, NULL);
#endif
    }
#endif
    env->exception_index = -1;
    longjmp(env->jmp_env, 1);
}

/* Execute the code without caching the generated code. An interpreter
   could be used if available. */
static void cpu_exec_nocache(int max_cycles, TranslationBlock *orig_tb)
{
    unsigned long next_tb;
    TranslationBlock *tb;

    /* Should never happen.
       We only end up here when an existing TB is too long.  */
    if (max_cycles > CF_COUNT_MASK)
        max_cycles = CF_COUNT_MASK;

    tb = tb_gen_code(env, orig_tb->pc, orig_tb->cs_base, orig_tb->flags,
                     max_cycles);
    env->current_tb = tb;
    /* execute the generated code */
    next_tb = tcg_qemu_tb_exec(tb->tc_ptr);
    env->current_tb = NULL;

    if ((next_tb & 3) == 2) {
        /* Restore PC.  This may happen if async event occurs before
           the TB starts executing.  */
        cpu_pc_from_tb(env, tb);
    }
    tb_phys_invalidate(tb, -1);
    tb_free(tb);
}

static TranslationBlock *tb_find_slow(target_ulong pc,
                                      target_ulong cs_base,
                                      uint64_t flags)
{
    TranslationBlock *tb, **ptb1;
    unsigned int h;
    tb_page_addr_t phys_pc, phys_page1, phys_page2;
    target_ulong virt_page2;

    tb_invalidated_flag = 0;

    /* find translated block using physical mappings */
    phys_pc = get_page_addr_code(env, pc);
    phys_page1 = phys_pc & TARGET_PAGE_MASK;
    phys_page2 = -1;
    h = tb_phys_hash_func(phys_pc);
    ptb1 = &tb_phys_hash[h];
    for(;;) {
        tb = *ptb1;
        if (!tb)
            goto not_found;
        if (tb->pc == pc &&
            tb->page_addr[0] == phys_page1 &&
            tb->cs_base == cs_base &&
            tb->flags == flags) {
            /* check next page if needed */
            if (tb->page_addr[1] != -1) {
                virt_page2 = (pc & TARGET_PAGE_MASK) +
                    TARGET_PAGE_SIZE;
                phys_page2 = get_page_addr_code(env, virt_page2);
                if (tb->page_addr[1] == phys_page2)
                    goto found;
            } else {
                goto found;
            }
        }
        ptb1 = &tb->phys_hash_next;
    }
 not_found:
   /* if no translated code available, then translate it now */
    tb = tb_gen_code(env, pc, cs_base, flags, 0);

 found:
    /* Move the last found TB to the head of the list */
    if (likely(*ptb1)) {
        *ptb1 = tb->phys_hash_next;
        tb->phys_hash_next = tb_phys_hash[h];
        tb_phys_hash[h] = tb;
    }
    /* we add the TB in the virtual pc hash table */
    env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)] = tb;
    return tb;
}

static inline TranslationBlock *tb_find_fast(void)
{
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    int flags;

    /* we record a subset of the CPU state. It will
       always be the same before a given translated block
       is executed. */
    cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
    tb = env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)];
    if (unlikely(!tb || tb->pc != pc || tb->cs_base != cs_base ||
                 tb->flags != flags)) {
        tb = tb_find_slow(pc, cs_base, flags);
    }
    return tb;
}

static CPUDebugExcpHandler *debug_excp_handler;

CPUDebugExcpHandler *cpu_set_debug_excp_handler(CPUDebugExcpHandler *handler)
{
    CPUDebugExcpHandler *old_handler = debug_excp_handler;

    debug_excp_handler = handler;
    return old_handler;
}

static void cpu_handle_debug_exception(struct CPUARMState *env)
{
    CPUWatchpoint *wp;

    if (!env->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }
    if (debug_excp_handler) {
        debug_excp_handler(env);
    }
}

/* main execution loop */

volatile sig_atomic_t exit_request;

/* must only be called from the generated code as an exception can be
   generated */
void tb_invalidate_page_range(target_ulong start, target_ulong end)
{
    /* XXX: cannot enable it yet because it yields to MMU exception
       where NIP != read address on PowerPC */
#if 0
    target_ulong phys_addr;
    phys_addr = get_phys_addr_code(env, start);
    tb_invalidate_phys_page_range(phys_addr, phys_addr + end - start, 0);
#endif
}

#if defined(TARGET_I386) && defined(CONFIG_USER_ONLY)

void cpu_x86_load_seg(CPUX86State *s, int seg_reg, int selector)
{
    CPUX86State *saved_env;

    saved_env = env;
    env = s;
    if (!(env->cr[0] & CR0_PE_MASK) || (env->eflags & VM_MASK)) {
        selector &= 0xffff;
        cpu_x86_load_seg_cache(env, seg_reg, selector,
                               (selector << 4), 0xffff, 0);
    } else {
        helper_load_seg(seg_reg, selector);
    }
    env = saved_env;
}

void cpu_x86_fsave(CPUX86State *s, target_ulong ptr, int data32)
{
    CPUX86State *saved_env;

    saved_env = env;
    env = s;

    helper_fsave(ptr, data32);

    env = saved_env;
}

void cpu_x86_frstor(CPUX86State *s, target_ulong ptr, int data32)
{
    CPUX86State *saved_env;

    saved_env = env;
    env = s;

    helper_frstor(ptr, data32);

    env = saved_env;
}

#endif /* TARGET_I386 */

#if !defined(CONFIG_SOFTMMU)

#if defined(TARGET_I386)
#define EXCEPTION_ACTION raise_exception_err(env->exception_index, env->error_code)
#else
#define EXCEPTION_ACTION cpu_loop_exit()
#endif

/* 'pc' is the host PC at which the exception was raised. 'address' is
   the effective address of the memory exception. 'is_write' is 1 if a
   write caused the exception and otherwise 0'. 'old_set' is the
   signal set which should be restored */
static inline int handle_cpu_signal(unsigned long pc, unsigned long address,
                                    int is_write, sigset_t *old_set,
                                    void *puc)
{
    TranslationBlock *tb;
    int ret;

    if (cpu_single_env)
        env = cpu_single_env; /* XXX: find a correct solution for multithread */
#if defined(DEBUG_SIGNAL)
    qemu_printf("qemu: SIGSEGV pc=0x%08lx address=%08lx w=%d oldset=0x%08lx\n",
                pc, address, is_write, *(unsigned long *)old_set);
#endif
    /* XXX: locking issue */
    if (is_write && page_unprotect(h2g(address), pc, puc)) {
        return 1;
    }

    /* see if it is an MMU fault */
    ret = cpu_handle_mmu_fault(env, address, is_write, MMU_USER_IDX, 0);
    if (ret < 0)
        return 0; /* not an MMU fault */
    if (ret == 0)
        return 1; /* the MMU fault was handled without causing real CPU fault */
    /* now we have a real cpu fault */
    tb = tb_find_pc(pc);
    if (tb) {
        /* the PC is inside the translated code. It means that we have
           a virtual CPU fault */
        cpu_restore_state(tb, env, pc, puc);
    }

    /* we restore the process signal mask as the sigreturn should
       do it (XXX: use sigsetjmp) */
    sigprocmask(SIG_SETMASK, old_set, NULL);
    EXCEPTION_ACTION;

    /* never comes here */
    return 1;
}

#if defined(__i386__)

#if defined(__APPLE__)
# include <sys/ucontext.h>

# define EIP_sig(context)  (*((unsigned long*)&(context)->uc_mcontext->ss.eip))
# define TRAP_sig(context)    ((context)->uc_mcontext->es.trapno)
# define ERROR_sig(context)   ((context)->uc_mcontext->es.err)
# define MASK_sig(context)    ((context)->uc_sigmask)
#elif defined (__NetBSD__)
# include <ucontext.h>

# define EIP_sig(context)     ((context)->uc_mcontext.__gregs[_REG_EIP])
# define TRAP_sig(context)    ((context)->uc_mcontext.__gregs[_REG_TRAPNO])
# define ERROR_sig(context)   ((context)->uc_mcontext.__gregs[_REG_ERR])
# define MASK_sig(context)    ((context)->uc_sigmask)
#elif defined (__FreeBSD__) || defined(__DragonFly__)
# include <ucontext.h>

# define EIP_sig(context)  (*((unsigned long*)&(context)->uc_mcontext.mc_eip))
# define TRAP_sig(context)    ((context)->uc_mcontext.mc_trapno)
# define ERROR_sig(context)   ((context)->uc_mcontext.mc_err)
# define MASK_sig(context)    ((context)->uc_sigmask)
#elif defined(__OpenBSD__)
# define EIP_sig(context)     ((context)->sc_eip)
# define TRAP_sig(context)    ((context)->sc_trapno)
# define ERROR_sig(context)   ((context)->sc_err)
# define MASK_sig(context)    ((context)->sc_mask)
#else
# define EIP_sig(context)     ((context)->uc_mcontext.gregs[REG_EIP])
# define TRAP_sig(context)    ((context)->uc_mcontext.gregs[REG_TRAPNO])
# define ERROR_sig(context)   ((context)->uc_mcontext.gregs[REG_ERR])
# define MASK_sig(context)    ((context)->uc_sigmask)
#endif

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
#if defined(__NetBSD__) || defined (__FreeBSD__) || defined(__DragonFly__)
    ucontext_t *uc = puc;
#elif defined(__OpenBSD__)
    struct sigcontext *uc = puc;
#else
    struct ucontext *uc = puc;
#endif
    unsigned long pc;
    int trapno;

#ifndef REG_EIP
/* for glibc 2.1 */
#define REG_EIP    EIP
#define REG_ERR    ERR
#define REG_TRAPNO TRAPNO
#endif
    pc = EIP_sig(uc);
    trapno = TRAP_sig(uc);
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             trapno == 0xe ?
                             (ERROR_sig(uc) >> 1) & 1 : 0,
                             &MASK_sig(uc), puc);
}

#elif defined(__x86_64__)

#ifdef __NetBSD__
#define PC_sig(context)       _UC_MACHINE_PC(context)
#define TRAP_sig(context)     ((context)->uc_mcontext.__gregs[_REG_TRAPNO])
#define ERROR_sig(context)    ((context)->uc_mcontext.__gregs[_REG_ERR])
#define MASK_sig(context)     ((context)->uc_sigmask)
#elif defined(__OpenBSD__)
#define PC_sig(context)       ((context)->sc_rip)
#define TRAP_sig(context)     ((context)->sc_trapno)
#define ERROR_sig(context)    ((context)->sc_err)
#define MASK_sig(context)     ((context)->sc_mask)
#elif defined (__FreeBSD__) || defined(__DragonFly__)
#include <ucontext.h>

#define PC_sig(context)  (*((unsigned long*)&(context)->uc_mcontext.mc_rip))
#define TRAP_sig(context)     ((context)->uc_mcontext.mc_trapno)
#define ERROR_sig(context)    ((context)->uc_mcontext.mc_err)
#define MASK_sig(context)     ((context)->uc_sigmask)
#else
#define PC_sig(context)       ((context)->uc_mcontext.gregs[REG_RIP])
#define TRAP_sig(context)     ((context)->uc_mcontext.gregs[REG_TRAPNO])
#define ERROR_sig(context)    ((context)->uc_mcontext.gregs[REG_ERR])
#define MASK_sig(context)     ((context)->uc_sigmask)
#endif

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    unsigned long pc;
#if defined(__NetBSD__) || defined (__FreeBSD__) || defined(__DragonFly__)
    ucontext_t *uc = puc;
#elif defined(__OpenBSD__)
    struct sigcontext *uc = puc;
#else
    struct ucontext *uc = puc;
#endif

    pc = PC_sig(uc);
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             TRAP_sig(uc) == 0xe ?
                             (ERROR_sig(uc) >> 1) & 1 : 0,
                             &MASK_sig(uc), puc);
}

#elif defined(_ARCH_PPC)

/***********************************************************************
 * signal context platform-specific definitions
 * From Wine
 */
#ifdef linux
/* All Registers access - only for local access */
# define REG_sig(reg_name, context)		((context)->uc_mcontext.regs->reg_name)
/* Gpr Registers access  */
# define GPR_sig(reg_num, context)		REG_sig(gpr[reg_num], context)
# define IAR_sig(context)			REG_sig(nip, context)	/* Program counter */
# define MSR_sig(context)			REG_sig(msr, context)   /* Machine State Register (Supervisor) */
# define CTR_sig(context)			REG_sig(ctr, context)   /* Count register */
# define XER_sig(context)			REG_sig(xer, context) /* User's integer exception register */
# define LR_sig(context)			REG_sig(link, context) /* Link register */
# define CR_sig(context)			REG_sig(ccr, context) /* Condition register */
/* Float Registers access  */
# define FLOAT_sig(reg_num, context)		(((double*)((char*)((context)->uc_mcontext.regs+48*4)))[reg_num])
# define FPSCR_sig(context)			(*(int*)((char*)((context)->uc_mcontext.regs+(48+32*2)*4)))
/* Exception Registers access */
# define DAR_sig(context)			REG_sig(dar, context)
# define DSISR_sig(context)			REG_sig(dsisr, context)
# define TRAP_sig(context)			REG_sig(trap, context)
#endif /* linux */

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <ucontext.h>
# define IAR_sig(context)		((context)->uc_mcontext.mc_srr0)
# define MSR_sig(context)		((context)->uc_mcontext.mc_srr1)
# define CTR_sig(context)		((context)->uc_mcontext.mc_ctr)
# define XER_sig(context)		((context)->uc_mcontext.mc_xer)
# define LR_sig(context)		((context)->uc_mcontext.mc_lr)
# define CR_sig(context)		((context)->uc_mcontext.mc_cr)
/* Exception Registers access */
# define DAR_sig(context)		((context)->uc_mcontext.mc_dar)
# define DSISR_sig(context)		((context)->uc_mcontext.mc_dsisr)
# define TRAP_sig(context)		((context)->uc_mcontext.mc_exc)
#endif /* __FreeBSD__|| __FreeBSD_kernel__ */

#ifdef __APPLE__
# include <sys/ucontext.h>
typedef struct ucontext SIGCONTEXT;
/* All Registers access - only for local access */
# define REG_sig(reg_name, context)		((context)->uc_mcontext->ss.reg_name)
# define FLOATREG_sig(reg_name, context)	((context)->uc_mcontext->fs.reg_name)
# define EXCEPREG_sig(reg_name, context)	((context)->uc_mcontext->es.reg_name)
# define VECREG_sig(reg_name, context)		((context)->uc_mcontext->vs.reg_name)
/* Gpr Registers access */
# define GPR_sig(reg_num, context)		REG_sig(r##reg_num, context)
# define IAR_sig(context)			REG_sig(srr0, context)	/* Program counter */
# define MSR_sig(context)			REG_sig(srr1, context)  /* Machine State Register (Supervisor) */
# define CTR_sig(context)			REG_sig(ctr, context)
# define XER_sig(context)			REG_sig(xer, context) /* Link register */
# define LR_sig(context)			REG_sig(lr, context)  /* User's integer exception register */
# define CR_sig(context)			REG_sig(cr, context)  /* Condition register */
/* Float Registers access */
# define FLOAT_sig(reg_num, context)		FLOATREG_sig(fpregs[reg_num], context)
# define FPSCR_sig(context)			((double)FLOATREG_sig(fpscr, context))
/* Exception Registers access */
# define DAR_sig(context)			EXCEPREG_sig(dar, context)     /* Fault registers for coredump */
# define DSISR_sig(context)			EXCEPREG_sig(dsisr, context)
# define TRAP_sig(context)			EXCEPREG_sig(exception, context) /* number of powerpc exception taken */
#endif /* __APPLE__ */

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    ucontext_t *uc = puc;
#else
    struct ucontext *uc = puc;
#endif
    unsigned long pc;
    int is_write;

    pc = IAR_sig(uc);
    is_write = 0;
#if 0
    /* ppc 4xx case */
    if (DSISR_sig(uc) & 0x00800000)
        is_write = 1;
#else
    if (TRAP_sig(uc) != 0x400 && (DSISR_sig(uc) & 0x02000000))
        is_write = 1;
#endif
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write, &uc->uc_sigmask, puc);
}

#elif defined(__alpha__)

int cpu_signal_handler(int host_signum, void *pinfo,
                           void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    uint32_t *pc = uc->uc_mcontext.sc_pc;
    uint32_t insn = *pc;
    int is_write = 0;

    /* XXX: need kernel patch to get write flag faster */
    switch (insn >> 26) {
    case 0x0d: // stw
    case 0x0e: // stb
    case 0x0f: // stq_u
    case 0x24: // stf
    case 0x25: // stg
    case 0x26: // sts
    case 0x27: // stt
    case 0x2c: // stl
    case 0x2d: // stq
    case 0x2e: // stl_c
    case 0x2f: // stq_c
	is_write = 1;
    }

    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write, &uc->uc_sigmask, puc);
}
#elif defined(__sparc__)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    int is_write;
    uint32_t insn;
#if !defined(__arch64__) || defined(CONFIG_SOLARIS)
    uint32_t *regs = (uint32_t *)(info + 1);
    void *sigmask = (regs + 20);
    /* XXX: is there a standard glibc define ? */
    unsigned long pc = regs[1];
#else
#ifdef __linux__
    struct sigcontext *sc = puc;
    unsigned long pc = sc->sigc_regs.tpc;
    void *sigmask = (void *)sc->sigc_mask;
#elif defined(__OpenBSD__)
    struct sigcontext *uc = puc;
    unsigned long pc = uc->sc_pc;
    void *sigmask = (void *)(long)uc->sc_mask;
#endif
#endif

    /* XXX: need kernel patch to get write flag faster */
    is_write = 0;
    insn = *(uint32_t *)pc;
    if ((insn >> 30) == 3) {
      switch((insn >> 19) & 0x3f) {
      case 0x05: // stb
      case 0x15: // stba
      case 0x06: // sth
      case 0x16: // stha
      case 0x04: // st
      case 0x14: // sta
      case 0x07: // std
      case 0x17: // stda
      case 0x0e: // stx
      case 0x1e: // stxa
      case 0x24: // stf
      case 0x34: // stfa
      case 0x27: // stdf
      case 0x37: // stdfa
      case 0x26: // stqf
      case 0x36: // stqfa
      case 0x25: // stfsr
      case 0x3c: // casa
      case 0x3e: // casxa
	is_write = 1;
	break;
      }
    }
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write, sigmask, NULL);
}

#elif defined(__arm__)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    unsigned long pc;
    int is_write;

#if (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ <= 3))
    pc = uc->uc_mcontext.gregs[R15];
#else
    pc = uc->uc_mcontext.arm_pc;
#endif
    /* XXX: compute is_write */
    is_write = 0;
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write,
                             &uc->uc_sigmask, puc);
}

#elif defined(__mc68000)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    unsigned long pc;
    int is_write;

    pc = uc->uc_mcontext.gregs[16];
    /* XXX: compute is_write */
    is_write = 0;
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write,
                             &uc->uc_sigmask, puc);
}

#elif defined(__ia64)

#ifndef __ISR_VALID
  /* This ought to be in <bits/siginfo.h>... */
# define __ISR_VALID	1
#endif

int cpu_signal_handler(int host_signum, void *pinfo, void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    unsigned long ip;
    int is_write = 0;

    ip = uc->uc_mcontext.sc_ip;
    switch (host_signum) {
      case SIGILL:
      case SIGFPE:
      case SIGSEGV:
      case SIGBUS:
      case SIGTRAP:
	  if (info->si_code && (info->si_segvflags & __ISR_VALID))
	      /* ISR.W (write-access) is bit 33:  */
	      is_write = (info->si_isr >> 33) & 1;
	  break;

      default:
	  break;
    }
    return handle_cpu_signal(ip, (unsigned long)info->si_addr,
                             is_write,
                             (sigset_t *)&uc->uc_sigmask, puc);
}

#elif defined(__s390__)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    unsigned long pc;
    uint16_t *pinsn;
    int is_write = 0;

    pc = uc->uc_mcontext.psw.addr;

    /* ??? On linux, the non-rt signal handler has 4 (!) arguments instead
       of the normal 2 arguments.  The 3rd argument contains the "int_code"
       from the hardware which does in fact contain the is_write value.
       The rt signal handler, as far as I can tell, does not give this value
       at all.  Not that we could get to it from here even if it were.  */
    /* ??? This is not even close to complete, since it ignores all
       of the read-modify-write instructions.  */
    pinsn = (uint16_t *)pc;
    switch (pinsn[0] >> 8) {
    case 0x50: /* ST */
    case 0x42: /* STC */
    case 0x40: /* STH */
        is_write = 1;
        break;
    case 0xc4: /* RIL format insns */
        switch (pinsn[0] & 0xf) {
        case 0xf: /* STRL */
        case 0xb: /* STGRL */
        case 0x7: /* STHRL */
            is_write = 1;
        }
        break;
    case 0xe3: /* RXY format insns */
        switch (pinsn[2] & 0xff) {
        case 0x50: /* STY */
        case 0x24: /* STG */
        case 0x72: /* STCY */
        case 0x70: /* STHY */
        case 0x8e: /* STPQ */
        case 0x3f: /* STRVH */
        case 0x3e: /* STRV */
        case 0x2f: /* STRVG */
            is_write = 1;
        }
        break;
    }
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write, &uc->uc_sigmask, puc);
}

#elif defined(__mips__)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    siginfo_t *info = pinfo;
    struct ucontext *uc = puc;
    greg_t pc = uc->uc_mcontext.pc;
    int is_write;

    /* XXX: compute is_write */
    is_write = 0;
    return handle_cpu_signal(pc, (unsigned long)info->si_addr,
                             is_write, &uc->uc_sigmask, puc);
}

#elif defined(__hppa__)

int cpu_signal_handler(int host_signum, void *pinfo,
                       void *puc)
{
    struct siginfo *info = pinfo;
    struct ucontext *uc = puc;
    unsigned long pc = uc->uc_mcontext.sc_iaoq[0];
    uint32_t insn = *(uint32_t *)pc;
    int is_write = 0;

    /* XXX: need kernel patch to get write flag faster.  */
    switch (insn >> 26) {
    case 0x1a: /* STW */
    case 0x19: /* STH */
    case 0x18: /* STB */
    case 0x1b: /* STWM */
        is_write = 1;
        break;

    case 0x09: /* CSTWX, FSTWX, FSTWS */
    case 0x0b: /* CSTDX, FSTDX, FSTDS */
        /* Distinguish from coprocessor load ... */
        is_write = (insn >> 9) & 1;
        break;

    case 0x03:
        switch ((insn >> 6) & 15) {
        case 0xa: /* STWS */
        case 0x9: /* STHS */
        case 0x8: /* STBS */
        case 0xe: /* STWAS */
        case 0xc: /* STBYS */
            is_write = 1;
        }
        break;
    }

    return handle_cpu_signal(pc, (unsigned long)info->si_addr, 
                             is_write, &uc->uc_sigmask, puc);
}

#else

#error host CPU specific signal handler needed

#endif

#endif /* !defined(CONFIG_SOFTMMU) */
