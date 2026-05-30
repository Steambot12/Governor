// SPDX-License-Identifier: GPL-2.0
/*
 * Vorpal CPUFreq Governor v6.6 — Smarter AI Adaptive Scene Controller
 *
 * Gaming-mode-on: built-in AI scene classifier with twitch detection,
 * asymmetric transitions (fast-up, slow-down), warmup period, and
 * minimum scene dwell. Optimized for FPS shooters where shooting bursts
 * cause sub-frame util spikes that previous symmetric transitions were
 * too slow to catch.
 *
 * Daily mode: passthrough. AI inactive.
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
#define CPUFREQ_VORPAL_VERSION	"6.6-AISmart"

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
#define RFX_BUILTIN_THERMAL_HEADROOM	30

/* === PREDICTIVE ENGINE (diagnostic only) === */
#define RFX_PREDICTOR_WINDOW		16

/* === FRAME DEADLINE === */
#define RFX_FRAME_BUDGET_120HZ_NS	8333333ULL
#define RFX_FRAME_BUDGET_90HZ_NS	11111111ULL
#define RFX_FRAME_BUDGET_60HZ_NS	16666667ULL
#define RFX_FRAME_EMERGENCY_NS		5000000ULL
#define RFX_FRAME_EMERGENCY_UTIL_PCT	18
#define RFX_FRAME_PHASE_THRESHOLD_PCT	8
#define RFX_FRAME_PHASE_EXIT_PCT	4

/* === JITTER REDUCTION === */
#define RFX_JITTER_MAX_DELTA_PCT_GAMING	8
#define RFX_JITTER_MAX_DELTA_PCT_DAILY	5
#define RFX_SUSTAIN_WINDOW_NS		50000000ULL
#define RFX_BURST_HOLD_NS		83000000ULL

/* === ASYMMETRIC CLUSTER POLICY ===
 * Adaptive sustain: scene controller picks dynamic sustain pct.
 * NORMAL is the most common scene during steady gameplay; lowered to
 * reduce CPU usage. TWITCH override + asymmetric fast-up handles
 * sub-frame spikes (FPS shooting) instantly.
 */
#define RFX_ASYM_PRIME_SUSTAIN_LIGHT_PCT	65
#define RFX_ASYM_PRIME_SUSTAIN_NORMAL_PCT	80
#define RFX_ASYM_PRIME_SUSTAIN_INTENSE_PCT	95
#define RFX_ASYM_PRIME_TARGET_PCT		100

#define RFX_ASYM_BIG_SUSTAIN_LIGHT_PCT	55
#define RFX_ASYM_BIG_SUSTAIN_NORMAL_PCT	70
#define RFX_ASYM_BIG_SUSTAIN_INTENSE_PCT	88
#define RFX_ASYM_BIG_CAP_PCT			92

#define RFX_ASYM_LITTLE_CAP_LIGHT_PCT	30
#define RFX_ASYM_LITTLE_CAP_INTENSE_PCT	55

/* === AI SCENE CLASSIFIER (gaming-only, built-in, no sysfs) ===
 * Three scenes — LIGHT / NORMAL / INTENSE — plus TWITCH override for
 * sub-frame spikes (FPS shooting, recoil). Classification inputs:
 *   - util EMA (smoothed cluster utilization)
 *   - util variance (jitter signal)
 *   - burst frequency from frame tracker
 *   - predictor trend (-2..+2)
 *   - single-tick util delta (twitch detection)
 * Hysteresis + minimum dwell time prevents flapping.
 * Asymmetric transition: fast UP, slow DOWN (anti-jitter).
 * Warmup period (2s after gaming_mode=1): forced INTENSE for loading.
 */
#define RFX_SCENE_HIST_SIZE		8	/* power-of-2 */
#define RFX_SCENE_LIGHT_EMA_PCT		28
#define RFX_SCENE_NORMAL_EMA_PCT	50
#define RFX_SCENE_HYSTERESIS_PCT	5
#define RFX_SCENE_BURST_INTENSE_NS	(2ULL * NSEC_PER_SEC)
#define RFX_SCENE_VARIANCE_INTENSE	160	/* sum-of-abs-diff threshold */
#define RFX_SCENE_TRANSITION_UP_STEP	4	/* pct/tick when scaling up */
#define RFX_SCENE_TRANSITION_DOWN_STEP	1	/* pct/tick when scaling down */
#define RFX_SCENE_MIN_DWELL_NS		(150ULL * NSEC_PER_MSEC)
#define RFX_SCENE_TWITCH_DELTA_PCT	25	/* single-tick spike threshold */
#define RFX_SCENE_TWITCH_HOLD_NS	(50ULL * NSEC_PER_MSEC)	/* ~6 frames */
#define RFX_SCENE_WARMUP_NS		(2ULL * NSEC_PER_SEC)

/* === CPU USAGE SOFT CAP (daily only) === */
#define RFX_CPU_USAGE_SOFT_CAP		55
#define RFX_CPU_USAGE_CAP_STEP_DIV	14

/* === IO WAIT BOOST === */
#define RFX_IOWAIT_BOOST_THRESHOLD	2
#define RFX_IOWAIT_BOOST_CAP_DAILY	(SCHED_CAPACITY_SCALE / 2)
#define RFX_IOWAIT_BOOST_CAP_GAMING	(SCHED_CAPACITY_SCALE * 3 / 4)
#define RFX_IOWAIT_BOOST_STEP		(SCHED_CAPACITY_SCALE / 8)

/* === THERMAL ===
 * Gaming: only throttle at the genuine cliff (headroom <2). Soft cap
 * 100% — never preemptively cap perf during gaming.
 */
#define RFX_THERMAL_HEADROOM_DEFAULT	30
#define RFX_THERMAL_HARD_MARGIN		2
#define RFX_THERMAL_SOFT_MARGIN		8
#define RFX_THERMAL_CAP_HARD_PCT_GAMING	95
#define RFX_THERMAL_CAP_SOFT_PCT_GAMING	100
#define RFX_THERMAL_CAP_HARD_PCT_DAILY	78
#define RFX_THERMAL_CAP_SOFT_PCT_DAILY	90
#define RFX_THERMAL_CAP_NORMAL_PCT	100

/* === DAILY COOLDOWN === */
#define RFX_DAILY_CAP_PCT		78

/* === RATE LIMITS === */
#define RFX_RATE_LIMIT_GAMING_UP_US	0
#define RFX_RATE_LIMIT_GAMING_DOWN_US	25000
#define RFX_RATE_LIMIT_DEFAULT_UP_US	500
#define RFX_RATE_LIMIT_DEFAULT_DOWN_US	18000
#define RFX_RATE_LIMIT_IDLE_DOWN_US	4000

/* === IDLE === */
#define RFX_IDLE_CAP_PCT		10

/* === EMA === */
#define RFX_EMA_ALPHA_NUM		55
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

enum rfx_scene {
	RFX_SCENE_LIGHT = 0,
	RFX_SCENE_NORMAL = 1,
	RFX_SCENE_INTENSE = 2,
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
	u64 last_burst_ns;
};

struct rfx_fps_state {
	unsigned int hint;
	unsigned int actual;
};

struct rfx_thermal_state {
	int headroom;
	bool active;
};

struct rfx_scene_state {
	u8 hist[RFX_SCENE_HIST_SIZE];
	u8 idx;
	enum rfx_scene scene;
	unsigned int prime_sustain_pct;
	unsigned int big_sustain_pct;
	unsigned int little_cap_pct;
	unsigned int target_prime_pct;
	unsigned int target_big_pct;
	unsigned int target_little_pct;
	u32 burst_count;
	u64 burst_window_start_ns;
	unsigned int variance;
	unsigned int last_util_pct;
	u64 scene_entry_ns;
	u64 twitch_until_ns;
	u64 gaming_started_ns;
};

struct rfx_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	enum rfx_cluster_type cluster_type;
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
	struct irq_work irq_work;
	struct kthread_work work;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;
	bool limits_changed;
	bool need_freq_update;

	struct rfx_frame_tracker frame_tracker;
	struct rfx_fps_state fps_state;
	struct rfx_thermal_state thermal;
	struct rfx_scene_state scene;
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

	if (rfx_gaming_active())
		p->ema = (RFX_EMA_ALPHA_NUM * util_pct +
			  (RFX_EMA_ALPHA_DEN - RFX_EMA_ALPHA_NUM) * old_ema) / RFX_EMA_ALPHA_DEN;
	else
		p->ema = (3 * util_pct + 7 * old_ema) / 10;

	for (i = 0; i < 4; i++) {
		recent_sum += p->hist[(p->idx - 1 - i) & (RFX_PREDICTOR_WINDOW - 1)];
		prev_sum += p->hist[(p->idx - 5 - i) & (RFX_PREDICTOR_WINDOW - 1)];
	}
	recent_avg = recent_sum >> 2;
	prev_avg = prev_sum >> 2;

	if (recent_avg > prev_avg + 15)
		p->trend = 2;
	else if (recent_avg > prev_avg + 8)
		p->trend = 1;
	else if (recent_avg + 15 < prev_avg)
		p->trend = -2;
	else if (recent_avg + 8 < prev_avg)
		p->trend = -1;
	else
		p->trend = 0;

	p->predicted = p->ema;
	p->last_update_ns = time;
}

/* === FRAME DEADLINE TRACKER === */

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
		if (util_pct < RFX_FRAME_PHASE_EXIT_PCT &&
		    elapsed > ft->budget_ns / 2) {
			if (ft->phase_ns > (ft->budget_ns * 80 / 100))
				ft->last_burst_ns = time;
			ft->in_render_phase = false;
		}
	}

	ft->phase_ns = time - ft->last_frame_ns;
}

static inline bool rfx_frame_in_burst_hold(const struct rfx_frame_tracker *ft,
					   u64 time)
{
	if (!ft->last_burst_ns)
		return false;
	return (time - ft->last_burst_ns) < RFX_BURST_HOLD_NS;
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

	if (remaining < RFX_FRAME_EMERGENCY_NS && util_pct > RFX_FRAME_EMERGENCY_UTIL_PCT)
		return true;

	if (ft->phase_ns > ft->budget_ns)
		return true;

	if (ft->phase_ns > (ft->budget_ns * 70 / 100) && util_pct > RFX_FRAME_EMERGENCY_UTIL_PCT)
		return true;

	return false;
}

/* === FPS HINT === */

static void rfx_fps_init(struct rfx_fps_state *fps)
{
	memset(fps, 0, sizeof(*fps));
}

/* === THERMAL HEADROOM === */

static void rfx_thermal_init(struct rfx_thermal_state *th)
{
	th->headroom = RFX_THERMAL_HEADROOM_DEFAULT;
	th->active = true;
}

/* Update headroom from arch thermal pressure (live throttle signal). */
static void rfx_thermal_refresh(struct rfx_thermal_state *th, int cpu)
{
	unsigned long pressure = arch_scale_thermal_pressure(cpu);
	int hr;

	if (pressure >= SCHED_CAPACITY_SCALE) {
		th->headroom = 0;
		return;
	}
	hr = RFX_THERMAL_HEADROOM_DEFAULT -
	     (int)((pressure * RFX_THERMAL_HEADROOM_DEFAULT) >> SCHED_CAPACITY_SHIFT);
	if (hr < 0)
		hr = 0;
	th->headroom = hr;
}

static unsigned int rfx_thermal_apply(struct rfx_thermal_state *th,
				      struct cpufreq_policy *policy,
				      unsigned int target_freq)
{
	unsigned int max_freq = policy->cpuinfo.max_freq;
	unsigned int thermal_max;
	unsigned int hard_pct, soft_pct;
	bool gaming = rfx_gaming_active();

	if (!th->active || th->headroom > RFX_THERMAL_SOFT_MARGIN)
		return target_freq;

	hard_pct = gaming ? RFX_THERMAL_CAP_HARD_PCT_GAMING :
			    RFX_THERMAL_CAP_HARD_PCT_DAILY;
	soft_pct = gaming ? RFX_THERMAL_CAP_SOFT_PCT_GAMING :
			    RFX_THERMAL_CAP_SOFT_PCT_DAILY;

	if (th->headroom < RFX_THERMAL_HARD_MARGIN)
		thermal_max = max_freq * hard_pct / 100;
	else if (th->headroom < RFX_THERMAL_SOFT_MARGIN)
		thermal_max = max_freq * soft_pct / 100;
	else
		thermal_max = max_freq * RFX_THERMAL_CAP_NORMAL_PCT / 100;

	return min(target_freq, thermal_max);
}

static unsigned int rfx_daily_cooldown_cap(struct rfx_policy *rfx_pol,
					  unsigned int target_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int daily_max;

	if (rfx_gaming_active())
		return target_freq;

	daily_max = policy->cpuinfo.max_freq * RFX_DAILY_CAP_PCT / 100;
	if (rfx_pol->avg_util_pct > 10 && target_freq > daily_max)
		return daily_max;

	return target_freq;
}

/* === AI ADAPTIVE SCENE CLASSIFIER === */

static void rfx_scene_init(struct rfx_scene_state *s)
{
	memset(s, 0, sizeof(*s));
	s->scene = RFX_SCENE_NORMAL;
	s->prime_sustain_pct = RFX_ASYM_PRIME_SUSTAIN_NORMAL_PCT;
	s->big_sustain_pct = RFX_ASYM_BIG_SUSTAIN_NORMAL_PCT;
	s->little_cap_pct = (RFX_ASYM_LITTLE_CAP_LIGHT_PCT +
			     RFX_ASYM_LITTLE_CAP_INTENSE_PCT) / 2;
	s->target_prime_pct = s->prime_sustain_pct;
	s->target_big_pct = s->big_sustain_pct;
	s->target_little_pct = s->little_cap_pct;
}

/* Sum-of-absolute-differences across hist as variance proxy */
static unsigned int rfx_scene_variance(const struct rfx_scene_state *s)
{
	unsigned int i, sum = 0;
	u8 prev = s->hist[(s->idx - 1) & (RFX_SCENE_HIST_SIZE - 1)];

	for (i = 0; i < RFX_SCENE_HIST_SIZE; i++) {
		u8 cur = s->hist[(s->idx + i) & (RFX_SCENE_HIST_SIZE - 1)];
		sum += (cur > prev) ? (cur - prev) : (prev - cur);
		prev = cur;
	}
	return sum;
}

static enum rfx_scene rfx_scene_classify(struct rfx_scene_state *s,
					 unsigned int ema_pct,
					 int trend,
					 u64 now_ns,
					 bool burst_recent)
{
	unsigned int variance = rfx_scene_variance(s);
	enum rfx_scene cur = s->scene;
	unsigned int hys = RFX_SCENE_HYSTERESIS_PCT;
	bool burst_active;

	s->variance = variance;

	/* Warmup period: forced INTENSE for first 2s of gaming_mode=1
	 * to absorb initial loading/asset-stream/UI render rush.
	 */
	if (s->gaming_started_ns &&
	    now_ns - s->gaming_started_ns < RFX_SCENE_WARMUP_NS)
		return RFX_SCENE_INTENSE;

	/* Twitch override: sub-frame spike (FPS shooting, recoil) */
	if (s->twitch_until_ns && now_ns < s->twitch_until_ns)
		return RFX_SCENE_INTENSE;

	if (burst_recent) {
		if (!s->burst_window_start_ns ||
		    now_ns - s->burst_window_start_ns > RFX_SCENE_BURST_INTENSE_NS) {
			s->burst_window_start_ns = now_ns;
			s->burst_count = 1;
		} else {
			s->burst_count++;
		}
	} else if (s->burst_window_start_ns &&
		   now_ns - s->burst_window_start_ns > RFX_SCENE_BURST_INTENSE_NS) {
		s->burst_count = 0;
		s->burst_window_start_ns = 0;
	}

	burst_active = s->burst_count >= 3;

	/* Min-dwell: prevent flapping by holding scene for at least 150ms */
	if (s->scene_entry_ns &&
	    now_ns - s->scene_entry_ns < RFX_SCENE_MIN_DWELL_NS)
		return cur;

	/* INTENSE: high util OR rapid bursts OR fast-up trend with mid util */
	if (ema_pct > RFX_SCENE_NORMAL_EMA_PCT + hys || burst_active ||
	    (trend >= 1 && ema_pct > RFX_SCENE_LIGHT_EMA_PCT + hys) ||
	    variance > RFX_SCENE_VARIANCE_INTENSE)
		return RFX_SCENE_INTENSE;

	/* LIGHT: low util, no recent burst, stable trend */
	if (ema_pct < RFX_SCENE_LIGHT_EMA_PCT - hys && !s->burst_count &&
	    trend <= 0 && variance < RFX_SCENE_VARIANCE_INTENSE / 3)
		return RFX_SCENE_LIGHT;

	/* Hysteresis: keep current scene at boundary */
	if (cur == RFX_SCENE_INTENSE && ema_pct > RFX_SCENE_NORMAL_EMA_PCT - hys)
		return RFX_SCENE_INTENSE;
	if (cur == RFX_SCENE_LIGHT && ema_pct < RFX_SCENE_LIGHT_EMA_PCT + hys)
		return RFX_SCENE_LIGHT;

	return RFX_SCENE_NORMAL;
}

static void rfx_scene_update_targets(struct rfx_scene_state *s)
{
	switch (s->scene) {
	case RFX_SCENE_LIGHT:
		s->target_prime_pct = RFX_ASYM_PRIME_SUSTAIN_LIGHT_PCT;
		s->target_big_pct = RFX_ASYM_BIG_SUSTAIN_LIGHT_PCT;
		s->target_little_pct = RFX_ASYM_LITTLE_CAP_LIGHT_PCT;
		break;
	case RFX_SCENE_NORMAL:
		s->target_prime_pct = RFX_ASYM_PRIME_SUSTAIN_NORMAL_PCT;
		s->target_big_pct = RFX_ASYM_BIG_SUSTAIN_NORMAL_PCT;
		s->target_little_pct = (RFX_ASYM_LITTLE_CAP_LIGHT_PCT +
					RFX_ASYM_LITTLE_CAP_INTENSE_PCT) / 2;
		break;
	case RFX_SCENE_INTENSE:
		s->target_prime_pct = RFX_ASYM_PRIME_SUSTAIN_INTENSE_PCT;
		s->target_big_pct = RFX_ASYM_BIG_SUSTAIN_INTENSE_PCT;
		s->target_little_pct = RFX_ASYM_LITTLE_CAP_INTENSE_PCT;
		break;
	}
}

/* Asymmetric: fast UP (catch spike), slow DOWN (anti-jitter) */
static void rfx_scene_step_smooth(unsigned int *cur, unsigned int target)
{
	if (*cur < target)
		*cur += min(target - *cur, (unsigned int)RFX_SCENE_TRANSITION_UP_STEP);
	else if (*cur > target)
		*cur -= min(*cur - target, (unsigned int)RFX_SCENE_TRANSITION_DOWN_STEP);
}

static void rfx_scene_tick(struct rfx_policy *rfx_pol,
			   unsigned int ema_pct,
			   unsigned int util_pct,
			   int trend,
			   u64 now_ns,
			   bool burst_recent)
{
	struct rfx_scene_state *s = &rfx_pol->scene;
	enum rfx_scene new_scene;
	unsigned int delta;

	s->hist[s->idx] = (u8)min(ema_pct, 100U);
	s->idx = (s->idx + 1) & (RFX_SCENE_HIST_SIZE - 1);

	/* Twitch detection: single-tick util jump >= 25 pct */
	if (s->last_util_pct && util_pct > s->last_util_pct) {
		delta = util_pct - s->last_util_pct;
		if (delta >= RFX_SCENE_TWITCH_DELTA_PCT)
			s->twitch_until_ns = now_ns + RFX_SCENE_TWITCH_HOLD_NS;
	}
	s->last_util_pct = util_pct;

	new_scene = rfx_scene_classify(s, ema_pct, trend, now_ns, burst_recent);
	if (new_scene != s->scene) {
		s->scene = new_scene;
		s->scene_entry_ns = now_ns;
	}
	rfx_scene_update_targets(s);

	rfx_scene_step_smooth(&s->prime_sustain_pct, s->target_prime_pct);
	rfx_scene_step_smooth(&s->big_sustain_pct, s->target_big_pct);
	rfx_scene_step_smooth(&s->little_cap_pct, s->target_little_pct);
}

/* === ASYMMETRIC CLUSTER POLICY (AI-driven) ===
 *
 * Uses scene-controller dynamic sustain pct. Burst hold + emergency
 * override sustain when frame deadline is at risk.
 */
static unsigned int rfx_asymmetric_apply(struct rfx_policy *rfx_pol,
					   enum rfx_cluster_type cluster,
					   unsigned int raw_freq,
					   bool gaming,
					   u64 time)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	struct rfx_scene_state *s = &rfx_pol->scene;
	unsigned int max = policy->cpuinfo.max_freq;
	unsigned int min = policy->cpuinfo.min_freq;
	unsigned int range = max - min;
	unsigned int sustain, target, cap;
	bool burst_hold;

	if (!gaming)
		return raw_freq;

	burst_hold = rfx_frame_in_burst_hold(&rfx_pol->frame_tracker, time);

	switch (cluster) {
	case RFX_CLUSTER_PRIME:
		sustain = min + (range * s->prime_sustain_pct / 100);
		target = min + (range * RFX_ASYM_PRIME_TARGET_PCT / 100);
		if (burst_hold || raw_freq > sustain)
			return min(raw_freq > target ? target : raw_freq, target);
		return sustain;

	case RFX_CLUSTER_BIG:
		sustain = min + (range * s->big_sustain_pct / 100);
		cap = min + (range * RFX_ASYM_BIG_CAP_PCT / 100);
		if (burst_hold || raw_freq > sustain)
			return min(raw_freq, cap);
		return sustain;

	case RFX_CLUSTER_LITTLE:
		cap = min + (range * s->little_cap_pct / 100);
		return min(raw_freq, cap);
	}

	return raw_freq;
}

/* === CPU USAGE SOFT CAP (daily only) === */

static unsigned int rfx_apply_cpu_usage_cap(struct rfx_policy *rfx_pol,
					    unsigned int target_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int step;

	if (rfx_gaming_active())
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
	unsigned int cap = rfx_gaming_active() ?
			   RFX_IOWAIT_BOOST_CAP_GAMING :
			   RFX_IOWAIT_BOOST_CAP_DAILY;

	if (flags & SCHED_CPUFREQ_IOWAIT) {
		rfx_c->iowait_boost_pending = true;
		if (rfx_c->iowait_streak < 255)
			rfx_c->iowait_streak++;

		if (rfx_c->iowait_boost) {
			rfx_c->iowait_boost = min(rfx_c->iowait_boost +
						  RFX_IOWAIT_BOOST_STEP, cap);
		} else {
			rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
		}
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

/* === JITTER REDUCTION: SMOOTH DOWN ONLY === */
static unsigned int rfx_smooth_freq(struct rfx_policy *rfx_pol,
				    unsigned int target_freq,
				    unsigned int current_freq,
				    bool emergency)
{
	unsigned int max_freq = rfx_pol->policy->cpuinfo.max_freq;
	unsigned int min_freq = rfx_pol->policy->cpuinfo.min_freq;
	unsigned int max_delta;
	unsigned int smoothed;
	bool gaming = rfx_gaming_active();

	if (!current_freq || current_freq == target_freq)
		return target_freq;

	if (emergency && gaming)
		return target_freq;

	/* Upward transitions in gaming bypass smoothing — let asymmetric
	 * floors and predictor jump freq instantly. Only graceful downward
	 * transitions are smoothed to avoid abrupt drop = jitter.
	 */
	if (target_freq > current_freq) {
		if (gaming)
			return target_freq;
		max_delta = (max_freq * RFX_JITTER_MAX_DELTA_PCT_DAILY) / 100;
		if (target_freq - current_freq > max_delta)
			smoothed = current_freq + max_delta;
		else
			smoothed = target_freq;
		return clamp_t(unsigned int, smoothed, min_freq, max_freq);
	}

	max_delta = gaming ?
		(max_freq * RFX_JITTER_MAX_DELTA_PCT_GAMING) / 100 :
		(max_freq * RFX_JITTER_MAX_DELTA_PCT_DAILY) / 100;

	if (current_freq - target_freq > max_delta)
		smoothed = current_freq - max_delta;
	else
		smoothed = target_freq;

	return clamp_t(unsigned int, smoothed, min_freq, max_freq);
}

/* === SUSTAIN WINDOW: hold freq after boost (gaming-only) === */
static bool rfx_in_sustain_window(struct rfx_policy *rfx_pol, u64 time)
{
	if (!rfx_gaming_active())
		return false;
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

	/* Sustain window — if recently boosted, don't drop */
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
		rfx_pol->last_downfreq_time = time;
	}

	rfx_pol->next_freq = next_freq;
	return true;
}

/* === MAIN FREQUENCY CALCULATION ===
 *
 * Pipeline:
 *   1. Base freq from util util/max_cap.
 *   2. Frame deadline emergency boost (gaming-only) → max freq.
 *   3. Asymmetric cluster lock (gaming-only) → sustain/cap.
 *   4. Thermal headroom (gaming has soft 100% cap; daily has 78/90%).
 *   5. Daily soft caps (cooldown + idle little).
 *   6. Smooth-down (jitter floor for graceful drain; up bypassed in gaming).
 *   7. Resolve to driver-supported freq.
 *
 * Predictor is now diagnostic-only (used by future tunables); its EMA/trend
 * does not feed directly into freq calc — eliminates tug-of-war with the
 * asymmetric lock and removes a class of false-positive boost spikes.
 */
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

	freq = (unsigned int)((u64)policy->cpuinfo.max_freq * util / max_cap);
	freq = clamp_t(unsigned int, freq, policy->cpuinfo.min_freq,
		       policy->cpuinfo.max_freq);

	if (gaming && rfx_frame_needs_emergency_boost(&rfx_pol->frame_tracker,
						      util_pct, time)) {
		freq = policy->cpuinfo.max_freq;
		emergency = true;
	}

	freq = rfx_asymmetric_apply(rfx_pol, cluster, freq, gaming, time);
	freq = rfx_thermal_apply(&rfx_pol->thermal, policy, freq);
	freq = rfx_apply_cpu_usage_cap(rfx_pol, freq);
	freq = rfx_daily_cooldown_cap(rfx_pol, freq);

	if (!gaming && cluster == RFX_CLUSTER_LITTLE && util_pct < 5) {
		unsigned int idle_cap = rfx_adaptive_max(policy, RFX_IDLE_CAP_PCT);
		if (freq > idle_cap)
			freq = idle_cap;
	}

	freq = rfx_smooth_freq(rfx_pol, freq, rfx_pol->prev_target_freq, emergency);
	rfx_pol->prev_target_freq = freq;

	freq = cpufreq_driver_resolve_freq(policy, freq);

	return freq;
}

/* === AVERAGE UTIL COMPUTATION === */

static unsigned int rfx_compute_avg_util(struct rfx_policy *rfx_pol)
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

	if (!rfx_pol)
		return;

	max_cap = arch_scale_cpu_capacity(rfx_c->cpu);

	rfx_iowait_boost(rfx_c, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	boost = rfx_iowait_apply(rfx_c, max_cap);
	rfx_get_util(rfx_c, boost);
	effective_util = max(rfx_c->util, boost);

	util_pct = max_cap ? (unsigned int)(effective_util * 100 / max_cap) : 0;

	raw_spin_lock(&rfx_pol->update_lock);

	rfx_predictor_update(&rfx_c->predictor, util_pct, time);
	rfx_thermal_refresh(&rfx_pol->thermal, rfx_c->cpu);
	rfx_frame_tracker_update(&rfx_pol->frame_tracker, time, util_pct,
				 rfx_c->iowait_boost_pending || rfx_c->iowait_streak > 3);
	rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol);

	if (rfx_gaming_active())
		rfx_scene_tick(rfx_pol, rfx_c->predictor.ema, util_pct,
			       rfx_c->predictor.trend, time,
			       rfx_pol->frame_tracker.last_burst_ns &&
			       (time - rfx_pol->frame_tracker.last_burst_ns) <
			       (100 * NSEC_PER_MSEC));

	next_f = rfx_get_next_freq(rfx_pol, rfx_c, effective_util, max_cap, time);

	hold = rfx_check_freq_hold_or_drop(rfx_c, max_cap, &nohz_drop);
	force_down = nohz_drop;

	if (hold && !force_down && !rfx_pol->need_freq_update &&
	    next_f < rfx_pol->next_freq)
		next_f = rfx_pol->next_freq;

	if (rfx_update_next_freq(rfx_pol, time, next_f, force_down)) {
		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy, rfx_pol->next_freq);
		else
			rfx_deferred_update(rfx_pol);
	}

	if (rfx_pol->prev_gaming != rfx_gaming_active()) {
		rfx_pol->prev_gaming = rfx_gaming_active();
		rfx_frame_tracker_init(&rfx_pol->frame_tracker,
				       rfx_gaming_active() ?
				       rfx_pol->fps_state.hint : 0);
		if (rfx_gaming_active()) {
			rfx_scene_init(&rfx_pol->scene);
			rfx_pol->scene.gaming_started_ns = time;
		}
	}

	raw_spin_unlock(&rfx_pol->update_lock);
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

	for_each_cpu(cpu, policy->cpus) {
		j_rfxc = per_cpu_ptr(&rfx_cpu, cpu);

		j_max_cap = arch_scale_cpu_capacity(cpu);
		if (j_max_cap > max_cap)
			max_cap = j_max_cap;

		boost = rfx_iowait_apply(j_rfxc, j_max_cap);
		rfx_get_util(j_rfxc, boost);
		j_util = max(j_rfxc->util, boost);

		if (j_util > util)
			util = j_util;

		rfx_check_freq_hold_or_drop(j_rfxc, j_max_cap, &nohz_drop);
		if (nohz_drop)
			any_force_down = true;
	}

	j_rfxc = per_cpu_ptr(&rfx_cpu, lead_cpu);
	lead_util_pct = max_cap ? (unsigned int)(util * 100 / max_cap) : 0;

	rfx_predictor_update(&j_rfxc->predictor, lead_util_pct, time);
	rfx_thermal_refresh(&rfx_pol->thermal, lead_cpu);
	rfx_frame_tracker_update(&rfx_pol->frame_tracker, time, lead_util_pct,
				 j_rfxc->iowait_boost_pending || j_rfxc->iowait_streak > 3);
	rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol);

	if (rfx_gaming_active())
		rfx_scene_tick(rfx_pol, j_rfxc->predictor.ema, lead_util_pct,
			       j_rfxc->predictor.trend, time,
			       rfx_pol->frame_tracker.last_burst_ns &&
			       (time - rfx_pol->frame_tracker.last_burst_ns) <
			       (100 * NSEC_PER_MSEC));

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

	if (rfx_pol->prev_gaming != rfx_gaming_active()) {
		rfx_pol->prev_gaming = rfx_gaming_active();
		rfx_frame_tracker_init(&rfx_pol->frame_tracker,
				       rfx_gaming_active() ?
				       rfx_pol->fps_state.hint : 0);
		if (rfx_gaming_active()) {
			rfx_scene_init(&rfx_pol->scene);
			rfx_pol->scene.gaming_started_ns = time;
		}
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

	/* Atomic store, no cpufreq iteration to avoid deadlock */
	smp_store_release(&rfx_global_gaming_mode, val);
	return count;
}

static struct governor_attr gaming_mode =
	__ATTR(gaming_mode, 0644, gaming_mode_show, gaming_mode_store);

/* === PRIME SYSFS === */
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
		.sched_runtime = 4 * NSEC_PER_MSEC,
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
	rfx_fps_init(&rfx_pol->fps_state);
	rfx_thermal_init(&rfx_pol->thermal);
	rfx_scene_init(&rfx_pol->scene);
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
	rfx_pol->need_freq_update = false;
	rfx_pol->avg_util_pct = 0;
	rfx_pol->prev_gaming = gaming;
	rfx_pol->prev_target_freq = policy->cur;
	rfx_pol->last_boost_time_ns = 0;

	if (gaming) {
		rfx_pol->fps_state.hint = RFX_BUILTIN_FPS_HINT;
		rfx_pol->fps_state.actual = RFX_BUILTIN_ACTUAL_FPS;
	}
	rfx_scene_init(&rfx_pol->scene);
	if (gaming)
		rfx_pol->scene.gaming_started_ns = ktime_get_ns();
	rfx_frame_tracker_init(&rfx_pol->frame_tracker, rfx_pol->fps_state.hint);

	uu = policy_is_shared(policy) ? rfx_update_shared : rfx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu = cpu;
		rfx_c->rfx_policy = rfx_pol;
		rfx_predictor_init(&rfx_c->predictor);
		rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
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
	pr_info("AI Smart|Twitch Detect|Warmup 2s|Asymmetric Trans|Built-in\n");
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
