// SPDX-License-Identifier: GPL-2.0-only
// A bounded, read-only process sampler. Snapshots own values, never task/mm pointers.
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/refcount.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

static unsigned int interval_ms = 1000;
module_param(interval_ms, uint, 0444);
MODULE_PARM_DESC(interval_ms, "Sampling interval, 10..60000 ms");
static unsigned int max_tasks = 4096;
module_param(max_tasks, uint, 0444);
MODULE_PARM_DESC(max_tasks, "Maximum process leaders sampled, 1..65536");

struct task_row {
	pid_t pid, ppid;
	char comm[TASK_COMM_LEN];
	int cpu, priority;
	unsigned int policy;
	u64 start_ns, user_ns, system_ns, vruntime;
	unsigned long weight, rss_pages;
	unsigned long code_start, code_end, data_start, data_end;
	unsigned long heap_start, heap_end, stack_pointer;
	bool memory_available;
};

struct snapshot {
	refcount_t refs;
	u64 generation, started_ns, finished_ns;
	unsigned int count;
	bool truncated;
	struct task_row rows[];
};

static DEFINE_MUTEX(snapshot_lock);
static struct snapshot *published;
static struct proc_dir_entry *proc_root;
static struct delayed_work sample_work;
static bool stopping;
static u64 generation;
static atomic64_t failed_samples = ATOMIC64_INIT(0);

static void snapshot_put(struct snapshot *s)
{
	if (s && refcount_dec_and_test(&s->refs))
		kvfree(s);
}

static struct snapshot *collect_snapshot(void)
{
	struct task_struct **tasks, *task;
	struct snapshot *s;
	unsigned int count = 0, i;

	/* Allocation and mmap locks may sleep: this function runs in process context. */
	tasks = kvcalloc(max_tasks, sizeof(*tasks), GFP_KERNEL);
	s = kvzalloc(struct_size(s, rows, max_tasks), GFP_KERNEL);
	if (!tasks || !s) {
		kvfree(tasks);
		kvfree(s);
		return NULL;
	}
	refcount_set(&s->refs, 1);
	s->started_ns = ktime_get_boottime_ns();
	/* Only take references while holding RCU; no sleeping work in this region. */
	rcu_read_lock();
	for_each_process(task) {
		if (READ_ONCE(task->flags) & PF_KTHREAD)
			continue;
		if (count == max_tasks) {
			s->truncated = true;
			break;
		}
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();

	for (i = 0; i < count; i++) {
		struct mm_struct *mm;
		struct task_row *r = &s->rows[i];
		task = tasks[i];
		r->pid = task_pid_nr(task);
		rcu_read_lock();
		r->ppid = task_pid_nr(rcu_dereference(task->real_parent));
		rcu_read_unlock();
		get_task_comm(r->comm, task);
		r->cpu = task_cpu(task);
		r->priority = READ_ONCE(task->prio);
		r->policy = READ_ONCE(task->policy);
		r->start_ns = READ_ONCE(task->start_boottime);
		r->user_ns = READ_ONCE(task->utime);
		r->system_ns = READ_ONCE(task->stime);
		r->vruntime = READ_ONCE(task->se.vruntime);
		r->weight = READ_ONCE(task->se.load.weight);
		/* A task reference alone does not keep its address space alive. */
		mm = get_task_mm(task);
		if (mm) {
			if (mmap_read_trylock(mm)) {
				r->code_start = mm->start_code;
				r->code_end = mm->end_code;
				r->data_start = mm->start_data;
				r->data_end = mm->end_data;
				r->heap_start = mm->start_brk;
				r->heap_end = mm->brk;
				r->stack_pointer = mm->start_stack;
				r->rss_pages = get_mm_rss(mm);
				r->memory_available = true;
				mmap_read_unlock(mm);
			}
			mmput(mm);
		}
		put_task_struct(task);
		cond_resched();
	}
	kvfree(tasks);
	s->count = count;
	s->finished_ns = ktime_get_boottime_ns();
	s->generation = ++generation;
	return s;
}

static void publish_snapshot(struct snapshot *next)
{
	struct snapshot *old;
	mutex_lock(&snapshot_lock);
	old = published;
	published = next;
	mutex_unlock(&snapshot_lock);
	snapshot_put(old);
}

static void sample_worker(struct work_struct *work)
{
	struct snapshot *next = collect_snapshot();
	if (next)
		publish_snapshot(next);
	else
		atomic64_inc(&failed_samples); /* Keep the last valid snapshot on ENOMEM. */
	if (!READ_ONCE(stopping))
		schedule_delayed_work(&sample_work, msecs_to_jiffies(interval_ms));
}

static int snapshot_show(struct seq_file *m, void *unused)
{
	const struct snapshot *s = m->private;
	unsigned int i;
	seq_printf(m, "# proc-lens v1 generation=%llu count=%u truncated=%u started_ns=%llu finished_ns=%llu failed_samples=%lld\n",
		   s->generation, s->count, s->truncated, s->started_ns,
		   s->finished_ns, atomic64_read(&failed_samples));
	seq_puts(m, "pid\tppid\tcomm\tcpu\tpriority\tpolicy\tstart_ns\tuser_ns\tsystem_ns\tvruntime\tweight\tmemory_available\trss_pages\tcode_start\tcode_end\tdata_start\tdata_end\theap_start\theap_end\tinitial_stack_pointer\n");
	for (i = 0; i < s->count; i++) {
		const struct task_row *r = &s->rows[i];
		seq_printf(m, "%d\t%d\t", r->pid, r->ppid);
		seq_escape(m, r->comm, "\t\n\\");
		seq_printf(m, "\t%d\t%d\t%u\t%llu\t%llu\t%llu\t%llu\t%lu\t%u\t%lu\t%lx\t%lx\t%lx\t%lx\t%lx\t%lx\t%lx\n",
			r->cpu, r->priority, r->policy, r->start_ns, r->user_ns,
			r->system_ns, r->vruntime, r->weight, r->memory_available,
			r->rss_pages, r->code_start, r->code_end, r->data_start,
			r->data_end, r->heap_start, r->heap_end, r->stack_pointer);
	}
	return 0;
}

static int snapshot_open(struct inode *inode, struct file *file)
{
	struct snapshot *s;
	int ret;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	mutex_lock(&snapshot_lock);
	s = published;
	if (s)
		refcount_inc(&s->refs);
	mutex_unlock(&snapshot_lock);
	if (!s)
		return -EAGAIN;
	ret = single_open(file, snapshot_show, s);
	if (ret)
		snapshot_put(s);
	return ret;
}

static int snapshot_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct snapshot *s = seq->private;
	int ret = single_release(inode, file);
	snapshot_put(s);
	return ret;
}

static const struct proc_ops snapshot_ops = {
	.proc_open = snapshot_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = snapshot_release,
};

static int __init proc_lens_init(void)
{
	struct snapshot *s;
	if (interval_ms < 10 || interval_ms > 60000 || max_tasks < 1 || max_tasks > 65536)
		return -EINVAL;
	s = collect_snapshot();
	if (!s)
		return -ENOMEM;
	publish_snapshot(s);
	proc_root = proc_mkdir("proc_lens", NULL);
	if (!proc_root)
		goto fail;
	if (!proc_create("snapshot", 0400, proc_root, &snapshot_ops)) {
		proc_remove(proc_root);
		goto fail;
	}
	INIT_DELAYED_WORK(&sample_work, sample_worker);
	schedule_delayed_work(&sample_work, msecs_to_jiffies(interval_ms));
	return 0;
fail:
	publish_snapshot(NULL);
	return -ENOMEM;
}

static void __exit proc_lens_exit(void)
{
	WRITE_ONCE(stopping, true);
	cancel_delayed_work_sync(&sample_work);
	/* Drain proc callbacks before dropping the publisher's reference. */
	proc_remove(proc_root);
	publish_snapshot(NULL);
}

module_init(proc_lens_init);
module_exit(proc_lens_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("JOON0512");
MODULE_DESCRIPTION("Bounded process snapshots for Linux kernel study");
