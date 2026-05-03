// SPDX-License-Identifier: GPL-2.0-only
/*
 * prefer_silver.c v2.3.1
 *
 * Fix v2.0:
 *  - prefer_silver_update_task() dipindah setelah semua static helpers
 *  - Guard 1: uclamp_min > 0 → skip silver
 *  - Guard 2: big-core recency (80ms)
 *  - Guard 3: burst hysteresis (150ms)
 *  - Lower heavy_task_thresh: 75 → 55
 *  - Lower cpu_util_thresh: 90 → 85
 *
 * Fix v2.1:
 *  - ps_gold_max_freq diisi saat detect_cluster_capacity()
 *  - ps_cpu_online_cb refresh ps_gold_max_freq saat gold CPU online
 *  - ps_check_burst_decay lockless (READ_ONCE)
 *
 * Fix v2.2:
 *  - Fix #1: ps_get_task_state() hash collision protection 500ms guard
 *  - Fix #2: ps_task_util_pct() early boot guard ps_detected
 *  - Fix #3: detect_cluster_capacity() ambil gold_max_freq dari prime
 *  - Fix #4: skip_freq_gate threshold 40% → 15%
 *  - Fix #5: find_best_silver_cpu() fallback hard cap 75%
 *
 * Fix v2.3:
 *  - Fix #1 (KRITIS): gold_max_freq selalu 0 karena cpufreq_quick_get_max()
 *    dipanggil di late_initcall sebelum CPUFreq policy siap. Solusi:
 *    tambah CPUFREQ_POLICY_NOTIFIER yang mengisi ps_gold_max_freq saat
 *    CPUFREQ_CREATE_POLICY event. Freq gate kini aktif otomatis.
 *  - Fix #2: big_core_guard_ns default 80ms → 40ms untuk 120fps gaming.
 *  - Fix #3: burst_decay_ns default 150ms → 80ms.
 *  - Fix #4: detect_cluster_capacity() tidak panggil cpufreq_quick_get_max().
 *
 * Fix v2.3.1:
 *  - Fix build error: CPUFREQ_NOTIFY tidak ada di kernel 5.10.
 *    CPUFREQ_POLICY_NOTIFIER hanya punya CPUFREQ_CREATE_POLICY dan
 *    CPUFREQ_REMOVE_POLICY. Hapus cek CPUFREQ_NOTIFY dari notifier handler.
 */

#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/topology.h>
#include <linux/cpufreq.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/prefer_silver.h>
#include <linux/workqueue.h>
#include "sched.h"


/* ------------------------------------------------------------------ *
 * Tunables
 * ------------------------------------------------------------------ */
int sysctl_prefer_silver     = 1;
int sysctl_heavy_task_thresh = 55;   /* task util % relatif silver_cap */
int sysctl_cpu_util_thresh   = 85;   /* silver CPU util % max */
int sysctl_freq_ratio_thresh = 95;   /* silver_freq / gold_max_freq % max */

/* v2.0 tunables — v2.3: big_core_guard 80ms→40ms, burst_decay 150ms→80ms */
unsigned long sysctl_big_core_guard_ns = 40000000UL;  /* 40ms */
int           sysctl_burst_thresh      = 35;           /* 35% silver_cap */
unsigned long sysctl_burst_decay_ns    = 80000000UL;  /* 80ms */


/* ------------------------------------------------------------------ *
 * Per-task state cache
 * ------------------------------------------------------------------ */
#define PS_TASK_SLOTS     512
#define PS_TASK_HASH(pid) ((unsigned int)(pid) & (PS_TASK_SLOTS - 1))

#define PS_SLOT_REUSE_NS  500000000ULL

struct ps_task_state {
	pid_t         pid;
	u64           last_big_ts;
	u64           last_burst_ts;
	unsigned long peak_util;
};

static struct ps_task_state ps_task_cache[PS_TASK_SLOTS];
static DEFINE_SPINLOCK(ps_task_lock);


/* ------------------------------------------------------------------ *
 * Debug counters
 * ------------------------------------------------------------------ */
atomic_t ps_hit_count       = ATOMIC_INIT(0);
atomic_t ps_miss_count      = ATOMIC_INIT(0);
atomic_t ps_miss_no_silver  = ATOMIC_INIT(0);
atomic_t ps_miss_freq       = ATOMIC_INIT(0);
atomic_t ps_miss_util       = ATOMIC_INIT(0);
atomic_t ps_miss_affinity   = ATOMIC_INIT(0);
atomic_t ps_miss_task_heavy = ATOMIC_INIT(0);
atomic_t ps_boot_miss       = ATOMIC_INIT(0);
atomic_t ps_miss_uclamp     = ATOMIC_INIT(0);
atomic_t ps_miss_bigcore    = ATOMIC_INIT(0);
atomic_t ps_miss_burst      = ATOMIC_INIT(0);


/* ------------------------------------------------------------------ *
 * Cluster state
 * ------------------------------------------------------------------ */
unsigned long ps_silver_cap;
unsigned long ps_gold_cap;
static unsigned long ps_gold_max_freq;
cpumask_t     ps_silver_mask;
cpumask_t     ps_silver_online;
atomic_t      ps_detected = ATOMIC_INIT(0);


static inline bool cpu_is_silver(int cpu)
{
	return cpumask_test_cpu(cpu, &ps_silver_mask);
}


/* ------------------------------------------------------------------ *
 * Debugfs
 * ------------------------------------------------------------------ */
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *ps_debugfs_dir;

static int ps_stats_show(struct seq_file *m, void *v)
{
	int hits    = atomic_read(&ps_hit_count);
	int misses  = atomic_read(&ps_miss_count);
	int total   = hits + misses;
	int hit_pct = total ? (hits * 100) / total : 0;
	int cpu;
	bool first;

	seq_printf(m, "enabled:             %d\n",   sysctl_prefer_silver);
	seq_printf(m, "detected:            %d\n",   atomic_read(&ps_detected));
	seq_printf(m, "boot_miss:           %d\n",   atomic_read(&ps_boot_miss));
	seq_puts(m,   "---\n");
	seq_printf(m, "hits:                %d\n",   hits);
	seq_printf(m, "misses:              %d\n",   misses);
	seq_printf(m, "hit_rate:            %d%%\n", hit_pct);
	seq_puts(m,   "--- miss breakdown ---\n");
	seq_printf(m, "miss_no_silver:      %d\n",   atomic_read(&ps_miss_no_silver));
	seq_printf(m, "miss_affinity:       %d\n",   atomic_read(&ps_miss_affinity));
	seq_printf(m, "miss_freq:           %d\n",   atomic_read(&ps_miss_freq));
	seq_printf(m, "miss_util:           %d\n",   atomic_read(&ps_miss_util));
	seq_printf(m, "miss_task_heavy:     %d\n",   atomic_read(&ps_miss_task_heavy));
	seq_printf(m, "miss_uclamp:         %d\n",   atomic_read(&ps_miss_uclamp));
	seq_printf(m, "miss_bigcore:        %d\n",   atomic_read(&ps_miss_bigcore));
	seq_printf(m, "miss_burst:          %d\n",   atomic_read(&ps_miss_burst));
	seq_puts(m,   "--- tunables ---\n");
	seq_printf(m, "heavy_task_thresh:   %d%%\n", sysctl_heavy_task_thresh);
	seq_printf(m, "cpu_util_thresh:     %d%%\n", sysctl_cpu_util_thresh);
	seq_printf(m, "freq_ratio_thresh:   %d%%\n", sysctl_freq_ratio_thresh);
	seq_printf(m, "big_core_guard_ns:   %lu\n",  sysctl_big_core_guard_ns);
	seq_printf(m, "burst_thresh:        %d%%\n", sysctl_burst_thresh);
	seq_printf(m, "burst_decay_ns:      %lu\n",  sysctl_burst_decay_ns);
	seq_puts(m,   "--- cluster ---\n");
	seq_printf(m, "silver_cap:          %lu\n",  ps_silver_cap);
	seq_printf(m, "gold_cap:            %lu\n",  ps_gold_cap);
	seq_printf(m, "gold_max_freq:       %lu\n",  READ_ONCE(ps_gold_max_freq));

	seq_puts(m, "silver_mask:         ");
	first = true;
	for_each_cpu(cpu, &ps_silver_mask) {
		if (!first) seq_puts(m, ",");
		seq_printf(m, "%d", cpu);
		first = false;
	}
	seq_puts(m, first ? "(none)\n" : "\n");

	seq_puts(m, "silver_online:       ");
	first = true;
	for_each_cpu(cpu, &ps_silver_online) {
		if (!first) seq_puts(m, ",");
		seq_printf(m, "%d", cpu);
		first = false;
	}
	seq_puts(m, first ? "(none)\n" : "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ps_stats);

static ssize_t ps_reset_write(struct file *f, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	atomic_set(&ps_hit_count,       0);
	atomic_set(&ps_miss_count,      0);
	atomic_set(&ps_miss_no_silver,  0);
	atomic_set(&ps_miss_affinity,   0);
	atomic_set(&ps_miss_freq,       0);
	atomic_set(&ps_miss_util,       0);
	atomic_set(&ps_miss_task_heavy, 0);
	atomic_set(&ps_miss_uclamp,     0);
	atomic_set(&ps_miss_bigcore,    0);
	atomic_set(&ps_miss_burst,      0);
	pr_info("prefer_silver: counters reset\n");
	return count;
}

static const struct file_operations ps_reset_fops = {
	.write  = ps_reset_write,
	.llseek = noop_llseek,
};

static void prefer_silver_debugfs_init(void)
{
	ps_debugfs_dir = debugfs_create_dir("prefer_silver", NULL);
	if (IS_ERR_OR_NULL(ps_debugfs_dir)) {
		pr_warn("prefer_silver: debugfs create dir failed\n");
		ps_debugfs_dir = NULL;
		return;
	}
	debugfs_create_file("stats", 0444, ps_debugfs_dir, NULL, &ps_stats_fops);
	debugfs_create_file("reset", 0200, ps_debugfs_dir, NULL, &ps_reset_fops);
}

#else
static inline void prefer_silver_debugfs_init(void) { }
#endif /* CONFIG_DEBUG_FS */


/* ------------------------------------------------------------------ *
 * Cluster detection
 * ------------------------------------------------------------------ */
static void update_silver_online(void)
{
	cpumask_and(&ps_silver_online, &ps_silver_mask, cpu_online_mask);
}


static bool detect_cluster_capacity(void)
{
	cpumask_t tmp_silver_mask;
	unsigned long min_cap    = ULONG_MAX;
	unsigned long second_cap = ULONG_MAX;
	int cpu;

	cpumask_clear(&tmp_silver_mask);

	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap < min_cap)
			min_cap = cap;
	}

	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap > (min_cap * 115 / 100) && cap < second_cap)
			second_cap = cap;
	}
	for_each_possible_cpu(cpu) {
		unsigned long cap = capacity_orig_of(cpu);
		if (cap <= (min_cap * 115 / 100))
			cpumask_set_cpu(cpu, &tmp_silver_mask);
	}

	if (min_cap == ULONG_MAX || cpumask_empty(&tmp_silver_mask))
		return false;
	if (cpumask_weight(&tmp_silver_mask) >= num_possible_cpus())
		return false;

	ps_silver_cap = min_cap;
	ps_gold_cap   = (second_cap == ULONG_MAX) ? (min_cap * 2) : second_cap;
	cpumask_copy(&ps_silver_mask, &tmp_silver_mask);

	/* ps_gold_max_freq diisi oleh ps_cpufreq_policy_nb via notifier */

	update_silver_online();

	pr_info("prefer_silver: detect OK silver_cap=%lu gold_cap=%lu mask=%*pbl\n",
		ps_silver_cap, ps_gold_cap, cpumask_pr_args(&ps_silver_mask));

	if (cpumask_empty(&ps_silver_online))
		cpumask_and(&ps_silver_online, &ps_silver_mask, cpu_possible_mask);
	if (cpumask_empty(&ps_silver_online))
		return false;

	return true;
}


/*
 * v2.3 / v2.3.1: CPUFreq policy notifier.
 *
 * Kernel 5.10 CPUFREQ_POLICY_NOTIFIER hanya mengirim dua event:
 *   CPUFREQ_CREATE_POLICY  — policy baru dibuat saat driver init/CPU hotplug
 *   CPUFREQ_REMOVE_POLICY  — policy dihapus
 *
 * CPUFREQ_NOTIFY tidak ada di 5.10 (dihapus sejak kernel 4.x transisi).
 * Notifier ini hanya perlu react ke CPUFREQ_CREATE_POLICY untuk mengisi
 * ps_gold_max_freq dengan cpuinfo.max_freq yang valid.
 */
static int ps_cpufreq_policy_nb_fn(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned long max_cap = 0;
	unsigned long cur_gold_max;
	int cpu, prime_cpu = -1;

	/* v2.3.1: hanya CPUFREQ_CREATE_POLICY yang valid di kernel 5.10 */
	if (event != CPUFREQ_CREATE_POLICY)
		return NOTIFY_DONE;

	if (!atomic_read(&ps_detected))
		return NOTIFY_DONE;

	for_each_cpu(cpu, policy->related_cpus) {
		unsigned long c = capacity_orig_of(cpu);
		if (c > max_cap) {
			max_cap   = c;
			prime_cpu = cpu;
		}
	}

	if (prime_cpu < 0 || cpu_is_silver(prime_cpu))
		return NOTIFY_DONE;

	cur_gold_max = READ_ONCE(ps_gold_max_freq);
	if (policy->cpuinfo.max_freq > cur_gold_max) {
		WRITE_ONCE(ps_gold_max_freq, policy->cpuinfo.max_freq);
		pr_info("prefer_silver: gold_max_freq=%u kHz via cpu%d policy\n",
			policy->cpuinfo.max_freq, prime_cpu);
	}

	return NOTIFY_DONE;
}

static struct notifier_block ps_cpufreq_policy_nb = {
	.notifier_call = ps_cpufreq_policy_nb_fn,
	.priority      = 0,
};


static bool ps_ensure_detected(void)
{
	if (likely(atomic_read(&ps_detected)))
		return !cpumask_empty(&ps_silver_online);

	if (detect_cluster_capacity()) {
		atomic_set(&ps_detected, 1);
		return !cpumask_empty(&ps_silver_online);
	}

	atomic_inc(&ps_boot_miss);
	return false;
}


static int ps_cpu_online_cb(unsigned int cpu)
{
	if (!atomic_read(&ps_detected)) {
		if (detect_cluster_capacity())
			atomic_set(&ps_detected, 1);
	} else {
		if (cpu_is_silver(cpu)) {
			cpumask_set_cpu(cpu, &ps_silver_online);
		} else {
			unsigned long cur = READ_ONCE(ps_gold_max_freq);
			unsigned long this_cap = capacity_orig_of(cpu);

			if (!cur || this_cap >= ps_gold_cap) {
				unsigned long gfreq = cpufreq_quick_get_max(cpu);
				if (gfreq && gfreq > cur) {
					WRITE_ONCE(ps_gold_max_freq, gfreq);
					pr_info("prefer_silver: gold_max_freq=%lukHz via cpu%d online\n",
						gfreq, cpu);
				}
			}
		}
	}
	return 0;
}


static int ps_cpu_offline_cb(unsigned int cpu)
{
	cpumask_clear_cpu(cpu, &ps_silver_online);
	return 0;
}


/* ------------------------------------------------------------------ *
 * Util helpers
 * ------------------------------------------------------------------ */
static inline unsigned long ps_task_util(struct task_struct *p)
{
#ifdef CONFIG_SCHED_WALT
	if (likely(!sched_disable_window_stats))
		return (unsigned long)p->ravg.demand_scaled;
#endif
	return (unsigned long)p->se.avg.util_avg;
}


unsigned long ps_cpu_util(int cpu)
{
#ifdef CONFIG_SCHED_WALT
	if (likely(!sched_disable_window_stats)) {
		unsigned long u =
			(unsigned long)cpu_rq(cpu)->walt_stats
			.cumulative_runnable_avg_scaled;
		return min_t(unsigned long, u, capacity_orig_of(cpu));
	}
#endif
	return (unsigned long)cpu_rq(cpu)->cfs.avg.util_avg;
}


static inline unsigned long ps_task_util_pct(struct task_struct *p)
{
	unsigned long util = ps_task_util(p);
	unsigned long cap;

	if (likely(ps_silver_cap && atomic_read(&ps_detected)))
		cap = ps_silver_cap;
	else
		cap = capacity_orig_of(task_cpu(p));

	if (unlikely(!cap))
		return 100;
	return (util * 100) / cap;
}


static inline unsigned long ps_cpu_util_pct(int cpu)
{
	unsigned long cap = capacity_orig_of(cpu);
	if (unlikely(!cap))
		return 100;
	return (ps_cpu_util(cpu) * 100) / cap;
}


/* ------------------------------------------------------------------ *
 * Per-task cache helper
 * ------------------------------------------------------------------ */
static struct ps_task_state *ps_get_task_state(pid_t pid)
{
	unsigned int slot = PS_TASK_HASH(pid);
	struct ps_task_state *s = &ps_task_cache[slot];

	if (s->pid != pid) {
		if (s->pid != 0) {
			u64 now = ktime_get_ns();
			u64 last_active = max(s->last_big_ts, s->last_burst_ts);

			if (last_active &&
			    (now - last_active) < PS_SLOT_REUSE_NS)
				return NULL;
		}
		s->pid           = pid;
		s->last_big_ts   = 0;
		s->last_burst_ts = 0;
		s->peak_util     = 0;
	}
	return s;
}


/* ------------------------------------------------------------------ *
 * prefer_silver_update_task
 * ------------------------------------------------------------------ */
void prefer_silver_update_task(struct task_struct *p, int cpu)
{
	struct ps_task_state *s;
	unsigned long flags;
	unsigned long util;
	u64 now;
	bool is_big;

	if (!sysctl_prefer_silver || !atomic_read(&ps_detected))
		return;

	is_big = !cpu_is_silver(cpu);
	util   = ps_task_util(p);
	now    = ktime_get_ns();

	spin_lock_irqsave(&ps_task_lock, flags);
	s = ps_get_task_state(p->pid);

	if (!s) {
		spin_unlock_irqrestore(&ps_task_lock, flags);
		return;
	}

	if (is_big)
		s->last_big_ts = now;

	if (s->peak_util < util)
		s->peak_util = util;

	if (ps_silver_cap) {
		unsigned long burst_pct = (util * 100) / ps_silver_cap;
		if ((int)burst_pct >= sysctl_burst_thresh)
			s->last_burst_ts = now;
	}

	spin_unlock_irqrestore(&ps_task_lock, flags);
}
EXPORT_SYMBOL_GPL(prefer_silver_update_task);


/* ------------------------------------------------------------------ *
 * Guard checks
 * ------------------------------------------------------------------ */
static inline bool ps_check_uclamp(struct task_struct *p)
{
#ifdef CONFIG_UCLAMP_TASK
	if (uclamp_eff_value(p, UCLAMP_MIN) > 0)
		return false;
#endif
	return true;
}


static inline bool ps_check_bigcore_recency(struct task_struct *p)
{
	unsigned int slot = PS_TASK_HASH(p->pid);
	u64 last_big, now;

	if (!sysctl_big_core_guard_ns)
		return true;

	last_big = READ_ONCE(ps_task_cache[slot].last_big_ts);
	if (!last_big)
		return true;

	now = ktime_get_ns();
	return (now - last_big) >= sysctl_big_core_guard_ns;
}


static inline bool ps_check_burst_decay(struct task_struct *p)
{
	unsigned int slot = PS_TASK_HASH(p->pid);
	u64 last_burst, now;

	if (!sysctl_burst_decay_ns)
		return true;

	last_burst = READ_ONCE(ps_task_cache[slot].last_burst_ts);
	if (!last_burst)
		return true;

	now = ktime_get_ns();
	return (now - last_burst) >= sysctl_burst_decay_ns;
}


/* ------------------------------------------------------------------ *
 * Main placement
 * ------------------------------------------------------------------ */
int find_best_silver_cpu(struct task_struct *p)
{
	int i, best_cpu = -1, best_cpu_fallback = -1;
	unsigned long min_util = ULONG_MAX, min_util_fallback = ULONG_MAX;
	unsigned long min_gold_util = ULONG_MAX;
	bool has_silver = false, freq_blocked = false;
	bool util_blocked = false, aff_blocked = false;
	bool freq_fallback = false;
	unsigned long task_util_pct;
	bool skip_freq_gate;

	if (!sysctl_prefer_silver)
		return -1;
	if (!ps_ensure_detected())
		return -1;

	if (!ps_check_uclamp(p)) {
		atomic_inc(&ps_miss_count);
		atomic_inc(&ps_miss_uclamp);
		return -1;
	}
	if (!ps_check_bigcore_recency(p)) {
		atomic_inc(&ps_miss_count);
		atomic_inc(&ps_miss_bigcore);
		return -1;
	}
	if (!ps_check_burst_decay(p)) {
		atomic_inc(&ps_miss_count);
		atomic_inc(&ps_miss_burst);
		return -1;
	}
	task_util_pct = ps_task_util_pct(p);
	if (task_util_pct >= (unsigned long)sysctl_heavy_task_thresh) {
		atomic_inc(&ps_miss_count);
		atomic_inc(&ps_miss_task_heavy);
		return -1;
	}

	skip_freq_gate = (task_util_pct < 15);

retry:
	for_each_cpu(i, &ps_silver_online) {
		unsigned long cur_util, weighted_util;

		has_silver = true;

		if (!cpumask_test_cpu(i, p->cpus_ptr)) {
			aff_blocked = true;
			continue;
		}
		if (!skip_freq_gate && !freq_fallback &&
		    !prefer_silver_check_freq(i)) {
			freq_blocked = true;
			continue;
		}

		cur_util = ps_cpu_util(i);

		if (cur_util < min_util_fallback) {
			min_util_fallback = cur_util;
			best_cpu_fallback = i;
		}

		if (!prefer_silver_check_cpu_util(i)) {
			util_blocked = true;
			continue;
		}

		weighted_util = (cur_util == 0 || available_idle_cpu(i)) ?
				0 : cur_util + (ps_silver_cap >> 3);

		if (weighted_util < min_util) {
			min_util = weighted_util;
			best_cpu = i;
		}
	}

	if (best_cpu < 0 && !freq_fallback && !skip_freq_gate &&
	    freq_blocked && !aff_blocked && !util_blocked) {
		freq_fallback = true;
		goto retry;
	}

	if (best_cpu >= 0) {
		atomic_inc(&ps_hit_count);
		return best_cpu;
	}

	if (best_cpu_fallback >= 0 && !aff_blocked) {
		unsigned long g_util;
		unsigned long fallback_util_pct;

		fallback_util_pct = ps_cpu_util_pct(best_cpu_fallback);
		if (fallback_util_pct >= 75)
			goto miss;

		for_each_cpu(i, cpu_online_mask) {
			if (cpu_is_silver(i))
				continue;
			if (!cpumask_test_cpu(i, p->cpus_ptr))
				continue;
			g_util = ps_cpu_util(i);
			if (g_util < min_gold_util)
				min_gold_util = g_util;
		}

		if (min_gold_util == ULONG_MAX ||
		    min_util_fallback <= (min_gold_util * 110 / 100)) {
			atomic_inc(&ps_hit_count);
			return best_cpu_fallback;
		}
	}

miss:
	atomic_inc(&ps_miss_count);
	if (!has_silver)
		atomic_inc(&ps_miss_no_silver);
	else if (aff_blocked)
		atomic_inc(&ps_miss_affinity);
	else if (freq_blocked)
		atomic_inc(&ps_miss_freq);
	else
		atomic_inc(&ps_miss_util);
	return -1;
}


/* ------------------------------------------------------------------ *
 * Check functions
 * ------------------------------------------------------------------ */
bool prefer_silver_check_freq(int cpu)
{
	unsigned long silver_freq = (unsigned long)cpufreq_quick_get(cpu);
	unsigned long gold_max    = READ_ONCE(ps_gold_max_freq);

	if (!silver_freq || !gold_max)
		return true;
	return silver_freq <= (gold_max * sysctl_freq_ratio_thresh / 100);
}


bool prefer_silver_check_cpu_util(int cpu)
{
	return ps_cpu_util_pct(cpu) < sysctl_cpu_util_thresh;
}


bool prefer_silver_check_task_util(struct task_struct *p)
{
	return ps_task_util_pct(p) < sysctl_heavy_task_thresh;
}


/* ------------------------------------------------------------------ *
 * Sysctl
 * ------------------------------------------------------------------ */
static struct ctl_table prefer_silver_table[] = {
	{
		.procname     = "sysctl_prefer_silver",
		.data         = &sysctl_prefer_silver,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "heavy_task_thresh",
		.data         = &sysctl_heavy_task_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "sysctl_cpu_util_thresh",
		.data         = &sysctl_cpu_util_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "freq_ratio_thresh",
		.data         = &sysctl_freq_ratio_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "ps_big_core_guard_ns",
		.data         = &sysctl_big_core_guard_ns,
		.maxlen       = sizeof(unsigned long),
		.mode         = 0644,
		.proc_handler = proc_doulongvec_minmax,
	},
	{
		.procname     = "ps_burst_thresh",
		.data         = &sysctl_burst_thresh,
		.maxlen       = sizeof(int),
		.mode         = 0644,
		.proc_handler = proc_dointvec,
	},
	{
		.procname     = "ps_burst_decay_ns",
		.data         = &sysctl_burst_decay_ns,
		.maxlen       = sizeof(unsigned long),
		.mode         = 0644,
		.proc_handler = proc_doulongvec_minmax,
	},
	{ }
};

static struct ctl_table prefer_silver_root[] = {
	{
		.procname = "kernel",
		.mode     = 0555,
		.child    = prefer_silver_table,
	},
	{ }
};

static struct ctl_table_header *ps_sysctl_header;


/* ------------------------------------------------------------------ *
 * Init
 * ------------------------------------------------------------------ */
static void ps_delayed_detect_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(ps_detect_work, ps_delayed_detect_work);

static void ps_delayed_detect_work(struct work_struct *work)
{
	if (atomic_read(&ps_detected))
		return;
	if (detect_cluster_capacity()) {
		atomic_set(&ps_detected, 1);
		pr_info("prefer_silver: delayed detect OK cap=%lu/%lu mask=%*pbl\n",
			ps_silver_cap, ps_gold_cap, cpumask_pr_args(&ps_silver_mask));
	} else {
		schedule_delayed_work(&ps_detect_work, msecs_to_jiffies(5000));
		pr_warn("prefer_silver: delayed detect FAILED, retry in 5s\n");
	}
}


int __init prefer_silver_init(void)
{
	int ret;

	cpumask_clear(&ps_silver_mask);
	cpumask_clear(&ps_silver_online);
	memset(ps_task_cache, 0, sizeof(ps_task_cache));

	if (detect_cluster_capacity())
		atomic_set(&ps_detected, 1);
	else
		schedule_delayed_work(&ps_detect_work, msecs_to_jiffies(3000));

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"sched/prefer_silver:online",
					ps_cpu_online_cb, ps_cpu_offline_cb);
	if (ret < 0)
		pr_warn("prefer_silver: cpuhp register failed (%d)\n", ret);

	ret = cpufreq_register_notifier(&ps_cpufreq_policy_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		pr_warn("prefer_silver: cpufreq notifier register failed (%d)\n", ret);
	else
		pr_info("prefer_silver: cpufreq policy notifier registered\n");

	ps_sysctl_header = register_sysctl_table(prefer_silver_root);
	if (!ps_sysctl_header) {
		pr_err("prefer_silver: failed to register sysctl\n");
		return -ENOMEM;
	}
	prefer_silver_debugfs_init();

	pr_info("prefer_silver v2.3.1: init OK | enabled=%d heavy=%d%% cpu=%d%% "
		"guard=%lums burst_decay=%lums\n",
		sysctl_prefer_silver,
		sysctl_heavy_task_thresh,
		sysctl_cpu_util_thresh,
		sysctl_big_core_guard_ns / 1000000,
		sysctl_burst_decay_ns / 1000000);

	return 0;
}
late_initcall(prefer_silver_init);
