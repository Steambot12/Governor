// SPDX-License-Identifier: GPL-2.0
/*
 * Vorpal CPUFreq Governor
 *
 * Game-aware CPU frequency governor for asymmetric big.LITTLE.PRIME
 * topologies. Combines frame-deadline tracking, predictive util boost,
 * per-cluster floors and caps, fluid jitter smoothing, and thermal-aware
 * limits to sustain high-FPS rendering with low jank.
 *
 * v5.7 - Synchronized Render Pipeline
 *
 *  Fixes against v5.6 (user-reported in-game telemetry:
 *    Delta Force: jank 5.8%, min FPS 76.6, avg CPU 64.9%, ~6.56W
 *    PUBG:        jank 3.1%, min FPS 73.5, avg CPU 72.6%, max 40C
 *  i.e. the governor was throttling itself while CPU still had
 *  headroom -- power was close to the 6.5W envelope but FPS still
 *  collapsed to 73-76 in heavy frames, and ~1 in 17 frames was janky):
 *
 *   - Root cause #1: the v5.6 emergency cap. RFX_FRAME_EMERGENCY_CAP_PCT
 *     was lowered to 92 to "save 0.5W on late frames". In practice the
 *     heavy frames that need to be caught are exactly the ones that need
 *     fmax for ~3ms; capping them at 92% PRIME meant they overran the
 *     deadline and the frame was dropped instead. With CPU at 64-72%
 *     average, the only path to a 73 FPS min is that the governor itself
 *     refused to give PRIME the headroom on a heavy frame. RESTORED TO
 *     100. Emergency frame rescue now goes to fmax. This is the single
 *     largest contributor to the Min FPS fix.
 *
 *   - Root cause #2: stacked caps. v5.6 had five independent power-
 *     reducing layers (asym target, asym stable-ceil, iowait, thermal,
 *     comfort-bleed) all running min() against each other every fast-
 *     path event. They competed: iowait pushed up to 82, asym floor
 *     raised to 82, comfort-bleed pulled down to mid-80s, stable-ceil
 *     re-clamped to 92, and the next event undid the previous. That
 *     thrash is what the user described as "features conflicting".
 *
 *     RESTRUCTURED so each layer has one job, applied in one order, no
 *     reciprocal compares:
 *       1. floor              -- single cluster minimum during gaming
 *       2. emergency override -- fmax, bypasses all caps
 *       3. one active ceiling -- computed once from state (pre-stable
 *                                or stable), not min()'d twice
 *       4. iowait boost       -- raises only, sits just below ceiling
 *       5. thermal cap        -- only engages when headroom <= 12
 *       6. IIR smoother       -- transition shape only
 *       7. rate limit         -- floor on rate of change only
 *     Each layer adjusts in one direction; later layers never undo
 *     earlier ones. This is what the user asked for: features "work
 *     one by one in synchronization", not stacked.
 *
 *   - Root cause #3: idle relaxation gap. v5.6 dropped PRIME idle floor
 *     86 -> 72. Between bursts PRIME relaxed by 10+ percentage points,
 *     and burst re-entry had to climb 72 -> 82 (floor) -> 94 (target).
 *     At UP_GAMING_BURST = 48/64 = 75% per IIR event, two events @ 833us
 *     ~= 1.67ms; that consumed 20% of a 120Hz frame just on freq ramp.
 *     PRIME idle floor 72 -> 80. Now a 4% step lands in ONE IIR event
 *     ~833us. Burst re-entry no longer steals a fifth of the frame.
 *
 *   - Stable ceiling 92 -> 95 (PRIME), 84 -> 88 (BIG). The 92 ceiling
 *     was below the band where heavy frames live; we were systematically
 *     starving the heavy 5% of frames to save 1W on the easy 95%. With
 *     emergency at fmax and stable ceil at 95, easy frames still sit at
 *     95 (close to v5.6 power) but heavy frames can escape upward.
 *
 *   - Pre-stable target 94 -> 97 (PRIME), 92 -> 96 (BIG). Same logic for
 *     the first ~1.5s of a session before the FPS-stable detector arms.
 *
 *   - Iowait boost now sits just below the active ceiling (PRIME 88,
 *     BIG 80, LITTLE 62). v5.6 had iowait at 82 which was BELOW the
 *     floor of 82 -- so iowait effectively did nothing on PRIME except
 *     extend the post-IO streak. Now iowait does its real job again
 *     (raise freq across an IO storm) without stacking against floor.
 *
 *   - Comfort-frame down-step REMOVED (3 -> 0). It was the v5.6 attempt
 *     to bleed power inside a "safe" frame, but it caused the freq to
 *     drift downward across every frame, so the next frame's heavy
 *     phase had to ramp up from a lower base. Net effect was negative
 *     for both smoothness and power. Power is now released exclusively
 *     between bursts via idle relaxation (which has a clean, fast
 *     re-entry now that idle floor is closer to active floor).
 *
 *   - Sustain window 9ms -> 11ms, sustain min jump 8 -> 6. v5.6 raised
 *     the jump threshold to 8 to "avoid arming on 5% twitches" but the
 *     side-effect was that legitimate 6-7% boosts (the most common case
 *     mid-burst) no longer armed the sustain at all. 11ms covers ~1.3
 *     frames at 120Hz so one slow frame inside a burst no longer pulls
 *     PRIME down before the next frame starts.
 *
 *   - Gaming down-rate 3500us -> 5000us. The faster release in v5.6
 *     was undermining the sustain window; in tandem with the comfort
 *     bleed it staircased PRIME down across every frame.
 *
 *   - FPS-stable arming time 500ms -> 1500ms, threshold hint-2 -> hint-1.
 *     v5.6 was declaring "stable" within half a second; the stable
 *     ceiling then clamped PRIME at 92 while the session was still
 *     warming up, causing the first jank cluster. 1500ms of evidence at
 *     hint-1 is a stricter precondition for the cooler ceiling.
 *
 *   - Predictor fast-up: in-burst gate retained; destination is now the
 *     ACTIVE ceiling (97/96/84%) not fmax-clamped-to-asym -- avoids a
 *     redundant fmax->clamp roundtrip that produced an oscillation on
 *     boundary trends.
 *
 *   - Frame phase warn 40% -> 30%. Emergency triggers earlier in a heavy
 *     frame, while there is still time for the freq ramp to actually
 *     help the deadline.
 *
 * Net effect: emergency = fmax, ceilings raised ~3% so heavy frames have
 * somewhere to go, idle relaxation moved closer to the active floor so
 * re-entry is single-step, comfort bleed removed so freq doesn't drift
 * downward across the burst, sustain restored. Layers are applied in
 * one direction only -- no two layers can fight.
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
#define CPUFREQ_VORPAL_VERSION	"5.7"

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
#define RFX_BUILTIN_THERMAL_HEADROOM	20

/* === MICRO-TICK RENDER PATH ===
 * Slow-path interval. Heavy work (predictor, frame tracker, avg util,
 * iowait decay) refreshes at most once per this interval. Fast path
 * (freq decision) runs on every util-update event using cached state.
 *
 * Gaming: 833us == 1/10th of an 8.33ms frame -> 10x render reactivity
 *         versus a 1-frame-tick governor, without re-running heavy
 *         logic at >1kHz which is what was pinning CPU before.
 * Daily : 5 ms -> a touch slower than v5.4 (4 ms) so idle / browsing
 *         leaves the CPU truly idle for longer.
 */
#define RFX_MICRO_TICK_GAMING_NS	833333ULL
#define RFX_MICRO_TICK_DAILY_NS		5000000ULL

/* === PREDICTIVE ENGINE === */
#define RFX_PREDICTOR_WINDOW		16
#define RFX_PREDICT_BOOST_THRESHOLD	28
#define RFX_PREDICT_FAST_UP_THRESHOLD	42

/* === FRAME DEADLINE === */
#define RFX_FRAME_BUDGET_120HZ_NS	8333333ULL
#define RFX_FRAME_BUDGET_90HZ_NS	11111111ULL
#define RFX_FRAME_BUDGET_60HZ_NS	16666667ULL
#define RFX_FRAME_EMERGENCY_NS		2500000ULL
#define RFX_FRAME_PHASE_THRESHOLD_PCT	10
/* v5.7: emergency cap RESTORED to fmax. v5.6 lowered this to 92 to save
 * power on late frames; the side-effect was that the heavy frames that
 * actually needed fmax got dropped instead, which is exactly what the
 * Min FPS 73-76 reading shows. Power on the easy 95% of frames is
 * controlled by the per-cluster ceiling -- emergency must bypass it. */
#define RFX_FRAME_EMERGENCY_CAP_PCT	100
/* v5.7: 40 -> 30. Earlier emergency arm so the freq ramp lands before
 * the deadline, not after. */
#define RFX_FRAME_WARN_PHASE_PCT	30
/* v5.7: unused now (comfort-bleed removed). Kept as a tunable for
 * future use; the smoother no longer references it. */
#define RFX_FRAME_COMFORT_PHASE_PCT	35

/* === FLUID IIR JITTER SMOOTHER ===
 *   smoothed = (current * (DEN - num) + target * num) / DEN
 *
 * DEN=64 so each event is a small proportional move toward the target.
 *
 * v5.7 change: the comfort-bleed branch is REMOVED. In-burst down-step
 * is hard-zero again. Power is no longer released mid-burst by a
 * micro-tick bleed (which produced a downward drift across each frame
 * and was a major contributor to next-frame ramp jank); it is released
 * exclusively between bursts via the idle floor, which is now close
 * enough to the active floor (80 vs 84) that re-entry is a single IIR
 * event. Smoothness and power are now driven by orthogonal mechanisms
 * instead of fighting inside the smoother.
 *
 * Emergency / predictor fast-up / gaming-entry bypass smoothing for
 * one cycle so an entering burst lands at the right freq immediately.
 */
#define RFX_IIR_DEN			64
#define RFX_IIR_UP_GAMING_BURST		48	/* 75% target weight - fast, fluid */
#define RFX_IIR_UP_GAMING_IDLE		36	/* 56%, between bursts */
#define RFX_IIR_DOWN_GAMING_BURST	0	/* hold: never drop mid-burst */
#define RFX_IIR_DOWN_GAMING_IDLE	24	/* 37%, controlled release */
#define RFX_IIR_UP_DAILY		28	/* 44% */
#define RFX_IIR_DOWN_DAILY		18	/* 28% */

/* v5.7: 11ms (~1.3 frames @120Hz). v5.6 dropped this to 9ms which let
 * PRIME release between back-to-back heavy frames. 11ms covers one slow
 * frame inside a burst so the next frame isn't ramping from scratch. */
#define RFX_SUSTAIN_WINDOW_NS		11000000ULL
/* v5.7: 6%. v5.6 raised this to 8 to avoid "5% twitches", but the
 * side-effect was that ordinary mid-burst boosts (6-7%) no longer armed
 * the sustain at all -- which is exactly when sustain is most useful. */
#define RFX_SUSTAIN_MIN_JUMP_PCT	6

/* === ASYMMETRIC CLUSTER POLICY ===
 * v5.7: ceilings raised so heavy frames have somewhere to go. Caps
 * applied in one direction only (later layers never undo earlier ones).
 * Floor is now closer to idle floor so burst re-entry is one IIR step. */
#define RFX_ASYM_PRIME_FLOOR_PCT	84
#define RFX_ASYM_PRIME_TARGET_PCT	97
#define RFX_ASYM_BIG_FLOOR_PCT		74
#define RFX_ASYM_BIG_CAP_PCT		96
#define RFX_ASYM_LITTLE_FLOOR_PCT	42
#define RFX_ASYM_LITTLE_CAP_PCT		88
/* v5.7: PRIME idle floor 72 -> 80. Between bursts PRIME relaxes by ~4%
 * (84 active -> 80 idle); re-entry is a single IIR event at 75% weight
 * == ~3% of fmax in 833us, i.e. lands before the next util-update.
 * v5.6's 72-floor cost up to 1.7ms of ramp on burst re-entry, which is
 * 20% of a 120Hz frame -- visible as jank. */
#define RFX_ASYM_PRIME_IDLE_PCT		80
#define RFX_ASYM_BIG_IDLE_PCT		68

/* === FPS-STABLE GAMING CEILING (v5.7) ===
 * Once FPS has held its target with very low slack for 1500ms during
 * gaming, cap PRIME at 95% and BIG at 88%. v5.6 used 92/84 which sat
 * BELOW the heavy-frame band, systematically dropping ~5% of frames.
 * The cap is bypassed by emergency frame-boost which now goes to fmax. */
#define RFX_ASYM_PRIME_STABLE_CEIL_PCT	95
#define RFX_ASYM_BIG_STABLE_CEIL_PCT	88

/* === CPU USAGE SOFT CAP (daily only) === */
#define RFX_CPU_USAGE_SOFT_CAP		72
#define RFX_CPU_USAGE_CAP_STEP_DIV	10

/* === FPS FEEDBACK ===
 * v5.7: stricter precondition for "stable" so we don't engage the cool
 * ceiling while the session is still warming up.
 *   threshold: hint - 2 -> hint - 1
 *   time:      500ms  -> 1500ms
 * That's ~180 frames @120Hz of >=119fps before we trust the cool cap. */
#define RFX_FPS_STABLE_THRESHOLD	1
#define RFX_FPS_STABLE_TIME_NS		(1500 * NSEC_PER_MSEC)

/* === IO WAIT BOOST ===
 * v5.7: iowait boost now sits ABOVE the cluster floor so it can do its
 * actual job (raise freq across an IO storm). v5.6 had iowait PRIME=82
 * == floor; effectively iowait did nothing on PRIME. Now PRIME=88,
 * which is between the active floor (84) and the stable ceiling (95),
 * so iowait can lift PRIME by ~4% during an IO burst -- enough to
 * absorb a binder/storage storm -- without ever clashing with the
 * ceiling layer above it. */
#define RFX_IOWAIT_BOOST_THRESHOLD	2
#define RFX_IOWAIT_BOOST_PRIME_PCT	88
#define RFX_IOWAIT_BOOST_BIG_PCT	80
#define RFX_IOWAIT_BOOST_LITTLE_PCT	62

/* === THERMAL === */
#define RFX_THERMAL_HEADROOM_DEFAULT	20
#define RFX_THERMAL_HARD_MARGIN		5
#define RFX_THERMAL_SOFT_MARGIN		12
#define RFX_THERMAL_CAP_HARD_PCT	55
#define RFX_THERMAL_CAP_SOFT_PCT	72
#define RFX_THERMAL_CAP_NORMAL_PCT	98

/* === DAILY COOLDOWN === */
#define RFX_DAILY_CAP_PCT		72

/* === RATE LIMITS ===
 * v5.7: gaming-down 3500us -> 5000us. v5.6's faster release undermined
 * the sustain window in tandem with the comfort bleed. With the bleed
 * gone, a slightly slower release here keeps PRIME from staircasing
 * between back-to-back frames. */
#define RFX_RATE_LIMIT_GAMING_UP_US	0
#define RFX_RATE_LIMIT_GAMING_DOWN_US	5000
#define RFX_RATE_LIMIT_DEFAULT_UP_US	500
#define RFX_RATE_LIMIT_DEFAULT_DOWN_US	8000
#define RFX_RATE_LIMIT_IDLE_DOWN_US	3000

/* === IDLE === */
#define RFX_IDLE_CAP_PCT		10

/* === EMA (predictor input) ===
 * Gaming alpha is moderate (62%) so the predictor reacts but does not
 * over-twitch on a single noisy sample. The IIR smoother and the BURST
 * down-step=0 guarantee the freq does not echo that twitch. Daily
 * alpha is left alone.
 */
#define RFX_EMA_ALPHA_GAMING_NUM	62
#define RFX_EMA_ALPHA_DAILY_NUM		30
#define RFX_EMA_ALPHA_DEN		100

/* === CLUSTER DETECTION === */
#define RFX_LITTLE_CAP_THRESHOLD	614
#define RFX_PRIME_CAP_THRESHOLD		1000

/* === SCHED FLAGS === */
#define SCHED_FLAGS_UGOV		0x10000000
#define IOWAIT_BOOST_MIN		(SCHED_CAPACITY_SCALE / 8)
#define IOWAIT_BOOST_MAX		(SCHED_CAPACITY_SCALE)

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
	int trend;
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

	/* Heavy-state snapshot, refreshed at MICRO_TICK_NS cadence */
	struct rfx_frame_tracker frame_tracker;
	struct rfx_fps_state fps_state;
	struct rfx_thermal_state thermal;
	unsigned int avg_util_pct;

	/* Render-aware fast path bookkeeping */
	u64 last_heavy_update_ns;
	u64 last_boost_time_ns;
	unsigned int prev_target_freq;
	bool prev_gaming;

	/* Iowait propagated across policy CPUs */
	bool policy_iowait_active;
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

static inline unsigned int rfx_adaptive_floor(struct cpufreq_policy *policy,
					      unsigned int pct)
{
	unsigned int floor;
	if (!policy || !policy->cpuinfo.max_freq)
		return 0;
	floor = (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
	return max(floor, policy->cpuinfo.min_freq);
}

static inline u64 rfx_micro_tick_ns(void)
{
	return rfx_gaming_active() ?
		RFX_MICRO_TICK_GAMING_NS : RFX_MICRO_TICK_DAILY_NS;
}

/* === PREDICTIVE ENGINE === */

static void rfx_predictor_init(struct rfx_predictor *p)
{
	memset(p, 0, sizeof(*p));
}

/* Seed the predictor as if it had been running at `pct` for a while.
 * Used on gaming-mode entry so the very first util-update has a sensible
 * prediction (was the cause of the "0 fps for a few seconds" freeze). */
static void rfx_predictor_seed(struct rfx_predictor *p, unsigned int pct, u64 time)
{
	unsigned int i;

	memset(p, 0, sizeof(*p));
	for (i = 0; i < RFX_PREDICTOR_WINDOW; i++)
		p->hist[i] = pct;
	p->ema = pct;
	p->predicted = pct;
	p->trend = 1;
	p->last_update_ns = time;
}

static void rfx_predictor_update(struct rfx_predictor *p, unsigned int util_pct, u64 time)
{
	unsigned int i, recent_sum = 0, prev_sum = 0;
	unsigned int recent_avg, prev_avg;
	unsigned int old_ema = p->ema;
	unsigned int alpha_num;

	p->hist[p->idx] = util_pct;
	p->idx = (p->idx + 1) & (RFX_PREDICTOR_WINDOW - 1);

	alpha_num = rfx_gaming_active() ?
		RFX_EMA_ALPHA_GAMING_NUM : RFX_EMA_ALPHA_DAILY_NUM;

	p->ema = (alpha_num * util_pct +
		  (RFX_EMA_ALPHA_DEN - alpha_num) * old_ema) /
		 RFX_EMA_ALPHA_DEN;

	for (i = 0; i < 4; i++) {
		recent_sum += p->hist[(p->idx - 1 - i) & (RFX_PREDICTOR_WINDOW - 1)];
		prev_sum += p->hist[(p->idx - 5 - i) & (RFX_PREDICTOR_WINDOW - 1)];
	}
	recent_avg = recent_sum >> 2;
	prev_avg = prev_sum >> 2;

	if (recent_avg > prev_avg + 15)
		p->trend = 2;
	else if (recent_avg > prev_avg + 6)
		p->trend = 1;
	else if (recent_avg + 15 < prev_avg)
		p->trend = -2;
	else if (recent_avg + 6 < prev_avg)
		p->trend = -1;
	else
		p->trend = 0;

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

/* Mark "we are inside a render burst right now" without waiting for the
 * normal phase-detect heuristic. Called on gaming entry so the first
 * frame is treated as a burst from cycle zero. */
static void rfx_frame_tracker_force_burst(struct rfx_frame_tracker *ft, u64 time)
{
	ft->in_render_phase = true;
	ft->last_frame_ns = time;
	ft->phase_ns = 0;
}

static void rfx_frame_tracker_update(struct rfx_frame_tracker *ft, u64 time,
				     unsigned int util_pct, bool iowait_active)
{
	u64 elapsed;

	if (!ft->last_frame_ns)
		ft->last_frame_ns = time;

	elapsed = time - ft->last_frame_ns;

	if (!ft->in_render_phase) {
		/* Enter render phase as soon as we see meaningful util or
		 * iowait. The previous "elapsed > budget/8" gate delayed
		 * entry by up to 1 ms on a fresh start, costing the first
		 * frame's emergency boost. */
		if (util_pct > RFX_FRAME_PHASE_THRESHOLD_PCT || iowait_active) {
			ft->in_render_phase = true;
			ft->last_frame_ns = time;
			ft->phase_ns = 0;
			return;
		}
	} else {
		/* Idle-out only if sustained low util across 3/4 of a frame.
		 * One low sample mid-frame is not enough. */
		if ((util_pct < 6 && elapsed > (ft->budget_ns * 3 / 4)) ||
		    elapsed > (ft->budget_ns * 3 / 2)) {
			ft->in_render_phase = false;
			ft->phase_ns = 0;
			return;
		}

		if (elapsed >= ft->budget_ns) {
			u64 frames = elapsed / ft->budget_ns;
			ft->last_frame_ns += frames * ft->budget_ns;
			elapsed = time - ft->last_frame_ns;
		}
	}

	ft->phase_ns = elapsed;
}

static bool rfx_frame_needs_emergency_boost(struct rfx_frame_tracker *ft,
					    unsigned int util_pct)
{
	u64 remaining;

	if (!ft->in_render_phase)
		return false;

	if (ft->budget_ns > ft->phase_ns)
		remaining = ft->budget_ns - ft->phase_ns;
	else
		remaining = 0;

	if (remaining < RFX_FRAME_EMERGENCY_NS && util_pct > 25)
		return true;

	if (ft->phase_ns > ft->budget_ns)
		return true;

	if (ft->phase_ns > (ft->budget_ns * RFX_FRAME_WARN_PHASE_PCT / 100) &&
	    util_pct > 25)
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

	/*
	 * v5.7: gaming participates in the settle logic with a stricter
	 * precondition (actual >= hint - 1 held for 1500ms). Once
	 * stabilized we arm the cooler asymmetric ceiling. v5.6 used
	 * (hint - 2, 500ms) which declared "stable" within half a second
	 * and then clamped at 92% PRIME while the session was still
	 * warming up -- the first jank cluster. The stricter window
	 * lets the cool ceiling engage only when the FPS evidence is
	 * actually convincing.
	 */
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

/* Daily cooldown cap to keep browsing/idle thermals down */
static unsigned int rfx_daily_cooldown_cap(struct rfx_policy *rfx_pol,
					  unsigned int target_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int max_freq = policy->cpuinfo.max_freq;
	unsigned int daily_max;

	if (rfx_gaming_active())
		return target_freq;

	daily_max = max_freq * RFX_DAILY_CAP_PCT / 100;

	if (rfx_pol->avg_util_pct > 10 && target_freq > daily_max)
		return daily_max;

	return target_freq;
}

/* === ASYMMETRIC CLUSTER POLICY ===
 *
 * v5.7 restructure: each cluster computes ONE active ceiling from state,
 * then the function does floor -> emergency-fmax -> ceiling-clamp in
 * a single pass. No nested compares, no min() of two different ceilings.
 * Layers later in the pipeline (iowait, thermal) never undo this.
 */
static unsigned int rfx_asymmetric_apply(struct rfx_policy *rfx_pol,
					   enum rfx_cluster_type cluster,
					   unsigned int raw_freq,
					   bool gaming, bool emergency)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int max = policy->cpuinfo.max_freq;
	unsigned int min = policy->cpuinfo.min_freq;
	unsigned int floor, ceiling;
	unsigned int ceil_pct;

	if (!gaming)
		return raw_freq;

	switch (cluster) {
	case RFX_CLUSTER_PRIME:
		floor = min + ((max - min) * RFX_ASYM_PRIME_FLOOR_PCT / 100);
		ceil_pct = rfx_pol->fps_state.stabilized ?
			RFX_ASYM_PRIME_STABLE_CEIL_PCT :
			RFX_ASYM_PRIME_TARGET_PCT;
		break;
	case RFX_CLUSTER_BIG:
		floor = min + ((max - min) * RFX_ASYM_BIG_FLOOR_PCT / 100);
		ceil_pct = rfx_pol->fps_state.stabilized ?
			RFX_ASYM_BIG_STABLE_CEIL_PCT :
			RFX_ASYM_BIG_CAP_PCT;
		break;
	case RFX_CLUSTER_LITTLE:
		floor = min + ((max - min) * RFX_ASYM_LITTLE_FLOOR_PCT / 100);
		ceil_pct = RFX_ASYM_LITTLE_CAP_PCT;
		break;
	default:
		return raw_freq;
	}

	/* 1. floor: cluster minimum during gaming */
	if (raw_freq < floor)
		raw_freq = floor;

	/* 2. emergency override: fmax wins over all caps.
	 *    This is the v5.7 fix for Min FPS dips -- heavy frames that
	 *    actually need fmax for 2-3ms get it. Power on the easy 95%
	 *    of frames is controlled by the ceiling below. */
	if (emergency)
		return min(raw_freq, max);

	/* 3. active ceiling: ONE value, computed above from state. */
	ceiling = min + ((max - min) * ceil_pct / 100);
	if (raw_freq > ceiling)
		raw_freq = ceiling;

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

	if (!rfx_pol->fps_state.stabilized)
		return target_freq;

	if (rfx_pol->avg_util_pct <= RFX_CPU_USAGE_SOFT_CAP)
		return target_freq;

	step = (policy->cpuinfo.max_freq - policy->cpuinfo.min_freq) /
	       RFX_CPU_USAGE_CAP_STEP_DIV;
	if (target_freq > step)
		target_freq -= step;

	return max(target_freq, policy->cpuinfo.min_freq);
}

/* === IO WAIT BOOST ===
 *
 * v5.5 split: rfx_iowait_observe() runs on the fast path and only
 * latches "iowait was seen". rfx_iowait_decay() runs on the slow path
 * and is the only place that decays the streak/boost. v5.4 was decaying
 * on every fast event, so iowait_boost collapsed in sub-microseconds
 * and re-inflated in a loop -- that thrash was a major source of the
 * "CPU pinned 85-98%" bug.
 */

static void rfx_iowait_observe(struct rfx_cpu *rfx_c, unsigned int flags)
{
	if (flags & SCHED_CPUFREQ_IOWAIT) {
		rfx_c->iowait_boost_pending = true;
		if (rfx_c->iowait_streak < 255)
			rfx_c->iowait_streak++;

		if (!rfx_c->iowait_boost)
			rfx_c->iowait_boost = IOWAIT_BOOST_MIN;
		else if (rfx_c->iowait_boost < IOWAIT_BOOST_MAX)
			rfx_c->iowait_boost = min_t(unsigned int,
				rfx_c->iowait_boost << 1, IOWAIT_BOOST_MAX);
	}
}

/* Decay streak/boost. SLOW PATH ONLY.
 * v5.6: streak decay -= 2 (was 1) so the policy_iowait_active gate
 * clears in 1-2 ticks instead of 3-5. The "post I/O storm CPU pin"
 * pattern was largely driven by this lag. */
static void rfx_iowait_decay(struct rfx_cpu *rfx_c)
{
	if (rfx_c->iowait_boost_pending) {
		/* Consumed by the slow tick; require a fresh sample next
		 * micro-tick to keep the boost held. */
		rfx_c->iowait_boost_pending = false;
		return;
	}

	if (rfx_c->iowait_streak >= 2)
		rfx_c->iowait_streak -= 2;
	else
		rfx_c->iowait_streak = 0;

	if (rfx_c->iowait_boost > IOWAIT_BOOST_MIN)
		rfx_c->iowait_boost >>= 1;
	else
		rfx_c->iowait_boost = 0;
}

static unsigned long rfx_iowait_apply(struct rfx_cpu *rfx_c,
				      unsigned long max_cap)
{
	unsigned int boost_util;

	if (!rfx_c->iowait_boost ||
	    (!rfx_c->iowait_boost_pending &&
	     rfx_c->iowait_streak < RFX_IOWAIT_BOOST_THRESHOLD))
		return 0;

	boost_util = (unsigned long)((u64)rfx_c->iowait_boost * max_cap >>
				     SCHED_CAPACITY_SHIFT);
	return boost_util;
}

static unsigned int rfx_iowait_boost_freq(struct rfx_policy *rfx_pol,
					  enum rfx_cluster_type cluster,
					  unsigned int current_freq)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int boost_freq;
	unsigned int pct;

	if (!rfx_pol->policy_iowait_active)
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

/* === FLUID IIR JITTER SMOOTHER ===
 *
 * smoothed = (current * (DEN - num) + target * num) / DEN
 *
 * Soft fluid transition: each event moves the chosen freq a proportional
 * fraction of the gap toward target. Big gaps land in a few events, tiny
 * gaps don't oscillate. Bypassed on:
 *   - emergency boost (frame-deadline crisis)
 *   - predictor fast-up
 *   - in-burst going UP   (let the burst freq snap to target)
 *
 * v5.7: in-burst going DOWN is ALWAYS held (was: comfort-bleed bled the
 * freq down across each frame, which raised next-frame ramp jank). The
 * power lever has moved to idle relaxation, which sits 4% below the
 * active floor and re-enters in a single IIR event.
 */
static unsigned int rfx_smooth_freq(struct rfx_policy *rfx_pol,
				    unsigned int target_freq,
				    unsigned int current_freq,
				    bool emergency,
				    bool fast_up)
{
	unsigned int max_freq = rfx_pol->policy->cpuinfo.max_freq;
	unsigned int min_freq = rfx_pol->policy->cpuinfo.min_freq;
	unsigned int up_num, down_num, num;
	u64 smoothed;
	bool gaming = rfx_gaming_active();
	bool in_burst = rfx_pol->frame_tracker.in_render_phase;
	bool going_up;

	if (!current_freq || current_freq == target_freq)
		return clamp_t(unsigned int, target_freq, min_freq, max_freq);

	if (emergency && gaming)
		return target_freq;

	if (fast_up && gaming && target_freq > current_freq)
		return target_freq;

	going_up = (target_freq > current_freq);

	/* v5.7: in-burst, going-down -> hold unconditionally. No comfort
	 * bleed. This is what keeps movement smooth across every phase of
	 * the frame, not just the late phase. Power is freed between
	 * bursts via the idle floor (see rfx_get_next_freq). */
	if (gaming && in_burst && !going_up)
		return current_freq;

	if (gaming) {
		up_num = in_burst ? RFX_IIR_UP_GAMING_BURST :
				    RFX_IIR_UP_GAMING_IDLE;
		down_num = RFX_IIR_DOWN_GAMING_IDLE;	/* only fires !in_burst */
	} else {
		up_num = RFX_IIR_UP_DAILY;
		down_num = RFX_IIR_DOWN_DAILY;
	}

	num = going_up ? up_num : down_num;

	smoothed = ((u64)current_freq * (RFX_IIR_DEN - num) +
		    (u64)target_freq * num) / RFX_IIR_DEN;

	return clamp_t(unsigned int, (unsigned int)smoothed, min_freq, max_freq);
}

/*
 * Sustain window: hold frequency briefly after a meaningful boost while
 * still inside a render burst, so a transient util dip mid-frame does
 * not pull the freq down before the burst completes.
 */
static bool rfx_in_sustain_window(struct rfx_policy *rfx_pol, u64 time)
{
	if (rfx_pol->last_boost_time_ns == 0)
		return false;
	if (!rfx_pol->frame_tracker.in_render_phase)
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
		if (rfx_pol->next_freq) {
			unsigned int max_freq = rfx_pol->policy->cpuinfo.max_freq;
			unsigned int jump = next_freq > rfx_pol->next_freq ?
				next_freq - rfx_pol->next_freq : 0;
			if (max_freq &&
			    (jump * 100 / max_freq) >= RFX_SUSTAIN_MIN_JUMP_PCT)
				rfx_pol->last_boost_time_ns = time;
		} else {
			rfx_pol->last_boost_time_ns = time;
		}
	} else {
		rfx_pol->last_downfreq_time = time;
	}

	rfx_pol->next_freq = next_freq;
	rfx_pol->prev_target_freq = next_freq;
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
	bool fast_up;

	if (!policy || !max_cap)
		return 0;

	cluster = rfx_get_cluster_type(cpumask_first(policy->cpus));
	util_pct = (unsigned int)(util * 100 / max_cap);
	gaming = rfx_gaming_active();
	fast_up = (rfx_c->predictor.trend == 2);

	freq = (unsigned int)((u64)policy->cpuinfo.max_freq * util / max_cap);
	freq = clamp_t(unsigned int, freq, policy->cpuinfo.min_freq,
		       policy->cpuinfo.max_freq);

	if (gaming && rfx_c->predictor.predicted > RFX_PREDICT_BOOST_THRESHOLD) {
		unsigned int predict_freq = (unsigned int)((u64)policy->cpuinfo.max_freq *
							   rfx_c->predictor.predicted / 100);
		if (predict_freq > freq)
			freq = predict_freq;
	}

	/* v5.7: fast-up targets the active ceiling directly. v5.6 requested
	 * fmax and let asym_apply clamp it back down to 92-94%; that
	 * round-trip caused a one-event overshoot at the cluster boundary
	 * that contributed to the visible "jitter". Now the predictor and
	 * the ceiling agree on the destination in a single step. Still
	 * gated on in_burst so a stray late trend=2 between bursts cannot
	 * re-pin freq. */
	if (gaming && rfx_pol->frame_tracker.in_render_phase &&
	    rfx_c->predictor.trend == 2 &&
	    rfx_c->predictor.predicted > RFX_PREDICT_FAST_UP_THRESHOLD) {
		unsigned int ceil_pct;

		if (cluster == RFX_CLUSTER_PRIME)
			ceil_pct = rfx_pol->fps_state.stabilized ?
				RFX_ASYM_PRIME_STABLE_CEIL_PCT :
				RFX_ASYM_PRIME_TARGET_PCT;
		else if (cluster == RFX_CLUSTER_BIG)
			ceil_pct = rfx_pol->fps_state.stabilized ?
				RFX_ASYM_BIG_STABLE_CEIL_PCT :
				RFX_ASYM_BIG_CAP_PCT;
		else
			ceil_pct = RFX_ASYM_LITTLE_CAP_PCT;

		{
			unsigned int fast_freq = rfx_adaptive_max(policy, ceil_pct);
			if (fast_freq > freq)
				freq = fast_freq;
		}
	}

	if (gaming && rfx_frame_needs_emergency_boost(&rfx_pol->frame_tracker,
						      util_pct)) {
		unsigned int em_freq = rfx_adaptive_max(policy,
						RFX_FRAME_EMERGENCY_CAP_PCT);
		freq = max(freq, em_freq);
		emergency = true;
	}

	freq = rfx_iowait_boost_freq(rfx_pol, cluster, freq);

	freq = rfx_asymmetric_apply(rfx_pol, cluster, freq, gaming, emergency);

	freq = rfx_thermal_apply(&rfx_pol->thermal, policy, freq);

	freq = rfx_apply_cpu_usage_cap(rfx_pol, freq);

	freq = rfx_daily_cooldown_cap(rfx_pol, freq);

	if (!gaming && cluster == RFX_CLUSTER_LITTLE && util_pct < 5) {
		unsigned int idle_cap = rfx_adaptive_max(policy, RFX_IDLE_CAP_PCT);
		if (freq > idle_cap)
			freq = idle_cap;
	}

	/*
	 * Between-burst relief: LITTLE deep-relaxes; BIG/PRIME drop to the
	 * v5.7 idle floors (PRIME 80%, BIG 68%) which sit JUST below the
	 * active floors (84%, 74%). Burst re-entry is a single ~4% step
	 * that lands in one IIR event (~833us), well inside one 120Hz
	 * frame. That is the v5.7 power lever: power released between
	 * frames, not by bleeding within a frame.
	 */
	if (gaming && !rfx_pol->frame_tracker.in_render_phase && util_pct < 15) {
		unsigned int idle_cap;

		if (cluster == RFX_CLUSTER_LITTLE) {
			idle_cap = rfx_adaptive_max(policy, RFX_IDLE_CAP_PCT);
		} else if (cluster == RFX_CLUSTER_BIG) {
			idle_cap = rfx_adaptive_floor(policy,
						RFX_ASYM_BIG_IDLE_PCT);
		} else {
			idle_cap = rfx_adaptive_floor(policy,
						RFX_ASYM_PRIME_IDLE_PCT);
		}
		if (freq > idle_cap)
			freq = idle_cap;
	}

	freq = rfx_smooth_freq(rfx_pol, freq, rfx_pol->prev_target_freq,
			       emergency, fast_up);

	freq = cpufreq_driver_resolve_freq(policy, freq);

	(void)time;
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

/* === MICRO-TICK GATE ===
 * Decide whether the heavy-work slot is due. Returns true at most once
 * per MICRO_TICK_NS. Called under rfx_pol->update_lock.
 */
static bool rfx_heavy_due(struct rfx_policy *rfx_pol, u64 time)
{
	u64 tick = rfx_micro_tick_ns();

	if (!rfx_pol->last_heavy_update_ns ||
	    (time - rfx_pol->last_heavy_update_ns) >= tick) {
		rfx_pol->last_heavy_update_ns = time;
		return true;
	}
	return false;
}

/* Push the freq immediately, bypassing the smoother / rate limit. Only
 * called on gaming mode-flip so the very first frame after entry runs
 * at the right freq. (Was the source of the 0-fps freeze in v5.4.) */
static void rfx_fast_seed_freq(struct rfx_policy *rfx_pol, unsigned int target,
			       u64 time)
{
	struct cpufreq_policy *policy = rfx_pol->policy;
	unsigned int resolved;

	if (!policy || !target)
		return;

	resolved = cpufreq_driver_resolve_freq(policy, target);
	rfx_pol->prev_target_freq = resolved;
	rfx_pol->next_freq = resolved;
	rfx_pol->last_upfreq_time = time;
	rfx_pol->last_boost_time_ns = time;

	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, resolved);
	else
		rfx_deferred_update(rfx_pol);
}

/* === GAMING MODE TRANSITION ===
 * Snap state cleanly when entering gaming so the smoother does not have
 * to climb from policy->cur. v5.5 also seeds the predictor and forces
 * the frame tracker into render phase from cycle zero, and immediately
 * fast-switches the freq to the cluster floor.
 */
static void rfx_handle_mode_transition(struct rfx_policy *rfx_pol, u64 time)
{
	bool gaming_now = rfx_gaming_active();

	if (rfx_pol->prev_gaming == gaming_now)
		return;

	rfx_pol->prev_gaming = gaming_now;

	if (gaming_now) {
		struct cpufreq_policy *policy = rfx_pol->policy;
		enum rfx_cluster_type cluster =
			rfx_get_cluster_type(cpumask_first(policy->cpus));
		unsigned int snap_pct;
		unsigned int seed_pct;
		unsigned int cpu;
		struct rfx_cpu *rfx_c;
		unsigned int floor_freq;

		switch (cluster) {
		case RFX_CLUSTER_PRIME:
			snap_pct = RFX_ASYM_PRIME_FLOOR_PCT;
			seed_pct = 55;	/* PRIME runs hot during render */
			break;
		case RFX_CLUSTER_BIG:
			snap_pct = RFX_ASYM_BIG_FLOOR_PCT;
			seed_pct = 45;
			break;
		default:
			snap_pct = RFX_ASYM_LITTLE_FLOOR_PCT;
			seed_pct = 25;
			break;
		}

		rfx_pol->fps_state.hint = RFX_BUILTIN_FPS_HINT;
		rfx_pol->fps_state.actual = RFX_BUILTIN_ACTUAL_FPS;
		rfx_frame_tracker_init(&rfx_pol->frame_tracker,
				       rfx_pol->fps_state.hint);
		rfx_frame_tracker_force_burst(&rfx_pol->frame_tracker, time);

		for_each_cpu(cpu, policy->cpus) {
			rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
			rfx_predictor_seed(&rfx_c->predictor, seed_pct, time);
			rfx_c->iowait_boost = 0;
			rfx_c->iowait_streak = 0;
			rfx_c->iowait_boost_pending = false;
		}

		floor_freq = rfx_adaptive_floor(policy, snap_pct);
		rfx_pol->need_freq_update = true;
		rfx_fast_seed_freq(rfx_pol, floor_freq, time);
	} else {
		/* Leaving gaming: clear in-burst so the down-step hold
		 * doesn't keep PRIME pinned. */
		rfx_pol->frame_tracker.in_render_phase = false;
		rfx_pol->frame_tracker.phase_ns = 0;
		rfx_pol->fps_state.hint = 0;
		rfx_pol->fps_state.actual = 0;
		rfx_pol->fps_state.stabilized = false;
		rfx_pol->fps_state.stabilized_since_ns = 0;
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
	bool hold, heavy;
	unsigned int util_pct;

	if (!rfx_pol)
		return;

	max_cap = arch_scale_cpu_capacity(rfx_c->cpu);

	/* Cheap, always-on iowait observation (fast path) */
	rfx_iowait_observe(rfx_c, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	boost = rfx_iowait_apply(rfx_c, max_cap);
	rfx_get_util(rfx_c, boost);
	effective_util = max(rfx_c->util, boost);

	util_pct = max_cap ?
		(unsigned int)(effective_util * 100 / max_cap) : 0;

	/* Fast path: try-lock so two CPUs in a shared policy don't serialize
	 * on the slow path. If contended, the other CPU is updating; we'll
	 * pick up the result next event. */
	if (!raw_spin_trylock(&rfx_pol->update_lock))
		return;

	rfx_handle_mode_transition(rfx_pol, time);

	heavy = rfx_heavy_due(rfx_pol, time);

	if (heavy) {
		rfx_iowait_decay(rfx_c);
		rfx_predictor_update(&rfx_c->predictor, util_pct, time);
		rfx_frame_tracker_update(&rfx_pol->frame_tracker, time, util_pct,
				 rfx_c->iowait_boost_pending ||
				 rfx_c->iowait_streak > 3);
		rfx_fps_update(&rfx_pol->fps_state, time);
		rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol);
		rfx_pol->policy_iowait_active = rfx_c->iowait_boost_pending ||
			rfx_c->iowait_streak >= RFX_IOWAIT_BOOST_THRESHOLD;
	} else if (rfx_c->iowait_boost_pending) {
		/* Iowait toggling needs to be visible to the fast path even
		 * when we skip heavy work, otherwise an IO-bound spike sits
		 * for a full micro-tick before being honored. */
		rfx_pol->policy_iowait_active = true;
	}

	next_f = rfx_get_next_freq(rfx_pol, rfx_c, effective_util, max_cap, time);

	hold = rfx_check_freq_hold_or_drop(rfx_c, max_cap, &nohz_drop);
	force_down = nohz_drop;

	if (hold && next_f == rfx_pol->next_freq && !rfx_pol->need_freq_update &&
	    !force_down)
		next_f = rfx_pol->next_freq;

	if (rfx_update_next_freq(rfx_pol, time, next_f, force_down)) {
		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy,
						   rfx_pol->next_freq);
		else
			rfx_deferred_update(rfx_pol);
	}

	raw_spin_unlock(&rfx_pol->update_lock);
}

/* === UPDATE SHARED FREQUENCY === */

static unsigned int rfx_next_freq_shared(struct rfx_cpu *rfx_c, u64 time,
					 bool *force_down_out, bool heavy)
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
	bool any_iowait = false;
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

		if (j_rfxc->iowait_boost_pending ||
		    j_rfxc->iowait_streak >= RFX_IOWAIT_BOOST_THRESHOLD)
			any_iowait = true;

		if (heavy) {
			rfx_iowait_decay(j_rfxc);
			rfx_check_freq_hold_or_drop(j_rfxc, j_max_cap,
						    &nohz_drop);
			if (nohz_drop)
				any_force_down = true;
		}
	}

	j_rfxc = per_cpu_ptr(&rfx_cpu, lead_cpu);
	lead_util_pct = max_cap ?
		(unsigned int)(util * 100 / max_cap) : 0;

	if (heavy) {
		rfx_predictor_update(&j_rfxc->predictor, lead_util_pct, time);
		rfx_frame_tracker_update(&rfx_pol->frame_tracker, time,
					 lead_util_pct,
					 any_iowait);
		rfx_fps_update(&rfx_pol->fps_state, time);
		rfx_pol->avg_util_pct = rfx_compute_avg_util(rfx_pol);
	}

	rfx_pol->policy_iowait_active = any_iowait;

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
	bool heavy;

	if (!rfx_pol)
		return;

	rfx_iowait_observe(rfx_c, flags);
	rfx_c->last_update = time;
	rfx_ignore_dl_rate_limit(rfx_c);

	if (!raw_spin_trylock(&rfx_pol->update_lock))
		return;

	rfx_handle_mode_transition(rfx_pol, time);

	heavy = rfx_heavy_due(rfx_pol, time);

	next_f = rfx_next_freq_shared(rfx_c, time, &force_down, heavy);

	if (rfx_update_next_freq(rfx_pol, time, next_f, force_down)) {
		if (rfx_pol->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(rfx_pol->policy,
						   rfx_pol->next_freq);
		else
			rfx_deferred_update(rfx_pol);
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

	smp_store_release(&rfx_global_gaming_mode, val);
	return count;
}

static struct governor_attr gaming_mode =
	__ATTR(gaming_mode, 0644, gaming_mode_show, gaming_mode_store);

/* PRIME exposes the gaming_mode toggle in addition to rate limits. */
static struct attribute *rfx_prime_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&gaming_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx_prime);

/* LITTLE cluster sysfs */
static struct attribute *rfx_little_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(rfx_little);

/* BIG cluster sysfs */
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

/* === KTHREAD WORKER ===
 * Runtime trimmed to 1.5ms / 8ms deadline / 8ms period: tight enough
 * to land inside one frame at 120Hz, loose enough not to starve other
 * SCHED_DEADLINE tasks (audio, display compose).
 */

static int rfx_kthread_create(struct rfx_policy *rfx_pol)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size = sizeof(struct sched_attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_flags = SCHED_FLAGS_UGOV,
		.sched_runtime = 1500000,	/* 1.5 ms */
		.sched_deadline = 8000000,	/* 8 ms   */
		.sched_period = 8000000,	/* 8 ms   */
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
	rfx_pol->last_heavy_update_ns = 0;
	rfx_pol->policy_iowait_active = false;

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
	unsigned int start_freq;

	rfx_pol->up_rate_delay_ns = (s64)rfx_pol->tunables->up_rate_limit_us * NSEC_PER_USEC;
	rfx_pol->down_rate_delay_ns = (s64)rfx_pol->tunables->down_rate_limit_us * NSEC_PER_USEC;

	now = ktime_get_ns();
	rfx_pol->last_upfreq_time = now;
	rfx_pol->last_downfreq_time = now;
	rfx_pol->last_heavy_update_ns = 0;

	start_freq = policy->cur > 0 ? policy->cur : policy->cpuinfo.min_freq;
	rfx_pol->next_freq = start_freq;
	rfx_pol->work_in_progress = false;
	rfx_pol->limits_changed = false;
	rfx_pol->need_freq_update = false;
	rfx_pol->avg_util_pct = 0;
	rfx_pol->prev_gaming = gaming;
	rfx_pol->prev_target_freq = start_freq;
	rfx_pol->last_boost_time_ns = 0;
	rfx_pol->policy_iowait_active = false;

	if (gaming) {
		enum rfx_cluster_type cluster =
			rfx_get_cluster_type(cpumask_first(policy->cpus));
		unsigned int snap_pct;

		switch (cluster) {
		case RFX_CLUSTER_PRIME:
			snap_pct = RFX_ASYM_PRIME_FLOOR_PCT;
			break;
		case RFX_CLUSTER_BIG:
			snap_pct = RFX_ASYM_BIG_FLOOR_PCT;
			break;
		default:
			snap_pct = RFX_ASYM_LITTLE_FLOOR_PCT;
			break;
		}

		rfx_pol->fps_state.hint = RFX_BUILTIN_FPS_HINT;
		rfx_pol->fps_state.actual = RFX_BUILTIN_ACTUAL_FPS;
		/* Pre-seed the smoother target at the cluster floor so the
		 * very first scheduler event puts the policy at game-ready
		 * freq without staircasing. */
		rfx_pol->prev_target_freq =
			rfx_adaptive_floor(policy, snap_pct);
		rfx_pol->last_boost_time_ns = now;
	}
	rfx_frame_tracker_init(&rfx_pol->frame_tracker, rfx_pol->fps_state.hint);

	uu = policy_is_shared(policy) ? rfx_update_shared : rfx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		rfx_c = per_cpu_ptr(&rfx_cpu, cpu);
		memset(rfx_c, 0, sizeof(*rfx_c));
		rfx_c->cpu = cpu;
		rfx_c->rfx_policy = rfx_pol;
		if (gaming)
			rfx_predictor_seed(&rfx_c->predictor, 50, now);
		else
			rfx_predictor_init(&rfx_c->predictor);
		rfx_c->iowait_boost = 0;
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
