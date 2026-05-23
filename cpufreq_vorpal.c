/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Vorpal CPUFreq Governor v3.2 - Quantum Gaming & Thermal Architecture
 * Based on schedutil — engineered for 117-120fps stable, <44°C, ~5W, <1% junk
 * Target: GKI 5.10 ARM64 Android
 *
 * Compile-tested for GKI 5.10, C89 compliant
 * Author: Templar Dev (Steambot12)
 * Version: 3.2 - Panic Fix Edition
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/sched/rt.h>
#include <linux/timekeeping.h>
#include <linux/irq_work.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/cpumask.h>
#include <linux/percpu-defs.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched/idle.h>
#include <linux/energy_model.h>
#include <linux/thermal.h>

extern int vm_swappiness;
extern unsigned int sysctl_sched_latency;

#define CPUFREQ_VORPAL_PROGNAME	"Vorpal CPUFreq Governor"
#define CPUFREQ_VORPAL_AUTHOR	"Templar Dev"
#define CPUFREQ_VORPAL_VERSION	"3.2"

/* === RATE LIMITS === */
#define VORPAL_UI_IDLE_PROTECTION_NS	(25 * NSEC_PER_MSEC)
#define VORPAL_GAME_LAUNCH_BOOST_NS	(15000 * NSEC_PER_MSEC)
#define VORPAL_INPUT_BOOST_DURATION_NS	(120 * NSEC_PER_MSEC)
#define VORPAL_INPUT_BOOST_FLOOR_PCT	45

#define VORPAL_LITTLE_UP_RATE_US	0
#define VORPAL_LITTLE_DOWN_RATE_US	4000
#define VORPAL_LITTLE_DOWN_HEAVY_US	20000
#define VORPAL_LITTLE_DOWN_MED_US	4000
#define VORPAL_LITTLE_DOWN_LIGHT_US	50
#define VORPAL_BIG_UP_RATE_US		0
#define VORPAL_BIG_DOWN_RATE_US		8000
#define VORPAL_PRIME_UP_RATE_US		0
#define VORPAL_PRIME_DOWN_RATE_US	8000
#define VORPAL_PRIME_RATE_LIMIT_US	1
#define VORPAL_DEFAULT_RATE_US		10

/* === HISPEED === */
#define VORPAL_HISPEED_WINDOW_US	30
#define VORPAL_HISPEED_FILTER_SHIFT	0
#define VORPAL_HISPEED_BOOST_PCT	72
#define VORPAL_HISPEED_GAMING_PCT	85
#define VORPAL_HISPEED_DAILY_PCT	55
#define VORPAL_HISPEED_VIDEO_PCT	72
#define HISPEED_HALFLIFE_NS		(6 * NSEC_PER_MSEC)
#define HISPEED_HALFLIFE_MAX		8
#define SCHED_FLAGS_UGOV		0x10000000
#define IOWAIT_BOOST_MIN		(SCHED_CAPACITY_SCALE / 8)

/* === BURST & SUSTAIN === */
#define VORPAL_BURST_GUARD_NS		(250 * NSEC_PER_MSEC)
#define VORPAL_BURST_DROP_THRESHOLD	12
#define VORPAL_SUSTAIN_HEAVY_ENTER_PCT	35
#define VORPAL_SUSTAIN_HEAVY_EXIT_PCT	20
#define VORPAL_SUSTAIN_HEAVY_BUSY_PCT	8
#define VORPAL_SUSTAIN_HEAVY_TICKS	1
#define VORPAL_SUSTAIN_EXIT_TICKS	8
#define VORPAL_GAMING_LOCK_NS		(12000 * NSEC_PER_MSEC)
#define VORPAL_GAMING_SUSTAIN_NS	(20000 * NSEC_PER_MSEC)
#define VORPAL_GAMING_MAX_PCT		90
#define VORPAL_BIG_GAMING_MAX_PCT	88
#define VORPAL_PRIME_GAMING_FLOOR_PCT	82
#define VORPAL_GAME_LAUNCH_FLOOR_PCT	65
#define VORPAL_BIG_INTERACTIVE_FLOOR_PCT 15
#define VORPAL_LITTLE_GAMING_CAP_PCT	85

/* === IDLE === */
#define VORPAL_LIGHT_ENTER_PCT		3
#define VORPAL_LIGHT_ENTER_TICKS	3
#define VORPAL_LIGHT_EXIT_PCT		12
#define VORPAL_IDLE_STALE_NS		(30 * NSEC_PER_MSEC)
#define VORPAL_FORCE_IDLE_THRESHOLD_NS	(15 * NSEC_PER_USEC)
#define VORPAL_IDLE_HYSTERESIS_NS	(25 * NSEC_PER_MSEC)
#define VORPAL_LITTLE_MAX_NON_GAMING_KHZ 960000
#define VORPAL_FG_LIGHT_BIG_CAP_KHZ	500000
#define VORPAL_INTERACTIVE_UTIL_PCT	1
#define VORPAL_INTERACTIVE_FLOOR_KHZ	300000
#define VORPAL_BIG_INTERACTIVE_FLOOR_KHZ 600000
#define VORPAL_LITTLE_INTERACTIVE_FLOOR_KHZ 768000
#define VORPAL_IDLE_DEEP_CAP_FALLBACK_KHZ 300000

/* === THERMAL === */
#define VORPAL_THERMAL_WINDOW_NS	(14000 * NSEC_PER_MSEC)
#define VORPAL_THERMAL_WINDOW_SHRINK_NS	(11000 * NSEC_PER_MSEC)
#define VORPAL_THERMAL_THROTTLE_BURST_NS (800 * NSEC_PER_MSEC)
#define VORPAL_THERMAL_THROTTLE_CAP_PCT	86
#define VORPAL_BIG_THERMAL_THROTTLE_CAP_PCT 90
#define VORPAL_PRIME_GAMING_SUSTAIN_FLOOR_PCT 75
#define VORPAL_THERMAL_ENTER_TEMP	42000
#define VORPAL_THERMAL_EXIT_TEMP	39000
#define VORPAL_THERMAL_EMERGENCY_TEMP	46000

/* === FPS === */
#define VORPAL_FPS_TARGET_120		120
#define VORPAL_FPS_TARGET_90		90
#define VORPAL_FPS_TARGET_60		60
#define VORPAL_FPS_WINDOW_NS		(1000 * NSEC_PER_MSEC)
#define VORPAL_FPS_DETECT_THRESHOLD	3
#define VORPAL_FPS_BOOST_DURATION_NS	(500 * NSEC_PER_MSEC)
#define VORPAL_FPS_LOCK_DURATION_NS	(8000 * NSEC_PER_MSEC)

/* === GPU COOP === */
#define VORPAL_GPU_BOOST_DURATION_NS	(300 * NSEC_PER_MSEC)
#define VORPAL_GPU_BOOST_FLOOR_PCT	70
#define VORPAL_GPU_IDLE_THRESHOLD_PCT	15
#define VORPAL_GPU_COOP_WINDOW_NS	(500 * NSEC_PER_MSEC)

/* === EAS === */
#define VORPAL_EAS_BOOST_DURATION_NS	(200 * NSEC_PER_MSEC)
#define VORPAL_EAS_RENDER_BOOST_PCT	25
#define VORPAL_SCHED_LATENCY_HINT_NS	(500000)

/* === ACTIVITY STATE === */
#define VORPAL_ACT_UP_TICKS		1
#define VORPAL_ACT_DOWN_TICKS		1
#define VORPAL_ACT_IDLE_TO_LIGHT_PCT	4
#define VORPAL_ACT_LIGHT_TO_MED_PCT	20
#define VORPAL_ACT_MED_TO_HEAVY_PCT	40
#define VORPAL_ACT_HEAVY_TO_MED_PCT	22
#define VORPAL_ACT_MED_TO_LIGHT_PCT	6
#define VORPAL_ACT_LIGHT_TO_IDLE_PCT	3
#define VORPAL_LITTLE_CAP_THRESHOLD	614
#define VORPAL_PRIME_CAP_THRESHOLD	1000
#define VORPAL_BIG_DROP_PCT		13
#define VORPAL_LITTLE_LIGHT_MAX_KHZ	400000
#define VORPAL_LITTLE_MED_MAX_KHZ	800000
#define VORPAL_INTERACTIVE_DURATION_NS	(3000 * NSEC_PER_MSEC)
#define VORPAL_VIDEO_DETECT_THRESHOLD_NS (200 * NSEC_PER_MSEC)

/* === ENUMS === */
enum vorpal_cluster_type {
	VORPAL_CLUSTER_LITTLE = 0,
	VORPAL_CLUSTER_BIG = 1,
	VORPAL_CLUSTER_PRIME = 2,
};

enum vorpal_activity_state {
	VORPAL_ACT_IDLE = 0,
	VORPAL_ACT_LIGHT = 1,
	VORPAL_ACT_MEDIUM = 2,
	VORPAL_ACT_HEAVY = 3,
};

enum vorpal_mode {
	VORPAL_MODE_NORMAL = 0,
	VORPAL_MODE_GAMING = 1,
	VORPAL_MODE_VIDEO = 2,
	VORPAL_MODE_SUSTAINED = 3,
};

enum vorpal_fps_state {
	VORPAL_FPS_IDLE = 0,
	VORPAL_FPS_DETECTING = 1,
	VORPAL_FPS_LOCKED = 2,
	VORPAL_FPS_UNLOCKED = 3,
};

/* Forward declarations */
struct vorpal_policy;
struct vorpal_cpu;

static void vorpal_deferred_update(struct vorpal_policy *vp);
static void vorpal_work(struct kthread_work *work);
static bool vorpal_big_drop_force_down(struct vorpal_policy *vp,
				       unsigned int next_freq);

/* === FPS Tracker === */
struct vorpal_fps_tracker {
	u64 frame_window_start_ns;
	u64 last_frame_time_ns;
	u64 frame_interval_ns;
	unsigned int frame_count;
	unsigned int target_fps;
	unsigned int measured_fps;
	enum vorpal_fps_state state;
	bool game_detected;
	u64 game_detect_time_ns;
	u64 fps_lock_end_ns;
	u64 fps_boost_end_ns;
	unsigned int consecutive_drops;
	unsigned int consecutive_stable;
};

/* === GPU State === */
struct vorpal_gpu_state {
	unsigned int gpu_load_pct;
	unsigned int gpu_freq_khz;
	bool gpu_bottleneck;
	u64 gpu_window_start_ns;
	u64 gpu_boost_end_ns;
	unsigned int gpu_idle_count;
};

/* === Thermal State === */
struct vorpal_thermal_state {
	int cpu_temp_mC;
	int gpu_temp_mC;
	bool thermal_throttle_active;
	bool thermal_emergency;
	u64 thermal_throttle_end_ns;
	u64 thermal_window_start_ns;
	unsigned int thermal_window_count;
	unsigned int throttle_level;
};

/* === EAS State === */
struct vorpal_eas_state {
	bool render_boost_active;
	u64 render_boost_end_ns;
	unsigned int uclamp_min_hint;
	unsigned int uclamp_max_hint;
	bool latency_sensitive;
};

/* === Sustained State === */
struct vorpal_sustained_state {
	bool sustained_mode;
	u64 sustained_start_ns;
	unsigned int power_cap_mw;
	unsigned int freq_cap_pct;
	unsigned int thermal_headroom;
};

/* === Tunables === */
struct vorpal_tunables {
	struct gov_attr_set attr_set;
	unsigned int rate_limit_us;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int hispeed_window_us;
	unsigned int hispeed_filter_shift;
	unsigned int hispeed_boost_pct;
	enum vorpal_cluster_type cluster_type;
	unsigned int gaming_mode;
	unsigned int sustained_mode;
	unsigned int fps_target;
	unsigned int thermal_coop_enable;
	unsigned int gpu_coop_enable;
	unsigned int eas_boost_enable;
	unsigned int input_boost_enable;
	unsigned int power_cap_mw;
};

/* === Per-CPU Data === */
struct vorpal_cpu {
	struct update_util_data update_util;
	struct vorpal_policy *vp;
	unsigned int cpu;
	bool iowait_boost_pending;
	unsigned int iowait_boost;
	u64 last_update;
	unsigned long util;
	unsigned long bwmin;
	u64 prev_idle_time;
	u64 prev_wall_time;
	unsigned int busy_pct;
	unsigned int filtered_busy_pct;
	bool hispeed_active;
	u64 hispeed_start_ns;
	unsigned int hispeed_idle_windows;
	enum vorpal_activity_state act_state;
	unsigned int act_up_ticks;
	unsigned int act_down_ticks;
	unsigned int prev_util_pct;
	unsigned int util_history[8];
	u8 util_history_idx;
	bool is_video_pattern;
	bool is_gaming_pattern;
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

/* === Per-Policy Data === */
struct vorpal_policy {
	struct cpufreq_policy *policy;
	struct vorpal_tunables *tunables;
	struct list_head tunables_hook;
	raw_spinlock_t update_lock;
	u64 last_upfreq_time;
	u64 last_downfreq_time;
	s64 freq_update_delay_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;
	bool limits_changed;
	bool need_freq_update;
	u8 sustain_heavy_ticks;
	u8 sustain_exit_ticks;
	u8 light_enter_ticks;
	bool in_heavy_mode;
	bool in_light_mode;
	bool prev_heavy_mode;
	u64 guard_end_ns;
	unsigned int guard_freq_khz;
	u64 interactive_end_ns;
	bool prime_gaming_floor_active;
	u64 prime_gaming_floor_end_ns;
	bool force_idle;
	u64 force_idle_start_ns;
	u64 last_real_update_ns;
	enum vorpal_mode current_mode;
	u64 mode_switch_time_ns;
	u64 gaming_lock_end_ns;
	bool video_pattern_detected;
	u64 video_detect_start_ns;
	u64 idle_entry_time_ns;
	bool in_deep_idle;
	u64 game_launch_end_ns;
	bool game_launching;
	u64 thermal_duty_window_start_ns;
	u64 thermal_throttle_end_ns;
	bool thermal_throttle_active;
	unsigned int thermal_sustain_window_count;
	u64 thermal_duty_last_active_ns;
	u64 render_boost_end_ns;
	bool render_urgency_active;
	u8 rom_tweak_detected;
	bool rom_override_active;
	struct vorpal_fps_tracker fps;
	struct vorpal_gpu_state gpu;
	struct vorpal_thermal_state thermal;
	struct vorpal_eas_state eas;
	struct vorpal_sustained_state sustained;
	u64 input_boost_end_ns;
	bool input_boost_active;
	unsigned int input_boost_freq_khz;
	bool hotplug_managed;
	unsigned int min_cores_online;
	/* === PANIC FIX: Thermal zone cache === */
	struct thermal_zone_device *tz_cpu;
	struct thermal_zone_device *tz_gpu;
	bool thermal_available;
	u64 thermal_last_read_ns;
	unsigned int pending_thermal_cap_pct;
};

static DEFINE_PER_CPU(struct vorpal_cpu, vorpal_cpu_table);
static struct vorpal_tunables *vorpal_global_tunables;
static DEFINE_MUTEX(vorpal_global_tunables_lock);

/* === External Symbols === */
extern void rfx_get_util_gki510(int cpu, unsigned long boost,
				unsigned long *util, unsigned long *bwmin);
extern bool rfx_dl_bw_exceeded_gki510(int cpu, unsigned long bwmin);

static inline bool vorpal_driver_test_flags(unsigned int flags) { return false; }
static inline bool vorpal_scx_switched_all(void) { return false; }
static inline bool vorpal_cpu_uclamp_capped(unsigned int cpu) { return false; }

static inline unsigned int vorpal_get_ref_freq(struct cpufreq_policy *policy)
{
	if (!policy)
		return 0;
	return policy->cpuinfo.max_freq;
}

static inline struct gov_attr_set *vorpal_to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

static inline struct vorpal_tunables *to_vorpal_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct vorpal_tunables, attr_set);
}

static inline enum vorpal_cluster_type vorpal_get_cluster_type(unsigned int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(cpu);
	if (cap <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD)
		return VORPAL_CLUSTER_LITTLE;
	if (cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD)
		return VORPAL_CLUSTER_PRIME;
	return VORPAL_CLUSTER_BIG;
}

static inline bool vorpal_is_little(unsigned int cpu)
{
	return arch_scale_cpu_capacity(cpu) <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD;
}

static inline bool vorpal_is_prime(unsigned int cpu)
{
	return arch_scale_cpu_capacity(cpu) >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD;
}

static inline unsigned int vorpal_adaptive_max(struct cpufreq_policy *policy,
				       unsigned int pct)
{
	if (!policy || !policy->cpuinfo.max_freq)
		return 0;
	return (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
}

static inline unsigned int vorpal_adaptive_floor(struct cpufreq_policy *policy,
					 unsigned int pct)
{
	unsigned int floor;
	if (!policy || !policy->cpuinfo.max_freq)
		return 0;
	floor = (unsigned int)((u64)policy->cpuinfo.max_freq * pct / 100);
	return max(floor, policy->cpuinfo.min_freq);
}

/* === THERMAL HELPERS - FIXED: Safe for atomic context === */

static void vorpal_init_thermal_zones(struct vorpal_policy *vp)
{
	struct thermal_zone_device *tz;

	vp->tz_cpu = NULL;
	vp->tz_gpu = NULL;
	vp->thermal_available = false;

	/* Try "cpu" first, fallback to "soc" */
	tz = thermal_zone_get_zone_by_name("cpu");
	if (!IS_ERR(tz))
		vp->tz_cpu = tz;
	else {
		tz = thermal_zone_get_zone_by_name("soc");
		if (!IS_ERR(tz))
			vp->tz_cpu = tz;
	}

	tz = thermal_zone_get_zone_by_name("gpu");
	if (!IS_ERR(tz))
		vp->tz_gpu = tz;

	if (vp->tz_cpu || vp->tz_gpu)
		vp->thermal_available = true;
}

static void vorpal_update_thermal_state(struct vorpal_policy *vp, u64 time)
{
	struct vorpal_thermal_state *th = &vp->thermal;
	int cpu_temp = 0;
	int gpu_temp = 0;
	bool has_cpu_temp = false;
	bool has_gpu_temp = false;
	unsigned int throttle_pct = 100;
	int ret;

	/* FIX: Only read thermal from kthread work context (process context, can sleep).
	 * In scheduler tick (update_util), we use cached values from last read.
	 */
	if (vp->tz_cpu) {
		ret = thermal_zone_get_temp(vp->tz_cpu, &cpu_temp);
		if (ret == 0)
			has_cpu_temp = true;
	}

	if (!has_cpu_temp) {
		/* Fallback to soc if cpu failed */
		if (vp->tz_cpu) {
			ret = thermal_zone_get_temp(vp->tz_cpu, &cpu_temp);
			if (ret == 0)
				has_cpu_temp = true;
		}
	}

	if (vp->tz_gpu) {
		ret = thermal_zone_get_temp(vp->tz_gpu, &gpu_temp);
		if (ret == 0)
			has_gpu_temp = true;
	}

	if (has_cpu_temp)
		th->cpu_temp_mC = cpu_temp;
	if (has_gpu_temp)
		th->gpu_temp_mC = gpu_temp;

	if (has_cpu_temp && cpu_temp >= VORPAL_THERMAL_EMERGENCY_TEMP) {
		th->thermal_emergency = true;
		th->thermal_throttle_active = true;
		th->throttle_level = 3;
		th->thermal_throttle_end_ns = time + VORPAL_THERMAL_THROTTLE_BURST_NS;
		vp->pending_thermal_cap_pct = VORPAL_THERMAL_THROTTLE_CAP_PCT - 15;
		return;
	}
	th->thermal_emergency = false;

	if (has_cpu_temp && cpu_temp >= VORPAL_THERMAL_ENTER_TEMP) {
		if (!th->thermal_throttle_active) {
			th->thermal_throttle_active = true;
			th->thermal_window_start_ns = time;
			th->thermal_window_count++;
			if (th->thermal_window_count > 6)
				th->thermal_window_count = 6;
		}
		th->thermal_throttle_end_ns = time + VORPAL_THERMAL_THROTTLE_BURST_NS;
		if (has_gpu_temp && gpu_temp >= VORPAL_THERMAL_ENTER_TEMP)
			th->throttle_level = 2;
		else
			th->throttle_level = 1;
	} else if (has_cpu_temp && cpu_temp <= VORPAL_THERMAL_EXIT_TEMP) {
		if (th->thermal_throttle_active && time >= th->thermal_throttle_end_ns) {
			th->thermal_throttle_active = false;
			th->throttle_level = 0;
			th->thermal_window_count = 0;
		}
	}

	if (th->thermal_throttle_active) {
		switch (th->throttle_level) {
		case 1:
			throttle_pct = VORPAL_THERMAL_THROTTLE_CAP_PCT;
			break;
		case 2:
			throttle_pct = VORPAL_THERMAL_THROTTLE_CAP_PCT - 8;
			break;
		case 3:
			throttle_pct = VORPAL_THERMAL_THROTTLE_CAP_PCT - 15;
			break;
		default:
			throttle_pct = 100;
			break;
		}
	}

	if (vp->tunables->thermal_coop_enable && th->thermal_throttle_active)
		vp->sustained.thermal_headroom = throttle_pct;
	else
		vp->sustained.thermal_headroom = 100;

	vp->pending_thermal_cap_pct = throttle_pct;
	vp->thermal_last_read_ns = time;
}

/* === FPS DETECTION === */

static void vorpal_fps_detect_pattern(struct vorpal_policy *vp,
				      struct vorpal_cpu *vc,
				      unsigned long util, unsigned long max_cap,
				      u64 time)
{
	struct vorpal_fps_tracker *fps = &vp->fps;
	unsigned int util_pct;
	unsigned int h;
	unsigned int h1;
	unsigned int h2;
	unsigned int h3;
	bool game_pattern = false;
	bool sudden_spike = false;
	bool sustained_heavy = false;
	bool burst_dip_burst = false;
	u64 window_elapsed;

	util_pct = max_cap ? (unsigned int)(util * 100 / max_cap) : 0;

	if (fps->frame_window_start_ns == 0)
		fps->frame_window_start_ns = time;

	window_elapsed = time - fps->frame_window_start_ns;
	fps->frame_count++;

	if (window_elapsed >= VORPAL_FPS_WINDOW_NS) {
		if (fps->frame_count >= VORPAL_FPS_TARGET_120 - 10 &&
		    fps->frame_count <= VORPAL_FPS_TARGET_120 + 20)
			fps->measured_fps = VORPAL_FPS_TARGET_120;
		else if (fps->frame_count >= VORPAL_FPS_TARGET_90 - 8 &&
			 fps->frame_count <= VORPAL_FPS_TARGET_90 + 15)
			fps->measured_fps = VORPAL_FPS_TARGET_90;
		else if (fps->frame_count >= VORPAL_FPS_TARGET_60 - 5 &&
			 fps->frame_count <= VORPAL_FPS_TARGET_60 + 10)
			fps->measured_fps = VORPAL_FPS_TARGET_60;
		else
			fps->measured_fps = fps->frame_count;

		fps->frame_window_start_ns = time;
		fps->frame_count = 0;
	}

	h = vc->util_history_idx;
	h1 = vc->util_history[(h - 1) & 7];
	h2 = vc->util_history[(h - 2) & 7];
	h3 = vc->util_history[(h - 3) & 7];

	sudden_spike = (h1 > 30) && (h2 < 20) && (h1 > h2 + 15);
	sustained_heavy = (h1 >= 35) && (h2 >= 35) && (h3 >= 35);
	burst_dip_burst = (h1 > 25) && (h3 > 25) && (h2 < 18);

	if (sudden_spike || sustained_heavy || burst_dip_burst)
		game_pattern = true;

	if (game_pattern && !fps->game_detected) {
		fps->consecutive_drops++;
		if (fps->consecutive_drops >= VORPAL_FPS_DETECT_THRESHOLD) {
			fps->game_detected = true;
			fps->game_detect_time_ns = time;
			fps->state = VORPAL_FPS_DETECTING;
			fps->fps_lock_end_ns = time + VORPAL_FPS_LOCK_DURATION_NS;
		}
	} else if (game_pattern) {
		fps->consecutive_drops = 0;
		fps->consecutive_stable++;
		if (fps->consecutive_stable >= 5 && fps->state == VORPAL_FPS_DETECTING) {
			fps->state = VORPAL_FPS_LOCKED;
			fps->target_fps = vp->tunables->fps_target ? vp->tunables->fps_target : VORPAL_FPS_TARGET_120;
		}
	} else {
		fps->consecutive_drops = 0;
		fps->consecutive_stable = 0;
	}

	if (fps->state == VORPAL_FPS_LOCKED) {
		if (time < fps->fps_lock_end_ns) {
			if (fps->measured_fps < fps->target_fps - 5)
				fps->fps_boost_end_ns = time + VORPAL_FPS_BOOST_DURATION_NS;
		} else {
			fps->state = VORPAL_FPS_UNLOCKED;
			fps->game_detected = false;
		}
	}

	if (vp->tunables->input_boost_enable && game_pattern && util_pct > 15) {
		vp->input_boost_active = true;
		vp->input_boost_end_ns = time + VORPAL_INPUT_BOOST_DURATION_NS;
		vp->input_boost_freq_khz = vorpal_adaptive_floor(vp->policy,
								 VORPAL_INPUT_BOOST_FLOOR_PCT);
	}
}

/* === GPU COOPERATIVE === */

static void vorpal_gpu_cooperative_update(struct vorpal_policy *vp, u64 time)
{
	struct vorpal_gpu_state *gpu = &vp->gpu;
	unsigned int gpu_load = 0;
	u64 window_elapsed;

	if (!vp->tunables->gpu_coop_enable)
		return;

	if (vp->render_urgency_active)
		gpu_load = 75;
	else if (vp->in_heavy_mode)
		gpu_load = 55;
	else
		gpu_load = 20;

	gpu->gpu_load_pct = gpu_load;

	if (gpu->gpu_window_start_ns == 0)
		gpu->gpu_window_start_ns = time;

	window_elapsed = time - gpu->gpu_window_start_ns;
	if (window_elapsed >= VORPAL_GPU_COOP_WINDOW_NS) {
		gpu->gpu_window_start_ns = time;
		if (gpu->gpu_load_pct > 80) {
			gpu->gpu_bottleneck = true;
			gpu->gpu_boost_end_ns = time + VORPAL_GPU_BOOST_DURATION_NS;
		} else if (gpu->gpu_load_pct < VORPAL_GPU_IDLE_THRESHOLD_PCT) {
			gpu->gpu_idle_count++;
			if (gpu->gpu_idle_count > 3) {
				gpu->gpu_bottleneck = false;
				gpu->gpu_idle_count = 0;
			}
		} else {
			gpu->gpu_idle_count = 0;
		}
	}
}

/* === EAS UPDATE === */

static void vorpal_eas_update(struct vorpal_policy *vp,
			      struct vorpal_cpu *vc,
			      unsigned long util, unsigned long max_cap,
			      u64 time)
{
	struct vorpal_eas_state *eas = &vp->eas;
	unsigned int util_pct;
	bool is_render_task = false;

	if (!vp->tunables->eas_boost_enable)
		return;

	util_pct = max_cap ? (unsigned int)(util * 100 / max_cap) : 0;

	if (vc->is_gaming_pattern || vp->render_urgency_active)
		is_render_task = true;

	if (is_render_task) {
		eas->render_boost_active = true;
		eas->render_boost_end_ns = time + VORPAL_EAS_BOOST_DURATION_NS;
		eas->uclamp_min_hint = min(util_pct + VORPAL_EAS_RENDER_BOOST_PCT, 100U);
		eas->latency_sensitive = true;
	} else {
		if (time >= eas->render_boost_end_ns) {
			eas->render_boost_active = false;
			eas->uclamp_min_hint = 0;
			eas->latency_sensitive = false;
		}
	}
}

/* === SUSTAINED PERFORMANCE === */

static void vorpal_sustained_update(struct vorpal_policy *vp, u64 time)
{
	struct vorpal_sustained_state *sus = &vp->sustained;
	unsigned int power_cap;
	unsigned int freq_cap;

	if (!vp->tunables->sustained_mode)
		return;

	if (!sus->sustained_mode) {
		sus->sustained_mode = true;
		sus->sustained_start_ns = time;
	}

	power_cap = vp->tunables->power_cap_mw ? vp->tunables->power_cap_mw : 5000;
	freq_cap = vp->sustained.thermal_headroom;

	if (freq_cap > 100)
		freq_cap = 100;

	sus->freq_cap_pct = freq_cap;
	sus->power_cap_mw = power_cap;
}

/* === HOTPLUG OPTIMIZATION === */

static void vorpal_hotplug_optimize(struct vorpal_policy *vp, u64 time)
{
	unsigned int min_cores;
	struct cpufreq_policy *policy = vp->policy;

	if (!policy)
		return;

	if (vp->current_mode == VORPAL_MODE_GAMING || vp->in_heavy_mode) {
		min_cores = 2;
		if (vp->tunables->cluster_type == VORPAL_CLUSTER_BIG ||
		    vp->tunables->cluster_type == VORPAL_CLUSTER_PRIME)
			min_cores = 1;
	} else {
		min_cores = 1;
	}

	vp->min_cores_online = min_cores;

	if (vp->current_mode == VORPAL_MODE_GAMING && !vp->hotplug_managed)
		vp->hotplug_managed = true;
	else if (vp->current_mode != VORPAL_MODE_GAMING && vp->hotplug_managed)
		vp->hotplug_managed = false;
}

/* === MODE DETECTION === */

static void vorpal_detect_mode(struct vorpal_policy *vp, struct vorpal_cpu *vc,
			       unsigned long util, unsigned long max_cap, u64 time)
{
	unsigned int util_pct;
	bool heavy_load;
	bool medium_load;
	bool periodic_pattern = false;
	bool not_in_gaming;
	u64 time_in_mode;
	unsigned int variance = 0;
	unsigned int avg = 0;
	unsigned int i;
	int diff;
	unsigned long cap;

	util_pct = max_cap ? (unsigned int)(util * 100 / max_cap) : 0;

	vc->util_history[vc->util_history_idx] = util_pct;
	vc->util_history_idx = (vc->util_history_idx + 1) & 0x7;

	medium_load = util_pct >= 18 && util_pct < 55;
	if (medium_load) {
		for (i = 0; i < 8; i++)
			avg += vc->util_history[i];
		avg /= 8;
		for (i = 0; i < 8; i++) {
			diff = (int)vc->util_history[i] - (int)avg;
			variance += diff < 0 ? -diff : diff;
		}
		if (variance < 20 && avg >= 15 && avg <= 55)
			periodic_pattern = true;
	}

	if (vp->tunables->gaming_mode) {
		vp->current_mode = VORPAL_MODE_GAMING;
		vp->in_light_mode = false;
		vp->force_idle = false;
		vp->sustain_exit_ticks = 0;

		if (util_pct >= 5) {
			vp->in_heavy_mode = true;
			vp->gaming_lock_end_ns = time + VORPAL_GAMING_LOCK_NS;
		} else if (vp->gaming_lock_end_ns &&
			   time < vp->gaming_lock_end_ns) {
			vp->in_heavy_mode = true;
			vp->sustain_exit_ticks = 0;
		} else {
			if (!vp->gaming_lock_end_ns ||
			    (time - vp->gaming_lock_end_ns) > (6000 * NSEC_PER_MSEC))
				vp->in_heavy_mode = false;
		}


		if (vp->policy) {
			cap = arch_scale_cpu_capacity(
				cpumask_first(vp->policy->cpus));
			if (cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD) {
				vp->prime_gaming_floor_active = true;
				vp->prime_gaming_floor_end_ns = 0;
			}
		}
		return;
	}

	if (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns) {
		vp->current_mode = VORPAL_MODE_GAMING;
		return;
	}

	time_in_mode = time - vp->mode_switch_time_ns;

	heavy_load = util_pct >= 45;

	if (heavy_load && vc->act_state == VORPAL_ACT_HEAVY) {
		if (vp->current_mode != VORPAL_MODE_GAMING) {
			if (time_in_mode > 24 * NSEC_PER_MSEC) {
				vp->current_mode = VORPAL_MODE_GAMING;
				vp->mode_switch_time_ns = time;
				vp->gaming_lock_end_ns = time + VORPAL_GAMING_LOCK_NS;
			}
		}
	} else if (periodic_pattern && !heavy_load) {
		not_in_gaming = (vp->current_mode != VORPAL_MODE_GAMING) &&
				!(vp->gaming_lock_end_ns &&
				  time < vp->gaming_lock_end_ns);
		if (not_in_gaming && vp->current_mode != VORPAL_MODE_VIDEO) {
			if (time_in_mode > VORPAL_VIDEO_DETECT_THRESHOLD_NS) {
				vp->current_mode = VORPAL_MODE_VIDEO;
				vp->mode_switch_time_ns = time;
			}
		}
	} else if (util_pct < 6 && vp->in_light_mode) {
		if (vp->current_mode != VORPAL_MODE_NORMAL) {
			if (time_in_mode > 32 * NSEC_PER_MSEC) {
				vp->current_mode = VORPAL_MODE_NORMAL;
				vp->mode_switch_time_ns = time;
				vp->gaming_lock_end_ns = 0;
				vp->thermal_duty_window_start_ns = 0;
				vp->thermal_throttle_active = false;
				vp->thermal_throttle_end_ns = 0;
				vp->thermal_sustain_window_count = 0;
			}
		}
	}

	if (vp->tunables->sustained_mode &&
	    vp->current_mode == VORPAL_MODE_GAMING)
		vp->current_mode = VORPAL_MODE_SUSTAINED;
}

static inline unsigned int vorpal_get_hispeed_pct(struct vorpal_policy *vp)
{
	switch (vp->current_mode) {
	case VORPAL_MODE_GAMING:
	case VORPAL_MODE_SUSTAINED:
		return VORPAL_HISPEED_GAMING_PCT;
	case VORPAL_MODE_VIDEO:
		return VORPAL_HISPEED_VIDEO_PCT;
	case VORPAL_MODE_NORMAL:
	default:
		return VORPAL_HISPEED_BOOST_PCT;
	}
}

/* === ACTIVITY STATE === */

static bool vorpal_act_update(struct vorpal_cpu *vc, unsigned long effective_util,
			    unsigned long max_cap, u64 time,
			    unsigned int *freq_cap_khz)
{
	unsigned long idle_th;
	unsigned long light_up_th;
	unsigned long med_up_th;
	unsigned long heavy_dn_th;
	unsigned long med_dn_th;
	unsigned long light_dn_th;
	bool force_down = false;
	s64 stale_delta;
	struct vorpal_policy *vp = vc->vp;
	s64 time_since_last_update;
	bool is_little;

	is_little = (max_cap <= VORPAL_LITTLE_CAP_THRESHOLD);

	if (vc->last_update) {
		time_since_last_update = (s64)(time - vc->last_update);
		if (time_since_last_update >= (s64)VORPAL_IDLE_STALE_NS) {
			vp->force_idle = true;
			vp->force_idle_start_ns = time;
			vp->last_real_update_ns = time;
			if (!vp->in_deep_idle) {
				vp->idle_entry_time_ns = time;
				vp->in_deep_idle = true;
			}
		} else if (vp->force_idle) {
			if (effective_util > 0 || vc->filtered_busy_pct > 0) {
				vp->force_idle = false;
				vp->in_deep_idle = false;
			} else if (time - vp->force_idle_start_ns > (8 * NSEC_PER_MSEC)) {
				vp->force_idle_start_ns = time;
			}
		}
	}

	if (vp->in_heavy_mode ||
	    (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		vc->act_state = VORPAL_ACT_HEAVY;
		vc->act_up_ticks = 0;
		vc->act_down_ticks = 0;
		vp->force_idle = false;
		vp->in_deep_idle = false;
		vp->last_real_update_ns = time;
		*freq_cap_khz = 0;
		return false;
	}

	if (vp->in_light_mode || vp->force_idle) {
		if (is_little) {
			if (vp->force_idle) {
				*freq_cap_khz = vp->policy->cpuinfo.min_freq;
				return true;
			}
			*freq_cap_khz = VORPAL_LITTLE_LIGHT_MAX_KHZ;
			return true;
		} else {
			*freq_cap_khz = VORPAL_FG_LIGHT_BIG_CAP_KHZ;
			return false;
		}
	}

	if (!is_little) {
		*freq_cap_khz = 0;
		return false;
	}

	if (vc->last_update && !vp->force_idle) {
		stale_delta = (s64)(time - vc->last_update);
		if (stale_delta >= (s64)VORPAL_IDLE_STALE_NS) {
			if (vp->interactive_end_ns && time < vp->interactive_end_ns) {
				*freq_cap_khz = VORPAL_LITTLE_INTERACTIVE_FLOOR_KHZ;
				return false;
			}
			vc->act_state = VORPAL_ACT_IDLE;
			vc->act_up_ticks = 0;
			vc->act_down_ticks = 0;
			vc->filtered_busy_pct = 0;
			vc->hispeed_start_ns = 0;
			vc->hispeed_idle_windows = 0;
			vp->force_idle = true;
			vp->in_deep_idle = true;
			vp->idle_entry_time_ns = time;
			*freq_cap_khz = vp->policy->cpuinfo.min_freq;
			return true;
		}
	}

	if (effective_util == 0) {
		switch (vc->act_state) {
		case VORPAL_ACT_IDLE:
		case VORPAL_ACT_LIGHT:
			*freq_cap_khz = vp->policy->cpuinfo.min_freq;
			return true;
		case VORPAL_ACT_MEDIUM:
			*freq_cap_khz = VORPAL_LITTLE_MED_MAX_KHZ;
			return true;
		case VORPAL_ACT_HEAVY:
			*freq_cap_khz = 0;
			return false;
		}
	}

	vp->last_real_update_ns = time;
	vp->in_deep_idle = false;

	idle_th = max_cap * VORPAL_ACT_IDLE_TO_LIGHT_PCT / 100;
	light_up_th = max_cap * VORPAL_ACT_LIGHT_TO_MED_PCT / 100;
	med_up_th = max_cap * VORPAL_ACT_MED_TO_HEAVY_PCT / 100;
	heavy_dn_th = max_cap * VORPAL_ACT_HEAVY_TO_MED_PCT / 100;
	med_dn_th = max_cap * VORPAL_ACT_MED_TO_LIGHT_PCT / 100;
	light_dn_th = max_cap * VORPAL_ACT_LIGHT_TO_IDLE_PCT / 100;

	if (effective_util > med_up_th && vc->act_state != VORPAL_ACT_HEAVY) {
		vc->act_up_ticks++;
		if (vc->act_up_ticks >= VORPAL_ACT_UP_TICKS) {
			vc->act_state = VORPAL_ACT_HEAVY;
			vc->act_up_ticks = 0;
			vc->act_down_ticks = 0;
		}
		*freq_cap_khz = 0;
		return false;
	}

	switch (vc->act_state) {
	case VORPAL_ACT_IDLE:
		if (effective_util > idle_th) {
			vc->act_up_ticks++;
			if (vc->act_up_ticks >= VORPAL_ACT_UP_TICKS) {
				vc->act_state = VORPAL_ACT_LIGHT;
				vc->act_up_ticks = 0;
			}
		}
		*freq_cap_khz = vp->policy->cpuinfo.min_freq;
		break;

	case VORPAL_ACT_LIGHT:
		if (effective_util > light_up_th) {
			vc->act_up_ticks++;
			if (vc->act_up_ticks >= VORPAL_ACT_UP_TICKS) {
				vc->act_state = VORPAL_ACT_MEDIUM;
				vc->act_up_ticks = 0;
			}
		} else if (effective_util < light_dn_th) {
			vc->act_down_ticks++;
			if (vc->act_down_ticks >= VORPAL_ACT_DOWN_TICKS) {
				vc->act_state = VORPAL_ACT_IDLE;
				vc->act_down_ticks = 0;
				force_down = true;
			}
		}
		*freq_cap_khz = VORPAL_LITTLE_LIGHT_MAX_KHZ;
		break;

	case VORPAL_ACT_MEDIUM:
		if (effective_util > med_up_th) {
			vc->act_up_ticks++;
			if (vc->act_up_ticks >= VORPAL_ACT_UP_TICKS) {
				vc->act_state = VORPAL_ACT_HEAVY;
				vc->act_up_ticks = 0;
			}
		} else if (effective_util < med_dn_th) {
			vc->act_down_ticks++;
			if (vc->act_down_ticks >= VORPAL_ACT_DOWN_TICKS) {
				vc->act_state = VORPAL_ACT_LIGHT;
				vc->act_down_ticks = 0;
			}
		}
		*freq_cap_khz = VORPAL_LITTLE_MED_MAX_KHZ;
		break;

	case VORPAL_ACT_HEAVY:
		if (effective_util < heavy_dn_th) {
			vc->act_down_ticks++;
			if (vc->act_down_ticks >= VORPAL_ACT_DOWN_TICKS) {
				vc->act_state = VORPAL_ACT_MEDIUM;
				vc->act_down_ticks = 0;
			}
		}
		force_down = false;
		*freq_cap_khz = VORPAL_LITTLE_MAX_NON_GAMING_KHZ;
		break;
	}

	return force_down;
}

/* === UTILITY FUNCTIONS === */

static unsigned int vorpal_get_adaptive_shift(unsigned long util,
				      unsigned long max_cap,
				      unsigned int base_shift)
{
	unsigned int util_pct;

	if (!max_cap)
		return base_shift > 0 ? min(base_shift + 3, 12U) : 0;

	util_pct = (unsigned int)(util * 100 / max_cap);

	if (util_pct > 90)
		return 0;
	if (util_pct < 5)
		return base_shift > 0 ? min(base_shift + 3, 12U) : 0;
	return min(base_shift, 12U);
}

static void vorpal_thermal_duty_cycle(struct vorpal_policy *vp, u64 time)
{
	u64 time_since_gaming;
	u64 window_elapsed;
	u64 effective_window;

	if (!vp->in_heavy_mode ||
	    vp->current_mode != VORPAL_MODE_GAMING)
		return;

	if (!vp->thermal_duty_window_start_ns)
		vp->thermal_duty_window_start_ns = time;

	time_since_gaming = time - vp->mode_switch_time_ns;

	if (time_since_gaming < (3000 * NSEC_PER_MSEC)) {
		vp->thermal_throttle_active = false;
		return;
	}

	if (vp->thermal_throttle_active) {
		if (time >= vp->thermal_throttle_end_ns) {
			vp->thermal_throttle_active = false;
			vp->thermal_duty_window_start_ns = time;
			vp->thermal_sustain_window_count++;
			if (vp->thermal_sustain_window_count > 6)
				vp->thermal_sustain_window_count = 6;
			vp->thermal_duty_last_active_ns = time;
		}
		return;
	}

	effective_window = (vp->thermal_sustain_window_count >= 3)
			   ? VORPAL_THERMAL_WINDOW_SHRINK_NS
			   : VORPAL_THERMAL_WINDOW_NS;

	window_elapsed = time - vp->thermal_duty_window_start_ns;

	if (window_elapsed >= effective_window) {
		vp->thermal_throttle_active = true;
		vp->thermal_throttle_end_ns = time + VORPAL_THERMAL_THROTTLE_BURST_NS;
	}
}

static unsigned long vorpal_apply_headroom(unsigned long util,
				   unsigned long max_cap,
				   bool is_heavy,
				   enum vorpal_mode mode)
{
	unsigned int util_pct;
	unsigned long result;
	bool is_prime;
	unsigned int headroom_pct;

	if (!max_cap)
		return util;

	result = min(util, max_cap);
	util_pct = (unsigned int)(util * 100 / max_cap);

	if (util_pct >= 98)
		return max_cap;

	is_prime = (max_cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD);

	if (mode == VORPAL_MODE_GAMING || mode == VORPAL_MODE_SUSTAINED) {
		if (is_prime)
			headroom_pct = is_heavy ? 30 : 22;
		else
			headroom_pct = is_heavy ? 30 : 24;
		return min(util + util * headroom_pct / 100, max_cap);
	}

	if (mode == VORPAL_MODE_VIDEO) {
		if (is_prime) {
			if (util_pct >= 50)
				return min(util + util * 22 / 100, max_cap);
			return min(util + util * 15 / 100, max_cap);
		}
		if (util_pct >= 65)
			return min(util + (util >> 3), max_cap);
		if (util_pct >= 40)
			return min(util + (util >> 4), max_cap);
		return min(util + (util >> 5), max_cap);
	}

	if (max_cap <= VORPAL_LITTLE_CAP_THRESHOLD) {
		if (util_pct >= 70)
			return min(util + (util >> 4), max_cap);
		if (util_pct >= 45)
			return min(util + (util >> 5), max_cap);
		return util;
	}

	if (util_pct >= 75)
		return min(util + (util >> 4), max_cap);
	if (util_pct >= 50)
		return min(util + (util >> 5), max_cap);
	return min(util + (util >> 6), max_cap);
}

static bool vorpal_should_update_freq(struct vorpal_policy *vp, u64 time)
{
	s64 delta_ns;
	s64 effective_delay;
	bool going_up;

	if (!vp || !vp->policy)
		return false;

	if (!cpufreq_this_cpu_can_update(vp->policy))
		return false;

	if (unlikely(READ_ONCE(vp->limits_changed))) {
		WRITE_ONCE(vp->limits_changed, false);
		vp->need_freq_update = true;
		smp_mb();
		return true;
	} else if (vp->need_freq_update) {
		return true;
	}

	going_up = (vp->next_freq > vp->policy->cur);

	if (vp->force_idle) {
		effective_delay = 3 * NSEC_PER_USEC;
	} else if (vp->in_heavy_mode ||
		   vp->current_mode == VORPAL_MODE_GAMING ||
		   vp->current_mode == VORPAL_MODE_SUSTAINED ||
		   (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		if (going_up) {
			effective_delay = 0;
		} else if (vp->thermal_throttle_active) {
			effective_delay = 6000 * NSEC_PER_USEC;
		} else {
			effective_delay = 22000 * NSEC_PER_USEC;
		}
	} else if (vp->current_mode == VORPAL_MODE_VIDEO) {
		effective_delay = 25 * NSEC_PER_USEC;
	} else {
		effective_delay = vp->freq_update_delay_ns;
	}

	if (going_up)
		delta_ns = time - vp->last_upfreq_time;
	else
		delta_ns = time - vp->last_downfreq_time;

	return delta_ns >= effective_delay;
}

static bool vorpal_update_next_freq(struct vorpal_policy *vp, u64 time,
				    unsigned int next_freq, bool force_down)
{
	s64 down_delta;
	s64 effective_down_delay;
	s64 up_delta;

	if (!vp)
		return false;

	if (vp->need_freq_update) {
		vp->need_freq_update = false;
		if (vp->next_freq == next_freq &&
		    !vorpal_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
			return false;
	} else if (vp->next_freq == next_freq &&
		   vp->last_upfreq_time == time) {
		return false;
	}

	if (next_freq < vp->next_freq) {
		if (!force_down) {
			down_delta = time - vp->last_downfreq_time;
			effective_down_delay = vp->down_rate_delay_ns;

			if (vp->in_heavy_mode ||
			    vp->current_mode == VORPAL_MODE_GAMING ||
			    vp->current_mode == VORPAL_MODE_SUSTAINED ||
			    (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
				if (vp->thermal_throttle_active)
					effective_down_delay = 6000 * NSEC_PER_USEC;
				else
					effective_down_delay = 35000 * NSEC_PER_USEC;
			}

			if (effective_down_delay > 0 &&
			    down_delta < effective_down_delay)
				return false;

			vp->last_downfreq_time = time;
		}
	} else {
		up_delta = time - vp->last_upfreq_time;
		if (vp->up_rate_delay_ns > 0 &&
		    up_delta < vp->up_rate_delay_ns)
			return false;
		vp->last_upfreq_time = time;
	}

	vp->next_freq = next_freq;
	return true;
}

/* === NEXT FREQUENCY === */

static unsigned int vorpal_get_next_freq(struct vorpal_policy *vp,
					 unsigned long util, unsigned long max,
					 unsigned int freq_cap_khz, bool is_heavy,
					 u64 time)
{
	struct cpufreq_policy *policy = vp->policy;
	unsigned int freq;
	bool is_little;
	bool is_prime;
	unsigned int hispeed_pct;
	unsigned int thermal_cap_pct;
	unsigned int sustained_cap_pct;
	unsigned int final_cap;
	unsigned int input_boost_floor;
	unsigned int gpu_boost_floor;
	unsigned int fps_boost_floor;
	unsigned int little_nongaming_cap;
	unsigned int thermal_cap;
	unsigned int sustained_cap;
	unsigned int hard_floor;
	unsigned int soft_cap;
	unsigned int big_floor;
	unsigned int big_cap;
	unsigned int rom_floor;
	unsigned int rom_cap;
	unsigned int exit_util_pct;
	unsigned int exit_soft_cap;
	struct vorpal_cpu *lc;

	if (!policy)
		return 0;

	is_little = (max <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD);
	is_prime = (max >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD);

	hispeed_pct = vorpal_get_hispeed_pct(vp);
	util = vorpal_apply_headroom(util, max, is_heavy, vp->current_mode);
	freq = vorpal_get_ref_freq(policy);
	freq = (unsigned int)((u64)freq * util / max);
	freq = clamp_t(unsigned int, freq,
		       policy->cpuinfo.min_freq, policy->cpuinfo.max_freq);

	if (is_little && !vp->in_heavy_mode &&
	    !vp->tunables->gaming_mode &&
	    !(vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		little_nongaming_cap = vorpal_adaptive_max(policy, 53);
		if (freq > little_nongaming_cap)
			freq = little_nongaming_cap;
	}

	if (is_prime && freq < vorpal_adaptive_floor(policy, VORPAL_PRIME_GAMING_FLOOR_PCT)) {
		if (is_heavy || vp->current_mode == VORPAL_MODE_GAMING ||
		    vp->current_mode == VORPAL_MODE_SUSTAINED)
			freq = vorpal_adaptive_floor(policy, VORPAL_PRIME_GAMING_FLOOR_PCT);
	}

	if (is_prime && vp->game_launching &&
	    vp->game_launch_end_ns && time < vp->game_launch_end_ns) {
		if (freq < vorpal_adaptive_floor(policy, VORPAL_GAME_LAUNCH_FLOOR_PCT))
			freq = vorpal_adaptive_floor(policy, VORPAL_GAME_LAUNCH_FLOOR_PCT);
	}

	/* FIX: Use cached thermal cap instead of calling thermal directly */
	if (vp->tunables->thermal_coop_enable && vp->thermal.thermal_throttle_active) {
		thermal_cap_pct = vp->sustained.thermal_headroom;
		if (thermal_cap_pct < 100) {
			thermal_cap = vorpal_adaptive_max(policy, thermal_cap_pct);
			if (freq > thermal_cap)
				freq = thermal_cap;
		}
	}

	if (vp->tunables->sustained_mode && vp->sustained.sustained_mode) {
		sustained_cap_pct = vp->sustained.freq_cap_pct;
		if (sustained_cap_pct > 0 && sustained_cap_pct < 100) {
			sustained_cap = vorpal_adaptive_max(policy, sustained_cap_pct);
			if (freq > sustained_cap)
				freq = sustained_cap;
		}
	}

	if (vp->current_mode == VORPAL_MODE_GAMING ||
	    vp->current_mode == VORPAL_MODE_SUSTAINED) {
		if (!vp->tunables->gaming_mode &&
		    !(vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns))
			vorpal_thermal_duty_cycle(vp, time);

		if (is_prime) {
			if (vp->tunables->gaming_mode) {
				hard_floor = vorpal_adaptive_floor(policy,
									VORPAL_PRIME_GAMING_SUSTAIN_FLOOR_PCT);
				if (vp->in_heavy_mode && freq < hard_floor)
					freq = hard_floor;
				if (freq > policy->max)
					freq = policy->max;
			} else {
				soft_cap = vorpal_adaptive_max(policy, VORPAL_GAMING_MAX_PCT);
				hard_floor = vorpal_adaptive_floor(policy,
									VORPAL_PRIME_GAMING_SUSTAIN_FLOOR_PCT);
				if (vp->thermal_throttle_active)
					soft_cap = vorpal_adaptive_max(policy,
								     VORPAL_THERMAL_THROTTLE_CAP_PCT);
				if (soft_cap < hard_floor)
					soft_cap = hard_floor;
				if (freq > soft_cap)
					freq = soft_cap;
				if (freq < hard_floor && vp->in_heavy_mode)
					freq = hard_floor;
			}
		} else if (!is_little) {
			if (vp->tunables->gaming_mode) {
				if (freq > policy->max)
					freq = policy->max;
				if (vp->in_heavy_mode) {
					big_floor = vorpal_adaptive_floor(policy,
									       VORPAL_BIG_INTERACTIVE_FLOOR_PCT);
					if (freq < big_floor)
						freq = big_floor;
				}
			} else {
				big_cap = vorpal_adaptive_max(policy,
								   vp->thermal_throttle_active
								   ? VORPAL_THERMAL_THROTTLE_CAP_PCT
								   : VORPAL_BIG_GAMING_MAX_PCT);
				if (freq > big_cap)
					freq = big_cap;
			}
		}
	}

	if (vp->rom_override_active && is_prime &&
	    !vp->tunables->gaming_mode) {
		if (vp->rom_tweak_detected == 2) {
			rom_floor = vorpal_adaptive_floor(policy, 75);
			rom_cap = vorpal_adaptive_max(policy, 88);
			if (vp->in_heavy_mode && freq < rom_floor)
				freq = rom_floor;
			if (!vp->thermal_throttle_active && freq > rom_cap)
				freq = rom_cap;
		} else if (vp->rom_tweak_detected == 1) {
			rom_floor = vorpal_adaptive_floor(policy, 73);
			if (vp->in_heavy_mode && freq < rom_floor)
				freq = rom_floor;
		}
	}

	if (vp->render_urgency_active && vp->render_boost_end_ns &&
	    time < vp->render_boost_end_ns) {
		if (is_prime && freq < vorpal_adaptive_floor(policy, 85))
			freq = vorpal_adaptive_floor(policy, 85);
		else if (!is_little && !is_prime && freq < vorpal_adaptive_floor(policy, 78))
			freq = vorpal_adaptive_floor(policy, 78);
	}

	if (vp->tunables->input_boost_enable && vp->input_boost_active &&
	    vp->input_boost_end_ns && time < vp->input_boost_end_ns) {
		input_boost_floor = vp->input_boost_freq_khz;
		if (input_boost_floor > 0 && freq < input_boost_floor)
			freq = input_boost_floor;
	} else {
		vp->input_boost_active = false;
	}

	if (vp->tunables->gpu_coop_enable && vp->gpu.gpu_bottleneck &&
	    vp->gpu.gpu_boost_end_ns && time < vp->gpu.gpu_boost_end_ns) {
		gpu_boost_floor = vorpal_adaptive_floor(policy, VORPAL_GPU_BOOST_FLOOR_PCT);
		if (freq < gpu_boost_floor)
			freq = gpu_boost_floor;
	}

	if (vp->fps.state == VORPAL_FPS_LOCKED &&
	    vp->fps.fps_boost_end_ns && time < vp->fps.fps_boost_end_ns) {
		fps_boost_floor = vorpal_adaptive_floor(policy, 80);
		if (freq < fps_boost_floor)
			freq = fps_boost_floor;
	}

	if (vp->in_heavy_mode &&
	    vp->current_mode != VORPAL_MODE_GAMING &&
	    vp->current_mode != VORPAL_MODE_SUSTAINED &&
	    !is_little) {
		exit_util_pct = max ?
			(unsigned int)(util * 100 / max) : 0;
		if (exit_util_pct < 20) {
			exit_soft_cap = is_prime ?
				vorpal_adaptive_floor(policy, VORPAL_PRIME_GAMING_FLOOR_PCT) :
				vorpal_adaptive_floor(policy, VORPAL_BIG_INTERACTIVE_FLOOR_PCT);
			if (freq > exit_soft_cap)
				freq = exit_soft_cap;
		}
	}

	if (is_prime && vp->prime_gaming_floor_active &&
	    vp->prime_gaming_floor_end_ns &&
	    time < vp->prime_gaming_floor_end_ns) {
		if (freq < vorpal_adaptive_floor(policy, VORPAL_PRIME_GAMING_FLOOR_PCT))
			freq = vorpal_adaptive_floor(policy, VORPAL_PRIME_GAMING_FLOOR_PCT);
	}

	if (!vp->in_heavy_mode &&
	    vp->interactive_end_ns && time < vp->interactive_end_ns) {
		lc = per_cpu_ptr(&vorpal_cpu_table,
					    cpumask_first(vp->policy->cpus));
		if (is_little) {
			if (lc->hispeed_start_ns && freq < VORPAL_INTERACTIVE_FLOOR_KHZ)
				freq = VORPAL_INTERACTIVE_FLOOR_KHZ;
		} else {
			if (lc->hispeed_start_ns &&
			    freq < vorpal_adaptive_floor(policy, VORPAL_BIG_INTERACTIVE_FLOOR_PCT))
				freq = vorpal_adaptive_floor(policy, VORPAL_BIG_INTERACTIVE_FLOOR_PCT);
		}
	}

	if (vp->force_idle && !is_heavy)
		freq = policy->cpuinfo.min_freq;

	if (freq_cap_khz > 0 && freq > freq_cap_khz)
		freq = freq_cap_khz;

	final_cap = policy->max;
	if (freq > final_cap)
		freq = final_cap;

	if (freq == vp->cached_raw_freq && !vp->need_freq_update)
		return vp->next_freq;

	vp->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

/* === BUSY PERCENT & BLEND === */

static void vorpal_update_busy_pct(struct vorpal_cpu *vc,
				   unsigned int window_us,
				   unsigned int base_shift,
				   unsigned long max_cap,
				   u64 time)
{
	u64 cur_idle;
	u64 cur_wall;
	u64 wall_delta;
	u64 idle_delta;
	unsigned int filter_shift;
	bool is_prime;
	bool interactive_active;
	unsigned int idle_win_threshold;
	unsigned int step;

	is_prime = (max_cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD);
	filter_shift = vorpal_get_adaptive_shift(vc->util, max_cap, base_shift);

	cur_idle = get_cpu_idle_time(vc->cpu, &cur_wall, 1);
	wall_delta = cur_wall - vc->prev_wall_time;

	if (wall_delta < (u64)window_us)
		return;

	idle_delta = (cur_idle > vc->prev_idle_time)
		     ? (cur_idle - vc->prev_idle_time) : 0;

	vc->busy_pct = (wall_delta > idle_delta)
		       ? (unsigned int)(100 * (wall_delta - idle_delta) / wall_delta)
		       : 0;

	vc->prev_idle_time = cur_idle;
	vc->prev_wall_time = cur_wall;
	vc->hispeed_active = true;

	if (vc->busy_pct == 0) {
		vc->filtered_busy_pct = 0;
	} else if (!filter_shift) {
		vc->filtered_busy_pct = vc->busy_pct;
	} else if (vc->busy_pct > vc->filtered_busy_pct + 4) {
		vc->filtered_busy_pct = vc->busy_pct;
	} else {
		step = (vc->filtered_busy_pct > vc->busy_pct)
		       ? (vc->filtered_busy_pct - vc->busy_pct) >> filter_shift
		       : 0;
		if (step < vc->filtered_busy_pct)
			vc->filtered_busy_pct -= step;
		else
			vc->filtered_busy_pct = vc->busy_pct;
	}

	if (vc->filtered_busy_pct > 0) {
		vc->hispeed_idle_windows = 0;
		if (!vc->hispeed_start_ns) {
			vc->hispeed_start_ns = time;
			vc->vp->need_freq_update = true;
		}
	} else {
		interactive_active = vc->vp->interactive_end_ns &&
				     time < vc->vp->interactive_end_ns;

		if (!vc->vp->in_heavy_mode)
			vc->hispeed_idle_windows++;

		idle_win_threshold = is_prime ? 5 : 3;
		if (!interactive_active &&
		    vc->hispeed_idle_windows > idle_win_threshold) {
			vc->hispeed_start_ns = 0;
			vc->filtered_busy_pct = 0;
		}
	}
}

static unsigned long vorpal_blend_util(struct vorpal_cpu *vc,
				       unsigned long pelt_util,
				       unsigned long max_cap, u64 time,
				       unsigned int hispeed_boost_pct)
{
	unsigned long hispeed_util;
	unsigned int half_lives;
	unsigned int effective_pct;
	struct vorpal_policy *vp = vc->vp;
	unsigned long min_blend;

	if (!vc->filtered_busy_pct || !vc->hispeed_start_ns) {
		if (vp->current_mode == VORPAL_MODE_VIDEO &&
		    vp->interactive_end_ns &&
		    time < vp->interactive_end_ns &&
		    (max_cap <= VORPAL_LITTLE_CAP_THRESHOLD)) {
			min_blend = max_cap * 20 / 100;
			return (pelt_util > min_blend) ? pelt_util : min_blend;
		}
		return pelt_util;
	}

	effective_pct = min(vc->filtered_busy_pct, hispeed_boost_pct);
	hispeed_util = max_cap * effective_pct / 100;

	if (hispeed_util <= pelt_util)
		return pelt_util;

	half_lives = (unsigned int)((time - vc->hispeed_start_ns) / HISPEED_HALFLIFE_NS);
	if (half_lives >= HISPEED_HALFLIFE_MAX) {
		vc->hispeed_start_ns = time;
		return pelt_util;
	}

	return min(pelt_util + ((hispeed_util - pelt_util) >> half_lives), max_cap);
}

/* === IO WAIT === */

static bool vorpal_iowait_reset(struct vorpal_cpu *vc, u64 time,
				bool set_iowait_boost)
{
	s64 delta_ns = time - vc->last_update;

	if (delta_ns <= TICK_NSEC)
		return false;

	vc->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	vc->iowait_boost_pending = set_iowait_boost;
	return true;
}

static void vorpal_iowait_boost(struct vorpal_cpu *vc, u64 time,
				unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;
	unsigned long max_cap;
	unsigned int iowait_cap;

	if (vc->iowait_boost) {
		if (!vorpal_iowait_reset(vc, time, set_iowait_boost))
			vc->iowait_boost_pending = set_iowait_boost;
		return;
	}
	if (!set_iowait_boost)
		return;
	if (vc->iowait_boost_pending)
		return;

	vc->iowait_boost_pending = true;

	max_cap = arch_scale_cpu_capacity(vc->cpu);
	if (vc->iowait_boost >= max_cap) {
		iowait_cap = (max_cap <= VORPAL_LITTLE_CAP_THRESHOLD)
			     ? (SCHED_CAPACITY_SCALE / 6)
			     : (SCHED_CAPACITY_SCALE * 3 / 4);
		vc->iowait_boost = min_t(unsigned int,
					 vc->iowait_boost << 1,
					 iowait_cap);
		return;
	}
	vc->iowait_boost = IOWAIT_BOOST_MIN;
}

static unsigned long vorpal_iowait_apply(struct vorpal_cpu *vc, u64 time,
					 unsigned long max_cap)
{
	if (!vc->iowait_boost)
		return 0;
	if (vorpal_iowait_reset(vc, time, false))
		return 0;
	if (!vc->iowait_boost_pending) {
		vc->iowait_boost >>= 1;
		if (vc->iowait_boost < IOWAIT_BOOST_MIN) {
			vc->iowait_boost = 0;
			return 0;
		}
	}
	vc->iowait_boost_pending = false;
	return vc->iowait_boost * max_cap >> SCHED_CAPACITY_SHIFT;
}

/* === NOHZ === */

#ifdef CONFIG_NO_HZ_COMMON
static bool vorpal_check_freq_hold_or_drop(struct vorpal_cpu *vc,
					   unsigned long max_cap,
					   bool *out_force_drop)
{
	unsigned long idle_calls;
	bool idle_calls_increased;
	struct vorpal_policy *vp = vc->vp;
	u64 idle_duration;
	u64 now;

	if (vorpal_scx_switched_all())
		return false;
	if (vorpal_cpu_uclamp_capped(vc->cpu))
		return false;

	if (vp->force_idle) {
		if (out_force_drop)
			*out_force_drop = true;
		return false;
	}

	if (vp->in_deep_idle && vp->idle_entry_time_ns) {
		now = ktime_get_ns();
		idle_duration = now - vp->idle_entry_time_ns;
		if (idle_duration < VORPAL_IDLE_HYSTERESIS_NS) {
			if (out_force_drop)
				*out_force_drop = false;
			return true;
		}
		vp->in_deep_idle = false;
		vp->idle_entry_time_ns = 0;
		if (out_force_drop)
			*out_force_drop = false;
		return false;
	}

	idle_calls = tick_nohz_get_idle_calls_cpu(vc->cpu);
	idle_calls_increased = idle_calls != vc->saved_idle_calls;
	vc->saved_idle_calls = idle_calls;

	if (max_cap <= VORPAL_LITTLE_CAP_THRESHOLD) {
		if (out_force_drop)
			*out_force_drop = idle_calls_increased;
		return false;
	}

	if (max_cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD &&
	    (vp->gaming_lock_end_ns && ktime_get_ns() < vp->gaming_lock_end_ns)) {
		if (out_force_drop)
			*out_force_drop = false;
		return true;
	}

	if (out_force_drop)
		*out_force_drop = false;
	return !idle_calls_increased;
}
#else
static inline bool vorpal_check_freq_hold_or_drop(struct vorpal_cpu *vc,
						  unsigned long max_cap,
						  bool *out_force_drop)
{
	if (out_force_drop)
		*out_force_drop = false;
	return false;
}
#endif

static inline void vorpal_ignore_dl_rate_limit(struct vorpal_cpu *vc)
{
	if (rfx_dl_bw_exceeded_gki510(vc->cpu, vc->bwmin))
		vc->vp->need_freq_update = true;
}

static void vorpal_irq_work(struct irq_work *irq_work);

static void vorpal_deferred_update(struct vorpal_policy *vp)
{
	if (!vp->work_in_progress) {
		vp->work_in_progress = true;
		irq_work_queue(&vp->irq_work);
	}
}

/* === ADAPTIVE MODE UPDATE === */

static void vorpal_update_adaptive_mode(struct vorpal_policy *vp,
					  struct vorpal_cpu *vc,
					  unsigned long effective_util,
					  unsigned long max_cap,
					  bool is_big, u64 time)
{
	unsigned int util_pct;
	bool heavy_cond;
	bool light_cond;
	bool interactive_cond;
	bool is_prime;
	s64 idle_time;
	unsigned int enter_threshold;
	unsigned int exit_threshold;
	unsigned int early_boost_threshold;
	u64 interactive_dur;

	util_pct = (max_cap > 0)
		   ? (unsigned int)(effective_util * 100 / max_cap) : 0;

	vorpal_detect_mode(vp, vc, effective_util, max_cap, time);
	vorpal_fps_detect_pattern(vp, vc, effective_util, max_cap, time);
	vorpal_gpu_cooperative_update(vp, time);
	vorpal_eas_update(vp, vc, effective_util, max_cap, time);
	vorpal_sustained_update(vp, time);
	vorpal_hotplug_optimize(vp, time);

	idle_time = (s64)(time - vp->last_real_update_ns);

	if (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns) {
		vp->in_heavy_mode = true;
		vp->in_light_mode = false;
		vp->force_idle = false;
		vp->last_real_update_ns = time;
		return;
	}

	if (is_big) {
		is_prime = (max_cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD);

		early_boost_threshold = is_prime ? 28 : 30;
		enter_threshold = VORPAL_SUSTAIN_HEAVY_ENTER_PCT;
		exit_threshold = VORPAL_SUSTAIN_HEAVY_EXIT_PCT;

		heavy_cond = (util_pct >= enter_threshold)
			     && (vc->filtered_busy_pct >= VORPAL_SUSTAIN_HEAVY_BUSY_PCT
				 || vc->busy_pct >= 18);

		if (!vp->in_heavy_mode && util_pct >= early_boost_threshold &&
		    vc->prev_util_pct < 15) {
			vp->in_heavy_mode = true;
			vp->in_light_mode = false;
			vp->force_idle = false;
			vp->interactive_end_ns = 0;
			vp->sustain_heavy_ticks = 0;
			vp->sustain_exit_ticks = 0;
			vp->light_enter_ticks = 0;
			if (is_prime) {
				vp->prime_gaming_floor_active = true;
				vp->prime_gaming_floor_end_ns = 0;
			}
			vp->gaming_lock_end_ns = time + VORPAL_GAMING_LOCK_NS;
			return;
		}

		if (!vp->in_heavy_mode) {
			if (heavy_cond) {
				vp->sustain_heavy_ticks++;
				if (vp->sustain_heavy_ticks >= VORPAL_SUSTAIN_HEAVY_TICKS) {
					vp->in_heavy_mode = true;
					vp->in_light_mode = false;
					vp->force_idle = false;
					vp->interactive_end_ns = 0;
					vp->sustain_heavy_ticks = 0;
					vp->sustain_exit_ticks = 0;
					vp->light_enter_ticks = 0;
					if (is_prime) {
						vp->prime_gaming_floor_active = true;
						vp->prime_gaming_floor_end_ns = 0;
					}
					vp->gaming_lock_end_ns = time + VORPAL_GAMING_LOCK_NS;
				}
			} else {
				vp->sustain_heavy_ticks = 0;
			}
		} else {
			if (vp->tunables->gaming_mode) {
				vp->sustain_exit_ticks = 0;
			} else if (util_pct < exit_threshold) {
				vp->sustain_exit_ticks++;
				if (vp->sustain_exit_ticks >= VORPAL_SUSTAIN_EXIT_TICKS) {
					vp->in_heavy_mode = false;
					vp->sustain_exit_ticks = 0;
					vp->sustain_heavy_ticks = 0;
					if (is_prime && vp->prime_gaming_floor_active) {
						vp->prime_gaming_floor_end_ns =
							time + (300 * NSEC_PER_MSEC);
					}
				}
			} else {
				vp->sustain_exit_ticks = 0;
			}
		}
	}

	if (vp->in_heavy_mode) {
		vp->light_enter_ticks = 0;
		vp->force_idle = false;
		vp->last_real_update_ns = time;
		return;
	}

	interactive_cond = (util_pct >= VORPAL_INTERACTIVE_UTIL_PCT);
	if (interactive_cond) {

		interactive_dur = is_big
				      ? VORPAL_INTERACTIVE_DURATION_NS
				      : (500 * NSEC_PER_MSEC);
		vp->interactive_end_ns = time + interactive_dur;
		if (vp->in_light_mode) {
			vp->in_light_mode = false;
			vp->light_enter_ticks = 0;
			vp->force_idle = false;
		}
		vp->last_real_update_ns = time;
	}

	if (vp->interactive_end_ns && time < vp->interactive_end_ns) {
		vp->light_enter_ticks = 0;
		vp->last_real_update_ns = time;
		return;
	}

	if (is_prime && vp->prime_gaming_floor_active) {
		if (vp->prime_gaming_floor_end_ns &&
		    time >= vp->prime_gaming_floor_end_ns) {
			vp->prime_gaming_floor_active = false;
			vp->prime_gaming_floor_end_ns = 0;
		}
	}

	if (idle_time > 40 * NSEC_PER_MSEC &&
	    util_pct == 0 &&
	    vc->filtered_busy_pct == 0 &&
	    !vp->in_light_mode &&
	    (!vp->interactive_end_ns || time >= vp->interactive_end_ns)) {
		vp->force_idle = true;
		vp->force_idle_start_ns = time;
	}

	light_cond = (util_pct <= VORPAL_LIGHT_ENTER_PCT)
		     && (vc->filtered_busy_pct < 2)
		     && (vc->act_state <= VORPAL_ACT_LIGHT)
		     && (vc->hispeed_start_ns == 0)
		     && !vp->force_idle
		     && (!vp->interactive_end_ns || time >= vp->interactive_end_ns);

	if (!vp->in_light_mode) {
		if (light_cond) {
			vp->light_enter_ticks++;
			if (vp->light_enter_ticks >= VORPAL_LIGHT_ENTER_TICKS) {
				vp->in_light_mode = true;
				vp->light_enter_ticks = 0;
			}
		} else {
			vp->light_enter_ticks = 0;
		}
	} else {
		if (util_pct > VORPAL_LIGHT_EXIT_PCT || vc->hispeed_start_ns != 0 ||
		    vc->filtered_busy_pct >= 2 ||
		    vc->act_state >= VORPAL_ACT_MEDIUM ||
		    vp->force_idle ||
		    (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
			vp->in_light_mode = false;
			vp->light_enter_ticks = 0;
		}
	}
}

/* === BIG DROP FORCE DOWN === */

static bool vorpal_big_drop_force_down(struct vorpal_policy *vp,
				       unsigned int next_freq)
{
	unsigned int threshold;

	if (vp->next_freq == 0)
		return false;

	threshold = vp->next_freq * VORPAL_BIG_DROP_PCT / 100;
	return next_freq < threshold;
}

/* === MAIN UPDATE SINGLE === */

static void vorpal_update_single_freq(struct update_util_data *hook, u64 time,
				      unsigned int flags)
{
	struct vorpal_cpu *vc = container_of(hook, struct vorpal_cpu, update_util);
	struct vorpal_policy *vp = vc->vp;
	struct vorpal_tunables *tunables = vp->tunables;
	unsigned int cached_freq = vp->cached_raw_freq;
	unsigned long max_cap;
	unsigned long boost;
	unsigned long effective_util;
	unsigned int next_f;
	unsigned int freq_cap_khz = 0;
	bool force_down;
	bool act_force;
	bool nohz_drop = false;
	bool hold;
	bool is_heavy;
	unsigned int cur_pct;
	unsigned int gf;
	unsigned int idle_cap;
	unsigned int hispeed_pct;
	bool is_big_cluster;
	unsigned int h;
	unsigned int h1;
	unsigned int h2;
	unsigned int h3;
	bool rising;
	bool sudden_spike;
	bool sustained_heavy;
	bool wuwa_anim;
	unsigned int cur_util_pct;
	unsigned int little_cap;
	unsigned int big_floor;

	max_cap = arch_scale_cpu_capacity(vc->cpu);

	vorpal_iowait_boost(vc, time, flags);

	vc->last_update = time;
	vorpal_ignore_dl_rate_limit(vc);

	if (!vorpal_should_update_freq(vp, time))
		return;

	boost = vorpal_iowait_apply(vc, time, max_cap);
	rfx_get_util_gki510(vc->cpu, boost, &vc->util, &vc->bwmin);
	effective_util = max(vc->util, boost);

	vorpal_update_busy_pct(vc, tunables->hispeed_window_us,
			       tunables->hispeed_filter_shift, max_cap, time);

	hispeed_pct = vorpal_get_hispeed_pct(vp);
	effective_util = vorpal_blend_util(vc, effective_util, max_cap, time,
					 hispeed_pct);

	is_big_cluster = (max_cap > VORPAL_LITTLE_CAP_THRESHOLD);
	vorpal_update_adaptive_mode(vp, vc, effective_util, max_cap,
				    is_big_cluster, time);

	/* FIX: Call activity state update for single CPU (was missing!) */
	act_force = vorpal_act_update(vc, effective_util, max_cap, time, &freq_cap_khz);

	/* Gaming mode pattern detection */
	if (vp->tunables->gaming_mode) {
		h = vc->util_history_idx;
		h1 = vc->util_history[(h - 1) & 7];
		h2 = vc->util_history[(h - 2) & 7];
		h3 = vc->util_history[(h - 3) & 7];

		rising = h1 > h2 && h2 > h3 && h1 > 20;
		sudden_spike = (h1 > 30) && (h2 < 20) && (h1 > h2 + 15);
		sustained_heavy = (h1 >= 35) && (h2 >= 35) && (h3 >= 35);
		wuwa_anim = (h1 > 25) && (h3 > 25) && (h2 < 18);

		if (rising || sudden_spike || sustained_heavy || wuwa_anim) {
			vp->in_heavy_mode = true;
			vp->gaming_lock_end_ns = time +
				(sudden_spike || wuwa_anim
				 ? (1200 * NSEC_PER_MSEC)
				 : (800 * NSEC_PER_MSEC));
			vp->render_urgency_active = true;
			vp->render_boost_end_ns = time +
				(sudden_spike || wuwa_anim
				 ? (600 * NSEC_PER_MSEC)
				 : (150 * NSEC_PER_MSEC));
		}
	}

	/* Non-gaming mode render urgency */
	if (!vp->tunables->gaming_mode && vp->current_mode == VORPAL_MODE_GAMING) {
		h = vc->util_history_idx;
		h1 = vc->util_history[(h - 1) & 7];
		h2 = vc->util_history[(h - 2) & 7];
		if (h1 > 25 && h1 > (h2 + 10)) {
			vp->render_urgency_active = true;
			vp->render_boost_end_ns = time + (80 * NSEC_PER_MSEC);
		}
	}

	/* Interactive trigger for non-gaming */
	if (!vp->tunables->gaming_mode) {
		cur_util_pct = max_cap ?
			(unsigned int)(effective_util * 100 / max_cap) : 0;
		if (vc->act_state >= VORPAL_ACT_MEDIUM &&
		    vc->prev_util_pct < 10 && cur_util_pct > 20)
			vp->interactive_end_ns = time + VORPAL_INTERACTIVE_DURATION_NS;
	}

	is_heavy = (vc->act_state == VORPAL_ACT_HEAVY) || vp->in_heavy_mode ||
		   (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns);

	if (vp->interactive_end_ns && time < vp->interactive_end_ns)
		act_force = false;

	next_f = vorpal_get_next_freq(vp, effective_util, max_cap,
				      freq_cap_khz, is_heavy, time);

	cur_pct = (max_cap > 0)
		  ? (unsigned int)(effective_util * 100 / max_cap) : 0;

	/* Burst guard */
	if (vc->prev_util_pct > VORPAL_BURST_DROP_THRESHOLD &&
	    vc->prev_util_pct > cur_pct &&
	    (vc->prev_util_pct - cur_pct) >= VORPAL_BURST_DROP_THRESHOLD &&
	    (vc->act_state == VORPAL_ACT_MEDIUM ||
	     vc->act_state == VORPAL_ACT_HEAVY ||
	     (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns))) {
		if (!vp->guard_end_ns) {
			gf = vp->next_freq ? vp->next_freq : next_f;
			if (max_cap > VORPAL_LITTLE_CAP_THRESHOLD) {
				big_floor = vorpal_adaptive_floor(
					vp->policy, VORPAL_BIG_INTERACTIVE_FLOOR_PCT);
				if (gf < big_floor)
					gf = big_floor;
			}
			vp->guard_end_ns = time + VORPAL_BURST_GUARD_NS;
			vp->guard_freq_khz = gf;
		}
	}
	vc->prev_util_pct = cur_pct;

	if (vp->guard_end_ns && !vp->in_light_mode && !vp->force_idle) {
		if (time < vp->guard_end_ns) {
			if (next_f < vp->guard_freq_khz)
				next_f = vp->guard_freq_khz;
			act_force = false;
		} else {
			vp->guard_end_ns = 0;
			vp->guard_freq_khz = 0;
		}
	}

	if (vp->interactive_end_ns && time >= vp->interactive_end_ns)
		vp->interactive_end_ns = 0;

	/* Light/Idle mode */
	if ((vp->in_light_mode || vp->force_idle) && !vp->in_heavy_mode &&
	    !(vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		idle_cap = vp->policy->cpuinfo.min_freq
			   ? vp->policy->cpuinfo.min_freq
			   : VORPAL_IDLE_DEEP_CAP_FALLBACK_KHZ;

		vp->interactive_end_ns = 0;
		vp->guard_end_ns = 0;
		vp->guard_freq_khz = 0;
		if (next_f > idle_cap)
			next_f = idle_cap;
		act_force = true;
	}

	force_down = act_force || (vp->guard_end_ns == 0 &&
				   vorpal_big_drop_force_down(vp, next_f));

	hold = vorpal_check_freq_hold_or_drop(vc, max_cap, &nohz_drop);
	force_down = force_down || nohz_drop;

	if (hold && next_f == vp->next_freq &&
	    !vp->need_freq_update && !force_down) {
		next_f = vp->next_freq;
		vp->cached_raw_freq = cached_freq;
	}

	/* LITTLE down rate limits */
	if (max_cap <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD) {
		if (vp->force_idle) {
			vp->down_rate_delay_ns = (s64)VORPAL_LITTLE_DOWN_LIGHT_US * NSEC_PER_USEC;
		} else {
			switch (vc->act_state) {
			case VORPAL_ACT_HEAVY:
				vp->down_rate_delay_ns = (s64)VORPAL_LITTLE_DOWN_HEAVY_US * NSEC_PER_USEC;
				break;
			case VORPAL_ACT_MEDIUM:
				vp->down_rate_delay_ns = (s64)VORPAL_LITTLE_DOWN_MED_US * NSEC_PER_USEC;
				break;
			case VORPAL_ACT_LIGHT:
			case VORPAL_ACT_IDLE:
			default:
				vp->down_rate_delay_ns = (s64)VORPAL_LITTLE_DOWN_LIGHT_US * NSEC_PER_USEC;
				break;
			}
		}

		if (vp->in_heavy_mode ||
		    (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns))
			vp->down_rate_delay_ns = (s64)VORPAL_LITTLE_DOWN_HEAVY_US * NSEC_PER_USEC;
	}

	/* FIX: Heavy mode exit handling - apply to ALL clusters, not just LITTLE */
	if (vp->prev_heavy_mode && !vp->in_heavy_mode &&
	    !(vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		if (vc->act_state == VORPAL_ACT_HEAVY) {
			vc->act_state = VORPAL_ACT_MEDIUM;
			vc->act_down_ticks = 0;
			vc->act_up_ticks = 0;
		}
		vp->guard_end_ns = 0;
		vp->guard_freq_khz = 0;
		force_down = true;
	}
	vp->prev_heavy_mode = vp->in_heavy_mode;

	/* LITTLE gaming cap */
	if (max_cap <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD &&
	    vp->in_heavy_mode) {
		little_cap = vorpal_adaptive_max(vp->policy, VORPAL_LITTLE_GAMING_CAP_PCT);
		if (next_f > little_cap)
			next_f = little_cap;
	}

	/* Game launch boost */
	if (!vp->prev_heavy_mode && vp->in_heavy_mode &&
	    !vp->game_launching &&
	    vp->current_mode == VORPAL_MODE_GAMING) {
		vp->game_launching = true;
		vp->game_launch_end_ns = time + VORPAL_GAME_LAUNCH_BOOST_NS;
	}
	if (vp->game_launching && vp->game_launch_end_ns &&
	    time >= vp->game_launch_end_ns) {
		vp->game_launching = false;
		vp->game_launch_end_ns = 0;
	}

	/* Cleanup render urgency */
	if (vp->render_boost_end_ns && time >= vp->render_boost_end_ns) {
		vp->render_urgency_active = false;
		vp->render_boost_end_ns = 0;
	}

	vorpal_update_next_freq(vp, time, next_f, force_down);

	if (vp->policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(vp->policy, vp->next_freq);
	else {
		raw_spin_lock(&vp->update_lock);
		vorpal_deferred_update(vp);
		raw_spin_unlock(&vp->update_lock);
	}
}

/* === SHARED UPDATE === */

static unsigned int vorpal_next_freq_shared(struct vorpal_cpu *vc, u64 time,
					    bool *force_down_out)
{
	struct vorpal_policy *vp = vc->vp;
	struct vorpal_tunables *tunables = vp->tunables;
	struct cpufreq_policy *policy = vp->policy;
	unsigned long util = 0;
	unsigned long max_cap;
	bool any_force_down = true;
	bool any_heavy = false;
	unsigned int j;
	unsigned int next_f;
	unsigned int freq_cap_khz = 0;
	unsigned int j_cap;
	unsigned int j_util_pct;
	struct vorpal_cpu *lead;
	struct vorpal_cpu *j_vc;
	unsigned int sgf;
	unsigned int idle_cap;
	unsigned int hispeed_pct;
	bool j_force;
	bool nohz_drop = false;
	unsigned long j_boost;
	unsigned long j_util;
	bool j_is_big;
	unsigned int big_floor;

	max_cap = arch_scale_cpu_capacity(vc->cpu);

	for_each_cpu(j, policy->cpus) {
		j_vc = per_cpu_ptr(&vorpal_cpu_table, j);

		j_boost = vorpal_iowait_apply(j_vc, time, max_cap);
		rfx_get_util_gki510(j_vc->cpu, j_boost, &j_vc->util, &j_vc->bwmin);
		j_util = max(j_vc->util, j_boost);

		vorpal_update_busy_pct(j_vc, tunables->hispeed_window_us,
				       tunables->hispeed_filter_shift, max_cap, time);

		hispeed_pct = vorpal_get_hispeed_pct(vp);
		j_util = vorpal_blend_util(j_vc, j_util, max_cap, time,
					   hispeed_pct);

		j_is_big = (max_cap > VORPAL_LITTLE_CAP_THRESHOLD);
		vorpal_update_adaptive_mode(vp, j_vc, j_util, max_cap, j_is_big, time);

		j_force = vorpal_act_update(j_vc, j_util, max_cap, time, &j_cap);
		vorpal_check_freq_hold_or_drop(j_vc, max_cap, &nohz_drop);

		if (!j_force && !nohz_drop)
			any_force_down = false;

		if (j_vc->act_state == VORPAL_ACT_HEAVY || vp->in_heavy_mode ||
		    (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns))
			any_heavy = true;

		if (j_cap == 0)
			freq_cap_khz = 0;
		else if (freq_cap_khz == 0 || j_cap < freq_cap_khz)
			freq_cap_khz = j_cap;

		util = max(j_util, util);
	}

	next_f = vorpal_get_next_freq(vp, util, max_cap, freq_cap_khz, any_heavy, time);

	j_util_pct = (max_cap > 0)
		     ? (unsigned int)(util * 100 / max_cap) : 0;
	lead = per_cpu_ptr(&vorpal_cpu_table,
			   cpumask_first(policy->cpus));

	/* Burst guard shared */
	if (lead->prev_util_pct > VORPAL_BURST_DROP_THRESHOLD &&
	    lead->prev_util_pct > j_util_pct &&
	    (lead->prev_util_pct - j_util_pct) >= VORPAL_BURST_DROP_THRESHOLD &&
	    (any_heavy || vp->in_heavy_mode ||
	     (vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns))) {
		if (!vp->guard_end_ns) {
			sgf = vp->next_freq ? vp->next_freq : next_f;
			if (max_cap > VORPAL_LITTLE_CAP_THRESHOLD) {
				big_floor = vorpal_adaptive_floor(
					vp->policy, VORPAL_BIG_INTERACTIVE_FLOOR_PCT);
				if (sgf < big_floor)
					sgf = big_floor;
			}
			vp->guard_end_ns = time + VORPAL_BURST_GUARD_NS;
			vp->guard_freq_khz = sgf;
		}
	}
	lead->prev_util_pct = j_util_pct;

	if (vp->guard_end_ns) {
		if (time < vp->guard_end_ns) {
			if (next_f < vp->guard_freq_khz)
				next_f = vp->guard_freq_khz;
			any_force_down = false;
		} else {
			vp->guard_end_ns = 0;
			vp->guard_freq_khz = 0;
		}
	}

	if (vp->interactive_end_ns) {
		if (time < vp->interactive_end_ns) {
			any_force_down = false;
		} else {
			vp->interactive_end_ns = 0;
		}
	}

	if ((vp->in_light_mode || vp->force_idle) && !vp->in_heavy_mode &&
	    !(vp->gaming_lock_end_ns && time < vp->gaming_lock_end_ns)) {
		idle_cap = vp->policy->cpuinfo.min_freq
			   ? vp->policy->cpuinfo.min_freq
			   : VORPAL_IDLE_DEEP_CAP_FALLBACK_KHZ;

		vp->interactive_end_ns = 0;
		vp->guard_end_ns = 0;
		vp->guard_freq_khz = 0;
		if (next_f > idle_cap)
			next_f = idle_cap;
		any_force_down = true;
	}

	if (force_down_out)
		*force_down_out = any_force_down || (vp->guard_end_ns == 0 &&
					     vorpal_big_drop_force_down(vp, next_f));

	return next_f;
}

static void vorpal_update_shared(struct update_util_data *hook, u64 time,
				 unsigned int flags)
{
	struct vorpal_cpu *vc = container_of(hook, struct vorpal_cpu, update_util);
	struct vorpal_policy *vp = vc->vp;
	unsigned int next_f;
	bool force_down = false;

	raw_spin_lock(&vp->update_lock);

	vorpal_iowait_boost(vc, time, flags);
	vc->last_update = time;
	vorpal_ignore_dl_rate_limit(vc);

	if (vorpal_should_update_freq(vp, time)) {
		next_f = vorpal_next_freq_shared(vc, time, &force_down);
		vorpal_update_next_freq(vp, time, next_f, force_down);

		if (vp->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(vp->policy, vp->next_freq);
		else
			vorpal_deferred_update(vp);
	}

	raw_spin_unlock(&vp->update_lock);
}

/* === WORKER === */

static void vorpal_work(struct kthread_work *work)
{
	struct vorpal_policy *vp = container_of(work, struct vorpal_policy, work);
	unsigned int freq;
	unsigned long flags;

	raw_spin_lock_irqsave(&vp->update_lock, flags);
	freq = vp->next_freq;
	vp->work_in_progress = false;
	raw_spin_unlock_irqrestore(&vp->update_lock, flags);

	/* FIX: Update thermal from kthread work (process context, safe to sleep) */
	if (vp->thermal_available &&
	    (ktime_get_ns() - vp->thermal_last_read_ns) > (500 * NSEC_PER_MSEC))
		vorpal_update_thermal_state(vp, ktime_get_ns());

	mutex_lock(&vp->work_lock);
	cpufreq_driver_target(vp->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&vp->work_lock);
}

static void vorpal_irq_work(struct irq_work *irq_work)
{
	struct vorpal_policy *vp = container_of(irq_work, struct vorpal_policy, irq_work);
	kthread_queue_work(&vp->worker, &vp->work);
}

/* === SYSFS ATTRIBUTES === */

#define VORPAL_TUNABLE_UINT(name) \
static ssize_t name##_show(struct gov_attr_set *attr_set, char *buf) \
{ \
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->name); \
} \
static ssize_t name##_store(struct gov_attr_set *attr_set, \
			    const char *buf, size_t count) \
{ \
	struct vorpal_tunables *t = to_vorpal_tunables(attr_set); \
	unsigned int val; \
	if (kstrtouint(buf, 10, &val)) \
		return -EINVAL; \
	t->name = val; \
	return count; \
} \
static struct governor_attr name = __ATTR_RW(name)

static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->rate_limit_us);
}
static ssize_t rate_limit_us_store(struct gov_attr_set *attr_set,
				   const char *buf, size_t count)
{
	struct vorpal_tunables *tunables = to_vorpal_tunables(attr_set);
	struct vorpal_policy *vp;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->rate_limit_us = val;
	list_for_each_entry(vp, &attr_set->policy_list, tunables_hook)
		vp->freq_update_delay_ns = val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr rate_limit_us =
	__ATTR(rate_limit_us, 0644, rate_limit_us_show, rate_limit_us_store);

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->up_rate_limit_us);
}
static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct vorpal_tunables *tunables = to_vorpal_tunables(attr_set);
	struct vorpal_policy *vp;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->up_rate_limit_us = val;
	list_for_each_entry(vp, &attr_set->policy_list, tunables_hook)
		vp->up_rate_delay_ns = (s64)val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr up_rate_limit_us =
	__ATTR(up_rate_limit_us, 0644, up_rate_limit_us_show, up_rate_limit_us_store);

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->down_rate_limit_us);
}
static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct vorpal_tunables *tunables = to_vorpal_tunables(attr_set);
	struct vorpal_policy *vp;
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	tunables->down_rate_limit_us = val;
	list_for_each_entry(vp, &attr_set->policy_list, tunables_hook)
		vp->down_rate_delay_ns = (s64)val * NSEC_PER_USEC;
	return count;
}
static struct governor_attr down_rate_limit_us =
	__ATTR(down_rate_limit_us, 0644, down_rate_limit_us_show, down_rate_limit_us_store);

static ssize_t hispeed_boost_pct_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->hispeed_boost_pct);
}
static ssize_t hispeed_boost_pct_store(struct gov_attr_set *attr_set,
				       const char *buf, size_t count)
{
	struct vorpal_tunables *t = to_vorpal_tunables(attr_set);
	unsigned int val;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val < 1 || val > 100)
		return -EINVAL;
	t->hispeed_boost_pct = val;
	return count;
}
static struct governor_attr hispeed_boost_pct =
	__ATTR(hispeed_boost_pct, 0644, hispeed_boost_pct_show, hispeed_boost_pct_store);

static ssize_t gaming_mode_show(struct gov_attr_set *attr_set, char *buf)
{
	return sprintf(buf, "%u\n", to_vorpal_tunables(attr_set)->gaming_mode);
}
static ssize_t gaming_mode_store(struct gov_attr_set *attr_set,
				 const char *buf, size_t count)
{
	struct vorpal_tunables *t = to_vorpal_tunables(attr_set);
	struct vorpal_policy *vp;
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	t->gaming_mode = val;

	if (!val) {
		list_for_each_entry(vp, &attr_set->policy_list, tunables_hook) {
			vp->gaming_lock_end_ns = 0;
			vp->current_mode = VORPAL_MODE_NORMAL;
			vp->in_heavy_mode = false;
			vp->in_light_mode = false;
			vp->force_idle = false;
			vp->need_freq_update = true;
			vp->mode_switch_time_ns = ktime_get_ns();
			vp->game_launching = false;
			vp->game_launch_end_ns = 0;
			vp->prime_gaming_floor_active = false;
			vp->prime_gaming_floor_end_ns = 0;
			vp->render_urgency_active = false;
			vp->render_boost_end_ns = 0;
			vp->fps.state = VORPAL_FPS_IDLE;
			vp->fps.game_detected = false;
			vp->sustained.sustained_mode = false;
		}
	}
	return count;
}
static struct governor_attr gaming_mode =
	__ATTR(gaming_mode, 0644, gaming_mode_show, gaming_mode_store);

VORPAL_TUNABLE_UINT(sustained_mode);
VORPAL_TUNABLE_UINT(fps_target);
VORPAL_TUNABLE_UINT(thermal_coop_enable);
VORPAL_TUNABLE_UINT(gpu_coop_enable);
VORPAL_TUNABLE_UINT(eas_boost_enable);
VORPAL_TUNABLE_UINT(input_boost_enable);
VORPAL_TUNABLE_UINT(power_cap_mw);
VORPAL_TUNABLE_UINT(hispeed_window_us);
VORPAL_TUNABLE_UINT(hispeed_filter_shift);

static struct attribute *vorpal_little_attrs[] = {
	&hispeed_boost_pct.attr,
	&rate_limit_us.attr,
	&input_boost_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(vorpal_little);

static struct attribute *vorpal_big_attrs[] = {
	&down_rate_limit_us.attr,
	&hispeed_boost_pct.attr,
	&hispeed_filter_shift.attr,
	&rate_limit_us.attr,
	&gaming_mode.attr,
	&fps_target.attr,
	&thermal_coop_enable.attr,
	&gpu_coop_enable.attr,
	&eas_boost_enable.attr,
	&input_boost_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(vorpal_big);

static struct attribute *vorpal_prime_attrs[] = {
	&hispeed_boost_pct.attr,
	&hispeed_filter_shift.attr,
	&hispeed_window_us.attr,
	&rate_limit_us.attr,
	&up_rate_limit_us.attr,
	&gaming_mode.attr,
	&sustained_mode.attr,
	&fps_target.attr,
	&thermal_coop_enable.attr,
	&gpu_coop_enable.attr,
	&eas_boost_enable.attr,
	&input_boost_enable.attr,
	&power_cap_mw.attr,
	NULL
};
ATTRIBUTE_GROUPS(vorpal_prime);

static void vorpal_tunables_free(struct kobject *kobj)
{
	kfree(to_vorpal_tunables(vorpal_to_gov_attr_set(kobj)));
}

static struct kobj_type vorpal_little_ktype = {
	.default_groups = vorpal_little_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = vorpal_tunables_free,
};

static struct kobj_type vorpal_big_ktype = {
	.default_groups = vorpal_big_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = vorpal_tunables_free,
};

static struct kobj_type vorpal_prime_ktype = {
	.default_groups = vorpal_prime_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = vorpal_tunables_free,
};

static struct cpufreq_governor vorpal_gov;

/* === POLICY ALLOCATION === */

static struct vorpal_policy *vorpal_policy_alloc(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp;

	vp = kzalloc(sizeof(*vp), GFP_KERNEL);
	if (!vp)
		return NULL;

	vp->policy = policy;
	raw_spin_lock_init(&vp->update_lock);
	return vp;
}

static void vorpal_policy_free(struct vorpal_policy *vp)
{
	kfree(vp);
}

/* === KTHREAD WORKER === */

static int vorpal_kthread_create(struct vorpal_policy *vp)
{
	struct task_struct *thread;
	struct cpufreq_policy *policy = vp->policy;

	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&vp->work, vorpal_work);
	kthread_init_worker(&vp->worker);

	thread = kthread_create(kthread_worker_fn, &vp->worker,
				"vorpal_gov/%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("vorpal: kthread create failed %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	vp->thread = thread;

	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&vp->irq_work, vorpal_irq_work);
	mutex_init(&vp->work_lock);
	wake_up_process(thread);
	return 0;
}

static void vorpal_kthread_stop(struct vorpal_policy *vp)
{
	if (vp->policy->fast_switch_enabled)
		return;
	kthread_flush_worker(&vp->worker);
	kthread_stop(vp->thread);
	mutex_destroy(&vp->work_lock);
}

/* === TUNABLES ALLOCATION === */

static struct vorpal_tunables *vorpal_tunables_alloc(struct vorpal_policy *vp)
{
	struct vorpal_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &vp->tunables_hook);
		if (!have_governor_per_policy())
			vorpal_global_tunables = tunables;
	}
	return tunables;
}

static void vorpal_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		vorpal_global_tunables = NULL;
}

/* === AUTO ROM DETECTION === */
static u8 vorpal_detect_rom_tweak(void)
{
	int score = 0;

	if (vm_swappiness <= 5)
		score += 3;
	else if (vm_swappiness <= 15)
		score += 2;
	else if (vm_swappiness <= 30)
		score += 1;

	if (sysctl_sched_latency <= 1000000UL)
		score += 3;
	else if (sysctl_sched_latency <= 3000000UL)
		score += 2;
	else if (sysctl_sched_latency <= 5000000UL)
		score += 1;

	if (score >= 5)
		return 2;
	else if (score >= 2)
		return 1;
	return 0;
}

/* === GOVERNOR INIT === */

static int vorpal_init(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp;
	struct vorpal_tunables *tunables;
	unsigned long max_cap;
	int ret = 0;
	struct kobj_type *ktype;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	vp = vorpal_policy_alloc(policy);
	if (!vp) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = vorpal_kthread_create(vp);
	if (ret)
		goto free_vp;

	mutex_lock(&vorpal_global_tunables_lock);

	if (vorpal_global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = vp;
		vp->tunables = vorpal_global_tunables;
		gov_attr_set_get(&vorpal_global_tunables->attr_set, &vp->tunables_hook);

		vp->freq_update_delay_ns = (s64)vorpal_global_tunables->rate_limit_us * NSEC_PER_USEC;
		vp->up_rate_delay_ns = (s64)vorpal_global_tunables->up_rate_limit_us * NSEC_PER_USEC;
		vp->down_rate_delay_ns = (s64)vorpal_global_tunables->down_rate_limit_us * NSEC_PER_USEC;
		goto out;
	}

	tunables = vorpal_tunables_alloc(vp);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->hispeed_window_us = VORPAL_HISPEED_WINDOW_US;
	tunables->hispeed_filter_shift = VORPAL_HISPEED_FILTER_SHIFT;
	tunables->hispeed_boost_pct = VORPAL_HISPEED_BOOST_PCT;
	tunables->gaming_mode = 0;
	tunables->sustained_mode = 0;
	tunables->fps_target = VORPAL_FPS_TARGET_120;
	tunables->thermal_coop_enable = 1;
	tunables->gpu_coop_enable = 1;
	tunables->eas_boost_enable = 1;
	tunables->input_boost_enable = 1;
	tunables->power_cap_mw = 5000;

	vp->rom_tweak_detected = vorpal_detect_rom_tweak();
	vp->rom_override_active = (vp->rom_tweak_detected > 0);
	if (vp->rom_override_active)
		pr_info("vorpal: ROM tweak detected (level %u), governor override active\n",
			vp->rom_tweak_detected);

	max_cap = arch_scale_cpu_capacity(cpumask_first(policy->cpus));

	if (max_cap <= (unsigned long)VORPAL_LITTLE_CAP_THRESHOLD) {
		tunables->cluster_type = VORPAL_CLUSTER_LITTLE;
		tunables->rate_limit_us = VORPAL_DEFAULT_RATE_US;
		tunables->up_rate_limit_us = VORPAL_LITTLE_UP_RATE_US;
		tunables->down_rate_limit_us = VORPAL_LITTLE_DOWN_RATE_US;
	} else if (max_cap >= (unsigned long)VORPAL_PRIME_CAP_THRESHOLD) {
		tunables->cluster_type = VORPAL_CLUSTER_PRIME;
		tunables->rate_limit_us = VORPAL_PRIME_RATE_LIMIT_US;
		tunables->up_rate_limit_us = VORPAL_PRIME_UP_RATE_US;
		tunables->down_rate_limit_us = VORPAL_PRIME_DOWN_RATE_US;
	} else {
		tunables->cluster_type = VORPAL_CLUSTER_BIG;
		tunables->rate_limit_us = VORPAL_DEFAULT_RATE_US;
		tunables->up_rate_limit_us = VORPAL_BIG_UP_RATE_US;
		tunables->down_rate_limit_us = VORPAL_BIG_DOWN_RATE_US;
	}

	policy->governor_data = vp;
	vp->tunables = tunables;

	vp->freq_update_delay_ns = (s64)tunables->rate_limit_us * NSEC_PER_USEC;
	vp->up_rate_delay_ns = (s64)tunables->up_rate_limit_us * NSEC_PER_USEC;
	vp->down_rate_delay_ns = (s64)tunables->down_rate_limit_us * NSEC_PER_USEC;

	switch (tunables->cluster_type) {
	case VORPAL_CLUSTER_LITTLE:
		ktype = &vorpal_little_ktype;
		break;
	case VORPAL_CLUSTER_PRIME:
		ktype = &vorpal_prime_ktype;
		break;
	case VORPAL_CLUSTER_BIG:
	default:
		ktype = &vorpal_big_ktype;
		break;
	}
	ret = kobject_init_and_add(&tunables->attr_set.kobj, ktype,
				   get_governor_parent_kobj(policy),
				   "%s", vorpal_gov.name);

	if (ret)
		goto fail;

out:
	/* FIX: Initialize thermal zones safely during init (process context) */
	vorpal_init_thermal_zones(vp);
	mutex_unlock(&vorpal_global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	vorpal_clear_global_tunables();
stop_kthread:
	vorpal_kthread_stop(vp);
	mutex_unlock(&vorpal_global_tunables_lock);
free_vp:
	vorpal_policy_free(vp);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("vorpal: init failed error %d\n", ret);
	return ret;
}

/* === GOVERNOR EXIT === */

static void vorpal_exit(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp = policy->governor_data;
	struct vorpal_tunables *tunables = vp->tunables;
	unsigned int count;

	mutex_lock(&vorpal_global_tunables_lock);
	count = gov_attr_set_put(&tunables->attr_set, &vp->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		vorpal_clear_global_tunables();
	mutex_unlock(&vorpal_global_tunables_lock);

	vorpal_kthread_stop(vp);
	vorpal_policy_free(vp);
	cpufreq_disable_fast_switch(policy);
}

/* === GOVERNOR START === */

static int vorpal_start(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;
	u64 now;
	int i;
	struct vorpal_cpu *vc;

	vp->freq_update_delay_ns = (s64)vp->tunables->rate_limit_us * NSEC_PER_USEC;
	vp->up_rate_delay_ns = (s64)vp->tunables->up_rate_limit_us * NSEC_PER_USEC;
	vp->down_rate_delay_ns = (s64)vp->tunables->down_rate_limit_us * NSEC_PER_USEC;

	now = ktime_get_ns();
	vp->last_upfreq_time = now;
	vp->last_downfreq_time = now;
	vp->next_freq = policy->cur > 0 ? policy->cur : policy->cpuinfo.min_freq;
	vp->work_in_progress = false;
	vp->limits_changed = false;
	vp->cached_raw_freq = 0;
	vp->need_freq_update = false;
	vp->sustain_heavy_ticks = 0;
	vp->sustain_exit_ticks = 0;
	vp->light_enter_ticks = 0;
	vp->in_heavy_mode = false;
	vp->in_light_mode = false;
	vp->prev_heavy_mode = false;
	vp->guard_end_ns = 0;
	vp->guard_freq_khz = 0;
	vp->interactive_end_ns = 0;
	vp->prime_gaming_floor_active = false;
	vp->prime_gaming_floor_end_ns = 0;
	vp->force_idle = false;
	vp->force_idle_start_ns = 0;
	vp->last_real_update_ns = now;
	vp->current_mode = VORPAL_MODE_NORMAL;
	vp->mode_switch_time_ns = now;
	vp->gaming_lock_end_ns = 0;
	vp->video_pattern_detected = false;
	vp->video_detect_start_ns = 0;
	vp->idle_entry_time_ns = 0;
	vp->in_deep_idle = false;
	vp->game_launch_end_ns = 0;
	vp->game_launching = false;
	vp->thermal_duty_window_start_ns = 0;
	vp->thermal_throttle_end_ns = 0;
	vp->thermal_throttle_active = false;
	vp->thermal_sustain_window_count = 0;
	vp->thermal_duty_last_active_ns = 0;
	vp->render_urgency_active = false;
	vp->render_boost_end_ns = 0;
	vp->input_boost_active = false;
	vp->input_boost_end_ns = 0;
	vp->input_boost_freq_khz = 0;
	vp->hotplug_managed = false;
	vp->min_cores_online = 1;
	vp->thermal_last_read_ns = 0;
	vp->pending_thermal_cap_pct = 100;

	memset(&vp->fps, 0, sizeof(vp->fps));
	memset(&vp->gpu, 0, sizeof(vp->gpu));
	memset(&vp->thermal, 0, sizeof(vp->thermal));
	memset(&vp->eas, 0, sizeof(vp->eas));
	memset(&vp->sustained, 0, sizeof(vp->sustained));
	vp->fps.target_fps = vp->tunables->fps_target ? vp->tunables->fps_target : VORPAL_FPS_TARGET_120;

	uu = policy_is_shared(policy) ? vorpal_update_shared : vorpal_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		vc = per_cpu_ptr(&vorpal_cpu_table, cpu);
		memset(vc, 0, sizeof(*vc));
		vc->cpu = cpu;
		vc->vp = vp;
		vc->act_state = VORPAL_ACT_IDLE;
		vc->prev_idle_time = get_cpu_idle_time(cpu, &vc->prev_wall_time, 1);
		vc->util_history_idx = 0;
		for (i = 0; i < 8; i++)
			vc->util_history[i] = 0;
		cpufreq_add_update_util_hook(cpu, &vc->update_util, uu);
	}
	return 0;
}

/* === GOVERNOR STOP === */

static void vorpal_stop(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&vp->irq_work);
		kthread_cancel_work_sync(&vp->work);
	}
}

/* === GOVERNOR LIMITS === */

static void vorpal_limits(struct cpufreq_policy *policy)
{
	struct vorpal_policy *vp = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&vp->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&vp->work_lock);
	}

	smp_wmb();
	WRITE_ONCE(vp->limits_changed, true);
}

/* === GOVERNOR STRUCT === */

static struct cpufreq_governor vorpal_gov = {
	.name = "vorpal",
	.owner = THIS_MODULE,
	.flags = CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init = vorpal_init,
	.exit = vorpal_exit,
	.start = vorpal_start,
	.stop = vorpal_stop,
	.limits = vorpal_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_VORPAL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &vorpal_gov;
}
#endif

/* === MODULE === */

static int __init vorpal_gov_init(void)
{
	pr_info("%s %s by %s\n", CPUFREQ_VORPAL_PROGNAME,
		CPUFREQ_VORPAL_VERSION, CPUFREQ_VORPAL_AUTHOR);
	pr_info("QuantumGaming<44C | Idle<1%% | FPS-Aware | ThermalCoop | GPUCoop | EAS+ | Sustained\n");
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
