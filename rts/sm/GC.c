/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team 1998-2008
 *
 * Generational garbage collector
 *
 * Documentation on the architecture of the Garbage Collector can be
 * found in the online commentary:
 * 
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/Rts/Storage/GC
 *
 * ---------------------------------------------------------------------------*/

#include "PosixSource.h"
#include "Rts.h"
#include "HsFFI.h"

#include "Storage.h"
#include "RtsUtils.h"
#include "Apply.h"
#include "Updates.h"
#include "Stats.h"
#include "Schedule.h"
#include "Sanity.h"
#include "BlockAlloc.h"
#include "ProfHeap.h"
#include "Weak.h"
#include "Prelude.h"
#include "RtsSignals.h"
#include "STM.h"
#if defined(RTS_GTK_FRONTPANEL)
#include "FrontPanel.h"
#endif
#include "Trace.h"
#include "RetainerProfile.h"
#include "LdvProfile.h"
#include "RaiseAsync.h"
#include "Papi.h"
#include "Stable.h"

#include "GC.h"
#include "GCThread.h"
#include "GCTDecl.h"
#include "Compact.h"
#include "Evac.h"
#include "Scav.h"
#include "GCUtils.h"
#include "MarkStack.h"
#include "MarkWeak.h"
#include "Sparks.h"
#include "Sweep.h"

#include <string.h> // for memset()
#include <unistd.h>

/* -----------------------------------------------------------------------------
   Global variables
   -------------------------------------------------------------------------- */

/* STATIC OBJECT LIST.
 *
 * During GC:
 * We maintain a linked list of static objects that are still live.
 * The requirements for this list are:
 *
 *  - we need to scan the list while adding to it, in order to
 *    scavenge all the static objects (in the same way that
 *    breadth-first scavenging works for dynamic objects).
 *
 *  - we need to be able to tell whether an object is already on
 *    the list, to break loops.
 *
 * Each static object has a "static link field", which we use for
 * linking objects on to the list.  We use a stack-type list, consing
 * objects on the front as they are added (this means that the
 * scavenge phase is depth-first, not breadth-first, but that
 * shouldn't matter).  
 *
 * A separate list is kept for objects that have been scavenged
 * already - this is so that we can zero all the marks afterwards.
 *
 * An object is on the list if its static link field is non-zero; this
 * means that we have to mark the end of the list with '1', not NULL.  
 *
 * Extra notes for generational GC:
 *
 * Each generation has a static object list associated with it.  When
 * collecting generations up to N, we treat the static object lists
 * from generations > N as roots.
 *
 * We build up a static object list while collecting generations 0..N,
 * which is then appended to the static object list of generation N+1.
 */

/* N is the oldest generation being collected, where the generations
 * are numbered starting at 0.  A major GC (indicated by the major_gc
 * flag) is when we're collecting all generations.  We only attempt to
 * deal with static objects and GC CAFs when doing a major GC.
 */
nat N;
rtsBool major_gc;

/* Data used for allocation area sizing.
 */
static lnat g0_pcnt_kept = 30; // percentage of g0 live at last minor GC 

/* Mut-list stats */
#ifdef DEBUG
nat mutlist_MUTVARS,
    mutlist_MUTARRS,
    mutlist_MVARS,
    mutlist_OTHERS;
#endif

/* Thread-local data for each GC thread
 */
gc_thread **gc_threads = NULL;

#if !defined(THREADED_RTS)
StgWord8 the_gc_thread[sizeof(gc_thread) + 64 * sizeof(gen_workspace)];
#endif

// Number of threads running in *this* GC.  Affects how many
// step->todos[] lists we have to look in to find work.
nat n_gc_threads;

// For stats:
long copied;        // *words* copied & scavenged during this GC

rtsBool work_stealing;

DECLARE_GCT

/* -----------------------------------------------------------------------------
   Static function declarations
   -------------------------------------------------------------------------- */

static void mark_root               (void *user, StgClosure **root);
static void zero_static_object_list (StgClosure* first_static);
static nat  initialise_N            (rtsBool force_major_gc);
static void prepare_collected_gen   (generation *gen);
static void prepare_uncollected_gen (generation *gen);
static void init_gc_thread          (gc_thread *t);
static void resize_generations      (void);
static void resize_nursery          (void);
static void start_gc_threads        (void);
static void scavenge_until_all_done (void);
static StgWord inc_running          (void);
static StgWord dec_running          (void);
static void wakeup_gc_threads       (nat me);
static void shutdown_gc_threads     (nat me);
static void collect_gct_blocks      (void);

#if 0 && defined(DEBUG)
static void gcCAFs                  (void);
#endif

/* -----------------------------------------------------------------------------
   The mark stack.
   -------------------------------------------------------------------------- */

bdescr *mark_stack_top_bd; // topmost block in the mark stack
bdescr *mark_stack_bd;     // current block in the mark stack
StgPtr mark_sp;            // pointer to the next unallocated mark stack entry

/* -----------------------------------------------------------------------------
   GarbageCollect: the main entry point to the garbage collector.

   Locks held: all capabilities are held throughout GarbageCollect().
   -------------------------------------------------------------------------- */

void
GarbageCollect (rtsBool force_major_gc, 
                rtsBool do_heap_census,
                nat gc_type USED_IF_THREADS,
                Capability *cap)
{
  bdescr *bd;
  generation *gen;
  lnat live_blocks, live_words, allocated, max_copied, avg_copied;
#if defined(THREADED_RTS)
  gc_thread *saved_gct;
#endif
  nat g, n;

  // necessary if we stole a callee-saves register for gct:
#if defined(THREADED_RTS)
  saved_gct = gct;
#endif

#ifdef PROFILING
  CostCentreStack *save_CCS[n_capabilities];
#endif

  ACQUIRE_SM_LOCK;

#if defined(RTS_USER_SIGNALS)
  if (RtsFlags.MiscFlags.install_signal_handlers) {
    // block signals
    blockUserSignals();
  }
#endif

  ASSERT(sizeof(gen_workspace) == 16 * sizeof(StgWord));
  // otherwise adjust the padding in gen_workspace.

  // this is the main thread
  SET_GCT(gc_threads[cap->no]);

  // tell the stats department that we've started a GC 
  stat_startGC(gct);

  // lock the StablePtr table
  stablePtrPreGC();

#ifdef DEBUG
  mutlist_MUTVARS = 0;
  mutlist_MUTARRS = 0;
  mutlist_OTHERS = 0;
#endif

  // attribute any costs to CCS_GC 
#ifdef PROFILING
  for (n = 0; n < n_capabilities; n++) {
      save_CCS[n] = capabilities[n].r.rCCCS;
      capabilities[n].r.rCCCS = CCS_GC;
  }
#endif

  /* Approximate how much we allocated.  
   * Todo: only when generating stats? 
   */
  allocated = calcAllocated(rtsFalse/* don't count the nursery yet */);

  /* Figure out which generation to collect
   */
  n = initialise_N(force_major_gc);

#if defined(THREADED_RTS)
  work_stealing = RtsFlags.ParFlags.parGcLoadBalancingEnabled &&
                  N >= RtsFlags.ParFlags.parGcLoadBalancingGen;
      // It's not always a good idea to do load balancing in parallel
      // GC.  In particular, for a parallel program we don't want to
      // lose locality by moving cached data into another CPU's cache
      // (this effect can be quite significant). 
      //
      // We could have a more complex way to deterimine whether to do
      // work stealing or not, e.g. it might be a good idea to do it
      // if the heap is big.  For now, we just turn it on or off with
      // a flag.
#endif

  /* Start threads, so they can be spinning up while we finish initialisation.
   */
  start_gc_threads();

#if defined(THREADED_RTS)
  /* How many threads will be participating in this GC?
   * We don't try to parallelise minor GCs (unless the user asks for
   * it with +RTS -gn0), or mark/compact/sweep GC.
   */
  if (gc_type == SYNC_GC_PAR) {
      n_gc_threads = n_capabilities;
  } else {
      n_gc_threads = 1;
  }
#else
  n_gc_threads = 1;
#endif

  debugTrace(DEBUG_gc, "GC (gen %d): %d KB to collect, %ld MB in use, using %d thread(s)",
        N, n * (BLOCK_SIZE / 1024), mblocks_allocated, n_gc_threads);

#ifdef RTS_GTK_FRONTPANEL
  if (RtsFlags.GcFlags.frontpanel) {
      updateFrontPanelBeforeGC(N);
  }
#endif

#ifdef DEBUG
  // check for memory leaks if DEBUG is on 
  memInventory(DEBUG_gc);
#endif

  // check sanity *before* GC
  IF_DEBUG(sanity, checkSanity(rtsFalse /* before GC */, major_gc));

  // Initialise all the generations/steps that we're collecting.
  for (g = 0; g <= N; g++) {
      prepare_collected_gen(&generations[g]);
  }
  // Initialise all the generations/steps that we're *not* collecting.
  for (g = N+1; g < RtsFlags.GcFlags.generations; g++) {
      prepare_uncollected_gen(&generations[g]);
  }

  // Prepare this gc_thread
  init_gc_thread(gct);

  /* Allocate a mark stack if we're doing a major collection.
   */
  if (major_gc && oldest_gen->mark) {
      mark_stack_bd     = allocBlock();
      mark_stack_top_bd = mark_stack_bd;
      mark_stack_bd->link = NULL;
      mark_stack_bd->u.back = NULL;
      mark_sp           = mark_stack_bd->start;
  } else {
      mark_stack_bd     = NULL;
      mark_stack_top_bd = NULL;
      mark_sp           = NULL;
  }

  /* -----------------------------------------------------------------------
   * follow all the roots that we know about:
   */

  // the main thread is running: this prevents any other threads from
  // exiting prematurely, so we can start them now.
  // NB. do this after the mutable lists have been saved above, otherwise
  // the other GC threads will be writing into the old mutable lists.
  inc_running();
  wakeup_gc_threads(gct->thread_index);

  traceEventGcWork(gct->cap);

  // scavenge the capability-private mutable lists.  This isn't part
  // of markSomeCapabilities() because markSomeCapabilities() can only
  // call back into the GC via mark_root() (due to the gct register
  // variable).
  if (n_gc_threads == 1) {
      for (n = 0; n < n_capabilities; n++) {
#if defined(THREADED_RTS)
          scavenge_capability_mut_Lists1(&capabilities[n]);
#else
          scavenge_capability_mut_lists(&capabilities[n]);
#endif
      }
  } else {
      scavenge_capability_mut_lists(gct->cap);
      for (n = 0; n < n_capabilities; n++) {
          if (gc_threads[n]->idle) {
              markCapability(mark_root, gct, &capabilities[n],
                             rtsTrue/*don't mark sparks*/);
              scavenge_capability_mut_lists(&capabilities[n]);
          }
      }
  }

  // follow roots from the CAF list (used by GHCi)
  gct->evac_gen_no = 0;
  markCAFs(mark_root, gct);

  // follow all the roots that the application knows about.
  gct->evac_gen_no = 0;
  if (n_gc_threads == 1) {
      for (n = 0; n < n_capabilities; n++) {
          markCapability(mark_root, gct, &capabilities[n],
                         rtsTrue/*don't mark sparks*/);
      }
  } else {
      markCapability(mark_root, gct, cap, rtsTrue/*don't mark sparks*/);
  }

  markScheduler(mark_root, gct);

#if defined(RTS_USER_SIGNALS)
  // mark the signal handlers (signals should be already blocked)
  markSignalHandlers(mark_root, gct);
#endif

  // Mark the weak pointer list, and prepare to detect dead weak pointers.
  markWeakPtrList();
  initWeakForGC();

  // Mark the stable pointer table.
  markStablePtrTable(mark_root, gct);

  /* -------------------------------------------------------------------------
   * Repeatedly scavenge all the areas we know about until there's no
   * more scavenging to be done.
   */
  for (;;)
  {
      scavenge_until_all_done();
      // The other threads are now stopped.  We might recurse back to
      // here, but from now on this is the only thread.
      
      // must be last...  invariant is that everything is fully
      // scavenged at this point.
      if (traverseWeakPtrList()) { // returns rtsTrue if evaced something 
	  inc_running();
	  continue;
      }

      // If we get to here, there's really nothing left to do.
      break;
  }

  shutdown_gc_threads(gct->thread_index);

  // Now see which stable names are still alive.
  gcStablePtrTable();

#ifdef THREADED_RTS
  if (n_gc_threads == 1) {
      for (n = 0; n < n_capabilities; n++) {
          pruneSparkQueue(&capabilities[n]);
      }
  } else {
      for (n = 0; n < n_capabilities; n++) {
          if (n == cap->no || gc_threads[n]->idle) {
              pruneSparkQueue(&capabilities[n]);
         }
      }
  }
#endif

#ifdef PROFILING
  // We call processHeapClosureForDead() on every closure destroyed during
  // the current garbage collection, so we invoke LdvCensusForDead().
  if (RtsFlags.ProfFlags.doHeapProfile == HEAP_BY_LDV
      || RtsFlags.ProfFlags.bioSelector != NULL) {
      RELEASE_SM_LOCK; // LdvCensusForDead may need to take the lock
      LdvCensusForDead(N);
      ACQUIRE_SM_LOCK;
  }
#endif

  // NO MORE EVACUATION AFTER THIS POINT!

  // Finally: compact or sweep the oldest generation.
  if (major_gc && oldest_gen->mark) {
      if (oldest_gen->compact) 
          compact(gct->scavenged_static_objects);
      else
          sweep(oldest_gen);
  }

  copied = 0;
  max_copied = 0;
  avg_copied = 0;
  { 
      nat i;
      for (i=0; i < n_gc_threads; i++) {
          if (n_gc_threads > 1) {
              debugTrace(DEBUG_gc,"thread %d:", i);
              debugTrace(DEBUG_gc,"   copied           %ld", gc_threads[i]->copied * sizeof(W_));
              debugTrace(DEBUG_gc,"   scanned          %ld", gc_threads[i]->scanned * sizeof(W_));
              debugTrace(DEBUG_gc,"   any_work         %ld", gc_threads[i]->any_work);
              debugTrace(DEBUG_gc,"   no_work          %ld", gc_threads[i]->no_work);
              debugTrace(DEBUG_gc,"   scav_find_work %ld",   gc_threads[i]->scav_find_work);
          }
          copied += gc_threads[i]->copied;
          max_copied = stg_max(gc_threads[i]->copied, max_copied);
      }
      if (n_gc_threads == 1) {
          max_copied = 0;
          avg_copied = 0;
      } else {
          avg_copied = copied;
      }
  }

  // Run through all the generations/steps and tidy up.
  // We're going to:
  //   - count the amount of "live" data (live_words, live_blocks)
  //   - count the amount of "copied" data in this GC (copied)
  //   - free from-space
  //   - make to-space the new from-space (set BF_EVACUATED on all blocks)
  //
  live_words = 0;
  live_blocks = 0;

  for (g = 0; g < RtsFlags.GcFlags.generations; g++) {

    if (g == N) {
      generations[g].collections++; // for stats 
      if (n_gc_threads > 1) generations[g].par_collections++;
    }

    // Count the mutable list as bytes "copied" for the purposes of
    // stats.  Every mutable list is copied during every GC.
    if (g > 0) {
	nat mut_list_size = 0;
        for (n = 0; n < n_capabilities; n++) {
            mut_list_size += countOccupied(capabilities[n].mut_lists[g]);
        }
	copied +=  mut_list_size;

	debugTrace(DEBUG_gc,
		   "mut_list_size: %lu (%d vars, %d arrays, %d MVARs, %d others)",
		   (unsigned long)(mut_list_size * sizeof(W_)),
		   mutlist_MUTVARS, mutlist_MUTARRS, mutlist_MVARS, mutlist_OTHERS);
    }

    bdescr *next, *prev;
    gen = &generations[g];

    // for generations we collected... 
    if (g <= N) {

	/* free old memory and shift to-space into from-space for all
	 * the collected steps (except the allocation area).  These
	 * freed blocks will probaby be quickly recycled.
	 */
        if (gen->mark)
        {
            // tack the new blocks on the end of the existing blocks
            if (gen->old_blocks != NULL) {
                
                prev = NULL;
                for (bd = gen->old_blocks; bd != NULL; bd = next) {
                    
                    next = bd->link;
                    
                    if (!(bd->flags & BF_MARKED))
                    {
                        if (prev == NULL) {
                            gen->old_blocks = next;
                        } else {
                            prev->link = next;
                        }
                        freeGroup(bd);
                        gen->n_old_blocks--;
                    }
                    else
                    {
                        gen->n_words += bd->free - bd->start;
                        
                        // NB. this step might not be compacted next
                        // time, so reset the BF_MARKED flags.
                        // They are set before GC if we're going to
                        // compact.  (search for BF_MARKED above).
                        bd->flags &= ~BF_MARKED;
                        
                        // between GCs, all blocks in the heap except
                        // for the nursery have the BF_EVACUATED flag set.
                        bd->flags |= BF_EVACUATED;
                        
                        prev = bd;
                    }
                }

                if (prev != NULL) {
                    prev->link = gen->blocks;
                    gen->blocks = gen->old_blocks;
                }
            }
            // add the new blocks to the block tally
            gen->n_blocks += gen->n_old_blocks;
            ASSERT(countBlocks(gen->blocks) == gen->n_blocks);
            ASSERT(countOccupied(gen->blocks) == gen->n_words);
        }
        else // not copacted
        {
            freeChain(gen->old_blocks);
        }

        gen->old_blocks = NULL;
        gen->n_old_blocks = 0;

        /* LARGE OBJECTS.  The current live large objects are chained on
         * scavenged_large, having been moved during garbage
         * collection from large_objects.  Any objects left on the
         * large_objects list are therefore dead, so we free them here.
         */
        freeChain(gen->large_objects);
        gen->large_objects  = gen->scavenged_large_objects;
        gen->n_large_blocks = gen->n_scavenged_large_blocks;
        gen->n_new_large_words = 0;
    }
    else // for generations > N
    {
	/* For older generations, we need to append the
	 * scavenged_large_object list (i.e. large objects that have been
	 * promoted during this GC) to the large_object list for that step.
	 */
	for (bd = gen->scavenged_large_objects; bd; bd = next) {
            next = bd->link;
            dbl_link_onto(bd, &gen->large_objects);
	}
        
	// add the new blocks we promoted during this GC 
	gen->n_large_blocks += gen->n_scavenged_large_blocks;
    }

    ASSERT(countBlocks(gen->large_objects) == gen->n_large_blocks);

    gen->scavenged_large_objects = NULL;
    gen->n_scavenged_large_blocks = 0;

    // Count "live" data
    live_words  += genLiveWords(gen);
    live_blocks += genLiveBlocks(gen);

    // add in the partial blocks in the gen_workspaces, but ignore gen 0
    // if this is a local GC (we can't count another capability's part_list)
    {
        nat i;
        for (i = 0; i < n_capabilities; i++) {
            live_words  += gcThreadLiveWords(i, gen->no);
            live_blocks += gcThreadLiveBlocks(i, gen->no);
        }
    }
  } // for all generations

  // update the max size of older generations after a major GC
  resize_generations();
  
  // Free the mark stack.
  if (mark_stack_top_bd != NULL) {
      debugTrace(DEBUG_gc, "mark stack: %d blocks",
                 countBlocks(mark_stack_top_bd));
      freeChain(mark_stack_top_bd);
  }

  // Free any bitmaps.
  for (g = 0; g <= N; g++) {
      gen = &generations[g];
      if (gen->bitmap != NULL) {
          freeGroup(gen->bitmap);
          gen->bitmap = NULL;
      }
  }

  // Reset the nursery: make the blocks empty
  allocated += clearNurseries();

  resize_nursery();

  resetNurseries();

 // mark the garbage collected CAFs as dead
#if 0 && defined(DEBUG) // doesn't work at the moment 
  if (major_gc) { gcCAFs(); }
#endif
  
#ifdef PROFILING
  // resetStaticObjectForRetainerProfiling() must be called before
  // zeroing below.

  // ToDo: fix the gct->scavenged_static_objects below
  resetStaticObjectForRetainerProfiling(gct->scavenged_static_objects);
#endif

  // zero the scavenged static object list 
  if (major_gc) {
      nat i;
      if (n_gc_threads == 1) {
          zero_static_object_list(gct->scavenged_static_objects);
      } else {
          for (i = 0; i < n_gc_threads; i++) {
              if (!gc_threads[i]->idle) {
                  zero_static_object_list(gc_threads[i]->scavenged_static_objects);
              }
          }
      }
  }

  // Update the stable pointer hash table.
  updateStablePtrTable(major_gc);

  // unlock the StablePtr table.  Must be before scheduleFinalizers(),
  // because a finalizer may call hs_free_fun_ptr() or
  // hs_free_stable_ptr(), both of which access the StablePtr table.
  stablePtrPostGC();

  // Start any pending finalizers.  Must be after
  // updateStablePtrTable() and stablePtrPostGC() (see #4221).
  RELEASE_SM_LOCK;
  scheduleFinalizers(cap, old_weak_ptr_list);
  ACQUIRE_SM_LOCK;

  // check sanity after GC
  // before resurrectThreads(), because that might overwrite some
  // closures, which will cause problems with THREADED where we don't
  // fill slop.
  IF_DEBUG(sanity, checkSanity(rtsTrue /* after GC */, major_gc));

  // If a heap census is due, we need to do it before
  // resurrectThreads(), for the same reason as checkSanity above:
  // resurrectThreads() will overwrite some closures and leave slop
  // behind.
  if (do_heap_census) {
      debugTrace(DEBUG_sched, "performing heap census");
      RELEASE_SM_LOCK;
      heapCensus(gct->gc_start_cpu);
      ACQUIRE_SM_LOCK;
  }

  // send exceptions to any threads which were about to die
  RELEASE_SM_LOCK;
  resurrectThreads(resurrected_threads);
  ACQUIRE_SM_LOCK;

  if (major_gc) {
      nat need, got;
      need = BLOCKS_TO_MBLOCKS(n_alloc_blocks);
      got = mblocks_allocated;
      /* If the amount of data remains constant, next major GC we'll
         require (F+1)*need. We leave (F+2)*need in order to reduce
         repeated deallocation and reallocation. */
      need = (RtsFlags.GcFlags.oldGenFactor + 2) * need;
      if (got > need) {
          returnMemoryToOS(got - need);
      }
  }

  // extra GC trace info
  IF_DEBUG(gc, statDescribeGens());

#ifdef DEBUG
  // symbol-table based profiling 
  /*  heapCensus(to_blocks); */ /* ToDo */
#endif

  // restore enclosing cost centre 
#ifdef PROFILING
  for (n = 0; n < n_capabilities; n++) {
      capabilities[n].r.rCCCS = save_CCS[n];
  }
#endif

#ifdef DEBUG
  // check for memory leaks if DEBUG is on 
  memInventory(DEBUG_gc);
#endif

#ifdef RTS_GTK_FRONTPANEL
  if (RtsFlags.GcFlags.frontpanel) {
      updateFrontPanelAfterGC( N, live );
  }
#endif

  // ok, GC over: tell the stats department what happened. 
  stat_endGC(gct, allocated, live_words,
             copied, N, max_copied, avg_copied,
             live_blocks * BLOCK_SIZE_W - live_words /* slop */);

  // Guess which generation we'll collect *next* time
  initialise_N(force_major_gc);

#if defined(RTS_USER_SIGNALS)
  if (RtsFlags.MiscFlags.install_signal_handlers) {
    // unblock signals again
    unblockUserSignals();
  }
#endif

  RELEASE_SM_LOCK;

  SET_GCT(saved_gct);
}

/* -----------------------------------------------------------------------------
   Figure out which generation to collect, initialise N and major_gc.

   Also returns the total number of blocks in generations that will be
   collected.
   -------------------------------------------------------------------------- */

static nat
initialise_N (rtsBool force_major_gc)
{
    int g;
    nat blocks, blocks_total;

    blocks = 0;
    blocks_total = 0;

    if (force_major_gc) {
        N = RtsFlags.GcFlags.generations - 1;
    } else {
        N = 0;
    }

    for (g = RtsFlags.GcFlags.generations - 1; g >= 0; g--) {

        blocks = generations[g].n_words / BLOCK_SIZE_W
               + generations[g].n_large_blocks;

        if (blocks >= generations[g].max_blocks) {
            N = stg_max(N,g);
        }
        if ((nat)g <= N) {
            blocks_total += blocks;
        }
    }

    blocks_total += countNurseryBlocks();

    major_gc = (N == RtsFlags.GcFlags.generations-1);
    return blocks_total;
}

/* -----------------------------------------------------------------------------
   Initialise the gc_thread structures.
   -------------------------------------------------------------------------- */

#define GC_THREAD_INACTIVE             0
#define GC_THREAD_STANDING_BY          1
#define GC_THREAD_RUNNING              2
#define GC_THREAD_WAITING_TO_CONTINUE  3

static void
new_gc_thread (nat n, gc_thread *t)
{
    nat g;
    gen_workspace *ws;

    t->cap = &capabilities[n];

#ifdef THREADED_RTS
    t->id = 0;
    initSpinLock(&t->gc_spin);
    initSpinLock(&t->mut_spin);
    ACQUIRE_SPIN_LOCK(&t->gc_spin);
    t->wakeup = GC_THREAD_INACTIVE;  // starts true, so we can wait for the
                          // thread to start up, see wakeup_gc_threads
#endif

    t->thread_index = n;
    t->idle = rtsFalse;
    t->free_blocks = NULL;
    t->gc_count = 0;

    init_gc_thread(t);
    
#ifdef USE_PAPI
    t->papi_events = -1;
#endif

    for (g = 0; g < RtsFlags.GcFlags.generations; g++)
    {
        ws = &t->gens[g];
        ws->gen = &generations[g];
        ASSERT(g == ws->gen->no);
        ws->my_gct = t;
        
        // We want to call
        //   alloc_todo_block(ws,0);
        // but can't, because it uses gct which isn't set up at this point.
        // Hence, allocate a block for todo_bd manually:
        {
            bdescr *bd = allocBlock(); // no lock, locks aren't initialised yet
            initBdescr(bd, ws->gen, ws->gen->to);
            bd->flags = BF_EVACUATED;
            bd->u.scan = bd->free = bd->start;

            ws->todo_bd = bd;
            ws->todo_free = bd->free;
            ws->todo_lim = bd->start + BLOCK_SIZE_W;
        }

        ws->todo_q = newWSDeque(128);
        ws->todo_overflow = NULL;
        ws->n_todo_overflow = 0;
        ws->todo_large_objects = NULL;

        ws->part_list = NULL;
        ws->n_part_blocks = 0;

        ws->scavd_list = NULL;
        ws->n_scavd_blocks = 0;
    }
}


void
initGcThreads (nat from USED_IF_THREADS, nat to USED_IF_THREADS)
{
#if defined(THREADED_RTS)
    nat i;

    if (from > 0) {
        gc_threads = stgReallocBytes (gc_threads, to * sizeof(gc_thread*),
                                      "initGcThreads");
    } else {
        gc_threads = stgMallocBytes (to * sizeof(gc_thread*),
                                     "initGcThreads");
    }

    // We have to update the gct->cap pointers to point to the new
    // Capability array now.
    for (i = 0; i < from; i++) {
        gc_threads[i]->cap = &capabilities[gc_threads[i]->cap->no];
    }

    for (i = from; i < to; i++) {
        gc_threads[i] =
            stgMallocBytes(sizeof(gc_thread) +
                           RtsFlags.GcFlags.generations * sizeof(gen_workspace),
                           "alloc_gc_threads");

        new_gc_thread(i, gc_threads[i]);
    }
#else
    ASSERT(from == 0 && to == 1);
    gc_threads = stgMallocBytes (sizeof(gc_thread*),"alloc_gc_threads");
    gc_threads[0] = gct;
    new_gc_thread(0,gc_threads[0]);
#endif
}

void
freeGcThreads (void)
{
    nat g;
    if (gc_threads != NULL) {
#if defined(THREADED_RTS)
        nat i;
	for (i = 0; i < n_capabilities; i++) {
            for (g = 0; g < RtsFlags.GcFlags.generations; g++)
            {
                freeWSDeque(gc_threads[i]->gens[g].todo_q);
            }
            stgFree (gc_threads[i]);
	}
        stgFree (gc_threads);
#else
        for (g = 0; g < RtsFlags.GcFlags.generations; g++)
        {
            freeWSDeque(gc_threads[0]->gens[g].todo_q);
        }
        stgFree (gc_threads);
#endif
        gc_threads = NULL;
    }
}

/* ----------------------------------------------------------------------------
   Start GC threads
   ------------------------------------------------------------------------- */

static volatile StgWord gc_running_threads;

static StgWord
inc_running (void)
{
    StgWord new;
    new = atomic_inc(&gc_running_threads);
    ASSERT(new <= n_gc_threads);
    return new;
}

static StgWord
dec_running (void)
{
    ASSERT(gc_running_threads != 0);
    return atomic_dec(&gc_running_threads);
}

static rtsBool
any_work (void)
{
    int g;
    gen_workspace *ws;

    gct->any_work++;

    write_barrier();

    // scavenge objects in compacted generation
    if (mark_stack_bd != NULL && !mark_stack_empty()) {
	return rtsTrue;
    }
    
    // Check for global work in any step.  We don't need to check for
    // local work, because we have already exited scavenge_loop(),
    // which means there is no local work for this thread.
    for (g = 0; g < (int)RtsFlags.GcFlags.generations; g++) {
        ws = &gct->gens[g];
        if (ws->todo_large_objects) return rtsTrue;
        if (!looksEmptyWSDeque(ws->todo_q)) return rtsTrue;
        if (ws->todo_overflow) return rtsTrue;
    }

#if defined(THREADED_RTS)
    if (work_stealing) {
        nat n;
        // look for work to steal
        for (n = 0; n < n_gc_threads; n++) {
            if (n == gct->thread_index) continue;
            for (g = RtsFlags.GcFlags.generations-1; g >= 0; g--) {
                ws = &gc_threads[n]->gens[g];
                if (!looksEmptyWSDeque(ws->todo_q)) return rtsTrue;
            }
        }
    }
#endif

    gct->no_work++;
#if defined(THREADED_RTS)
    yieldThread();
#endif

    return rtsFalse;
}    

static void
scavenge_until_all_done (void)
{
    DEBUG_ONLY( nat r );
	

loop:
#if defined(THREADED_RTS)
    if (n_gc_threads > 1) {
        scavenge_loop();
    } else {
        scavenge_loop1();
    }
#else
    scavenge_loop();
#endif

    collect_gct_blocks();

    // scavenge_loop() only exits when there's no work to do

#ifdef DEBUG
    r = dec_running();
#else
    dec_running();
#endif

    traceEventGcIdle(gct->cap);

    debugTrace(DEBUG_gc, "%d GC threads still running", r);
    
    while (gc_running_threads != 0) {
        // usleep(1);
        if (any_work()) {
            inc_running();
            traceEventGcWork(gct->cap);
            goto loop;
        }
        // any_work() does not remove the work from the queue, it
        // just checks for the presence of work.  If we find any,
        // then we increment gc_running_threads and go back to 
        // scavenge_loop() to perform any pending work.
    }
    
    traceEventGcDone(gct->cap);
}

#if defined(THREADED_RTS)

void
gcWorkerThread (Capability *cap)
{
    gc_thread *saved_gct;

    // necessary if we stole a callee-saves register for gct:
    saved_gct = gct;

    SET_GCT(gc_threads[cap->no]);
    gct->id = osThreadId();

    stat_gcWorkerThreadStart(gct);

    // Wait until we're told to wake up
    RELEASE_SPIN_LOCK(&gct->mut_spin);
    // yieldThread();
    //    Strangely, adding a yieldThread() here makes the CPU time
    //    measurements more accurate on Linux, perhaps because it syncs
    //    the CPU time across the multiple cores.  Without this, CPU time
    //    is heavily skewed towards GC rather than MUT.
    gct->wakeup = GC_THREAD_STANDING_BY;
    debugTrace(DEBUG_gc, "GC thread %d standing by...", gct->thread_index);
    ACQUIRE_SPIN_LOCK(&gct->gc_spin);
    
#ifdef USE_PAPI
    // start performance counters in this thread...
    if (gct->papi_events == -1) {
        papi_init_eventset(&gct->papi_events);
    }
    papi_thread_start_gc1_count(gct->papi_events);
#endif

    init_gc_thread(gct);

    traceEventGcWork(gct->cap);

    // Every thread evacuates some roots.
    gct->evac_gen_no = 0;
    markCapability(mark_root, gct, cap, rtsTrue/*prune sparks*/);
    scavenge_capability_mut_lists(cap);

    scavenge_until_all_done();
    
#ifdef THREADED_RTS
    // Now that the whole heap is marked, we discard any sparks that
    // were found to be unreachable.  The main GC thread is currently
    // marking heap reachable via weak pointers, so it is
    // non-deterministic whether a spark will be retained if it is
    // only reachable via weak pointers.  To fix this problem would
    // require another GC barrier, which is too high a price.
    pruneSparkQueue(cap);
#endif

#ifdef USE_PAPI
    // count events in this thread towards the GC totals
    papi_thread_stop_gc1_count(gct->papi_events);
#endif

    // Wait until we're told to continue
    RELEASE_SPIN_LOCK(&gct->gc_spin);
    gct->wakeup = GC_THREAD_WAITING_TO_CONTINUE;
    debugTrace(DEBUG_gc, "GC thread %d waiting to continue...", 
               gct->thread_index);
    ACQUIRE_SPIN_LOCK(&gct->mut_spin);
    debugTrace(DEBUG_gc, "GC thread %d on my way...", gct->thread_index);

    // record the time spent doing GC in the Task structure
    stat_gcWorkerThreadDone(gct);

    SET_GCT(saved_gct);
}

#endif

#if defined(THREADED_RTS)

void
waitForGcThreads (Capability *cap USED_IF_THREADS)
{
    const nat n_threads = n_capabilities;
    const nat me = cap->no;
    nat i, j;
    rtsBool retry = rtsTrue;

    while(retry) {
        for (i=0; i < n_threads; i++) {
            if (i == me || gc_threads[i]->idle) continue;
            if (gc_threads[i]->wakeup != GC_THREAD_STANDING_BY) {
                prodCapability(&capabilities[i], cap->running_task);
            }
        }
        for (j=0; j < 10; j++) {
            retry = rtsFalse;
            for (i=0; i < n_threads; i++) {
                if (i == me || gc_threads[i]->idle) continue;
                write_barrier();
                interruptCapability(&capabilities[i]);
                if (gc_threads[i]->wakeup != GC_THREAD_STANDING_BY) {
                    retry = rtsTrue;
                }
            }
            if (!retry) break;
            yieldThread();
        }
    }
}

#endif // THREADED_RTS

static void
start_gc_threads (void)
{
#if defined(THREADED_RTS)
    gc_running_threads = 0;
#endif
}

static void
wakeup_gc_threads (nat me USED_IF_THREADS)
{
#if defined(THREADED_RTS)
    nat i;

    if (n_gc_threads == 1) return;

    for (i=0; i < n_gc_threads; i++) {
        if (i == me || gc_threads[i]->idle) continue;
        inc_running();
        debugTrace(DEBUG_gc, "waking up gc thread %d", i);
        if (gc_threads[i]->wakeup != GC_THREAD_STANDING_BY) barf("wakeup_gc_threads");

	gc_threads[i]->wakeup = GC_THREAD_RUNNING;
        ACQUIRE_SPIN_LOCK(&gc_threads[i]->mut_spin);
        RELEASE_SPIN_LOCK(&gc_threads[i]->gc_spin);
    }
#endif
}

// After GC is complete, we must wait for all GC threads to enter the
// standby state, otherwise they may still be executing inside
// any_work(), and may even remain awake until the next GC starts.
static void
shutdown_gc_threads (nat me USED_IF_THREADS)
{
#if defined(THREADED_RTS)
    nat i;

    if (n_gc_threads == 1) return;

    for (i=0; i < n_gc_threads; i++) {
        if (i == me || gc_threads[i]->idle) continue;
        while (gc_threads[i]->wakeup != GC_THREAD_WAITING_TO_CONTINUE) { write_barrier(); }
    }
#endif
}

#if defined(THREADED_RTS)
void
releaseGCThreads (Capability *cap USED_IF_THREADS)
{
    const nat n_threads = n_capabilities;
    const nat me = cap->no;
    nat i;
    for (i=0; i < n_threads; i++) {
        if (i == me || gc_threads[i]->idle) continue;
        if (gc_threads[i]->wakeup != GC_THREAD_WAITING_TO_CONTINUE)
            barf("releaseGCThreads");
        
        gc_threads[i]->wakeup = GC_THREAD_INACTIVE;
        ACQUIRE_SPIN_LOCK(&gc_threads[i]->gc_spin);
        RELEASE_SPIN_LOCK(&gc_threads[i]->mut_spin);
    }
}
#endif

/* ----------------------------------------------------------------------------
   Initialise a generation that is to be collected 
   ------------------------------------------------------------------------- */

static void
prepare_collected_gen (generation *gen)
{
    nat i, g, n;
    gen_workspace *ws;
    bdescr *bd, *next;

    // Throw away the current mutable list.  Invariant: the mutable
    // list always has at least one block; this means we can avoid a
    // check for NULL in recordMutable().
    g = gen->no;
    if (g != 0) {
        for (i = 0; i < n_capabilities; i++) {
	    freeChain(capabilities[i].mut_lists[g]);
	    capabilities[i].mut_lists[g] = allocBlock();
	}
    }

    gen = &generations[g];
    ASSERT(gen->no == g);

    // we'll construct a new list of threads in this step
    // during GC, throw away the current list.
    gen->old_threads = gen->threads;
    gen->threads = END_TSO_QUEUE;

    // deprecate the existing blocks
    gen->old_blocks   = gen->blocks;
    gen->n_old_blocks = gen->n_blocks;
    gen->blocks       = NULL;
    gen->n_blocks     = 0;
    gen->n_words      = 0;
    gen->live_estimate = 0;

    // initialise the large object queues.
    ASSERT(gen->scavenged_large_objects == NULL);
    ASSERT(gen->n_scavenged_large_blocks == 0);

    // grab all the partial blocks stashed in the gc_thread workspaces and
    // move them to the old_blocks list of this gen.
    for (n = 0; n < n_capabilities; n++) {
        ws = &gc_threads[n]->gens[gen->no];

        for (bd = ws->part_list; bd != NULL; bd = next) {
            next = bd->link;
            bd->link = gen->old_blocks;
            gen->old_blocks = bd;
            gen->n_old_blocks += bd->blocks;
        }
        ws->part_list = NULL;
        ws->n_part_blocks = 0;

        ASSERT(ws->scavd_list == NULL);
        ASSERT(ws->n_scavd_blocks == 0);

        if (ws->todo_free != ws->todo_bd->start) {
            ws->todo_bd->free = ws->todo_free;
            ws->todo_bd->link = gen->old_blocks;
            gen->old_blocks = ws->todo_bd;
            gen->n_old_blocks += ws->todo_bd->blocks;
            alloc_todo_block(ws,0); // always has one block.
        }
    }

    // mark the small objects as from-space
    for (bd = gen->old_blocks; bd; bd = bd->link) {
        bd->flags &= ~BF_EVACUATED;
    }
    
    // mark the large objects as from-space
    for (bd = gen->large_objects; bd; bd = bd->link) {
        bd->flags &= ~BF_EVACUATED;
    }

    // for a compacted generation, we need to allocate the bitmap
    if (gen->mark) {
        lnat bitmap_size; // in bytes
        bdescr *bitmap_bdescr;
        StgWord *bitmap;
	
        bitmap_size = gen->n_old_blocks * BLOCK_SIZE / (sizeof(W_)*BITS_PER_BYTE);
	
        if (bitmap_size > 0) {
            bitmap_bdescr = allocGroup((lnat)BLOCK_ROUND_UP(bitmap_size) 
                                       / BLOCK_SIZE);
            gen->bitmap = bitmap_bdescr;
            bitmap = bitmap_bdescr->start;
            
            debugTrace(DEBUG_gc, "bitmap_size: %d, bitmap: %p",
                       bitmap_size, bitmap);
            
            // don't forget to fill it with zeros!
            memset(bitmap, 0, bitmap_size);
            
            // For each block in this step, point to its bitmap from the
            // block descriptor.
            for (bd=gen->old_blocks; bd != NULL; bd = bd->link) {
                bd->u.bitmap = bitmap;
                bitmap += BLOCK_SIZE_W / (sizeof(W_)*BITS_PER_BYTE);
		
                // Also at this point we set the BF_MARKED flag
                // for this block.  The invariant is that
                // BF_MARKED is always unset, except during GC
                // when it is set on those blocks which will be
                // compacted.
                if (!(bd->flags & BF_FRAGMENTED)) {
                    bd->flags |= BF_MARKED;
                }

                // BF_SWEPT should be marked only for blocks that are being
                // collected in sweep()
                bd->flags &= ~BF_SWEPT;
            }
        }
    }
}


/* ----------------------------------------------------------------------------
   Save the mutable lists in saved_mut_lists
   ------------------------------------------------------------------------- */

static void
stash_mut_list (Capability *cap, nat gen_no)
{
    cap->saved_mut_lists[gen_no] = cap->mut_lists[gen_no];
    cap->mut_lists[gen_no] = allocBlock_sync();
}

/* ----------------------------------------------------------------------------
   Initialise a generation that is *not* to be collected 
   ------------------------------------------------------------------------- */

static void
prepare_uncollected_gen (generation *gen)
{
    nat i;


    ASSERT(gen->no > 0);

    // save the current mutable lists for this generation, and
    // allocate a fresh block for each one.  We'll traverse these
    // mutable lists as roots early on in the GC.
    for (i = 0; i < n_capabilities; i++) {
        stash_mut_list(&capabilities[i], gen->no);
    }

    ASSERT(gen->scavenged_large_objects == NULL);
    ASSERT(gen->n_scavenged_large_blocks == 0);
}

/* -----------------------------------------------------------------------------
   Collect the completed blocks from a GC thread and attach them to
   the generation.
   -------------------------------------------------------------------------- */

static void
collect_gct_blocks (void)
{
    nat g;
    gen_workspace *ws;
    bdescr *bd, *prev;
    
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
        ws = &gct->gens[g];
        
        // there may still be a block attached to ws->todo_bd;
        // leave it there to use next time.

        if (ws->scavd_list != NULL) {
            ACQUIRE_SPIN_LOCK(&ws->gen->sync);

            ASSERT(gct->scan_bd == NULL);
            ASSERT(countBlocks(ws->scavd_list) == ws->n_scavd_blocks);
        
            prev = NULL;
            for (bd = ws->scavd_list; bd != NULL; bd = bd->link) {
                ws->gen->n_words += bd->free - bd->start;
                prev = bd;
            }
            if (prev != NULL) {
                prev->link = ws->gen->blocks;
                ws->gen->blocks = ws->scavd_list;
            } 
            ws->gen->n_blocks += ws->n_scavd_blocks;

            ws->scavd_list = NULL;
            ws->n_scavd_blocks = 0;

            RELEASE_SPIN_LOCK(&ws->gen->sync);
        }
    }
}

/* -----------------------------------------------------------------------------
   Initialise a gc_thread before GC
   -------------------------------------------------------------------------- */

static void
init_gc_thread (gc_thread *t)
{
    t->static_objects = END_OF_STATIC_LIST;
    t->scavenged_static_objects = END_OF_STATIC_LIST;
    t->scan_bd = NULL;
    t->mut_lists = t->cap->mut_lists;
    t->evac_gen_no = 0;
    t->failed_to_evac = rtsFalse;
    t->eager_promotion = rtsTrue;
    t->thunk_selector_depth = 0;
    t->copied = 0;
    t->scanned = 0;
    t->any_work = 0;
    t->no_work = 0;
    t->scav_find_work = 0;
}

/* -----------------------------------------------------------------------------
   Function we pass to evacuate roots.
   -------------------------------------------------------------------------- */

static void
mark_root(void *user USED_IF_THREADS, StgClosure **root)
{
    // we stole a register for gct, but this function is called from
    // *outside* the GC where the register variable is not in effect,
    // so we need to save and restore it here.  NB. only call
    // mark_root() from the main GC thread, otherwise gct will be
    // incorrect.
#if defined(THREADED_RTS)
    gc_thread *saved_gct;
    saved_gct = gct;
#endif
    SET_GCT(user);
    
    evacuate(root);
    
    SET_GCT(saved_gct);
}

/* -----------------------------------------------------------------------------
   Initialising the static object & mutable lists
   -------------------------------------------------------------------------- */

static void
zero_static_object_list(StgClosure* first_static)
{
  StgClosure* p;
  StgClosure* link;
  const StgInfoTable *info;

  for (p = first_static; p != END_OF_STATIC_LIST; p = link) {
    info = get_itbl(p);
    link = *STATIC_LINK(info, p);
    *STATIC_LINK(info,p) = NULL;
  }
}

/* ----------------------------------------------------------------------------
   Reset the sizes of the older generations when we do a major
   collection.
  
   CURRENT STRATEGY: make all generations except zero the same size.
   We have to stay within the maximum heap size, and leave a certain
   percentage of the maximum heap size available to allocate into.
   ------------------------------------------------------------------------- */

static void
resize_generations (void)
{
    nat g;

    if (major_gc && RtsFlags.GcFlags.generations > 1) {
	nat live, size, min_alloc, words;
	const nat max  = RtsFlags.GcFlags.maxHeapSize;
	const nat gens = RtsFlags.GcFlags.generations;
	
	// live in the oldest generations
        if (oldest_gen->live_estimate != 0) {
            words = oldest_gen->live_estimate;
        } else {
            words = oldest_gen->n_words;
        }
        live = (words + BLOCK_SIZE_W - 1) / BLOCK_SIZE_W +
            oldest_gen->n_large_blocks;
	
	// default max size for all generations except zero
	size = stg_max(live * RtsFlags.GcFlags.oldGenFactor,
		       RtsFlags.GcFlags.minOldGenSize);
	
        if (RtsFlags.GcFlags.heapSizeSuggestionAuto) {
            RtsFlags.GcFlags.heapSizeSuggestion = size;
        }

	// minimum size for generation zero
	min_alloc = stg_max((RtsFlags.GcFlags.pcFreeHeap * max) / 200,
			    RtsFlags.GcFlags.minAllocAreaSize);

	// Auto-enable compaction when the residency reaches a
	// certain percentage of the maximum heap size (default: 30%).
	if (RtsFlags.GcFlags.compact ||
            (max > 0 &&
             oldest_gen->n_blocks > 
             (RtsFlags.GcFlags.compactThreshold * max) / 100)) {
	    oldest_gen->mark = 1;
	    oldest_gen->compact = 1;
//	  debugBelch("compaction: on\n", live);
	} else {
	    oldest_gen->mark = 0;
	    oldest_gen->compact = 0;
//	  debugBelch("compaction: off\n", live);
	}

        if (RtsFlags.GcFlags.sweep) {
	    oldest_gen->mark = 1;
        }

	// if we're going to go over the maximum heap size, reduce the
	// size of the generations accordingly.  The calculation is
	// different if compaction is turned on, because we don't need
	// to double the space required to collect the old generation.
	if (max != 0) {
	    
	    // this test is necessary to ensure that the calculations
	    // below don't have any negative results - we're working
	    // with unsigned values here.
	    if (max < min_alloc) {
		heapOverflow();
	    }
	    
	    if (oldest_gen->compact) {
		if ( (size + (size - 1) * (gens - 2) * 2) + min_alloc > max ) {
		    size = (max - min_alloc) / ((gens - 1) * 2 - 1);
		}
	    } else {
		if ( (size * (gens - 1) * 2) + min_alloc > max ) {
		    size = (max - min_alloc) / ((gens - 1) * 2);
		}
	    }
	    
	    if (size < live) {
		heapOverflow();
	    }
	}
	
#if 0
	debugBelch("live: %d, min_alloc: %d, size : %d, max = %d\n", live,
		   min_alloc, size, max);
#endif
	
	for (g = 0; g < gens; g++) {
	    generations[g].max_blocks = size;
	}
    }
}

/* -----------------------------------------------------------------------------
   Calculate the new size of the nursery, and resize it.
   -------------------------------------------------------------------------- */

static void
resize_nursery (void)
{
    const lnat min_nursery = RtsFlags.GcFlags.minAllocAreaSize * n_capabilities;

    if (RtsFlags.GcFlags.generations == 1)
    {   // Two-space collector:
	nat blocks;
    
	/* set up a new nursery.  Allocate a nursery size based on a
	 * function of the amount of live data (by default a factor of 2)
	 * Use the blocks from the old nursery if possible, freeing up any
	 * left over blocks.
	 *
	 * If we get near the maximum heap size, then adjust our nursery
	 * size accordingly.  If the nursery is the same size as the live
	 * data (L), then we need 3L bytes.  We can reduce the size of the
	 * nursery to bring the required memory down near 2L bytes.
	 * 
	 * A normal 2-space collector would need 4L bytes to give the same
	 * performance we get from 3L bytes, reducing to the same
	 * performance at 2L bytes.
	 */
	blocks = generations[0].n_blocks;
	
	if ( RtsFlags.GcFlags.maxHeapSize != 0 &&
	     blocks * RtsFlags.GcFlags.oldGenFactor * 2 > 
	     RtsFlags.GcFlags.maxHeapSize )
	{
	    long adjusted_blocks;  // signed on purpose 
	    int pc_free; 
	    
	    adjusted_blocks = (RtsFlags.GcFlags.maxHeapSize - 2 * blocks);
	    
	    debugTrace(DEBUG_gc, "near maximum heap size of 0x%x blocks, blocks = %d, adjusted to %ld", 
		       RtsFlags.GcFlags.maxHeapSize, blocks, adjusted_blocks);
	    
	    pc_free = adjusted_blocks * 100 / RtsFlags.GcFlags.maxHeapSize;
	    if (pc_free < RtsFlags.GcFlags.pcFreeHeap) /* might even * be < 0 */
	    {
		heapOverflow();
	    }
	    blocks = adjusted_blocks;
	}
	else
	{
	    blocks *= RtsFlags.GcFlags.oldGenFactor;
	    if (blocks < min_nursery)
	    {
		blocks = min_nursery;
	    }
	}
	resizeNurseries(blocks);
    }
    else  // Generational collector
    {
	/* 
	 * If the user has given us a suggested heap size, adjust our
	 * allocation area to make best use of the memory available.
	 */
	if (RtsFlags.GcFlags.heapSizeSuggestion)
	{
	    long blocks;
	    const nat needed = calcNeeded(); 	// approx blocks needed at next GC 
	    
	    /* Guess how much will be live in generation 0 step 0 next time.
	     * A good approximation is obtained by finding the
	     * percentage of g0 that was live at the last minor GC.
	     *
	     * We have an accurate figure for the amount of copied data in
	     * 'copied', but we must convert this to a number of blocks, with
	     * a small adjustment for estimated slop at the end of a block
	     * (- 10 words).
	     */
	    if (N == 0)
	    {
		g0_pcnt_kept = ((copied / (BLOCK_SIZE_W - 10)) * 100)
		    / countNurseryBlocks();
	    }
	    
	    /* Estimate a size for the allocation area based on the
	     * information available.  We might end up going slightly under
	     * or over the suggested heap size, but we should be pretty
	     * close on average.
	     *
	     * Formula:            suggested - needed
	     *                ----------------------------
	     *                    1 + g0_pcnt_kept/100
	     *
	     * where 'needed' is the amount of memory needed at the next
	     * collection for collecting all gens except g0.
	     */
	    blocks = 
		(((long)RtsFlags.GcFlags.heapSizeSuggestion - (long)needed) * 100) /
		(100 + (long)g0_pcnt_kept);
	    
	    if (blocks < (long)min_nursery) {
		blocks = min_nursery;
	    }
	    
	    resizeNurseries((nat)blocks);
	}
	else
	{
	    // we might have added extra large blocks to the nursery, so
	    // resize back to minAllocAreaSize again.
	    resizeNurseriesFixed(RtsFlags.GcFlags.minAllocAreaSize);
	}
    }
}

/* -----------------------------------------------------------------------------
   Sanity code for CAF garbage collection.

   With DEBUG turned on, we manage a CAF list in addition to the SRT
   mechanism.  After GC, we run down the CAF list and blackhole any
   CAFs which have been garbage collected.  This means we get an error
   whenever the program tries to enter a garbage collected CAF.

   Any garbage collected CAFs are taken off the CAF list at the same
   time. 
   -------------------------------------------------------------------------- */

#if 0 && defined(DEBUG)

static void
gcCAFs(void)
{
  StgClosure*  p;
  StgClosure** pp;
  const StgInfoTable *info;
  nat i;

  i = 0;
  p = caf_list;
  pp = &caf_list;

  while (p != NULL) {
    
    info = get_itbl(p);

    ASSERT(info->type == IND_STATIC);

    if (STATIC_LINK(info,p) == NULL) {
	debugTrace(DEBUG_gccafs, "CAF gc'd at 0x%04lx", (long)p);
	// black hole it 
	SET_INFO(p,&stg_BLACKHOLE_info);
	p = STATIC_LINK2(info,p);
	*pp = p;
    }
    else {
      pp = &STATIC_LINK2(info,p);
      p = *pp;
      i++;
    }

  }

  debugTrace(DEBUG_gccafs, "%d CAFs live", i); 
}
#endif
