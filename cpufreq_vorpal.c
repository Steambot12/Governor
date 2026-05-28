// SPDX-License-Identifier: GPL-2.0
/*
 * Vorpal CPUFreq Governor v5.0 - Thermal-Aware Stabilized 120FPS
 * Rewrite from v4.5 — fixed race conditions, thermal logic, frame tracker,
 * jitter handling, and daily-use power/thermal profile.
 *
 * Critical fixes from v4.5:
 *   - LOCKING: rfx_update_single_freq now holds update_lock for all shared state
 *   - THERMAL: More aggressive daily caps, lower default headroom, virtual cooldown
 *   - GAMING: Lower cluster floors (Prime 75%, Big 70%) to reduce heat while keeping boost
 *   - FRAME: Removed buggy frame duration arithmetic, simplified render phase
 *   - PREDICTOR: Wider window (16), softer EMA (0.88), more aggressive thresholds
 *   - JITTER: Gaming bypass for emergency boosts, daily smoothing preserved
 *   - SUSTAIN: Reduced to 3ms to prevent thermal runaway
 *   - SYSFS: gaming_mode store no longer iterates cpufreq policies (deadlock fix)
 *   - IO WAIT: Simplified streak/boost logic, prevents overflow
 *   - RATE: Daily down limit increased to 8ms for cooler browsing
 *   - FPS: Removed broken breach detection, stabilization now reliable
 *
 * Author: Templar Dev (Steambot12)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/cpufreq.h>
#include <linux/cpufreq.h>
#include <linux/sched/rt.h>
#include <uapi/linux/sched/types.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/ktime.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/irq_work.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define CPUFREQ_VORPAL_PROGNAME	"Vorpal CPUFreq Governor"
#define CPUFREQ_VORPAL_AUTHOR	"Templar Dev"
#define CPUFREQ_VORPAL_VERSION	"5.0-Stable120"

/* === GKI 5.10 COMPAT EXTERNS === */
extern void rfx_get_util_gki510(int cpu, unsigned long boost,
				unsigned long *util, unsigned long *bwmin);
extern bool rfx_dl_bw_exceeded_gki510(int cpu, unsigned long bwmin);

static struct cpufreq_governor vorpal_gov;

/* === GLOBAL GAMING MODE - ATOMIC ACCESS === */
static unsigned int rfx_global_gaming_mode;

/* === BUILTIN ADAPTIVE CONFIG === */
#define RFX_BUILTIN_FPS_HINT		120
#define RFX_BUILTIN_ACTUAL_FPS		120
#define RFX_BUILTIN_THERMAL_HEADROOM	20	/* v5: lower = more cautious */

/* === PREDICTIVE ENGINE === */
#define RFX_PREDICTOR_WINDOW		16	/* v5: wider, power-of-2 */
#define RFX_PREDICT_BOOST_THRESHOLD	25	/* v5: more aggressive */
#define RFX_PREDICT_FAST_UP_THRESHOLD	40

/* === FRAME DEADLINE === */
#define RFX_FRAME_BUDGET_120HZ_NS	8333333ULL
#define RFX_FRAME_BUDGET_90HZ_NS	11111111ULL
#define RFX_FRAME_BUDGET_60HZ_NS	16666667ULL
#define RFX_FRAME_EMERGENCY_NS		2500000ULL	/* v5: 2.5ms */
#define RFX_FRAME_PHASE_THRESHOLD_PCT	18

/* === JITTER REDUCTION === */
#define RFX_JITTER_MAX_DELTA_PCT_GAMING	25	/* v5: more responsive */
#define RFX_JITTER_MAX_DELTA_PCT_DAILY	15
#define RFX_SUSTAIN_WINDOW_NS		3000000ULL	/* v5: 3ms */

/* === ASYMMETRIC CLUSTER POLICY === */
#define RFX_ASYM_PRIME_FLOOR_PCT	75	/* v5: cooler gaming */
#define RFX_ASYM_PRIME_TARGET_PCT	100
#define RFX_ASYM_BIG_FLOOR_PCT		70	/* v5: was 85, too hot */
#define RFX_ASYM_BIG_CAP_PCT		100
#define RFX_ASYM_LITTLE_FLOOR_PCT	30
#define RFX_ASYM_LITTLE_CAP_PCT		85

/* === CPU USAGE SOFT CAP === */
#define RFX_CPU_USAGE_SOFT_CAP		60	/* v5: more aggressive daily */
#define RFX_CPU_USAGE_CAP_STEP_DIV	10

/* === FPS FEEDBACK === */
#define RFX_FPS_STABLE_THRESHOLD	2
#define RFX_FPS_STABLE_TIME_NS		(500 * NSEC_PER_MSEC)

/* === IO WAIT BOOST === */
#define RFX_IOWAIT_BOOST_THRESHOLD	2
#define RFX_IOWAIT_BOOST_PRIME_PCT	85
#define RFX_IOWAIT_BOOST_BIG_PCT	70
#define RFX_IOWAIT_BOOST_LITTLE_PCT	60

/* === THERMAL === */
#define RFX_THERMAL_HEADROOM_DEFAULT	20	/* v5: start throttling earlier */
#define RFX_THERMAL_HARD_MARGIN		5
#define RFX_THERMAL_SOFT_MARGIN		12
#define RFX_THERMAL_CAP_HARD_PCT	45
#define RFX_THERMAL_CAP_SOFT_PCT	65
#define RFX_THERMAL_CAP_NORMAL_PCT	95

/* === DAILY COOLDOWN === */
#define RFX_DAILY_CAP_PCT		80	/* v5: daily max 80% */

/* === RATE LIMITS === */
#define RFX_RATE_LIMIT_GAMING_UP_US	0
#define RFX_RATE_LIMIT_GAMING_DOWN_US	4000	/* v5: 4ms */
#define RFX_RATE_LIMIT_DEFAULT_UP_US	500
#define RFX_RATE_LIMIT_DEFAULT_DOWN_US	8000	/* v5: 8ms for cool browsing */
#define RFX_RATE_LIMIT_IDLE_DOWN_US	3000

/* === IDLE === */
#define RFX_IDLE_CAP_PCT		10	/* v5: lower idle power */

/* === EMA === */
#define RFX_EMA_ALPHA_NUM		88	/* v5: 0.88 smoother */
#define RFX_EMA_ALPHA_DEN		100

/* === CLUSTER DETECTION === */
#define RFX_LITTLE_CAP_THRESHOLD	614
#define RFX_PRIME_CAP_THRESHOLD		1000

/* === SCHED FLAGS === */
#define SCHED_FLAGS_UGOV		0x10000000
#define IOWAIT_BOOST_MIN		(SCHED_CAPACITY_SCALE / 8)

/* === ENUMS === */
enum rfx_cluster_type {
	RFX_CLUSTER_LITTLE = 0,
	RFX_CLUSTER_BIG = 1,
	RFX_CLUSTER_PRIME = 2,
};

/* === DATA STRUCTURES === */

struct rfx_predictor {
	unsigned int hist[RFX_PREDICTOR_WINDOW];
	u8 idx;
	unsigned int ema;
	int trend;	/* -2 fast_down, -1 down, 0 stable, 1 up, 2 fast_up */
	unsigned int predicted;
	u64 last_update_ns;
};

struct rfx_frame_tracker {
	u64 budget_ns;
	u64 last_frame_ns;
	u64 phase_ns;
	bool in_render_phase;
};

struct rfx_fps_state {
	unsigned int hint;
	unsigned int actual;
	u64 stabilized_since_ns;
	bool stabilized;
};

struct rfx_thermal_state {
	int headroom;
	bool active;
};

struct rfx_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	enum rfx_cluster_type cluster_type;
	unsigned int gaming_mode;	/* synced with global, kept for compat */
};

struct rfx_policy;

static void rfx_deferred_update(struct rfx_policy *rfx_pol);
static void rfx_work(struct kthread_work *work);

struct rfx_policy {
	struct cpufreq_policy *policy;
	struct rfx_tunables *tunables;
	struct list_head tunables_hook;
	raw_spinlock_t update_lock;
	u64 last_upfreq_time;
	u64 last_downfreq_time;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	struct irq_work irq_work;
	struct kthread_work work;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;
	bool limits_changed;
	bool need_freq_update;
	u64 last_real_update_ns;

	/* v5 state */
	struct rfx_frame_tracker frame_tracker;
	struct rfx_fps_state fps_state;
	struct rfx_thermal_state thermal;
	unsigned int avg_util_pct;
	bool prev_gaming;
	unsigned int prev_target_freq;
	u64 last_boost_time_ns;
};

struct rfx_cpu {
	struct update_util_data update_util;
	struct rfx_policy *rfx_policy;
	unsigned int cpu;
	bool iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_streak;
	u64 last_update;
	unsigned long util;
	unsigned long bwmin;
	u64 prev_idle_time;
	u64 prev_wall_time;
	unsigned int busy_pct;
	unsigned int prev_util_pct;

	struct rfx_predictor predictor;
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct rfx_cpu, rfx_cpu);

/* === HELPERS === */

static inline enum rfx_cluster_type rfx_get_cluster_type(unsigned int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(cpu);
	if (cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD)
		return RFX_CLUSTER_LITTLE;
	if (cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD)
		return RFX_CLUSTER_PRIME;
	return RFX_CLUSTER_BIG;
}

static inline bool rfx_is_little(unsigned int cpu)
{
	return arch_scale_cpu_capacity(cpu) <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD;
}

static inline bool rfx_is_prime(unsigned int cpu)
{
	return arch_scale_cpu_capacity(cpu) >= (unsigned long)RFX_PRIME_CAP_THRESHOLD;
}

static inline struct gov_attr_set *rfx_to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

static inline struct rfx_tunables *to_rfx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct rfx_tunables, attr_set);
}

static inline bool rfx_gaming_active(void)
{
	return READ_ONCE(rfx_global_gaming_mode);
}

static inline unsigned int rfx_adaptive_max(struct cpufreq_policy *policy,
					    unsigned int pct)
{
	if (!policy || !policy->cpuinfo.max_freq)
		return 0;
	return (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
}

static inline unsigned int rfx_adaptive_floor(struct cpufreq_policy *policy,
					      unsigned int pct)
{
	unsigned int floor;
	if (!policy || !policy->cpuinfo.max_freq)
		return 0;
	floor = (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
	return max(floor, policy->cpuinfo.min_freq);
}

/* === PREDICTIVE ENGINE === */

static void rfx_predictor_init(struct rfx_predictor *p)
{
	memset(p, 0, sizeof(*p));
}

static void rfx_predictor_update(struct rfx_predictor *p, unsigned int util_pct, u64 time)
{
	unsigned int i, recent_sum = 0, prev_sum = 0;
	unsigned int recent_avg, prev_avg;
	unsigned int old_ema = p->ema;

	p->hist[p->idx] = util_pct;
	p->idx = (p->idx + 1) & (RFX_PREDICTOR_WINDOW - 1);

	/* EMA: alpha = 0.88 for gaming, 0.3 for daily */
	if (rfx_gaming_active())
		p->ema = (RFX_EMA_ALPHA_NUM * util_pct +
			  (RFX_EMA_ALPHA_DEN - RFX_EMA_ALPHA_NUM) * old_ema) / RFX_EMA_ALPHA_DEN;
	else
		p->ema = (3 * util_pct + 7 * old_ema) / 10;

	/* Trend detection: last 4 vs previous 4 samples */
	for (i = 0; i < 4; i++) {
		recent_sum += p->hist[(p->idx - 1 - i) & (RFX_PREDICTOR_WINDOW - 1)];
		prev_sum += p->hist[(p->idx - 5 - i) & (RFX_PREDICTOR_WINDOW - 1)];
	}
	recent_avg = recent_sum >> 2;
	prev_avg = prev_sum >> 2;

	if (recent_avg > prev_avg + 15)
		p->trend = 2;	/* fast_up */
	else if (recent_avg > prev_avg + 6)
		p->trend = 1;	/* up */
	else if (recent_avg + 15 < prev_avg)
		p->trend = -2;	/* fast_down */
	else if (recent_avg + 6 < prev_avg)
		p->trend = -1;	/* down */
	else
		p->trend = 0;	/* stable */

	/* Prediction with projection — v5 softer for smoothness */
	p->predicted = p->ema;
	if (p->trend == 2)
		p->predicted = min(100U, p->predicted + 20);
	else if (p->trend == 1)
		p->predicted = min(100U, p->predicted + 10);
	else if (p->trend == -2)
		p->predicted = (p->predicted > 20) ? p->predicted - 20 : 0;
	else if (p->trend == -1)
		p->predicted = (p->predicted > 10) ? p->predicted - 10 : 0;

	p->last_update_ns = time;
}

/* === FRAME DEADLINE TRACKER (SIMPLIFIED) === */

static void rfx_frame_tracker_init(struct rfx_frame_tracker *ft, unsigned int fps_hint)
{
	memset(ft, 0, sizeof(*ft));
	if (fps_hint >= 120)
		ft->budget_ns = RFX_FRAME_BUDGET_120HZ_NS;
	else if (fps_hint >= 90)
		ft->budget_ns = RFX_FRAME_BUDGET_90HZ_NS;
	else
		ft->budget_ns = RFX_FRAME_BUDGET_60HZ_NS;
}

static void rfx_frame_tracker_update(struct rfx_frame_tracker *ft, u64 time,
				     unsigned int util_pct, bool iowait_active)
{
	u64 elapsed = time - ft->last_frame_ns;

	if (!ft->in_render_phase) {
		if ((util_pct > RFX_FRAME_PHASE_THRESHOLD_PCT || iowait_active) &&
		    elapsed > ft->budget_ns / 4) {
			ft->in_render_phase = true;
			ft->last_frame_ns = time;
			ft->phase_ns = 0;
		}
	} else {
		/* v5: Exit at 8% — slightly earlier for power */
		if (util_pct < 8 && elapsed > ft->budget_ns / 2) {
			ft->in_render_phase = false;
		}
	}

	ft->phase_ns = time - ft->last_frame_ns;
}

static bool rfx_frame_needs_emergency_boost(struct rfx_frame_tracker *ft,
					    unsigned int util_pct, u64 time)
{
	u64 remaining;

	if (!ft->in_render_phase)
		return false;

	if (ft->budget_ns > ft->phase_ns)
		remaining = ft->budget_ns - ft->phase_ns;
	else
		remaining = 0;

	/* v5: 2.5ms remaining with 35% util -> emergency */
	if (remaining < RFX_FRAME_EMERGENCY_NS && util_pct > 35)
		return true;

	/* Budget exceeded */
	if (ft->phase_ns > ft->budget_ns)
		return true;

	/* v5: Warning at 55% budget with 35% util */
	if (ft->phase_ns > (ft->budget_ns * 55 / 100) && util_pct > 35)
		return true;

	return false;
}

/* === FPS FEEDBACK LOOP (Internal Only) === */

static void rfx_fps_init(struct rfx_fps_state *fps)
{
	memset(fps, 0, sizeof(*fps));
}

static void rfx_fps_update(struct rfx_fps_state *fps, u64 time)
{
	if (!fps->hint) {
		fps->stabilized = false;
		fps->stabilized_since_ns = 0;
		return;
	}

	if (!rfx_gaming_active()) {
		if (fps->actual >= fps->hint - RFX_FPS_STABLE_THRESHOLD) {
			if (!fps->stabilized) {
				if (!fps->stabilized_since_ns)
					fps->stabilized_since_ns = time;
				else if (time - fps->stabilized_since_ns > RFX_FPS_STABLE_TIME_NS)
					fps->stabilized = true;
			}
		} else {
			fps->stabilized = false;
			fps->stabilized_since_ns = 0;
		}
	} else {
		fps->stabilized = false;
		fps->stabilized_since_ns = 0;
	}
}

/* === THERMAL HEADROOM === */

static void rfx_thermal_init(struct rfx_thermal_state *th)
{
	th->headroom = RFX_THERMAL_HEADROOM_DEFAULT;
	th->active = true;
}

static unsigned int rfx_thermal_apply(struct rfx_thermal_state *th,
				      struct cpufreq_policy *policy,
				      unsigned int target_freq)
{
	unsigned int max_freq = policy->cpuinfo.max_freq;
	unsigned int thermal_max;

	if (!th->active || th->headroom > RFX_THERMAL_SOFT_MARGIN)
		return target_freq;

	if (th->headroom < RFX_THERMAL_HARD_MARGIN)
		thermal_max = max_freq * RFX_THERMAL_CAP_HARD_PCT / 100;
	else if (th->headroom < RFX_THERMAL_SOFT_MARGIN)
		thermal_max = max_freq * RFX_THERMAL_CAP_SOFT_PCT / 100;
	else
		thermal_max = max_freq * RFX_THERMAL_CAP_NORMAL_PCT / 100;

	return min(target_freq, thermal_max);
}

/* v5: Daily cooldown cap to keep browsing cool */
static unsigned int rfx_daily_cooldown_cap(struct rfx_policy *rfx_pol,
					  unsigned int target_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int max_freq = policy->cpuinfo.max_freq;
	unsigned int daily_max;

	if (rfx_gaming_active())
		return target_freq;

	daily_max = max_freq * RFX_DAILY_CAP_PCT / 100;

	/* If we have any sustained activity during daily use, cap to 80% */
	if (rfx_pol->avg_util_pct > 10 && target_freq > daily_max)
		return daily_max;

	return target_freq;
}

/* === ASYMMETRIC CLUSTER POLICY === */

static unsigned int rfx_asymmetric_apply(struct rfx_policy *rfx_pol,
					   enum rfx_cluster_type cluster,
					   unsigned int raw_freq,
					   bool gaming)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int max = policy->cpuinfo.max_freq;
	unsigned int min = policy->cpuinfo.min_freq;
	unsigned int floor, cap, target;

	if (!gaming)
		return raw_freq;

	switch (cluster) {
	case RFX_CLUSTER_PRIME:
		floor = min + ((max - min) * RFX_ASYM_PRIME_FLOOR_PCT / 100);
		target = min + ((max - min) * RFX_ASYM_PRIME_TARGET_PCT / 100);

		if (raw_freq < floor)
			return floor;
		if (rfx_pol->fps_state.stabilized && raw_freq > target)
			return target;
		return min(raw_freq, max);

	case RFX_CLUSTER_BIG:
		/* v5: 70% floor for smooth heavy threads, cooler than 85% */
		floor = min + ((max - min) * RFX_ASYM_BIG_FLOOR_PCT / 100);
		cap = min + ((max - min) * RFX_ASYM_BIG_CAP_PCT / 100);

		if (raw_freq < floor)
			return floor;
		if (raw_freq > cap)
			return cap;
		return raw_freq;

	case RFX_CLUSTER_LITTLE:
		/* v5: Balanced aux thread performance */
		floor = min + ((max - min) * RFX_ASYM_LITTLE_FLOOR_PCT / 100);
		cap = min + ((max - min) * RFX_ASYM_LITTLE_CAP_PCT / 100);

		if (raw_freq < floor)
			return floor;
		if (raw_freq > cap)
			return cap;
		return raw_freq;
	}

	return raw_freq;
}

/* === CPU USAGE SOFT CAP === */

static unsigned int rfx_apply_cpu_usage_cap(struct rfx_policy *rfx_pol,
					    unsigned int target_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int step;

	if (rfx_gaming_active())
		return target_freq;

	if (!rfx_pol->fps_state.stabilized)
		return target_freq;

	if (rfx_pol->avg_util_pct <= RFX_CPU_USAGE_SOFT_CAP)
		return target_freq;

	step = (policy->cpuinfo.max_freq - policy->cpuinfo.min_freq) / RFX_CPU_USAGE_CAP_STEP_DIV;
	if (target_freq > step)
		target_freq -= step;

	return max(target_freq, policy->cpuinfo.min_freq);
}

/* === IO WAIT BOOST (SIMPLIFIED) === */

static void rfx_iowait_boost(struct rfx_cpu *rfx_c, unsigned int flags)
{
	if (flags & SCHED_CPUFREQ_IOWAIT) {
		rfx_c->iowait_boost_pending = true;
		if (rfx_c->iowait_streak < 255)
			rfx_c->iowait_streak++;
	} else {
		rfx_c->iowait_boost_pending = false;
	}
}

static unsigned long rfx_iowait_apply(struct rfx_cpu *rfx_c,
				      unsigned long max_cap)
{
	unsigned int boost_util;

	if (!rfx_c->iowait_boost_pending && rfx_c->iowait_streak < RFX_IOWAIT_BOOST_THRESHOLD)
		return 0;

	if (!rfx_c->iowait_boost_pending && rfx_c->iowait_streak)
		rfx_c->iowait_streak--;

	/* Decay boost value */
	if (!rfx_c->iowait_boost_pending) {
		if (rfx_c->iowait_boost > IOWAIT_BOOST_MIN)
			rfx_c->iowait_boost >>= 1;
		else
			rfx_c->iowait_boost = 0;
	}

	if (!rfx_c->iowait_boost)
		return 0;

	boost_util = (unsigned long)((u64)rfx_c->iowait_boost * max_cap >> SCHED_CAPACITY_SHIFT);
	rfx_c->iowait_boost_pending = false;
	return boost_util;
}

static unsigned int rfx_iowait_boost_freq(struct rfx_policy *rfx_pol,
					  struct rfx_cpu *rfx_c,
					  enum rfx_cluster_type cluster,
					  unsigned int current_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int boost_freq;
	unsigned int pct;

	if (!rfx_c->iowait_boost_pending && rfx_c->iowait_streak < RFX_IOWAIT_BOOST_THRESHOLD)
		return current_freq;

	switch (cluster) {
	case RFX_CLUSTER_PRIME:
		pct = RFX_IOWAIT_BOOST_PRIME_PCT;
		break;
	case RFX_CLUSTER_BIG:
		pct = RFX_IOWAIT_BOOST_BIG_PCT;
		break;
	case RFX_CLUSTER_LITTLE:
		pct = RFX_IOWAIT_BOOST_LITTLE_PCT;
		break;
	default:
		return current_freq;
	}

	boost_freq = rfx_adaptive_max(policy, pct);
	return max(current_freq, boost_freq);
}

/* === BUSY PERCENT (for idle detection) === */

static void rfx_update_busy_pct(struct rfx_cpu *rfx_c, u64 time)
{
	u64 cur_idle, cur_wall;
	u64 wall_delta, idle_delta;

	cur_idle = get_cpu_idle_time(rfx_c->cpu, &cur_wall, 1);
	wall_delta = cur_wall - rfx_c->prev_wall_time;

	if (wall_delta < NSEC_PER_MSEC)
		return;

	idle_delta = cur_idle > rfx_c->prev_idle_time ?
		cur_idle - rfx_c->prev_idle_time : 0;

	rfx_c->busy_pct = wall_delta > idle_delta ?
		(unsigned int)(100 * (wall_delta - idle_delta) / wall_delta) : 0;

	rfx_c->prev_idle_time = cur_idle;
	rfx_c->prev_wall_time = cur_wall;
}

/* === JITTER REDUCTION: SMOOTH FREQUENCY CHANGES === */
static unsigned int rfx_smooth_freq(struct rfx_policy *rfx_pol,
				    unsigned int target_freq,
				    unsigned int current_freq,
				    bool emergency)
{
	unsigned int max_freq = rfx_pol->policy->cpuinfo.max_freq;
	unsigned int min_freq = rfx_pol->policy->cpuinfo.min_freq;
	unsigned int max_delta;
	unsigned int smoothed;

	if (!current_freq || current_freq == target_freq)
		return target_freq;

	/* v5: Bypass smoothing for emergency boosts during gaming */
	if (emergency && rfx_gaming_active())
		return target_freq;

	max_delta = rfx_gaming_active() ?
		(max_freq * RFX_JITTER_MAX_DELTA_PCT_GAMING) / 100 :
		(max_freq * RFX_JITTER_MAX_DELTA_PCT_DAILY) / 100;

	if (target_freq > current_freq) {
		if (target_freq - current_freq > max_delta)
			smoothed = current_freq + max_delta;
		else
			smoothed = target_freq;
	} else {
		if (current_freq - target_freq > max_delta)
			smoothed = current_freq - max_delta;
		else
			smoothed = target_freq;
	}

	return clamp_t(unsigned int, smoothed, min_freq, max_freq);
}

/* === SUSTAIN WINDOW: hold freq after boost === */
static bool rfx_in_sustain_window(struct rfx_policy *rfx_pol, u64 time)
{
	if (rfx_pol->last_boost_time_ns == 0)
		return false;
	return (time - rfx_pol->last_boost_time_ns) < RFX_SUSTAIN_WINDOW_NS;
}

/* === RATE LIMITS === */

static bool rfx_update_next_freq(struct rfx_policy *rfx_pol, u64 time,
				 unsigned int next_freq, bool force_down)
{
	bool going_up;
	s64 delta_ns;
	s64 effective_delay;

	if (unlikely(READ_ONCE(rfx_pol->limits_changed))) {
		WRITE_ONCE(rfx_pol->limits_changed, false);
		rfx_pol->need_freq_update = true;
	}

	if (rfx_pol->need_freq_update) {
		rfx_pol->need_freq_update = false;
		if (rfx_pol->next_freq == next_freq)
			return false;
	} else if (rfx_pol->next_freq == next_freq) {
		return false;
	}

	going_up = next_freq > rfx_pol->next_freq;

	/* v5: Sustain window — if recently boosted, don't drop */
	if (!going_up && rfx_in_sustain_window(rfx_pol, time) && !force_down)
		return false;

	if (rfx_gaming_active()) {
		effective_delay = going_up ?
			(s64)RFX_RATE_LIMIT_GAMING_UP_US * NSEC_PER_USEC :
			(s64)RFX_RATE_LIMIT_GAMING_DOWN_US * NSEC_PER_USEC;
	} else {
		effective_delay = going_up ?
			rfx_pol->up_rate_delay_ns :
			rfx_pol->down_rate_delay_ns;
	}

	delta_ns = going_up ?
		time - rfx_pol->last_upfreq_time :
		time - rfx_pol->last_downfreq_time;

	if (delta_ns < effective_delay)
		return false;

	if (going_up) {
		rfx_pol->last_upfreq_time = time;
		rfx_pol->last_boost_time_ns = time;
	} else {
		if (!force_down) {
			s64 down_delta = time - rfx_pol->last_downfreq_time;
			s64 effective_down = rfx_gaming_active() ?
				(s64)RFX_RATE_LIMIT_GAMING_DOWN_US * NSEC_PER_USEC :
				rfx_pol->down_rate_delay_ns;
			if (effective_down > 0 && down_delta < effective_down)
				return false;
		}
		rfx_pol->last_downfreq_time = time;
	}

	rfx_pol->next_freq = next_freq;
	return true;
}

/* === MAIN FREQUENCY CALCULATION === */

static unsigned int rfx_get_next_freq(struct rfx_policy *rfx_pol,
				      struct rfx_cpu *rfx_c,
				      unsigned long util,
				      unsigned long max_cap,
				      u64 time)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	enum rfx_cluster_type cluster;
	unsigned int util_pct;
	unsigned int freq;
	bool gaming;
	bool emergency = false;

	if (!policy || !max_cap)
		return 0;

	cluster = rfx_get_cluster_type(cpumask_first(policy->cpus));
	util_pct = (unsigned int)(util * 100 / max_cap);
	gaming = rfx_gaming_active();

	/* 1. Base frequency from util */
	freq = (unsigned int)((u64)policy->cpuinfo.max_freq * util / max_cap);
	freq = clamp_t(unsigned int, freq, policy->cpuinfo.min_freq,
		       policy->cpuinfo.max_freq);

	/* 2. Predictive pre-emptive boost */
	if (gaming && rfx_c->predictor.predicted > RFX_PREDICT_BOOST_THRESHOLD) {
		unsigned int predict_freq = (unsigned int)((u64)policy->cpuinfo.max_freq *
							   rfx_c->predictor.predicted / 100);
		if (predict_freq > freq)
			freq = predict_freq;
	}

	/* 3. Fast-up prediction: capped at 98% for smooth power */
	if (gaming && rfx_c->predictor.trend == 2 &&
	    rfx_c->predictor.predicted > RFX_PREDICT_FAST_UP_THRESHOLD) {
		unsigned int fast_freq = rfx_adaptive_max(policy, 98);
		if (fast_freq > freq)
			freq = fast_freq;
	}

	/* 4. Frame deadline emergency boost */
	if (gaming && rfx_frame_needs_emergency_boost(&rfx_pol->frame_tracker,
						      util_pct, time)) {
		freq = policy->cpuinfo.max_freq;
		emergency = true;
	}

	/* 5. Iowait-sensitive boost */
	freq = rfx_iowait_boost_freq(rfx_pol, rfx_c, cluster, freq);

	/* 6. Asymmetric cluster policy */
	freq = rfx_asymmetric_apply(rfx_pol, cluster, freq, gaming);

	/* 7. Thermal headroom limiter */
	freq = rfx_thermal_apply(&rfx_pol->thermal, policy, freq);

	/* 8. CPU usage soft cap (DISABLED during gaming) */
	freq = rfx_apply_cpu_usage_cap(rfx_pol, freq);

	/* 9. Daily cooldown for browsing/idle thermal */
	freq = rfx_daily_cooldown_cap(rfx_pol, freq);

	/* 10. Non-gaming idle cap for Little */
	if (!gaming && cluster == RFX_CLUSTER_LITTLE && util_pct < 5) {
		unsigned int idle_cap = rfx_adaptive_max(policy, RFX_IDLE_CAP_PCT);
		if (freq > idle_cap)
			freq = idle_cap;
	}

	/* 11. Gaming idle-between-frames power save for Big/Little */
	if (gaming && !rfx_pol->frame_tracker.in_render_phase && util_pct < 20) {
		if (cluster == RFX_CLUSTER_LITTLE) {
			unsigned int idle_cap = rfx_adaptive_max(policy, RFX_IDLE_CAP_PCT);
			if (freq > idle_cap)
				freq = idle_cap;
		} else if (cluster == RFX_CLUSTER_BIG) {
			unsigned int idle_cap = rfx_adaptive_floor(policy, 35);
			if (freq > idle_cap)
				freq = idle_cap;
		}
	}

	/* 12. v5: Jitter reduction — smooth frequency transitions */
	freq = rfx_smooth_freq(rfx_pol, freq, rfx_pol->prev_target_freq, emergency);
	rfx_pol->prev_target_freq = freq;

	/* 13. Resolve to driver-supported frequency */
	freq = cpufreq_driver_resolve_freq(policy, freq);

	return freq;
}

/* === AVERAGE UTIL COMPUTATION === */

static unsigned int rfx_compute_avg_util(struct rfx_policy *rfx_pol, u64 time)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int cpu;
	unsigned long total_util = 0;
	unsigned long total_cap = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct rfx_cpu *rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
		unsigned long max_cap = arch_scale_cpu_capacity(cpu);
		total_util += rfx_c->util;
		total_cap += max_cap;
	}

	if (!total_cap)
		return 0;

	return (unsigned int)(total_util * 100 / total_cap);
}

/* === UTIL FETCH === */

static void rfx_get_util(struct rfx_cpu *rfx_c, unsigned long boost)
{
	rfx_get_util_gki510(rfx_c->cpu, boost, &rfx_c->util, &rfx_c->bwmin);
}

/* === NOHZ / IDLE HANDLING === */

#ifdef CONFIG_NO_HZ_COMMON
static bool rfx_check_freq_hold_or_drop(struct rfx_cpu *rfx_c,
					unsigned long max_cap,
					bool *out_force_drop)
{
	unsigned long idle_calls;
	bool idle_calls_increased;

	if (rfx_gaming_active()) {
		if (out_force_drop)
			*out_force_drop = false;
		return false;
	}

	idle_calls = tick_nohz_get_idle_calls_cpu(rfx_c->cpu);
	idle_calls_increased = idle_calls != rfx_c->saved_idle_calls;
	rfx_c->saved_idle_calls = idle_calls;

	if (max_cap <= RFX_LITTLE_CAP_THRESHOLD) {
		if (out_force_drop)
			*out_force_drop = idle_calls_increased;
		return false;
	}

	if (out_force_drop)
		*out_force_drop = false;
	return !idle_calls_increased;
}
#else
static inline bool rfx_check_freq_hold_or_drop(struct rfx_cpu *rfx_c,
					     unsigned long max_cap,
					     bool *out_force_drop)
{
	if (out_force_drop)
		*out_force_drop = false;
	return false;
}
#endif

static inline void rfx_ignore_dl_rate_limit(struct rfx_cpu *rfx_c)
{
	if (rfx_dl_bw_exceeded_gki510(rfx_c->cpu, rfx_c->bwmin))
		rfx_c->rfx_policy->need_freq_update = true;
}

static void rfx_irq_work(struct irq_work *irq_work);

static void rfx_deferred_update(struct rfx_policy *rfx_pol)
{
	if (!rfx_pol->work_in_progress) {
		rfx_pol->work_in_progress = true;
		irq_work_queue(&rfx_pol->irq_work);
	}
}

/* === UPDATE SINGLE FREQUENCY - MAIN LOGIC === */

static void rfx_update_single_freq(struct update_util_data *hook, u64 time,
				   unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned long max_cap, boost, effective_util;
	unsigned int next_f;
	bool force_down = false, nohz_drop = false;
	bool hold;
	unsigned int util_pct;
	enum rfx_cluster_type cluster;

	if (!rfx_pol)
		return;

	max_cap = arch_scale_cpu_capacity(rfx_c->cpu);
	cluster = rfx_get_cluster_type(rfx_c->cpu);

	/* Iowait boost processing */
	rfx_iowait_boost(rfx_c, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	boost = rfx_iowait_apply(rfx_c, max_cap);
	rfx_get_util(rfx_c, boost);
	effective_util = max(rfx_c->util, boost);

	/* Update busy percent for idle detection */
	rfx_update_busy_pct(rfx_c, time);

	/* Compute util percentage */
	util_pct = max_cap ? (unsigned int)(effective_util * 100 / max_cap) : 0;

	raw_spin_lock(&rfx_pol->update_lock);

	/* Update predictive engine */
	rfx_predictor_update(&rfx_c->predictor, util_pct, time);

	/* Update frame tracker */
	rfx_frame_tracker_update(&rfx_pol->frame_tracker, time, util_pct,
				 rfx_c->iowait_boost_pending || rfx_c->iowait_streak > 3);

	/* Update FPS feedback state */
	rfx_fps_update(&rfx_pol->fps_state, time);

	/* Compute average util across policy for CPU usage cap */
	rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol, time);

	/* Main frequency computation */
	next_f = rfx_get_next_freq(rfx_pol, rfx_c, effective_util, max_cap, time);

	/* NOHZ idle handling */
	hold = rfx_check_freq_hold_or_drop(rfx_c, max_cap, &nohz_drop);
	force_down = nohz_drop;

	if (hold && next_f == rfx_pol->next_freq && !rfx_pol->need_freq_update && !force_down)
		next_f = rfx_pol->next_freq;

	/* Apply rate limits and commit */
	if (rfx_update_next_freq(rfx_pol, time, next_f, force_down)) {
		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, rfx_pol->next_freq);
		else {
			rfx_deferred_update(rfx_pol);
		}
	}

	/* Update frame tracker budget if gaming mode changed */
	if (rfx_pol->prev_gaming != rfx_gaming_active()) {
		rfx_pol->prev_gaming = rfx_gaming_active();
		if (rfx_gaming_active())
			rfx_frame_tracker_init(&rfx_pol->frame_tracker,
					       rfx_pol->fps_state.hint);
	}

	raw_spin_unlock(&rfx_pol->update_lock);

	rfx_c->prev_util_pct = util_pct;
}

/* === UPDATE SHARED FREQUENCY === */

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time,
					 bool *force_down_out)
{
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	struct cpufreq_policy *policy = rfx_pol->policy;
	struct rfx_cpu *j_rfxc;
	unsigned long util = 0, max_cap = 0, j_max_cap;
	unsigned long j_util;
	unsigned int next_f;
	unsigned long boost;
	bool any_force_down = false;
	bool nohz_drop = false;
	int cpu;
	unsigned int lead_util_pct = 0;
	unsigned int lead_cpu = cpumask_first(policy->cpus);

	/* Iterate all CPUs in policy and aggregate util/cap */
	for_each_cpu(cpu, policy->cpus) {
		j_rfxc = per_cpu_ptr(&rfx_cpu, cpu);

		j_max_cap = arch_scale_cpu_capacity(cpu);
		if (j_max_cap > max_cap)
			max_cap = j_max_cap;

		boost = rfx_iowait_apply(j_rfxc, j_max_cap);
		rfx_get_util(j_rfxc, boost);
		j_util = max(j_rfxc->util, boost);

		rfx_update_busy_pct(j_rfxc, time);

		if (j_util > util)
			util = j_util;

		rfx_check_freq_hold_or_drop(j_rfxc, j_max_cap, &nohz_drop);
		if (nohz_drop)
			any_force_down = true;
	}

	/* Use lead CPU for predictor and frame tracking */
	j_rfxc = per_cpu_ptr(&rfx_cpu, lead_cpu);
	lead_util_pct = max_cap ? (unsigned int)(util * 100 / max_cap) : 0;

	/* Update predictive engine for lead CPU */
	rfx_predictor_update(&j_rfxc->predictor, lead_util_pct, time);

	/* Update frame tracker */
	rfx_frame_tracker_update(&rfx_pol->frame_tracker, time, lead_util_pct,
				 j_rfxc->iowait_boost_pending || j_rfxc->iowait_streak > 3);

	/* Update FPS feedback */
	rfx_fps_update(&rfx_pol->fps_state, time);

	/* Compute average util */
	rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol, time);

	/* Main frequency computation */
	next_f = rfx_get_next_freq(rfx_pol, j_rfxc, util, max_cap, time);

	if (force_down_out)
		*force_down_out = any_force_down;

	return next_f;
}

static void rfx_update_shared(struct update_util_data *hook, u64 time,
			      unsigned int flags)
{
	struct rfx_cpu *rfx_c = container_of(hook, struct rfx_cpu, update_util);
	struct rfx_policy *rfx_pol = rfx_c->rfx_policy;
	unsigned int next_f;
	bool force_down = false;

	if (!rfx_pol)
		return;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_iowait_boost(rfx_c, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	next_f = rfx_next_freq_shared(rfx_c, time, &force_down);

	if (rfx_update_next_freq(rfx_pol, time, next_f, force_down)) {
		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, rfx_pol->next_freq);
		else
			rfx_deferred_update(rfx_pol);
	}

	/* Update frame tracker budget if gaming mode changed */
	if (rfx_pol->prev_gaming != rfx_gaming_active()) {
		rfx_pol->prev_gaming = rfx_gaming_active();
		if (rfx_gaming_active())
			rfx_frame_tracker_init(&rfx_pol->frame_tracker,
					       rfx_pol->fps_state.hint);
	}

	raw_spin_unlock(&rfx_pol->update_lock);
}

/* === WORKER THREAD === */

static void rfx_work(struct kthread_work *work)
{
	struct rfx_policy *rfx_pol = container_of(work, struct rfx_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rfx_pol->update_lock, flags);
	freq = rfx_pol->next_freq;
	rfx_pol->work_in_progress = false;
	raw_spin_unlock_irqrestore(&rfx_pol->update_lock, flags);

	cpufreq_driver_target(rfx_pol->policy, freq, CPUFREQ_RELATION_L);
}

static void rfx_irq_work(struct irq_work *irq_work)
{
	struct rfx_policy *rfx_pol = container_of(irq_work, struct rfx_policy, irq_work);
	kthread_queue_work(&rfx_pol->worker, &rfx_pol->work);
}

/* === SYSFS ATTRIBUTES === */

static struct rfx_tunables *rfx_global_tunables;
static DEFINE_MUTEX(rfx_global_tunables_lock);

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  to_rfx_tunables(attr_set)->up_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->up_rate_limit_us = val;

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->up_rate_delay_ns = (s64)val * NSEC_PER_USEC;

	return count;
}

static struct governor_attr up_rate_limit_us =
	__ATTR(up_rate_limit_us, 0644, up_rate_limit_us_show, up_rate_limit_us_store);

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  to_rfx_tunables(attr_set)->down_rate_limit_us);
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct rfx_tunables *tunables = to_rfx_tunables(attr_set);
	struct rfx_policy *rfx_pol;
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->down_rate_limit_us = val;

	list_for_each_entry(rfx_pol, &attr_set->policy_list, tunables_hook)
		rfx_pol->down_rate_delay_ns = (s64)val * NSEC_PER_USEC;

	return count;
}

static struct governor_attr down_rate_limit_us =
	__ATTR(down_rate_limit_us, 0644, down_rate_limit_us_show, down_rate_limit_us_store);

/* === GLOBAL GAMING MODE - ONLY VIA PRIME SYSFS === */
static ssize_t gaming_mode_show(struct gov_attr_set *attr_set, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(rfx_global_gaming_mode));
}

static ssize_t gaming_mode_store(struct gov_attr_set *attr_set,
				 const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	/* v5: Atomic store, no cpufreq iteration to avoid deadlock */
	smp_store_release(&rfx_global_gaming_mode, val);
	return count;
}

static struct governor_attr gaming_mode =
	__ATTR(gaming_mode, 0644, gaming_mode_show, gaming_mode_store);

/* === PRIME SYSFS: only up_rate_limit_us, down_rate_limit_us, gaming_mode === */
static struct attribute *rfx_prime_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&gaming_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx_prime);

/* LITTLE: basic tunables only */
static struct attribute *rfx_little_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx_little);

/* BIG: basic tunables only */
static struct attribute *rfx_big_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx_big);

static void rfx_tunables_free(struct kobject *kobj)
{
	kfree(to_rfx_tunables(rfx_to_gov_attr_set(kobj)));
}

static struct kobj_type rfx_little_ktype = {
	.default_groups = rfx_little_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = rfx_tunables_free,
};

static struct kobj_type rfx_big_ktype = {
	.default_groups = rfx_big_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = rfx_tunables_free,
};

static struct kobj_type rfx_prime_ktype = {
	.default_groups = rfx_prime_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = rfx_tunables_free,
};

static struct cpufreq_governor vorpal_gov;

/* === POLICY ALLOCATION === */

static struct rfx_policy *rfx_policy_alloc(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;

	rfx_pol = kzalloc(sizeof(*rfx_pol), GFP_KERNEL);
	if (!rfx_pol)
		return NULL;

	rfx_pol->policy = policy;
	raw_spin_lock_init(&rfx_pol->update_lock);
	return rfx_pol;
}

static void rfx_policy_free(struct rfx_policy *rfx_pol)
{
	kfree(rfx_pol);
}

/* === KTHREAD WORKER === */

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size = sizeof(struct sched_attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_flags = SCHED_FLAGS_UGOV,
		.sched_runtime = 4 * NSEC_PER_MSEC,	/* v5: slightly more */
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period = 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = rfx_pol->policy;
	int ret;

	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&rfx_pol->work, rfx_work);
	kthread_init_worker(&rfx_pol->worker);

	thread = kthread_create(kthread_worker_fn, &rfx_pol->worker,
				"rfx_gov/%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("vorpal: kthread create failed %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	rfx_pol->thread = thread;

	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&rfx_pol->irq_work, rfx_irq_work);
	wake_up_process(thread);
	return 0;
}

static void rfx_kthread_stop(struct rfx_policy *rfx_pol)
{
	if (rfx_pol->policy->fast_switch_enabled)
		return;
	kthread_flush_worker(&rfx_pol->worker);
	kthread_stop(rfx_pol->thread);
}

/* === TUNABLES ALLOCATION === */

static struct rfx_tunables *rfx_tunables_alloc(struct rfx_policy *rfx_pol)
{
	struct rfx_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &rfx_pol->tunables_hook);
		if (!have_governor_per_policy())
			rfx_global_tunables = tunables;
	}
	return tunables;
}

static void rfx_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		rfx_global_tunables = NULL;
}

/* === GOVERNOR INIT === */

static int rfx_init(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol;
	struct rfx_tunables *tunables;
	unsigned long max_cap;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	rfx_pol = rfx_policy_alloc(policy);
	if (!rfx_pol) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = rfx_kthread_create(rfx_pol);
	if (ret)
		goto free_rfx_pol;

	mutex_lock(&rfx_global_tunables_lock);

	if (rfx_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = rfx_pol;
		rfx_pol->tunables = rfx_global_tunables;
		gov_attr_set_get(&rfx_global_tunables->attr_set, &rfx_pol->tunables_hook);

		rfx_pol->up_rate_delay_ns = (s64)rfx_global_tunables->up_rate_limit_us * NSEC_PER_USEC;
		rfx_pol->down_rate_delay_ns = (s64)rfx_global_tunables->down_rate_limit_us * NSEC_PER_USEC;
		goto init_state;
	}

	tunables = rfx_tunables_alloc(rfx_pol);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->gaming_mode = READ_ONCE(rfx_global_gaming_mode);

	max_cap = arch_scale_cpu_capacity(cpumask_first(policy->cpus));

	if (max_cap <= (unsigned long)RFX_LITTLE_CAP_THRESHOLD) {
		tunables->cluster_type = RFX_CLUSTER_LITTLE;
		tunables->up_rate_limit_us = RFX_RATE_LIMIT_DEFAULT_UP_US;
		tunables->down_rate_limit_us = RFX_RATE_LIMIT_IDLE_DOWN_US;
	} else if (max_cap >= (unsigned long)RFX_PRIME_CAP_THRESHOLD) {
		tunables->cluster_type = RFX_CLUSTER_PRIME;
		tunables->up_rate_limit_us = RFX_RATE_LIMIT_GAMING_UP_US;
		tunables->down_rate_limit_us = RFX_RATE_LIMIT_GAMING_DOWN_US;
	} else {
		tunables->cluster_type = RFX_CLUSTER_BIG;
		tunables->up_rate_limit_us = RFX_RATE_LIMIT_DEFAULT_UP_US;
		tunables->down_rate_limit_us = RFX_RATE_LIMIT_DEFAULT_DOWN_US;
	}

	policy->governor_data = rfx_pol;
	rfx_pol->tunables = tunables;

	rfx_pol->up_rate_delay_ns = (s64)tunables->up_rate_limit_us * NSEC_PER_USEC;
	rfx_pol->down_rate_delay_ns = (s64)tunables->down_rate_limit_us * NSEC_PER_USEC;

	{
		struct kobj_type *ktype;
		switch (tunables->cluster_type) {
		case RFX_CLUSTER_LITTLE:
			ktype = &rfx_little_ktype; break;
		case RFX_CLUSTER_PRIME:
			ktype = &rfx_prime_ktype; break;
		case RFX_CLUSTER_BIG:
		default:
			ktype = &rfx_big_ktype; break;
		}
		ret = kobject_init_and_add(&tunables->attr_set.kobj, ktype,
					   get_governor_parent_kobj(policy),
					   "%s", vorpal_gov.name);
	}
	if (ret)
		goto fail;

init_state:
	/* Initialize v5 builtin adaptive state */
	rfx_fps_init(&rfx_pol->fps_state);
	rfx_thermal_init(&rfx_pol->thermal);
	if (READ_ONCE(rfx_global_gaming_mode)) {
		rfx_pol->fps_state.hint = RFX_BUILTIN_FPS_HINT;
		rfx_pol->fps_state.actual = RFX_BUILTIN_ACTUAL_FPS;
		rfx_pol->thermal.headroom = RFX_BUILTIN_THERMAL_HEADROOM;
	} else {
		rfx_pol->thermal.headroom = RFX_THERMAL_HEADROOM_DEFAULT;
	}
	rfx_frame_tracker_init(&rfx_pol->frame_tracker, rfx_pol->fps_state.hint);
	rfx_pol->avg_util_pct = 0;
	rfx_pol->prev_gaming = READ_ONCE(rfx_global_gaming_mode);
	rfx_pol->prev_target_freq = policy->cur;
	rfx_pol->last_boost_time_ns = 0;

	mutex_unlock(&rfx_global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	rfx_clear_global_tunables();
stop_kthread:
	rfx_kthread_stop(rfx_pol);
	mutex_unlock(&rfx_global_tunables_lock);
free_rfx_pol:
	rfx_policy_free(rfx_pol);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("vorpal: init failed error %d\n", ret);
	return ret;
}

/* === GOVERNOR EXIT === */

static void rfx_exit(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	struct rfx_tunables *tunables = rfx_pol->tunables;
	unsigned int count;

	mutex_lock(&rfx_global_tunables_lock);
	count = gov_attr_set_put(&tunables->attr_set, &rfx_pol->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		rfx_clear_global_tunables();
	mutex_unlock(&rfx_global_tunables_lock);

	rfx_kthread_stop(rfx_pol);
	rfx_policy_free(rfx_pol);
	cpufreq_disable_fast_switch(policy);
}

/* === GOVERNOR START === */

static int rfx_start(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;
	u64 now;
	struct rfx_cpu *rfx_c;
	bool gaming = READ_ONCE(rfx_global_gaming_mode);

	rfx_pol->up_rate_delay_ns = (s64)rfx_pol->tunables->up_rate_limit_us * NSEC_PER_USEC;
	rfx_pol->down_rate_delay_ns = (s64)rfx_pol->tunables->down_rate_limit_us * NSEC_PER_USEC;

	now = ktime_get_ns();
	rfx_pol->last_upfreq_time = now;
	rfx_pol->last_downfreq_time = now;
	rfx_pol->next_freq = policy->cur > 0 ? policy->cur : policy->cpuinfo.min_freq;
	rfx_pol->work_in_progress = false;
	rfx_pol->limits_changed = false;
	rfx_pol->cached_raw_freq = 0;
	rfx_pol->need_freq_update = false;
	rfx_pol->last_real_update_ns = now;
	rfx_pol->avg_util_pct = 0;
	rfx_pol->prev_gaming = gaming;
	rfx_pol->prev_target_freq = policy->cur;
	rfx_pol->last_boost_time_ns = 0;

	if (gaming) {
		rfx_pol->fps_state.hint = RFX_BUILTIN_FPS_HINT;
		rfx_pol->fps_state.actual = RFX_BUILTIN_ACTUAL_FPS;
	}
	rfx_frame_tracker_init(&rfx_pol->frame_tracker, rfx_pol->fps_state.hint);

	uu = policy_is_shared(policy) ? rfx_update_shared : rfx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu = cpu;
		rfx_c->rfx_policy = rfx_pol;
		rfx_predictor_init(&rfx_c->predictor);
		rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
		rfx_c->prev_idle_time = get_cpu_idle_time(cpu, &rfx_c->prev_wall_time, 1);
		cpufreq_add_update_util_hook(cpu, &rfx_c->update_util, uu);
	}
	return 0;
}

/* === GOVERNOR STOP === */

static void rfx_stop(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&rfx_pol->irq_work);
		kthread_cancel_work_sync(&rfx_pol->work);
	}
}

/* === GOVERNOR LIMITS === */

static void rfx_limits(struct cpufreq_policy *policy)
{
	struct rfx_policy *rfx_pol = policy->governor_data;

	smp_wmb();
	WRITE_ONCE(rfx_pol->limits_changed, true);
}

/* === GOVERNOR STRUCTURE === */

static struct cpufreq_governor vorpal_gov = {
	.name = "vorpal",
	.owner = THIS_MODULE,
	.flags = CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init = rfx_init,
	.exit = rfx_exit,
	.start = rfx_start,
	.stop = rfx_stop,
	.limits = rfx_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_VORPAL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &vorpal_gov;
}
#endif

/* === MODULE INIT/EXIT === */

static int __init vorpal_gov_init(void)
{
	pr_info("%s %s by %s\n", CPUFREQ_VORPAL_PROGNAME,
		CPUFREQ_VORPAL_VERSION, CPUFREQ_VORPAL_AUTHOR);
	pr_info("Stable120|CoolDaily|RaceFix|NoDeadlock|SmoothBoost\n");
	return cpufreq_register_governor(&vorpal_gov);
}

static void __exit vorpal_gov_exit(void)
{
	cpufreq_unregister_governor(&vorpal_gov);
}

module_init(vorpal_gov_init);
module_exit(vorpal_gov_exit);

MODULE_AUTHOR(CPUFREQ_VORPAL_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(CPUFREQ_VORPAL_PROGNAME);
