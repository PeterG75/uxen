/******************************************************************************
 * timer.c
 * 
 * Copyright (c) 2002-2003 Rolf Neugebauer
 * Copyright (c) 2002-2005 K A Fraser
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/types.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/lib.h>
#include <xen/smp.h>
#include <xen/perfc.h>
#include <xen/time.h>
#include <xen/softirq.h>
#include <xen/timer.h>
#include <xen/keyhandler.h>
#include <xen/percpu.h>
#include <xen/cpu.h>
#include <xen/rcupdate.h>
#include <xen/symbols.h>
#include <asm/system.h>
#include <asm/desc.h>
#include <asm/atomic.h>

/* We program the time hardware this far behind the closest deadline. */
static unsigned int timer_slop __read_mostly = 50000; /* 50 us */
#ifndef __UXEN__
integer_param("timer_slop", timer_slop);
#endif /* __UXEN__ */

static DEFINE_PER_CPU(struct timers, timers);

/* Protects lock-free access to per-timer cpu field against cpu offlining. */
static DEFINE_RCU_READ_LOCK(timer_cpu_read_lock);

DEFINE_PER_CPU(s_time_t, timer_deadline);

struct susp_iter {
    s_time_t now;
    int n;
};

/****************************************************************************
 * HEAP OPERATIONS.
 */

#define GET_HEAP_SIZE(_h)     ((int)(((u16 *)(_h))[0]))
#define SET_HEAP_SIZE(_h,_v)  (((u16 *)(_h))[0] = (u16)(_v))

#define GET_HEAP_LIMIT(_h)    ((int)(((u16 *)(_h))[1]))
#define SET_HEAP_LIMIT(_h,_v) (((u16 *)(_h))[1] = (u16)(_v))

/* Sink down element @pos of @heap. */
static void down_heap(struct timer **heap, int pos)
{
    int sz = GET_HEAP_SIZE(heap), nxt;
    struct timer *t = heap[pos];

    while ( (nxt = (pos << 1)) <= sz )
    {
        if ( ((nxt+1) <= sz) && (heap[nxt+1]->expires < heap[nxt]->expires) )
            nxt++;
        if ( heap[nxt]->expires > t->expires )
            break;
        heap[pos] = heap[nxt];
        heap[pos]->heap_offset = pos;
        pos = nxt;
    }

    heap[pos] = t;
    t->heap_offset = pos;
}

/* Float element @pos up @heap. */
static void up_heap(struct timer **heap, int pos)
{
    struct timer *t = heap[pos];

    while ( (pos > 1) && (t->expires < heap[pos>>1]->expires) )
    {
        heap[pos] = heap[pos>>1];
        heap[pos]->heap_offset = pos;
        pos >>= 1;
    }

    heap[pos] = t;
    t->heap_offset = pos;
}


/* Delete @t from @heap. Return TRUE if new top of heap. */
static int remove_from_heap(struct timer **heap, struct timer *t)
{
    int sz = GET_HEAP_SIZE(heap);
    int pos = t->heap_offset;

    if ( unlikely(pos == sz) )
    {
        SET_HEAP_SIZE(heap, sz-1);
        goto out;
    }

    heap[pos] = heap[sz];
    heap[pos]->heap_offset = pos;

    SET_HEAP_SIZE(heap, --sz);

    if ( (pos > 1) && (heap[pos]->expires < heap[pos>>1]->expires) )
        up_heap(heap, pos);
    else
        down_heap(heap, pos);

 out:
    return (pos == 1);
}


/* Add new entry @t to @heap. Return TRUE if new top of heap. */
static int add_to_heap(struct timer **heap, struct timer *t)
{
    int sz = GET_HEAP_SIZE(heap);

    /* Fail if the heap is full. */
    if ( unlikely(sz == GET_HEAP_LIMIT(heap)) )
        return 0;

    SET_HEAP_SIZE(heap, ++sz);
    heap[sz] = t;
    t->heap_offset = sz;
    up_heap(heap, sz);

    return (t->heap_offset == 1);
}


/****************************************************************************
 * LINKED LIST OPERATIONS.
 */

static int remove_from_list(struct timer **pprev, struct timer *t)
{
    struct timer *curr, **_pprev = pprev;

    while ( (curr = *_pprev) != t )
        _pprev = &curr->list_next;

    *_pprev = t->list_next;

    return (_pprev == pprev);
}

static int add_to_list(struct timer **pprev, struct timer *t)
{
    struct timer *curr, **_pprev = pprev;

    while ( ((curr = *_pprev) != NULL) && (curr->expires <= t->expires) )
        _pprev = &curr->list_next;

    t->list_next = curr;
    *_pprev = t;

    return (_pprev == pprev);
}


/****************************************************************************
 * TIMER OPERATIONS.
 */

static int remove_entry(struct timer *t, struct timers *timers)
{
    int rc;

    switch ( t->status )
    {
    case TIMER_STATUS_in_heap:
        rc = remove_from_heap(timers->heap, t);
        break;
    case TIMER_STATUS_in_list:
        rc = remove_from_list(&timers->list, t);
        break;
    default:
        rc = 0;
        BUG();
    }

    t->status = TIMER_STATUS_invalid;
    return rc;
}

static int add_entry(struct timer *t, struct timers *timers)
{
    int rc;

    ASSERT(t->status == TIMER_STATUS_invalid);

    /* Try to add to heap. t->heap_offset indicates whether we succeed. */
    t->heap_offset = 0;
    t->status = TIMER_STATUS_in_heap;
    rc = add_to_heap(timers->heap, t);
    if ( t->heap_offset != 0 )
        return rc;

    /* Fall back to adding to the slower linked list. */
    t->status = TIMER_STATUS_in_list;
    return add_to_list(&timers->list, t);
}

static inline void activate_timer(struct timer *timer)
{
    ASSERT(timer->status == TIMER_STATUS_inactive);
    timer->status = TIMER_STATUS_invalid;
    list_del(&timer->inactive);
    if (timer->type == TIMER_TYPE_cpu) {
        if ( add_entry(timer, &per_cpu(timers, timer->cpu)) )
            cpu_raise_softirq(timer->cpu, TIMER_CPU0_SOFTIRQ);
    } else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        if ( add_entry(timer, &timer->vcpu->timers) )
            vcpu_raise_softirq(timer->vcpu, TIMER_VCPU_SOFTIRQ);
    }
}

static inline void deactivate_timer(struct timer *timer)
{
    if (timer->type == TIMER_TYPE_cpu) {
        if ( remove_entry(timer, &per_cpu(timers, timer->cpu)) )
            cpu_raise_softirq(timer->cpu, TIMER_CPU0_SOFTIRQ);

        timer->status = TIMER_STATUS_inactive;
        list_add(&timer->inactive, &per_cpu(timers, timer->cpu).inactive);
    } else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        if ( remove_entry(timer, &timer->vcpu->timers) )
            vcpu_raise_softirq(timer->vcpu, TIMER_VCPU_SOFTIRQ);

        timer->status = TIMER_STATUS_inactive;
        list_add(&timer->inactive, &timer->vcpu->timers.inactive);
    }
}

static inline bool_t timer_lock(struct timer *timer)
{
    unsigned int cpu;
    struct vcpu *vcpu;

    rcu_read_lock(&timer_cpu_read_lock);

    if (timer->type == TIMER_TYPE_cpu) {
        for ( ; ; )
            {
                cpu = atomic_read16(&timer->cpu);
                if ( unlikely(cpu == TIMER_CPU_status_killed) )
                    {
                        rcu_read_unlock(&timer_cpu_read_lock);
                        return 0;
                    }
                spin_lock(&per_cpu(timers, cpu).lock);
                if ( likely(timer->cpu == cpu) )
                    break;
                spin_unlock(&per_cpu(timers, cpu).lock);
            }
    } else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        for ( ; ; )
            {
                vcpu = (struct vcpu *)atomic_readptr(&timer->vcpu);
                if ( unlikely(vcpu == (struct vcpu *)TIMER_VCPU_status_killed) )
                    {
                        rcu_read_unlock(&timer_cpu_read_lock);
                        return 0;
                    }
                spin_lock(&vcpu->timers.lock);
                if ( likely(timer->vcpu == vcpu) )
                    break;
                spin_unlock(&vcpu->timers.lock);
            }
    }

    rcu_read_unlock(&timer_cpu_read_lock);
    return 1;
}

#define timer_lock_irqsave(t, flags) ({         \
    bool_t __x;                                 \
    local_irq_save(flags);                      \
    if ( !(__x = timer_lock(t)) )               \
        local_irq_restore(flags);               \
    __x;                                        \
})

static inline void timer_unlock(struct timer *timer)
{
    if (timer->type == TIMER_TYPE_cpu)
        spin_unlock(&per_cpu(timers, timer->cpu).lock);
    else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        spin_unlock(&timer->vcpu->timers.lock);
    }
}

#define timer_unlock_irqrestore(t, flags) ({    \
    timer_unlock(t);                            \
    local_irq_restore(flags);                   \
})


static bool_t active_timer(struct timer *timer)
{
    ASSERT(timer->status >= TIMER_STATUS_inactive);
    ASSERT(timer->status <= TIMER_STATUS_in_list);
    return (timer->status >= TIMER_STATUS_in_heap);
}

bool_t vcpu_active_timer(struct timer *timer)
{

    ASSERT(timer->type == TIMER_TYPE_vcpu);
    ASSERT(timer->vcpu == current);
    return active_timer(timer);
}

void init_timer(
    struct timer *timer,
    void        (*function)(void *),
    void         *data,
    unsigned int  cpu)
{
    unsigned long flags;
#ifdef __UXEN__
    cpu = 0;                    /* all cpu timers on cpu 0 */
#endif  /* __UXEN__ */
    memset(timer, 0, sizeof(*timer));
    timer->function = function;
    timer->data = data;
    atomic_write16(&timer->cpu, cpu);
    timer->type = TIMER_TYPE_cpu;
    timer->status = TIMER_STATUS_inactive;
    if ( !timer_lock_irqsave(timer, flags) )
        BUG();
    list_add(&timer->inactive, &per_cpu(timers, cpu).inactive);
    timer_unlock_irqrestore(timer, flags);
}


void init_vcpu_timer(
    struct timer *timer,
    void        (*function)(void *),
    void         *data,
    struct vcpu  *vcpu)
{
    unsigned long flags;
    memset(timer, 0, sizeof(*timer));
    timer->function = function;
    timer->data = data;
    timer->status = TIMER_STATUS_inactive;
    atomic_writeptr(&timer->vcpu, vcpu);
    timer->type = TIMER_TYPE_vcpu;
    if ( !timer_lock_irqsave(timer, flags) )
        BUG();
    list_add(&timer->inactive, &vcpu->timers.inactive);
    timer_unlock_irqrestore(timer, flags);
}


void set_timer(struct timer *timer, s_time_t expires)
{
    unsigned long flags;

    if ( !timer_lock_irqsave(timer, flags) )
        return;

    if ( active_timer(timer) )
        deactivate_timer(timer);

    timer->expires = expires;

    activate_timer(timer);

    timer_unlock_irqrestore(timer, flags);
}


void advance_timer(struct timer *timer, s_time_t expires)
{
    unsigned long flags;

    if ( !timer_lock_irqsave(timer, flags) )
        return;

    if (active_timer(timer) && expires <= timer->expires)
        goto out;

    if ( active_timer(timer) )
        deactivate_timer(timer);

    timer->expires = expires;

    activate_timer(timer);

  out:
    timer_unlock_irqrestore(timer, flags);
}


void stop_timer(struct timer *timer)
{
    unsigned long flags;

    if ( !timer_lock_irqsave(timer, flags) )
        return;

    if ( active_timer(timer) )
        deactivate_timer(timer);

    timer_unlock_irqrestore(timer, flags);
}


void migrate_timer(struct timer *timer, unsigned int new_cpu)
{
    unsigned int old_cpu;
    bool_t active;
    unsigned long flags;

    if (timer->type == TIMER_TYPE_vcpu)
        return;
    ASSERT(timer->type == TIMER_TYPE_cpu);

    rcu_read_lock(&timer_cpu_read_lock);

    for ( ; ; )
    {
        old_cpu = atomic_read16(&timer->cpu);
        if ( (old_cpu == new_cpu) || (old_cpu == TIMER_CPU_status_killed) )
        {
            rcu_read_unlock(&timer_cpu_read_lock);
            return;
        }

        if ( old_cpu < new_cpu )
        {
            spin_lock_irqsave(&per_cpu(timers, old_cpu).lock, flags);
            spin_lock(&per_cpu(timers, new_cpu).lock);
        }
        else
        {
            spin_lock_irqsave(&per_cpu(timers, new_cpu).lock, flags);
            spin_lock(&per_cpu(timers, old_cpu).lock);
        }

        if ( likely(timer->cpu == old_cpu) )
             break;

        spin_unlock(&per_cpu(timers, old_cpu).lock);
        spin_unlock_irqrestore(&per_cpu(timers, new_cpu).lock, flags);
    }

    rcu_read_unlock(&timer_cpu_read_lock);

    active = active_timer(timer);
    if ( active )
        deactivate_timer(timer);

    list_del(&timer->inactive);
    atomic_write16(&timer->cpu, new_cpu);
    list_add(&timer->inactive, &per_cpu(timers, new_cpu).inactive);

    if ( active )
        activate_timer(timer);

    spin_unlock(&per_cpu(timers, old_cpu).lock);
    spin_unlock_irqrestore(&per_cpu(timers, new_cpu).lock, flags);
}


void migrate_timer_to_vcpu(struct timer *timer, struct vcpu *v)
{
    unsigned long flags;
    bool_t active;

    ASSERT(timer->type == TIMER_TYPE_vcpu);

    timer_lock_irqsave(timer, flags);

    active = active_timer(timer);
    if ( active )
        deactivate_timer(timer);

    list_del(&timer->inactive);

    timer_unlock(timer);

    timer->vcpu = v;
    timer->type = TIMER_TYPE_vcpu;

    timer_lock(timer);

    list_add(&timer->inactive, &timer->vcpu->timers.inactive);

    if ( active )
        activate_timer(timer);

    timer_unlock_irqrestore(timer, flags);
}


void kill_timer(struct timer *timer)
{
    unsigned int old_cpu, cpu;
    struct vcpu *old_vcpu;
    unsigned long flags;

    /* timer not initialized */
    if (!timer->type)
        return;

    if (timer->type == TIMER_TYPE_cpu)
        BUG_ON(this_cpu(timers).running == timer);
    else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        BUG_ON(current->timers.running == timer);
    }

    if ( !timer_lock_irqsave(timer, flags) )
        return;

    if ( active_timer(timer) )
        deactivate_timer(timer);

    list_del(&timer->inactive);
    timer->status = TIMER_STATUS_killed;

    if (timer->type == TIMER_TYPE_cpu) {
        old_cpu = timer->cpu;
        atomic_write16(&timer->cpu, TIMER_CPU_status_killed);
        spin_unlock_irqrestore(&per_cpu(timers, old_cpu).lock, flags);

        for_each_online_cpu ( cpu )
            while ( per_cpu(timers, cpu).running == timer )
                cpu_relax();
    } else {
        ASSERT(timer->type == TIMER_TYPE_vcpu);
        old_vcpu = timer->vcpu;
        atomic_writeptr(&timer->vcpu, TIMER_VCPU_status_killed);
        spin_unlock_irqrestore(&old_vcpu->timers.lock, flags);

        while ( old_vcpu->timers.running == timer )
            cpu_relax();
    }
}


static void execute_timer(struct timers *ts, struct timer *t)
{
    void (*fn)(void *) = t->function;
    void *data = t->data;

    t->status = TIMER_STATUS_inactive;
    list_add(&t->inactive, &ts->inactive);

    ts->running = t;
    spin_unlock_irq(&ts->lock);
    (*fn)(data);
    spin_lock_irq(&ts->lock);
    ts->running = NULL;
}


static void timer_softirq_action(struct timers *ts, struct vcpu *v)
{
    struct timer  *t, **heap, *next;
    s_time_t       now, deadline;
    int executed = 0;

    heap = ts->heap;

    /* If we overflowed the heap, try to allocate a larger heap. */
    if ( unlikely(ts->list != NULL) )
    {
        /* old_limit == (2^n)-1; new_limit == (2^(n+4))-1 */
        int old_limit = GET_HEAP_LIMIT(heap);
        int new_limit = ((old_limit + 1) << 4) - 1;
        struct timer **newheap = xmalloc_array(struct timer *, new_limit + 1);
        if ( newheap != NULL )
        {
            spin_lock_irq(&ts->lock);
            memcpy(newheap, heap, (old_limit + 1) * sizeof(*heap));
            SET_HEAP_LIMIT(newheap, new_limit);
            ts->heap = newheap;
            spin_unlock_irq(&ts->lock);
            if ( old_limit != 0 )
                xfree(heap);
            heap = newheap;
        }
    }

    spin_lock_irq(&ts->lock);

    now = NOW();

    /* Execute ready heap timers. */
    while ( (GET_HEAP_SIZE(heap) != 0) &&
            ((t = heap[1])->expires < now) )
    {
        remove_from_heap(heap, t);
        execute_timer(ts, t);
        executed++;
        if (now > NOW())
            now = NOW();
    }

    /* Execute ready list timers. */
    while ( ((t = ts->list) != NULL) && (t->expires < now) )
    {
        ts->list = t->list_next;
        execute_timer(ts, t);
        executed++;
        if (now > NOW())
            now = NOW();
    }

    /* Try to move timers from linked list to more efficient heap. */
    next = ts->list;
    ts->list = NULL;
    while ( unlikely((t = next) != NULL) )
    {
        next = t->list_next;
        t->status = TIMER_STATUS_invalid;
        add_entry(t, ts);
    }

    /* Find earliest deadline from head of linked list and top of heap. */
    deadline = STIME_MAX;
    if ( GET_HEAP_SIZE(heap) != 0 )
        deadline = heap[1]->expires;
    if ( (ts->list != NULL) && (ts->list->expires < deadline) )
        deadline = ts->list->expires;
    if (!v) {
        this_cpu(timer_deadline) =
            (deadline == STIME_MAX) ? 0 : deadline + timer_slop;
        if ( !reprogram_timer(this_cpu(timer_deadline), v) )
            raise_softirq(TIMER_CPU0_SOFTIRQ);
    } else {
        v->timer_deadline =
            (deadline == STIME_MAX) ? 0 : deadline + timer_slop;
        if ( !reprogram_timer(v->timer_deadline, v) )
            vcpu_raise_softirq(v, TIMER_VCPU_SOFTIRQ);
    }

    spin_unlock_irq(&ts->lock);
}


static void timer_softirq_action_this_cpu(void)
{
    struct timers *ts;

    ts = &this_cpu(timers);
    return timer_softirq_action(ts, NULL);
}


static void timer_softirq_action_vcpu(struct vcpu *v)
{
    struct timers *ts;

    ts = &v->timers;
    return timer_softirq_action(ts, v);
}


s_time_t align_timer(s_time_t firsttick, uint64_t period)
{
    if ( !period )
        return firsttick;

    return firsttick + (period - 1) - ((firsttick - 1) % period);
}

static void
iterate_timers(struct timers *ts,
               void (*fn)(struct timers *,
                          struct timer *,
                          void *),
               void *opaque)
{
    struct timer *t;
    int j;

    for (j = 1; j <= GET_HEAP_SIZE(ts->heap); j++)
        fn(ts, ts->heap[j], opaque);
    for (t = ts->list, j = 0; t != NULL; t = t->list_next, j++)
        fn(ts, t, opaque);
}

static void
dump_timer(struct timers *ts, struct timer *t, void *opaque)
{
    s_time_t now = *((s_time_t*)opaque);

    printk("  ex=%8"PRId64"us timer=%p cb=%p(%p) %S\n",
           (t->expires - now) / 1000, t, t->function, t->data,
           (printk_symbol)t->function);
}

/* static */ void dump_timerq(unsigned char key)
{
    struct timers *ts;
    unsigned long  flags;
    s_time_t       now = NOW();
    struct domain *d;
    struct vcpu   *v;
    int            i;

    printk("Dumping timer queues: NOW %"PRId64"\n", now);

    for_each_online_cpu( i )
    {
        ts = &per_cpu(timers, i);

        printk("CPU%02d:\n", i);
        spin_lock_irqsave(&ts->lock, flags);
        iterate_timers(ts, dump_timer, &now);
        spin_unlock_irqrestore(&ts->lock, flags);
    }

    for_each_domain(d) {
        for_each_vcpu(d, v) {
            printk("VCPU[vm%u.%u]:\n", d->domain_id, v->vcpu_id);
            ts = &v->timers;
            spin_lock_irqsave(&ts->lock, flags);
            iterate_timers(ts, dump_timer, &now);
            spin_unlock_irqrestore(&ts->lock, flags);
        }
    }
}

static void
suspend_one_timer(struct timers *ts, struct timer *timer, void *opaque)
{
    struct susp_iter *it = (struct susp_iter*)opaque;

    timer->suspended = 0;
    if (active_timer(timer)) {
        timer->suspended = 1;

        /* save expiration delta WRT now now */
        timer->expires -= it->now;
    }
    it->n += timer->suspended;
}

static void
resume_one_timer(struct timers *ts, struct timer *timer, void *opaque)
{
    struct susp_iter *it = (struct susp_iter*)opaque;
    int s = timer->suspended;

    /* restore expiration time using saved delta (held in timer->expires) */
    if (s) {
        timer->suspended = 0;
        timer->expires += it->now;
        if (timer->type == TIMER_TYPE_cpu)
            cpu_raise_softirq(timer->cpu, TIMER_CPU0_SOFTIRQ);
        else if (timer->type == TIMER_TYPE_vcpu)
            vcpu_raise_softirq(timer->vcpu, TIMER_VCPU_SOFTIRQ);
    }
    it->n += s;
}

static int
__suspend_timers(struct timers *ts, s_time_t now)
{
    struct susp_iter it;

    it.now = now;
    it.n = 0;
    iterate_timers(ts, suspend_one_timer, &it);
    return it.n;
}

static int
__resume_timers(struct timers *ts, s_time_t now)
{
    struct susp_iter it;

    it.now = now;
    it.n = 0;
    iterate_timers(ts, resume_one_timer, &it);
    return it.n;
}

static void
suspend_resume_timers(int suspend)
{
    struct timers *ts;
    struct domain *d;
    struct vcpu *v;
    unsigned long flags;
    int i, n;
    s_time_t now;

    rcu_read_lock(&timer_cpu_read_lock);
    for_each_online_cpu(i) {
        ts = &per_cpu(timers, i);
        spin_lock_irqsave(&ts->lock, flags);
        now = NOW();
        if (suspend) {
            ts->suspend_time = now;
            n = __suspend_timers(ts, now);
            if (n)
                printk("suspended pcpu%d timers; count=%d\n",
                       i, n);
        } else {
            n = __resume_timers(ts, now);
            if (n)
                printk("resumed pcpu%d timers; count=%d; delta=%"PRId64"ms\n",
                       i, n, (now - ts->suspend_time) / MILLISECS(1));
        }
        spin_unlock_irqrestore(&ts->lock, flags);
    }
    rcu_read_unlock(&timer_cpu_read_lock);

    rcu_read_lock(&domlist_read_lock);
    for_each_domain(d) {
        domain_lock(d);
        for_each_vcpu(d, v) {
            ts = &v->timers;
            spin_lock_irqsave(&ts->lock, flags);
            now = NOW();
            if (suspend) {
                ts->suspend_time = now;
                n = __suspend_timers(ts, now);
                if (n)
                    printk("suspended vcpu(vm%u.%u) timers; count=%d\n",
                           d->domain_id, v->vcpu_id, n);
            } else {
                n = __resume_timers(ts, now);
                if (n)
                    printk("resumed vcpu(vm%u.%u) timers; count=%d; "
                           "delta=%"PRId64"ms\n",
                           d->domain_id, v->vcpu_id, n,
                           (now - ts->suspend_time) / MILLISECS(1));
            }
            spin_unlock_irqrestore(&ts->lock, flags);
        }
        domain_unlock(d);
    }
    rcu_read_unlock(&domlist_read_lock);
}

void
suspend_timers(void)
{
    suspend_resume_timers(1);
}

void
resume_timers(void)
{
    suspend_resume_timers(0);
}

static struct keyhandler dump_timerq_keyhandler = {
    .diagnostic = 1,
    .u.fn = dump_timerq,
    .desc = "dump timer queues"
};

static void migrate_timers_from_cpu(unsigned int old_cpu)
{
    unsigned int new_cpu = cpumask_any(&cpu_online_map);
    struct timers *old_ts, *new_ts;
    struct timer *t;
    bool_t notify = 0;

    ASSERT(!cpu_online(old_cpu) && cpu_online(new_cpu));

    old_ts = &per_cpu(timers, old_cpu);
    new_ts = &per_cpu(timers, new_cpu);

    if ( old_cpu < new_cpu )
    {
        spin_lock_irq(&old_ts->lock);
        spin_lock(&new_ts->lock);
    }
    else
    {
        spin_lock_irq(&new_ts->lock);
        spin_lock(&old_ts->lock);
    }

    while ( (t = GET_HEAP_SIZE(old_ts->heap)
             ? old_ts->heap[1] : old_ts->list) != NULL )
    {
        remove_entry(t, &per_cpu(timers, t->cpu));
        atomic_write16(&t->cpu, new_cpu);
        notify |= add_entry(t, &per_cpu(timers, t->cpu));
    }

    while ( !list_empty(&old_ts->inactive) )
    {
        t = list_entry(old_ts->inactive.next, struct timer, inactive);
        list_del(&t->inactive);
        atomic_write16(&t->cpu, new_cpu);
        list_add(&t->inactive, &new_ts->inactive);
    }

    spin_unlock(&old_ts->lock);
    spin_unlock_irq(&new_ts->lock);
    local_irq_enable();

    if ( notify )
        cpu_raise_softirq(new_cpu, TIMER_CPU0_SOFTIRQ);
}

static struct timer *dummy_heap;

void init_timers(struct timers *ts)
{
    INIT_LIST_HEAD(&ts->inactive);
    spin_lock_init(&ts->lock);
    ts->heap = &dummy_heap;
}

static int cpu_callback(
    struct notifier_block *nfb, unsigned long action, void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    struct timers *ts = &per_cpu(timers, cpu);

    switch ( action )
    {
    case CPU_UP_PREPARE:
        INIT_LIST_HEAD(&ts->inactive);
        spin_lock_init(&ts->lock);
        ts->heap = &dummy_heap;
        break;
    case CPU_UP_CANCELED:
    case CPU_DEAD:
        migrate_timers_from_cpu(cpu);
        break;
    default:
        break;
    }

    return NOTIFY_DONE;
}

static struct notifier_block cpu_nfb = {
    .notifier_call = cpu_callback,
    .priority = 99
};

void __init timer_init(void)
{
    void *cpu = (void *)(long)smp_processor_id();

    open_softirq(TIMER_CPU0_SOFTIRQ, timer_softirq_action_this_cpu);
    open_softirq_vcpu(TIMER_VCPU_SOFTIRQ, timer_softirq_action_vcpu);

    /*
     * All CPUs initially share an empty dummy heap. Only those CPUs that
     * are brought online will be dynamically allocated their own heap.
     */
    SET_HEAP_SIZE(&dummy_heap, 0);
    SET_HEAP_LIMIT(&dummy_heap, 0);

    cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);
    register_cpu_notifier(&cpu_nfb);

    register_keyhandler('a', &dump_timerq_keyhandler);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
