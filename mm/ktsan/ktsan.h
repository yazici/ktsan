#ifndef __X86_MM_KTSAN_KTSAN_H
#define __X86_MM_KTSAN_KTSAN_H

#include <linux/ktsan.h>
#include <linux/list.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/mm.h>

#define KT_DEBUG 0
#define KT_DEBUG_TRACE 0
#define KT_ENABLE_STATS 0

#define KT_GRAIN 8
#define KT_SHADOW_SLOTS_LOG 2
#define KT_SHADOW_SLOTS (1 << KT_SHADOW_SLOTS_LOG)
#define KT_SHADOW_TO_LONG(shadow) (*(long *)(&shadow))

/* Logarithms of access sizes, used in shadow encoding. */
#define KT_ACCESS_SIZE_1 0
#define KT_ACCESS_SIZE_2 1
#define KT_ACCESS_SIZE_4 2
#define KT_ACCESS_SIZE_8 3

#define KT_THREAD_ID_BITS 12
#define KT_CLOCK_BITS 42

#define KT_SYNC_TAB_SIZE 196613
#define KT_MEMBLOCK_TAB_SIZE 196613

#define KT_MAX_SYNC_COUNT (1700 * 1000)
#define KT_MAX_MEMBLOCK_COUNT (200 * 1000)
#define KT_MAX_PERCPU_SYNC_COUNT (30 * 1000)

#define KT_MAX_TASK_COUNT 1024
#define KT_MAX_THREAD_COUNT KT_MAX_TASK_COUNT

#define KT_MAX_STACK_FRAMES 96
#define KT_TAME_COUNTER_LIMIT 3
#define KT_MAX_LOCKED_MTX 32

#define KT_STACK_DEPOT_PARTS 196613
/* Can't be more than 16 GB because offset divided by 4 is stored in uint32 */
#define KT_STACK_DEPOT_MEMORY_LIMIT (512 << 20)

#define KT_TRACE_PARTS 8
#define KT_TRACE_PART_SIZE (8 * 1024)
#define KT_TRACE_SIZE (KT_TRACE_PARTS * KT_TRACE_PART_SIZE)

/* For use on performance-critical paths. */
#if KT_DEBUG
#define KT_BUG_ON(x) BUG_ON(x)
#else
#define KT_BUG_ON(x) {}
#endif

typedef unsigned long			uptr_t;
typedef unsigned long			kt_time_t;

typedef struct kt_thr_s			kt_thr_t;
typedef struct kt_clk_s			kt_clk_t;
typedef struct kt_tab_s			kt_tab_t;
typedef struct kt_tab_obj_s		kt_tab_obj_t;
typedef struct kt_tab_part_s		kt_tab_part_t;
typedef struct kt_tab_sync_s		kt_tab_sync_t;
typedef struct kt_tab_memblock_s	kt_tab_memblock_t;
typedef struct kt_tab_lock_s		kt_tab_lock_t;
typedef struct kt_tab_test_s		kt_tab_test_t;
typedef struct kt_ctx_s			kt_ctx_t;
typedef enum kt_stat_e			kt_stat_t;
typedef struct kt_stats_s		kt_stats_t;
typedef struct kt_cpu_s			kt_cpu_t;
typedef struct kt_task_s		kt_task_t;
typedef struct kt_race_info_s		kt_race_info_t;
typedef struct kt_cache_s		kt_cache_t;
typedef struct kt_stack_s		kt_stack_t;
typedef u32				kt_stack_handle_t;
typedef struct kt_stack_depot_s		kt_stack_depot_t;
typedef struct kt_stack_depot_obj_s	kt_stack_depot_obj_t;
typedef enum kt_event_type_e		kt_event_type_t;
typedef struct kt_event_s		kt_event_t;
typedef struct kt_locked_mutex_s	kt_locked_mutex_t;
typedef struct kt_mutexset_s		kt_mutexset_t;
typedef struct kt_trace_state_s		kt_trace_state_t;
typedef struct kt_trace_part_header_s	kt_trace_part_header_t;
typedef struct kt_trace_s		kt_trace_t;
typedef struct kt_id_manager_s		kt_id_manager_t;
typedef struct kt_thr_pool_s		kt_thr_pool_t;
typedef struct kt_shadow_s		kt_shadow_t;
typedef struct kt_percpu_sync_s		kt_percpu_sync_t;
typedef struct kt_spinlock_s		kt_spinlock_t;
typedef struct kt_interrupted_s		kt_interrupted_t;

/* Ktsan runtime internal, non-instrumented spinlock. */

struct kt_spinlock_s {
	u8			state;
};

/* Internal allocator. */

struct kt_cache_s {
	unsigned long		base;
	unsigned long		mem_size;
	void			*head;
	kt_spinlock_t		lock;
};

/* Stack. */

struct kt_stack_s {
	s32			size;
	u32			pc[KT_MAX_STACK_FRAMES];
};

/* Stack depot. */

struct kt_stack_depot_obj_s {
	kt_stack_depot_obj_t	*link;
	u32			hash;
	s32			stack_size;
	u32			stack_pc[0];
};

struct kt_stack_depot_s {
	kt_cache_t		stack_cache;
	u64			stack_offset;
	unsigned long		nstacks;
	kt_stack_depot_obj_t	*parts[KT_STACK_DEPOT_PARTS];
	kt_spinlock_t		lock;
};

/* Trace. */

enum kt_event_type_e {
	kt_event_nop,
	kt_event_mop, /* memory operation */
	kt_event_func_enter,
	kt_event_func_exit,
	kt_event_thr_start,
	kt_event_thr_stop,
	kt_event_lock,
	kt_event_rlock,
	kt_event_unlock,
	kt_event_runlock,
	kt_event_interrupt,
#if KT_DEBUG
	kt_event_acquire,
	kt_event_release,
	kt_event_nonmat_acquire,
	kt_event_nonmat_release,
	kt_event_membar_acquire,
	kt_event_membar_release,
	kt_event_preempt_enable,
	kt_event_preempt_disable,
	kt_event_irq_enable,
	kt_event_irq_disable,
	kt_event_event_disable,
	kt_event_event_enable,
#endif /* KT_DEBUG */
};

struct kt_event_s {
	u64			type : 16;
	u64			data : 48;
	/* The data field is
	   cpu id for thread start and stop events,
	   sync uid for lock and unlock events,
	   and pc for other kinds of event. */
};

struct kt_locked_mutex_s {
	u64			uid;
	kt_stack_handle_t	stack;
	bool			write;
};

struct kt_mutexset_s {
	kt_locked_mutex_t	mtx[KT_MAX_LOCKED_MTX];
	int			size;
};

struct kt_trace_state_s {
	kt_stack_t		stack;
	kt_mutexset_t		mutexset;
	int			pid;
	int			cpu_id;
};

struct kt_trace_part_header_s {
	kt_trace_state_t	state;
	kt_time_t		clock;
};

struct kt_trace_s {
	kt_trace_part_header_t	headers[KT_TRACE_PARTS];
	kt_event_t		events[KT_TRACE_SIZE];
	kt_spinlock_t		lock;
};

/* Clocks. */

struct kt_clk_s {
	kt_time_t		time[KT_MAX_THREAD_COUNT];
};

/* Shadow. */

struct kt_shadow_s {
	unsigned long tid	: KT_THREAD_ID_BITS;
	unsigned long clock	: KT_CLOCK_BITS;
	unsigned long offset	: 3;
	unsigned long size	: 2;
	unsigned long read	: 1;
	unsigned long atomic	: 1;
};

/* Reports. */

struct kt_race_info_s {
	unsigned long		addr;
	kt_shadow_t		old;
	kt_shadow_t		new;
};

/* Hash table. */

struct kt_tab_obj_s {
	kt_spinlock_t		lock;
	kt_tab_obj_t		*link;
	uptr_t			key;
};

struct kt_tab_part_s {
	kt_spinlock_t		lock;
	kt_tab_obj_t		*head;
};

struct kt_tab_s {
	unsigned		size;
	unsigned		objsize;
	kt_tab_part_t		*parts;
	kt_cache_t		obj_cache;
	kt_cache_t		parts_cache;
};

struct kt_tab_sync_s {
	kt_tab_obj_t		tab;
	u64			uid;
	kt_clk_t		clk;
	int			lock_tid; /* id of thread that locked mutex */
	struct list_head	list;
	uptr_t			pc;
	kt_time_t		last_lock_time;
	kt_time_t		last_unlock_time;
};

struct kt_tab_lock_s {
	kt_tab_obj_t		tab;
	kt_spinlock_t		lock;
	struct list_head	list;
};

struct kt_tab_memblock_s {
	kt_tab_obj_t		tab;
	struct list_head	sync_list;
	struct list_head	lock_list;
};

struct kt_tab_test_s {
	kt_tab_obj_t tab;
	unsigned long data[4];
};

/* Threads. */

struct kt_thr_s {
	int			id;
	int			pid;
	unsigned long		inside;	/* already inside of ktsan runtime */
	kt_cpu_t		*cpu;
	kt_clk_t		clk;
	kt_stack_t		stack;
	kt_mutexset_t		mutexset;
	kt_clk_t		acquire_clk;
	int			acquire_active;
	kt_clk_t		release_clk;
	int			release_active;
	kt_trace_t		trace;
	int			read_disable_depth;
	int			event_disable_depth;
	int			report_disable_depth;
	int			preempt_disable_depth;
	bool			irqs_disabled;
	unsigned long		irq_flags_before_disable;
	struct list_head	quarantine_list; /* list entry */
	struct list_head	percpu_list; /* list head */
	/* List of currently "acquired" for reading seqcounts. */
	uptr_t			seqcount[6];
	/* Where the seqcounts were acquired (for debugging). */
	uptr_t			seqcount_pc[6];
	/* Ignore of all seqcount-related events. */
	int			seqcount_ignore;
	int			interrupt_depth;
#if KT_DEBUG
	kt_time_t		last_event_disable_time;
	kt_time_t		last_event_enable_time;
#endif
};

/* Holds state of an interrupted thread while it executes interrupts.
 * Essentially a subset of kt_thr_t state.
 */
struct kt_interrupted_s {
	kt_thr_t		*thr;
	kt_stack_t		stack;
	kt_mutexset_t		mutexset;
	kt_clk_t		acquire_clk;
	int			acquire_active;
	kt_clk_t		release_clk;
	int			release_active;
	int			read_disable_depth;
	int			report_disable_depth;
	int			preempt_disable_depth;
	struct list_head	percpu_list;
	uptr_t			seqcount[6];
	uptr_t			seqcount_pc[6];
	int			seqcount_ignore;
};

struct kt_thr_pool_s {
	kt_cache_t		cache;
	kt_thr_t		*thrs[KT_MAX_THREAD_COUNT];
	int			new_id;
	int			new_pid;
	struct list_head	quarantine;
	int			quarantine_size;
	kt_spinlock_t		lock;
};

/* Per-cpu synchronization. */

struct kt_percpu_sync_s {
	uptr_t addr;
	struct list_head list;
};

/* Statistics. */

enum kt_stat_e {
	kt_stat_reports,
	kt_stat_access_read,
	kt_stat_access_write,
	kt_stat_access_size1,
	kt_stat_access_size2,
	kt_stat_access_size4,
	kt_stat_access_size8,
	kt_stat_sync_objects,
	kt_stat_sync_alloc,
	kt_stat_sync_free,
	kt_stat_memblock_objects,
	kt_stat_memblock_alloc,
	kt_stat_memblock_free,
	kt_stat_threads,
	kt_stat_thread_create,
	kt_stat_thread_destroy,
	kt_stat_acquire,
	kt_stat_release,
	kt_stat_func_entry,
	kt_stat_func_exit,
	kt_stat_trace_event,
	kt_stat_count,
};

struct kt_stats_s {
	unsigned long		stat[kt_stat_count];
};

/* KTSAN per-cpu state. */

struct kt_cpu_s {
	/* Thread that currently runs on the CPU or NULL. */
	kt_thr_t		*thr;
	kt_stats_t		stat;
	u64			sync_uid_pos;
	u64			sync_uid_end;
	kt_interrupted_t	interrupted;
};

/* KTSAN per-task state. */

struct kt_task_s {
	/* Thread that is associated with this task. Never NULL. */
	kt_thr_t		*thr;
	/* Shows whether this task is being executed. */
	bool			running;
};

/* Global. */

struct kt_ctx_s {
	int			enabled;
	kt_cpu_t __percpu	*cpus;
	kt_cache_t		task_cache;
	kt_tab_t		sync_tab; /* sync addr -> sync object */
	kt_tab_t		memblock_tab; /* memory block -> sync objects */
	kt_tab_t		test_tab;
	kt_cache_t		percpu_sync_cache;
	kt_thr_pool_t		thr_pool;
	kt_stack_depot_t	stack_depot;
	u64			sync_uid_gen;
};

extern kt_ctx_t kt_ctx;

/* Statistics. Enabled only when KT_ENABLE_STATS = 1. */

void kt_stat_init(void);

static inline void kt_stat_add(kt_stat_t what, unsigned long x)
{
#if KT_ENABLE_STATS
	this_cpu_ptr(kt_ctx.cpus)->stat.stat[what] += x;
#endif
}

static inline void kt_stat_inc(kt_stat_t what)
{
	kt_stat_add(what, 1);
}

static inline void kt_stat_dec(kt_stat_t what)
{
	kt_stat_add(what, -1);
}

/* Stack. */

/* All kernel addresses have 0xffff in high 2 bytes (on x86_64). */
#define KT_ADDR_MASK	0xffff000000000000ull
#define KT_PC_MASK	0xffffffff00000000ull

static inline u64 kt_compress(u64 addr)
{
	KT_BUG_ON((addr | KT_ADDR_MASK) != addr);
	return addr & ~KT_ADDR_MASK;
}

static inline u64 kt_decompress(u64 addr)
{
	KT_BUG_ON((addr & KT_ADDR_MASK) != 0);
	if (addr & KT_PC_MASK)
		return addr | KT_ADDR_MASK;
	/* This must be a PC with cut off high part. */
	return addr | KT_PC_MASK;
}

static __always_inline void kt_stack_init(kt_stack_t *stack)
{
	stack->size = 0;
}

static __always_inline void kt_stack_push(kt_stack_t *stack, u32 pc)
{
	BUG_ON(stack->size + 1 >= KT_MAX_STACK_FRAMES);
	stack->pc[stack->size++] = pc;
}

static __always_inline u32 kt_stack_pop(kt_stack_t *stack)
{
	BUG_ON(stack->size <= 0);
	return stack->pc[--stack->size];
}

void kt_stack_copy(kt_stack_t *dst, kt_stack_t *src);
void kt_stack_print(kt_stack_t *stack, uptr_t top_pc);

#if KT_DEBUG
void kt_stack_print_current(unsigned long strip_addr);
void kt_stack_save_current(kt_stack_t *stack, unsigned long strip_addr);
#endif

/* Stack depot. */

void kt_stack_depot_init(kt_stack_depot_t *depot);
kt_stack_handle_t kt_stack_depot_save(kt_stack_depot_t *depot,
					kt_stack_t *stack);
kt_stack_t *kt_stack_depot_get(kt_stack_depot_t *depot,
				kt_stack_handle_t handle);
void kt_stack_depot_stats(kt_stack_depot_t *depot, unsigned long *nstacks,
	unsigned long *memory);

/* Clocks. */

void kt_clk_init(kt_clk_t *clk);
void kt_clk_acquire(kt_clk_t *dst, kt_clk_t *src);
void kt_clk_set(kt_clk_t *dst, kt_clk_t *src);

static __always_inline
kt_time_t kt_clk_get(kt_clk_t *clk, int tid)
{
	KT_BUG_ON(tid >= KT_MAX_THREAD_COUNT);
	return clk->time[tid];
}

static __always_inline
void kt_clk_tick(kt_clk_t *clk, int tid)
{
	KT_BUG_ON(tid >= KT_MAX_THREAD_COUNT);
	clk->time[tid]++;
}

/* Trace. */

void kt_trace_init(kt_trace_t *trace);
void kt_trace_switch(kt_thr_t *thr);
void kt_trace_restore_state(kt_thr_t *thr, kt_time_t clock,
				kt_trace_state_t *state);
void kt_trace_dump(kt_trace_t *trace, unsigned long beg, unsigned long end);
u64 kt_trace_last_data(kt_thr_t *thr);

/* Adds the event to the thread trace.
 * Only 48 low bits of data are saved, use kt_compress if you need to save
 * addresses or pcs.
 */
static inline
void kt_trace_add_event(kt_thr_t *thr, kt_event_type_t type, u64 data)
{
	kt_trace_t *trace;
	kt_time_t clock;
	kt_event_t event;
	unsigned pos;

	kt_stat_inc(kt_stat_trace_event);

	kt_clk_tick(&thr->clk, thr->id);

	trace = &thr->trace;
	clock = kt_clk_get(&thr->clk, thr->id);
	pos = clock % KT_TRACE_SIZE;

	if ((pos % KT_TRACE_PART_SIZE) == 0)
		kt_trace_switch(thr);

	event.type = type;
	event.data = data;
	BUG_ON(event.data != data);
	trace->events[pos] = event;
}

/* Same as kt_trace_add_event but saves 2 data items to trace.
 * Data is still stripped to 48 bits, but data2 is saved entirely.
 * The function ensures that the two words do not cross part boundary.
 * It is responsibility of kt_trace_follow to deal with both data items.
 */
void kt_trace_add_event2(kt_thr_t *thr, kt_event_type_t type, u64 data,
	u64 data2);

/* Spinlock. */

void kt_spin_init(kt_spinlock_t *l);
void kt_spin_lock(kt_spinlock_t *l);
void kt_spin_unlock(kt_spinlock_t *l);
int kt_spin_is_locked(kt_spinlock_t *l);

/* Shadow. */

static __always_inline
void *kt_shadow_get(uptr_t addr)
{
	struct page *page;
	unsigned long aligned_addr;
	unsigned long shadow_offset;

	if (unlikely(addr < (unsigned long)(__va(0)) ||
			addr >= (unsigned long)(__va(max_pfn << PAGE_SHIFT))))
		return NULL;

	/* XXX: kmemcheck checks something about pte here. */

	page = virt_to_page(addr);
	if (unlikely(!page->shadow))
		return NULL;

	aligned_addr = round_down(addr, KT_GRAIN);
	shadow_offset = (aligned_addr & (PAGE_SIZE - 1)) * KT_SHADOW_SLOTS;
	return page->shadow + shadow_offset;
}

void kt_shadow_clear(uptr_t addr, size_t size);

extern unsigned long kt_shadow_pages;


/* Threads. */

void kt_thr_pool_init(void);

kt_thr_t *kt_thr_create(kt_thr_t *thr, int pid);
void kt_thr_destroy(kt_thr_t *thr, kt_thr_t *old);
kt_thr_t *kt_thr_get(int id);

void kt_thr_start(kt_thr_t *thr, uptr_t pc);
void kt_thr_stop(kt_thr_t *thr, uptr_t pc);

void kt_thr_interrupt(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state);
void kt_thr_resume(kt_thr_t *thr, uptr_t pc, kt_interrupted_t *state);

bool kt_thr_event_disable(kt_thr_t *thr, uptr_t pc, unsigned long *flags);
bool kt_thr_event_enable(kt_thr_t *thr, uptr_t pc, unsigned long *flags);
void kt_thr_report_disable(kt_thr_t *thr);
void kt_thr_report_enable(kt_thr_t *thr);

/* Synchronization. */

kt_tab_sync_t *kt_sync_ensure_created(kt_thr_t *thr, uptr_t pc, uptr_t addr);
void kt_sync_free(kt_thr_t *thr, uptr_t addr);

void kt_sync_acquire(kt_thr_t *thr, uptr_t pc, uptr_t addr);
void kt_sync_release(kt_thr_t *thr, uptr_t pc, uptr_t addr);

void kt_acquire(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync);
void kt_release(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync);

void kt_mtx_pre_lock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr, bool try);
void kt_mtx_post_lock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr, bool try,
		      bool success);
void kt_mtx_pre_unlock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr);
void kt_mtx_post_unlock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr);
void kt_mtx_downgrade(kt_thr_t *thr, uptr_t pc, uptr_t addr);

void kt_mutexset_init(kt_mutexset_t *s);
void kt_mutexset_lock(kt_mutexset_t *s, u64 uid, kt_stack_handle_t h, bool wr);
void kt_mutexset_unlock(kt_mutexset_t *set, u64 uid, bool wr);

void kt_mutex_lock(kt_thr_t *thr, uptr_t pc, u64 sync_uid, bool write);
void kt_mutex_unlock(kt_thr_t *thr, u64 sync_uid, bool write);
void kt_mutex_downgrade(kt_thr_t *thr, u64 sync_uid);

void kt_seqcount_begin(kt_thr_t *thr, uptr_t pc, uptr_t addr);
void kt_seqcount_end(kt_thr_t *thr, uptr_t pc, uptr_t addr);
void kt_seqcount_ignore_begin(kt_thr_t *thr, uptr_t pc);
void kt_seqcount_ignore_end(kt_thr_t *thr, uptr_t pc);
void kt_seqcount_bug(kt_thr_t *thr, uptr_t addr, const char *what);

void kt_thread_fence(kt_thr_t *thr, uptr_t pc, ktsan_memory_order_t mo);

void kt_atomic8_store(kt_thr_t *thr, uptr_t pc,
		void *addr, u8 value, ktsan_memory_order_t mo);
void kt_atomic16_store(kt_thr_t *thr, uptr_t pc,
		void *addr, u16 value, ktsan_memory_order_t mo);
void kt_atomic32_store(kt_thr_t *thr, uptr_t pc,
		void *addr, u32 value, ktsan_memory_order_t mo);
void kt_atomic64_store(kt_thr_t *thr, uptr_t pc,
		void *addr, u64 value, ktsan_memory_order_t mo);

u8 kt_atomic8_load(kt_thr_t *thr, uptr_t pc,
		const void *addr, ktsan_memory_order_t mo);
u16 kt_atomic16_load(kt_thr_t *thr, uptr_t pc,
		const void *addr, ktsan_memory_order_t mo);
u32 kt_atomic32_load(kt_thr_t *thr, uptr_t pc,
		const void *addr, ktsan_memory_order_t mo);
u64 kt_atomic64_load(kt_thr_t *thr, uptr_t pc,
		const void *addr, ktsan_memory_order_t mo);

u8 kt_atomic8_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u8 value, ktsan_memory_order_t mo);
u16 kt_atomic16_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u16 value, ktsan_memory_order_t mo);
u32 kt_atomic32_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u32 value, ktsan_memory_order_t mo);
u64 kt_atomic64_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u64 value, ktsan_memory_order_t mo);

u8 kt_atomic8_compare_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u8 old, u8 new, ktsan_memory_order_t mo);
u16 kt_atomic16_compare_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u16 old, u16 new, ktsan_memory_order_t mo);
u32 kt_atomic32_compare_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u32 old, u32 new, ktsan_memory_order_t mo);
u64 kt_atomic64_compare_exchange(kt_thr_t *thr, uptr_t pc,
		void *addr, u64 old, u64 new, ktsan_memory_order_t mo);

u8 kt_atomic8_fetch_add(kt_thr_t *thr, uptr_t pc,
		void *addr, u8 value, ktsan_memory_order_t mo);
u16 kt_atomic16_fetch_add(kt_thr_t *thr, uptr_t pc,
		void *addr, u16 value, ktsan_memory_order_t mo);
u32 kt_atomic32_fetch_add(kt_thr_t *thr, uptr_t pc,
		void *addr, u32 value, ktsan_memory_order_t mo);
u64 kt_atomic64_fetch_add(kt_thr_t *thr, uptr_t pc,
		void *addr, u64 value, ktsan_memory_order_t mo);

void kt_atomic_set_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);
void kt_atomic_clear_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);
void kt_atomic_change_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);

int kt_atomic_fetch_set_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);
int kt_atomic_fetch_clear_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);
int kt_atomic_fetch_change_bit(kt_thr_t *thr, uptr_t pc,
		void *addr, long nr, ktsan_memory_order_t mo);

void kt_thread_fence_no_ktsan(ktsan_memory_order_t mo);

static __always_inline
u8 kt_atomic8_load_no_ktsan(const void *addr)
{
	return *(volatile u8 *)addr;
}

static __always_inline
u16 kt_atomic16_load_no_ktsan(const void *addr)
{
	return *(volatile u16 *)addr;
}

static __always_inline
u32 kt_atomic32_load_no_ktsan(const void *addr)
{
	return *(volatile u32 *)addr;
}

static __always_inline
u64 kt_atomic64_load_no_ktsan(const void *addr)
{
	return *(volatile u64 *)addr;
}

static __always_inline
void kt_atomic8_store_no_ktsan(void *addr, u8 value)
{
	*(volatile u8 *)addr = value;
}

static __always_inline
void kt_atomic16_store_no_ktsan(void *addr, u16 value)
{
	*(volatile u16 *)addr = value;
}

static __always_inline
void kt_atomic32_store_no_ktsan(void *addr, u32 value)
{
	*(volatile u32 *)addr = value;
}

static __always_inline
void kt_atomic64_store_no_ktsan(void *addr, u64 value)
{
	*(volatile u64 *)addr = value;
}

u8 kt_atomic8_exchange_no_ktsan(void *addr, u8 value);
u16 kt_atomic16_exchange_no_ktsan(void *addr, u16 value);
u32 kt_atomic32_exchange_no_ktsan(void *addr, u32 value);
u64 kt_atomic64_exchange_no_ktsan(void *addr, u64 value);

u8 kt_atomic8_compare_exchange_no_ktsan(void *addr, u8 old, u8 new);
u16 kt_atomic16_compare_exchange_no_ktsan(void *addr, u16 old, u16 new);
u32 kt_atomic32_compare_exchange_no_ktsan(void *addr, u32 old, u32 new);
u64 kt_atomic64_compare_exchange_no_ktsan(void *addr, u64 old, u64 new);

u8 kt_atomic8_fetch_add_no_ktsan(void *addr, u8 value);
u16 kt_atomic16_fetch_add_no_ktsan(void *addr, u16 value);
u32 kt_atomic32_fetch_add_no_ktsan(void *addr, u32 value);
u64 kt_atomic64_fetch_add_no_ktsan(void *addr, u64 value);

void kt_atomic_set_bit_no_ktsan(void *addr, long nr);
void kt_atomic_clear_bit_no_ktsan(void *addr, long nr);
void kt_atomic_change_bit_no_ktsan(void *addr, long nr);

int kt_atomic_fetch_set_bit_no_ktsan(void *addr, long nr);
int kt_atomic_fetch_clear_bit_no_ktsan(void *addr, long nr);
int kt_atomic_fetch_change_bit_no_ktsan(void *addr, long nr);

/* Per-cpu synchronization. */

void kt_preempt_add(kt_thr_t *thr, uptr_t pc, int value);
void kt_preempt_sub(kt_thr_t *thr, uptr_t pc, int value);

void kt_irq_disable(kt_thr_t *thr, uptr_t pc);
void kt_irq_enable(kt_thr_t *thr, uptr_t pc);
void kt_irq_save(kt_thr_t *thr, uptr_t pc);
void kt_irq_restore(kt_thr_t *thr, uptr_t pc, unsigned long flags);

void kt_percpu_acquire(kt_thr_t *thr, uptr_t pc, uptr_t addr);
void kt_percpu_release(kt_thr_t *thr, uptr_t pc);

/* Memory block allocation. */

uptr_t kt_memblock_addr(uptr_t addr);
void kt_memblock_add_sync(kt_thr_t *thr, uptr_t addr, kt_tab_sync_t *sync);
void kt_memblock_remove_sync(kt_thr_t *thr, uptr_t addr, kt_tab_sync_t *sync);
void kt_memblock_alloc(kt_thr_t *thr, uptr_t pc, uptr_t addr, size_t size,
			bool write_to_shadow);
void kt_memblock_free(kt_thr_t *thr, uptr_t pc, uptr_t addr, size_t size,
			bool write_to_shadow);

/* For usage in ktsan_free_page, which doesn't use ENTER / LEAVE. */
void ktsan_memblock_free(void *addr, unsigned long size, bool write_to_shadow);

/* Generic memory access. */

void kt_access(kt_thr_t *thr, uptr_t pc, uptr_t addr,
		size_t size, bool read, bool atomic);
void kt_access_range(kt_thr_t *thr, uptr_t pc, uptr_t addr,
			size_t size, bool read);

void kt_access_range_imitate(kt_thr_t *thr, uptr_t pc, uptr_t addr,
				size_t size, bool read);

/* Function tracing. */

void kt_func_entry(kt_thr_t *thr, uptr_t pc);
void kt_func_exit(kt_thr_t *thr);

/* Reports. */

void kt_report_race(kt_thr_t *thr, kt_race_info_t *info);
void kt_report_bad_mtx_unlock(kt_thr_t *thr, uptr_t pc, kt_tab_sync_t *sync);
void kt_report_sync_usage(void);

#if KT_DEBUG
void kt_report_sync_usage(void);
#endif /* KT_DEBUG */

/* Suppressions. */
void kt_supp_init(void);
bool kt_supp_suppressed(unsigned long pc);

/* Internal allocator. */

void kt_cache_init(kt_cache_t *cache, size_t obj_size, size_t obj_max_num);
void kt_cache_destroy(kt_cache_t *cache);
void *kt_cache_alloc(kt_cache_t *cache);
void kt_cache_free(kt_cache_t *cache, void *obj);

/*
 * Hash table. Maps an address to an arbitrary object.
 * The object must start with kt_tab_obj_t.
 */

void kt_tab_init(kt_tab_t *tab, unsigned size,
		 unsigned obj_size, unsigned obj_max_num);
void kt_tab_destroy(kt_tab_t *tab);
void *kt_tab_access(kt_tab_t *tab, uptr_t key, bool *created, bool destroy);

/* Tests. */

void kt_tests_init(void);
void kt_tests_run_noinst(void);
void kt_tests_run_inst(void);
void kt_tests_run(void);

#endif /* __X86_MM_KTSAN_KTSAN_H */
