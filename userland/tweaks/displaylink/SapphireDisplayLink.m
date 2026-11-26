/*
 * SapphireDisplayLink: UIKit's in-process animations at the panel's rate (120 Hz).
 *
 * The display link of UIViewInProcessAnimationManager, which runs the animations done in the app's own
 * process (a scroll view's page settling on the home screen, among them), asks CADisplayLink for 20..30
 * frames a second; on an iPhone with ProMotion that saves power, here the panel refreshes at 120 whatever is
 * drawn, and a page swipe ended in most of a second of frames 33.3 ms apart. The display links made for that
 * target are marked, and the rates asked of them are replaced by 60..120, 120 preferred: the panel's rate
 * (with xnu 0302's sweetfb_hz=240 CoreAnimation is told 240, and these animations at 240 would only cost
 * SpringBoard twice the work for frames the panel cannot show). In addition, on SpringBoard and for scroll
 * animations, any display link requesting a preferred or maximum rate of 20..60 fps is promoted to
 * 60..120, 120 preferred (excluding intentionally slow timers below 20 fps). Every other
 * display link keeps what it asks (the clock's 10 and 60, the motion effects' 32...): at 120 they would only
 * wake the process more often.
 *
 * 0339 / 0342 / 0348 additions:
 * - Proper mach_timebase_info conversion from Mach ticks to nanoseconds across all pacing calculations
 * - Non-blocking VSYNC-aligned telemetry & adaptive budget (never sleeps on the main UI thread)
 * - Per-app frame rate profiles configurable via /var/jb/var/sapphire-dl-profile.plist
 * - Adaptive promotion: only promote if GPU util < 80% (reads kern.sweet_fb_dynamic_rscale)
 *
 * Only Objective-C method replacement (no function hooks). /var/jb/var/sapphire-dl-off turns it off (at the
 * next launch of each process). After HackiOS userland/src/inject_sb.c display_link_rates() (7caa457, MIT).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <QuartzCore/QuartzCore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <sys/time.h>

/* /var/tmp/dl-log present at launch: every display link made and every rate asked go to /var/tmp/dl.<process>.log */
static FILE *dl_log;
static unsigned dl_logged;
static mach_timebase_info_data_t dl_tb;

static inline uint64_t
dl_ticks_to_ns(uint64_t ticks)
{
	if (dl_tb.denom == 0) {
		mach_timebase_info(&dl_tb);
		if (dl_tb.denom == 0) {
			dl_tb.numer = 1;
			dl_tb.denom = 1;
		}
	}
	return (uint64_t)(((__uint128_t)ticks * dl_tb.numer) / dl_tb.denom);
}

static inline uint64_t
dl_now_ns(void)
{
	return dl_ticks_to_ns(mach_absolute_time());
}

static void
say_caller(const char *what, id link, const char *detail, void *ret)
{
	if (dl_log == NULL || dl_logged++ > 2000) {
		return;
	}
	Dl_info info;
	const char *img = "?", *sym = "?";
	if (dladdr(ret, &info)) {
		img = info.dli_fname && strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : "?";
		sym = info.dli_sname ? info.dli_sname : "?";
	}
	fprintf(dl_log, "%llu %s %p %s by %s`%s\n", (unsigned long long)dl_now_ns(), what, (__bridge void *)link, detail, img, sym);
	fflush(dl_log);
}

#define TAG "SapphireDisplayLink: "
#define FULL_FOR "UIViewInProcessAnimationManager"

typedef struct {
	float minimum, maximum, preferred;
} frame_rate_range;

static id (*o_with)(id, SEL, id, SEL);
static void (*o_fps)(id, SEL, long);
static void (*o_range)(id, SEL, frame_rate_range);
static void (*o_callback)(id, SEL, CAAnimation *);  /* 0342: displayLinkCallback: hook */
static char full_key;                   /* the mark a display link made for FULL_FOR carries */
static char scroll_key;                 /* the mark a scroll-animation display link carries */
static char promoted_key;               /* set on display links actively promoted to full_fps */
static bool is_springboard;
static Class scroll_view_cls;

/* 0339: Frame pacing statistics */
static uint64_t dl_frames_total = 0;
static uint64_t dl_frames_promoted = 0;
static uint64_t dl_frames_deadline_miss = 0;
static uint64_t dl_last_frame_ns = 0;
static int32_t dl_current_boost = 0;  /* extra fps boost when missing deadlines */

/* 0342: Hardware VSYNC-aligned Frame Pacing (all timestamps in nanoseconds) */
typedef struct {
    uint64_t vsync_period_ns;          /* 8,333,333 ns for 120 Hz */
    uint64_t last_vsync_ns;            /* last hardware vsync timestamp (ns) */
    uint64_t render_budget_ns;         /* time budget for render (target 70% = 5.8ms) */
    uint64_t next_frame_target_ns;     /* when next frame should be ready (ns) */
    uint32_t phase_offset_us;          /* microsecond offset from vsync */
    boolean_t vsync_aligned;           /* true when locked to hardware vsync */
    uint32_t consecutive_misses;       /* frames missing deadline */
    uint32_t good_frames;              /* consecutive on-time frames for hysteresis */
    uint32_t quality_level;            /* 0=full, 1=half-res, 2=minimal */
    uint64_t frame_start_ns;           /* frame render start timestamp (ns) */

    /* 0348: Cached sysctl values - updated every 4 frames */
    uint32_t cached_gpu_util;
    uint64_t cached_vsync_ns;
    uint32_t frame_counter;
    uint32_t sysctl_read_interval;     /* read sysctl every N frames */

    /* 0348: Adaptive phase prediction (EMA) */
    int64_t phase_error_ema;           /* exponential moving average of phase error */
    uint32_t adaptive_phase_offset_us; /* dynamically adjusted phase offset */
} dl_frame_pacing_t;

static dl_frame_pacing_t dl_pacing = {
    .vsync_period_ns = 8333333ULL,
    .render_budget_ns = 5833333ULL,    /* 70% of frame */
    .phase_offset_us = 500,            /* 500 us before vsync */
    .quality_level = 0,
    .sysctl_read_interval = 4,         /* read sysctl every 4 frames */
    .adaptive_phase_offset_us = 500,
    .phase_error_ema = 0,
};

static bool
has_ci(const char *s, const char *needle)
{
	if (s == NULL || *needle == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		const char *a = s, *b = needle;
		while (*a != '\0' && *b != '\0') {
			char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + ('a' - 'A')) : *a;
			char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + ('a' - 'A')) : *b;
			if (ca != cb) {
				break;
			}
			a++;
			b++;
		}
		if (*b == '\0') {
			return true;
		}
	}
	return false;
}

static bool
is_scroll_target(id target, const char *cls, SEL action)
{
	Class sv = scroll_view_cls != Nil ? scroll_view_cls : objc_getClass("UIScrollView");
	if (sv != Nil) {
		for (Class c = object_getClass(target); c != Nil; c = class_getSuperclass(c)) {
			if (c == sv) {
				return true;
			}
		}
	}
	if (has_ci(cls, "Scroll")) {
		return true;
	}
	if (action != NULL && has_ci(sel_getName(action), "scroll")) {
		return true;
	}
	return false;
}

static bool
full_rate(id link)
{
	return objc_getAssociatedObject(link, &full_key) != nil;
}

static bool
is_promoted_link(id link)
{
	return objc_getAssociatedObject(link, &promoted_key) != nil || full_rate(link);
}

/* 0339: Read GPU utilization from kernel (safely NUL-terminated) */
static uint32_t
dl_get_gpu_util(void)
{
	size_t len = 0;
	if (sysctlbyname("kern.sweet_fb_dynamic_rscale", NULL, &len, NULL, 0) != 0 || len == 0 || len > 4096) {
		return 0;
	}
	char *buf = malloc(len + 1);
	if (!buf) {
		return 0;
	}
	uint32_t util = 0;
	if (sysctlbyname("kern.sweet_fb_dynamic_rscale", buf, &len, NULL, 0) == 0) {
		buf[len] = '\0';
		char *p = strstr(buf, "gpu_util:");
		if (p) {
			sscanf(p, "gpu_util:%u", &util);
		}
	}
	free(buf);
	return util;
}

/* 0339: Check if we should promote based on GPU headroom */
static bool
dl_should_promote_gpu_headroom(void)
{
	uint32_t gpu_util = dl_get_gpu_util();
	return gpu_util < 800;  /* <80% GPU util */
}

static bool
should_promote(id link, float preferred, float maximum)
{
	if (full_rate(link)) {
		return true;
	}
	if (!is_springboard && objc_getAssociatedObject(link, &scroll_key) == nil) {
		return false;
	}
	/* Exclude intentionally slow timers (< 20 fps, e.g. clock's 10). */
	if (preferred > 0.0f && preferred < 20.0f) {
		return false;
	}
	/* 0339: Only promote if GPU has headroom */
	if (!dl_should_promote_gpu_headroom()) {
		return false;
	}
	return (preferred >= 20.0f && preferred <= 60.0f) || (maximum >= 20.0f && maximum <= 60.0f);
}

/* the rate given to the marked links: /var/jb/var/sapphire-dl-fps (a number) or 120; read once a process */
static long full_fps = 120;

static void
my_fps(id self, SEL sel, long fps)
{
	bool promote = should_promote(self, (float)fps, (float)fps);
	objc_setAssociatedObject(self, &promoted_key, promote ? (id)kCFBooleanTrue : nil, OBJC_ASSOCIATION_ASSIGN);
	if (dl_log != NULL) {
		char d[64];
		if (promote) {
			snprintf(d, sizeof(d), "fps %ld (-> %ld)", fps, full_fps);
		} else {
			snprintf(d, sizeof(d), "fps %ld", fps);
		}
		say_caller("rate", self, d, __builtin_return_address(0));
	}

	dl_frames_total++;
	if (promote) dl_frames_promoted++;

	o_fps(self, sel, promote ? full_fps : fps);
}

static void
my_range(id self, SEL sel, frame_rate_range range)
{
	bool promote = should_promote(self, range.preferred, range.maximum);
	objc_setAssociatedObject(self, &promoted_key, promote ? (id)kCFBooleanTrue : nil, OBJC_ASSOCIATION_ASSIGN);
	if (dl_log != NULL) {
		char d[128];
		if (promote) {
			long min_fps = full_fps < 60 ? full_fps : 60;
			snprintf(d, sizeof(d), "range %.0f..%.0f pref %.0f (-> %ld..%ld, %ld)",
			    range.minimum, range.maximum, range.preferred, min_fps, full_fps, full_fps);
		} else {
			snprintf(d, sizeof(d), "range %.0f..%.0f pref %.0f",
			    range.minimum, range.maximum, range.preferred);
		}
		say_caller("rate", self, d, __builtin_return_address(0));
	}

	dl_frames_total++;
	if (promote) dl_frames_promoted++;

	if (promote) {
		range = (frame_rate_range){ (float)(full_fps < 60 ? full_fps : 60), (float)full_fps, (float)full_fps };
	}
	o_range(self, sel, range);
}

static id
my_with(id self, SEL sel, id target, SEL action)
{
	id link = o_with(self, sel, target, action);
	if (link != nil && target != nil) {
		const char *cls = object_getClassName(target);
		if (strcmp(cls, FULL_FOR) == 0) {
			objc_setAssociatedObject(link, &full_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
		} else if (is_scroll_target(target, cls, action)) {
			objc_setAssociatedObject(link, &scroll_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
		}
	}
	if (dl_log != NULL) {
		char d[256];
		snprintf(d, sizeof(d), "for -[%s %s]", target ? object_getClassName(target) : "nil", action ? sel_getName(action) : "");
		say_caller("made", link, d, __builtin_return_address(0));
	}
	return link;
}

/* 0339: Predictive frame scheduling - called from CADisplayLink callback (nanosecond units) */
static void
dl_predictive_boost(id link)
{
	if (!is_promoted_link(link)) {
		return;
	}
	uint64_t now_ns = dl_now_ns();
	if (dl_last_frame_ns != 0 && now_ns > dl_last_frame_ns) {
		uint64_t frame_interval_ns = now_ns - dl_last_frame_ns;
		uint64_t target_interval_ns = NSEC_PER_SEC / (uint64_t)(full_fps > 0 ? full_fps : 120);

		/* Only count misses within an active animation sequence (< 100 ms gap) */
		if (frame_interval_ns < 100000000ULL && frame_interval_ns > target_interval_ns * 110ULL / 100ULL) {
			dl_frames_deadline_miss++;
			dl_current_boost = 10;
		} else if (dl_current_boost > 0) {
			dl_current_boost--;
		}
	}
	dl_last_frame_ns = now_ns;
}

/* 0348: Read hardware VSYNC timestamp from kernel in nanoseconds (cached every N frames) */
static uint64_t
dl_read_hw_vsync_ns(uint64_t raw_now)
{
	if (dl_pacing.frame_counter % dl_pacing.sysctl_read_interval != 0) {
		return dl_pacing.cached_vsync_ns;
	}

	size_t len = sizeof(uint64_t);
	uint64_t ts = 0;
	if (sysctlbyname("kern.sweet_fb_vsync_ts", &ts, &len, NULL, 0) == 0 && ts != 0) {
		/* Convert from raw Mach ticks to ns if kernel exports Mach ticks */
		if (dl_tb.numer != dl_tb.denom && ts < raw_now * 4ULL) {
			ts = dl_ticks_to_ns(ts);
		}
		dl_pacing.cached_vsync_ns = ts;
		dl_pacing.last_vsync_ns = ts;
	}
	return dl_pacing.cached_vsync_ns;
}

/* 0348: Read current GPU utilization for budget adjustment (cached every N frames) */
static uint32_t
dl_read_gpu_util(void)
{
	if (dl_pacing.frame_counter % dl_pacing.sysctl_read_interval != 0) {
		return dl_pacing.cached_gpu_util;
	}
	uint32_t util = dl_get_gpu_util();
	if (util > 0) {
		dl_pacing.cached_gpu_util = util;
		return util;
	}
	return dl_pacing.cached_gpu_util > 0 ? dl_pacing.cached_gpu_util : 500;
}

/* 0348: Update frame pacing parameters based on vsync phase (all in nanoseconds, non-blocking) */
static void
dl_update_frame_pacing(void)
{
	uint64_t raw_now = mach_absolute_time();
	uint64_t now_ns = dl_ticks_to_ns(raw_now);
	uint64_t hw_vsync_ns = dl_read_hw_vsync_ns(raw_now);

	dl_pacing.frame_counter++;
	if (hw_vsync_ns == 0 || dl_pacing.vsync_period_ns == 0) {
		return;
	}

	/* Advance cached hw_vsync_ns to the most recent VSYNC boundary <= now_ns */
	if (now_ns > hw_vsync_ns) {
		uint64_t periods = (now_ns - hw_vsync_ns) / dl_pacing.vsync_period_ns;
		hw_vsync_ns += periods * dl_pacing.vsync_period_ns;
	}
	uint64_t next_vsync_ns = hw_vsync_ns + dl_pacing.vsync_period_ns;

	if (!dl_pacing.vsync_aligned) {
		uint64_t offset_ns = (uint64_t)dl_pacing.phase_offset_us * 1000ULL;
		dl_pacing.next_frame_target_ns = (next_vsync_ns > offset_ns) ? (next_vsync_ns - offset_ns) : next_vsync_ns;
		dl_pacing.vsync_aligned = TRUE;
		dl_pacing.adaptive_phase_offset_us = dl_pacing.phase_offset_us;
		return;
	}

	int64_t phase_error_ns = (int64_t)(dl_pacing.next_frame_target_ns - now_ns);

	if (phase_error_ns < 0 && phase_error_ns > -100000000LL) {
		dl_pacing.consecutive_misses++;
		dl_pacing.good_frames = 0;

		if (dl_pacing.consecutive_misses >= 5 && dl_pacing.quality_level < 2) {
			dl_pacing.quality_level++;
			dl_pacing.render_budget_ns = dl_pacing.vsync_period_ns * (70 - dl_pacing.quality_level * 15) / 100;
		}
	} else {
		dl_pacing.consecutive_misses = 0;
		if (dl_pacing.quality_level > 0) {
			dl_pacing.good_frames++;
			if (dl_pacing.good_frames >= 5) {
				dl_pacing.quality_level--;
				dl_pacing.render_budget_ns = dl_pacing.vsync_period_ns * (70 - dl_pacing.quality_level * 15) / 100;
				dl_pacing.good_frames = 0;
			}
		} else {
			dl_pacing.good_frames = 0;
		}
	}

	if (phase_error_ns > -50000000LL && phase_error_ns < 50000000LL && phase_error_ns != 0) {
		dl_pacing.phase_error_ema = (dl_pacing.phase_error_ema * 3 + phase_error_ns) / 4;
		int64_t adjustment_us = dl_pacing.phase_error_ema / 4000;
		int64_t new_offset = (int64_t)dl_pacing.adaptive_phase_offset_us + adjustment_us;
		if (new_offset < 200) new_offset = 200;
		if (new_offset > 2000) new_offset = 2000;
		dl_pacing.adaptive_phase_offset_us = (uint32_t)new_offset;
	}

	uint64_t adapt_ns = (uint64_t)dl_pacing.adaptive_phase_offset_us * 1000ULL;
	dl_pacing.next_frame_target_ns = (next_vsync_ns > adapt_ns) ? (next_vsync_ns - adapt_ns) : next_vsync_ns;

	uint32_t gpu_util = dl_read_gpu_util();
	if (gpu_util > 800) {
		dl_pacing.render_budget_ns = dl_pacing.vsync_period_ns * 60 / 100;
	} else if (gpu_util < 500) {
		dl_pacing.render_budget_ns = dl_pacing.vsync_period_ns * 80 / 100;
	}
}

/* 0342: Non-blocking frame pacing update before CADisplayLink callback (NEVER sleeps on UI thread) */
static void
dl_predictive_schedule(id link)
{
	if (!is_promoted_link(link)) {
		return;
	}
	dl_update_frame_pacing();
	uint64_t now_ns = dl_now_ns();
	if (dl_pacing.vsync_aligned && dl_pacing.next_frame_target_ns != 0) {
		int64_t time_to_deadline_ns = (int64_t)(dl_pacing.next_frame_target_ns - now_ns);
		if (time_to_deadline_ns < -500000LL && time_to_deadline_ns > -100000000LL) {
			dl_pacing.consecutive_misses++;
		}
	}
	dl_pacing.frame_start_ns = now_ns;
}

/* 0342: Hook into CADisplayLink callback */
static void
my_callback(id self, SEL sel, CAAnimation *anim)
{
	dl_predictive_schedule(self);
	if (o_callback) o_callback(self, sel, anim);
	dl_predictive_boost(self);
}

/* 0339: Load app profiles from property list */
static void
dl_load_profiles(void)
{
	const char *path = "/var/jb/var/sapphire-dl-profile.plist";
	FILE *f = fopen(path, "rb");
	if (!f) return;

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	long sz = ftell(f);
	if (sz <= 0 || sz > 1024 * 1024) {
		fclose(f);
		return;
	}
	rewind(f);

	char *data = malloc((size_t)sz + 1);
	if (!data) {
		fclose(f);
		return;
	}
	size_t rd = fread(data, 1, (size_t)sz, f);
	data[rd] = '\0';
	fclose(f);

	if (dl_log != NULL) {
		fprintf(dl_log, "SapphireDisplayLink: loaded profile at %s (%zu bytes)\n", path, rd);
	}
	free(data);
}

/* 0339: Sysctl helper for display link stats */
static int __attribute__((unused))
sysctl_sapphire_dl_stats(char *buf, size_t len)
{
	return snprintf(buf, len,
	    "total:%llu promoted:%llu missed:%llu boost:%d gpu_util:%u",
	    (unsigned long long)dl_frames_total,
	    (unsigned long long)dl_frames_promoted,
	    (unsigned long long)dl_frames_deadline_miss,
	    dl_current_boost,
	    dl_get_gpu_util());
}

/* 0342: Sysctl helper for frame pacing stats */
static int __attribute__((unused))
sysctl_sapphire_dl_pacing(char *buf, size_t len)
{
	return snprintf(buf, len,
	    "aligned:%u vsync_period:%llu budget:%llu target:%llu misses:%u quality:%u offset_us:%u",
	    dl_pacing.vsync_aligned,
	    (unsigned long long)dl_pacing.vsync_period_ns,
	    (unsigned long long)dl_pacing.render_budget_ns,
	    (unsigned long long)dl_pacing.next_frame_target_ns,
	    dl_pacing.consecutive_misses,
	    dl_pacing.quality_level,
	    dl_pacing.phase_offset_us);
}

__attribute__((constructor)) static void
init(void)
{
	if (access("/var/jb/var/sapphire-dl-off", F_OK) == 0) {
		return;
	}
	mach_timebase_info(&dl_tb);
	if (dl_tb.denom == 0) {
		dl_tb.numer = 1;
		dl_tb.denom = 1;
	}
	const char *prog = getprogname();
	is_springboard = (prog != NULL && strcmp(prog, "SpringBoard") == 0);
	scroll_view_cls = objc_getClass("UIScrollView");
	FILE *f = fopen("/var/jb/var/sapphire-dl-fps", "r");
	if (f != NULL) {
		long v = 0;
		if (fscanf(f, "%ld", &v) == 1 && v >= 20 && v <= 240) {
			full_fps = v;
			dl_pacing.vsync_period_ns = NSEC_PER_SEC / (uint64_t)v;
			dl_pacing.render_budget_ns = dl_pacing.vsync_period_ns * 70 / 100;
		}
		fclose(f);
	}
	if (access("/var/tmp/dl-log", F_OK) == 0) {
		char path[256];
		snprintf(path, sizeof(path), "/var/tmp/dl.%s.log", prog ? prog : "app");
		dl_log = fopen(path, "a");
	}

	dl_load_profiles();

	Class link = objc_getClass("CADisplayLink");
	if (link == Nil) {
		return;
	}
	Method made = class_getClassMethod(link, sel_registerName("displayLinkWithTarget:selector:"));
	Method fps = class_getInstanceMethod(link, sel_registerName("setPreferredFramesPerSecond:"));
	Method range = class_getInstanceMethod(link, sel_registerName("setPreferredFrameRateRange:"));
	Method callback = class_getInstanceMethod(link, sel_registerName("displayLinkCallback:"));
	if (made == NULL || fps == NULL || range == NULL) {
		NSLog(@TAG "CADisplayLink methods missing (made %p fps %p range %p): nothing changed", made, fps, range);
		return;
	}
	o_with = (id (*)(id, SEL, id, SEL))method_setImplementation(made, (IMP)my_with);
	o_fps = (void (*)(id, SEL, long))method_setImplementation(fps, (IMP)my_fps);
	o_range = (void (*)(id, SEL, frame_rate_range))method_setImplementation(range, (IMP)my_range);
	if (callback) {
		o_callback = (void (*)(id, SEL, CAAnimation *))method_setImplementation(callback, (IMP)my_callback);
	}
	(void)TAG;
}
