/*
 * sgen-gc.c: Simple generational GC.
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *  Rodrigo Kumpera (kumpera@gmail.com)
 *
 * Copyright 2005-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 *
 * Thread start/stop adapted from Boehm's GC:
 * Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1998 by Fergus Henderson.  All rights reserved.
 * Copyright (c) 2000-2004 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin, Inc.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Important: allocation provides always zeroed memory, having to do
 * a memset after allocation is deadly for performance.
 * Memory usage at startup is currently as follows:
 * 64 KB pinned space
 * 64 KB internal space
 * size of nursery
 * We should provide a small memory config with half the sizes
 *
 * We currently try to make as few mono assumptions as possible:
 * 1) 2-word header with no GC pointers in it (first vtable, second to store the
 *    forwarding ptr)
 * 2) gc descriptor is the second word in the vtable (first word in the class)
 * 3) 8 byte alignment is the minimum and enough (not true for special structures (SIMD), FIXME)
 * 4) there is a function to get an object's size and the number of
 *    elements in an array.
 * 5) we know the special way bounds are allocated for complex arrays
 * 6) we know about proxies and how to treat them when domains are unloaded
 *
 * Always try to keep stack usage to a minimum: no recursive behaviour
 * and no large stack allocs.
 *
 * General description.
 * Objects are initially allocated in a nursery using a fast bump-pointer technique.
 * When the nursery is full we start a nursery collection: this is performed with a
 * copying GC.
 * When the old generation is full we start a copying GC of the old generation as well:
 * this will be changed to mark&sweep with copying when fragmentation becomes to severe
 * in the future.  Maybe we'll even do both during the same collection like IMMIX.
 *
 * The things that complicate this description are:
 * *) pinned objects: we can't move them so we need to keep track of them
 * *) no precise info of the thread stacks and registers: we need to be able to
 *    quickly find the objects that may be referenced conservatively and pin them
 *    (this makes the first issues more important)
 * *) large objects are too expensive to be dealt with using copying GC: we handle them
 *    with mark/sweep during major collections
 * *) some objects need to not move even if they are small (interned strings, Type handles):
 *    we use mark/sweep for them, too: they are not allocated in the nursery, but inside
 *    PinnedChunks regions
 */

/*
 * TODO:

 *) we could have a function pointer in MonoClass to implement
  customized write barriers for value types

 *) investigate the stuff needed to advance a thread to a GC-safe
  point (single-stepping, read from unmapped memory etc) and implement it.
  This would enable us to inline allocations and write barriers, for example,
  or at least parts of them, like the write barrier checks.
  We may need this also for handling precise info on stacks, even simple things
  as having uninitialized data on the stack and having to wait for the prolog
  to zero it. Not an issue for the last frame that we scan conservatively.
  We could always not trust the value in the slots anyway.

 *) modify the jit to save info about references in stack locations:
  this can be done just for locals as a start, so that at least
  part of the stack is handled precisely.

 *) test/fix endianess issues

 *) Implement a card table as the write barrier instead of remembered
    sets?  Card tables are not easy to implement with our current
    memory layout.  We have several different kinds of major heap
    objects: Small objects in regular blocks, small objects in pinned
    chunks and LOS objects.  If we just have a pointer we have no way
    to tell which kind of object it points into, therefore we cannot
    know where its card table is.  The least we have to do to make
    this happen is to get rid of write barriers for indirect stores.
    (See next item)

 *) Get rid of write barriers for indirect stores.  We can do this by
    telling the GC to wbarrier-register an object once we do an ldloca
    or ldelema on it, and to unregister it once it's not used anymore
    (it can only travel downwards on the stack).  The problem with
    unregistering is that it needs to happen eventually no matter
    what, even if exceptions are thrown, the thread aborts, etc.
    Rodrigo suggested that we could do only the registering part and
    let the collector find out (pessimistically) when it's safe to
    unregister, namely when the stack pointer of the thread that
    registered the object is higher than it was when the registering
    happened.  This might make for a good first implementation to get
    some data on performance.

 *) Some sort of blacklist support?  Blacklists is a concept from the
    Boehm GC: if during a conservative scan we find pointers to an
    area which we might use as heap, we mark that area as unusable, so
    pointer retention by random pinning pointers is reduced.

 *) experiment with max small object size (very small right now - 2kb,
    because it's tied to the max freelist size)

  *) add an option to mmap the whole heap in one chunk: it makes for many
     simplifications in the checks (put the nursery at the top and just use a single
     check for inclusion/exclusion): the issue this has is that on 32 bit systems it's
     not flexible (too much of the address space may be used by default or we can't
     increase the heap as needed) and we'd need a race-free mechanism to return memory
     back to the system (mprotect(PROT_NONE) will still keep the memory allocated if it
     was written to, munmap is needed, but the following mmap may not find the same segment
     free...)

 *) memzero the major fragments after restarting the world and optionally a smaller
    chunk at a time

 *) investigate having fragment zeroing threads

 *) separate locks for finalization and other minor stuff to reduce
    lock contention

 *) try a different copying order to improve memory locality

 *) a thread abort after a store but before the write barrier will
    prevent the write barrier from executing

 *) specialized dynamically generated markers/copiers

 *) Dynamically adjust TLAB size to the number of threads.  If we have
    too many threads that do allocation, we might need smaller TLABs,
    and we might get better performance with larger TLABs if we only
    have a handful of threads.  We could sum up the space left in all
    assigned TLABs and if that's more than some percentage of the
    nursery size, reduce the TLAB size.

 *) Explore placing unreachable objects on unused nursery memory.
	Instead of memset'ng a region to zero, place an int[] covering it.
	A good place to start is add_nursery_frag. The tricky thing here is
	placing those objects atomically outside of a collection.

 *) Allocation should use asymmetric Dekker synchronization:
 	http://blogs.oracle.com/dave/resource/Asymmetric-Dekker-Synchronization.txt
	This should help weak consistency archs.
 */
#include "config.h"
#ifdef HAVE_SGEN_GC

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#ifdef __MACH__
#undef _XOPEN_SOURCE
#endif
#ifdef __MACH__
#define _XOPEN_SOURCE
#endif

#include "metadata/sgen-gc.h"
#include "metadata/metadata-internals.h"
#include "metadata/class-internals.h"
#include "metadata/gc-internal.h"
#include "metadata/object-internals.h"
#include "metadata/threads.h"
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-ssb.h"
#include "metadata/sgen-protocol.h"
#include "metadata/sgen-archdep.h"
#include "metadata/sgen-bridge.h"
#include "metadata/mono-gc.h"
#include "metadata/method-builder.h"
#include "metadata/profiler-private.h"
#include "metadata/monitor.h"
#include "metadata/threadpool-internals.h"
#include "metadata/mempool-internals.h"
#include "metadata/marshal.h"
#include "metadata/runtime.h"
#include "metadata/sgen-cardtable.h"
#include "metadata/sgen-pinning.h"
#include "metadata/sgen-workers.h"
#include "utils/mono-mmap.h"
#include "utils/mono-time.h"
#include "utils/mono-semaphore.h"
#include "utils/mono-counters.h"
#include "utils/mono-proclib.h"
#include "utils/mono-memory-model.h"
#include "utils/mono-logger-internal.h"

#include <mono/utils/mono-logger-internal.h>
#include <mono/utils/memcheck.h>

#if defined(__MACH__)
#include "utils/mach-support.h"
#endif

#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	CEE_LAST
};

#undef OPDEF

#undef pthread_create
#undef pthread_join
#undef pthread_detach

/*
 * ######################################################################
 * ########  Types and constants used by the GC.
 * ######################################################################
 */

/* 0 means not initialized, 1 is initialized, -1 means in progress */
static gint32 gc_initialized = 0;
/* If set, do a minor collection before every X allocation */
guint32 collect_before_allocs = 0;
/* If set, do a heap consistency check before each minor collection */
static gboolean consistency_check_at_minor_collection = FALSE;
/* If set, check that there are no references to the domain left at domain unload */
static gboolean xdomain_checks = FALSE;
/* If not null, dump the heap after each collection into this file */
static FILE *heap_dump_file = NULL;
/* If set, mark stacks conservatively, even if precise marking is possible */
static gboolean conservative_stack_mark = FALSE;
/* If set, do a plausibility check on the scan_starts before and after
   each collection */
static gboolean do_scan_starts_check = FALSE;
static gboolean nursery_collection_is_parallel = FALSE;
static gboolean disable_minor_collections = FALSE;
static gboolean disable_major_collections = FALSE;
gboolean do_pin_stats = FALSE;
static gboolean do_verify_nursery = FALSE;
static gboolean do_dump_nursery_content = FALSE;

#ifdef HEAVY_STATISTICS
long long stat_objects_alloced_degraded = 0;
long long stat_bytes_alloced_degraded = 0;

long long stat_copy_object_called_nursery = 0;
long long stat_objects_copied_nursery = 0;
long long stat_copy_object_called_major = 0;
long long stat_objects_copied_major = 0;

long long stat_scan_object_called_nursery = 0;
long long stat_scan_object_called_major = 0;

long long stat_nursery_copy_object_failed_from_space = 0;
long long stat_nursery_copy_object_failed_forwarded = 0;
long long stat_nursery_copy_object_failed_pinned = 0;

static int stat_wbarrier_set_field = 0;
static int stat_wbarrier_set_arrayref = 0;
static int stat_wbarrier_arrayref_copy = 0;
static int stat_wbarrier_generic_store = 0;
static int stat_wbarrier_set_root = 0;
static int stat_wbarrier_value_copy = 0;
static int stat_wbarrier_object_copy = 0;
#endif

int stat_minor_gcs = 0;
int stat_major_gcs = 0;

static long long stat_pinned_objects = 0;

static long long time_minor_pre_collection_fragment_clear = 0;
static long long time_minor_pinning = 0;
static long long time_minor_scan_remsets = 0;
static long long time_minor_scan_pinned = 0;
static long long time_minor_scan_registered_roots = 0;
static long long time_minor_scan_thread_data = 0;
static long long time_minor_finish_gray_stack = 0;
static long long time_minor_fragment_creation = 0;

static long long time_major_pre_collection_fragment_clear = 0;
static long long time_major_pinning = 0;
static long long time_major_scan_pinned = 0;
static long long time_major_scan_registered_roots = 0;
static long long time_major_scan_thread_data = 0;
static long long time_major_scan_alloc_pinned = 0;
static long long time_major_scan_finalized = 0;
static long long time_major_scan_big_objects = 0;
static long long time_major_finish_gray_stack = 0;
static long long time_major_free_bigobjs = 0;
static long long time_major_los_sweep = 0;
static long long time_major_sweep = 0;
static long long time_major_fragment_creation = 0;

int gc_debug_level = 0;
FILE* gc_debug_file;
static gboolean debug_print_allowance = FALSE;

/*
void
mono_gc_flush_info (void)
{
	fflush (gc_debug_file);
}
*/

#define TV_DECLARE SGEN_TV_DECLARE
#define TV_GETTIME SGEN_TV_GETTIME
#define TV_ELAPSED SGEN_TV_ELAPSED
#define TV_ELAPSED_MS SGEN_TV_ELAPSED_MS

#define ALIGN_TO(val,align) ((((guint64)val) + ((align) - 1)) & ~((align) - 1))

NurseryClearPolicy nursery_clear_policy = CLEAR_AT_TLAB_CREATION;

/* the runtime can register areas of memory as roots: we keep two lists of roots,
 * a pinned root set for conservatively scanned roots and a normal one for
 * precisely scanned roots (currently implemented as a single list).
 */
typedef struct _RootRecord RootRecord;
struct _RootRecord {
	char *end_root;
	mword root_desc;
};

#define object_is_forwarded	SGEN_OBJECT_IS_FORWARDED
#define object_is_pinned	SGEN_OBJECT_IS_PINNED
#define pin_object		SGEN_PIN_OBJECT
#define unpin_object		SGEN_UNPIN_OBJECT

#define ptr_in_nursery mono_sgen_ptr_in_nursery

#define LOAD_VTABLE	SGEN_LOAD_VTABLE

static const char*
safe_name (void* obj)
{
	MonoVTable *vt = (MonoVTable*)LOAD_VTABLE (obj);
	return vt->klass->name;
}

#define safe_object_get_size	mono_sgen_safe_object_get_size

const char*
mono_sgen_safe_name (void* obj)
{
	return safe_name (obj);
}

/*
 * ######################################################################
 * ########  Global data.
 * ######################################################################
 */
LOCK_DECLARE (gc_mutex);
static int gc_disabled = 0;

static gboolean use_cardtable;

#define MIN_MINOR_COLLECTION_ALLOWANCE	(DEFAULT_NURSERY_SIZE * 4)

#define SCAN_START_SIZE	SGEN_SCAN_START_SIZE

static mword pagesize = 4096;
static mword nursery_size;
int degraded_mode = 0;

static mword bytes_pinned_from_failed_allocation = 0;

static mword total_alloc = 0;
/* use this to tune when to do a major/minor collection */
static mword memory_pressure = 0;
static mword minor_collection_allowance;
static int minor_collection_sections_alloced = 0;


/* GC Logging stats */
static int last_major_num_sections = 0;
static int last_los_memory_usage = 0;
static gboolean major_collection_happened = FALSE;

GCMemSection *nursery_section = NULL;
static mword lowest_heap_address = ~(mword)0;
static mword highest_heap_address = 0;

static LOCK_DECLARE (interruption_mutex);
static LOCK_DECLARE (pin_queue_mutex);

#define LOCK_PIN_QUEUE mono_mutex_lock (&pin_queue_mutex)
#define UNLOCK_PIN_QUEUE mono_mutex_unlock (&pin_queue_mutex)

typedef struct _FinalizeReadyEntry FinalizeReadyEntry;
struct _FinalizeReadyEntry {
	FinalizeReadyEntry *next;
	void *object;
};

typedef struct _EphemeronLinkNode EphemeronLinkNode;

struct _EphemeronLinkNode {
	EphemeronLinkNode *next;
	char *array;
};

typedef struct {
       void *key;
       void *value;
} Ephemeron;

int current_collection_generation = -1;

/*
 * The link pointer is hidden by negating each bit.  We use the lowest
 * bit of the link (before negation) to store whether it needs
 * resurrection tracking.
 */
#define HIDE_POINTER(p,t)	((gpointer)(~((gulong)(p)|((t)?1:0))))
#define REVEAL_POINTER(p)	((gpointer)((~(gulong)(p))&~3L))

/* objects that are ready to be finalized */
static FinalizeReadyEntry *fin_ready_list = NULL;
static FinalizeReadyEntry *critical_fin_list = NULL;

static EphemeronLinkNode *ephemeron_list;

static int num_ready_finalizers = 0;
static int no_finalize = 0;

enum {
	ROOT_TYPE_NORMAL = 0, /* "normal" roots */
	ROOT_TYPE_PINNED = 1, /* roots without a GC descriptor */
	ROOT_TYPE_WBARRIER = 2, /* roots with a write barrier */
	ROOT_TYPE_NUM
};

/* registered roots: the key to the hash is the root start address */
/* 
 * Different kinds of roots are kept separate to speed up pin_from_roots () for example.
 */
static SgenHashTable roots_hash [ROOT_TYPE_NUM] = {
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL),
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL),
	SGEN_HASH_TABLE_INIT (INTERNAL_MEM_ROOTS_TABLE, INTERNAL_MEM_ROOT_RECORD, sizeof (RootRecord), mono_aligned_addr_hash, NULL)
};
static mword roots_size = 0; /* amount of memory in the root set */

#define GC_ROOT_NUM 32
typedef struct {
	int count;
	void *objects [GC_ROOT_NUM];
	int root_types [GC_ROOT_NUM];
	uintptr_t extra_info [GC_ROOT_NUM];
} GCRootReport;

static void
notify_gc_roots (GCRootReport *report)
{
	if (!report->count)
		return;
	mono_profiler_gc_roots (report->count, report->objects, report->root_types, report->extra_info);
	report->count = 0;
}

static void
add_profile_gc_root (GCRootReport *report, void *object, int rtype, uintptr_t extra_info)
{
	if (report->count == GC_ROOT_NUM)
		notify_gc_roots (report);
	report->objects [report->count] = object;
	report->root_types [report->count] = rtype;
	report->extra_info [report->count++] = (uintptr_t)((MonoVTable*)LOAD_VTABLE (object))->klass;
}

MonoNativeTlsKey thread_info_key;

#ifdef HAVE_KW_THREAD
__thread SgenThreadInfo *thread_info;
__thread gpointer *store_remset_buffer;
__thread long store_remset_buffer_index;
__thread char *stack_end;
__thread long *store_remset_buffer_index_addr;
#endif

/* The size of a TLAB */
/* The bigger the value, the less often we have to go to the slow path to allocate a new 
 * one, but the more space is wasted by threads not allocating much memory.
 * FIXME: Tune this.
 * FIXME: Make this self-tuning for each thread.
 */
guint32 tlab_size = (1024 * 4);

#define MAX_SMALL_OBJ_SIZE	SGEN_MAX_SMALL_OBJ_SIZE

/* Functions supplied by the runtime to be called by the GC */
static MonoGCCallbacks gc_callbacks;

#define ALLOC_ALIGN		SGEN_ALLOC_ALIGN
#define ALLOC_ALIGN_BITS	SGEN_ALLOC_ALIGN_BITS

#define ALIGN_UP		SGEN_ALIGN_UP

#define MOVED_OBJECTS_NUM 64
static void *moved_objects [MOVED_OBJECTS_NUM];
static int moved_objects_idx = 0;

/* Vtable of the objects used to fill out nursery fragments before a collection */
static MonoVTable *array_fill_vtable;

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
MonoNativeThreadId main_gc_thread = NULL;
#endif

/*
 * ######################################################################
 * ########  Heap size accounting
 * ######################################################################
 */
/*heap limits*/
static mword max_heap_size = ((mword)0)- ((mword)1);
static mword soft_heap_limit = ((mword)0) - ((mword)1);
static mword allocated_heap;

/*Object was pinned during the current collection*/
static mword objects_pinned;

void
mono_sgen_release_space (mword size, int space)
{
	allocated_heap -= size;
}

static size_t
available_free_space (void)
{
	return max_heap_size - MIN (allocated_heap, max_heap_size);
}

gboolean
mono_sgen_try_alloc_space (mword size, int space)
{
	if (available_free_space () < size)
		return FALSE;

	allocated_heap += size;
	mono_runtime_resource_check_limit (MONO_RESOURCE_GC_HEAP, allocated_heap);
	return TRUE;
}

static void
init_heap_size_limits (glong max_heap, glong soft_limit)
{
	if (soft_limit)
		soft_heap_limit = soft_limit;

	if (max_heap == 0)
		return;

	if (max_heap < soft_limit) {
		fprintf (stderr, "max-heap-size must be at least as large as soft-heap-limit.\n");
		exit (1);
	}

	if (max_heap < nursery_size * 4) {
		fprintf (stderr, "max-heap-size must be at least 4 times larger than nursery size.\n");
		exit (1);
	}
	max_heap_size = max_heap - nursery_size;
}

/*
 * ######################################################################
 * ########  Macros and function declarations.
 * ######################################################################
 */

inline static void*
align_pointer (void *ptr)
{
	mword p = (mword)ptr;
	p += sizeof (gpointer) - 1;
	p &= ~ (sizeof (gpointer) - 1);
	return (void*)p;
}

typedef SgenGrayQueue GrayQueue;

/* forward declarations */
static int stop_world (int generation);
static int restart_world (int generation);
static void scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise, GrayQueue *queue);
static void scan_from_registered_roots (CopyOrMarkObjectFunc copy_func, char *addr_start, char *addr_end, int root_type, GrayQueue *queue);
static void scan_finalizer_entries (CopyOrMarkObjectFunc copy_func, FinalizeReadyEntry *list, GrayQueue *queue);
static void report_finalizer_roots (void);
static void report_registered_roots (void);
static void find_pinning_ref_from_thread (char *obj, size_t size);
static void update_current_thread_stack (void *start);
static void collect_bridge_objects (CopyOrMarkObjectFunc copy_func, char *start, char *end, int generation, GrayQueue *queue);
static void finalize_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, int generation, GrayQueue *queue);
static void process_fin_stage_entries (void);
static void null_link_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, int generation, gboolean before_finalization, GrayQueue *queue);
static void null_links_for_domain (MonoDomain *domain, int generation);
static void process_dislink_stage_entries (void);

static void pin_from_roots (void *start_nursery, void *end_nursery, GrayQueue *queue);
static int pin_objects_from_addresses (GCMemSection *section, void **start, void **end, void *start_nursery, void *end_nursery, GrayQueue *queue);
static void finish_gray_stack (char *start_addr, char *end_addr, int generation, GrayQueue *queue);
static gboolean need_major_collection (mword space_needed);
static void major_collection (const char *reason);

static void mono_gc_register_disappearing_link (MonoObject *obj, void **link, gboolean track, gboolean in_gc);
static gboolean mono_gc_is_critical_method (MonoMethod *method);

void mono_gc_scan_for_specific_ref (MonoObject *key, gboolean precise);

static void init_stats (void);

static int mark_ephemerons_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, GrayQueue *queue);
static void clear_unreachable_ephemerons (CopyOrMarkObjectFunc copy_func, char *start, char *end, GrayQueue *queue);
static void null_ephemerons_for_domain (MonoDomain *domain);

SgenMajorCollector major_collector;
static GrayQueue gray_queue;

static SgenRemeberedSet remset;


#define WORKERS_DISTRIBUTE_GRAY_QUEUE (mono_sgen_collection_is_parallel () ? mono_sgen_workers_get_distribute_gray_queue () : &gray_queue)

static SgenGrayQueue*
mono_sgen_workers_get_job_gray_queue (WorkerData *worker_data)
{
	return worker_data ? &worker_data->private_gray_queue : WORKERS_DISTRIBUTE_GRAY_QUEUE;
}

static gboolean
is_xdomain_ref_allowed (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	int offset = (char*)(ptr) - (char*)o;

	if (o->vtable->klass == mono_defaults.thread_class && offset == G_STRUCT_OFFSET (MonoThread, internal_thread))
		return TRUE;
	if (o->vtable->klass == mono_defaults.internal_thread_class && offset == G_STRUCT_OFFSET (MonoInternalThread, current_appcontext))
		return TRUE;
	if (mono_class_has_parent_fast (o->vtable->klass, mono_defaults.real_proxy_class) &&
			offset == G_STRUCT_OFFSET (MonoRealProxy, unwrapped_server))
		return TRUE;
	/* Thread.cached_culture_info */
	if (!strcmp (ref->vtable->klass->name_space, "System.Globalization") &&
			!strcmp (ref->vtable->klass->name, "CultureInfo") &&
			!strcmp(o->vtable->klass->name_space, "System") &&
			!strcmp(o->vtable->klass->name, "Object[]"))
		return TRUE;
	/*
	 *  at System.IO.MemoryStream.InternalConstructor (byte[],int,int,bool,bool) [0x0004d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:121
	 * at System.IO.MemoryStream..ctor (byte[]) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:81
	 * at (wrapper remoting-invoke-with-check) System.IO.MemoryStream..ctor (byte[]) <IL 0x00020, 0xffffffff>
	 * at System.Runtime.Remoting.Messaging.CADMethodCallMessage.GetArguments () [0x0000d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/CADMessages.cs:327
	 * at System.Runtime.Remoting.Messaging.MethodCall..ctor (System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/MethodCall.cs:87
	 * at System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) [0x00018] in /home/schani/Work/novell/trunk/mcs/class/corlib/System/AppDomain.cs:1213
	 * at (wrapper remoting-invoke-with-check) System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) <IL 0x0003d, 0xffffffff>
	 * at System.Runtime.Remoting.Channels.CrossAppDomainSink.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00008] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Channels/CrossAppDomainChannel.cs:198
	 * at (wrapper runtime-invoke) object.runtime_invoke_CrossAppDomainSink/ProcessMessageRes_object_object (object,intptr,intptr,intptr) <IL 0x0004c, 0xffffffff>
	 */
	if (!strcmp (ref->vtable->klass->name_space, "System") &&
			!strcmp (ref->vtable->klass->name, "Byte[]") &&
			!strcmp (o->vtable->klass->name_space, "System.IO") &&
			!strcmp (o->vtable->klass->name, "MemoryStream"))
		return TRUE;
	/* append_job() in threadpool.c */
	if (!strcmp (ref->vtable->klass->name_space, "System.Runtime.Remoting.Messaging") &&
			!strcmp (ref->vtable->klass->name, "AsyncResult") &&
			!strcmp (o->vtable->klass->name_space, "System") &&
			!strcmp (o->vtable->klass->name, "Object[]") &&
			mono_thread_pool_is_queue_array ((MonoArray*) o))
		return TRUE;
	return FALSE;
}

static void
check_reference_for_xdomain (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	int offset = (char*)(ptr) - (char*)o;
	MonoClass *class;
	MonoClassField *field;
	char *str;

	if (!ref || ref->vtable->domain == domain)
		return;
	if (is_xdomain_ref_allowed (ptr, obj, domain))
		return;

	field = NULL;
	for (class = o->vtable->klass; class; class = class->parent) {
		int i;

		for (i = 0; i < class->field.count; ++i) {
			if (class->fields[i].offset == offset) {
				field = &class->fields[i];
				break;
			}
		}
		if (field)
			break;
	}

	if (ref->vtable->klass == mono_defaults.string_class)
		str = mono_string_to_utf8 ((MonoString*)ref);
	else
		str = NULL;
	g_print ("xdomain reference in %p (%s.%s) at offset %d (%s) to %p (%s.%s) (%s)  -  pointed to by:\n",
			o, o->vtable->klass->name_space, o->vtable->klass->name,
			offset, field ? field->name : "",
			ref, ref->vtable->klass->name_space, ref->vtable->klass->name, str ? str : "");
	mono_gc_scan_for_specific_ref (o, TRUE);
	if (str)
		g_free (str);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	check_reference_for_xdomain ((ptr), (obj), domain)

static void
scan_object_for_xdomain_refs (char *start, mword size, void *data)
{
	MonoDomain *domain = ((MonoObject*)start)->vtable->domain;

	#include "sgen-scan-object.h"
}

static gboolean scan_object_for_specific_ref_precise = TRUE;

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj) do {		\
	if ((MonoObject*)*(ptr) == key) {	\
	g_print ("found ref to %p in object %p (%s) at offset %td\n",	\
			key, (obj), safe_name ((obj)), ((char*)(ptr) - (char*)(obj))); \
	}								\
	} while (0)

static void
scan_object_for_specific_ref (char *start, MonoObject *key)
{
	char *forwarded;

	if ((forwarded = SGEN_OBJECT_IS_FORWARDED (start)))
		start = forwarded;

	if (scan_object_for_specific_ref_precise) {
		#include "sgen-scan-object.h"
	} else {
		mword *words = (mword*)start;
		size_t size = safe_object_get_size ((MonoObject*)start);
		int i;
		for (i = 0; i < size / sizeof (mword); ++i) {
			if (words [i] == (mword)key) {
				g_print ("found possible ref to %p in object %p (%s) at offset %td\n",
						key, start, safe_name (start), i * sizeof (mword));
			}
		}
	}
}

void
mono_sgen_scan_area_with_callback (char *start, char *end, IterateObjectCallbackFunc callback, void *data, gboolean allow_flags)
{
	while (start < end) {
		size_t size;
		char *obj;

		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		if (allow_flags) {
			if (!(obj = SGEN_OBJECT_IS_FORWARDED (start)))
				obj = start;
		} else {
			obj = start;
		}

		size = ALIGN_UP (safe_object_get_size ((MonoObject*)obj));

		callback (obj, size, data);

		start += size;
	}
}

static void
scan_object_for_specific_ref_callback (char *obj, size_t size, MonoObject *key)
{
	scan_object_for_specific_ref (obj, key);
}

static void
check_root_obj_specific_ref (RootRecord *root, MonoObject *key, MonoObject *obj)
{
	if (key != obj)
		return;
	g_print ("found ref to %p in root record %p\n", key, root);
}

static MonoObject *check_key = NULL;
static RootRecord *check_root = NULL;

static void
check_root_obj_specific_ref_from_marker (void **obj)
{
	check_root_obj_specific_ref (check_root, check_key, *obj);
}

static void
scan_roots_for_specific_ref (MonoObject *key, int root_type)
{
	void **start_root;
	RootRecord *root;
	check_key = key;

	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		mword desc = root->root_desc;

		check_root = root;

		switch (desc & ROOT_DESC_TYPE_MASK) {
		case ROOT_DESC_BITMAP:
			desc >>= ROOT_DESC_TYPE_SHIFT;
			while (desc) {
				if (desc & 1)
					check_root_obj_specific_ref (root, key, *start_root);
				desc >>= 1;
				start_root++;
			}
			return;
		case ROOT_DESC_COMPLEX: {
			gsize *bitmap_data = mono_sgen_get_complex_descriptor_bitmap (desc);
			int bwords = (*bitmap_data) - 1;
			void **start_run = start_root;
			bitmap_data++;
			while (bwords-- > 0) {
				gsize bmap = *bitmap_data++;
				void **objptr = start_run;
				while (bmap) {
					if (bmap & 1)
						check_root_obj_specific_ref (root, key, *objptr);
					bmap >>= 1;
					++objptr;
				}
				start_run += GC_BITS_PER_WORD;
			}
			break;
		}
		case ROOT_DESC_USER: {
			MonoGCRootMarkFunc marker = mono_sgen_get_user_descriptor_func (desc);
			marker (start_root, check_root_obj_specific_ref_from_marker);
			break;
		}
		case ROOT_DESC_RUN_LEN:
			g_assert_not_reached ();
		default:
			g_assert_not_reached ();
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	check_key = NULL;
	check_root = NULL;
}

void
mono_gc_scan_for_specific_ref (MonoObject *key, gboolean precise)
{
	void **ptr;
	RootRecord *root;

	scan_object_for_specific_ref_precise = precise;

	mono_sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key, TRUE);

	major_collector.iterate_objects (TRUE, TRUE, (IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	mono_sgen_los_iterate_objects ((IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	scan_roots_for_specific_ref (key, ROOT_TYPE_NORMAL);
	scan_roots_for_specific_ref (key, ROOT_TYPE_WBARRIER);

	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_PINNED], ptr, root) {
		while (ptr < (void**)root->end_root) {
			check_root_obj_specific_ref (root, *ptr, key);
			++ptr;
		}
	} SGEN_HASH_TABLE_FOREACH_END;
}

static gboolean
need_remove_object_for_domain (char *start, MonoDomain *domain)
{
	if (mono_object_domain (start) == domain) {
		DEBUG (4, fprintf (gc_debug_file, "Need to cleanup object %p\n", start));
		binary_protocol_cleanup (start, (gpointer)LOAD_VTABLE (start), safe_object_get_size ((MonoObject*)start));
		return TRUE;
	}
	return FALSE;
}

static void
process_object_for_domain_clearing (char *start, MonoDomain *domain)
{
	GCVTable *vt = (GCVTable*)LOAD_VTABLE (start);
	if (vt->klass == mono_defaults.internal_thread_class)
		g_assert (mono_object_domain (start) == mono_get_root_domain ());
	/* The object could be a proxy for an object in the domain
	   we're deleting. */
	if (mono_class_has_parent_fast (vt->klass, mono_defaults.real_proxy_class)) {
		MonoObject *server = ((MonoRealProxy*)start)->unwrapped_server;

		/* The server could already have been zeroed out, so
		   we need to check for that, too. */
		if (server && (!LOAD_VTABLE (server) || mono_object_domain (server) == domain)) {
			DEBUG (4, fprintf (gc_debug_file, "Cleaning up remote pointer in %p to object %p\n",
					start, server));
			((MonoRealProxy*)start)->unwrapped_server = NULL;
		}
	}
}

static MonoDomain *check_domain = NULL;

static void
check_obj_not_in_domain (void **o)
{
	g_assert (((MonoObject*)(*o))->vtable->domain != check_domain);
}

static void
scan_for_registered_roots_in_domain (MonoDomain *domain, int root_type)
{
	void **start_root;
	RootRecord *root;
	check_domain = domain;
	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		mword desc = root->root_desc;

		/* The MonoDomain struct is allowed to hold
		   references to objects in its own domain. */
		if (start_root == (void**)domain)
			continue;

		switch (desc & ROOT_DESC_TYPE_MASK) {
		case ROOT_DESC_BITMAP:
			desc >>= ROOT_DESC_TYPE_SHIFT;
			while (desc) {
				if ((desc & 1) && *start_root)
					check_obj_not_in_domain (*start_root);
				desc >>= 1;
				start_root++;
			}
			break;
		case ROOT_DESC_COMPLEX: {
			gsize *bitmap_data = mono_sgen_get_complex_descriptor_bitmap (desc);
			int bwords = (*bitmap_data) - 1;
			void **start_run = start_root;
			bitmap_data++;
			while (bwords-- > 0) {
				gsize bmap = *bitmap_data++;
				void **objptr = start_run;
				while (bmap) {
					if ((bmap & 1) && *objptr)
						check_obj_not_in_domain (*objptr);
					bmap >>= 1;
					++objptr;
				}
				start_run += GC_BITS_PER_WORD;
			}
			break;
		}
		case ROOT_DESC_USER: {
			MonoGCRootMarkFunc marker = mono_sgen_get_user_descriptor_func (desc);
			marker (start_root, check_obj_not_in_domain);
			break;
		}
		case ROOT_DESC_RUN_LEN:
			g_assert_not_reached ();
		default:
			g_assert_not_reached ();
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	check_domain = NULL;
}

static void
check_for_xdomain_refs (void)
{
	LOSObject *bigobj;

	mono_sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_object_for_xdomain_refs, NULL, FALSE);

	major_collector.iterate_objects (TRUE, TRUE, (IterateObjectCallbackFunc)scan_object_for_xdomain_refs, NULL);

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		scan_object_for_xdomain_refs (bigobj->data, bigobj->size, NULL);
}

static gboolean
clear_domain_process_object (char *obj, MonoDomain *domain)
{
	gboolean remove;

	process_object_for_domain_clearing (obj, domain);
	remove = need_remove_object_for_domain (obj, domain);

	if (remove && ((MonoObject*)obj)->synchronisation) {
		void **dislink = mono_monitor_get_object_monitor_weak_link ((MonoObject*)obj);
		if (dislink)
			mono_gc_register_disappearing_link (NULL, dislink, FALSE, TRUE);
	}

	return remove;
}

static void
clear_domain_process_minor_object_callback (char *obj, size_t size, MonoDomain *domain)
{
	if (clear_domain_process_object (obj, domain))
		memset (obj, 0, size);
}

static void
clear_domain_process_major_object_callback (char *obj, size_t size, MonoDomain *domain)
{
	clear_domain_process_object (obj, domain);
}

static void
clear_domain_free_major_non_pinned_object_callback (char *obj, size_t size, MonoDomain *domain)
{
	if (need_remove_object_for_domain (obj, domain))
		major_collector.free_non_pinned_object (obj, size);
}

static void
clear_domain_free_major_pinned_object_callback (char *obj, size_t size, MonoDomain *domain)
{
	if (need_remove_object_for_domain (obj, domain))
		major_collector.free_pinned_object (obj, size);
}

/*
 * When appdomains are unloaded we can easily remove objects that have finalizers,
 * but all the others could still be present in random places on the heap.
 * We need a sweep to get rid of them even though it's going to be costly
 * with big heaps.
 * The reason we need to remove them is because we access the vtable and class
 * structures to know the object size and the reference bitmap: once the domain is
 * unloaded the point to random memory.
 */
void
mono_gc_clear_domain (MonoDomain * domain)
{
	LOSObject *bigobj, *prev;
	int i;

	LOCK_GC;

	process_fin_stage_entries ();
	process_dislink_stage_entries ();

	mono_sgen_clear_nursery_fragments ();

	if (xdomain_checks && domain != mono_get_root_domain ()) {
		scan_for_registered_roots_in_domain (domain, ROOT_TYPE_NORMAL);
		scan_for_registered_roots_in_domain (domain, ROOT_TYPE_WBARRIER);
		check_for_xdomain_refs ();
	}

	mono_sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)clear_domain_process_minor_object_callback, domain, FALSE);

	/*Ephemerons and dislinks must be processed before LOS since they might end up pointing
	to memory returned to the OS.*/
	null_ephemerons_for_domain (domain);

	for (i = GENERATION_NURSERY; i < GENERATION_MAX; ++i)
		null_links_for_domain (domain, i);

	/* We need two passes over major and large objects because
	   freeing such objects might give their memory back to the OS
	   (in the case of large objects) or obliterate its vtable
	   (pinned objects with major-copying or pinned and non-pinned
	   objects with major-mark&sweep), but we might need to
	   dereference a pointer from an object to another object if
	   the first object is a proxy. */
	major_collector.iterate_objects (TRUE, TRUE, (IterateObjectCallbackFunc)clear_domain_process_major_object_callback, domain);
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		clear_domain_process_object (bigobj->data, domain);

	prev = NULL;
	for (bigobj = los_object_list; bigobj;) {
		if (need_remove_object_for_domain (bigobj->data, domain)) {
			LOSObject *to_free = bigobj;
			if (prev)
				prev->next = bigobj->next;
			else
				los_object_list = bigobj->next;
			bigobj = bigobj->next;
			DEBUG (4, fprintf (gc_debug_file, "Freeing large object %p\n",
					bigobj->data));
			mono_sgen_los_free_object (to_free);
			continue;
		}
		prev = bigobj;
		bigobj = bigobj->next;
	}
	major_collector.iterate_objects (TRUE, FALSE, (IterateObjectCallbackFunc)clear_domain_free_major_non_pinned_object_callback, domain);
	major_collector.iterate_objects (FALSE, TRUE, (IterateObjectCallbackFunc)clear_domain_free_major_pinned_object_callback, domain);

	if (G_UNLIKELY (do_pin_stats)) {
		if (domain == mono_get_root_domain ())
			mono_sgen_pin_stats_print_class_stats ();
	}

	UNLOCK_GC;
}

/*
 * mono_sgen_add_to_global_remset:
 *
 *   The global remset contains locations which point into newspace after
 * a minor collection. This can happen if the objects they point to are pinned.
 *
 * LOCKING: If called from a parallel collector, the global remset
 * lock must be held.  For serial collectors that is not necessary.
 */
void
mono_sgen_add_to_global_remset (gpointer ptr)
{
	remset.record_pointer (ptr);
}

/*
 * mono_sgen_drain_gray_stack:
 *
 *   Scan objects in the gray stack until the stack is empty. This should be called
 * frequently after each object is copied, to achieve better locality and cache
 * usage.
 */
gboolean
mono_sgen_drain_gray_stack (GrayQueue *queue, int max_objs)
{
	char *obj;

	if (current_collection_generation == GENERATION_NURSERY) {
		ScanObjectFunc scan_func = mono_sgen_get_minor_scan_object ();

		for (;;) {
			GRAY_OBJECT_DEQUEUE (queue, obj);
			if (!obj)
				return TRUE;
			DEBUG (9, fprintf (gc_debug_file, "Precise gray object scan %p (%s)\n", obj, safe_name (obj)));
			scan_func (obj, queue);
		}
	} else {
		int i;

		if (mono_sgen_collection_is_parallel () && mono_sgen_workers_is_distributed_queue (queue))
			return TRUE;

		do {
			for (i = 0; i != max_objs; ++i) {
				GRAY_OBJECT_DEQUEUE (queue, obj);
				if (!obj)
					return TRUE;
				DEBUG (9, fprintf (gc_debug_file, "Precise gray object scan %p (%s)\n", obj, safe_name (obj)));
				major_collector.major_scan_object (obj, queue);
			}
		} while (max_objs < 0);
		return FALSE;
	}
}

/*
 * Addresses from start to end are already sorted. This function finds
 * the object header for each address and pins the object. The
 * addresses must be inside the passed section.  The (start of the)
 * address array is overwritten with the addresses of the actually
 * pinned objects.  Return the number of pinned objects.
 */
static int
pin_objects_from_addresses (GCMemSection *section, void **start, void **end, void *start_nursery, void *end_nursery, GrayQueue *queue)
{
	void *last = NULL;
	int count = 0;
	void *search_start;
	void *last_obj = NULL;
	size_t last_obj_size = 0;
	void *addr;
	int idx;
	void **definitely_pinned = start;

	mono_sgen_nursery_allocator_prepare_for_pinning ();

	while (start < end) {
		addr = *start;
		/* the range check should be reduntant */
		if (addr != last && addr >= start_nursery && addr < end_nursery) {
			DEBUG (5, fprintf (gc_debug_file, "Considering pinning addr %p\n", addr));
			/* multiple pointers to the same object */
			if (addr >= last_obj && (char*)addr < (char*)last_obj + last_obj_size) {
				start++;
				continue;
			}
			idx = ((char*)addr - (char*)section->data) / SCAN_START_SIZE;
			g_assert (idx < section->num_scan_start);
			search_start = (void*)section->scan_starts [idx];
			if (!search_start || search_start > addr) {
				while (idx) {
					--idx;
					search_start = section->scan_starts [idx];
					if (search_start && search_start <= addr)
						break;
				}
				if (!search_start || search_start > addr)
					search_start = start_nursery;
			}
			if (search_start < last_obj)
				search_start = (char*)last_obj + last_obj_size;
			/* now addr should be in an object a short distance from search_start
			 * Note that search_start must point to zeroed mem or point to an object.
			 */

			do {
				if (!*(void**)search_start) {
					/* Consistency check */
					/*
					for (frag = nursery_fragments; frag; frag = frag->next) {
						if (search_start >= frag->fragment_start && search_start < frag->fragment_end)
							g_assert_not_reached ();
					}
					*/

					search_start = (void*)ALIGN_UP ((mword)search_start + sizeof (gpointer));
					continue;
				}
				last_obj = search_start;
				last_obj_size = ALIGN_UP (safe_object_get_size ((MonoObject*)search_start));

				if (((MonoObject*)last_obj)->synchronisation == GINT_TO_POINTER (-1)) {
					/* Marks the beginning of a nursery fragment, skip */
				} else {
					DEBUG (8, fprintf (gc_debug_file, "Pinned try match %p (%s), size %zd\n", last_obj, safe_name (last_obj), last_obj_size));
					if (addr >= search_start && (char*)addr < (char*)last_obj + last_obj_size) {
						DEBUG (4, fprintf (gc_debug_file, "Pinned object %p, vtable %p (%s), count %d\n", search_start, *(void**)search_start, safe_name (search_start), count));
						binary_protocol_pin (search_start, (gpointer)LOAD_VTABLE (search_start), safe_object_get_size (search_start));
						pin_object (search_start);
						GRAY_OBJECT_ENQUEUE (queue, search_start);
						if (G_UNLIKELY (do_pin_stats))
							mono_sgen_pin_stats_register_object (search_start, last_obj_size);
						definitely_pinned [count] = search_start;
						count++;
						break;
					}
				}
				/* skip to the next object */
				search_start = (void*)((char*)search_start + last_obj_size);
			} while (search_start <= addr);
			/* we either pinned the correct object or we ignored the addr because
			 * it points to unused zeroed memory.
			 */
			last = addr;
		}
		start++;
	}
	//printf ("effective pinned: %d (at the end: %d)\n", count, (char*)end_nursery - (char*)last);
	if (mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS) {
		GCRootReport report;
		report.count = 0;
		for (idx = 0; idx < count; ++idx)
			add_profile_gc_root (&report, definitely_pinned [idx], MONO_PROFILE_GC_ROOT_PINNING | MONO_PROFILE_GC_ROOT_MISC, 0);
		notify_gc_roots (&report);
	}
	stat_pinned_objects += count;
	return count;
}

void
mono_sgen_pin_objects_in_section (GCMemSection *section, GrayQueue *queue)
{
	int num_entries = section->pin_queue_num_entries;
	if (num_entries) {
		void **start = section->pin_queue_start;
		int reduced_to;
		reduced_to = pin_objects_from_addresses (section, start, start + num_entries,
				section->data, section->next_data, queue);
		section->pin_queue_num_entries = reduced_to;
		if (!reduced_to)
			section->pin_queue_start = NULL;
	}
}


void
mono_sgen_pin_object (void *object, GrayQueue *queue)
{
	if (mono_sgen_collection_is_parallel ()) {
		LOCK_PIN_QUEUE;
		/*object arrives pinned*/
		mono_sgen_pin_stage_ptr (object);
		++objects_pinned ;
		UNLOCK_PIN_QUEUE;
	} else {
		SGEN_PIN_OBJECT (object);
		mono_sgen_pin_stage_ptr (object);
		++objects_pinned;
		if (G_UNLIKELY (do_pin_stats))
			mono_sgen_pin_stats_register_object (object, safe_object_get_size (object));
	}
	GRAY_OBJECT_ENQUEUE (queue, object);
	binary_protocol_pin (object, (gpointer)LOAD_VTABLE (object), safe_object_get_size (object));
}

/* Sort the addresses in array in increasing order.
 * Done using a by-the book heap sort. Which has decent and stable performance, is pretty cache efficient.
 */
void
mono_sgen_sort_addresses (void **array, int size)
{
	int i;
	void *tmp;

	for (i = 1; i < size; ++i) {
		int child = i;
		while (child > 0) {
			int parent = (child - 1) / 2;

			if (array [parent] >= array [child])
				break;

			tmp = array [parent];
			array [parent] = array [child];
			array [child] = tmp;

			child = parent;
		}
	}

	for (i = size - 1; i > 0; --i) {
		int end, root;
		tmp = array [i];
		array [i] = array [0];
		array [0] = tmp;

		end = i - 1;
		root = 0;

		while (root * 2 + 1 <= end) {
			int child = root * 2 + 1;

			if (child < end && array [child] < array [child + 1])
				++child;
			if (array [root] >= array [child])
				break;

			tmp = array [root];
			array [root] = array [child];
			array [child] = tmp;

			root = child;
		}
	}
}

/* 
 * Scan the memory between start and end and queue values which could be pointers
 * to the area between start_nursery and end_nursery for later consideration.
 * Typically used for thread stacks.
 */
static void
conservatively_pin_objects_from (void **start, void **end, void *start_nursery, void *end_nursery, int pin_type)
{
	int count = 0;
	while (start < end) {
		if (*start >= start_nursery && *start < end_nursery) {
			/*
			 * *start can point to the middle of an object
			 * note: should we handle pointing at the end of an object?
			 * pinning in C# code disallows pointing at the end of an object
			 * but there is some small chance that an optimizing C compiler
			 * may keep the only reference to an object by pointing
			 * at the end of it. We ignore this small chance for now.
			 * Pointers to the end of an object are indistinguishable
			 * from pointers to the start of the next object in memory
			 * so if we allow that we'd need to pin two objects...
			 * We queue the pointer in an array, the
			 * array will then be sorted and uniqued. This way
			 * we can coalesce several pinning pointers and it should
			 * be faster since we'd do a memory scan with increasing
			 * addresses. Note: we can align the address to the allocation
			 * alignment, so the unique process is more effective.
			 */
			mword addr = (mword)*start;
			addr &= ~(ALLOC_ALIGN - 1);
			if (addr >= (mword)start_nursery && addr < (mword)end_nursery)
				mono_sgen_pin_stage_ptr ((void*)addr);
			if (G_UNLIKELY (do_pin_stats)) { 
				if (ptr_in_nursery ((void*)addr))
					mono_sgen_pin_stats_register_address ((char*)addr, pin_type);
			}
			DEBUG (6, if (count) fprintf (gc_debug_file, "Pinning address %p from %p\n", (void*)addr, start));
			count++;
		}
		start++;
	}
	DEBUG (7, if (count) fprintf (gc_debug_file, "found %d potential pinned heap pointers\n", count));
}

/*
 * Debugging function: find in the conservative roots where @obj is being pinned.
 */
static G_GNUC_UNUSED void
find_pinning_reference (char *obj, size_t size)
{
	char **start;
	RootRecord *root;
	char *endobj = obj + size;

	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_NORMAL], start, root) {
		/* if desc is non-null it has precise info */
		if (!root->root_desc) {
			while (start < (char**)root->end_root) {
				if (*start >= obj && *start < endobj) {
					DEBUG (0, fprintf (gc_debug_file, "Object %p referenced in pinned roots %p-%p\n", obj, start, root->end_root));
				}
				start++;
			}
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	find_pinning_ref_from_thread (obj, size);
}

/*
 * The first thing we do in a collection is to identify pinned objects.
 * This function considers all the areas of memory that need to be
 * conservatively scanned.
 */
static void
pin_from_roots (void *start_nursery, void *end_nursery, GrayQueue *queue)
{
	void **start_root;
	RootRecord *root;
	DEBUG (2, fprintf (gc_debug_file, "Scanning pinned roots (%d bytes, %d/%d entries)\n", (int)roots_size, roots_hash [ROOT_TYPE_NORMAL].num_entries, roots_hash [ROOT_TYPE_PINNED].num_entries));
	/* objects pinned from the API are inside these roots */
	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_PINNED], start_root, root) {
		DEBUG (6, fprintf (gc_debug_file, "Pinned roots %p-%p\n", start_root, root->end_root));
		conservatively_pin_objects_from (start_root, (void**)root->end_root, start_nursery, end_nursery, PIN_TYPE_OTHER);
	} SGEN_HASH_TABLE_FOREACH_END;
	/* now deal with the thread stacks
	 * in the future we should be able to conservatively scan only:
	 * *) the cpu registers
	 * *) the unmanaged stack frames
	 * *) the _last_ managed stack frame
	 * *) pointers slots in managed frames
	 */
	scan_thread_data (start_nursery, end_nursery, FALSE, queue);
}

typedef struct {
	CopyOrMarkObjectFunc func;
	GrayQueue *queue;
} UserCopyOrMarkData;

static MonoNativeTlsKey user_copy_or_mark_key;

static void
init_user_copy_or_mark_key (void)
{
	mono_native_tls_alloc (&user_copy_or_mark_key, NULL);
}

static void
set_user_copy_or_mark_data (UserCopyOrMarkData *data)
{
	mono_native_tls_set_value (user_copy_or_mark_key, data);
}

static void
single_arg_user_copy_or_mark (void **obj)
{
	UserCopyOrMarkData *data = mono_native_tls_get_value (user_copy_or_mark_key);

	data->func (obj, data->queue);
}

/*
 * The memory area from start_root to end_root contains pointers to objects.
 * Their position is precisely described by @desc (this means that the pointer
 * can be either NULL or the pointer to the start of an object).
 * This functions copies them to to_space updates them.
 *
 * This function is not thread-safe!
 */
static void
precisely_scan_objects_from (CopyOrMarkObjectFunc copy_func, void** start_root, void** end_root, char* n_start, char *n_end, mword desc, GrayQueue *queue)
{
	switch (desc & ROOT_DESC_TYPE_MASK) {
	case ROOT_DESC_BITMAP:
		desc >>= ROOT_DESC_TYPE_SHIFT;
		while (desc) {
			if ((desc & 1) && *start_root) {
				copy_func (start_root, queue);
				DEBUG (9, fprintf (gc_debug_file, "Overwrote root at %p with %p\n", start_root, *start_root));
				mono_sgen_drain_gray_stack (queue, -1);
			}
			desc >>= 1;
			start_root++;
		}
		return;
	case ROOT_DESC_COMPLEX: {
		gsize *bitmap_data = mono_sgen_get_complex_descriptor_bitmap (desc);
		int bwords = (*bitmap_data) - 1;
		void **start_run = start_root;
		bitmap_data++;
		while (bwords-- > 0) {
			gsize bmap = *bitmap_data++;
			void **objptr = start_run;
			while (bmap) {
				if ((bmap & 1) && *objptr) {
					copy_func (objptr, queue);
					DEBUG (9, fprintf (gc_debug_file, "Overwrote root at %p with %p\n", objptr, *objptr));
					mono_sgen_drain_gray_stack (queue, -1);
				}
				bmap >>= 1;
				++objptr;
			}
			start_run += GC_BITS_PER_WORD;
		}
		break;
	}
	case ROOT_DESC_USER: {
		UserCopyOrMarkData data = { copy_func, queue };
		MonoGCRootMarkFunc marker = mono_sgen_get_user_descriptor_func (desc);
		set_user_copy_or_mark_data (&data);
		marker (start_root, single_arg_user_copy_or_mark);
		set_user_copy_or_mark_data (NULL);
		break;
	}
	case ROOT_DESC_RUN_LEN:
		g_assert_not_reached ();
	default:
		g_assert_not_reached ();
	}
}

static void
reset_heap_boundaries (void)
{
	lowest_heap_address = ~(mword)0;
	highest_heap_address = 0;
}

void
mono_sgen_update_heap_boundaries (mword low, mword high)
{
	mword old;

	do {
		old = lowest_heap_address;
		if (low >= old)
			break;
	} while (SGEN_CAS_PTR ((gpointer*)&lowest_heap_address, (gpointer)low, (gpointer)old) != (gpointer)old);

	do {
		old = highest_heap_address;
		if (high <= old)
			break;
	} while (SGEN_CAS_PTR ((gpointer*)&highest_heap_address, (gpointer)high, (gpointer)old) != (gpointer)old);
}

static unsigned long
prot_flags_for_activate (int activate)
{
	unsigned long prot_flags = activate? MONO_MMAP_READ|MONO_MMAP_WRITE: MONO_MMAP_NONE;
	return prot_flags | MONO_MMAP_PRIVATE | MONO_MMAP_ANON;
}

/*
 * Allocate a big chunk of memory from the OS (usually 64KB to several megabytes).
 * This must not require any lock.
 */
void*
mono_sgen_alloc_os_memory (size_t size, int activate)
{
	void *ptr = mono_valloc (0, size, prot_flags_for_activate (activate));
	if (ptr) {
		/* FIXME: CAS */
		total_alloc += size;
	}
	return ptr;
}

/* size must be a power of 2 */
void*
mono_sgen_alloc_os_memory_aligned (mword size, mword alignment, gboolean activate)
{
	void *ptr = mono_valloc_aligned (size, alignment, prot_flags_for_activate (activate));
	if (ptr) {
		/* FIXME: CAS */
		total_alloc += size;
	}
	return ptr;
}

/*
 * Free the memory returned by mono_sgen_alloc_os_memory (), returning it to the OS.
 */
void
mono_sgen_free_os_memory (void *addr, size_t size)
{
	mono_vfree (addr, size);
	/* FIXME: CAS */
	total_alloc -= size;
}

/*
 * Allocate and setup the data structures needed to be able to allocate objects
 * in the nursery. The nursery is stored in nursery_section.
 */
static void
alloc_nursery (void)
{
	GCMemSection *section;
	char *data;
	int scan_starts;
	int alloc_size;

	if (nursery_section)
		return;
	DEBUG (2, fprintf (gc_debug_file, "Allocating nursery size: %lu\n", (unsigned long)nursery_size));
	/* later we will alloc a larger area for the nursery but only activate
	 * what we need. The rest will be used as expansion if we have too many pinned
	 * objects in the existing nursery.
	 */
	/* FIXME: handle OOM */
	section = mono_sgen_alloc_internal (INTERNAL_MEM_SECTION);

	g_assert (nursery_size == DEFAULT_NURSERY_SIZE);
	alloc_size = nursery_size;
#ifdef SGEN_ALIGN_NURSERY
	data = major_collector.alloc_heap (alloc_size, alloc_size, DEFAULT_NURSERY_BITS);
#else
	data = major_collector.alloc_heap (alloc_size, 0, DEFAULT_NURSERY_BITS);
#endif
	mono_sgen_update_heap_boundaries ((mword)data, (mword)(data + nursery_size));
	DEBUG (4, fprintf (gc_debug_file, "Expanding nursery size (%p-%p): %lu, total: %lu\n", data, data + alloc_size, (unsigned long)nursery_size, (unsigned long)total_alloc));
	section->data = section->next_data = data;
	section->size = alloc_size;
	section->end_data = mono_sgen_get_nursery_end ();
	scan_starts = (alloc_size + SCAN_START_SIZE - 1) / SCAN_START_SIZE;
	section->scan_starts = mono_sgen_alloc_internal_dynamic (sizeof (char*) * scan_starts, INTERNAL_MEM_SCAN_STARTS);
	section->num_scan_start = scan_starts;
	section->block.role = MEMORY_ROLE_GEN0;
	section->block.next = NULL;

	nursery_section = section;

	mono_sgen_nursery_allocator_set_nursery_bounds (data, data + nursery_size);
}

void*
mono_gc_get_nursery (int *shift_bits, size_t *size)
{
	*size = nursery_size;
#ifdef SGEN_ALIGN_NURSERY
	*shift_bits = DEFAULT_NURSERY_BITS;
#else
	*shift_bits = -1;
#endif
	return mono_sgen_get_nursery_start ();
}

void
mono_gc_set_current_thread_appdomain (MonoDomain *domain)
{
	SgenThreadInfo *info = mono_thread_info_current ();

	/* Could be called from sgen_thread_unregister () with a NULL info */
	if (domain) {
		g_assert (info);
		info->stopped_domain = domain;
	}
}

gboolean
mono_gc_precise_stack_mark_enabled (void)
{
	return !conservative_stack_mark;
}

FILE *
mono_gc_get_logfile (void)
{
	return mono_sgen_get_logfile ();
}

static void
report_finalizer_roots_list (FinalizeReadyEntry *list)
{
	GCRootReport report;
	FinalizeReadyEntry *fin;

	report.count = 0;
	for (fin = list; fin; fin = fin->next) {
		if (!fin->object)
			continue;
		add_profile_gc_root (&report, fin->object, MONO_PROFILE_GC_ROOT_FINALIZER, 0);
	}
	notify_gc_roots (&report);
}

static void
report_finalizer_roots (void)
{
	report_finalizer_roots_list (fin_ready_list);
	report_finalizer_roots_list (critical_fin_list);
}

static GCRootReport *root_report;

static void
single_arg_report_root (void **obj)
{
	if (*obj)
		add_profile_gc_root (root_report, *obj, MONO_PROFILE_GC_ROOT_OTHER, 0);
}

static void
precisely_report_roots_from (GCRootReport *report, void** start_root, void** end_root, mword desc)
{
	switch (desc & ROOT_DESC_TYPE_MASK) {
	case ROOT_DESC_BITMAP:
		desc >>= ROOT_DESC_TYPE_SHIFT;
		while (desc) {
			if ((desc & 1) && *start_root) {
				add_profile_gc_root (report, *start_root, MONO_PROFILE_GC_ROOT_OTHER, 0);
			}
			desc >>= 1;
			start_root++;
		}
		return;
	case ROOT_DESC_COMPLEX: {
		gsize *bitmap_data = mono_sgen_get_complex_descriptor_bitmap (desc);
		int bwords = (*bitmap_data) - 1;
		void **start_run = start_root;
		bitmap_data++;
		while (bwords-- > 0) {
			gsize bmap = *bitmap_data++;
			void **objptr = start_run;
			while (bmap) {
				if ((bmap & 1) && *objptr) {
					add_profile_gc_root (report, *objptr, MONO_PROFILE_GC_ROOT_OTHER, 0);
				}
				bmap >>= 1;
				++objptr;
			}
			start_run += GC_BITS_PER_WORD;
		}
		break;
	}
	case ROOT_DESC_USER: {
		MonoGCRootMarkFunc marker = mono_sgen_get_user_descriptor_func (desc);
		root_report = report;
		marker (start_root, single_arg_report_root);
		break;
	}
	case ROOT_DESC_RUN_LEN:
		g_assert_not_reached ();
	default:
		g_assert_not_reached ();
	}
}

static void
report_registered_roots_by_type (int root_type)
{
	GCRootReport report;
	void **start_root;
	RootRecord *root;
	report.count = 0;
	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		DEBUG (6, fprintf (gc_debug_file, "Precise root scan %p-%p (desc: %p)\n", start_root, root->end_root, (void*)root->root_desc));
		precisely_report_roots_from (&report, start_root, (void**)root->end_root, root->root_desc);
	} SGEN_HASH_TABLE_FOREACH_END;
	notify_gc_roots (&report);
}

static void
report_registered_roots (void)
{
	report_registered_roots_by_type (ROOT_TYPE_NORMAL);
	report_registered_roots_by_type (ROOT_TYPE_WBARRIER);
}

static void
scan_finalizer_entries (CopyOrMarkObjectFunc copy_func, FinalizeReadyEntry *list, GrayQueue *queue)
{
	FinalizeReadyEntry *fin;

	for (fin = list; fin; fin = fin->next) {
		if (!fin->object)
			continue;
		DEBUG (5, fprintf (gc_debug_file, "Scan of fin ready object: %p (%s)\n", fin->object, safe_name (fin->object)));
		copy_func (&fin->object, queue);
	}
}

static const char*
generation_name (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return "nursery";
	case GENERATION_OLD: return "old";
	default: g_assert_not_reached ();
	}
}


static void
stw_bridge_process (void)
{
	mono_sgen_bridge_processing_stw_step ();
}

static void
bridge_process (void)
{
	mono_sgen_bridge_processing_finish ();
}

CopyOrMarkObjectFunc
mono_sgen_get_copy_object (void)
{
	if (current_collection_generation == GENERATION_NURSERY) {
		if (mono_sgen_collection_is_parallel ())
			return major_collector.copy_object;
		else
			return major_collector.nopar_copy_object;
	} else {
		return major_collector.copy_or_mark_object;
	}
}

ScanObjectFunc
mono_sgen_get_minor_scan_object (void)
{
	g_assert (current_collection_generation == GENERATION_NURSERY);

	if (mono_sgen_collection_is_parallel ())
		return major_collector.minor_scan_object;
	else
		return major_collector.nopar_minor_scan_object;
}

ScanVTypeFunc
mono_sgen_get_minor_scan_vtype (void)
{
	g_assert (current_collection_generation == GENERATION_NURSERY);

	if (mono_sgen_collection_is_parallel ())
		return major_collector.minor_scan_vtype;
	else
		return major_collector.nopar_minor_scan_vtype;
}

static void
finish_gray_stack (char *start_addr, char *end_addr, int generation, GrayQueue *queue)
{
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	int fin_ready;
	int done_with_ephemerons, ephemeron_rounds = 0;
	int num_loops;
	CopyOrMarkObjectFunc copy_func = mono_sgen_get_copy_object ();

	/*
	 * We copied all the reachable objects. Now it's the time to copy
	 * the objects that were not referenced by the roots, but by the copied objects.
	 * we built a stack of objects pointed to by gray_start: they are
	 * additional roots and we may add more items as we go.
	 * We loop until gray_start == gray_objects which means no more objects have
	 * been added. Note this is iterative: no recursion is involved.
	 * We need to walk the LO list as well in search of marked big objects
	 * (use a flag since this is needed only on major collections). We need to loop
	 * here as well, so keep a counter of marked LO (increasing it in copy_object).
	 *   To achieve better cache locality and cache usage, we drain the gray stack 
	 * frequently, after each object is copied, and just finish the work here.
	 */
	mono_sgen_drain_gray_stack (queue, -1);
	TV_GETTIME (atv);
	DEBUG (2, fprintf (gc_debug_file, "%s generation done\n", generation_name (generation)));

	/*
	Reset bridge data, we might have lingering data from a previous collection if this is a major
	collection trigged by minor overflow.

	We must reset the gathered bridges since their original block might be evacuated due to major
	fragmentation in the meanwhile and the bridge code should not have to deal with that.
	*/
	mono_sgen_bridge_reset_data ();

	/*
	 * Walk the ephemeron tables marking all values with reachable keys. This must be completely done
	 * before processing finalizable objects or non-tracking weak hamdle to avoid finalizing/clearing
	 * objects that are in fact reachable.
	 */
	done_with_ephemerons = 0;
	do {
		done_with_ephemerons = mark_ephemerons_in_range (copy_func, start_addr, end_addr, queue);
		mono_sgen_drain_gray_stack (queue, -1);
		++ephemeron_rounds;
	} while (!done_with_ephemerons);

	mono_sgen_scan_togglerefs (copy_func, start_addr, end_addr, queue);
	if (generation == GENERATION_OLD)
		mono_sgen_scan_togglerefs (copy_func, mono_sgen_get_nursery_start (), mono_sgen_get_nursery_end (), queue);

	if (mono_sgen_need_bridge_processing ()) {
		collect_bridge_objects (copy_func, start_addr, end_addr, generation, queue);
		if (generation == GENERATION_OLD)
			collect_bridge_objects (copy_func, mono_sgen_get_nursery_start (), mono_sgen_get_nursery_end (), GENERATION_NURSERY, queue);
		mono_sgen_drain_gray_stack (queue, -1);
	}

	/*
	We must clear weak links that don't track resurrection before processing object ready for
	finalization so they can be cleared before that.
	*/
	null_link_in_range (copy_func, start_addr, end_addr, generation, TRUE, queue);
	if (generation == GENERATION_OLD)
		null_link_in_range (copy_func, start_addr, end_addr, GENERATION_NURSERY, TRUE, queue);


	/* walk the finalization queue and move also the objects that need to be
	 * finalized: use the finalized objects as new roots so the objects they depend
	 * on are also not reclaimed. As with the roots above, only objects in the nursery
	 * are marked/copied.
	 * We need a loop here, since objects ready for finalizers may reference other objects
	 * that are fin-ready. Speedup with a flag?
	 */
	num_loops = 0;
	do {		
		fin_ready = num_ready_finalizers;
		finalize_in_range (copy_func, start_addr, end_addr, generation, queue);
		if (generation == GENERATION_OLD)
			finalize_in_range (copy_func, mono_sgen_get_nursery_start (), mono_sgen_get_nursery_end (), GENERATION_NURSERY, queue);

		if (fin_ready != num_ready_finalizers)
			++num_loops;

		/* drain the new stack that might have been created */
		DEBUG (6, fprintf (gc_debug_file, "Precise scan of gray area post fin\n"));
		mono_sgen_drain_gray_stack (queue, -1);
	} while (fin_ready != num_ready_finalizers);

	if (mono_sgen_need_bridge_processing ())
		g_assert (num_loops <= 1);

	/*
	 * This must be done again after processing finalizable objects since CWL slots are cleared only after the key is finalized.
	 */
	done_with_ephemerons = 0;
	do {
		done_with_ephemerons = mark_ephemerons_in_range (copy_func, start_addr, end_addr, queue);
		mono_sgen_drain_gray_stack (queue, -1);
		++ephemeron_rounds;
	} while (!done_with_ephemerons);

	/*
	 * Clear ephemeron pairs with unreachable keys.
	 * We pass the copy func so we can figure out if an array was promoted or not.
	 */
	clear_unreachable_ephemerons (copy_func, start_addr, end_addr, queue);

	TV_GETTIME (btv);
	DEBUG (2, fprintf (gc_debug_file, "Finalize queue handling scan for %s generation: %d usecs %d ephemeron roundss\n", generation_name (generation), TV_ELAPSED (atv, btv), ephemeron_rounds));

	/*
	 * handle disappearing links
	 * Note we do this after checking the finalization queue because if an object
	 * survives (at least long enough to be finalized) we don't clear the link.
	 * This also deals with a possible issue with the monitor reclamation: with the Boehm
	 * GC a finalized object my lose the monitor because it is cleared before the finalizer is
	 * called.
	 */
	g_assert (mono_sgen_gray_object_queue_is_empty (queue));
	for (;;) {
		null_link_in_range (copy_func, start_addr, end_addr, generation, FALSE, queue);
		if (generation == GENERATION_OLD)
			null_link_in_range (copy_func, start_addr, end_addr, GENERATION_NURSERY, FALSE, queue);
		if (mono_sgen_gray_object_queue_is_empty (queue))
			break;
		mono_sgen_drain_gray_stack (queue, -1);
	}

	g_assert (mono_sgen_gray_object_queue_is_empty (queue));
}

void
mono_sgen_check_section_scan_starts (GCMemSection *section)
{
	int i;
	for (i = 0; i < section->num_scan_start; ++i) {
		if (section->scan_starts [i]) {
			guint size = safe_object_get_size ((MonoObject*) section->scan_starts [i]);
			g_assert (size >= sizeof (MonoObject) && size <= MAX_SMALL_OBJ_SIZE);
		}
	}
}

static void
check_scan_starts (void)
{
	if (!do_scan_starts_check)
		return;
	mono_sgen_check_section_scan_starts (nursery_section);
	major_collector.check_scan_starts ();
}

static void
scan_from_registered_roots (CopyOrMarkObjectFunc copy_func, char *addr_start, char *addr_end, int root_type, GrayQueue *queue)
{
	void **start_root;
	RootRecord *root;
	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		DEBUG (6, fprintf (gc_debug_file, "Precise root scan %p-%p (desc: %p)\n", start_root, root->end_root, (void*)root->root_desc));
		precisely_scan_objects_from (copy_func, start_root, (void**)root->end_root, addr_start, addr_end, root->root_desc, queue);
	} SGEN_HASH_TABLE_FOREACH_END;
}

void
mono_sgen_dump_occupied (char *start, char *end, char *section_start)
{
	fprintf (heap_dump_file, "<occupied offset=\"%td\" size=\"%td\"/>\n", start - section_start, end - start);
}

void
mono_sgen_dump_section (GCMemSection *section, const char *type)
{
	char *start = section->data;
	char *end = section->data + section->size;
	char *occ_start = NULL;
	GCVTable *vt;
	char *old_start = NULL;	/* just for debugging */

	fprintf (heap_dump_file, "<section type=\"%s\" size=\"%lu\">\n", type, (unsigned long)section->size);

	while (start < end) {
		guint size;
		MonoClass *class;

		if (!*(void**)start) {
			if (occ_start) {
				mono_sgen_dump_occupied (occ_start, start, section->data);
				occ_start = NULL;
			}
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		g_assert (start < section->next_data);

		if (!occ_start)
			occ_start = start;

		vt = (GCVTable*)LOAD_VTABLE (start);
		class = vt->klass;

		size = ALIGN_UP (safe_object_get_size ((MonoObject*) start));

		/*
		fprintf (heap_dump_file, "<object offset=\"%d\" class=\"%s.%s\" size=\"%d\"/>\n",
				start - section->data,
				vt->klass->name_space, vt->klass->name,
				size);
		*/

		old_start = start;
		start += size;
	}
	if (occ_start)
		mono_sgen_dump_occupied (occ_start, start, section->data);

	fprintf (heap_dump_file, "</section>\n");
}

static void
dump_object (MonoObject *obj, gboolean dump_location)
{
	static char class_name [1024];

	MonoClass *class = mono_object_class (obj);
	int i, j;

	/*
	 * Python's XML parser is too stupid to parse angle brackets
	 * in strings, so we just ignore them;
	 */
	i = j = 0;
	while (class->name [i] && j < sizeof (class_name) - 1) {
		if (!strchr ("<>\"", class->name [i]))
			class_name [j++] = class->name [i];
		++i;
	}
	g_assert (j < sizeof (class_name));
	class_name [j] = 0;

	fprintf (heap_dump_file, "<object class=\"%s.%s\" size=\"%d\"",
			class->name_space, class_name,
			safe_object_get_size (obj));
	if (dump_location) {
		const char *location;
		if (ptr_in_nursery (obj))
			location = "nursery";
		else if (safe_object_get_size (obj) <= MAX_SMALL_OBJ_SIZE)
			location = "major";
		else
			location = "LOS";
		fprintf (heap_dump_file, " location=\"%s\"", location);
	}
	fprintf (heap_dump_file, "/>\n");
}

static void
dump_heap (const char *type, int num, const char *reason)
{
	ObjectList *list;
	LOSObject *bigobj;

	fprintf (heap_dump_file, "<collection type=\"%s\" num=\"%d\"", type, num);
	if (reason)
		fprintf (heap_dump_file, " reason=\"%s\"", reason);
	fprintf (heap_dump_file, ">\n");
	fprintf (heap_dump_file, "<other-mem-usage type=\"mempools\" size=\"%ld\"/>\n", mono_mempool_get_bytes_allocated ());
	mono_sgen_dump_internal_mem_usage (heap_dump_file);
	fprintf (heap_dump_file, "<pinned type=\"stack\" bytes=\"%zu\"/>\n", mono_sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_STACK));
	/* fprintf (heap_dump_file, "<pinned type=\"static-data\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_STATIC_DATA]); */
	fprintf (heap_dump_file, "<pinned type=\"other\" bytes=\"%zu\"/>\n", mono_sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_OTHER));

	fprintf (heap_dump_file, "<pinned-objects>\n");
	for (list = mono_sgen_pin_stats_get_object_list (); list; list = list->next)
		dump_object (list->obj, TRUE);
	fprintf (heap_dump_file, "</pinned-objects>\n");

	mono_sgen_dump_section (nursery_section, "nursery");

	major_collector.dump_heap (heap_dump_file);

	fprintf (heap_dump_file, "<los>\n");
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		dump_object ((MonoObject*)bigobj->data, FALSE);
	fprintf (heap_dump_file, "</los>\n");

	fprintf (heap_dump_file, "</collection>\n");
}

void
mono_sgen_register_moved_object (void *obj, void *destination)
{
	g_assert (mono_profiler_events & MONO_PROFILE_GC_MOVES);

	/* FIXME: handle this for parallel collector */
	g_assert (!mono_sgen_collection_is_parallel ());

	if (moved_objects_idx == MOVED_OBJECTS_NUM) {
		mono_profiler_gc_moves (moved_objects, moved_objects_idx);
		moved_objects_idx = 0;
	}
	moved_objects [moved_objects_idx++] = obj;
	moved_objects [moved_objects_idx++] = destination;
}

static void
init_stats (void)
{
	static gboolean inited = FALSE;

	if (inited)
		return;

	mono_counters_register ("Minor fragment clear", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_pre_collection_fragment_clear);
	mono_counters_register ("Minor pinning", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_pinning);
	mono_counters_register ("Minor scan remembered set", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_scan_remsets);
	mono_counters_register ("Minor scan pinned", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_scan_pinned);
	mono_counters_register ("Minor scan registered roots", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_scan_registered_roots);
	mono_counters_register ("Minor scan thread data", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_scan_thread_data);
	mono_counters_register ("Minor finish gray stack", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_finish_gray_stack);
	mono_counters_register ("Minor fragment creation", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_minor_fragment_creation);

	mono_counters_register ("Major fragment clear", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_pre_collection_fragment_clear);
	mono_counters_register ("Major pinning", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_pinning);
	mono_counters_register ("Major scan pinned", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_pinned);
	mono_counters_register ("Major scan registered roots", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_registered_roots);
	mono_counters_register ("Major scan thread data", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_thread_data);
	mono_counters_register ("Major scan alloc_pinned", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_alloc_pinned);
	mono_counters_register ("Major scan finalized", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_finalized);
	mono_counters_register ("Major scan big objects", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_scan_big_objects);
	mono_counters_register ("Major finish gray stack", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_finish_gray_stack);
	mono_counters_register ("Major free big objects", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_free_bigobjs);
	mono_counters_register ("Major LOS sweep", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_los_sweep);
	mono_counters_register ("Major sweep", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_sweep);
	mono_counters_register ("Major fragment creation", MONO_COUNTER_GC | MONO_COUNTER_TIME_INTERVAL, &time_major_fragment_creation);

	mono_counters_register ("Number of pinned objects", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_pinned_objects);

#ifdef HEAVY_STATISTICS
	mono_counters_register ("WBarrier set field", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_field);
	mono_counters_register ("WBarrier set arrayref", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_arrayref);
	mono_counters_register ("WBarrier arrayref copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_arrayref_copy);
	mono_counters_register ("WBarrier generic store called", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_generic_store);
	mono_counters_register ("WBarrier set root", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_root);
	mono_counters_register ("WBarrier value copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_value_copy);
	mono_counters_register ("WBarrier object copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_object_copy);

	mono_counters_register ("# objects allocated degraded", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_alloced_degraded);
	mono_counters_register ("bytes allocated degraded", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_bytes_alloced_degraded);

	mono_counters_register ("# copy_object() called (nursery)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_called_nursery);
	mono_counters_register ("# objects copied (nursery)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_copied_nursery);
	mono_counters_register ("# copy_object() called (major)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_called_major);
	mono_counters_register ("# objects copied (major)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_copied_major);

	mono_counters_register ("# scan_object() called (nursery)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_scan_object_called_nursery);
	mono_counters_register ("# scan_object() called (major)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_scan_object_called_major);

	mono_counters_register ("# nursery copy_object() failed from space", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_nursery_copy_object_failed_from_space);
	mono_counters_register ("# nursery copy_object() failed forwarded", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_nursery_copy_object_failed_forwarded);
	mono_counters_register ("# nursery copy_object() failed pinned", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_nursery_copy_object_failed_pinned);

	mono_sgen_nursery_allocator_init_heavy_stats ();
	mono_sgen_alloc_init_heavy_stats ();
#endif

	inited = TRUE;
}

static gboolean need_calculate_minor_collection_allowance;

static int last_collection_old_num_major_sections;
static mword last_collection_los_memory_usage = 0;
static mword last_collection_old_los_memory_usage;
static mword last_collection_los_memory_alloced;

static void
reset_minor_collection_allowance (void)
{
	need_calculate_minor_collection_allowance = TRUE;
}

static void
try_calculate_minor_collection_allowance (gboolean overwrite)
{
	int num_major_sections, num_major_sections_saved, save_target, allowance_target;
	mword los_memory_saved, new_major, new_heap_size;

	if (overwrite)
		g_assert (need_calculate_minor_collection_allowance);

	if (!need_calculate_minor_collection_allowance)
		return;

	if (!*major_collector.have_swept) {
		if (overwrite)
			minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
		return;
	}

	num_major_sections = major_collector.get_num_major_sections ();

	num_major_sections_saved = MAX (last_collection_old_num_major_sections - num_major_sections, 0);
	los_memory_saved = MAX (last_collection_old_los_memory_usage - last_collection_los_memory_usage, 1);

	new_major = num_major_sections * major_collector.section_size;
	new_heap_size = new_major + last_collection_los_memory_usage;

	/*
	 * FIXME: Why is save_target half the major memory plus half the
	 * LOS memory saved?  Shouldn't it be half the major memory
	 * saved plus half the LOS memory saved?  Or half the whole heap
	 * size?
	 */
	save_target = (new_major + los_memory_saved) / 2;

	/*
	 * We aim to allow the allocation of as many sections as is
	 * necessary to reclaim save_target sections in the next
	 * collection.  We assume the collection pattern won't change.
	 * In the last cycle, we had num_major_sections_saved for
	 * minor_collection_sections_alloced.  Assuming things won't
	 * change, this must be the same ratio as save_target for
	 * allowance_target, i.e.
	 *
	 *    num_major_sections_saved            save_target
	 * --------------------------------- == ----------------
	 * minor_collection_sections_alloced    allowance_target
	 *
	 * hence:
	 */
	allowance_target = (mword)((double)save_target * (double)(minor_collection_sections_alloced * major_collector.section_size + last_collection_los_memory_alloced) / (double)(num_major_sections_saved * major_collector.section_size + los_memory_saved));

	minor_collection_allowance = MAX (MIN (allowance_target, num_major_sections * major_collector.section_size + los_memory_usage), MIN_MINOR_COLLECTION_ALLOWANCE);

	if (new_heap_size + minor_collection_allowance > soft_heap_limit) {
		if (new_heap_size > soft_heap_limit)
			minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
		else
			minor_collection_allowance = MAX (soft_heap_limit - new_heap_size, MIN_MINOR_COLLECTION_ALLOWANCE);
	}

	if (debug_print_allowance) {
		mword old_major = last_collection_old_num_major_sections * major_collector.section_size;

		fprintf (gc_debug_file, "Before collection: %ld bytes (%ld major, %ld LOS)\n",
				old_major + last_collection_old_los_memory_usage, old_major, last_collection_old_los_memory_usage);
		fprintf (gc_debug_file, "After collection: %ld bytes (%ld major, %ld LOS)\n",
				new_heap_size, new_major, last_collection_los_memory_usage);
		fprintf (gc_debug_file, "Allowance: %ld bytes\n", minor_collection_allowance);
	}

	if (major_collector.have_computed_minor_collection_allowance)
		major_collector.have_computed_minor_collection_allowance ();

	need_calculate_minor_collection_allowance = FALSE;
}

static gboolean
need_major_collection (mword space_needed)
{
	mword los_alloced = los_memory_usage - MIN (last_collection_los_memory_usage, los_memory_usage);
	return (space_needed > available_free_space ()) ||
		minor_collection_sections_alloced * major_collector.section_size + los_alloced > minor_collection_allowance;
}

gboolean
mono_sgen_need_major_collection (mword space_needed)
{
	return need_major_collection (space_needed);
}

static void
reset_pinned_from_failed_allocation (void)
{
	bytes_pinned_from_failed_allocation = 0;
}

void
mono_sgen_set_pinned_from_failed_allocation (mword objsize)
{
	bytes_pinned_from_failed_allocation += objsize;
}

gboolean
mono_sgen_collection_is_parallel (void)
{
	switch (current_collection_generation) {
	case GENERATION_NURSERY:
		return nursery_collection_is_parallel;
	case GENERATION_OLD:
		return major_collector.is_parallel;
	default:
		g_assert_not_reached ();
	}
}

gboolean
mono_sgen_nursery_collection_is_parallel (void)
{
	return nursery_collection_is_parallel;
}

typedef struct
{
	char *heap_start;
	char *heap_end;
} FinishRememberedSetScanJobData;

static void
job_finish_remembered_set_scan (WorkerData *worker_data, void *job_data_untyped)
{
	FinishRememberedSetScanJobData *job_data = job_data_untyped;

	remset.finish_scan_remsets (job_data->heap_start, job_data->heap_end, mono_sgen_workers_get_job_gray_queue (worker_data));
}

typedef struct
{
	CopyOrMarkObjectFunc func;
	char *heap_start;
	char *heap_end;
	int root_type;
} ScanFromRegisteredRootsJobData;

static void
job_scan_from_registered_roots (WorkerData *worker_data, void *job_data_untyped)
{
	ScanFromRegisteredRootsJobData *job_data = job_data_untyped;

	scan_from_registered_roots (job_data->func,
			job_data->heap_start, job_data->heap_end,
			job_data->root_type,
			mono_sgen_workers_get_job_gray_queue (worker_data));
}

typedef struct
{
	char *heap_start;
	char *heap_end;
} ScanThreadDataJobData;

static void
job_scan_thread_data (WorkerData *worker_data, void *job_data_untyped)
{
	ScanThreadDataJobData *job_data = job_data_untyped;

	scan_thread_data (job_data->heap_start, job_data->heap_end, TRUE,
			mono_sgen_workers_get_job_gray_queue (worker_data));
}

static void
verify_scan_starts (char *start, char *end)
{
	int i;

	for (i = 0; i < nursery_section->num_scan_start; ++i) {
		char *addr = nursery_section->scan_starts [i];
		if (addr > start && addr < end)
			fprintf (gc_debug_file, "NFC-BAD SCAN START [%d] %p for obj [%p %p]\n", i, addr, start, end);
	}
}

static void
verify_nursery (void)
{
	char *start, *end, *cur, *hole_start;

	if (!do_verify_nursery)
		return;

	/*This cleans up unused fragments */
	mono_sgen_nursery_allocator_prepare_for_pinning ();

	hole_start = start = cur = mono_sgen_get_nursery_start ();
	end = mono_sgen_get_nursery_end ();

	while (cur < end) {
		size_t ss, size;

		if (!*(void**)cur) {
			cur += sizeof (void*);
			continue;
		}

		if (object_is_forwarded (cur))
			fprintf (gc_debug_file, "FORWARDED OBJ %p\n", cur);
		else if (object_is_pinned (cur))
			fprintf (gc_debug_file, "PINNED OBJ %p\n", cur);

		ss = safe_object_get_size ((MonoObject*)cur);
		size = ALIGN_UP (safe_object_get_size ((MonoObject*)cur));
		verify_scan_starts (cur, cur + size);
		if (do_dump_nursery_content) {
			if (cur > hole_start)
				fprintf (gc_debug_file, "HOLE [%p %p %d]\n", hole_start, cur, (int)(cur - hole_start));
			fprintf (gc_debug_file, "OBJ  [%p %p %d %d %s %d]\n", cur, cur + size, (int)size, (int)ss, mono_sgen_safe_name ((MonoObject*)cur), (gpointer)LOAD_VTABLE (cur) == mono_sgen_get_array_fill_vtable ());
		}
		cur += size;
		hole_start = cur;
	}
	fflush (gc_debug_file);
}

/*
 * Collect objects in the nursery.  Returns whether to trigger a major
 * collection.
 */
static gboolean
collect_nursery (size_t requested_size)
{
	gboolean needs_major;
	size_t max_garbage_amount;
	char *nursery_next;
	FinishRememberedSetScanJobData frssjd;
	ScanFromRegisteredRootsJobData scrrjd_normal, scrrjd_wbarrier;
	ScanThreadDataJobData stdjd;
	mword fragment_total;
	TV_DECLARE (all_atv);
	TV_DECLARE (all_btv);
	TV_DECLARE (atv);
	TV_DECLARE (btv);

	if (disable_minor_collections)
		return TRUE;

	verify_nursery ();

	mono_perfcounters->gc_collections0++;

	current_collection_generation = GENERATION_NURSERY;

	reset_pinned_from_failed_allocation ();

	binary_protocol_collection (GENERATION_NURSERY);
	check_scan_starts ();

	degraded_mode = 0;
	objects_pinned = 0;
	nursery_next = mono_sgen_nursery_alloc_get_upper_alloc_bound ();
	/* FIXME: optimize later to use the higher address where an object can be present */
	nursery_next = MAX (nursery_next, mono_sgen_get_nursery_end ());

	DEBUG (1, fprintf (gc_debug_file, "Start nursery collection %d %p-%p, size: %d\n", stat_minor_gcs, mono_sgen_get_nursery_start (), nursery_next, (int)(nursery_next - mono_sgen_get_nursery_start ())));
	max_garbage_amount = nursery_next - mono_sgen_get_nursery_start ();
	g_assert (nursery_section->size >= max_garbage_amount);

	/* world must be stopped already */
	TV_GETTIME (all_atv);
	atv = all_atv;

	/* Pinning no longer depends on clearing all nursery fragments */
	mono_sgen_clear_current_nursery_fragment ();

	TV_GETTIME (btv);
	time_minor_pre_collection_fragment_clear += TV_ELAPSED (atv, btv);

	if (xdomain_checks)
		check_for_xdomain_refs ();

	nursery_section->next_data = nursery_next;

	major_collector.start_nursery_collection ();

	try_calculate_minor_collection_allowance (FALSE);

	mono_sgen_gray_object_queue_init (&gray_queue);
	mono_sgen_workers_init_distribute_gray_queue ();

	stat_minor_gcs++;
	mono_stats.minor_gc_count ++;

	if (remset.prepare_for_minor_collection)
		remset.prepare_for_minor_collection ();

	process_fin_stage_entries ();
	process_dislink_stage_entries ();

	/* pin from pinned handles */
	mono_sgen_init_pinning ();
	mono_profiler_gc_event (MONO_GC_EVENT_MARK_START, 0);
	pin_from_roots (mono_sgen_get_nursery_start (), nursery_next, WORKERS_DISTRIBUTE_GRAY_QUEUE);
	/* identify pinned objects */
	mono_sgen_optimize_pin_queue (0);
	mono_sgen_pinning_setup_section (nursery_section);
	mono_sgen_pin_objects_in_section (nursery_section, WORKERS_DISTRIBUTE_GRAY_QUEUE);	

	TV_GETTIME (atv);
	time_minor_pinning += TV_ELAPSED (btv, atv);
	DEBUG (2, fprintf (gc_debug_file, "Finding pinned pointers: %d in %d usecs\n", mono_sgen_get_pinned_count (), TV_ELAPSED (btv, atv)));
	DEBUG (4, fprintf (gc_debug_file, "Start scan with %d pinned objects\n", mono_sgen_get_pinned_count ()));

	if (consistency_check_at_minor_collection)
		mono_sgen_check_consistency ();

	mono_sgen_workers_start_all_workers ();

	/*
	 * Perform the sequential part of remembered set scanning.
	 * This usually involves scanning global information that might later be produced by evacuation.
	 */
	if (remset.begin_scan_remsets)
		remset.begin_scan_remsets (mono_sgen_get_nursery_start (), nursery_next, WORKERS_DISTRIBUTE_GRAY_QUEUE);

	mono_sgen_workers_start_marking ();

	frssjd.heap_start = mono_sgen_get_nursery_start ();
	frssjd.heap_end = nursery_next;
	mono_sgen_workers_enqueue_job (job_finish_remembered_set_scan, &frssjd);

	/* we don't have complete write barrier yet, so we scan all the old generation sections */
	TV_GETTIME (btv);
	time_minor_scan_remsets += TV_ELAPSED (atv, btv);
	DEBUG (2, fprintf (gc_debug_file, "Old generation scan: %d usecs\n", TV_ELAPSED (atv, btv)));

	if (!mono_sgen_collection_is_parallel ())
		mono_sgen_drain_gray_stack (&gray_queue, -1);

	if (mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS)
		report_registered_roots ();
	if (mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS)
		report_finalizer_roots ();
	TV_GETTIME (atv);
	time_minor_scan_pinned += TV_ELAPSED (btv, atv);

	/* registered roots, this includes static fields */
	scrrjd_normal.func = mono_sgen_collection_is_parallel () ? major_collector.copy_object : major_collector.nopar_copy_object;
	scrrjd_normal.heap_start = mono_sgen_get_nursery_start ();
	scrrjd_normal.heap_end = nursery_next;
	scrrjd_normal.root_type = ROOT_TYPE_NORMAL;
	mono_sgen_workers_enqueue_job (job_scan_from_registered_roots, &scrrjd_normal);

	scrrjd_wbarrier.func = mono_sgen_collection_is_parallel () ? major_collector.copy_object : major_collector.nopar_copy_object;
	scrrjd_wbarrier.heap_start = mono_sgen_get_nursery_start ();
	scrrjd_wbarrier.heap_end = nursery_next;
	scrrjd_wbarrier.root_type = ROOT_TYPE_WBARRIER;
	mono_sgen_workers_enqueue_job (job_scan_from_registered_roots, &scrrjd_wbarrier);

	TV_GETTIME (btv);
	time_minor_scan_registered_roots += TV_ELAPSED (atv, btv);

	/* thread data */
	stdjd.heap_start = mono_sgen_get_nursery_start ();
	stdjd.heap_end = nursery_next;
	mono_sgen_workers_enqueue_job (job_scan_thread_data, &stdjd);

	TV_GETTIME (atv);
	time_minor_scan_thread_data += TV_ELAPSED (btv, atv);
	btv = atv;

	if (mono_sgen_collection_is_parallel ()) {
		while (!mono_sgen_gray_object_queue_is_empty (WORKERS_DISTRIBUTE_GRAY_QUEUE)) {
			mono_sgen_workers_distribute_gray_queue_sections ();
			g_usleep (1000);
		}
	}
	mono_sgen_workers_join ();

	if (mono_sgen_collection_is_parallel ())
		g_assert (mono_sgen_gray_object_queue_is_empty (&gray_queue));

	finish_gray_stack (mono_sgen_get_nursery_start (), nursery_next, GENERATION_NURSERY, &gray_queue);
	TV_GETTIME (atv);
	time_minor_finish_gray_stack += TV_ELAPSED (btv, atv);
	mono_profiler_gc_event (MONO_GC_EVENT_MARK_END, 0);

	/*
	 * The (single-threaded) finalization code might have done
	 * some copying/marking so we can only reset the GC thread's
	 * worker data here instead of earlier when we joined the
	 * workers.
	 */
	mono_sgen_workers_reset_data ();

	if (objects_pinned) {
		mono_sgen_optimize_pin_queue (0);
		mono_sgen_pinning_setup_section (nursery_section);
	}

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	mono_profiler_gc_event (MONO_GC_EVENT_RECLAIM_START, 0);
	fragment_total = mono_sgen_build_nursery_fragments (nursery_section, nursery_section->pin_queue_start, nursery_section->pin_queue_num_entries);
	if (!fragment_total)
		degraded_mode = 1;

	/* Clear TLABs for all threads */
	mono_sgen_clear_tlabs ();

	mono_profiler_gc_event (MONO_GC_EVENT_RECLAIM_END, 0);
	TV_GETTIME (btv);
	time_minor_fragment_creation += TV_ELAPSED (atv, btv);
	DEBUG (2, fprintf (gc_debug_file, "Fragment creation: %d usecs, %lu bytes available\n", TV_ELAPSED (atv, btv), (unsigned long)fragment_total));

	if (consistency_check_at_minor_collection)
		mono_sgen_check_major_refs ();

	major_collector.finish_nursery_collection ();

	TV_GETTIME (all_btv);
	mono_stats.minor_gc_time_usecs += TV_ELAPSED (all_atv, all_btv);

	if (heap_dump_file)
		dump_heap ("minor", stat_minor_gcs - 1, NULL);

	/* prepare the pin queue for the next collection */
	mono_sgen_finish_pinning ();
	if (fin_ready_list || critical_fin_list) {
		DEBUG (4, fprintf (gc_debug_file, "Finalizer-thread wakeup: ready %d\n", num_ready_finalizers));
		mono_gc_finalize_notify ();
	}
	mono_sgen_pin_stats_reset ();

	g_assert (mono_sgen_gray_object_queue_is_empty (&gray_queue));

	if (remset.finish_minor_collection)
		remset.finish_minor_collection ();

	check_scan_starts ();

	binary_protocol_flush_buffers (FALSE);

	/*objects are late pinned because of lack of memory, so a major is a good call*/
	needs_major = need_major_collection (0) || objects_pinned;
	current_collection_generation = -1;
	objects_pinned = 0;

	return needs_major;
}

void
mono_sgen_collect_nursery_no_lock (size_t requested_size)
{
	gint64 gc_start_time;

	mono_profiler_gc_event (MONO_GC_EVENT_START, 0);
	gc_start_time = mono_100ns_ticks ();

	stop_world (0);
	collect_nursery (requested_size);
	restart_world (0);

	mono_trace_message (MONO_TRACE_GC, "minor gc took %d usecs", (mono_100ns_ticks () - gc_start_time) / 10);
	mono_profiler_gc_event (MONO_GC_EVENT_END, 0);
}

typedef struct
{
	FinalizeReadyEntry *list;
} ScanFinalizerEntriesJobData;

static void
job_scan_finalizer_entries (WorkerData *worker_data, void *job_data_untyped)
{
	ScanFinalizerEntriesJobData *job_data = job_data_untyped;

	scan_finalizer_entries (major_collector.copy_or_mark_object,
			job_data->list,
			mono_sgen_workers_get_job_gray_queue (worker_data));
}

static gboolean
major_do_collection (const char *reason)
{
	LOSObject *bigobj, *prevbo;
	TV_DECLARE (all_atv);
	TV_DECLARE (all_btv);
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	/* FIXME: only use these values for the precise scan
	 * note that to_space pointers should be excluded anyway...
	 */
	char *heap_start = NULL;
	char *heap_end = (char*)-1;
	int old_next_pin_slot;
	ScanFromRegisteredRootsJobData scrrjd_normal, scrrjd_wbarrier;
	ScanThreadDataJobData stdjd;
	ScanFinalizerEntriesJobData sfejd_fin_ready, sfejd_critical_fin;

	mono_perfcounters->gc_collections1++;

	reset_pinned_from_failed_allocation ();

	last_collection_old_num_major_sections = major_collector.get_num_major_sections ();

	/*
	 * A domain could have been freed, resulting in
	 * los_memory_usage being less than last_collection_los_memory_usage.
	 */
	last_collection_los_memory_alloced = los_memory_usage - MIN (last_collection_los_memory_usage, los_memory_usage);
	last_collection_old_los_memory_usage = los_memory_usage;
	objects_pinned = 0;

	//count_ref_nonref_objs ();
	//consistency_check ();

	binary_protocol_collection (GENERATION_OLD);
	check_scan_starts ();
	mono_sgen_gray_object_queue_init (&gray_queue);
	mono_sgen_workers_init_distribute_gray_queue ();

	degraded_mode = 0;
	DEBUG (1, fprintf (gc_debug_file, "Start major collection %d\n", stat_major_gcs));
	stat_major_gcs++;
	mono_stats.major_gc_count ++;

	/* world must be stopped already */
	TV_GETTIME (all_atv);
	atv = all_atv;

	/* Pinning depends on this */
	mono_sgen_clear_nursery_fragments ();

	TV_GETTIME (btv);
	time_major_pre_collection_fragment_clear += TV_ELAPSED (atv, btv);

	nursery_section->next_data = mono_sgen_get_nursery_end ();
	/* we should also coalesce scanning from sections close to each other
	 * and deal with pointers outside of the sections later.
	 */

	if (major_collector.start_major_collection)
		major_collector.start_major_collection ();

	*major_collector.have_swept = FALSE;
	reset_minor_collection_allowance ();

	if (xdomain_checks)
		check_for_xdomain_refs ();

	/* Remsets are not useful for a major collection */
	remset.prepare_for_major_collection ();

	process_fin_stage_entries ();
	process_dislink_stage_entries ();

	TV_GETTIME (atv);
	mono_sgen_init_pinning ();
	DEBUG (6, fprintf (gc_debug_file, "Collecting pinned addresses\n"));
	pin_from_roots ((void*)lowest_heap_address, (void*)highest_heap_address, WORKERS_DISTRIBUTE_GRAY_QUEUE);
	mono_sgen_optimize_pin_queue (0);

	/*
	 * pin_queue now contains all candidate pointers, sorted and
	 * uniqued.  We must do two passes now to figure out which
	 * objects are pinned.
	 *
	 * The first is to find within the pin_queue the area for each
	 * section.  This requires that the pin_queue be sorted.  We
	 * also process the LOS objects and pinned chunks here.
	 *
	 * The second, destructive, pass is to reduce the section
	 * areas to pointers to the actually pinned objects.
	 */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from sections\n"));
	/* first pass for the sections */
	mono_sgen_find_section_pin_queue_start_end (nursery_section);
	major_collector.find_pin_queue_start_ends (WORKERS_DISTRIBUTE_GRAY_QUEUE);
	/* identify possible pointers to the insize of large objects */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from large objects\n"));
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		int dummy;
		gboolean profile_roots = mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS;
		GCRootReport report;
		report.count = 0;
		if (mono_sgen_find_optimized_pin_queue_area (bigobj->data, (char*)bigobj->data + bigobj->size, &dummy)) {
			binary_protocol_pin (bigobj->data, (gpointer)LOAD_VTABLE (bigobj->data), safe_object_get_size (bigobj->data));
			pin_object (bigobj->data);
			/* FIXME: only enqueue if object has references */
			GRAY_OBJECT_ENQUEUE (WORKERS_DISTRIBUTE_GRAY_QUEUE, bigobj->data);
			if (G_UNLIKELY (do_pin_stats))
				mono_sgen_pin_stats_register_object ((char*) bigobj->data, safe_object_get_size ((MonoObject*) bigobj->data));
			DEBUG (6, fprintf (gc_debug_file, "Marked large object %p (%s) size: %lu from roots\n", bigobj->data, safe_name (bigobj->data), (unsigned long)bigobj->size));
			
			if (profile_roots)
				add_profile_gc_root (&report, bigobj->data, MONO_PROFILE_GC_ROOT_PINNING | MONO_PROFILE_GC_ROOT_MISC, 0);
		}
		if (profile_roots)
			notify_gc_roots (&report);
	}
	/* second pass for the sections */
	mono_sgen_pin_objects_in_section (nursery_section, WORKERS_DISTRIBUTE_GRAY_QUEUE);
	major_collector.pin_objects (WORKERS_DISTRIBUTE_GRAY_QUEUE);
	old_next_pin_slot = mono_sgen_get_pinned_count ();

	TV_GETTIME (btv);
	time_major_pinning += TV_ELAPSED (atv, btv);
	DEBUG (2, fprintf (gc_debug_file, "Finding pinned pointers: %d in %d usecs\n", mono_sgen_get_pinned_count (), TV_ELAPSED (atv, btv)));
	DEBUG (4, fprintf (gc_debug_file, "Start scan with %d pinned objects\n", mono_sgen_get_pinned_count ()));

	major_collector.init_to_space ();

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
	main_gc_thread = mono_native_thread_self ();
#endif

	mono_sgen_workers_start_all_workers ();
	mono_sgen_workers_start_marking ();

	if (mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS)
		report_registered_roots ();
	TV_GETTIME (atv);
	time_major_scan_pinned += TV_ELAPSED (btv, atv);

	/* registered roots, this includes static fields */
	scrrjd_normal.func = major_collector.copy_or_mark_object;
	scrrjd_normal.heap_start = heap_start;
	scrrjd_normal.heap_end = heap_end;
	scrrjd_normal.root_type = ROOT_TYPE_NORMAL;
	mono_sgen_workers_enqueue_job (job_scan_from_registered_roots, &scrrjd_normal);

	scrrjd_wbarrier.func = major_collector.copy_or_mark_object;
	scrrjd_wbarrier.heap_start = heap_start;
	scrrjd_wbarrier.heap_end = heap_end;
	scrrjd_wbarrier.root_type = ROOT_TYPE_WBARRIER;
	mono_sgen_workers_enqueue_job (job_scan_from_registered_roots, &scrrjd_wbarrier);

	TV_GETTIME (btv);
	time_major_scan_registered_roots += TV_ELAPSED (atv, btv);

	/* Threads */
	stdjd.heap_start = heap_start;
	stdjd.heap_end = heap_end;
	mono_sgen_workers_enqueue_job (job_scan_thread_data, &stdjd);

	TV_GETTIME (atv);
	time_major_scan_thread_data += TV_ELAPSED (btv, atv);

	TV_GETTIME (btv);
	time_major_scan_alloc_pinned += TV_ELAPSED (atv, btv);

	if (mono_profiler_get_events () & MONO_PROFILE_GC_ROOTS)
		report_finalizer_roots ();

	/* scan the list of objects ready for finalization */
	sfejd_fin_ready.list = fin_ready_list;
	mono_sgen_workers_enqueue_job (job_scan_finalizer_entries, &sfejd_fin_ready);

	sfejd_critical_fin.list = critical_fin_list;
	mono_sgen_workers_enqueue_job (job_scan_finalizer_entries, &sfejd_critical_fin);

	TV_GETTIME (atv);
	time_major_scan_finalized += TV_ELAPSED (btv, atv);
	DEBUG (2, fprintf (gc_debug_file, "Root scan: %d usecs\n", TV_ELAPSED (btv, atv)));

	TV_GETTIME (btv);
	time_major_scan_big_objects += TV_ELAPSED (atv, btv);

	if (major_collector.is_parallel) {
		while (!mono_sgen_gray_object_queue_is_empty (WORKERS_DISTRIBUTE_GRAY_QUEUE)) {
			mono_sgen_workers_distribute_gray_queue_sections ();
			g_usleep (1000);
		}
	}
	mono_sgen_workers_join ();

#ifdef SGEN_DEBUG_INTERNAL_ALLOC
	main_gc_thread = NULL;
#endif

	if (major_collector.is_parallel)
		g_assert (mono_sgen_gray_object_queue_is_empty (&gray_queue));

	/* all the objects in the heap */
	finish_gray_stack (heap_start, heap_end, GENERATION_OLD, &gray_queue);
	TV_GETTIME (atv);
	time_major_finish_gray_stack += TV_ELAPSED (btv, atv);

	/*
	 * The (single-threaded) finalization code might have done
	 * some copying/marking so we can only reset the GC thread's
	 * worker data here instead of earlier when we joined the
	 * workers.
	 */
	mono_sgen_workers_reset_data ();

	if (objects_pinned) {
		/*This is slow, but we just OOM'd*/
		mono_sgen_pin_queue_clear_discarded_entries (nursery_section, old_next_pin_slot);
		mono_sgen_optimize_pin_queue (0);
		mono_sgen_find_section_pin_queue_start_end (nursery_section);
		objects_pinned = 0;
	}

	reset_heap_boundaries ();
	mono_sgen_update_heap_boundaries ((mword)mono_sgen_get_nursery_start (), (mword)mono_sgen_get_nursery_end ());

	/* sweep the big objects list */
	prevbo = NULL;
	for (bigobj = los_object_list; bigobj;) {
		if (object_is_pinned (bigobj->data)) {
			unpin_object (bigobj->data);
			mono_sgen_update_heap_boundaries ((mword)bigobj->data, (mword)bigobj->data + bigobj->size);
		} else {
			LOSObject *to_free;
			/* not referenced anywhere, so we can free it */
			if (prevbo)
				prevbo->next = bigobj->next;
			else
				los_object_list = bigobj->next;
			to_free = bigobj;
			bigobj = bigobj->next;
			mono_sgen_los_free_object (to_free);
			continue;
		}
		prevbo = bigobj;
		bigobj = bigobj->next;
	}

	TV_GETTIME (btv);
	time_major_free_bigobjs += TV_ELAPSED (atv, btv);

	mono_sgen_los_sweep ();

	TV_GETTIME (atv);
	time_major_los_sweep += TV_ELAPSED (btv, atv);

	major_collector.sweep ();

	TV_GETTIME (btv);
	time_major_sweep += TV_ELAPSED (atv, btv);

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	if (!mono_sgen_build_nursery_fragments (nursery_section, nursery_section->pin_queue_start, nursery_section->pin_queue_num_entries))
		degraded_mode = 1;

	/* Clear TLABs for all threads */
	mono_sgen_clear_tlabs ();

	TV_GETTIME (atv);
	time_major_fragment_creation += TV_ELAPSED (btv, atv);

	TV_GETTIME (all_btv);
	mono_stats.major_gc_time_usecs += TV_ELAPSED (all_atv, all_btv);

	if (heap_dump_file)
		dump_heap ("major", stat_major_gcs - 1, reason);

	/* prepare the pin queue for the next collection */
	mono_sgen_finish_pinning ();

	if (fin_ready_list || critical_fin_list) {
		DEBUG (4, fprintf (gc_debug_file, "Finalizer-thread wakeup: ready %d\n", num_ready_finalizers));
		mono_gc_finalize_notify ();
	}
	mono_sgen_pin_stats_reset ();

	g_assert (mono_sgen_gray_object_queue_is_empty (&gray_queue));

	try_calculate_minor_collection_allowance (TRUE);

	minor_collection_sections_alloced = 0;
	last_collection_los_memory_usage = los_memory_usage;

	major_collector.finish_major_collection ();

	check_scan_starts ();

	binary_protocol_flush_buffers (FALSE);

	//consistency_check ();

	return bytes_pinned_from_failed_allocation > 0;
}

static void
major_collection (const char *reason)
{
	gboolean need_minor_collection;

	if (disable_major_collections) {
		collect_nursery (0);
		return;
	}

	major_collection_happened = TRUE;
	current_collection_generation = GENERATION_OLD;
	need_minor_collection = major_do_collection (reason);
	current_collection_generation = -1;

	if (need_minor_collection)
		collect_nursery (0);
}

void
sgen_collect_major_no_lock (const char *reason)
{
	gint64 gc_start_time;

	mono_profiler_gc_event (MONO_GC_EVENT_START, 1);
	gc_start_time = mono_100ns_ticks ();
	stop_world (1);
	major_collection (reason);
	restart_world (1);
	mono_trace_message (MONO_TRACE_GC, "major gc took %d usecs", (mono_100ns_ticks () - gc_start_time) / 10);
	mono_profiler_gc_event (MONO_GC_EVENT_END, 1);
}

/*
 * When deciding if it's better to collect or to expand, keep track
 * of how much garbage was reclaimed with the last collection: if it's too
 * little, expand.
 * This is called when we could not allocate a small object.
 */
static void __attribute__((noinline))
minor_collect_or_expand_inner (size_t size)
{
	int do_minor_collection = 1;

	g_assert (nursery_section);
	if (do_minor_collection) {
		gint64 total_gc_time, major_gc_time = 0;

		mono_profiler_gc_event (MONO_GC_EVENT_START, 0);
		total_gc_time = mono_100ns_ticks ();

		stop_world (0);
		if (collect_nursery (size)) {
			mono_profiler_gc_event (MONO_GC_EVENT_START, 1);
			major_gc_time = mono_100ns_ticks ();

			major_collection ("minor overflow");

			/* keep events symmetric */
			major_gc_time = mono_100ns_ticks () - major_gc_time;
			mono_profiler_gc_event (MONO_GC_EVENT_END, 1);
		}
		DEBUG (2, fprintf (gc_debug_file, "Heap size: %lu, LOS size: %lu\n", (unsigned long)total_alloc, (unsigned long)los_memory_usage));
		restart_world (0);

		total_gc_time = mono_100ns_ticks () - total_gc_time;
		if (major_gc_time)
			mono_trace_message (MONO_TRACE_GC, "overflow major gc took %d usecs minor gc took %d usecs", total_gc_time / 10, (total_gc_time - major_gc_time) / 10);
		else
			mono_trace_message (MONO_TRACE_GC, "minor gc took %d usecs", total_gc_time / 10);
		
		/* this also sets the proper pointers for the next allocation */
		if (!mono_sgen_can_alloc_size (size)) {
			/* TypeBuilder and MonoMethod are killing mcs with fragmentation */
			DEBUG (1, fprintf (gc_debug_file, "nursery collection didn't find enough room for %zd alloc (%d pinned)\n", size, mono_sgen_get_pinned_count ()));
			mono_sgen_dump_pin_queue ();
			degraded_mode = 1;
		}
		mono_profiler_gc_event (MONO_GC_EVENT_END, 0);
	}
	//report_internal_mem_usage ();
}

void
mono_sgen_minor_collect_or_expand_inner (size_t size)
{
	minor_collect_or_expand_inner (size);
}

/*
 * ######################################################################
 * ########  Memory allocation from the OS
 * ######################################################################
 * This section of code deals with getting memory from the OS and
 * allocating memory for GC-internal data structures.
 * Internal memory can be handled with a freelist for small objects.
 */

/*
 * Debug reporting.
 */
G_GNUC_UNUSED static void
report_internal_mem_usage (void)
{
	printf ("Internal memory usage:\n");
	mono_sgen_report_internal_mem_usage ();
	printf ("Pinned memory usage:\n");
	major_collector.report_pinned_memory_usage ();
}

/*
 * ######################################################################
 * ########  Finalization support
 * ######################################################################
 */

/*
 * this is valid for the nursery: if the object has been forwarded it means it's
 * still refrenced from a root. If it is pinned it's still alive as well.
 * Return TRUE if @obj is ready to be finalized.
 */
#define object_is_fin_ready(obj) (!object_is_pinned (obj) && !object_is_forwarded (obj))


gboolean
mono_sgen_gc_is_object_ready_for_finalization (void *object)
{
	return !major_collector.is_object_live (object) && object_is_fin_ready (object);
}

static gboolean
has_critical_finalizer (MonoObject *obj)
{
	MonoClass *class;

	if (!mono_defaults.critical_finalizer_object)
		return FALSE;

	class = ((MonoVTable*)LOAD_VTABLE (obj))->klass;

	return mono_class_has_parent_fast (class, mono_defaults.critical_finalizer_object);
}

static void
queue_finalization_entry (MonoObject *obj) {
	FinalizeReadyEntry *entry = mono_sgen_alloc_internal (INTERNAL_MEM_FINALIZE_READY_ENTRY);
	entry->object = obj;
	if (has_critical_finalizer (obj)) {
		entry->next = critical_fin_list;
		critical_fin_list = entry;
	} else {
		entry->next = fin_ready_list;
		fin_ready_list = entry;
	}
}

static int
object_is_reachable (char *object, char *start, char *end)
{
	/*This happens for non nursery objects during minor collections. We just treat all objects as alive.*/
	if (object < start || object >= end)
		return TRUE;
	return !object_is_fin_ready (object) || major_collector.is_object_live (object);
}

#include "sgen-fin-weak-hash.c"

gboolean
mono_sgen_object_is_live (void *obj)
{
	if (ptr_in_nursery (obj))
		return object_is_pinned (obj);
	if (current_collection_generation == GENERATION_NURSERY)
		return FALSE;
	return major_collector.is_object_live (obj);
}

/* LOCKING: requires that the GC lock is held */
static void
null_ephemerons_for_domain (MonoDomain *domain)
{
	EphemeronLinkNode *current = ephemeron_list, *prev = NULL;

	while (current) {
		MonoObject *object = (MonoObject*)current->array;

		if (object && !object->vtable) {
			EphemeronLinkNode *tmp = current;

			if (prev)
				prev->next = current->next;
			else
				ephemeron_list = current->next;

			current = current->next;
			mono_sgen_free_internal (tmp, INTERNAL_MEM_EPHEMERON_LINK);
		} else {
			prev = current;
			current = current->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static void
clear_unreachable_ephemerons (CopyOrMarkObjectFunc copy_func, char *start, char *end, GrayQueue *queue)
{
	int was_in_nursery, was_promoted;
	EphemeronLinkNode *current = ephemeron_list, *prev = NULL;
	MonoArray *array;
	Ephemeron *cur, *array_end;
	char *tombstone;

	while (current) {
		char *object = current->array;

		if (!object_is_reachable (object, start, end)) {
			EphemeronLinkNode *tmp = current;

			DEBUG (5, fprintf (gc_debug_file, "Dead Ephemeron array at %p\n", object));

			if (prev)
				prev->next = current->next;
			else
				ephemeron_list = current->next;

			current = current->next;
			mono_sgen_free_internal (tmp, INTERNAL_MEM_EPHEMERON_LINK);

			continue;
		}

		was_in_nursery = ptr_in_nursery (object);
		copy_func ((void**)&object, queue);
		current->array = object;

		/*The array was promoted, add global remsets for key/values left behind in nursery.*/
		was_promoted = was_in_nursery && !ptr_in_nursery (object);

		DEBUG (5, fprintf (gc_debug_file, "Clearing unreachable entries for ephemeron array at %p\n", object));

		array = (MonoArray*)object;
		cur = mono_array_addr (array, Ephemeron, 0);
		array_end = cur + mono_array_length_fast (array);
		tombstone = (char*)((MonoVTable*)LOAD_VTABLE (object))->domain->ephemeron_tombstone;

		for (; cur < array_end; ++cur) {
			char *key = (char*)cur->key;

			if (!key || key == tombstone)
				continue;

			DEBUG (5, fprintf (gc_debug_file, "[%td] key %p (%s) value %p (%s)\n", cur - mono_array_addr (array, Ephemeron, 0),
				key, object_is_reachable (key, start, end) ? "reachable" : "unreachable",
				cur->value, cur->value && object_is_reachable (cur->value, start, end) ? "reachable" : "unreachable"));

			if (!object_is_reachable (key, start, end)) {
				cur->key = tombstone;
				cur->value = NULL;
				continue;
			}

			if (was_promoted) {
				if (ptr_in_nursery (key)) {/*key was not promoted*/
					DEBUG (5, fprintf (gc_debug_file, "\tAdded remset to key %p\n", key));
					mono_sgen_add_to_global_remset (&cur->key);
				}
				if (ptr_in_nursery (cur->value)) {/*value was not promoted*/
					DEBUG (5, fprintf (gc_debug_file, "\tAdded remset to value %p\n", cur->value));
					mono_sgen_add_to_global_remset (&cur->value);
				}
			}
		}
		prev = current;
		current = current->next;
	}
}

/* LOCKING: requires that the GC lock is held */
static int
mark_ephemerons_in_range (CopyOrMarkObjectFunc copy_func, char *start, char *end, GrayQueue *queue)
{
	int nothing_marked = 1;
	EphemeronLinkNode *current = ephemeron_list;
	MonoArray *array;
	Ephemeron *cur, *array_end;
	char *tombstone;

	for (current = ephemeron_list; current; current = current->next) {
		char *object = current->array;
		DEBUG (5, fprintf (gc_debug_file, "Ephemeron array at %p\n", object));

		/*
		For now we process all ephemerons during all collections.
		Ideally we should use remset information to partially scan those
		arrays.
		We already emit write barriers for Ephemeron fields, it's
		just that we don't process them.
		*/
		/*if (object < start || object >= end)
			continue;*/

		/*It has to be alive*/
		if (!object_is_reachable (object, start, end)) {
			DEBUG (5, fprintf (gc_debug_file, "\tnot reachable\n"));
			continue;
		}

		copy_func ((void**)&object, queue);

		array = (MonoArray*)object;
		cur = mono_array_addr (array, Ephemeron, 0);
		array_end = cur + mono_array_length_fast (array);
		tombstone = (char*)((MonoVTable*)LOAD_VTABLE (object))->domain->ephemeron_tombstone;

		for (; cur < array_end; ++cur) {
			char *key = cur->key;

			if (!key || key == tombstone)
				continue;

			DEBUG (5, fprintf (gc_debug_file, "[%td] key %p (%s) value %p (%s)\n", cur - mono_array_addr (array, Ephemeron, 0),
				key, object_is_reachable (key, start, end) ? "reachable" : "unreachable",
				cur->value, cur->value && object_is_reachable (cur->value, start, end) ? "reachable" : "unreachable"));

			if (object_is_reachable (key, start, end)) {
				char *value = cur->value;

				copy_func ((void**)&cur->key, queue);
				if (value) {
					if (!object_is_reachable (value, start, end))
						nothing_marked = 0;
					copy_func ((void**)&cur->value, queue);
				}
			}
		}
	}

	DEBUG (5, fprintf (gc_debug_file, "Ephemeron run finished. Is it done %d\n", nothing_marked));
	return nothing_marked;
}

int
mono_gc_invoke_finalizers (void)
{
	FinalizeReadyEntry *entry = NULL;
	gboolean entry_is_critical = FALSE;
	int count = 0;
	void *obj;
	/* FIXME: batch to reduce lock contention */
	while (fin_ready_list || critical_fin_list) {
		LOCK_GC;

		if (entry) {
			FinalizeReadyEntry **list = entry_is_critical ? &critical_fin_list : &fin_ready_list;

			/* We have finalized entry in the last
			   interation, now we need to remove it from
			   the list. */
			if (*list == entry)
				*list = entry->next;
			else {
				FinalizeReadyEntry *e = *list;
				while (e->next != entry)
					e = e->next;
				e->next = entry->next;
			}
			mono_sgen_free_internal (entry, INTERNAL_MEM_FINALIZE_READY_ENTRY);
			entry = NULL;
		}

		/* Now look for the first non-null entry. */
		for (entry = fin_ready_list; entry && !entry->object; entry = entry->next)
			;
		if (entry) {
			entry_is_critical = FALSE;
		} else {
			entry_is_critical = TRUE;
			for (entry = critical_fin_list; entry && !entry->object; entry = entry->next)
				;
		}

		if (entry) {
			g_assert (entry->object);
			num_ready_finalizers--;
			obj = entry->object;
			entry->object = NULL;
			DEBUG (7, fprintf (gc_debug_file, "Finalizing object %p (%s)\n", obj, safe_name (obj)));
		}

		UNLOCK_GC;

		if (!entry)
			break;

		g_assert (entry->object == NULL);
		count++;
		/* the object is on the stack so it is pinned */
		/*g_print ("Calling finalizer for object: %p (%s)\n", entry->object, safe_name (entry->object));*/
		mono_gc_run_finalize (obj, NULL);
	}
	g_assert (!entry);
	return count;
}

gboolean
mono_gc_pending_finalizers (void)
{
	return fin_ready_list || critical_fin_list;
}

/* Negative value to remove */
void
mono_gc_add_memory_pressure (gint64 value)
{
	/* FIXME: Use interlocked functions */
	LOCK_GC;
	memory_pressure += value;
	UNLOCK_GC;
}

void
mono_sgen_register_major_sections_alloced (int num_sections)
{
	minor_collection_sections_alloced += num_sections;
}

mword
mono_sgen_get_minor_collection_allowance (void)
{
	return minor_collection_allowance;
}

/*
 * ######################################################################
 * ########  registered roots support
 * ######################################################################
 */

/*
 * We do not coalesce roots.
 */
static int
mono_gc_register_root_inner (char *start, size_t size, void *descr, int root_type)
{
	RootRecord new_root;
	int i;
	LOCK_GC;
	for (i = 0; i < ROOT_TYPE_NUM; ++i) {
		RootRecord *root = mono_sgen_hash_table_lookup (&roots_hash [i], start);
		/* we allow changing the size and the descriptor (for thread statics etc) */
		if (root) {
			size_t old_size = root->end_root - start;
			root->end_root = start + size;
			g_assert (((root->root_desc != 0) && (descr != NULL)) ||
					  ((root->root_desc == 0) && (descr == NULL)));
			root->root_desc = (mword)descr;
			roots_size += size;
			roots_size -= old_size;
			UNLOCK_GC;
			return TRUE;
		}
	}

	new_root.end_root = start + size;
	new_root.root_desc = (mword)descr;

	mono_sgen_hash_table_replace (&roots_hash [root_type], start, &new_root);
	roots_size += size;

	DEBUG (3, fprintf (gc_debug_file, "Added root for range: %p-%p, descr: %p  (%d/%d bytes)\n", start, new_root.end_root, descr, (int)size, (int)roots_size));

	UNLOCK_GC;
	return TRUE;
}

int
mono_gc_register_root (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, descr ? ROOT_TYPE_NORMAL : ROOT_TYPE_PINNED);
}

int
mono_gc_register_root_wbarrier (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, ROOT_TYPE_WBARRIER);
}

void
mono_gc_deregister_root (char* addr)
{
	int root_type;
	RootRecord root;

	LOCK_GC;
	for (root_type = 0; root_type < ROOT_TYPE_NUM; ++root_type) {
		if (mono_sgen_hash_table_remove (&roots_hash [root_type], addr, &root))
			roots_size -= (root.end_root - addr);
	}
	UNLOCK_GC;
}

/*
 * ######################################################################
 * ########  Thread handling (stop/start code)
 * ######################################################################
 */

unsigned int mono_sgen_global_stop_count = 0;

#ifdef USE_MONO_CTX
static MonoContext cur_thread_ctx = {0};
#else
static mword cur_thread_regs [ARCH_NUM_REGS] = {0};
#endif

static void
update_current_thread_stack (void *start)
{
	int stack_guard = 0;
#ifndef USE_MONO_CTX
	void *ptr = cur_thread_regs;
#endif
	SgenThreadInfo *info = mono_thread_info_current ();
	
	info->stack_start = align_pointer (&stack_guard);
	g_assert (info->stack_start >= info->stack_start_limit && info->stack_start < info->stack_end);
#ifdef USE_MONO_CTX
	MONO_CONTEXT_GET_CURRENT (cur_thread_ctx);
	info->monoctx = &cur_thread_ctx;
#else
	ARCH_STORE_REGS (ptr);
	info->stopped_regs = ptr;
#endif
	if (gc_callbacks.thread_suspend_func)
		gc_callbacks.thread_suspend_func (info->runtime_data, NULL);
}

void
mono_sgen_fill_thread_info_for_suspend (SgenThreadInfo *info)
{
	if (remset.fill_thread_info_for_suspend)
		remset.fill_thread_info_for_suspend (info);
}

static gboolean
is_ip_in_managed_allocator (MonoDomain *domain, gpointer ip);

static int
restart_threads_until_none_in_managed_allocator (void)
{
	SgenThreadInfo *info;
	int num_threads_died = 0;
	int sleep_duration = -1;

	for (;;) {
		int restart_count = 0, restarted_count = 0;
		/* restart all threads that stopped in the
		   allocator */
		FOREACH_THREAD_SAFE (info) {
			gboolean result;
			if (info->skip || info->gc_disabled)
				continue;
			if (!info->thread_is_dying && (!info->stack_start || info->in_critical_region ||
					is_ip_in_managed_allocator (info->stopped_domain, info->stopped_ip))) {
				binary_protocol_thread_restart ((gpointer)mono_thread_info_get_tid (info));
				result = mono_sgen_resume_thread (info);
				if (result) {
					++restart_count;
				} else {
					info->skip = 1;
				}
			} else {
				/* we set the stopped_ip to
				   NULL for threads which
				   we're not restarting so
				   that we can easily identify
				   the others */
				info->stopped_ip = NULL;
				info->stopped_domain = NULL;
			}
		} END_FOREACH_THREAD_SAFE
		/* if no threads were restarted, we're done */
		if (restart_count == 0)
			break;

		/* wait for the threads to signal their restart */
		mono_sgen_wait_for_suspend_ack (restart_count);

		if (sleep_duration < 0) {
#ifdef HOST_WIN32
			SwitchToThread ();
#else
			sched_yield ();
#endif
			sleep_duration = 0;
		} else {
			g_usleep (sleep_duration);
			sleep_duration += 10;
		}

		/* stop them again */
		FOREACH_THREAD (info) {
			gboolean result;
			if (info->skip || info->stopped_ip == NULL)
				continue;
			result = mono_sgen_suspend_thread (info);

			if (result) {
				++restarted_count;
			} else {
				info->skip = 1;
			}
		} END_FOREACH_THREAD
		/* some threads might have died */
		num_threads_died += restart_count - restarted_count;
		/* wait for the threads to signal their suspension
		   again */
		mono_sgen_wait_for_suspend_ack (restart_count);
	}

	return num_threads_died;
}

static void
acquire_gc_locks (void)
{
	LOCK_INTERRUPTION;
	mono_thread_info_suspend_lock ();
}

static void
release_gc_locks (void)
{
	mono_thread_info_suspend_unlock ();
	UNLOCK_INTERRUPTION;
}

static TV_DECLARE (stop_world_time);
static unsigned long max_pause_usec = 0;

/* LOCKING: assumes the GC lock is held */
static int
stop_world (int generation)
{
	int count;

	/*XXX this is the right stop, thought might not be the nicest place to put it*/
	mono_sgen_process_togglerefs ();

	mono_profiler_gc_event (MONO_GC_EVENT_PRE_STOP_WORLD, generation);
	acquire_gc_locks ();

	update_current_thread_stack (&count);

	mono_sgen_global_stop_count++;
	DEBUG (3, fprintf (gc_debug_file, "stopping world n %d from %p %p\n", mono_sgen_global_stop_count, mono_thread_info_current (), (gpointer)mono_native_thread_id_get ()));
	TV_GETTIME (stop_world_time);
	count = mono_sgen_thread_handshake (TRUE);
	count -= restart_threads_until_none_in_managed_allocator ();
	g_assert (count >= 0);
	DEBUG (3, fprintf (gc_debug_file, "world stopped %d thread(s)\n", count));
	mono_profiler_gc_event (MONO_GC_EVENT_POST_STOP_WORLD, generation);

	last_major_num_sections = major_collector.get_num_major_sections ();
	last_los_memory_usage = los_memory_usage;
	major_collection_happened = FALSE;
	return count;
}

/* LOCKING: assumes the GC lock is held */
static int
restart_world (int generation)
{
	int count, num_major_sections;
	SgenThreadInfo *info;
	TV_DECLARE (end_sw);
	TV_DECLARE (end_bridge);
	unsigned long usec, bridge_usec;

	/* notify the profiler of the leftovers */
	if (G_UNLIKELY (mono_profiler_events & MONO_PROFILE_GC_MOVES)) {
		if (moved_objects_idx) {
			mono_profiler_gc_moves (moved_objects, moved_objects_idx);
			moved_objects_idx = 0;
		}
	}
	mono_profiler_gc_event (MONO_GC_EVENT_PRE_START_WORLD, generation);
	FOREACH_THREAD (info) {
		info->stack_start = NULL;
#ifdef USE_MONO_CTX
		info->monoctx = NULL;
#else
		info->stopped_regs = NULL;
#endif
	} END_FOREACH_THREAD

	stw_bridge_process ();
	release_gc_locks ();

	count = mono_sgen_thread_handshake (FALSE);
	TV_GETTIME (end_sw);
	usec = TV_ELAPSED (stop_world_time, end_sw);
	max_pause_usec = MAX (usec, max_pause_usec);
	DEBUG (2, fprintf (gc_debug_file, "restarted %d thread(s) (pause time: %d usec, max: %d)\n", count, (int)usec, (int)max_pause_usec));
	mono_profiler_gc_event (MONO_GC_EVENT_POST_START_WORLD, generation);

	bridge_process ();

	TV_GETTIME (end_bridge);
	bridge_usec = TV_ELAPSED (end_sw, end_bridge);

	num_major_sections = major_collector.get_num_major_sections ();
	if (major_collection_happened)
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR: %s pause %.2fms, bridge %.2fms major %dK/%dK los %dK/%dK",
			generation ? "" : "(minor overflow)",
			(int)usec / 1000.0f, (int)bridge_usec / 1000.0f,
			major_collector.section_size * num_major_sections / 1024,
			major_collector.section_size * last_major_num_sections / 1024,
			los_memory_usage / 1024,
			last_los_memory_usage / 1024);
	else
		mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MINOR: pause %.2fms, bridge %.2fms promoted %dK major %dK los %dK",
			(int)usec / 1000.0f, (int)bridge_usec / 1000.0f,
			(num_major_sections - last_major_num_sections) * major_collector.section_size / 1024,
			major_collector.section_size * num_major_sections / 1024,
			los_memory_usage / 1024);

	return count;
}

int
mono_sgen_get_current_collection_generation (void)
{
	return current_collection_generation;
}

void
mono_gc_set_gc_callbacks (MonoGCCallbacks *callbacks)
{
	gc_callbacks = *callbacks;
}

MonoGCCallbacks *
mono_gc_get_gc_callbacks ()
{
	return &gc_callbacks;
}

/* Variables holding start/end nursery so it won't have to be passed at every call */
static void *scan_area_arg_start, *scan_area_arg_end;

void
mono_gc_conservatively_scan_area (void *start, void *end)
{
	conservatively_pin_objects_from (start, end, scan_area_arg_start, scan_area_arg_end, PIN_TYPE_STACK);
}

void*
mono_gc_scan_object (void *obj)
{
	UserCopyOrMarkData *data = mono_native_tls_get_value (user_copy_or_mark_key);

	if (current_collection_generation == GENERATION_NURSERY) {
		if (mono_sgen_collection_is_parallel ())
			major_collector.copy_object (&obj, data->queue);
		else
			major_collector.nopar_copy_object (&obj, data->queue);
	} else {
		major_collector.copy_or_mark_object (&obj, data->queue);
	}
	return obj;
}

/*
 * Mark from thread stacks and registers.
 */
static void
scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise, GrayQueue *queue)
{
	SgenThreadInfo *info;

	scan_area_arg_start = start_nursery;
	scan_area_arg_end = end_nursery;

	FOREACH_THREAD (info) {
		if (info->skip) {
			DEBUG (3, fprintf (gc_debug_file, "Skipping dead thread %p, range: %p-%p, size: %td\n", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start));
			continue;
		}
		if (info->gc_disabled) {
			DEBUG (3, fprintf (gc_debug_file, "GC disabled for thread %p, range: %p-%p, size: %td\n", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start));
			continue;
		}
		DEBUG (3, fprintf (gc_debug_file, "Scanning thread %p, range: %p-%p, size: %ld, pinned=%d\n", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start, mono_sgen_get_pinned_count ()));
		if (!info->thread_is_dying) {
			if (gc_callbacks.thread_mark_func && !conservative_stack_mark) {
				UserCopyOrMarkData data = { NULL, queue };
				set_user_copy_or_mark_data (&data);
				gc_callbacks.thread_mark_func (info->runtime_data, info->stack_start, info->stack_end, precise);
				set_user_copy_or_mark_data (NULL);
			} else if (!precise) {
				conservatively_pin_objects_from (info->stack_start, info->stack_end, start_nursery, end_nursery, PIN_TYPE_STACK);
			}
		}

#ifdef USE_MONO_CTX
		if (!info->thread_is_dying && !precise)
			conservatively_pin_objects_from ((void**)info->monoctx, (void**)info->monoctx + ARCH_NUM_REGS,
				start_nursery, end_nursery, PIN_TYPE_STACK);
#else
		if (!info->thread_is_dying && !precise)
			conservatively_pin_objects_from (info->stopped_regs, info->stopped_regs + ARCH_NUM_REGS,
					start_nursery, end_nursery, PIN_TYPE_STACK);
#endif
	} END_FOREACH_THREAD
}

static void
find_pinning_ref_from_thread (char *obj, size_t size)
{
	int j;
	SgenThreadInfo *info;
	char *endobj = obj + size;

	FOREACH_THREAD (info) {
		char **start = (char**)info->stack_start;
		if (info->skip)
			continue;
		while (start < (char**)info->stack_end) {
			if (*start >= obj && *start < endobj) {
				DEBUG (0, fprintf (gc_debug_file, "Object %p referenced in thread %p (id %p) at %p, stack: %p-%p\n", obj, info, (gpointer)mono_thread_info_get_tid (info), start, info->stack_start, info->stack_end));
			}
			start++;
		}

		for (j = 0; j < ARCH_NUM_REGS; ++j) {
#ifdef USE_MONO_CTX
			mword w = ((mword*)info->monoctx) [j];
#else
			mword w = (mword)info->stopped_regs [j];
#endif

			if (w >= (mword)obj && w < (mword)obj + size)
				DEBUG (0, fprintf (gc_debug_file, "Object %p referenced in saved reg %d of thread %p (id %p)\n", obj, j, info, (gpointer)mono_thread_info_get_tid (info)));
		} END_FOREACH_THREAD
	}
}

static gboolean
ptr_on_stack (void *ptr)
{
	gpointer stack_start = &stack_start;
	SgenThreadInfo *info = mono_thread_info_current ();

	if (ptr >= stack_start && ptr < (gpointer)info->stack_end)
		return TRUE;
	return FALSE;
}

static void*
sgen_thread_register (SgenThreadInfo* info, void *addr)
{
#ifndef HAVE_KW_THREAD
	SgenThreadInfo *__thread_info__ = info;
#endif

	LOCK_GC;
#ifndef HAVE_KW_THREAD
	info->tlab_start = info->tlab_next = info->tlab_temp_end = info->tlab_real_end = NULL;

	g_assert (!mono_native_tls_get_value (thread_info_key));
	mono_native_tls_set_value (thread_info_key, info);
#else
	thread_info = info;
#endif

#if !defined(__MACH__)
	info->stop_count = -1;
	info->signal = 0;
#endif
	info->skip = 0;
	info->doing_handshake = FALSE;
	info->thread_is_dying = FALSE;
	info->stack_start = NULL;
	info->store_remset_buffer_addr = &STORE_REMSET_BUFFER;
	info->store_remset_buffer_index_addr = &STORE_REMSET_BUFFER_INDEX;
	info->stopped_ip = NULL;
	info->stopped_domain = NULL;
#ifdef USE_MONO_CTX
	info->monoctx = NULL;
#else
	info->stopped_regs = NULL;
#endif

	mono_sgen_init_tlab_info (info);

	binary_protocol_thread_register ((gpointer)mono_thread_info_get_tid (info));

#ifdef HAVE_KW_THREAD
	store_remset_buffer_index_addr = &store_remset_buffer_index;
#endif

#if defined(__MACH__)
	info->mach_port = mach_thread_self ();
#endif

	/* try to get it with attributes first */
#if defined(HAVE_PTHREAD_GETATTR_NP) && defined(HAVE_PTHREAD_ATTR_GETSTACK)
	{
		size_t size;
		void *sstart;
		pthread_attr_t attr;
		pthread_getattr_np (pthread_self (), &attr);
		pthread_attr_getstack (&attr, &sstart, &size);
		info->stack_start_limit = sstart;
		info->stack_end = (char*)sstart + size;
		pthread_attr_destroy (&attr);
	}
#elif defined(HAVE_PTHREAD_GET_STACKSIZE_NP) && defined(HAVE_PTHREAD_GET_STACKADDR_NP)
		 info->stack_end = (char*)pthread_get_stackaddr_np (pthread_self ());
		 info->stack_start_limit = (char*)info->stack_end - pthread_get_stacksize_np (pthread_self ());
#else
	{
		/* FIXME: we assume the stack grows down */
		gsize stack_bottom = (gsize)addr;
		stack_bottom += 4095;
		stack_bottom &= ~4095;
		info->stack_end = (char*)stack_bottom;
	}
#endif

#ifdef HAVE_KW_THREAD
	stack_end = info->stack_end;
#endif

	if (remset.register_thread)
		remset.register_thread (info);

	DEBUG (3, fprintf (gc_debug_file, "registered thread %p (%p) stack end %p\n", info, (gpointer)mono_thread_info_get_tid (info), info->stack_end));

	if (gc_callbacks.thread_attach_func)
		info->runtime_data = gc_callbacks.thread_attach_func ();

	UNLOCK_GC;
	return info;
}

static void
mono_sgen_wbarrier_cleanup_thread (SgenThreadInfo *p)
{
	if (remset.cleanup_thread)
		remset.cleanup_thread (p);
}

static void
sgen_thread_unregister (SgenThreadInfo *p)
{
	/* If a delegate is passed to native code and invoked on a thread we dont
	 * know about, the jit will register it with mono_jit_thread_attach, but
	 * we have no way of knowing when that thread goes away.  SGen has a TSD
	 * so we assume that if the domain is still registered, we can detach
	 * the thread
	 */
	if (mono_domain_get ())
		mono_thread_detach (mono_thread_current ());

	p->thread_is_dying = TRUE;

	/*
	There is a race condition between a thread finishing executing and been removed
	from the GC thread set.
	This happens on posix systems when TLS data is been cleaned-up, libpthread will
	set the thread_info slot to NULL before calling the cleanup function. This
	opens a window in which the thread is registered but has a NULL TLS.

	The suspend signal handler needs TLS data to know where to store thread state
	data or otherwise it will simply ignore the thread.

	This solution works because the thread doing STW will wait until all threads been
	suspended handshake back, so there is no race between the doing_hankshake test
	and the suspend_thread call.

	This is not required on systems that do synchronous STW as those can deal with
	the above race at suspend time.

	FIXME: I believe we could avoid this by using mono_thread_info_lookup when
	mono_thread_info_current returns NULL. Or fix mono_thread_info_lookup to do so.
	*/
#if (defined(__MACH__) && MONO_MACH_ARCH_SUPPORTED) || !defined(HAVE_PTHREAD_KILL)
	LOCK_GC;
#else
	while (!TRYLOCK_GC) {
		if (!mono_sgen_park_current_thread_if_doing_handshake (p))
			g_usleep (50);
	}
#endif

	binary_protocol_thread_unregister ((gpointer)mono_thread_info_get_tid (p));
	DEBUG (3, fprintf (gc_debug_file, "unregister thread %p (%p)\n", p, (gpointer)mono_thread_info_get_tid (p)));

#if defined(__MACH__)
	mach_port_deallocate (current_task (), p->mach_port);
#endif

	if (gc_callbacks.thread_detach_func) {
		gc_callbacks.thread_detach_func (p->runtime_data);
		p->runtime_data = NULL;
	}
	mono_sgen_wbarrier_cleanup_thread (p);

	mono_threads_unregister_current_thread (p);
	UNLOCK_GC;
}


static void
sgen_thread_attach (SgenThreadInfo *info)
{
	LOCK_GC;
	/*this is odd, can we get attached before the gc is inited?*/
	init_stats ();
	UNLOCK_GC;
	
	if (gc_callbacks.thread_attach_func && !info->runtime_data)
		info->runtime_data = gc_callbacks.thread_attach_func ();
}
gboolean
mono_gc_register_thread (void *baseptr)
{
	return mono_thread_info_attach (baseptr) != NULL;
}

/*
 * mono_gc_set_stack_end:
 *
 *   Set the end of the current threads stack to STACK_END. The stack space between 
 * STACK_END and the real end of the threads stack will not be scanned during collections.
 */
void
mono_gc_set_stack_end (void *stack_end)
{
	SgenThreadInfo *info;

	LOCK_GC;
	info = mono_thread_info_current ();
	if (info) {
		g_assert (stack_end < info->stack_end);
		info->stack_end = stack_end;
	}
	UNLOCK_GC;
}

#if USE_PTHREAD_INTERCEPT


int
mono_gc_pthread_create (pthread_t *new_thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	return pthread_create (new_thread, attr, start_routine, arg);
}

int
mono_gc_pthread_join (pthread_t thread, void **retval)
{
	return pthread_join (thread, retval);
}

int
mono_gc_pthread_detach (pthread_t thread)
{
	return pthread_detach (thread);
}

void
mono_gc_pthread_exit (void *retval)
{
	pthread_exit (retval);
}

#endif /* USE_PTHREAD_INTERCEPT */

/*
 * ######################################################################
 * ########  Write barriers
 * ######################################################################
 */

/*
 * Note: the write barriers first do the needed GC work and then do the actual store:
 * this way the value is visible to the conservative GC scan after the write barrier
 * itself. If a GC interrupts the barrier in the middle, value will be kept alive by
 * the conservative scan, otherwise by the remembered set scan.
 */
void
mono_gc_wbarrier_set_field (MonoObject *obj, gpointer field_ptr, MonoObject* value)
{
	HEAVY_STAT (++stat_wbarrier_set_field);
	if (ptr_in_nursery (field_ptr)) {
		*(void**)field_ptr = value;
		return;
	}
	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", field_ptr));
	if (value)
		binary_protocol_wbarrier (field_ptr, value, value->vtable);

	remset.wbarrier_set_field (obj, field_ptr, value);
}

void
mono_gc_wbarrier_set_arrayref (MonoArray *arr, gpointer slot_ptr, MonoObject* value)
{
	HEAVY_STAT (++stat_wbarrier_set_arrayref);
	if (ptr_in_nursery (slot_ptr)) {
		*(void**)slot_ptr = value;
		return;
	}
	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", slot_ptr));
	if (value)
		binary_protocol_wbarrier (slot_ptr, value, value->vtable);

	remset.wbarrier_set_arrayref (arr, slot_ptr, value);
}

void
mono_gc_wbarrier_arrayref_copy (gpointer dest_ptr, gpointer src_ptr, int count)
{
	HEAVY_STAT (++stat_wbarrier_arrayref_copy);
	/*This check can be done without taking a lock since dest_ptr array is pinned*/
	if (ptr_in_nursery (dest_ptr) || count <= 0) {
		mono_gc_memmove (dest_ptr, src_ptr, count * sizeof (gpointer));
		return;
	}

#ifdef SGEN_BINARY_PROTOCOL
	{
		int i;
		for (i = 0; i < count; ++i) {
			gpointer dest = (gpointer*)dest_ptr + i;
			gpointer obj = *((gpointer*)src_ptr + i);
			if (obj)
				binary_protocol_wbarrier (dest, obj, (gpointer)LOAD_VTABLE (obj));
		}
	}
#endif

	remset.wbarrier_arrayref_copy (dest_ptr, src_ptr, count);
}

static char *found_obj;

static void
find_object_for_ptr_callback (char *obj, size_t size, void *user_data)
{
	char *ptr = user_data;

	if (ptr >= obj && ptr < obj + size) {
		g_assert (!found_obj);
		found_obj = obj;
	}
}

/* for use in the debugger */
char* find_object_for_ptr (char *ptr);
char*
find_object_for_ptr (char *ptr)
{
	if (ptr >= nursery_section->data && ptr < nursery_section->end_data) {
		found_obj = NULL;
		mono_sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
				find_object_for_ptr_callback, ptr, TRUE);
		if (found_obj)
			return found_obj;
	}

	found_obj = NULL;
	mono_sgen_los_iterate_objects (find_object_for_ptr_callback, ptr);
	if (found_obj)
		return found_obj;

	/*
	 * Very inefficient, but this is debugging code, supposed to
	 * be called from gdb, so we don't care.
	 */
	found_obj = NULL;
	major_collector.iterate_objects (TRUE, TRUE, find_object_for_ptr_callback, ptr);
	return found_obj;
}

void
mono_gc_wbarrier_generic_nostore (gpointer ptr)
{
	HEAVY_STAT (++stat_wbarrier_generic_store);

#ifdef XDOMAIN_CHECKS_IN_WBARRIER
	/* FIXME: ptr_in_heap must be called with the GC lock held */
	if (xdomain_checks && *(MonoObject**)ptr && ptr_in_heap (ptr)) {
		char *start = find_object_for_ptr (ptr);
		MonoObject *value = *(MonoObject**)ptr;
		LOCK_GC;
		g_assert (start);
		if (start) {
			MonoObject *obj = (MonoObject*)start;
			if (obj->vtable->domain != value->vtable->domain)
				g_assert (is_xdomain_ref_allowed (ptr, start, obj->vtable->domain));
		}
		UNLOCK_GC;
	}
#endif

	if (*(gpointer*)ptr)
		binary_protocol_wbarrier (ptr, *(gpointer*)ptr, (gpointer)LOAD_VTABLE (*(gpointer*)ptr));

	if (ptr_in_nursery (ptr) || ptr_on_stack (ptr) || !ptr_in_nursery (*(gpointer*)ptr)) {
		DEBUG (8, fprintf (gc_debug_file, "Skipping remset at %p\n", ptr));
		return;
	}

	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", ptr));

	remset.wbarrier_generic_nostore (ptr);
}

void
mono_gc_wbarrier_generic_store (gpointer ptr, MonoObject* value)
{
	DEBUG (8, fprintf (gc_debug_file, "Wbarrier store at %p to %p (%s)\n", ptr, value, value ? safe_name (value) : "null"));
	*(void**)ptr = value;
	if (ptr_in_nursery (value))
		mono_gc_wbarrier_generic_nostore (ptr);
	mono_sgen_dummy_use (value);
}

void mono_gc_wbarrier_value_copy_bitmap (gpointer _dest, gpointer _src, int size, unsigned bitmap)
{
	mword *dest = _dest;
	mword *src = _src;

	while (size) {
		if (bitmap & 0x1)
			mono_gc_wbarrier_generic_store (dest, (MonoObject*)*src);
		else
			*dest = *src;
		++src;
		++dest;
		size -= SIZEOF_VOID_P;
		bitmap >>= 1;
	}
}

#ifdef SGEN_BINARY_PROTOCOL
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj) do {					\
		gpointer o = *(gpointer*)(ptr);				\
		if ((o)) {						\
			gpointer d = ((char*)dest) + ((char*)(ptr) - (char*)(obj)); \
			binary_protocol_wbarrier (d, o, (gpointer) LOAD_VTABLE (o)); \
		}							\
	} while (0)

static void
scan_object_for_binary_protocol_copy_wbarrier (gpointer dest, char *start, mword desc)
{
#define SCAN_OBJECT_NOVTABLE
#include "sgen-scan-object.h"
}
#endif

void
mono_gc_wbarrier_value_copy (gpointer dest, gpointer src, int count, MonoClass *klass)
{
	HEAVY_STAT (++stat_wbarrier_value_copy);
	g_assert (klass->valuetype);

	DEBUG (8, fprintf (gc_debug_file, "Adding value remset at %p, count %d, descr %p for class %s (%p)\n", dest, count, klass->gc_descr, klass->name, klass));

	if (ptr_in_nursery (dest) || ptr_on_stack (dest) || !SGEN_CLASS_HAS_REFERENCES (klass)) {
		size_t element_size = mono_class_value_size (klass, NULL);
		size_t size = count * element_size;
		mono_gc_memmove (dest, src, size);		
		return;
	}

#ifdef SGEN_BINARY_PROTOCOL
	{
		int i;
		for (i = 0; i < count; ++i) {
			scan_object_for_binary_protocol_copy_wbarrier ((char*)dest + i * element_size,
					(char*)src + i * element_size - sizeof (MonoObject),
					(mword) klass->gc_descr);
		}
	}
#endif

	remset.wbarrier_value_copy (dest, src, count, klass);
}

/**
 * mono_gc_wbarrier_object_copy:
 *
 * Write barrier to call when obj is the result of a clone or copy of an object.
 */
void
mono_gc_wbarrier_object_copy (MonoObject* obj, MonoObject *src)
{
	int size;

	HEAVY_STAT (++stat_wbarrier_object_copy);

	if (ptr_in_nursery (obj) || ptr_on_stack (obj)) {
		size = mono_object_class (obj)->instance_size;
		mono_gc_memmove ((char*)obj + sizeof (MonoObject), (char*)src + sizeof (MonoObject),
				size - sizeof (MonoObject));
		return;	
	}

#ifdef SGEN_BINARY_PROTOCOL
	scan_object_for_binary_protocol_copy_wbarrier (obj, (char*)src, (mword) src->vtable->gc_descr);
#endif

	remset.wbarrier_object_copy (obj, src);
}

/*
 * ######################################################################
 * ########  Other mono public interface functions.
 * ######################################################################
 */

#define REFS_SIZE 128
typedef struct {
	void *data;
	MonoGCReferences callback;
	int flags;
	int count;
	int called;
	MonoObject *refs [REFS_SIZE];
	uintptr_t offsets [REFS_SIZE];
} HeapWalkInfo;

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr)) {	\
			if (hwi->count == REFS_SIZE) {	\
				hwi->callback ((MonoObject*)start, mono_object_class (start), hwi->called? 0: size, hwi->count, hwi->refs, hwi->offsets, hwi->data);	\
				hwi->count = 0;	\
				hwi->called = 1;	\
			}	\
			hwi->offsets [hwi->count] = (char*)(ptr)-(char*)start;	\
			hwi->refs [hwi->count++] = *(ptr);	\
		}	\
	} while (0)

static void
collect_references (HeapWalkInfo *hwi, char *start, size_t size)
{
#include "sgen-scan-object.h"
}

static void
walk_references (char *start, size_t size, void *data)
{
	HeapWalkInfo *hwi = data;
	hwi->called = 0;
	hwi->count = 0;
	collect_references (hwi, start, size);
	if (hwi->count || !hwi->called)
		hwi->callback ((MonoObject*)start, mono_object_class (start), hwi->called? 0: size, hwi->count, hwi->refs, hwi->offsets, hwi->data);
}

/**
 * mono_gc_walk_heap:
 * @flags: flags for future use
 * @callback: a function pointer called for each object in the heap
 * @data: a user data pointer that is passed to callback
 *
 * This function can be used to iterate over all the live objects in the heap:
 * for each object, @callback is invoked, providing info about the object's
 * location in memory, its class, its size and the objects it references.
 * For each referenced object it's offset from the object address is
 * reported in the offsets array.
 * The object references may be buffered, so the callback may be invoked
 * multiple times for the same object: in all but the first call, the size
 * argument will be zero.
 * Note that this function can be only called in the #MONO_GC_EVENT_PRE_START_WORLD
 * profiler event handler.
 *
 * Returns: a non-zero value if the GC doesn't support heap walking
 */
int
mono_gc_walk_heap (int flags, MonoGCReferences callback, void *data)
{
	HeapWalkInfo hwi;

	hwi.flags = flags;
	hwi.callback = callback;
	hwi.data = data;

	mono_sgen_clear_nursery_fragments ();
	mono_sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data, walk_references, &hwi, FALSE);

	major_collector.iterate_objects (TRUE, TRUE, walk_references, &hwi);
	mono_sgen_los_iterate_objects (walk_references, &hwi);

	return 0;
}

void
mono_gc_collect (int generation)
{
	LOCK_GC;
	if (generation > 1)
		generation = 1;
	mono_profiler_gc_event (MONO_GC_EVENT_START, generation);
	stop_world (generation);
	if (generation == 0) {
		collect_nursery (0);
	} else {
		major_collection ("user request");
	}
	restart_world (generation);
	mono_profiler_gc_event (MONO_GC_EVENT_END, generation);
	UNLOCK_GC;
}

int
mono_gc_max_generation (void)
{
	return 1;
}

int
mono_gc_collection_count (int generation)
{
	if (generation == 0)
		return stat_minor_gcs;
	return stat_major_gcs;
}

int64_t
mono_gc_get_used_size (void)
{
	gint64 tot = 0;
	LOCK_GC;
	tot = los_memory_usage;
	tot += nursery_section->next_data - nursery_section->data;
	tot += major_collector.get_used_size ();
	/* FIXME: account for pinned objects */
	UNLOCK_GC;
	return tot;
}

int64_t
mono_gc_get_heap_size (void)
{
	return total_alloc;
}

void
mono_gc_disable (void)
{
	LOCK_GC;
	gc_disabled++;
	UNLOCK_GC;
}

void
mono_gc_enable (void)
{
	LOCK_GC;
	gc_disabled--;
	UNLOCK_GC;
}

int
mono_gc_get_los_limit (void)
{
	return MAX_SMALL_OBJ_SIZE;
}

gboolean
mono_object_is_alive (MonoObject* o)
{
	return TRUE;
}

int
mono_gc_get_generation (MonoObject *obj)
{
	if (ptr_in_nursery (obj))
		return 0;
	return 1;
}

void
mono_gc_enable_events (void)
{
}

void
mono_gc_weak_link_add (void **link_addr, MonoObject *obj, gboolean track)
{
	mono_gc_register_disappearing_link (obj, link_addr, track, FALSE);
}

void
mono_gc_weak_link_remove (void **link_addr)
{
	mono_gc_register_disappearing_link (NULL, link_addr, FALSE, FALSE);
}

MonoObject*
mono_gc_weak_link_get (void **link_addr)
{
	if (!*link_addr)
		return NULL;
	return (MonoObject*) REVEAL_POINTER (*link_addr);
}

gboolean
mono_gc_ephemeron_array_add (MonoObject *obj)
{
	EphemeronLinkNode *node;

	LOCK_GC;

	node = mono_sgen_alloc_internal (INTERNAL_MEM_EPHEMERON_LINK);
	if (!node) {
		UNLOCK_GC;
		return FALSE;
	}
	node->array = (char*)obj;
	node->next = ephemeron_list;
	ephemeron_list = node;

	DEBUG (5, fprintf (gc_debug_file, "Registered ephemeron array %p\n", obj));

	UNLOCK_GC;
	return TRUE;
}

void*
mono_gc_invoke_with_gc_lock (MonoGCLockedCallbackFunc func, void *data)
{
	void *result;
	LOCK_INTERRUPTION;
	result = func (data);
	UNLOCK_INTERRUPTION;
	return result;
}

gboolean
mono_gc_is_gc_thread (void)
{
	gboolean result;
	LOCK_GC;
	result = mono_thread_info_current () != NULL;
	UNLOCK_GC;
	return result;
}

static gboolean
is_critical_method (MonoMethod *method)
{
	return mono_runtime_is_critical_method (method) || mono_gc_is_critical_method (method);
}
	
void
mono_gc_base_init (void)
{
	MonoThreadInfoCallbacks cb;
	char *env;
	char **opts, **ptr;
	char *major_collector_opt = NULL;
	glong max_heap = 0;
	glong soft_limit = 0;
	int num_workers;
	int result;
	int dummy;

	do {
		result = InterlockedCompareExchange (&gc_initialized, -1, 0);
		switch (result) {
		case 1:
			/* already inited */
			return;
		case -1:
			/* being inited by another thread */
			g_usleep (1000);
			break;
		case 0:
			/* we will init it */
			break;
		default:
			g_assert_not_reached ();
		}
	} while (result != 0);

	LOCK_INIT (gc_mutex);

	pagesize = mono_pagesize ();
	gc_debug_file = stderr;

	cb.thread_register = sgen_thread_register;
	cb.thread_unregister = sgen_thread_unregister;
	cb.thread_attach = sgen_thread_attach;
	cb.mono_method_is_critical = (gpointer)is_critical_method;
#ifndef HOST_WIN32
	cb.mono_gc_pthread_create = (gpointer)mono_gc_pthread_create;
#endif

	mono_threads_init (&cb, sizeof (SgenThreadInfo));

	LOCK_INIT (interruption_mutex);
	LOCK_INIT (pin_queue_mutex);

	init_user_copy_or_mark_key ();

	if ((env = getenv ("MONO_GC_PARAMS"))) {
		opts = g_strsplit (env, ",", -1);
		for (ptr = opts; *ptr; ++ptr) {
			char *opt = *ptr;
			if (g_str_has_prefix (opt, "major=")) {
				opt = strchr (opt, '=') + 1;
				major_collector_opt = g_strdup (opt);
			}
		}
	} else {
		opts = NULL;
	}

	init_stats ();
	mono_sgen_init_internal_allocator ();
	mono_sgen_init_nursery_allocator ();

	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_SECTION, SGEN_SIZEOF_GC_MEM_SECTION);
	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_FINALIZE_READY_ENTRY, sizeof (FinalizeReadyEntry));
	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_GRAY_QUEUE, sizeof (GrayQueueSection));
	g_assert (sizeof (GenericStoreRememberedSet) == sizeof (gpointer) * STORE_REMSET_BUFFER_SIZE);
	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_STORE_REMSET, sizeof (GenericStoreRememberedSet));
	mono_sgen_register_fixed_internal_mem_type (INTERNAL_MEM_EPHEMERON_LINK, sizeof (EphemeronLinkNode));

#ifndef HAVE_KW_THREAD
	mono_native_tls_alloc (&thread_info_key, NULL);
#endif

	/*
	 * This needs to happen before any internal allocations because
	 * it inits the small id which is required for hazard pointer
	 * operations.
	 */
	mono_sgen_os_init ();

	mono_thread_info_attach (&dummy);

	if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep")) {
		mono_sgen_marksweep_init (&major_collector);
	} else if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep-fixed")) {
		mono_sgen_marksweep_fixed_init (&major_collector);
	} else if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep-par")) {
		mono_sgen_marksweep_par_init (&major_collector);
	} else if (!major_collector_opt || !strcmp (major_collector_opt, "marksweep-fixed-par")) {
		mono_sgen_marksweep_fixed_par_init (&major_collector);
	} else if (!strcmp (major_collector_opt, "copying")) {
		mono_sgen_copying_init (&major_collector);
	} else {
		fprintf (stderr, "Unknown major collector `%s'.\n", major_collector_opt);
		exit (1);
	}

#ifdef SGEN_HAVE_CARDTABLE
	use_cardtable = major_collector.supports_cardtable;
#else
	use_cardtable = FALSE;
#endif

	num_workers = mono_cpu_count ();
	g_assert (num_workers > 0);
	if (num_workers > 16)
		num_workers = 16;

	///* Keep this the default for now */
#ifdef __APPLE
	conservative_stack_mark = TRUE;
#endif

	if (opts) {
		for (ptr = opts; *ptr; ++ptr) {
			char *opt = *ptr;
			if (g_str_has_prefix (opt, "major="))
				continue;
			if (g_str_has_prefix (opt, "wbarrier=")) {
				opt = strchr (opt, '=') + 1;
				if (strcmp (opt, "remset") == 0) {
					use_cardtable = FALSE;
				} else if (strcmp (opt, "cardtable") == 0) {
					if (!use_cardtable) {
						if (major_collector.supports_cardtable)
							fprintf (stderr, "The cardtable write barrier is not supported on this platform.\n");
						else
							fprintf (stderr, "The major collector does not support the cardtable write barrier.\n");
						exit (1);
					}
				}
				continue;
			}
			if (g_str_has_prefix (opt, "max-heap-size=")) {
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &max_heap)) {
					if ((max_heap & (mono_pagesize () - 1))) {
						fprintf (stderr, "max-heap-size size must be a multiple of %d.\n", mono_pagesize ());
						exit (1);
					}
				} else {
					fprintf (stderr, "max-heap-size must be an integer.\n");
					exit (1);
				}
				continue;
			}
			if (g_str_has_prefix (opt, "soft-heap-limit=")) {
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &soft_limit)) {
					if (soft_limit <= 0) {
						fprintf (stderr, "soft-heap-limit must be positive.\n");
						exit (1);
					}
				} else {
					fprintf (stderr, "soft-heap-limit must be an integer.\n");
					exit (1);
				}
				continue;
			}
			if (g_str_has_prefix (opt, "workers=")) {
				long val;
				char *endptr;
				if (!major_collector.is_parallel) {
					fprintf (stderr, "The workers= option can only be used for parallel collectors.");
					exit (1);
				}
				opt = strchr (opt, '=') + 1;
				val = strtol (opt, &endptr, 10);
				if (!*opt || *endptr) {
					fprintf (stderr, "Cannot parse the workers= option value.");
					exit (1);
				}
				if (val <= 0 || val > 16) {
					fprintf (stderr, "The number of workers must be in the range 1 to 16.");
					exit (1);
				}
				num_workers = (int)val;
				continue;
			}
			if (g_str_has_prefix (opt, "stack-mark=")) {
				opt = strchr (opt, '=') + 1;
				if (!strcmp (opt, "precise")) {
					conservative_stack_mark = FALSE;
				} else if (!strcmp (opt, "conservative")) {
					conservative_stack_mark = TRUE;
				} else {
					fprintf (stderr, "Invalid value '%s' for stack-mark= option, possible values are: 'precise', 'conservative'.\n", opt);
					exit (1);
				}
				continue;
			}
			if (g_str_has_prefix (opt, "bridge=")) {
				opt = strchr (opt, '=') + 1;
				mono_sgen_register_test_bridge_callbacks (g_strdup (opt));
				continue;
			}
#ifdef USER_CONFIG
			if (g_str_has_prefix (opt, "nursery-size=")) {
				long val;
				opt = strchr (opt, '=') + 1;
				if (*opt && mono_gc_parse_environment_string_extract_number (opt, &val)) {
					mono_sgen_nursery_size = val;
#ifdef SGEN_ALIGN_NURSERY
					if ((val & (val - 1))) {
						fprintf (stderr, "The nursery size must be a power of two.\n");
						exit (1);
					}

					if (val < SGEN_MAX_NURSERY_WASTE) {
						fprintf (stderr, "The nursery size must be at least %d bytes.\n", SGEN_MAX_NURSERY_WASTE);
						exit (1);
					}

					mono_sgen_nursery_bits = 0;
					while (1 << (++ mono_sgen_nursery_bits) != mono_sgen_nursery_size)
						;
#endif
				} else {
					fprintf (stderr, "nursery-size must be an integer.\n");
					exit (1);
				}
				continue;
			}
#endif
			if (!(major_collector.handle_gc_param && major_collector.handle_gc_param (opt))) {
				fprintf (stderr, "MONO_GC_PARAMS must be a comma-delimited list of one or more of the following:\n");
				fprintf (stderr, "  max-heap-size=N (where N is an integer, possibly with a k, m or a g suffix)\n");
				fprintf (stderr, "  soft-heap-limit=n (where N is an integer, possibly with a k, m or a g suffix)\n");
				fprintf (stderr, "  nursery-size=N (where N is an integer, possibly with a k, m or a g suffix)\n");
				fprintf (stderr, "  major=COLLECTOR (where COLLECTOR is `marksweep', `marksweep-par' or `copying')\n");
				fprintf (stderr, "  wbarrier=WBARRIER (where WBARRIER is `remset' or `cardtable')\n");
				fprintf (stderr, "  stack-mark=MARK-METHOD (where MARK-METHOD is 'precise' or 'conservative')\n");
				if (major_collector.print_gc_param_usage)
					major_collector.print_gc_param_usage ();
				exit (1);
			}
		}
		g_strfreev (opts);
	}

	if (major_collector.is_parallel)
		mono_sgen_workers_init (num_workers);

	if (major_collector_opt)
		g_free (major_collector_opt);

	nursery_size = DEFAULT_NURSERY_SIZE;
	minor_collection_allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
	init_heap_size_limits (max_heap, soft_limit);

	alloc_nursery ();

	if ((env = getenv ("MONO_GC_DEBUG"))) {
		opts = g_strsplit (env, ",", -1);
		for (ptr = opts; ptr && *ptr; ptr ++) {
			char *opt = *ptr;
			if (opt [0] >= '0' && opt [0] <= '9') {
				gc_debug_level = atoi (opt);
				opt++;
				if (opt [0] == ':')
					opt++;
				if (opt [0]) {
					char *rf = g_strdup_printf ("%s.%d", opt, getpid ());
					gc_debug_file = fopen (rf, "wb");
					if (!gc_debug_file)
						gc_debug_file = stderr;
					g_free (rf);
				}
			} else if (!strcmp (opt, "print-allowance")) {
				debug_print_allowance = TRUE;
			} else if (!strcmp (opt, "print-pinning")) {
				do_pin_stats = TRUE;
			} else if (!strcmp (opt, "collect-before-allocs")) {
				collect_before_allocs = 1;
			} else if (g_str_has_prefix (opt, "collect-before-allocs=")) {
				char *arg = strchr (opt, '=') + 1;
				collect_before_allocs = atoi (arg);
			} else if (!strcmp (opt, "check-at-minor-collections")) {
				consistency_check_at_minor_collection = TRUE;
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "xdomain-checks")) {
				xdomain_checks = TRUE;
			} else if (!strcmp (opt, "clear-at-gc")) {
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "clear-nursery-at-gc")) {
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (!strcmp (opt, "check-scan-starts")) {
				do_scan_starts_check = TRUE;
			} else if (!strcmp (opt, "verify-nursery-at-minor-gc")) {
				do_verify_nursery = TRUE;
			} else if (!strcmp (opt, "dump-nursery-at-minor-gc")) {
				do_dump_nursery_content = TRUE;
			} else if (!strcmp (opt, "disable-minor")) {
				disable_minor_collections = TRUE;
			} else if (!strcmp (opt, "disable-major")) {
				disable_major_collections = TRUE;
			} else if (g_str_has_prefix (opt, "heap-dump=")) {
				char *filename = strchr (opt, '=') + 1;
				nursery_clear_policy = CLEAR_AT_GC;
				heap_dump_file = fopen (filename, "w");
				if (heap_dump_file) {
					fprintf (heap_dump_file, "<sgen-dump>\n");
					do_pin_stats = TRUE;
				}
#ifdef SGEN_BINARY_PROTOCOL
			} else if (g_str_has_prefix (opt, "binary-protocol=")) {
				char *filename = strchr (opt, '=') + 1;
				binary_protocol_init (filename);
				if (use_cardtable)
					fprintf (stderr, "Warning: Cardtable write barriers will not be binary-protocolled.\n");
#endif
			} else {
				fprintf (stderr, "Invalid format for the MONO_GC_DEBUG env variable: '%s'\n", env);
				fprintf (stderr, "The format is: MONO_GC_DEBUG=[l[:filename]|<option>]+ where l is a debug level 0-9.\n");
				fprintf (stderr, "Valid options are:\n");
				fprintf (stderr, "  collect-before-allocs[=<n>]\n");
				fprintf (stderr, "  check-at-minor-collections\n");
				fprintf (stderr, "  disable-minor\n");
				fprintf (stderr, "  disable-major\n");
				fprintf (stderr, "  xdomain-checks\n");
				fprintf (stderr, "  clear-at-gc\n");
				fprintf (stderr, "  print-allowance\n");
				fprintf (stderr, "  print-pinning\n");
				exit (1);
			}
		}
		g_strfreev (opts);
	}

	if (major_collector.is_parallel) {
		if (heap_dump_file) {
			fprintf (stderr, "Error: Cannot do heap dump with the parallel collector.\n");
			exit (1);
		}
		if (do_pin_stats) {
			fprintf (stderr, "Error: Cannot gather pinning statistics with the parallel collector.\n");
			exit (1);
		}
	}

	if (major_collector.post_param_init)
		major_collector.post_param_init ();

	memset (&remset, 0, sizeof (remset));

#ifdef SGEN_HAVE_CARDTABLE
	if (use_cardtable)
		sgen_card_table_init (&remset);
	else
#endif
		mono_sgen_ssb_init (&remset);

	if (remset.register_thread)
		remset.register_thread (mono_thread_info_current ());

	gc_initialized = 1;
}

const char *
mono_gc_get_gc_name (void)
{
	return "sgen";
}

static MonoMethod *write_barrier_method;

static gboolean
mono_gc_is_critical_method (MonoMethod *method)
{
	return (method == write_barrier_method || mono_sgen_is_managed_allocator (method));
}

static gboolean
is_ip_in_managed_allocator (MonoDomain *domain, gpointer ip)
{
	MonoJitInfo *ji;

	if (!mono_thread_internal_current ())
		/* Happens during thread attach */
		return FALSE;

	if (!ip || !domain)
		return FALSE;
	ji = mono_jit_info_table_find (domain, ip);
	if (!ji)
		return FALSE;

	return mono_gc_is_critical_method (ji->method);
}

static void
emit_nursery_check (MonoMethodBuilder *mb, int *nursery_check_return_labels)
{
	memset (nursery_check_return_labels, 0, sizeof (int) * 3);
#ifdef SGEN_ALIGN_NURSERY
	// if (ptr_in_nursery (ptr)) return;
	/*
	 * Masking out the bits might be faster, but we would have to use 64 bit
	 * immediates, which might be slower.
	 */
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_icon (mb, DEFAULT_NURSERY_BITS);
	mono_mb_emit_byte (mb, CEE_SHR_UN);
	mono_mb_emit_icon (mb, (mword)mono_sgen_get_nursery_start () >> DEFAULT_NURSERY_BITS);
	nursery_check_return_labels [0] = mono_mb_emit_branch (mb, CEE_BEQ);

	// if (!ptr_in_nursery (*ptr)) return;
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_byte (mb, CEE_LDIND_I);
	mono_mb_emit_icon (mb, DEFAULT_NURSERY_BITS);
	mono_mb_emit_byte (mb, CEE_SHR_UN);
	mono_mb_emit_icon (mb, (mword)mono_sgen_get_nursery_start () >> DEFAULT_NURSERY_BITS);
	nursery_check_return_labels [1] = mono_mb_emit_branch (mb, CEE_BNE_UN);
#else
	int label_continue1, label_continue2;
	int dereferenced_var;

	// if (ptr < (mono_sgen_get_nursery_start ())) goto continue;
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ptr (mb, (gpointer) mono_sgen_get_nursery_start ());
	label_continue_1 = mono_mb_emit_branch (mb, CEE_BLT);

	// if (ptr >= mono_sgen_get_nursery_end ())) goto continue;
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ptr (mb, (gpointer) mono_sgen_get_nursery_end ());
	label_continue_2 = mono_mb_emit_branch (mb, CEE_BGE);

	// Otherwise return
	nursery_check_return_labels [0] = mono_mb_emit_branch (mb, CEE_BR);

	// continue:
	mono_mb_patch_branch (mb, label_continue_1);
	mono_mb_patch_branch (mb, label_continue_2);

	// Dereference and store in local var
	dereferenced_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_byte (mb, CEE_LDIND_I);
	mono_mb_emit_stloc (mb, dereferenced_var);

	// if (*ptr < mono_sgen_get_nursery_start ()) return;
	mono_mb_emit_ldloc (mb, dereferenced_var);
	mono_mb_emit_ptr (mb, (gpointer) mono_sgen_get_nursery_start ());
	nursery_check_return_labels [1] = mono_mb_emit_branch (mb, CEE_BLT);

	// if (*ptr >= mono_sgen_get_nursery_end ()) return;
	mono_mb_emit_ldloc (mb, dereferenced_var);
	mono_mb_emit_ptr (mb, (gpointer) mono_sgen_get_nursery_end ());
	nursery_check_return_labels [2] = mono_mb_emit_branch (mb, CEE_BGE);
#endif	
}

MonoMethod*
mono_gc_get_write_barrier (void)
{
	MonoMethod *res;
	MonoMethodBuilder *mb;
	MonoMethodSignature *sig;
#ifdef MANAGED_WBARRIER
	int i, nursery_check_labels [3];
	int label_no_wb_3, label_no_wb_4, label_need_wb, label_slow_path;
	int buffer_var, buffer_index_var, dummy_var;

#ifdef HAVE_KW_THREAD
	int stack_end_offset = -1, store_remset_buffer_offset = -1;
	int store_remset_buffer_index_offset = -1, store_remset_buffer_index_addr_offset = -1;

	MONO_THREAD_VAR_OFFSET (stack_end, stack_end_offset);
	g_assert (stack_end_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer, store_remset_buffer_offset);
	g_assert (store_remset_buffer_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer_index, store_remset_buffer_index_offset);
	g_assert (store_remset_buffer_index_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer_index_addr, store_remset_buffer_index_addr_offset);
	g_assert (store_remset_buffer_index_addr_offset != -1);
#endif
#endif

	// FIXME: Maybe create a separate version for ctors (the branch would be
	// correctly predicted more times)
	if (write_barrier_method)
		return write_barrier_method;

	/* Create the IL version of mono_gc_barrier_generic_store () */
	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 1);
	sig->ret = &mono_defaults.void_class->byval_arg;
	sig->params [0] = &mono_defaults.int_class->byval_arg;

	mb = mono_mb_new (mono_defaults.object_class, "wbarrier", MONO_WRAPPER_WRITE_BARRIER);

#ifdef MANAGED_WBARRIER
	if (use_cardtable) {
		emit_nursery_check (mb, nursery_check_labels);
		/*
		addr = sgen_cardtable + ((address >> CARD_BITS) & CARD_MASK)
		*addr = 1;

		sgen_cardtable: 
			LDC_PTR sgen_cardtable

		address >> CARD_BITS
			LDARG_0
			LDC_I4 CARD_BITS
			SHR_UN
		if (SGEN_HAVE_OVERLAPPING_CARDS) {
			LDC_PTR card_table_mask
			AND
		}
		AND
		ldc_i4_1
		stind_i1
		*/
		mono_mb_emit_ptr (mb, sgen_cardtable);
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icon (mb, CARD_BITS);
		mono_mb_emit_byte (mb, CEE_SHR_UN);
#ifdef SGEN_HAVE_OVERLAPPING_CARDS
		mono_mb_emit_ptr (mb, (gpointer)CARD_MASK);
		mono_mb_emit_byte (mb, CEE_AND);
#endif
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_icon (mb, 1);
		mono_mb_emit_byte (mb, CEE_STIND_I1);

		// return;
		for (i = 0; i < 3; ++i) {
			if (nursery_check_labels [i])
				mono_mb_patch_branch (mb, nursery_check_labels [i]);
		}		
		mono_mb_emit_byte (mb, CEE_RET);
	} else if (mono_runtime_has_tls_get ()) {
		emit_nursery_check (mb, nursery_check_labels);

		// if (ptr >= stack_end) goto need_wb;
		mono_mb_emit_ldarg (mb, 0);
		EMIT_TLS_ACCESS (mb, stack_end, stack_end_offset);
		label_need_wb = mono_mb_emit_branch (mb, CEE_BGE_UN);

		// if (ptr >= stack_start) return;
		dummy_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_ldloc_addr (mb, dummy_var);
		label_no_wb_3 = mono_mb_emit_branch (mb, CEE_BGE_UN);

		// need_wb:
		mono_mb_patch_branch (mb, label_need_wb);

		// buffer = STORE_REMSET_BUFFER;
		buffer_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
		EMIT_TLS_ACCESS (mb, store_remset_buffer, store_remset_buffer_offset);
		mono_mb_emit_stloc (mb, buffer_var);

		// buffer_index = STORE_REMSET_BUFFER_INDEX;
		buffer_index_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
		EMIT_TLS_ACCESS (mb, store_remset_buffer_index, store_remset_buffer_index_offset);
		mono_mb_emit_stloc (mb, buffer_index_var);

		// if (buffer [buffer_index] == ptr) return;
		mono_mb_emit_ldloc (mb, buffer_var);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		g_assert (sizeof (gpointer) == 4 || sizeof (gpointer) == 8);
		mono_mb_emit_icon (mb, sizeof (gpointer) == 4 ? 2 : 3);
		mono_mb_emit_byte (mb, CEE_SHL);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_byte (mb, CEE_LDIND_I);
		mono_mb_emit_ldarg (mb, 0);
		label_no_wb_4 = mono_mb_emit_branch (mb, CEE_BEQ);

		// ++buffer_index;
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_icon (mb, 1);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_stloc (mb, buffer_index_var);

		// if (buffer_index >= STORE_REMSET_BUFFER_SIZE) goto slow_path;
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_icon (mb, STORE_REMSET_BUFFER_SIZE);
		label_slow_path = mono_mb_emit_branch (mb, CEE_BGE);

		// buffer [buffer_index] = ptr;
		mono_mb_emit_ldloc (mb, buffer_var);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		g_assert (sizeof (gpointer) == 4 || sizeof (gpointer) == 8);
		mono_mb_emit_icon (mb, sizeof (gpointer) == 4 ? 2 : 3);
		mono_mb_emit_byte (mb, CEE_SHL);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_byte (mb, CEE_STIND_I);

		// STORE_REMSET_BUFFER_INDEX = buffer_index;
		EMIT_TLS_ACCESS (mb, store_remset_buffer_index_addr, store_remset_buffer_index_addr_offset);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_byte (mb, CEE_STIND_I);

		// return;
		for (i = 0; i < 3; ++i) {
			if (nursery_check_labels [i])
				mono_mb_patch_branch (mb, nursery_check_labels [i]);
		}
		mono_mb_patch_branch (mb, label_no_wb_3);
		mono_mb_patch_branch (mb, label_no_wb_4);
		mono_mb_emit_byte (mb, CEE_RET);

		// slow path
		mono_mb_patch_branch (mb, label_slow_path);

		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icall (mb, mono_gc_wbarrier_generic_nostore);
		mono_mb_emit_byte (mb, CEE_RET);
	} else
#endif
	{
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icall (mb, mono_gc_wbarrier_generic_nostore);
		mono_mb_emit_byte (mb, CEE_RET);
	}

	res = mono_mb_create_method (mb, sig, 16);
	mono_mb_free (mb);

	mono_loader_lock ();
	if (write_barrier_method) {
		/* Already created */
		mono_free_method (res);
	} else {
		/* double-checked locking */
		mono_memory_barrier ();
		write_barrier_method = res;
	}
	mono_loader_unlock ();

	return write_barrier_method;
}

char*
mono_gc_get_description (void)
{
	return g_strdup ("sgen");
}

void
mono_gc_set_desktop_mode (void)
{
}

gboolean
mono_gc_is_moving (void)
{
	return TRUE;
}

gboolean
mono_gc_is_disabled (void)
{
	return FALSE;
}

void
mono_sgen_debug_printf (int level, const char *format, ...)
{
	va_list ap;

	if (level > gc_debug_level)
		return;

	va_start (ap, format);
	vfprintf (gc_debug_file, format, ap);
	va_end (ap);
}

FILE*
mono_sgen_get_logfile (void)
{
	return gc_debug_file;
}

#ifdef HOST_WIN32
BOOL APIENTRY mono_gc_dllmain (HMODULE module_handle, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
#endif

NurseryClearPolicy
mono_sgen_get_nursery_clear_policy (void)
{
	return nursery_clear_policy;
}

MonoVTable*
mono_sgen_get_array_fill_vtable (void)
{
	if (!array_fill_vtable) {
		static MonoClass klass;
		static MonoVTable vtable;
		gsize bmap;

		MonoDomain *domain = mono_get_root_domain ();
		g_assert (domain);

		klass.element_class = mono_defaults.byte_class;
		klass.rank = 1;
		klass.instance_size = sizeof (MonoArray);
		klass.sizes.element_size = 1;
		klass.name = "array_filler_type";

		vtable.klass = &klass;
		bmap = 0;
		vtable.gc_descr = mono_gc_make_descr_for_array (TRUE, &bmap, 0, 1);
		vtable.rank = 1;

		array_fill_vtable = &vtable;
	}
	return array_fill_vtable;
}

void
mono_sgen_gc_lock (void)
{
	LOCK_GC;
}

void
mono_sgen_gc_unlock (void)
{
	UNLOCK_GC;
}

void
sgen_major_collector_iterate_live_block_ranges (sgen_cardtable_block_callback callback)
{
	major_collector.iterate_live_block_ranges (callback);
}

void
sgen_major_collector_scan_card_table (SgenGrayQueue *queue)
{
	major_collector.scan_card_table (queue);
}

SgenMajorCollector*
mono_sgen_get_major_collector (void)
{
	return &major_collector;
}

void mono_gc_set_skip_thread (gboolean skip)
{
	SgenThreadInfo *info = mono_thread_info_current ();

	LOCK_GC;
	info->gc_disabled = skip;
	UNLOCK_GC;
}

SgenRemeberedSet*
mono_sgen_get_remset (void)
{
	return &remset;
}

guint
mono_gc_get_vtable_bits (MonoClass *class)
{
	if (mono_sgen_need_bridge_processing () && mono_sgen_is_bridge_class (class))
		return SGEN_GC_BIT_BRIDGE_OBJECT;
	return 0;
}

#endif /* HAVE_SGEN_GC */
