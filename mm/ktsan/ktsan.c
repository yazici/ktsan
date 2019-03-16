#include "ktsan.h"

#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/nmi.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>

int ktsan_glob_sync[ktsan_glob_sync_type_count];
EXPORT_SYMBOL(ktsan_glob_sync);

kt_ctx_t kt_ctx;

static inline kt_cpu_t *kt_current_cpu(void)
{
	return this_cpu_ptr(kt_ctx.cpus);
}

static inline kt_task_t *kt_current_task(void)
{
	KT_BUG_ON(!current);
	return current->ktsan.task;
}

#define DISABLE_INTERRUPTS(flags)	\
	preempt_disable();		\
	flags = arch_local_irq_save();	\
	stop_nmi()			\
/**/

#define ENABLE_INTERRUPTS(flags)	\
	restart_nmi();			\
	arch_local_irq_restore(flags);	\
	preempt_enable()		\
/**/

#define IN_INTERRUPT()			\
	(in_nmi())			\
/**/

/* Nothing special. */
#define KT_ENTER_NORMAL		0
/* Handle events that come from the scheduler internals. */
#define KT_ENTER_SCHED		(1<<0)
/* Handle events even if events were disabled with ktsan_disable(). */
#define KT_ENTER_DISABLED	(1<<1)

#define ENTER(enter_flags)						\
	kt_task_t *task;						\
	kt_thr_t *thr;							\
	uptr_t pc;							\
	unsigned long kt_flags;						\
	bool event_handled;						\
									\
	thr = NULL;							\
	event_handled = false;						\
									\
	DISABLE_INTERRUPTS(kt_flags);					\
									\
	if (unlikely(!kt_ctx.enabled))					\
		goto exit;						\
									\
	/* Ignore NMIs for now. */					\
	if (unlikely(IN_INTERRUPT()))					\
		goto exit;						\
									\
	task = kt_current_task();					\
	if (unlikely(!task))						\
		goto exit;						\
									\
	if (unlikely(!task->running &&					\
			!((enter_flags) & KT_ENTER_SCHED))) 		\
		goto exit;						\
									\
	thr = task->thr;						\
	KT_BUG_ON(!thr);						\
									\
	if (unlikely(thr->event_disable_depth != 0 &&			\
			!((enter_flags) & KT_ENTER_DISABLED)))		\
		goto exit;						\
									\
	if (unlikely(__test_and_set_bit(0, &thr->inside)))		\
		goto exit;						\
									\
	pc = (uptr_t)_RET_IP_;						\
	event_handled = true;						\
/**/

#define LEAVE()								\
	KT_BUG_ON(task != current->ktsan.task);				\
	if (unlikely(!__test_and_clear_bit(0, &thr->inside)))		\
		KT_BUG_ON(1);						\
									\
exit:									\
	KT_BUG_ON(thr && event_handled && thr->inside);			\
	ENABLE_INTERRUPTS(kt_flags);					\
/**/

void __init ktsan_init_early(void)
{
	kt_ctx_t *ctx = &kt_ctx;

	memset(ctx, 0, sizeof(*ctx));
	kt_tab_init(&ctx->sync_tab, KT_SYNC_TAB_SIZE,
		    sizeof(kt_tab_sync_t), KT_MAX_SYNC_COUNT);
	kt_tab_init(&ctx->memblock_tab, KT_MEMBLOCK_TAB_SIZE,
		    sizeof(kt_tab_memblock_t), KT_MAX_MEMBLOCK_COUNT);
	kt_tab_init(&ctx->test_tab, 13, sizeof(kt_tab_test_t), 20);

	kt_cache_init(&ctx->percpu_sync_cache,
		      sizeof(kt_percpu_sync_t), KT_MAX_PERCPU_SYNC_COUNT);
	kt_cache_init(&ctx->task_cache, sizeof(kt_task_t), KT_MAX_TASK_COUNT);

	kt_thr_pool_init();

	kt_stack_depot_init(&ctx->stack_depot);
}

static void ktsan_report_memory_usage(void)
{
	u64 sync_tab_mem = KT_SYNC_TAB_SIZE * sizeof(kt_tab_part_t);
	u64 sync_cache_mem = KT_MAX_SYNC_COUNT * sizeof(kt_tab_sync_t);
	u64 sync_total_mem = sync_tab_mem + sync_cache_mem;

	u64 memblock_tab_mem = KT_MEMBLOCK_TAB_SIZE * sizeof(kt_tab_part_t);
	u64 memblock_cache_mem = KT_MAX_MEMBLOCK_COUNT *
					sizeof(kt_tab_memblock_t);
	u64 memblock_total_mem = memblock_tab_mem + memblock_cache_mem;

	u64 percpu_sync_cache_mem = KT_MAX_PERCPU_SYNC_COUNT *
					sizeof(kt_percpu_sync_t);
	u64 task_cache_mem = KT_MAX_TASK_COUNT * sizeof(kt_task_t);

	u64 thr_cache_mem = KT_MAX_THREAD_COUNT * sizeof(kt_thr_t);

	u64 depot_objs_mem = KT_STACK_DEPOT_MEMORY_LIMIT;

	u64 total_mem = sync_total_mem + memblock_total_mem + task_cache_mem +
		percpu_sync_cache_mem + thr_cache_mem + depot_objs_mem;

	pr_err("ktsan memory usage: %llu GB + shadow.\n", total_mem >> 30);
	pr_err("             syncs: %llu MB \n", sync_total_mem >> 20);
	pr_err("          memblock: %llu MB \n", memblock_total_mem >> 20);
	pr_err("      percpu syncs: %llu MB\n", percpu_sync_cache_mem >> 20);
	pr_err("             tasks: %llu MB\n", task_cache_mem >> 20);
	pr_err("      thrs (trace): %llu MB\n", thr_cache_mem >> 20);
	pr_err("       stack depot: %llu MB\n", depot_objs_mem >> 20);
}

void ktsan_init(void)
{
	kt_ctx_t *ctx;
	kt_cpu_t *cpu;
	kt_task_t *task;
	kt_thr_t *thr;
	int inside, i;

	ctx = &kt_ctx;

	ctx->cpus = alloc_percpu(kt_cpu_t);
	for_each_possible_cpu(i) {
		cpu = per_cpu_ptr(ctx->cpus, i);
		memset(cpu, 0, sizeof(*cpu));
	}

	thr = kt_thr_create(NULL, current->pid);
	task = kt_cache_alloc(&kt_ctx.task_cache);
	task->thr = thr;
	current->ktsan.task = task;

	BUG_ON(ctx->enabled);
	inside = __test_and_set_bit(0, &thr->inside);
	BUG_ON(inside != 0);

	kt_stat_init();
	kt_supp_init();
	kt_tests_init();

	inside = __test_and_clear_bit(0, &thr->inside);
	BUG_ON(!inside);
	ctx->enabled = 1;

	pr_err("KTSAN: enabled\n");
	ktsan_report_memory_usage();
}

void ktsan_print_diagnostics(void)
{
	ENTER(KT_ENTER_DISABLED);
	LEAVE();

	pr_err("# # # # # # # # # # ktsan diagnostics # # # # # # # # # #\n");
	pr_err("\n");

#if KT_DEBUG_TRACE
	if (thr != NULL) {
		kt_time_t time;

		time = kt_clk_get(&thr->clk, thr->id);
		pr_err("Trace:\n");
		kt_trace_dump(&thr->trace, (time - 512) % KT_TRACE_SIZE, time);
		pr_err("\n");
	}
#endif /* KT_DEBUG_TRACE */

	pr_err("Runtime:\n");
	pr_err(" runtime active:                %s\n", event_handled ? "+" : "-");
	if (!event_handled) {
		pr_err(" kt_ctx.enabled:                %s\n",
			(kt_ctx.enabled) ? "+" : "-");
		pr_err(" !IN_INTERRUPT():               %s\n",
			(!IN_INTERRUPT()) ? "+" : "-");
		pr_err(" current:                       %s\n",
			(current) ? "+" : "-");
		pr_err(" current->ktsan.task:           %s\n",
			(current->ktsan.task) ? "+" : "-");
		if (thr != NULL) {
			pr_err(" thr->event_disable_depth == 0: %s\n",
				(thr->event_disable_depth == 0) ? "+" : "-");
			pr_err(" thr->cpu != NULL:              %s\n",
				(thr->cpu != NULL) ? "+" : "-");
		}
	}

	pr_err("\n");

	if (thr != NULL) {
		pr_err("Thread:\n");
		pr_err(" thr->id:                    %d\n", thr->id);
		pr_err(" thr->pid:                   %d\n", thr->pid);
		pr_err(" thr->inside:                %d\n",
			kt_atomic32_load_no_ktsan((void *)&thr->inside));
		pr_err(" thr->report_disable_depth:  %d\n",
			thr->report_disable_depth);
		pr_err(" thr->preempt_disable_depth: %d\n",
			thr->preempt_disable_depth);
		pr_err(" thr->event_disable_depth:   %d\n",
			thr->event_disable_depth);
		pr_err(" thr->irqs_disabled:         %s\n",
			thr->irqs_disabled ? "+" : "-");
		pr_err("\n");
	}
	pr_err("\n");

#if KT_DEBUG
	if (thr != NULL) {
		kt_trace_state_t state;

		pr_err("Last event disable:\n");
		kt_trace_restore_state(thr,
				thr->last_event_disable_time, &state);
		kt_stack_print(&state.stack, 0);
		pr_err("\n");

		pr_err("Last event enable:\n");
		kt_trace_restore_state(thr,
				thr->last_event_enable_time, &state);
		kt_stack_print(&state.stack, 0);
		pr_err("\n");
	}
#endif /* KT_DEBUG */

	pr_err("# # # # # # # # # # # # # # # # # # # # # # # # # # # # #\n");
}

/* FIXME(xairy): not sure if this is the best place for this
   function, but it requires access to ENTER and LEAVE. */
void kt_tests_run(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_tests_run_noinst();
	LEAVE();
	kt_tests_run_inst();
}

void ktsan_interrupt_enter(void)
{
	/* Switch to a dedicated stack for interrupts. This helps to keep stack
	 * depot memory consumption sane. If we would glue interrupted thread
	 * and interrupt stacks together, we will have to constantly save new
	 * stacks in stack depot.
	 */
	ENTER(KT_ENTER_NORMAL);
	if (thr->interrupt_depth++ == 0)
		kt_thr_interrupt(thr, pc, &thr->cpu->interrupted);
	LEAVE();
}

void ktsan_interrupt_exit(void)
{
	ENTER(KT_ENTER_NORMAL);
	if (--thr->interrupt_depth == 0)
		kt_thr_resume(thr, pc, &thr->cpu->interrupted);
	BUG_ON(thr->interrupt_depth < 0);
	LEAVE();
}

void ktsan_syscall_enter(void)
{
	/* Does nothing for now. */
}

void ktsan_syscall_exit(void)
{
	/* Does nothing for now. */
}

void ktsan_cpu_start(void)
{
	/* Does nothing for now. */
}

void ktsan_task_create(struct ktsan_task_s *new, int pid)
{
	ENTER(KT_ENTER_SCHED | KT_ENTER_DISABLED);
	new->task = kt_cache_alloc(&kt_ctx.task_cache);
	new->task->thr = kt_thr_create(thr, pid);
	new->task->running = false;
	LEAVE();
}

void ktsan_task_destroy(struct ktsan_task_s *old)
{
	ENTER(KT_ENTER_SCHED | KT_ENTER_DISABLED);
	kt_thr_destroy(thr, old->task->thr);
	old->task->thr = NULL;
	kt_cache_free(&kt_ctx.task_cache, old->task);
	LEAVE();
}

void ktsan_task_start(void)
{
	ENTER(KT_ENTER_SCHED | KT_ENTER_DISABLED);
	BUG_ON(task->running);
	task->running = true;
	kt_thr_start(thr, pc);
	LEAVE();
}

void ktsan_task_stop(void)
{
	ENTER(KT_ENTER_SCHED | KT_ENTER_DISABLED);
	BUG_ON(thr->interrupt_depth); /* Context switch during an interrupt? */
	BUG_ON(!task->running);
	task->running = false;
	kt_thr_stop(thr, pc);
	LEAVE();
}

void ktsan_thr_event_disable(void)
{
	ENTER(KT_ENTER_DISABLED);
	kt_thr_event_disable(thr, pc, &kt_flags);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_thr_event_disable);

void ktsan_thr_event_enable(void)
{
	ENTER(KT_ENTER_DISABLED);
	kt_thr_event_enable(thr, pc, &kt_flags);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_thr_event_enable);

void ktsan_thr_report_disable(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_thr_report_disable(thr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_thr_report_disable);

void ktsan_thr_report_enable(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_thr_report_enable(thr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_thr_report_enable);

void ktsan_sync_acquire(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_sync_acquire(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_sync_acquire);

void ktsan_sync_release(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_sync_release(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_sync_release);

static
void ktsan_memblock_alloc(void *addr, unsigned long size, bool write_to_shadow)
{
	ENTER(KT_ENTER_DISABLED);
	BUG_ON(thr->event_disable_depth != 0);
	kt_memblock_alloc(thr, pc, (uptr_t)addr, (size_t)size, write_to_shadow);
	LEAVE();
}

void ktsan_memblock_free(void *addr, unsigned long size, bool write_to_shadow)
{
	ENTER(KT_ENTER_DISABLED);
	BUG_ON(thr->event_disable_depth != 0);
	kt_memblock_free(thr, pc, (uptr_t)addr, (size_t)size, write_to_shadow);
	LEAVE();
}

void ktsan_slab_alloc(void *addr, unsigned long size, unsigned long flags)
{
	ktsan_memblock_alloc(addr, size, !(flags & SLAB_TYPESAFE_BY_RCU));
}

void ktsan_slab_free(void *addr, unsigned long size, unsigned long flags)
{
	ktsan_memblock_free(addr, size, !(flags & SLAB_TYPESAFE_BY_RCU));
}

void ktsan_mtx_pre_lock(void *addr, bool write, bool try)
{
	ENTER(KT_ENTER_DISABLED);

	if (kt_thr_event_disable(thr, pc, &kt_flags))
		kt_mtx_pre_lock(thr, pc, (uptr_t)addr, write, try);

	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_pre_lock);

void ktsan_mtx_post_lock(void *addr, bool write, bool try, bool success)
{
	ENTER(KT_ENTER_DISABLED);

	if (kt_thr_event_enable(thr, pc, &kt_flags))
		kt_mtx_post_lock(thr, pc, (uptr_t)addr, write, try, success);

	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_post_lock);

void ktsan_mtx_pre_unlock(void *addr, bool write)
{
	ENTER(KT_ENTER_DISABLED);

	if (kt_thr_event_disable(thr, pc, &kt_flags))
		kt_mtx_pre_unlock(thr, pc, (uptr_t)addr, write);

	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_pre_unlock);

void ktsan_mtx_post_unlock(void *addr, bool write)
{
	ENTER(KT_ENTER_DISABLED);

	if (kt_thr_event_enable(thr, pc, &kt_flags))
		kt_mtx_post_unlock(thr, pc, (uptr_t)addr, write);

	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_post_unlock);

void ktsan_mtx_downgrade(void *addr)
{
	ENTER(KT_ENTER_DISABLED);
	kt_mtx_downgrade(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_mtx_downgrade);

void ktsan_seqcount_begin(const void *s)
{
	ENTER(KT_ENTER_DISABLED);
	kt_seqcount_begin(thr, pc, (uptr_t)s);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_seqcount_begin);

void ktsan_seqcount_end(const void *s)
{
	ENTER(KT_ENTER_DISABLED);
	kt_seqcount_end(thr, pc, (uptr_t)s);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_seqcount_end);


void ktsan_seqcount_ignore_begin(void)
{
	ENTER(KT_ENTER_DISABLED);
	kt_seqcount_ignore_begin(thr, pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_seqcount_ignore_begin);

void ktsan_seqcount_ignore_end(void)
{
	ENTER(KT_ENTER_DISABLED);
	kt_seqcount_ignore_end(thr, pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_seqcount_ignore_end);

void ktsan_thread_fence(ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_thread_fence(thr, pc, mo);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_thread_fence);

void ktsan_atomic8_store(void *addr, u8 value, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic8_store(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic8_store_no_ktsan(addr, value);
}
EXPORT_SYMBOL(ktsan_atomic8_store);

void ktsan_atomic16_store(void *addr, u16 value, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic16_store(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic16_store_no_ktsan(addr, value);
}
EXPORT_SYMBOL(ktsan_atomic16_store);

void ktsan_atomic32_store(void *addr, u32 value, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic32_store(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic32_store_no_ktsan(addr, value);
}
EXPORT_SYMBOL(ktsan_atomic32_store);

void ktsan_atomic64_store(void *addr, u64 value, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic64_store(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic64_store_no_ktsan(addr, value);
}
EXPORT_SYMBOL(ktsan_atomic64_store);

u8 ktsan_atomic8_load(const void *addr, ktsan_memory_order_t mo)
{
	u8 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic8_load(thr, pc, addr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic8_load_no_ktsan(addr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic8_load);

u16 ktsan_atomic16_load(const void *addr, ktsan_memory_order_t mo)
{
	u16 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic16_load(thr, pc, addr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic16_load_no_ktsan(addr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic16_load);

u32 ktsan_atomic32_load(const void *addr, ktsan_memory_order_t mo)
{
	u32 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic32_load(thr, pc, addr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic32_load_no_ktsan(addr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic32_load);

u64 ktsan_atomic64_load(const void *addr, ktsan_memory_order_t mo)
{
	u64 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic64_load(thr, pc, addr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic64_load_no_ktsan(addr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic64_load);

u8 ktsan_atomic8_exchange(void *addr, u8 value, ktsan_memory_order_t mo)
{
	u8 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic8_exchange(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic8_exchange_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic8_exchange);

u16 ktsan_atomic16_exchange(void *addr, u16 value, ktsan_memory_order_t mo)
{
	u16 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic16_exchange(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic16_exchange_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic16_exchange);

u32 ktsan_atomic32_exchange(void *addr, u32 value, ktsan_memory_order_t mo)
{
	u32 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic32_exchange(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic32_exchange_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic32_exchange);

u64 ktsan_atomic64_exchange(void *addr, u64 value, ktsan_memory_order_t mo)
{
	u64 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic64_exchange(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic64_exchange_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic64_exchange);

u8 ktsan_atomic8_compare_exchange(void *addr, u8 old, u8 new,
					ktsan_memory_order_t mo)
{
	u8 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic8_compare_exchange(thr, pc, addr, old, new, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic8_compare_exchange_no_ktsan(addr, old, new);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic8_compare_exchange);

u16 ktsan_atomic16_compare_exchange(void *addr, u16 old, u16 new,
					ktsan_memory_order_t mo)
{
	u16 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic16_compare_exchange(thr, pc, addr, old, new, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic16_compare_exchange_no_ktsan(addr, old, new);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic16_compare_exchange);

u32 ktsan_atomic32_compare_exchange(void *addr, u32 old, u32 new,
					ktsan_memory_order_t mo)
{
	u32 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic32_compare_exchange(thr, pc, addr, old, new, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic32_compare_exchange_no_ktsan(addr, old, new);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic32_compare_exchange);

u64 ktsan_atomic64_compare_exchange(void *addr, u64 old, u64 new,
					ktsan_memory_order_t mo)
{
	u64 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic64_compare_exchange(thr, pc, addr, old, new, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic64_compare_exchange_no_ktsan(addr, old, new);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic64_compare_exchange);

u8 ktsan_atomic8_fetch_add(void *addr, u8 value, ktsan_memory_order_t mo)
{
	u8 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic8_fetch_add(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic8_fetch_add_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic8_fetch_add);

u16 ktsan_atomic16_fetch_add(void *addr, u16 value, ktsan_memory_order_t mo)
{
	u16 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic16_fetch_add(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic16_fetch_add_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic16_fetch_add);

u32 ktsan_atomic32_fetch_add(void *addr, u32 value, ktsan_memory_order_t mo)
{
	u32 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic32_fetch_add(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic32_fetch_add_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic32_fetch_add);

u64 ktsan_atomic64_fetch_add(void *addr, u64 value, ktsan_memory_order_t mo)
{
	u64 rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic64_fetch_add(thr, pc, addr, value, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic64_fetch_add_no_ktsan(addr, value);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic64_fetch_add);

void ktsan_atomic_set_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic_set_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		kt_atomic_set_bit_no_ktsan(addr, nr);
}
EXPORT_SYMBOL(ktsan_atomic_set_bit);

void ktsan_atomic_clear_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic_clear_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		kt_atomic_clear_bit_no_ktsan(addr, nr);
}
EXPORT_SYMBOL(ktsan_atomic_clear_bit);

void ktsan_atomic_change_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	ENTER(KT_ENTER_NORMAL);
	kt_atomic_change_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		kt_atomic_change_bit_no_ktsan(addr, nr);
}
EXPORT_SYMBOL(ktsan_atomic_change_bit);

int ktsan_atomic_fetch_set_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	int rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic_fetch_set_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic_fetch_set_bit_no_ktsan(addr, nr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic_fetch_set_bit);

int ktsan_atomic_fetch_clear_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	int rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic_fetch_clear_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic_fetch_clear_bit_no_ktsan(addr, nr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic_fetch_clear_bit);

int ktsan_atomic_fetch_change_bit(void *addr, long nr, ktsan_memory_order_t mo)
{
	int rv = 0;

	ENTER(KT_ENTER_NORMAL);
	rv = kt_atomic_fetch_change_bit(thr, pc, addr, nr, mo);
	LEAVE();

	if (!event_handled)
		return kt_atomic_fetch_change_bit_no_ktsan(addr, nr);
	return rv;
}
EXPORT_SYMBOL(ktsan_atomic_fetch_change_bit);

void ktsan_preempt_add(int value)
{
	ENTER(KT_ENTER_NORMAL);
	kt_preempt_add(thr, pc, value);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_preempt_add);

void ktsan_preempt_sub(int value)
{
	ENTER(KT_ENTER_NORMAL);
	kt_preempt_sub(thr, pc, value);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_preempt_sub);

void ktsan_irq_disable(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_irq_disable(thr, pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_irq_disable);

void ktsan_irq_enable(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_irq_enable(thr, pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_irq_enable);

void ktsan_irq_save(void)
{
	ENTER(KT_ENTER_NORMAL);
	kt_irq_save(thr, pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_irq_save);

void ktsan_irq_restore(unsigned long flags)
{
	ENTER(KT_ENTER_NORMAL);
	kt_irq_restore(thr, pc, flags);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_irq_restore);

void ktsan_percpu_acquire(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_percpu_acquire(thr, pc, (uptr_t)addr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_percpu_acquire);

void ktsan_read1(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 0, true, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read1);

void ktsan_read2(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 1, true, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read2);

void ktsan_read4(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 2, true, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read4);

void ktsan_read8(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 3, true, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read8);

void ktsan_read16(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 3, true, false);
	kt_access(thr, pc, (uptr_t)addr + 8, 3, true, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read16);

void ktsan_read_range(void *addr, size_t sz)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access_range(thr, pc, (uptr_t)addr, sz, true);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_read_range);

void ktsan_write1(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 0, false, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write1);

void ktsan_write2(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 1, false, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write2);

void ktsan_write4(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 2, false, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write4);

void ktsan_write8(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 3, false, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write8);

void ktsan_write16(void *addr)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access(thr, pc, (uptr_t)addr, 3, false, false);
	kt_access(thr, pc, (uptr_t)addr + 8, 3, false, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write16);

void ktsan_write_range(void *addr, size_t sz)
{
	ENTER(KT_ENTER_NORMAL);
	kt_access_range(thr, pc, (uptr_t)addr, sz, false);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_write_range);

void ktsan_func_entry(void *call_pc)
{
#if KT_DEBUG
	/* mutex_lock calls mutex_lock_slowpath, and it might be useful
	   to see these frames in trace when debugging. Same in func_exit. */
	ENTER(KT_ENTER_DISABLED);
#else
	ENTER(KT_ENTER_NORMAL);
#endif
	kt_func_entry(thr, (uptr_t)call_pc);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_func_entry);

void ktsan_func_exit(void)
{
#if KT_DEBUG
	ENTER(KT_ENTER_DISABLED);
#else
	ENTER(KT_ENTER_NORMAL);
#endif
	kt_func_exit(thr);
	LEAVE();
}
EXPORT_SYMBOL(ktsan_func_exit);

void __tsan_read1(void *) __attribute__ ((alias("ktsan_read1")));
EXPORT_SYMBOL(__tsan_read1);

void __tsan_read2(void *) __attribute__ ((alias("ktsan_read2")));
EXPORT_SYMBOL(__tsan_read2);

void __tsan_read4(void *) __attribute__ ((alias("ktsan_read4")));
EXPORT_SYMBOL(__tsan_read4);

void __tsan_read8(void *) __attribute__ ((alias("ktsan_read8")));
EXPORT_SYMBOL(__tsan_read8);

void __tsan_read16(void *) __attribute__ ((alias("ktsan_read16")));
EXPORT_SYMBOL(__tsan_read16);

void __tsan_read_range(void *, unsigned long size)
	__attribute__ ((alias("ktsan_read_range")));
EXPORT_SYMBOL(__tsan_read_range);

void __tsan_write1(void *) __attribute__ ((alias("ktsan_write1")));
EXPORT_SYMBOL(__tsan_write1);

void __tsan_write2(void *) __attribute__ ((alias("ktsan_write2")));
EXPORT_SYMBOL(__tsan_write2);

void __tsan_write4(void *) __attribute__ ((alias("ktsan_write4")));
EXPORT_SYMBOL(__tsan_write4);

void __tsan_write8(void *) __attribute__ ((alias("ktsan_write8")));
EXPORT_SYMBOL(__tsan_write8);

void __tsan_write16(void *) __attribute__ ((alias("ktsan_write16")));
EXPORT_SYMBOL(__tsan_write16);

void __tsan_write_range(void *, unsigned long size)
	__attribute__ ((alias("ktsan_write_range")));
EXPORT_SYMBOL(__tsan_write_range);

void __tsan_func_entry(void *) __attribute__ ((alias("ktsan_func_entry")));
EXPORT_SYMBOL(__tsan_func_entry);

void __tsan_func_exit(void) __attribute__ ((alias("ktsan_func_exit")));
EXPORT_SYMBOL(__tsan_func_exit);

void __tsan_init(void) __attribute__ ((alias("ktsan_init")));
EXPORT_SYMBOL(__tsan_init);
