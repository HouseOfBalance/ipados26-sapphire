/*
 * thermald: Qualcomm Thermal & Deep-Sleep Manager for iPadOS 26.1 on Sapphire (SM6225Pro / Snapdragon 685)
 *
 * Reads thermal sensors via XNU sysctls and Qualcomm PMIC (PM6125 / PM7250B via kern.sweet_spmi):
 *   - Zone 0 (SoC):     kern.sweet_tsens (TSENS0 v2.3 max silicon die) / PM6125 QPNP_TM (0x2408)
 *   - Zone 1 (GPU):     kern.sweet_thermal_zone1 / sysfs GPU fallback
 *   - Zone 2 (Battery): PM7250B QG SDAM (0xb148) + JEITA status (0x100d) / kern.sweet_thermal_zone2
 *   - Zone 3 (Skin):    PM7250B QPNP_TM (0x2408) / kern.sweet_thermal_zone3
 *   (With clean sysfs fallback if /sys/class/thermal is ever mounted in a test harness.)
 *
 * Glitch & Spike Filtering:
 *   - Rejects physically impossible ADC glitches (<= -40.0 °C or >= 135.0 °C; battery > 85.0 °C).
 *   - Per-zone 3-sample median filter suppresses single-sample TSENS0 / SPMI conversion spikes
 *     (including > 15.0 °C/poll slew anomalies and boot-time hot-sample verification).
 *   - Asymmetric filter: fast peak-tracking attack on verified thermal rises (debounced by
 *     state hysteresis), 3-sample median + smooth EMA decay on cool-down (alpha = 0.50).
 *
 * 4-Tier Asymmetric Hysteresis State Machine (Nominal=0, Fair=1, Serious=2, Critical=3):
 *   - Nominal  (0): < 40.0 °C                | Full user ceiling (up to 2.80 GHz CPU, 1113.6 MHz GPU)
 *   - Fair     (1): enter >= 40.0 °C (2x)    | Exit <= 36.0 °C (3x) | Cap 2.40 GHz CPU, 900 MHz GPU, <=90 Hz
 *   - Serious  (2): enter >= 45.0 °C (2x)    | Exit <= 41.0 °C (3x) | Cap 1.90 GHz CPU, 610 MHz GPU, 60 Hz, boost off
 *   - Critical (3): enter >= 50.0 °C (1x!)   | Exit <= 46.0 °C (3x) | Cap  400 MHz CPU, 310 MHz GPU, 60 Hz, boost off
 *   Stepping down requires 4.0 °C deadband (HYSTERESIS_MC = 4000) held for 3 consecutive polls (9 s)
 *   and steps down strictly one tier at a time (3 -> 2 -> 1 -> 0).
 *
 * Publishes Darwin OS thermal notifications (com.apple.system.thermalpressurelevel & OSThermalNotification)
 * and preserves manual Eco / Balance / Max sysctl overrides (kern.sweet_cpufreq, kern.sweet_thermal_policy).
 */

#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<libkern/OSThermalNotification.h>)
#include <libkern/OSThermalNotification.h>
#endif
#endif

#ifndef kOSThermalNotificationPressureLevelName
#define kOSThermalNotificationPressureLevelName "com.apple.system.thermalpressurelevel"
#define SWEET_FALLBACK_PRESSURE_LEVELS 1
#endif

#define SWEET_OSTHERMAL_NOTIFY_NAME "OSThermalNotification"

#define LOG_PATH                "/var/tmp/thermald.log"
#define LOG_MAX                 (128 * 1024)
#define THERMAL_BASE            "/sys/class/thermal"
#define PMIC_TEMP_PATH          "/sys/class/power_supply/battery/temp"

#define ZONE_SOC                0
#define ZONE_GPU                1
#define ZONE_BATTERY            2
#define ZONE_SKIN               3
#define ZONE_COUNT              4

#define POLL_INTERVAL_SEC       3ull

/* Physical plausibility bounds in millicelsius (reject TSENS0 / NTC ADC glitches) */
#define TEMP_MIN_VALID_MC       (-40000)        /* <= -40.0 °C: ADC underflow / disconnect */
#define TEMP_MAX_VALID_MC       135000          /* >= 135.0 °C: ADC rail / conversion latch */
#define TEMP_MIN_BATTERY_MC     (-20000)        /* <= -20.0 °C: open battery NTC */
#define TEMP_MAX_BATTERY_MC     85000           /* >   85.0 °C: shorted battery NTC */

/* Per-zone 3-sample median + slew-spike rejection parameters */
#define MEDIAN_WINDOW           3
#define SLEW_SPIKE_MC           15000           /* > 15.0 °C jump in one 3 s poll -> median gate */
#define MAX_STALE_POLLS         5               /* Hold last valid reading up to 5 failed polls */

/* Thermal entry thresholds & asymmetric cool-down hysteresis (millicelsius) */
#define TRIP_FAIR_ENTER         40000           /* 40.0 °C: Nominal -> Fair */
#define TRIP_SERIOUS_ENTER      45000           /* 45.0 °C: Fair    -> Serious */
#define TRIP_CRITICAL_ENTER     50000           /* 50.0 °C: Serious -> Critical (immediate!) */

#define TRIP_NORMAL_MAX         TRIP_FAIR_ENTER
#define TRIP_WARM_MAX           TRIP_SERIOUS_ENTER
#define TRIP_HOT_MAX            TRIP_CRITICAL_ENTER

#define HYSTERESIS_MC           4000            /* 4.0 °C cool-down deadband per tier */
#define TRIP_FAIR_EXIT          (TRIP_FAIR_ENTER     - HYSTERESIS_MC)   /* 36.0 °C */
#define TRIP_SERIOUS_EXIT       (TRIP_SERIOUS_ENTER  - HYSTERESIS_MC)   /* 41.0 °C */
#define TRIP_CRITICAL_EXIT      (TRIP_CRITICAL_ENTER - HYSTERESIS_MC)   /* 46.0 °C */

/* Asymmetric consecutive-sample debounce / dwell counts */
#define UP_CONSECUTIVE_SAMPLES  2               /* 2 hot polls (6 s) for Fair/Serious; 1 for Critical */
#define DOWN_DWELL_SAMPLES      3               /* 3 cool polls (9 s) below exit threshold per tier */

/* Default hardware frequency ceilings (SM6225Pro Gold 2.80 GHz, Adreno 610 1113.6 MHz) */
#define CPU_FREQ_FLOOR_KHZ      300000
#define CPU_FREQ_CRIT_KHZ       400000
#define CPU_FREQ_SERIOUS_KHZ    1900000
#define CPU_FREQ_FAIR_KHZ       2400000
#define CPU_FREQ_MAX_KHZ        2803200

#define GPU_FREQ_CRIT_HZ        310000000
#define GPU_FREQ_SERIOUS_HZ     610000000
#define GPU_FREQ_FAIR_HZ        900000000
#define GPU_FREQ_MAX_HZ         1113600000

enum {
	THERMAL_NOMINAL  = 0,
	THERMAL_FAIR     = 1,
	THERMAL_SERIOUS  = 2,
	THERMAL_CRITICAL = 3,

	THERMAL_NORMAL   = THERMAL_NOMINAL,
	THERMAL_WARM     = THERMAL_FAIR,
	THERMAL_HOT      = THERMAL_SERIOUS
};

/* Policy bitmask (kern.sweet_thermal_policy) */
#define POLICY_A73_LIMIT_2400   (1u << 0)
#define POLICY_A73_LIMIT_1900   (1u << 1)
#define POLICY_A73_MIN_FREQ     (1u << 2)
#define POLICY_GPU_LIMIT_900    (1u << 3)
#define POLICY_GPU_LIMIT_610    (1u << 4)
#define POLICY_GPU_MIN_FREQ     (1u << 5)
#define POLICY_DISABLE_BOOST    (1u << 6)
#define POLICY_DEEP_SLEEP       (1u << 7)
#define POLICY_MANUAL_LOCK      (1u << 8)       /* Latched manual mitigation bits active */
#define POLICY_MANUAL_MAX       (1u << 9)       /* Manual Max override (unthrottled below Critical) */
#define POLICY_MITIGATION_MASK  0x7fu

#define N_BLANKED "com.apple.springboard.hasBlankedScreen"

enum zone_source {
	SRC_NONE = 0,
	SRC_SYSCTL,
	SRC_PMIC_DIE,
	SRC_PMIC_BAT,
	SRC_SYSFS
};

struct zone_filter {
	int      hist[MEDIAN_WINDOW];
	int      hist_idx;
	int      ema_mc;
	int      last_raw_mc;
	int      stale_polls;
	unsigned glitch_count;
	bool     seeded;
};

static enum zone_source zone_src[ZONE_COUNT];
static char zone_path[ZONE_COUNT][128];
static unsigned zone_sid[ZONE_COUNT];
static int zone_temp[ZONE_COUNT] = { -1, -1, -1, -1 };
static struct zone_filter zone_flt[ZONE_COUNT];
static int zone_count = 0;

static int therm_state = THERMAL_NOMINAL;
static unsigned therm_policy = 0;
static unsigned manual_policy_override = 0;
static bool manual_force_max = false;
static int hot_count_fair = 0;
static int hot_count_serious = 0;
static int cool_streak = 0;

static int deep_sleep_active = 0;
static bool initialized_eval = false;
static int blank_token = -1;
static uint64_t last_blank_state = 0;

static int pressure_notify_token = -1;
static int ostherm_notify_token = -1;
static int last_notified_state = -1;

/* Saved user/kernel baseline settings restored in Nominal / wake (tracks Eco/Balance/Max) */
static int saved_sched = 5;
static int saved_fb_hz = 120;
static int saved_cpu_max_freq = CPU_FREQ_MAX_KHZ;
static int saved_gpu_max_freq = GPU_FREQ_MAX_HZ;
static unsigned saved_panel_sleep = 1;
static bool have_saved_sched = false;
static bool have_saved_fb_hz = false;
static bool have_saved_cpu_max = false;
static bool have_saved_gpu_max = false;
static bool have_saved_panel_sleep = false;
static bool has_dsi_sysctl = false;

/* Last values written by thermald (used to detect external user/toolsd overrides) */
static int last_applied_sched = -1;
static int last_applied_fb_hz = -1;
static int last_applied_cpu_max = -1;
static int last_applied_gpu_max = -1;
static unsigned last_applied_policy = 0;
static bool have_applied_policy = false;

static dispatch_queue_t serial_queue;
static dispatch_source_t poll_timer;

static void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
logf(const char *fmt, ...)
{
	char msg[512], line[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	struct stat st;
	if (lstat(LOG_PATH, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > LOG_MAX) {
		(void)rename(LOG_PATH, LOG_PATH ".1");
	}
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s\n", msg);
		return;
	}

	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	int n = snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d %s\n",
	    tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
	if (n > 0) {
		size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
		(void)write(fd, line, len);
	}
	close(fd);
}

static bool
sysctl_exists(const char *name)
{
	int mib[CTL_MAXNAME];
	size_t n = CTL_MAXNAME;
	return sysctlnametomib(name, mib, &n) == 0;
}

static int
sysctl_get_int(const char *name, int *out)
{
	int v = 0;
	size_t len = sizeof(v);
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0) {
		return -1;
	}
	*out = v;
	return 0;
}

static int
sysctl_get_uint(const char *name, unsigned *out)
{
	unsigned v = 0;
	size_t len = sizeof(v);
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0) {
		return -1;
	}
	*out = v;
	return 0;
}

static int
sysctl_set_int(const char *name, int val)
{
	return sysctlbyname(name, NULL, NULL, &val, sizeof(val));
}

static int
sysctl_set_uint(const char *name, unsigned val)
{
	return sysctlbyname(name, NULL, NULL, &val, sizeof(val));
}

static int
min_int(int a, int b)
{
	return a < b ? a : b;
}

static int
median3(int a, int b, int c)
{
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	if (b > c) {
		int t = b;
		b = c;
		c = t;
	}
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	return b;
}

static bool
is_valid_raw_temp_mc(int zone, int mc)
{
	if (mc <= TEMP_MIN_VALID_MC || mc >= TEMP_MAX_VALID_MC) {
		return false;
	}
	if (zone == ZONE_BATTERY && (mc <= TEMP_MIN_BATTERY_MC || mc > TEMP_MAX_BATTERY_MC)) {
		return false;
	}
	return true;
}

static int
pmic_read(unsigned sid, unsigned addr, uint8_t *out)
{
	uint64_t req = (1ull << 56) | ((uint64_t)(sid & 0xffu) << 48) | ((uint64_t)(addr & 0xffffu) << 32);
	if (sysctlbyname("kern.sweet_spmi", NULL, NULL, &req, sizeof(req)) != 0) {
		return -1;
	}
	uint64_t ans = 0x100;
	size_t len = sizeof(ans);
	if (sysctlbyname("kern.sweet_spmi", &ans, &len, NULL, 0) != 0 || ans > 0xffu) {
		return -1;
	}
	*out = (uint8_t)ans;
	return 0;
}

/* Read PMIC QPNP_TM (TEMP_ALARM peripheral 0x2400): stage 0..3 -> conservative millicelsius */
static int
read_pmic_die_mc(unsigned sid)
{
	uint8_t ptype = 0, st = 0;
	if (pmic_read(sid, 0x2404, &ptype) != 0 || ptype != 0x09) {
		return -1;
	}
	if (pmic_read(sid, 0x2408, &st) != 0) {
		return -1;
	}
	switch (st & 0x03u) {
	case 3:  return 55000; /* Hardware over-temp stage 3 */
	case 2:  return 48000; /* Hardware over-temp stage 2 */
	case 1:  return 42000; /* Hardware warning stage 1 */
	default: return 34000; /* Normal operating range */
	}
}

/* Read PM7250B battery temperature (JEITA live band at 0x100d + QG SDAM 0xb148 when valid) */
static int
read_pmic_battery_mc(unsigned sid)
{
	int mc = -1;
	uint8_t valid = 0, lo = 0, hi = 0;
	if (pmic_read(sid, 0xb146, &valid) == 0 && valid == 1 &&
	    pmic_read(sid, 0xb148, &lo) == 0 && pmic_read(sid, 0xb149, &hi) == 0) {
		int16_t deci_c = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
		if (deci_c > -200 && deci_c <= 850) {
			mc = (int)deci_c * 100;
		}
	}
	uint8_t jeita = 0;
	if (pmic_read(sid, 0x100d, &jeita) == 0) {
		int jeita_mc = 32000;
		if (jeita & (1u << 3)) {
			jeita_mc = 52000;       /* JEITA too-hot */
		} else if (jeita & (1u << 5)) {
			jeita_mc = 43000;       /* JEITA warm */
		}
		if (mc < 0 || jeita_mc > mc) {
			mc = jeita_mc;
		}
	}
	return mc;
}

static int
read_temp_sysfs(const char *path)
{
	char buf[32];
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return -1;
	}
	buf[n] = '\0';
	char *end = NULL;
	errno = 0;
	long val = strtol(buf, &end, 10);
	if (end == buf || errno != 0 || val < -1000000L || val > 1000000L) {
		return -1;
	}
	/* PMIC power_supply battery/temp reports 0.1 °C (e.g. 325 = 32.5 °C) */
	if (strstr(path, "power_supply") != NULL) {
		if (val > -200 && val <= 850) {
			val *= 100;
		}
	} else if (val > 0 && val < 150) {
		/* Coarse integer °C sysfs thermal zones */
		val *= 1000;
	}
	return (int)val;
}

static int
read_zone_temp_raw(int i, bool *had_hw_value)
{
	int v = 0;
	if (had_hw_value != NULL) {
		*had_hw_value = false;
	}
	switch (zone_src[i]) {
	case SRC_SYSCTL:
		/* kern.sweet_tsens & kern.sweet_thermal_zone0..3 report exact millicelsius */
		if (sysctl_get_int(zone_path[i], &v) == 0) {
			if (had_hw_value != NULL) {
				*had_hw_value = true;
			}
			return v;
		}
		return -1;
	case SRC_PMIC_DIE:
		v = read_pmic_die_mc(zone_sid[i]);
		if (v >= 0 && had_hw_value != NULL) {
			*had_hw_value = true;
		}
		return v;
	case SRC_PMIC_BAT:
		v = read_pmic_battery_mc(zone_sid[i]);
		if (v >= 0 && had_hw_value != NULL) {
			*had_hw_value = true;
		}
		return v;
	case SRC_SYSFS:
		v = read_temp_sysfs(zone_path[i]);
		if (v != -1 && had_hw_value != NULL) {
			*had_hw_value = true;
		}
		return v;
	default:
		return -1;
	}
}

/*
 * Confirm an elevated or spike-like TSENS0 hardware reading with two 2 ms re-reads
 * (~1 ms TSENS0 conversion cycle) so a single ADC glitch never trips Critical, while
 * genuine silicon heat is confirmed within 4 ms on the very same poll.
 */
static int
confirm_tsens_reading(int i, int r0)
{
	if (zone_src[i] != SRC_SYSCTL || strcmp(zone_path[i], "kern.sweet_tsens") != 0) {
		return r0;
	}
	int r1 = r0, r2 = r0;
	bool ok1 = false, ok2 = false;
	usleep(2000);
	int tmp = read_zone_temp_raw(i, &ok1);
	if (ok1 && is_valid_raw_temp_mc(i, tmp)) {
		r1 = tmp;
	}
	usleep(2000);
	tmp = read_zone_temp_raw(i, &ok2);
	if (ok2 && is_valid_raw_temp_mc(i, tmp)) {
		r2 = tmp;
	}
	return median3(r0, r1, r2);
}

/*
 * Sample zone i with:
 *   1. Physical range validation (rejects <= -40 °C / >= 135 °C TSENS0 glitches)
 *   2. 3-sample median spike filter (gates > 15 °C single-poll jumps & cooling dips)
 *   3. Asymmetric tracking: zero-lag attack on verified rises, smooth 0.50 EMA decay on cooling
 */
static int
sample_zone_filtered(int i)
{
	if (i < 0 || i >= ZONE_COUNT || zone_src[i] == SRC_NONE) {
		return -1;
	}
	struct zone_filter *zf = &zone_flt[i];
	bool had_val = false;
	int raw = read_zone_temp_raw(i, &had_val);

	if (!had_val || !is_valid_raw_temp_mc(i, raw)) {
		if (had_val) {
			zf->glitch_count++;
			if ((zf->glitch_count & (zf->glitch_count - 1u)) == 0u) {
				logf("Rejected out-of-range thermal glitch on zone %d (%s): %d m°C (count=%u)",
				    i, zone_path[i], raw, zf->glitch_count);
			}
		}
		if (zf->seeded && zf->stale_polls < MAX_STALE_POLLS) {
			zf->stale_polls++;
			return zf->ema_mc;
		}
		zf->seeded = false;
		return -1;
	}

	zf->stale_polls = 0;

	if (!zf->seeded) {
		if (raw >= TRIP_FAIR_ENTER) {
			raw = confirm_tsens_reading(i, raw);
		}
		for (int k = 0; k < MEDIAN_WINDOW; k++) {
			zf->hist[k] = raw;
		}
		zf->hist_idx = 0;
		zf->ema_mc = raw;
		zf->last_raw_mc = raw;
		zf->seeded = true;
		return zf->ema_mc;
	}

	if (raw >= TRIP_CRITICAL_ENTER && zf->last_raw_mc < TRIP_CRITICAL_ENTER) {
		raw = confirm_tsens_reading(i, raw);
	}

	int delta = raw - zf->last_raw_mc;
	zf->hist[zf->hist_idx] = raw;
	zf->hist_idx = (zf->hist_idx + 1) % MEDIAN_WINDOW;
	int med = median3(zf->hist[0], zf->hist[1], zf->hist[2]);

	int effective = raw;
	if (delta > SLEW_SPIKE_MC || delta < -SLEW_SPIKE_MC) {
		/*
		 * Single-poll jump > 15 °C: gate through 3-sample median.
		 * A 1-sample spike stays at the prior steady value; a real sustained jump
		 * passes median3 on the very next poll.
		 */
		effective = med;
		if (effective != raw) {
			zf->glitch_count++;
			if ((zf->glitch_count & (zf->glitch_count - 1u)) == 0u) {
				logf("Filtered thermal slew spike on zone %d: raw=%d m°C prev=%d m°C -> med=%d m°C",
				    i, raw, zf->last_raw_mc, med);
			}
		}
		zf->last_raw_mc = effective;
	} else {
		/* Normal physical slew: zero lag on heating, median-gated on cooling */
		effective = (raw >= zf->ema_mc) ? raw : med;
		zf->last_raw_mc = raw;
	}

	if (effective >= zf->ema_mc) {
		zf->ema_mc = effective;
	} else {
		zf->ema_mc -= ((zf->ema_mc - effective) + 1) / 2;
	}

	return zf->ema_mc;
}

static void
capture_baseline_settings(void)
{
	int v = 0;
	if (!have_saved_sched && sysctl_get_int("kern.sweet_sched", &v) == 0) {
		saved_sched = v > 0 ? v : 5;
		have_saved_sched = true;
	}
	if (!have_saved_fb_hz && sysctl_get_int("kern.sweet_fb_hz", &v) == 0 && v >= 60) {
		saved_fb_hz = v;
		have_saved_fb_hz = true;
	}
	if (!have_saved_cpu_max && sysctl_get_int("kern.sweet_cpu_max_freq", &v) == 0 &&
	    v >= CPU_FREQ_FLOOR_KHZ && v <= CPU_FREQ_MAX_KHZ) {
		saved_cpu_max_freq = v;
		have_saved_cpu_max = true;
	}
	if (!have_saved_gpu_max && sysctl_get_int("kern.sweet_gpu_max_freq", &v) == 0 &&
	    v >= GPU_FREQ_CRIT_HZ && v <= GPU_FREQ_MAX_HZ) {
		saved_gpu_max_freq = v;
		have_saved_gpu_max = true;
	}
	if (!have_saved_panel_sleep && sysctl_get_int("kern.sweet_panel_sleep", &v) == 0 && v >= 0) {
		saved_panel_sleep = (unsigned)v;
		have_saved_panel_sleep = true;
	}
}

/*
 * Detect live user / SapphireTools overrides written to:
 *   - kern.sweet_thermal_policy (0 = auto; 0x01..0x7f = Eco/Balance bitmask; 0x200 = Max)
 *   - kern.sweet_cpufreq ("eco"=600MHz, "balance"=2.4GHz, "max"=2.8GHz) -> kern.sweet_cpu_max_freq
 *   - kern.sweet_gpu_max_freq, kern.sweet_fb_hz, kern.sweet_sched
 * Returns true if active hardware caps must be re-applied immediately.
 */
static bool
sync_external_overrides(void)
{
	bool changed = false;
	unsigned cur_pol = 0;

	if (sysctl_get_uint("kern.sweet_thermal_policy", &cur_pol) == 0) {
		if (!have_applied_policy) {
			/* Adopt any pre-existing manual policy set before thermald started */
			unsigned init_bits = cur_pol & POLICY_MITIGATION_MASK;
			bool init_max = (cur_pol & POLICY_MANUAL_MAX) != 0;
			if (init_bits != 0 || init_max) {
				manual_policy_override = init_bits;
				manual_force_max = init_max;
				logf("Adopted initial manual thermal policy override: %#x", cur_pol);
			}
		} else if (cur_pol != last_applied_policy) {
			if (cur_pol == 0 || cur_pol == POLICY_DEEP_SLEEP) {
				if (manual_policy_override != 0 || manual_force_max) {
					logf("Cleared manual thermal policy override (auto hysteresis active)");
				}
				manual_policy_override = 0;
				manual_force_max = false;
			} else if (cur_pol & POLICY_MANUAL_MAX) {
				manual_force_max = true;
				manual_policy_override = cur_pol & POLICY_MITIGATION_MASK;
				logf("Manual Max thermal policy override active (mask=%#x)", manual_policy_override);
			} else {
				manual_force_max = false;
				manual_policy_override = cur_pol & POLICY_MITIGATION_MASK;
				logf("Manual thermal policy override set: %#x", manual_policy_override);
			}
			changed = true;
		}
	}

	if (deep_sleep_active) {
		return changed;
	}

	int v = 0;
	if (last_applied_cpu_max > 0 && sysctl_get_int("kern.sweet_cpu_max_freq", &v) == 0 &&
	    v != last_applied_cpu_max && v >= CPU_FREQ_FLOOR_KHZ && v <= CPU_FREQ_MAX_KHZ) {
		saved_cpu_max_freq = v;
		have_saved_cpu_max = true;
		logf("Adopted user CPU max frequency baseline: %d kHz", saved_cpu_max_freq);
		changed = true;
	}
	if (last_applied_gpu_max > 0 && sysctl_get_int("kern.sweet_gpu_max_freq", &v) == 0 &&
	    v != last_applied_gpu_max && v >= GPU_FREQ_CRIT_HZ && v <= GPU_FREQ_MAX_HZ) {
		saved_gpu_max_freq = v;
		have_saved_gpu_max = true;
		logf("Adopted user GPU max frequency baseline: %d Hz", saved_gpu_max_freq);
		changed = true;
	}
	if (last_applied_fb_hz > 0 && sysctl_get_int("kern.sweet_fb_hz", &v) == 0 &&
	    v != last_applied_fb_hz && v >= 60) {
		saved_fb_hz = v;
		have_saved_fb_hz = true;
		logf("Adopted user display refresh baseline: %d Hz", saved_fb_hz);
		changed = true;
	}
	if (last_applied_sched > 0 && sysctl_get_int("kern.sweet_sched", &v) == 0 &&
	    v != last_applied_sched && v > 0) {
		saved_sched = v;
		have_saved_sched = true;
		logf("Adopted user scheduler baseline: %d", saved_sched);
		changed = true;
	}

	return changed;
}

static uint64_t
thermal_state_to_pressure_level(int state)
{
#ifdef SWEET_FALLBACK_PRESSURE_LEVELS
	switch (state) {
	case THERMAL_CRITICAL: return 3;
	case THERMAL_SERIOUS:  return 2;
	case THERMAL_FAIR:     return 1;
	default:               return 0;
	}
#else
	switch (state) {
	case THERMAL_CRITICAL: return (uint64_t)kOSThermalPressureLevelTrapping;
	case THERMAL_SERIOUS:  return (uint64_t)kOSThermalPressureLevelHeavy;
	case THERMAL_FAIR:     return (uint64_t)kOSThermalPressureLevelModerate;
	default:               return (uint64_t)kOSThermalPressureLevelNominal;
	}
#endif
}

static void
publish_os_thermal_notification(int state)
{
	if (state == last_notified_state) {
		return;
	}
	if (pressure_notify_token < 0) {
		if (notify_register_check(kOSThermalNotificationPressureLevelName,
		    &pressure_notify_token) != NOTIFY_STATUS_OK) {
			pressure_notify_token = -1;
		}
	}
	if (pressure_notify_token >= 0) {
		uint64_t level = thermal_state_to_pressure_level(state);
		(void)notify_set_state(pressure_notify_token, level);
		(void)notify_post(kOSThermalNotificationPressureLevelName);
	}

	if (ostherm_notify_token < 0) {
		if (notify_register_check(SWEET_OSTHERMAL_NOTIFY_NAME,
		    &ostherm_notify_token) != NOTIFY_STATUS_OK) {
			ostherm_notify_token = -1;
		}
	}
	if (ostherm_notify_token >= 0) {
		(void)notify_set_state(ostherm_notify_token, (uint64_t)state);
		(void)notify_post(SWEET_OSTHERMAL_NOTIFY_NAME);
	}

	last_notified_state = state;
}

static void
update_sysctls(void)
{
	for (int i = 0; i < zone_count; i++) {
		if (zone_temp[i] >= 0) {
			char sysctl_name[64];
			snprintf(sysctl_name, sizeof(sysctl_name), "kern.sweet_thermal_zone%d", i);
			(void)sysctl_set_int(sysctl_name, zone_temp[i]);
		}
	}
	(void)sysctl_set_int("kern.sweet_thermal_state", therm_state);
	if (sysctl_set_uint("kern.sweet_thermal_policy", therm_policy) == 0) {
		unsigned rb = therm_policy;
		if (sysctl_get_uint("kern.sweet_thermal_policy", &rb) == 0) {
			last_applied_policy = rb;
		} else {
			last_applied_policy = therm_policy;
		}
		have_applied_policy = true;
	}
	(void)sysctl_set_int("kern.sweet_deep_sleep", deep_sleep_active);
}

static void
set_tracked_int_sysctl(const char *name, int val, int *tracked_out)
{
	if (sysctl_set_int(name, val) == 0) {
		int rb = val;
		if (sysctl_get_int(name, &rb) == 0) {
			*tracked_out = rb;
		} else {
			*tracked_out = val;
		}
	}
}

static void
apply_cpu_policy(void)
{
	int base_cpu = have_saved_cpu_max ? saved_cpu_max_freq : CPU_FREQ_MAX_KHZ;
	int target_sched = saved_sched;
	int target_cpu = base_cpu;

	if (therm_policy & POLICY_A73_MIN_FREQ) {
		target_sched = 3; /* UT + BG + DF prefer E-cluster (A53) */
		target_cpu = min_int(base_cpu, CPU_FREQ_CRIT_KHZ);
	} else if (therm_policy & POLICY_A73_LIMIT_1900) {
		target_sched = 3; /* offload Default QoS onto A53s */
		target_cpu = min_int(base_cpu, CPU_FREQ_SERIOUS_KHZ);
	} else if (therm_policy & POLICY_A73_LIMIT_2400) {
		target_sched = saved_sched;
		target_cpu = min_int(base_cpu, CPU_FREQ_FAIR_KHZ);
	}

	set_tracked_int_sysctl("kern.sweet_sched", target_sched, &last_applied_sched);
	if (sysctl_exists("kern.sweet_cpu_max_freq")) {
		set_tracked_int_sysctl("kern.sweet_cpu_max_freq", target_cpu, &last_applied_cpu_max);
	} else if (target_cpu >= CPU_FREQ_MAX_KHZ) {
		/* Legacy patch-0328 fallback when kern.sweet_cpu_max_freq is absent */
		char max_cmd[] = "max";
		(void)sysctlbyname("kern.sweet_cpufreq", NULL, NULL, max_cmd, sizeof(max_cmd));
	}
}

static void
apply_gpu_policy(void)
{
	int base_gpu = have_saved_gpu_max ? saved_gpu_max_freq : GPU_FREQ_MAX_HZ;
	int target_gpu = base_gpu;
	int target_hz = saved_fb_hz;

	if (therm_policy & POLICY_GPU_MIN_FREQ) {
		target_hz = 60;
		target_gpu = min_int(base_gpu, GPU_FREQ_CRIT_HZ);
	} else if (therm_policy & POLICY_GPU_LIMIT_610) {
		target_hz = 60;
		target_gpu = min_int(base_gpu, GPU_FREQ_SERIOUS_HZ);
	} else if (therm_policy & POLICY_GPU_LIMIT_900) {
		target_hz = saved_fb_hz > 90 ? 90 : saved_fb_hz;
		target_gpu = min_int(base_gpu, GPU_FREQ_FAIR_HZ);
	}

	if (have_saved_fb_hz || (therm_policy & (POLICY_GPU_MIN_FREQ | POLICY_GPU_LIMIT_610))) {
		set_tracked_int_sysctl("kern.sweet_fb_hz", target_hz, &last_applied_fb_hz);
	}
	set_tracked_int_sysctl("kern.sweet_gpu_max_freq", target_gpu, &last_applied_gpu_max);
}

static void
apply_boost_policy(void)
{
	int en = (therm_policy & POLICY_DISABLE_BOOST) ? 0 : 1;
	(void)sysctl_set_int("kern.sweet_touch_boost_enabled", en);
	(void)sysctl_set_int("kern.sweet_launch_boost_enabled", en);
}

static int
send_dsi_cmd(uint8_t cmd, const uint8_t *params, int param_count)
{
	if (!has_dsi_sysctl || param_count < 0 || param_count > 30) {
		return -1;
	}
	uint8_t payload[32];
	payload[0] = cmd;
	payload[1] = (uint8_t)param_count;
	if (param_count > 0 && params != NULL) {
		memcpy(payload + 2, params, (size_t)param_count);
	}
	return sysctlbyname("kern.sweet_dsi_cmd", NULL, NULL, payload, (size_t)(2 + param_count));
}

/* Must run on serial_queue */
static void
enter_deep_sleep(void)
{
	if (deep_sleep_active) {
		return;
	}
	capture_baseline_settings();
	(void)sync_external_overrides();
	logf("Entering deep sleep: panel idle unstage + E-cluster parking");

	/* Enable MDP border-color unstage + idle vsync (patch 0362) */
	(void)sysctl_set_uint("kern.sweet_panel_sleep", 2);

	/* Optional raw DSI commands on kernels exposing kern.sweet_dsi_cmd */
	(void)send_dsi_cmd(0x28, NULL, 0);
	(void)send_dsi_cmd(0x10, NULL, 0);

	/* Offload movable work to efficiency cluster while screen is off */
	set_tracked_int_sysctl("kern.sweet_sched", 3, &last_applied_sched);
	set_tracked_int_sysctl("kern.sweet_cpu_max_freq", CPU_FREQ_CRIT_KHZ, &last_applied_cpu_max);
	set_tracked_int_sysctl("kern.sweet_gpu_max_freq", GPU_FREQ_CRIT_HZ, &last_applied_gpu_max);
	(void)sysctl_set_int("kern.sweet_touch_boost_enabled", 0);
	(void)sysctl_set_int("kern.sweet_launch_boost_enabled", 0);

	deep_sleep_active = 1;
	therm_policy |= POLICY_DEEP_SLEEP;
	update_sysctls();
}

/* Must run on serial_queue */
static void
exit_deep_sleep(void)
{
	if (!deep_sleep_active) {
		return;
	}
	logf("Exiting deep sleep: restoring panel & thermal policy");

	if (send_dsi_cmd(0x11, NULL, 0) == 0) {
		usleep(120000); /* 120ms per MIPI DSI spec when Sleep Out was sent */
		(void)send_dsi_cmd(0x29, NULL, 0);
	}
	(void)sysctl_set_uint("kern.sweet_panel_sleep", have_saved_panel_sleep ? saved_panel_sleep : 1u);

	deep_sleep_active = 0;
	therm_policy &= ~POLICY_DEEP_SLEEP;

	apply_cpu_policy();
	apply_gpu_policy();
	apply_boost_policy();
	update_sysctls();
}

/* Must run on serial_queue */
static void
sync_blank_state(int token)
{
	if (token < 0) {
		return;
	}
	uint64_t state = 0;
	if (notify_get_state(token, &state) != NOTIFY_STATUS_OK) {
		return;
	}
	if (state != last_blank_state) {
		last_blank_state = state;
		logf("Screen blank state changed: %llu", (unsigned long long)state);
	}
	if (state == 1) {
		enter_deep_sleep();
	} else {
		exit_deep_sleep();
	}
}

static unsigned
policy_for_thermal_state(int state)
{
	switch (state) {
	case THERMAL_CRITICAL:
		return POLICY_A73_MIN_FREQ | POLICY_GPU_MIN_FREQ | POLICY_DISABLE_BOOST;
	case THERMAL_SERIOUS:
		return POLICY_A73_LIMIT_1900 | POLICY_GPU_LIMIT_610 | POLICY_DISABLE_BOOST;
	case THERMAL_FAIR:
		return POLICY_A73_LIMIT_2400 | POLICY_GPU_LIMIT_900;
	default:
		return 0;
	}
}

static int
exit_threshold_for_state(int state)
{
	switch (state) {
	case THERMAL_CRITICAL: return TRIP_CRITICAL_EXIT;
	case THERMAL_SERIOUS:  return TRIP_SERIOUS_EXIT;
	case THERMAL_FAIR:     return TRIP_FAIR_EXIT;
	default:               return TEMP_MIN_VALID_MC;
	}
}

/* Must run on serial_queue */
static void
evaluate_thermal_state(void)
{
	bool override_changed = sync_external_overrides();
	int max_temp = -1;
	int hottest_zone = -1;

	for (int i = 0; i < zone_count; i++) {
		int temp = sample_zone_filtered(i);
		zone_temp[i] = temp;
		if (temp > max_temp) {
			max_temp = temp;
			hottest_zone = i;
		}
	}

	if (max_temp < 0) {
		if (override_changed) {
			unsigned eff = manual_policy_override;
			if (manual_policy_override != 0) {
				eff |= POLICY_MANUAL_LOCK;
			}
			if (manual_force_max) {
				eff |= POLICY_MANUAL_MAX;
			}
			if (deep_sleep_active) {
				eff |= POLICY_DEEP_SLEEP;
			}
			therm_policy = eff;
			if (!deep_sleep_active) {
				apply_cpu_policy();
				apply_gpu_policy();
				apply_boost_policy();
			}
			update_sysctls();
		}
		return;
	}

	/*
	 * Asymmetric hysteresis:
	 *   - Step UP: immediate (1 sample) for Critical (>= 50.0 °C);
	 *     UP_CONSECUTIVE_SAMPLES (2 polls = 6 s) for Fair (>= 40.0 °C) / Serious (>= 45.0 °C),
	 *     or 1 sample on initial daemon evaluation.
	 *   - Step DOWN: require max_temp <= (entry_threshold - 4.0 °C) for DOWN_DWELL_SAMPLES
	 *     (3 consecutive polls = 9 s), stepping down strictly one tier at a time.
	 */
	if (max_temp >= TRIP_FAIR_ENTER) {
		if (hot_count_fair < 1000) {
			hot_count_fair++;
		}
	} else {
		hot_count_fair = 0;
	}

	if (max_temp >= TRIP_SERIOUS_ENTER) {
		if (hot_count_serious < 1000) {
			hot_count_serious++;
		}
	} else {
		hot_count_serious = 0;
	}

	int req_up_samples = initialized_eval ? UP_CONSECUTIVE_SAMPLES : 1;
	int candidate_up = THERMAL_NOMINAL;
	if (max_temp >= TRIP_CRITICAL_ENTER) {
		candidate_up = THERMAL_CRITICAL;
	} else if (hot_count_serious >= req_up_samples) {
		candidate_up = THERMAL_SERIOUS;
	} else if (hot_count_fair >= req_up_samples) {
		candidate_up = THERMAL_FAIR;
	}

	int new_state = therm_state;
	if (!initialized_eval) {
		new_state = candidate_up;
		cool_streak = 0;
	} else if (candidate_up > therm_state) {
		new_state = candidate_up;
		cool_streak = 0;
	} else if (therm_state > THERMAL_NOMINAL) {
		int exit_mc = exit_threshold_for_state(therm_state);
		if (max_temp <= exit_mc) {
			cool_streak++;
			if (cool_streak >= DOWN_DWELL_SAMPLES) {
				new_state = therm_state - 1;
				cool_streak = 0;
			}
		} else {
			cool_streak = 0;
		}
	} else {
		cool_streak = 0;
	}

	/*
	 * Combine automatic hysteresis policy with manual overrides:
	 *   - Critical (>= 50 °C) ALWAYS enforces emergency silicon throttling, even under Manual Max.
	 *   - Manual mitigation bits (Eco / Balance via kern.sweet_thermal_policy) OR with auto_policy
	 *     so strictest cap wins without breaking background thermal hysteresis.
	 */
	unsigned auto_policy = policy_for_thermal_state(new_state);
	if (manual_force_max && new_state < THERMAL_CRITICAL) {
		auto_policy = 0;
	}
	unsigned combined_policy = auto_policy | manual_policy_override;
	if (manual_policy_override != 0) {
		combined_policy |= POLICY_MANUAL_LOCK;
	}
	if (manual_force_max) {
		combined_policy |= POLICY_MANUAL_MAX;
	}
	if (deep_sleep_active) {
		combined_policy |= POLICY_DEEP_SLEEP;
	}

	if (!initialized_eval || new_state != therm_state ||
	    combined_policy != therm_policy || override_changed) {
		if (!initialized_eval || (therm_state == THERMAL_NOMINAL && new_state != THERMAL_NOMINAL)) {
			capture_baseline_settings();
		}
		static const char *names[] = { "NOMINAL", "FAIR", "SERIOUS", "CRITICAL" };
		logf("THERMAL %s: zone %d at %d m°C (policy %#x, cpu_base=%d kHz)",
		    names[new_state], hottest_zone, max_temp, combined_policy, saved_cpu_max_freq);
		initialized_eval = true;
		therm_state = new_state;
		therm_policy = combined_policy;

		if (!deep_sleep_active) {
			apply_cpu_policy();
			apply_gpu_policy();
			apply_boost_policy();
		}
		publish_os_thermal_notification(therm_state);
	}
	update_sysctls();
}

static void
register_zone(int idx, enum zone_source src, const char *path_or_sysctl, unsigned sid, const char *desc)
{
	if (idx < 0 || idx >= ZONE_COUNT) {
		return;
	}
	zone_src[idx] = src;
	zone_sid[idx] = sid;
	memset(&zone_flt[idx], 0, sizeof(zone_flt[idx]));
	snprintf(zone_path[idx], sizeof(zone_path[idx]), "%s", path_or_sysctl ? path_or_sysctl : "");
	if (idx + 1 > zone_count) {
		zone_count = idx + 1;
	}
	logf("Configured thermal zone %d: %s", idx, desc);
}

static void
discover_thermal_zones(void)
{
	has_dsi_sysctl = sysctl_exists("kern.sweet_dsi_cmd");

	/*
	 * 1. Hardware Qualcomm TSENS0 silicon die sensor (kern.sweet_tsens).
	 * Must take precedence over kern.sweet_thermal_zone0..3, which thermald itself
	 * populates as RW telemetry (otherwise restarting thermald would loop on stale output).
	 */
	int tsens_mc = 0;
	if (sysctl_get_int("kern.sweet_tsens", &tsens_mc) == 0 &&
	    is_valid_raw_temp_mc(ZONE_SOC, tsens_mc)) {
		register_zone(ZONE_SOC, SRC_SYSCTL, "kern.sweet_tsens", 0, "kern.sweet_tsens (TSENS0)");
	}

	/* 2. Qualcomm PMIC SPMI hardware sensors via kern.sweet_spmi (patch 0242/0325) */
	if (sysctl_exists("kern.sweet_spmi")) {
		if (zone_src[ZONE_SOC] == SRC_NONE && read_pmic_die_mc(0) >= 0) {
			register_zone(ZONE_SOC, SRC_PMIC_DIE, "kern.sweet_spmi", 0, "PM6125 TEMP_ALARM (sid 0)");
		}
		if (zone_src[ZONE_BATTERY] == SRC_NONE && read_pmic_battery_mc(2) >= 0) {
			register_zone(ZONE_BATTERY, SRC_PMIC_BAT, "kern.sweet_spmi", 2, "PM7250B QG/JEITA battery (sid 2)");
		}
		if (zone_src[ZONE_SKIN] == SRC_NONE && read_pmic_die_mc(2) >= 0) {
			register_zone(ZONE_SKIN, SRC_PMIC_DIE, "kern.sweet_spmi", 2, "PM7250B TEMP_ALARM (sid 2)");
		}
	}

	if (zone_src[ZONE_SOC] == SRC_NONE && sysctl_exists("kern.sweet_tsens")) {
		register_zone(ZONE_SOC, SRC_SYSCTL, "kern.sweet_tsens", 0, "kern.sweet_tsens");
	}

	/* 3. Clean sysfs fallback only if /sys/class/thermal is present */
	if (access(THERMAL_BASE, R_OK) == 0) {
		DIR *d = opendir(THERMAL_BASE);
		if (d != NULL) {
			struct dirent *e;
			int seq_idx = 0;
			while ((e = readdir(d)) != NULL) {
				if (strncmp(e->d_name, "thermal_zone", 12) != 0) {
					continue;
				}
				char path[256], type_path[280], temp_path[280];
				snprintf(path, sizeof(path), "%s/%s", THERMAL_BASE, e->d_name);
				snprintf(type_path, sizeof(type_path), "%s/type", path);
				snprintf(temp_path, sizeof(temp_path), "%s/temp", path);
				if (access(temp_path, R_OK) != 0) {
					continue;
				}
				char type_buf[64] = "unknown";
				int fd = open(type_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
				if (fd >= 0) {
					ssize_t n = read(fd, type_buf, sizeof(type_buf) - 1);
					close(fd);
					if (n > 0) {
						type_buf[n] = '\0';
						type_buf[strcspn(type_buf, "\r\n")] = '\0';
					}
				}
				int idx = -1;
				if (strstr(type_buf, "cpu") || strstr(type_buf, "soc") || strstr(type_buf, "cluster")) {
					idx = ZONE_SOC;
				} else if (strstr(type_buf, "gpu") || strstr(type_buf, "adreno")) {
					idx = ZONE_GPU;
				} else if (strstr(type_buf, "battery") || strstr(type_buf, "pmic")) {
					idx = ZONE_BATTERY;
				} else if (strstr(type_buf, "skin")) {
					idx = ZONE_SKIN;
				} else {
					while (seq_idx < ZONE_COUNT && zone_src[seq_idx] != SRC_NONE) {
						seq_idx++;
					}
					idx = seq_idx < ZONE_COUNT ? seq_idx++ : -1;
				}
				if (idx >= 0 && idx < ZONE_COUNT && zone_src[idx] == SRC_NONE) {
					register_zone(idx, SRC_SYSFS, temp_path, 0, type_buf);
				}
			}
			closedir(d);
		}
	}

	if (zone_src[ZONE_BATTERY] == SRC_NONE && access(PMIC_TEMP_PATH, R_OK) == 0) {
		register_zone(ZONE_BATTERY, SRC_SYSFS, PMIC_TEMP_PATH, 0, PMIC_TEMP_PATH);
	}

	/* 4. Fallback to externally populated kern.sweet_thermal_zone* only for unbacked zones */
	for (int i = 0; i < ZONE_COUNT; i++) {
		if (zone_src[i] != SRC_NONE) {
			continue;
		}
		char sname[64];
		snprintf(sname, sizeof(sname), "kern.sweet_thermal_zone%d", i);
		int v = 0;
		if (sysctl_get_int(sname, &v) == 0 && v > 0 && is_valid_raw_temp_mc(i, v)) {
			register_zone(i, SRC_SYSCTL, sname, 0, sname);
		}
	}

	logf("Total thermal zones active: %d", zone_count);
}

int
main(void)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	logf("thermald starting (PID %d)", (int)getpid());

	serial_queue = dispatch_queue_create("com.sapphire.thermald", DISPATCH_QUEUE_SERIAL);

	dispatch_sync(serial_queue, ^{
		discover_thermal_zones();
		capture_baseline_settings();

		/* Single registration directly on serial_queue (no token leak, no cross-queue race) */
		if (notify_register_dispatch(N_BLANKED, &blank_token, serial_queue, ^(int token) {
			sync_blank_state(token);
		}) == NOTIFY_STATUS_OK) {
			logf("Registered for %s notification", N_BLANKED);
			sync_blank_state(blank_token);
		} else {
			blank_token = -1;
			logf("Failed to register for %s", N_BLANKED);
		}

		evaluate_thermal_state();
	});

	poll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, serial_queue);
	dispatch_source_set_timer(poll_timer,
	    dispatch_time(DISPATCH_TIME_NOW, POLL_INTERVAL_SEC * NSEC_PER_SEC),
	    POLL_INTERVAL_SEC * NSEC_PER_SEC, 200ull * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(poll_timer, ^{
		if (blank_token >= 0) {
			sync_blank_state(blank_token);
		}
		evaluate_thermal_state();
	});
	dispatch_resume(poll_timer);

	logf("thermald entering main loop");
	dispatch_main();
	return 0;
}
