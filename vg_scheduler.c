
/*--------------------------------------------------------------------*/
/*--- A user-space pthreads implementation.         vg_scheduler.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, an x86 protected-mode emulator 
   designed for debugging and profiling binaries on x86-Unixes.

   Copyright (C) 2000-2002 Julian Seward 
      jseward@acm.org
      Julian_Seward@muraroa.demon.co.uk

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file LICENSE.
*/

#include "vg_include.h"
#include "vg_constants.h"

#include "valgrind.h" /* for VG_USERREQ__MAKE_NOACCESS and
                         VG_USERREQ__DO_LEAK_CHECK */

/* BORKAGE/ISSUES as of 14 Apr 02

Note!  This pthreads implementation is so poor as to not be
suitable for use by anyone at all!

- Currently, when a signal is run, just the ThreadStatus.status fields 
  are saved in the signal frame, along with the CPU state.  Question: 
  should I also save and restore:
     ThreadStatus.joiner 
     ThreadStatus.waited_on_mid
     ThreadStatus.awaken_at
     ThreadStatus.retval
  Currently unsure, and so am not doing so.

- Signals interrupting read/write and nanosleep: SA_RESTART settings.
  Read/write correctly return with EINTR when SA_RESTART isn't
  specified and they are interrupted by a signal.  nanosleep just
  pretends signals don't exist -- should be fixed.

- when a thread is done mark its stack as noaccess 

- 0xDEADBEEF syscall errors ... fix.

*/


/* ---------------------------------------------------------------------
   Types and globals for the scheduler.
   ------------------------------------------------------------------ */

/* type ThreadId is defined in vg_include.h. */

/* struct ThreadState is defined in vg_include.h. */

/* Private globals.  A statically allocated array of threads. */
static ThreadState vg_threads[VG_N_THREADS];


/* vg_oursignalhandler() might longjmp().  Here's the jmp_buf. */
jmp_buf VG_(scheduler_jmpbuf);
/* ... and if so, here's the signal which caused it to do so. */
Int     VG_(longjmpd_on_signal);


/* Machinery to keep track of which threads are waiting on which
   fds. */
typedef
   struct {
      /* The thread which made the request. */
      ThreadId tid;

      /* The next two fields describe the request. */
      /* File descriptor waited for.  -1 means this slot is not in use */
      Int      fd;
      /* The syscall number the fd is used in. */
      Int      syscall_no;

      /* False => still waiting for select to tell us the fd is ready
         to go.  True => the fd is ready, but the results have not yet
         been delivered back to the calling thread.  Once the latter
         happens, this entire record is marked as no longer in use, by
         making the fd field be -1.  */
      Bool     ready; 
   }
   VgWaitedOnFd;

static VgWaitedOnFd vg_waiting_fds[VG_N_WAITING_FDS];



typedef
   struct {
      /* Is this slot in use, or free? */
      Bool in_use;
      /* If in_use, is this mutex held by some thread, or not? */
      Bool held;
      /* if held==True, owner indicates who by. */
      ThreadId owner;
   }
   VgMutex;

static VgMutex vg_mutexes[VG_N_MUTEXES];

/* Forwards */
static void do_nontrivial_clientreq ( ThreadId tid );


/* ---------------------------------------------------------------------
   Helper functions for the scheduler.
   ------------------------------------------------------------------ */

static
void pp_sched_status ( void )
{
   Int i; 
   VG_(printf)("\nsched status:\n"); 
   for (i = 0; i < VG_N_THREADS; i++) {
      if (vg_threads[i].status == VgTs_Empty) continue;
      VG_(printf)("tid %d:  ", i);
      switch (vg_threads[i].status) {
         case VgTs_Runnable:   VG_(printf)("Runnable\n"); break;
         case VgTs_WaitFD:     VG_(printf)("WaitFD\n"); break;
         case VgTs_WaitJoiner: VG_(printf)("WaitJoiner(%d)\n", 
                                           vg_threads[i].joiner); break;
         case VgTs_WaitJoinee: VG_(printf)("WaitJoinee\n"); break;
         default: VG_(printf)("???"); break;
      }
   }
   VG_(printf)("\n");
}

static
void add_waiting_fd ( ThreadId tid, Int fd, Int syscall_no )
{
   Int i;

   vg_assert(fd != -1); /* avoid total chaos */

   for (i = 0;  i < VG_N_WAITING_FDS; i++)
      if (vg_waiting_fds[i].fd == -1)
         break;

   if (i == VG_N_WAITING_FDS)
      VG_(panic)("add_waiting_fd: VG_N_WAITING_FDS is too low");
   /*
   VG_(printf)("add_waiting_fd: add (tid %d, fd %d) at slot %d\n", 
               tid, fd, i);
   */
   vg_waiting_fds[i].fd         = fd;
   vg_waiting_fds[i].tid        = tid;
   vg_waiting_fds[i].ready      = False;
   vg_waiting_fds[i].syscall_no = syscall_no;
}



static
void print_sched_event ( ThreadId tid, Char* what )
{
   VG_(message)(Vg_DebugMsg, "SCHED[%d]: %s", tid, what );
}


static
void print_pthread_event ( ThreadId tid, Char* what )
{
   VG_(message)(Vg_DebugMsg, "PTHREAD[%d]: %s", tid, what );
}


static
Char* name_of_sched_event ( UInt event )
{
   switch (event) {
      case VG_TRC_EBP_JMP_SYSCALL:    return "SYSCALL";
      case VG_TRC_EBP_JMP_CLIENTREQ:  return "CLIENTREQ";
      case VG_TRC_INNER_COUNTERZERO:  return "COUNTERZERO";
      case VG_TRC_INNER_FASTMISS:     return "FASTMISS";
      case VG_TRC_UNRESUMABLE_SIGNAL: return "FATALSIGNAL";
      default:                        return "??UNKNOWN??";
  }
}


/* Create a translation of the client basic block beginning at
   orig_addr, and add it to the translation cache & translation table.
   This probably doesn't really belong here, but, hey ... 
*/
void VG_(create_translation_for) ( Addr orig_addr )
{
   Addr    trans_addr;
   TTEntry tte;
   Int orig_size, trans_size;
   /* Ensure there is space to hold a translation. */
   VG_(maybe_do_lru_pass)();
   VG_(translate)( orig_addr, &orig_size, &trans_addr, &trans_size );
   /* Copy data at trans_addr into the translation cache.
      Returned pointer is to the code, not to the 4-byte
      header. */
   /* Since the .orig_size and .trans_size fields are
      UShort, be paranoid. */
   vg_assert(orig_size > 0 && orig_size < 65536);
   vg_assert(trans_size > 0 && trans_size < 65536);
   tte.orig_size  = orig_size;
   tte.orig_addr  = orig_addr;
   tte.trans_size = trans_size;
   tte.trans_addr = VG_(copy_to_transcache)
                       ( trans_addr, trans_size );
   tte.mru_epoch  = VG_(current_epoch);
   /* Free the intermediary -- was allocated by VG_(emit_code). */
   VG_(jitfree)( (void*)trans_addr );
   /* Add to trans tab and set back pointer. */
   VG_(add_to_trans_tab) ( &tte );
   /* Update stats. */
   VG_(this_epoch_in_count) ++;
   VG_(this_epoch_in_osize) += orig_size;
   VG_(this_epoch_in_tsize) += trans_size;
   VG_(overall_in_count) ++;
   VG_(overall_in_osize) += orig_size;
   VG_(overall_in_tsize) += trans_size;
   /* Record translated area for SMC detection. */
   VG_(smc_mark_original) ( orig_addr, orig_size );
}


/* Allocate a completely empty ThreadState record. */
static
ThreadId vg_alloc_ThreadState ( void )
{
   Int i;
   for (i = 0; i < VG_N_THREADS; i++) {
      if (vg_threads[i].status == VgTs_Empty)
         return i;
   }
   VG_(printf)("vg_alloc_ThreadState: no free slots available\n");
   VG_(printf)("Increase VG_N_THREADS, rebuild and try again.\n");
   VG_(panic)("VG_N_THREADS is too low");
   /*NOTREACHED*/
}


ThreadState* VG_(get_thread_state) ( ThreadId tid )
{
   vg_assert(tid >= 0 && tid < VG_N_THREADS);
   vg_assert(vg_threads[tid].status != VgTs_Empty);
   return & vg_threads[tid];
}


/* Find an unused VgMutex record. */
static
MutexId vg_alloc_VgMutex ( void )
{
   Int i;
   for (i = 0; i < VG_N_MUTEXES; i++) {
      if (!vg_mutexes[i].in_use)
         return i;
   }
   VG_(printf)("vg_alloc_VgMutex: no free slots available\n");
   VG_(printf)("Increase VG_N_MUTEXES, rebuild and try again.\n");
   VG_(panic)("VG_N_MUTEXES is too low");
   /*NOTREACHED*/
}


/* Copy the saved state of a thread into VG_(baseBlock), ready for it
   to be run. */
__inline__
void VG_(load_thread_state) ( ThreadId tid )
{
   Int i;
   VG_(baseBlock)[VGOFF_(m_eax)] = vg_threads[tid].m_eax;
   VG_(baseBlock)[VGOFF_(m_ebx)] = vg_threads[tid].m_ebx;
   VG_(baseBlock)[VGOFF_(m_ecx)] = vg_threads[tid].m_ecx;
   VG_(baseBlock)[VGOFF_(m_edx)] = vg_threads[tid].m_edx;
   VG_(baseBlock)[VGOFF_(m_esi)] = vg_threads[tid].m_esi;
   VG_(baseBlock)[VGOFF_(m_edi)] = vg_threads[tid].m_edi;
   VG_(baseBlock)[VGOFF_(m_ebp)] = vg_threads[tid].m_ebp;
   VG_(baseBlock)[VGOFF_(m_esp)] = vg_threads[tid].m_esp;
   VG_(baseBlock)[VGOFF_(m_eflags)] = vg_threads[tid].m_eflags;
   VG_(baseBlock)[VGOFF_(m_eip)] = vg_threads[tid].m_eip;

   for (i = 0; i < VG_SIZE_OF_FPUSTATE_W; i++)
      VG_(baseBlock)[VGOFF_(m_fpustate) + i] = vg_threads[tid].m_fpu[i];

   VG_(baseBlock)[VGOFF_(sh_eax)] = vg_threads[tid].sh_eax;
   VG_(baseBlock)[VGOFF_(sh_ebx)] = vg_threads[tid].sh_ebx;
   VG_(baseBlock)[VGOFF_(sh_ecx)] = vg_threads[tid].sh_ecx;
   VG_(baseBlock)[VGOFF_(sh_edx)] = vg_threads[tid].sh_edx;
   VG_(baseBlock)[VGOFF_(sh_esi)] = vg_threads[tid].sh_esi;
   VG_(baseBlock)[VGOFF_(sh_edi)] = vg_threads[tid].sh_edi;
   VG_(baseBlock)[VGOFF_(sh_ebp)] = vg_threads[tid].sh_ebp;
   VG_(baseBlock)[VGOFF_(sh_esp)] = vg_threads[tid].sh_esp;
   VG_(baseBlock)[VGOFF_(sh_eflags)] = vg_threads[tid].sh_eflags;
}


/* Copy the state of a thread from VG_(baseBlock), presumably after it
   has been descheduled.  For sanity-check purposes, fill the vacated
   VG_(baseBlock) with garbage so as to make the system more likely to
   fail quickly if we erroneously continue to poke around inside
   VG_(baseBlock) without first doing a load_thread_state().  
*/
__inline__
void VG_(save_thread_state) ( ThreadId tid )
{
   Int i;
   const UInt junk = 0xDEADBEEF;

   vg_threads[tid].m_eax = VG_(baseBlock)[VGOFF_(m_eax)];
   vg_threads[tid].m_ebx = VG_(baseBlock)[VGOFF_(m_ebx)];
   vg_threads[tid].m_ecx = VG_(baseBlock)[VGOFF_(m_ecx)];
   vg_threads[tid].m_edx = VG_(baseBlock)[VGOFF_(m_edx)];
   vg_threads[tid].m_esi = VG_(baseBlock)[VGOFF_(m_esi)];
   vg_threads[tid].m_edi = VG_(baseBlock)[VGOFF_(m_edi)];
   vg_threads[tid].m_ebp = VG_(baseBlock)[VGOFF_(m_ebp)];
   vg_threads[tid].m_esp = VG_(baseBlock)[VGOFF_(m_esp)];
   vg_threads[tid].m_eflags = VG_(baseBlock)[VGOFF_(m_eflags)];
   vg_threads[tid].m_eip = VG_(baseBlock)[VGOFF_(m_eip)];

   for (i = 0; i < VG_SIZE_OF_FPUSTATE_W; i++)
      vg_threads[tid].m_fpu[i] = VG_(baseBlock)[VGOFF_(m_fpustate) + i];

   vg_threads[tid].sh_eax = VG_(baseBlock)[VGOFF_(sh_eax)];
   vg_threads[tid].sh_ebx = VG_(baseBlock)[VGOFF_(sh_ebx)];
   vg_threads[tid].sh_ecx = VG_(baseBlock)[VGOFF_(sh_ecx)];
   vg_threads[tid].sh_edx = VG_(baseBlock)[VGOFF_(sh_edx)];
   vg_threads[tid].sh_esi = VG_(baseBlock)[VGOFF_(sh_esi)];
   vg_threads[tid].sh_edi = VG_(baseBlock)[VGOFF_(sh_edi)];
   vg_threads[tid].sh_ebp = VG_(baseBlock)[VGOFF_(sh_ebp)];
   vg_threads[tid].sh_esp = VG_(baseBlock)[VGOFF_(sh_esp)];
   vg_threads[tid].sh_eflags = VG_(baseBlock)[VGOFF_(sh_eflags)];

   /* Fill it up with junk. */
   VG_(baseBlock)[VGOFF_(m_eax)] = junk;
   VG_(baseBlock)[VGOFF_(m_ebx)] = junk;
   VG_(baseBlock)[VGOFF_(m_ecx)] = junk;
   VG_(baseBlock)[VGOFF_(m_edx)] = junk;
   VG_(baseBlock)[VGOFF_(m_esi)] = junk;
   VG_(baseBlock)[VGOFF_(m_edi)] = junk;
   VG_(baseBlock)[VGOFF_(m_ebp)] = junk;
   VG_(baseBlock)[VGOFF_(m_esp)] = junk;
   VG_(baseBlock)[VGOFF_(m_eflags)] = junk;
   VG_(baseBlock)[VGOFF_(m_eip)] = junk;

   for (i = 0; i < VG_SIZE_OF_FPUSTATE_W; i++)
      VG_(baseBlock)[VGOFF_(m_fpustate) + i] = junk;
}


/* Run the thread tid for a while, and return a VG_TRC_* value to the
   scheduler indicating what happened. */
static 
UInt run_thread_for_a_while ( ThreadId tid )
{
   UInt trc = 0;
   vg_assert(tid >= 0 && tid < VG_N_THREADS);
   vg_assert(vg_threads[tid].status != VgTs_Empty);
   vg_assert(VG_(bbs_to_go) > 0);

   VG_(load_thread_state) ( tid );
   if (__builtin_setjmp(VG_(scheduler_jmpbuf)) == 0) {
      /* try this ... */
      trc = VG_(run_innerloop)();
      /* We get here if the client didn't take a fault. */
   } else {
      /* We get here if the client took a fault, which caused our
         signal handler to longjmp. */
      vg_assert(trc == 0);
      trc = VG_TRC_UNRESUMABLE_SIGNAL;
   }
   VG_(save_thread_state) ( tid );
   return trc;
}


/* Increment the LRU epoch counter. */
static
void increment_epoch ( void )
{
   VG_(current_epoch)++;
   if (VG_(clo_verbosity) > 2) {
      UInt tt_used, tc_used;
      VG_(get_tt_tc_used) ( &tt_used, &tc_used );
      VG_(message)(Vg_UserMsg,
         "%lu bbs, in: %d (%d -> %d), out %d (%d -> %d), TT %d, TC %d",
          VG_(bbs_done), 
          VG_(this_epoch_in_count),
          VG_(this_epoch_in_osize),
          VG_(this_epoch_in_tsize),
          VG_(this_epoch_out_count),
          VG_(this_epoch_out_osize),
          VG_(this_epoch_out_tsize),
          tt_used, tc_used
       );
   }
   VG_(this_epoch_in_count) = 0;
   VG_(this_epoch_in_osize) = 0;
   VG_(this_epoch_in_tsize) = 0;
   VG_(this_epoch_out_count) = 0;
   VG_(this_epoch_out_osize) = 0;
   VG_(this_epoch_out_tsize) = 0;
}


/* Initialise the scheduler.  Create a single "main" thread ready to
   run, with special ThreadId of zero.  This is called at startup; the
   caller takes care to park the client's state is parked in
   VG_(baseBlock).  
*/
void VG_(scheduler_init) ( void )
{
   Int      i;
   Addr     startup_esp;
   ThreadId tid_main;

   startup_esp = VG_(baseBlock)[VGOFF_(m_esp)];
   if ((startup_esp & VG_STARTUP_STACK_MASK) != VG_STARTUP_STACK_MASK) {
      VG_(printf)("%esp at startup = %p is not near %p; aborting\n", 
                  startup_esp, VG_STARTUP_STACK_MASK);
      VG_(panic)("unexpected %esp at startup");
   }

   for (i = 0; i < VG_N_THREADS; i++) {
      vg_threads[i].stack_size = 0;
      vg_threads[i].stack_base = (Addr)NULL;
   }

   for (i = 0; i < VG_N_WAITING_FDS; i++)
      vg_waiting_fds[i].fd = -1; /* not in use */

   for (i = 0; i < VG_N_MUTEXES; i++)
      vg_mutexes[i].in_use = False;

   /* Assert this is thread zero, which has certain magic
      properties. */
   tid_main = vg_alloc_ThreadState();
   vg_assert(tid_main == 0); 

   vg_threads[tid_main].status      = VgTs_Runnable;
   vg_threads[tid_main].joiner      = VG_INVALID_THREADID;
   vg_threads[tid_main].retval      = NULL; /* not important */

   /* Copy VG_(baseBlock) state to tid_main's slot. */
   VG_(save_thread_state) ( tid_main );
}


/* What if fd isn't a valid fd? */
static
void set_fd_nonblocking ( Int fd )
{
   Int res = VG_(fcntl)( fd, VKI_F_GETFL, 0 );
   vg_assert(!VG_(is_kerror)(res));
   res |= VKI_O_NONBLOCK;
   res = VG_(fcntl)( fd, VKI_F_SETFL, res );
   vg_assert(!VG_(is_kerror)(res));
}

static
void set_fd_blocking ( Int fd )
{
   Int res = VG_(fcntl)( fd, VKI_F_GETFL, 0 );
   vg_assert(!VG_(is_kerror)(res));
   res &= ~VKI_O_NONBLOCK;
   res = VG_(fcntl)( fd, VKI_F_SETFL, res );
   vg_assert(!VG_(is_kerror)(res));
}

static
Bool fd_is_blockful ( Int fd )
{
   Int res = VG_(fcntl)( fd, VKI_F_GETFL, 0 );
   vg_assert(!VG_(is_kerror)(res));
   return (res & VKI_O_NONBLOCK) ? False : True;
}



/* Do a purely thread-local request for tid, and put the result in its
   %EDX, without changing its scheduling state in any way, nor that of
   any other threads.  Return True if so.

   If the request is non-trivial, return False; a more capable but
   slower mechanism will deal with it.  
*/
static 
Bool maybe_do_trivial_clientreq ( ThreadId tid )
{
#  define SIMPLE_RETURN(vvv)                      \
       { vg_threads[tid].m_edx = (vvv);           \
         return True;                             \
       }

   UInt* arg    = (UInt*)(vg_threads[tid].m_eax);
   UInt  req_no = arg[0];
   switch (req_no) {
      case VG_USERREQ__MALLOC:
         SIMPLE_RETURN(
            (UInt)VG_(client_malloc) ( arg[1], Vg_AllocMalloc ) 
         );
      case VG_USERREQ__BUILTIN_NEW:
         SIMPLE_RETURN(
            (UInt)VG_(client_malloc) ( arg[1], Vg_AllocNew )
         );
      case VG_USERREQ__BUILTIN_VEC_NEW:
         SIMPLE_RETURN(
            (UInt)VG_(client_malloc) ( arg[1], Vg_AllocNewVec )
         );
      case VG_USERREQ__FREE:
         VG_(client_free) ( (void*)arg[1], Vg_AllocMalloc );
	 SIMPLE_RETURN(0); /* irrelevant */
      case VG_USERREQ__BUILTIN_DELETE:
         VG_(client_free) ( (void*)arg[1], Vg_AllocNew );
	 SIMPLE_RETURN(0); /* irrelevant */
      case VG_USERREQ__BUILTIN_VEC_DELETE:
         VG_(client_free) ( (void*)arg[1], Vg_AllocNewVec );
	 SIMPLE_RETURN(0); /* irrelevant */
      case VG_USERREQ__CALLOC:
         SIMPLE_RETURN(
            (UInt)VG_(client_calloc) ( arg[1], arg[2] )
         );
      case VG_USERREQ__REALLOC:
         SIMPLE_RETURN(
            (UInt)VG_(client_realloc) ( (void*)arg[1], arg[2] )
         );
      case VG_USERREQ__MEMALIGN:
         SIMPLE_RETURN(
            (UInt)VG_(client_memalign) ( arg[1], arg[2] )
         );
      default:
         /* Too hard; wimp out. */
         return False;
   }
#  undef SIMPLE_RETURN
}


static
void sched_do_syscall ( ThreadId tid )
{
   UInt saved_eax;
   UInt res, syscall_no;
   UInt fd;
   Bool might_block, assumed_nonblocking;
   Bool orig_fd_blockness;
   Char msg_buf[100];

   vg_assert(tid >= 0 && tid < VG_N_THREADS);
   vg_assert(vg_threads[tid].status == VgTs_Runnable);

   syscall_no = vg_threads[tid].m_eax; /* syscall number */

   if (syscall_no == __NR_nanosleep) {
      ULong t_now, t_awaken;
      struct vki_timespec* req;
      req = (struct vki_timespec*)vg_threads[tid].m_ebx; /* arg1 */
      t_now = VG_(read_microsecond_timer)();     
      t_awaken 
         = t_now
           + (ULong)1000000ULL * (ULong)(req->tv_sec) 
           + (ULong)( (UInt)(req->tv_nsec) / 1000 );
      vg_threads[tid].status    = VgTs_Sleeping;
      vg_threads[tid].awaken_at = t_awaken;
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf, "at %lu: nanosleep for %lu", 
                               t_now, t_awaken-t_now);
	 print_sched_event(tid, msg_buf);
      }
      /* Force the scheduler to run something else for a while. */
      return;
   }

   switch (syscall_no) {
      case __NR_read:
      case __NR_write:
         assumed_nonblocking 
            = False;
         might_block 
            = fd_is_blockful(vg_threads[tid].m_ebx /* arg1 */);
         break;
      default: 
         might_block = False;
         assumed_nonblocking = True;
   }
   
   if (assumed_nonblocking) {
      /* We think it's non-blocking.  Just do it in the normal way. */
      VG_(perform_assumed_nonblocking_syscall)(tid);
      /* The thread is still runnable. */
      return;
   }

   /* It might block.  Take evasive action. */
   switch (syscall_no) {
      case __NR_read:
      case __NR_write:
         fd = vg_threads[tid].m_ebx; break;
      default:
         vg_assert(3+3 == 7);
   }

   /* Set the fd to nonblocking, and do the syscall, which will return
      immediately, in order to lodge a request with the Linux kernel.
      We later poll for I/O completion using select().  */

   orig_fd_blockness = fd_is_blockful(fd);
   set_fd_nonblocking(fd);
   vg_assert(!fd_is_blockful(fd));
   VG_(check_known_blocking_syscall)(tid, syscall_no, NULL /* PRE */);

   /* This trashes the thread's %eax; we have to preserve it. */
   saved_eax = vg_threads[tid].m_eax;
   KERNEL_DO_SYSCALL(tid,res);

   /* Restore original blockfulness of the fd. */
   if (orig_fd_blockness)
      set_fd_blocking(fd);
   else
      set_fd_nonblocking(fd);

   if (res != -VKI_EWOULDBLOCK) {
      /* It didn't block; it went through immediately.  So finish off
         in the normal way.  Don't restore %EAX, since that now
         (correctly) holds the result of the call. */
      VG_(check_known_blocking_syscall)(tid, syscall_no, &res /* POST */);
      /* We're still runnable. */
      vg_assert(vg_threads[tid].status == VgTs_Runnable);

   } else {

      /* It would have blocked.  First, restore %EAX to what it was
         before our speculative call. */
      vg_threads[tid].m_eax = saved_eax;
      /* Put this fd in a table of fds on which we are waiting for
         completion. The arguments for select() later are constructed
         from this table.  */
      add_waiting_fd(tid, fd, saved_eax /* which holds the syscall # */);
      /* Deschedule thread until an I/O completion happens. */
      vg_threads[tid].status = VgTs_WaitFD;
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf,"block until I/O ready on fd %d", fd);
	 print_sched_event(tid, msg_buf);
      }

   }
}


/* Find out which of the fds in vg_waiting_fds are now ready to go, by
   making enquiries with select(), and mark them as ready.  We have to
   wait for the requesting threads to fall into the the WaitFD state
   before we can actually finally deliver the results, so this
   procedure doesn't do that; complete_blocked_syscalls() does it.

   It might seem odd that a thread which has done a blocking syscall
   is not in WaitFD state; the way this can happen is if it initially
   becomes WaitFD, but then a signal is delivered to it, so it becomes
   Runnable for a while.  In this case we have to wait for the
   sighandler to return, whereupon the WaitFD state is resumed, and
   only at that point can the I/O result be delivered to it.  However,
   this point may be long after the fd is actually ready.  

   So, poll_for_ready_fds() merely detects fds which are ready.
   complete_blocked_syscalls() does the second half of the trick,
   possibly much later: it delivers the results from ready fds to
   threads in WaitFD state. 
*/
void poll_for_ready_fds ( void )
{
   vki_ksigset_t      saved_procmask;
   vki_fd_set         readfds;
   vki_fd_set         writefds;
   vki_fd_set         exceptfds;
   struct vki_timeval timeout;
   Int                fd, fd_max, i, n_ready, syscall_no, n_ok;
   ThreadId           tid;
   Bool               rd_ok, wr_ok, ex_ok;
   Char               msg_buf[100];

   struct vki_timespec* rem;
   ULong                t_now;

   /* Awaken any sleeping threads whose sleep has expired. */
   t_now = VG_(read_microsecond_timer)();
   for (tid = 0; tid < VG_N_THREADS; tid++) {
      if (vg_threads[tid].status != VgTs_Sleeping)
         continue;
      if (t_now >= vg_threads[tid].awaken_at) {
         /* Resume this thread.  Set to zero the remaining-time (second)
            arg of nanosleep, since it's used up all its time. */
         vg_assert(vg_threads[tid].m_eax == __NR_nanosleep);
         rem = (struct vki_timespec *)vg_threads[tid].m_ecx; /* arg2 */
         if (rem != NULL) {
	    rem->tv_sec = 0;
            rem->tv_nsec = 0;
 	 }
         /* Make the syscall return 0 (success). */
         vg_threads[tid].m_eax = 0;
	 /* Reschedule this thread. */
	 vg_threads[tid].status = VgTs_Runnable;
         if (VG_(clo_trace_sched)) {
            VG_(sprintf)(msg_buf, "at %lu: nanosleep done", 
                                  t_now);
            print_sched_event(tid, msg_buf);
         }
      }
   }

   /* And look for threads waiting on file descriptors which are now
      ready for I/O.*/
   timeout.tv_sec = 0;
   timeout.tv_usec = 0;

   VKI_FD_ZERO(&readfds);
   VKI_FD_ZERO(&writefds);
   VKI_FD_ZERO(&exceptfds);
   fd_max = -1;
   for (i = 0; i < VG_N_WAITING_FDS; i++) {
      if (vg_waiting_fds[i].fd == -1 /* not in use */) 
         continue;
      if (vg_waiting_fds[i].ready /* already ready? */) 
         continue;
      fd = vg_waiting_fds[i].fd;
      /* VG_(printf)("adding QUERY for fd %d\n", fd); */
      vg_assert(fd >= 0);
      if (fd > fd_max) 
         fd_max = fd;
      tid = vg_waiting_fds[i].tid;
      vg_assert(tid >= 0 && tid < VG_N_THREADS);
      syscall_no = vg_waiting_fds[i].syscall_no;
      switch (syscall_no) {
         case __NR_read: 
            VKI_FD_SET(fd, &readfds); break;
         case __NR_write: 
            VKI_FD_SET(fd, &writefds); break;
         default: 
            VG_(panic)("poll_for_ready_fds: unexpected syscall");
            /*NOTREACHED*/
            break;
      }
   }

   /* Short cut: if no fds are waiting, give up now. */
   if (fd_max == -1)
      return;

   /* BLOCK ALL SIGNALS.  We don't want the complication of select()
      getting interrupted. */
   VG_(block_all_host_signals)( &saved_procmask );

   n_ready = VG_(select)
                ( fd_max+1, &readfds, &writefds, &exceptfds, &timeout);
   if (VG_(is_kerror)(n_ready)) {
      VG_(printf)("poll_for_ready_fds: select returned %d\n", n_ready);
      VG_(panic)("poll_for_ready_fds: select failed?!");
      /*NOTREACHED*/
   }
   
   /* UNBLOCK ALL SIGNALS */
   VG_(restore_host_signals)( &saved_procmask );

   /* VG_(printf)("poll_for_io_completions: %d fs ready\n", n_ready); */

   if (n_ready == 0)
      return;   

   /* Inspect all the fds we know about, and handle any completions that
      have happened. */
   /*
   VG_(printf)("\n\n");
   for (fd = 0; fd < 100; fd++)
     if (VKI_FD_ISSET(fd, &writefds) || VKI_FD_ISSET(fd, &readfds)) {
       VG_(printf)("X"); } else { VG_(printf)("."); };
   VG_(printf)("\n\nfd_max = %d\n", fd_max);
   */

   for (fd = 0; fd <= fd_max; fd++) {
      rd_ok = VKI_FD_ISSET(fd, &readfds);
      wr_ok = VKI_FD_ISSET(fd, &writefds);
      ex_ok = VKI_FD_ISSET(fd, &exceptfds);

      n_ok = (rd_ok ? 1 : 0) + (wr_ok ? 1 : 0) + (ex_ok ? 1 : 0);
      if (n_ok == 0) 
         continue;
      if (n_ok > 1) {
         VG_(printf)("offending fd = %d\n", fd);
         VG_(panic)("poll_for_ready_fds: multiple events on fd");
      }
      
      /* An I/O event completed for fd.  Find the thread which
         requested this. */
      for (i = 0; i < VG_N_WAITING_FDS; i++) {
         if (vg_waiting_fds[i].fd == -1 /* not in use */) 
            continue;
         if (vg_waiting_fds[i].fd == fd) 
            break;
      }

      /* And a bit more paranoia ... */
      vg_assert(i >= 0 && i < VG_N_WAITING_FDS);

      /* Mark the fd as ready. */      
      vg_assert(! vg_waiting_fds[i].ready);
      vg_waiting_fds[i].ready = True;
   }
}


/* See comment attached to poll_for_ready_fds() for explaination. */
void complete_blocked_syscalls ( void )
{
   Int      fd, i, res, syscall_no;
   ThreadId tid;
   Char     msg_buf[100];

   /* Inspect all the outstanding fds we know about. */

   for (i = 0; i < VG_N_WAITING_FDS; i++) {
      if (vg_waiting_fds[i].fd == -1 /* not in use */) 
         continue;
      if (! vg_waiting_fds[i].ready)
         continue;

      fd  = vg_waiting_fds[i].fd;
      tid = vg_waiting_fds[i].tid;
      vg_assert(tid >= 0 && tid < VG_N_THREADS);

      /* The thread actually has to be waiting for the I/O event it
         requested before we can deliver the result! */
      if (vg_threads[tid].status != VgTs_WaitFD)
         continue;

      /* Ok, actually do it!  We can safely use %EAX as the syscall
         number, because the speculative call made by
         sched_do_syscall() doesn't change %EAX in the case where the
         call would have blocked. */

      syscall_no = vg_waiting_fds[i].syscall_no;
      vg_assert(syscall_no == vg_threads[tid].m_eax);
      KERNEL_DO_SYSCALL(tid,res);
      VG_(check_known_blocking_syscall)(tid, syscall_no, &res /* POST */);

      /* Reschedule. */
      vg_threads[tid].status = VgTs_Runnable;
      /* Mark slot as no longer in use. */
      vg_waiting_fds[i].fd = -1;
      /* pp_sched_status(); */
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf,"resume due to I/O completion on fd %d", fd);
	 print_sched_event(tid, msg_buf);
      }
   }
}


static
void nanosleep_for_a_while ( void )
{
   Int res;
   struct vki_timespec req;
   struct vki_timespec rem;
   req.tv_sec = 0;
   req.tv_nsec = 20 * 1000 * 1000;
   res = VG_(nanosleep)( &req, &rem );   
   /* VG_(printf)("after ns, unused = %d\n", rem.tv_nsec ); */
   vg_assert(res == 0);
}


/* ---------------------------------------------------------------------
   The scheduler proper.
   ------------------------------------------------------------------ */

/* Run user-space threads until either
   * Deadlock occurs
   * One thread asks to shutdown Valgrind
   * The specified number of basic blocks has gone by.
*/
VgSchedReturnCode VG_(scheduler) ( void )
{
   ThreadId tid, tid_next;
   UInt     trc;
   UInt     dispatch_ctr_SAVED;
   Int      request_code, done_this_time, n_in_fdwait_or_sleep;
   Char     msg_buf[100];
   Addr     trans_addr;

   /* For the LRU structures, records when the epoch began. */
   ULong lru_epoch_started_at = 0;

   /* Start with the root thread.  tid in general indicates the
      currently runnable/just-finished-running thread. */
   tid = 0;

   /* This is the top level scheduler loop.  It falls into three
      phases. */
   while (True) {

      /* ======================= Phase 1 of 3 =======================
         Handle I/O completions and signals.  This may change the
         status of various threads.  Then select a new thread to run,
         or declare deadlock, or sleep if there are no runnable
         threads but some are blocked on I/O.  */

      /* Age the LRU structures if an epoch has been completed. */
      if (VG_(bbs_done) - lru_epoch_started_at >= VG_BBS_PER_EPOCH) {
         lru_epoch_started_at = VG_(bbs_done);
         increment_epoch();
      }

      /* Was a debug-stop requested? */
      if (VG_(bbs_to_go) == 0) 
         goto debug_stop;

      /* Do the following loop until a runnable thread is found, or
         deadlock is detected. */
      while (True) {

         /* For stats purposes only. */
         VG_(num_scheduling_events_MAJOR) ++;

         /* See if any I/O operations which we were waiting for have
            completed, and, if so, make runnable the relevant waiting
            threads. */
         poll_for_ready_fds();
         complete_blocked_syscalls();

         /* See if there are any signals which need to be delivered.  If
            so, choose thread(s) to deliver them to, and build signal
            delivery frames on those thread(s) stacks. */
         VG_(deliver_signals)( 0 /*HACK*/ );
         VG_(do_sanity_checks)(0 /*HACK*/, False);

         /* Try and find a thread (tid) to run. */
         tid_next = tid;
         n_in_fdwait_or_sleep = 0;
         while (True) {
            tid_next++;
            if (tid_next >= VG_N_THREADS) tid_next = 0;
            if (vg_threads[tid_next].status == VgTs_WaitFD
                || vg_threads[tid_next].status == VgTs_Sleeping)
               n_in_fdwait_or_sleep ++;
            if (vg_threads[tid_next].status == VgTs_Runnable) 
               break; /* We can run this one. */
            if (tid_next == tid) 
               break; /* been all the way round */
         }
         tid = tid_next;
       
         if (vg_threads[tid].status == VgTs_Runnable) {
            /* Found a suitable candidate.  Fall out of this loop, so
               we can advance to stage 2 of the scheduler: actually
               running the thread. */
            break;
	 }

         /* We didn't find a runnable thread.  Now what? */
         if (n_in_fdwait_or_sleep == 0) {
            /* No runnable threads and no prospect of any appearing
               even if we wait for an arbitrary length of time.  In
               short, we have a deadlock. */
	    pp_sched_status();
            return VgSrc_Deadlock;
         }

         /* At least one thread is in a fd-wait state.  Delay for a
            while, and go round again, in the hope that eventually a
            thread becomes runnable. */
         nanosleep_for_a_while();
	 //         pp_sched_status();
	 //	 VG_(printf)(".\n");
      }


      /* ======================= Phase 2 of 3 =======================
         Wahey!  We've finally decided that thread tid is runnable, so
         we now do that.  Run it for as much of a quanta as possible.
         Trivial requests are handled and the thread continues.  The
         aim is not to do too many of Phase 1 since it is expensive.  */

      if (0)
         VG_(printf)("SCHED: tid %d, used %d\n", tid, VG_N_THREADS);

      /* Figure out how many bbs to ask vg_run_innerloop to do.  Note
         that it decrements the counter before testing it for zero, so
         that if VG_(dispatch_ctr) is set to N you get at most N-1
         iterations.  Also this means that VG_(dispatch_ctr) must
         exceed zero before entering the innerloop.  Also also, the
         decrement is done before the bb is actually run, so you
         always get at least one decrement even if nothing happens.
      */
      if (VG_(bbs_to_go) >= VG_SCHEDULING_QUANTUM)
         VG_(dispatch_ctr) = VG_SCHEDULING_QUANTUM + 1;
      else
         VG_(dispatch_ctr) = (UInt)VG_(bbs_to_go) + 1;

      /* ... and remember what we asked for. */
      dispatch_ctr_SAVED = VG_(dispatch_ctr);

      /* Actually run thread tid. */
      while (True) {

         /* For stats purposes only. */
         VG_(num_scheduling_events_MINOR) ++;

         if (0)
            VG_(message)(Vg_DebugMsg, "thread %d: running for %d bbs", 
                                      tid, VG_(dispatch_ctr) - 1 );

         trc = run_thread_for_a_while ( tid );

         /* Deal quickly with trivial scheduling events, and resume the
            thread. */

         if (trc == VG_TRC_INNER_FASTMISS) {
            vg_assert(VG_(dispatch_ctr) > 0);

            /* Trivial event.  Miss in the fast-cache.  Do a full
               lookup for it. */
            trans_addr 
               = VG_(search_transtab) ( vg_threads[tid].m_eip );
            if (trans_addr == (Addr)0) {
               /* Not found; we need to request a translation. */
               VG_(create_translation_for)( vg_threads[tid].m_eip ); 
               trans_addr = VG_(search_transtab) ( vg_threads[tid].m_eip ); 
               if (trans_addr == (Addr)0)
                  VG_(panic)("VG_TRC_INNER_FASTMISS: missing tt_fast entry");
            }
            continue; /* with this thread */
         }

         if (trc == VG_TRC_EBP_JMP_CLIENTREQ) {
            Bool is_triv = maybe_do_trivial_clientreq(tid);
            if (is_triv) {
               /* NOTE: a trivial request is something like a call to
                  malloc() or free().  It DOES NOT change the
                  Runnability of this thread nor the status of any
                  other thread; it is purely thread-local. */
               continue; /* with this thread */
	    }
	 }

	 /* It's a non-trivial event.  Give up running this thread and
            handle things the expensive way. */
	 break;
      }

      /* ======================= Phase 3 of 3 =======================
         Handle non-trivial thread requests, mostly pthread stuff. */

      /* Ok, we've fallen out of the dispatcher for a
         non-completely-trivial reason. First, update basic-block
         counters. */

      done_this_time = (Int)dispatch_ctr_SAVED - (Int)VG_(dispatch_ctr) - 1;
      vg_assert(done_this_time >= 0);
      VG_(bbs_to_go)   -= (ULong)done_this_time;
      VG_(bbs_done)    += (ULong)done_this_time;

      if (0 && trc != VG_TRC_INNER_FASTMISS)
         VG_(message)(Vg_DebugMsg, "thread %d:   completed %d bbs, trc %d", 
                                   tid, done_this_time, (Int)trc );

      if (0 && trc != VG_TRC_INNER_FASTMISS)
         VG_(message)(Vg_DebugMsg, "thread %d:  %ld bbs, event %s", 
                                   tid, VG_(bbs_done),
                                   name_of_sched_event(trc) );

      /* Examine the thread's return code to figure out why it
         stopped, and handle requests. */

      switch (trc) {

         case VG_TRC_INNER_FASTMISS:
            VG_(panic)("VG_(scheduler):  VG_TRC_INNER_FASTMISS");
            /*NOTREACHED*/
            break;

         case VG_TRC_INNER_COUNTERZERO:
            /* Timeslice is out.  Let a new thread be scheduled,
               simply by doing nothing, causing us to arrive back at
               Phase 1. */
            if (VG_(bbs_to_go) == 0) {
               goto debug_stop;
            }
            vg_assert(VG_(dispatch_ctr) == 0);
            break;

         case VG_TRC_UNRESUMABLE_SIGNAL:
            /* It got a SIGSEGV/SIGBUS, which we need to deliver right
               away.  Again, do nothing, so we wind up back at Phase
               1, whereupon the signal will be "delivered". */
	    break;

         case VG_TRC_EBP_JMP_SYSCALL:
            /* Do a syscall for the vthread tid.  This could cause it
               to become non-runnable. */
            sched_do_syscall(tid);
            break;

         case VG_TRC_EBP_JMP_CLIENTREQ: 
            /* Do a client request for the vthread tid.  Note that
               some requests will have been handled by
               maybe_do_trivial_clientreq(), so we don't expect to see
               those here. 
            */
            /* The thread's %EAX points at an arg block, the first
               word of which is the request code. */
            request_code = ((UInt*)(vg_threads[tid].m_eax))[0];
            if (0) {
               VG_(sprintf)(msg_buf, "request 0x%x", request_code );
               print_sched_event(tid, msg_buf);
	    }
	    /* Do a non-trivial client request for thread tid.  tid's
               %EAX points to a short vector of argument words, the
               first of which is the request code.  The result of the
               request is put in tid's %EDX.  Alternatively, perhaps
               the request causes tid to become non-runnable and/or
               other blocked threads become runnable.  In general we
               can and often do mess with the state of arbitrary
               threads at this point. */
            if (request_code == VG_USERREQ__SHUTDOWN_VALGRIND) {
               return VgSrc_Shutdown;
            } else {
               do_nontrivial_clientreq(tid);
	    }
            break;

         default: 
            VG_(printf)("\ntrc = %d\n", trc);
            VG_(panic)("VG_(scheduler), phase 3: "
                       "unexpected thread return code");
            /* NOTREACHED */
            break;

      } /* switch (trc) */

      /* That completes Phase 3 of 3.  Return now to the top of the
	 main scheduler loop, to Phase 1 of 3. */

   } /* top-level scheduler loop */


   /* NOTREACHED */
   VG_(panic)("scheduler: post-main-loop ?!");
   /* NOTREACHED */

  debug_stop:
   /* If we exited because of a debug stop, print the translation 
      of the last block executed -- by translating it again, and 
      throwing away the result. */
   VG_(printf)(
      "======vvvvvvvv====== LAST TRANSLATION ======vvvvvvvv======\n");
   VG_(translate)( vg_threads[tid].m_eip, NULL, NULL, NULL );
   VG_(printf)("\n");
   VG_(printf)(
      "======^^^^^^^^====== LAST TRANSLATION ======^^^^^^^^======\n");

   return VgSrc_BbsDone;
}


/* ---------------------------------------------------------------------
   The pthread implementation.
   ------------------------------------------------------------------ */

#include <pthread.h>
#include <errno.h>

#if !defined(PTHREAD_STACK_MIN)
#  define PTHREAD_STACK_MIN (16384 - VG_AR_CLIENT_STACKBASE_REDZONE_SZB)
#endif

/*  /usr/include/bits/pthreadtypes.h:
    typedef unsigned long int pthread_t;
*/


static
void do_pthread_cancel ( ThreadId  tid_canceller,
                         pthread_t tid_cancellee )
{
   Char msg_buf[100];
   /* We want make is appear that this thread has returned to
      do_pthread_create_bogusRA with PTHREAD_CANCELED as the
      return value.  So: simple: put PTHREAD_CANCELED into %EAX
      and &do_pthread_create_bogusRA into %EIP and keep going! */
   if (VG_(clo_trace_sched)) {
      VG_(sprintf)(msg_buf, "cancelled by %d", tid_canceller);
      print_sched_event(tid_cancellee, msg_buf);
   }
   vg_threads[tid_cancellee].m_eax  = (UInt)PTHREAD_CANCELED;
   vg_threads[tid_cancellee].m_eip  = (UInt)&VG_(pthreadreturn_bogusRA);
   vg_threads[tid_cancellee].status = VgTs_Runnable;
}



/* Thread tid is exiting, by returning from the function it was
   created with.  Or possibly due to pthread_exit or cancellation.
   The main complication here is to resume any thread waiting to join
   with this one. */
static 
void handle_pthread_return ( ThreadId tid, void* retval )
{
   ThreadId jnr; /* joiner, the thread calling pthread_join. */
   UInt*    jnr_args;
   void**   jnr_thread_return;
   Char     msg_buf[100];

   /* Mark it as not in use.  Leave the stack in place so the next
      user of this slot doesn't reallocate it. */
   vg_assert(tid >= 0 && tid < VG_N_THREADS);
   vg_assert(vg_threads[tid].status != VgTs_Empty);

   vg_threads[tid].retval = retval;

   if (vg_threads[tid].joiner == VG_INVALID_THREADID) {
      /* No one has yet done a join on me */
      vg_threads[tid].status = VgTs_WaitJoiner;
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf, 
            "root fn returns, waiting for a call pthread_join(%d)", 
            tid);
         print_sched_event(tid, msg_buf);
      }
   } else {
      /* Some is waiting; make their join call return with success,
         putting my exit code in the place specified by the caller's
         thread_return param.  This is all very horrible, since we
         need to consult the joiner's arg block -- pointed to by its
         %EAX -- in order to extract the 2nd param of its pthread_join
         call.  TODO: free properly the slot (also below). 
      */
      jnr = vg_threads[tid].joiner;
      vg_assert(jnr >= 0 && jnr < VG_N_THREADS);
      vg_assert(vg_threads[jnr].status == VgTs_WaitJoinee);
      jnr_args = (UInt*)vg_threads[jnr].m_eax;
      jnr_thread_return = (void**)(jnr_args[2]);
      if (jnr_thread_return != NULL)
         *jnr_thread_return = vg_threads[tid].retval;
      vg_threads[jnr].m_edx = 0; /* success */
      vg_threads[jnr].status = VgTs_Runnable;
      vg_threads[tid].status = VgTs_Empty; /* bye! */
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf, 
            "root fn returns, to find a waiting pthread_join(%d)", tid);
         print_sched_event(tid, msg_buf);
         VG_(sprintf)(msg_buf, 
            "my pthread_join(%d) returned; resuming", tid);
         print_sched_event(jnr, msg_buf);
      }
   }

   /* Return value is irrelevant; this thread will not get
      rescheduled. */
}


static
void do_pthread_join ( ThreadId tid, ThreadId jee, void** thread_return )
{
   Char msg_buf[100];

   /* jee, the joinee, is the thread specified as an arg in thread
      tid's call to pthread_join.  So tid is the join-er. */
   vg_assert(tid >= 0 && tid < VG_N_THREADS);
   vg_assert(vg_threads[tid].status == VgTs_Runnable);

   if (jee == tid) {
      vg_threads[tid].m_edx = EDEADLK; /* libc constant, not a kernel one */
      vg_threads[tid].status = VgTs_Runnable;
      return;
   }

   if (jee < 0 
       || jee >= VG_N_THREADS
       || vg_threads[jee].status == VgTs_Empty) {
      /* Invalid thread to join to. */
      vg_threads[tid].m_edx = EINVAL;
      vg_threads[tid].status = VgTs_Runnable;
      return;
   }

   if (vg_threads[jee].joiner != VG_INVALID_THREADID) {
      /* Someone already did join on this thread */
      vg_threads[tid].m_edx = EINVAL;
      vg_threads[tid].status = VgTs_Runnable;
      return;
   }

   /* if (vg_threads[jee].detached) ... */

   /* Perhaps the joinee has already finished?  If so return
      immediately with its return code, and free up the slot. TODO:
      free it properly (also above). */
   if (vg_threads[jee].status == VgTs_WaitJoiner) {
      vg_assert(vg_threads[jee].joiner == VG_INVALID_THREADID);
      vg_threads[tid].m_edx = 0; /* success */
      if (thread_return != NULL) 
         *thread_return = vg_threads[jee].retval;
      vg_threads[tid].status = VgTs_Runnable;
      vg_threads[jee].status = VgTs_Empty; /* bye! */
      if (VG_(clo_trace_sched)) {
	 VG_(sprintf)(msg_buf,
		      "someone called pthread_join() on me; bye!");
         print_sched_event(jee, msg_buf);
	 VG_(sprintf)(msg_buf,
            "my pthread_join(%d) returned immediately", 
            jee );
         print_sched_event(tid, msg_buf);
      }
      return;
   }

   /* Ok, so we'll have to wait on jee. */
   vg_threads[jee].joiner = tid;
   vg_threads[tid].status = VgTs_WaitJoinee;
   if (VG_(clo_trace_sched)) {
      VG_(sprintf)(msg_buf,
         "blocking on call of pthread_join(%d)", jee );
      print_sched_event(tid, msg_buf);
   }
   /* So tid's join call does not return just now. */
}


static
void do_pthread_create ( ThreadId parent_tid,
                         pthread_t* thread, 
                         pthread_attr_t* attr, 
                         void* (*start_routine)(void *), 
                         void* arg )
{
   Addr     new_stack;
   UInt     new_stk_szb;
   ThreadId tid;
   Char     msg_buf[100];

   /* Paranoia ... */
   vg_assert(sizeof(pthread_t) == sizeof(UInt));

   vg_assert(vg_threads[parent_tid].status != VgTs_Empty);

   tid         = vg_alloc_ThreadState();

   /* If we've created the main thread's tid, we're in deep trouble :) */
   vg_assert(tid != 0);

   /* Copy the parent's CPU state into the child's, in a roundabout
      way (via baseBlock). */
   VG_(load_thread_state)(parent_tid);
   VG_(save_thread_state)(tid);

   /* Consider allocating the child a stack, if the one it already has
      is inadequate. */
   new_stk_szb = PTHREAD_STACK_MIN;

   if (new_stk_szb > vg_threads[tid].stack_size) {
      /* Again, for good measure :) We definitely don't want to be
         allocating a stack for the main thread. */
      vg_assert(tid != 0);
      /* for now, we don't handle the case of anything other than
         assigning it for the first time. */
      vg_assert(vg_threads[tid].stack_size == 0);
      vg_assert(vg_threads[tid].stack_base == (Addr)NULL);
      new_stack = (Addr)VG_(get_memory_from_mmap)( new_stk_szb );
      vg_threads[tid].stack_base = new_stack;
      vg_threads[tid].stack_size = new_stk_szb;
      vg_threads[tid].m_esp 
         = new_stack + new_stk_szb 
                     - VG_AR_CLIENT_STACKBASE_REDZONE_SZB;
   }
   if (VG_(clo_instrument))
      VGM_(make_noaccess)( vg_threads[tid].m_esp, 
                           VG_AR_CLIENT_STACKBASE_REDZONE_SZB );
   
   /* push arg */
   vg_threads[tid].m_esp -= 4;
   * (UInt*)(vg_threads[tid].m_esp) = (UInt)arg;

   /* push (magical) return address */
   vg_threads[tid].m_esp -= 4;
   * (UInt*)(vg_threads[tid].m_esp) = (UInt)VG_(pthreadreturn_bogusRA);

   if (VG_(clo_instrument))
      VGM_(make_readable)( vg_threads[tid].m_esp, 2 * 4 );

   /* this is where we start */
   vg_threads[tid].m_eip = (UInt)start_routine;

   if (VG_(clo_trace_sched)) {
      VG_(sprintf)(msg_buf,
         "new thread, created by %d", parent_tid );
      print_sched_event(tid, msg_buf);
   }

   /* store the thread id in *thread. */
   //   if (VG_(clo_instrument))
   // ***** CHECK *thread is writable
   *thread = (pthread_t)tid;

   /* return zero */
   vg_threads[tid].joiner = VG_INVALID_THREADID;
   vg_threads[tid].status = VgTs_Runnable;
   vg_threads[tid].m_edx  = 0; /* success */
}


/* Horrible hacks to do with pthread_mutex_t: the real pthread_mutex_t 
   is a struct with at least 5 words:
      typedef struct
      {
        int __m_reserved;         -- Reserved for future use
        int __m_count;            -- Depth of recursive locking
        _pthread_descr __m_owner; -- Owner thread (if recursive or errcheck)
        int __m_kind;      -- Mutex kind: fast, recursive or errcheck
        struct _pthread_fastlock __m_lock;  -- Underlying fast lock
      } pthread_mutex_t;
   Ours is just a single word, an index into vg_mutexes[].  
   For now I'll park it in the __m_reserved field.

   Uninitialised mutexes (PTHREAD_MUTEX_INITIALIZER) all have
   a zero __m_count field (see /usr/include/pthread.h).  So I'll
   use zero to mean non-inited, and 1 to mean inited.

   How convenient. 
*/

static
void initialise_mutex ( ThreadId tid, pthread_mutex_t *mutex )
{
   MutexId  mid;
   Char     msg_buf[100];
   /* vg_alloc_MutexId aborts if we can't allocate a mutex, for
      whatever reason. */
   mid = vg_alloc_VgMutex();
   vg_mutexes[mid].in_use = True;
   vg_mutexes[mid].held = False;
   vg_mutexes[mid].owner = VG_INVALID_THREADID; /* irrelevant */
   mutex->__m_reserved = mid;
   mutex->__m_count = 1; /* initialised */
   if (VG_(clo_trace_pthread)) {
      VG_(sprintf)(msg_buf, "(initialise mutex) (%p) -> %d", 
                            mutex, mid );
      print_pthread_event(tid, msg_buf);
   }
}

/* Allocate a new MutexId and write it into *mutex.  Ideally take
   notice of the attributes in *mutexattr.  */
static
void do_pthread_mutex_init ( ThreadId tid, 
                             pthread_mutex_t *mutex, 
                             const  pthread_mutexattr_t *mutexattr)
{
   Char     msg_buf[100];
   /* Paranoia ... */
   vg_assert(sizeof(pthread_mutex_t) >= sizeof(UInt));

   initialise_mutex(tid, mutex);

   if (VG_(clo_trace_pthread)) {
      VG_(sprintf)(msg_buf, "pthread_mutex_init (%p) -> %d", 
                            mutex, mutex->__m_reserved );
      print_pthread_event(tid, msg_buf);
   }

   /*
   RETURN VALUE
       pthread_mutex_init  always  returns 0. The other mutex functions 
       return 0 on success and a non-zero error code on error.
   */
   /* THIS THREAD returns with 0. */
   vg_threads[tid].m_edx = 0;
}


static
void do_pthread_mutex_lock( ThreadId tid, pthread_mutex_t *mutex )
{
   MutexId  mid;
   Char     msg_buf[100];

   /* *mutex contains the MutexId, or one of the magic values
      PTHREAD_*MUTEX_INITIALIZER*, indicating we need to initialise it
      now.  See comment(s) above re use of __m_count to indicated 
      initialisation status.
   */

   /* POSIX doesn't mandate this, but for sanity ... */
   if (mutex == NULL) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   if (mutex->__m_count == 0) {
      initialise_mutex(tid, mutex);
   }

   mid = mutex->__m_reserved;
   if (mid < 0 || mid >= VG_N_MUTEXES || !vg_mutexes[mid].in_use) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   if (VG_(clo_trace_pthread)) {
      VG_(sprintf)(msg_buf, "pthread_mutex_lock   %d (%p)", 
                            mid, mutex );
      print_pthread_event(tid, msg_buf);
   }

   /* Assert initialised. */
   vg_assert(mutex->__m_count == 1);

   /* Assume tid valid. */
   vg_assert(vg_threads[tid].status == VgTs_Runnable);

   if (vg_mutexes[mid].held) {
      if (vg_mutexes[mid].owner == tid) {
         vg_threads[tid].m_edx = EDEADLK;
         return;
      }
      /* Someone else has it; we have to wait. */
      vg_threads[tid].status = VgTs_WaitMX;
      vg_threads[tid].waited_on_mid = mid;
      /* No assignment to %EDX, since we're blocking. */
      if (VG_(clo_trace_pthread)) {
         VG_(sprintf)(msg_buf, "pthread_mutex_lock   %d (%p): BLOCK", 
                               mid, mutex );
         print_pthread_event(tid, msg_buf);
      }
   } else {
      /* We get it! */
      vg_mutexes[mid].held  = True;
      vg_mutexes[mid].owner = tid;
      /* return 0 (success). */
      vg_threads[tid].m_edx = 0;
   }
}


static
void do_pthread_mutex_unlock ( ThreadId tid,
                               pthread_mutex_t *mutex )
{
   MutexId  mid;
   Int      i;
   Char     msg_buf[100];

   if (mutex == NULL 
       || mutex->__m_count != 1) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   mid = mutex->__m_reserved;
   if (mid < 0 || mid >= VG_N_MUTEXES || !vg_mutexes[mid].in_use) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   if (VG_(clo_trace_pthread)) {
      VG_(sprintf)(msg_buf, "pthread_mutex_unlock %d (%p)", 
                            mid, mutex );
      print_pthread_event(tid, msg_buf);
   }

   /* Assume tid valid */
   vg_assert(vg_threads[tid].status == VgTs_Runnable);

   /* Barf if we don't currently hold the mutex. */
   if (!vg_mutexes[mid].held || vg_mutexes[mid].owner != tid) {
      vg_threads[tid].m_edx = EPERM;
      return;
   }

   /* Find some arbitrary thread waiting on this mutex, and make it
      runnable.  If none are waiting, mark the mutex as not held. */
   for (i = 0; i < VG_N_THREADS; i++) {
      if (vg_threads[i].status == VgTs_Empty) 
         continue;
      if (vg_threads[i].status == VgTs_WaitMX 
          && vg_threads[i].waited_on_mid == mid)
         break;
   }

   vg_assert(i <= VG_N_THREADS);
   if (i == VG_N_THREADS) {
      /* Nobody else is waiting on it. */
      vg_mutexes[mid].held = False;
   } else {
      /* Notionally transfer the hold to thread i, whose
         pthread_mutex_lock() call now returns with 0 (success). */
      vg_mutexes[mid].owner = i;
      vg_threads[i].status = VgTs_Runnable;
      vg_threads[i].m_edx = 0; /* pth_lock() success */

      if (VG_(clo_trace_pthread)) {
         VG_(sprintf)(msg_buf, "pthread_mutex_lock   %d: RESUME", 
                               mid );
         print_pthread_event(tid, msg_buf);
      }
   }

   /* In either case, our (tid's) pth_unlock() returns with 0
      (success). */
   vg_threads[tid].m_edx = 0; /* Success. */
}


static void do_pthread_mutex_destroy ( ThreadId tid,
                                       pthread_mutex_t *mutex )
{
   MutexId  mid;
   Char     msg_buf[100];

   if (mutex == NULL 
       || mutex->__m_count != 1) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   mid = mutex->__m_reserved;
   if (mid < 0 || mid >= VG_N_MUTEXES || !vg_mutexes[mid].in_use) {
      vg_threads[tid].m_edx = EINVAL;
      return;
   }

   if (VG_(clo_trace_pthread)) {
      VG_(sprintf)(msg_buf, "pthread_mutex_destroy %d (%p)", 
                            mid, mutex );
      print_pthread_event(tid, msg_buf);
   }

   /* Assume tid valid */
   vg_assert(vg_threads[tid].status == VgTs_Runnable);

   /* Barf if the mutex is currently held. */
   if (vg_mutexes[mid].held) {
      vg_threads[tid].m_edx = EBUSY;
      return;
   }

   mutex->__m_count = 0; /* uninitialised */
   vg_mutexes[mid].in_use = False;
   vg_threads[tid].m_edx = 0;
}


/* vthread tid is returning from a signal handler; modify its
   stack/regs accordingly. */
static
void handle_signal_return ( ThreadId tid )
{
   Char msg_buf[100];
   Bool restart_blocked_syscalls = VG_(signal_returns)(tid);

   if (restart_blocked_syscalls)
      /* Easy; we don't have to do anything. */
      return;

   if (vg_threads[tid].status == VgTs_WaitFD) {
      vg_assert(vg_threads[tid].m_eax == __NR_read 
                || vg_threads[tid].m_eax == __NR_write);
      /* read() or write() interrupted.  Force a return with EINTR. */
      vg_threads[tid].m_eax = -VKI_EINTR;
      vg_threads[tid].status = VgTs_Runnable;
      if (VG_(clo_trace_sched)) {
         VG_(sprintf)(msg_buf, 
            "read() / write() interrupted by signal; return EINTR" );
         print_sched_event(tid, msg_buf);
      }
      return;
   }

   if (vg_threads[tid].status == VgTs_WaitFD) {
      vg_assert(vg_threads[tid].m_eax == __NR_nanosleep);
      /* We interrupted a nanosleep().  The right thing to do is to
         write the unused time to nanosleep's second param and return
         EINTR, but I'm too lazy for that. */
      return;
   }

   /* All other cases?  Just return. */
}


/* ---------------------------------------------------------------------
   Handle non-trivial client requests.
   ------------------------------------------------------------------ */

static
void do_nontrivial_clientreq ( ThreadId tid )
{
   UInt* arg    = (UInt*)(vg_threads[tid].m_eax);
   UInt  req_no = arg[0];
   switch (req_no) {

      case VG_USERREQ__PTHREAD_CREATE:
         do_pthread_create( tid, 
                            (pthread_t*)arg[1], 
                            (pthread_attr_t*)arg[2], 
                            (void*(*)(void*))arg[3], 
                            (void*)arg[4] );
         break;

      case VG_USERREQ__PTHREAD_RETURNS:
         handle_pthread_return( tid, (void*)arg[1] );
         break;

      case VG_USERREQ__PTHREAD_JOIN:
         do_pthread_join( tid, arg[1], (void**)(arg[2]) );
         break;

      /* Sigh ... this probably will cause huge numbers of major
         (expensive) scheduling events, for no real reason.
         Perhaps should be classified as a trivial-request. */
      case VG_USERREQ__PTHREAD_GET_THREADID:
         vg_threads[tid].m_edx = tid;
	 break;

      case VG_USERREQ__PTHREAD_MUTEX_INIT:
         do_pthread_mutex_init( tid, 
                                (pthread_mutex_t *)(arg[1]),
                                (pthread_mutexattr_t *)(arg[2]) );
         break;

      case VG_USERREQ__PTHREAD_MUTEX_LOCK:
         do_pthread_mutex_lock( tid, (pthread_mutex_t *)(arg[1]) );
         break;

      case VG_USERREQ__PTHREAD_MUTEX_UNLOCK:
         do_pthread_mutex_unlock( tid, (pthread_mutex_t *)(arg[1]) );
         break;

      case VG_USERREQ__PTHREAD_MUTEX_DESTROY:
         do_pthread_mutex_destroy( tid, (pthread_mutex_t *)(arg[1]) );
         break;

      case VG_USERREQ__PTHREAD_CANCEL:
         do_pthread_cancel( tid, (pthread_t)(arg[1]) );
         break;

      case VG_USERREQ__MAKE_NOACCESS:
      case VG_USERREQ__MAKE_WRITABLE:
      case VG_USERREQ__MAKE_READABLE:
      case VG_USERREQ__DISCARD:
      case VG_USERREQ__CHECK_WRITABLE:
      case VG_USERREQ__CHECK_READABLE:
      case VG_USERREQ__MAKE_NOACCESS_STACK:
      case VG_USERREQ__RUNNING_ON_VALGRIND:
      case VG_USERREQ__DO_LEAK_CHECK:
         vg_threads[tid].m_edx = VG_(handle_client_request) ( arg );
	 break;

      case VG_USERREQ__SIGNAL_RETURNS: 
         handle_signal_return(tid);
	 break;

      default:
         VG_(printf)("panic'd on private request = 0x%x\n", arg[0] );
         VG_(panic)("handle_private_client_pthread_request: "
                    "unknown request");
         /*NOTREACHED*/
         break;
   }
}


/*--------------------------------------------------------------------*/
/*--- end                                           vg_scheduler.c ---*/
/*--------------------------------------------------------------------*/
