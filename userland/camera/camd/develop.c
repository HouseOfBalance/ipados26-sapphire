/* camd develop, see develop.h. The graph is rawtool's (rawtool.c main) with camd's choices:
 * guided remosaic (cell sensors) or MHC demosaic (Bayer), white balance from the preview, the
 * "typical" colour matrix (the same one the preview uses), sRGB, Exif in the JPEG.
 *
 * Memory pressure & Jetsam protection:
 *  - Before developing a full-res 108 MP (12000x9000, cell > 1) frame, queries XNU Jetsam /
 *    process headroom via os_proc_available_memory() (resolved via dlsym), TASK_VM_INFO,
 *    HOST_VM_INFO64, and hw.memsize. When available headroom is < 512 MiB, automatically falls
 *    back to 3x3 nona-binned 12 MP (4000x3000) via cell_bin() + MHC demosaic instead of getting
 *    SIGKILL'd by Jetsam.
 *  - Uses F_NOCACHE on the temporary RAW input, closes the RAW fd as soon as the source stage
 *    finishes reading, and immediately releases each intermediate Stage ring buffer via
 *    madvise(..., MADV_FREE) + free() + malloc_zone_pressure_relief() as soon as downstream
 *    consumers finish reading its final row.
 */
#include "develop.h"
#include "../rawio.h"
#include "../remosaic.h"
#include "../color.h"
#include "../out.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#define DEV_108MP_MIN_HEADROOM ((size_t)512 * 1024 * 1024) /* 512 MiB safe Jetsam headroom */
#define DEV_108MP_MIN_MEMSIZE  ((uint64_t)3ULL << 30)      /* 3 GiB minimum XNU physical RAM */
#define DEV_108MP_PIXEL_THRESH 50000000ULL                 /* >= 50 MP cell frame triggers check */
#define DEV_TASK_BUDGET_BYTES  ((uint64_t)700 * 1024 * 1024)

#ifdef __APPLE__
typedef size_t (*os_proc_avail_fn)(void);
typedef size_t (*malloc_pressure_relief_fn)(void *, size_t);

static os_proc_avail_fn g_os_proc_avail;
static malloc_pressure_relief_fn g_malloc_pressure_relief;
static pthread_once_t g_mem_syms_once = PTHREAD_ONCE_INIT;

static void init_mem_syms(void)
{
	g_os_proc_avail = (os_proc_avail_fn)dlsym(RTLD_DEFAULT, "os_proc_available_memory");
	g_malloc_pressure_relief = (malloc_pressure_relief_fn)dlsym(RTLD_DEFAULT, "malloc_zone_pressure_relief");
}
#endif

static bool dev_has_108mp_headroom(size_t *avail_out, uint64_t *memsize_out)
{
	size_t eff_avail = SIZE_MAX;
	uint64_t memsize = 0;
#ifdef __APPLE__
	pthread_once(&g_mem_syms_once, init_mem_syms);
	size_t mlen = sizeof memsize;
	if (sysctlbyname("hw.memsize", &memsize, &mlen, NULL, 0) != 0) memsize = 0;

	if (g_os_proc_avail != NULL) {
		errno = 0;
		size_t pa = g_os_proc_avail();
		if (errno == 0 && pa > 0 && pa < eff_avail) eff_avail = pa;
	}

	task_vm_info_data_t tvi;
	mach_msg_type_number_t tcnt = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&tvi, &tcnt) == KERN_SUCCESS) {
		uint64_t rem = tvi.phys_footprint < DEV_TASK_BUDGET_BYTES ? DEV_TASK_BUDGET_BYTES - tvi.phys_footprint : 0;
		size_t tr = rem > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)rem;
		if (tr < eff_avail) eff_avail = tr;
	}

	vm_size_t pg = 0;
	if (host_page_size(mach_host_self(), &pg) != KERN_SUCCESS || pg == 0) {
		long pgl = sysconf(_SC_PAGESIZE);
		pg = pgl > 0 ? (vm_size_t)pgl : 16384;
	}
	vm_statistics64_data_t vm;
	mach_msg_type_number_t vcnt = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &vcnt) == KERN_SUCCESS) {
		uint64_t pages = (uint64_t)vm.free_count + (uint64_t)vm.speculative_count +
		    (uint64_t)vm.purgeable_count + (uint64_t)vm.external_page_count;
		uint64_t sb = pages * (uint64_t)pg;
		size_t sa = sb > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)sb;
		if (sa < eff_avail) eff_avail = sa;
	}

	if (avail_out) *avail_out = eff_avail == SIZE_MAX ? 0 : eff_avail;
	if (memsize_out) *memsize_out = memsize;
	if (memsize > 0 && memsize < DEV_108MP_MIN_MEMSIZE) return false;
	if (eff_avail != SIZE_MAX && eff_avail < DEV_108MP_MIN_HEADROOM) return false;
#else
	if (avail_out) *avail_out = 0;
	if (memsize_out) *memsize_out = 0;
	(void)eff_avail;
	(void)memsize;
#endif
	return true;
}

static void madvise_free_interior(void *ptr, size_t bytes)
{
	if (!ptr || bytes == 0) return;
	long pgl = sysconf(_SC_PAGESIZE);
	size_t pg = pgl > 0 ? (size_t)pgl : 16384;
	uintptr_t start = ((uintptr_t)ptr + pg - 1) & ~(uintptr_t)(pg - 1);
	uintptr_t end = ((uintptr_t)ptr + bytes) & ~(uintptr_t)(pg - 1);
	if (end > start) {
#ifdef MADV_FREE
		(void)madvise((void *)start, (size_t)(end - start), MADV_FREE);
#elif defined(MADV_DONTNEED)
		(void)madvise((void *)start, (size_t)(end - start), MADV_DONTNEED);
#endif
	}
}

static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Update ExifIFD PixelXDimension (0xA002) and PixelYDimension (0xA003) in a copy of an APP1 segment
 * when develop_run() falls back from 108 MP (12000x9000) to 3x3 nona-binned 12 MP (4000x3000). */
static void patch_exif_dims(uint8_t *app1, size_t len, uint32_t w, uint32_t h)
{
	if (!app1 || len < 26) return;
	if (app1[0] != 0xff || app1[1] != 0xe1 || memcmp(app1 + 4, "Exif\0\0II*\0", 10) != 0) return;
	uint8_t *t = app1 + 10;
	size_t tlen = len - 10;
	uint32_t ifd0 = rd_le32(t + 4);
	if (ifd0 + 2 > tlen) return;
	uint16_t n0 = rd_le16(t + ifd0);
	if ((size_t)ifd0 + 2 + 12 * (size_t)n0 > tlen) return;
	uint32_t exif_off = 0;
	for (uint16_t i = 0; i < n0; i++) {
		const uint8_t *e = t + ifd0 + 2 + 12 * (size_t)i;
		if (rd_le16(e) == 0x8769 && rd_le16(e + 2) == 4 && rd_le32(e + 4) == 1) {
			exif_off = rd_le32(e + 8);
			break;
		}
	}
	if (exif_off == 0 || (size_t)exif_off + 2 > tlen) return;
	uint16_t m = rd_le16(t + exif_off);
	if ((size_t)exif_off + 2 + 12 * (size_t)m > tlen) return;
	for (uint16_t i = 0; i < m; i++) {
		uint8_t *e = t + exif_off + 2 + 12 * (size_t)i;
		uint16_t tag = rd_le16(e), type = rd_le16(e + 2);
		uint32_t cnt = rd_le32(e + 4);
		if (cnt != 1 || (type != 4 && type != 3)) continue;
		if (tag == 0xA002) wr_le32(e + 8, type == 3 && w <= 65535 ? (w & 0xffffu) : w);
		else if (tag == 0xA003) wr_le32(e + 8, type == 3 && h <= 65535 ? (h & 0xffffu) : h);
	}
}

static const float ccm_typical[9] = { 1.62f, -0.48f, -0.14f, -0.24f, 1.52f, -0.28f, -0.04f, -0.56f, 1.60f };

static void mat3_inv(const float *m, float *o)
{
	double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
	double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (fabs(det) < 1e-9) die("CCM is singular");
	double r[9] = { e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g,
			c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d };
	for (int k = 0; k < 9; k++) o[k] = (float)(r[k] / det);
}

static void mat3_mul(const float *a, const float *b, float *o)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) o[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
}

static void say(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int fd, const char *fmt, ...)
{
	if (fd < 0) return;
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n > 0) (void)!write(fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

/* ---- progress: how far the source stage has been pulled ---- */
static _Atomic(Stage *) g_src;
static int g_pfd = -1;
static atomic_bool g_done;

static void *progress_thread(void *arg)
{
	(void)arg;
	int last = -1;
	while (!atomic_load(&g_done)) {
		usleep(250000);
		Stage *src = atomic_load(&g_src);
		if (!src || src->h <= 0) continue;
		int p = (int)((long long)src->hi * 100 / src->h);
		if (p > 99) p = 99;
		if (p != last) say(g_pfd, "P %d\n", p);
		last = p;
	}
	return NULL;
}

/* per-colour means of ~400 evenly spaced rows (linear, unclipped pixels only) */
static void sample_means(RawIn *r, double mean[3])
{
	uint16_t *t = xmalloc(sizeof(uint16_t) * r->w);
	double s[3] = { 0 }, n[3] = { 0 };
	int rows = 400, step = r->h / rows > 1 ? r->h / rows : 1;
	for (int y = step / 2; y < r->h; y += step) {
		rawin_read_rows(r, y, 1, t);
		for (int x = 0; x + 1 < r->w; x += 2) {
			int c = geo_color(&r->geo, x, y);
			float v = (t[x] - r->black[c]) / (r->white - r->black[c]);
			if (v > 0.002f && v < 0.92f) {
				s[c] += v;
				n[c] += 1;
			}
			c = geo_color(&r->geo, x + 1, y);
			v = (t[x + 1] - r->black[c]) / (r->white - r->black[c]);
			if (v > 0.002f && v < 0.92f) {
				s[c] += v;
				n[c] += 1;
			}
		}
	}
	for (int c = 0; c < 3; c++) mean[c] = n[c] > 0 ? s[c] / n[c] : 0;
	free(t);
}

/* ---- topological stage tracker for immediate ring buffer release ---- */
#define MAX_DEV_STAGES 32

typedef struct {
	Stage *list[MAX_DEV_STAGES];
	int n;
} StageSet;

static void collect_stages(StageSet *set, Stage *s)
{
	if (!s) return;
	for (int i = 0; i < set->n; i++)
		if (set->list[i] == s) return;
	for (int i = 0; i < s->nin; i++)
		collect_stages(set, s->in[i]);
	if (set->n < MAX_DEV_STAGES)
		set->list[set->n++] = s;
}

static void release_stage_ring(Stage *st)
{
	if (!st || !st->ring) return;
	size_t bytes = sizeof(float) * st->rowlen * (size_t)st->cap;
	madvise_free_interior(st->ring, bytes);
	free(st->ring);
	st->ring = NULL;
}

static void reclaim_finished_stages(StageSet *set, Stage *raw_stage, RawIn *raw_in, Sink **sinks, int ns)
{
	if (raw_in && raw_in->fd >= 0 && raw_stage && raw_stage->hi >= raw_stage->h) {
		close(raw_in->fd);
		raw_in->fd = -1;
	}
	bool freed_any = false;
	for (int i = 0; i < set->n; i++) {
		Stage *st = set->list[i];
		if (!st->ring || st->hi < st->h) continue;
		bool busy = false;
		for (int j = 0; j < set->n && !busy; j++) {
			Stage *c = set->list[j];
			if (c->hi >= c->h) continue;
			for (int k = 0; k < c->nin; k++) {
				if (c->in[k] == st) {
					busy = true;
					break;
				}
			}
		}
		for (int k = 0; k < ns && !busy; k++) {
			if (sinks[k] && sinks[k]->src == st && sinks[k]->done < sinks[k]->h)
				busy = true;
		}
		if (!busy) {
			release_stage_ring(st);
			freed_any = true;
		}
	}
#ifdef __APPLE__
	if (freed_any && g_malloc_pressure_relief)
		g_malloc_pressure_relief(NULL, 0);
#else
	(void)freed_any;
#endif
}

static void develop_sinks_streaming(Sink **s, int ns, StageSet *stages, Stage *raw_stage, RawIn *raw_in)
{
	int H = 0;
	for (int i = 0; i < ns; i++)
		if (s[i]->h > H) H = s[i]->h;
	for (int r = 0; r < H;) {
		r += g_chunk;
		if (r > H) r = H;
		for (int i = 0; i < ns; i++) {
			Sink *k = s[i];
			int t = (int)(((long long)r * k->h + H - 1) / H);
			t = (t + k->gran - 1) / k->gran * k->gran;
			if (t > k->h) t = k->h;
			if (r == H) t = k->h;
			while (k->done < t) {
				int b = k->done + g_chunk;
				b = (b + k->gran - 1) / k->gran * k->gran;
				if (b > t) b = t;
				stage_ensure(k->src, b + 1 > k->h ? k->h : b + 1);
				reclaim_finished_stages(stages, raw_stage, raw_in, s, ns);
				double t0 = now_sec();
				k->consume(k, k->done, b);
				k->t += now_sec() - t0;
				k->done = b;
				reclaim_finished_stages(stages, raw_stage, raw_in, s, ns);
			}
		}
	}
	for (int i = 0; i < ns; i++) {
		double t0 = now_sec();
		s[i]->finish(s[i]);
		s[i]->t += now_sec() - t0;
	}
}

int develop_run(const DevJob *j)
{
	if (!j || !j->raw || !j->jpeg || j->w <= 0 || j->h <= 0) return -1;
	g_pfd = j->progress_fd;
	atomic_store(&g_src, NULL);
	atomic_store(&g_done, false);

	size_t avail_bytes = 0;
	uint64_t memsize_bytes = 0;
	bool is_fullres_cell = (j->cell > 1 && (uint64_t)j->w * (uint64_t)j->h >= DEV_108MP_PIXEL_THRESH);
	bool binned_fallback = is_fullres_cell && !dev_has_108mp_headroom(&avail_bytes, &memsize_bytes);

	if (is_fullres_cell && !binned_fallback)
		g_chunk = 16; /* tight 16-row JPEG MCU band keeps 108 MP ring buffers compact */
	else
		g_chunk = j->chunk >= 16 ? (j->chunk + 15) / 16 * 16 : 32;

	RawIn r;
	memset(&r, 0, sizeof r);
	r.fd = -1;
	r.fmt = FMT_RAW10;
	r.w = j->w;
	r.h = j->h;
	r.stride = j->stride;
	r.white = j->white;
	r.geo.n = j->cell > 0 ? j->cell : 1;
	cfa_parse(j->cfa ? j->cfa : "grbg", &r.geo.cfa);
	for (int c = 0; c < 3; c++) r.black[c] = j->black;
	rawin_open(&r, j->raw);
#ifdef F_NOCACHE
	if (r.fd >= 0) (void)fcntl(r.fd, F_NOCACHE, 1);
#endif

	/* white balance and exposure check from a sample of the frame */
	double m[3];
	sample_means(&r, m);
	say(g_pfd, "CAL %.6f\n", m[CG]);
	float gain[3];
	if (j->wb[0] > 0 && j->wb[1] > 0 && j->wb[2] > 0) memcpy(gain, j->wb, sizeof gain);
	else
		for (int c = 0; c < 3; c++) gain[c] = m[c] > 1e-6 ? (float)(m[CG] / m[c]) : 1;
	float gmin = fminf(gain[0], fminf(gain[1], gain[2]));
	for (int c = 0; c < 3; c++) gain[c] /= gmin;
	memcpy(r.gain, gain, sizeof gain);
	float ev = j->ev;
	if (j->expect_g > 1e-4f && m[CG] > 1e-4) {
		/* the still came out brighter/darker than the preview said (mode sensitivity): fix in the JPEG */
		float fix = log2f(j->expect_g / (float)m[CG]);
		fix = fix < -1 ? -1 : fix > 1.5f ? 1.5f : fix;
		if (fabsf(fix) > 0.1f) ev += fix;
	}

	pool_init(j->threads > 0 ? j->threads : ncpus());
	pthread_t pt;
	bool pt_ok = (pthread_create(&pt, NULL, progress_thread, NULL) == 0);

	Stage *raw = rawin_stage(&r);
	atomic_store(&g_src, raw);
	Stage *rgb, *bayer = NULL;
	Cfa bcfa;
	if (binned_fallback) {
		bayer = cell_bin(raw, &r.geo, &bcfa);
		rgb = demosaic_mhc(bayer, &bcfa);
		fprintf(stderr, "develop: low memory headroom (%zu MiB < %zu MiB, hw.memsize %llu MiB): "
			"falling back from %dx%d to %dx%d (%dx%d nona-binned)\n",
			avail_bytes >> 20, (size_t)(DEV_108MP_MIN_HEADROOM >> 20),
			(unsigned long long)(memsize_bytes >> 20), r.w, r.h, rgb->w, rgb->h, r.geo.n, r.geo.n);
		say(g_pfd, "T low memory headroom (%zu MiB < %zu MiB, hw.memsize %llu MiB): "
		    "falling back from %dx%d to %dx%d (%dx%d nona-binned)\n",
		    avail_bytes >> 20, (size_t)(DEV_108MP_MIN_HEADROOM >> 20),
		    (unsigned long long)(memsize_bytes >> 20), r.w, r.h, rgb->w, rgb->h, r.geo.n, r.geo.n);
		say(g_pfd, "BIN %d %d\n", rgb->w, rgb->h);
	} else if (r.geo.n > 1) {
		RemosaicParams rp = { 0.04f, 0.002f, 0.002f, 1, 0.4f, 0 };
		rgb = remosaic_rgb(raw, &r.geo, &rp);
		cfa_parse("rggb", &bcfa);
		if (j->dng) bayer = mosaic_sample(rgb, &bcfa);
	} else {
		bayer = raw;
		bcfa = geo_bayer_cfa(&r.geo);
		rgb = demosaic_mhc(bayer, &bcfa);
	}
	rgb = stage_color(rgb, ccm_typical, powf(2, ev), 1);
	float sharpen = j->sharpen > 0 ? j->sharpen : (binned_fallback ? 0.3f : 0.0f);
	if (sharpen > 0) rgb = stage_sharpen(rgb, sharpen);

	int out_w = rgb->w, out_h = rgb->h;
	Tone *tone = xmalloc(sizeof *tone);
	tone_init(tone, "srgb");
	Sink *sinks[2] = { NULL, NULL };
	int ns = 0;
	if (j->dng) {
		DngInfo di;
		memset(&di, 0, sizeof di);
		di.cfa = bcfa;
		memcpy(di.gain, gain, sizeof gain);
		di.black = r.black[CG] * 64;
		di.white = r.white * 64;
		for (int c = 0; c < 3; c++) di.neutral[c] = gain[CG] / gain[c];
		static const float xyz2srgb[9] = { 3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
						   0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f };
		float inv[9];
		mat3_inv(ccm_typical, inv);
		mat3_mul(inv, xyz2srgb, di.colmat);
		static char uniq[96], desc[160];
		const char *sensor = j->sensor ? j->sensor : "S5KHM6";
		snprintf(uniq, sizeof uniq, "Xiaomi Redmi Note 13 %s", sensor);
		snprintf(desc, sizeof desc, "%s %sraw", sensor,
			 binned_fallback ? "3x3 nona-binned Bayer, " :
			 (r.geo.n == 3 ? "nonacell remosaiced to Bayer, " : ""));
		di.make = "Xiaomi";
		di.model = "Redmi Note 13";
		di.unique = uniq;
		di.software = "sapphire camd 1.0";
		di.desc = desc;
		sinks[ns++] = sink_dng(bayer, j->dng, &di);
	}

	uint8_t *app1_patched = NULL;
	if (binned_fallback && j->app1 && j->app1_len > 0) {
		app1_patched = malloc(j->app1_len);
		if (app1_patched) {
			memcpy(app1_patched, j->app1, j->app1_len);
			patch_exif_dims(app1_patched, j->app1_len, (uint32_t)out_w, (uint32_t)out_h);
		}
	}
	g_jpeg_app1 = app1_patched ? app1_patched : j->app1;
	g_jpeg_app1_len = j->app1_len;
	sinks[ns++] = sink_jpeg(rgb, j->jpeg, j->quality > 0 ? j->quality : 92, tone);
	/* sink_jpeg() has already written the JFIF + APP1 header: drop temporary Exif buffer now */
	g_jpeg_app1 = NULL;
	g_jpeg_app1_len = 0;
	free(app1_patched);
	app1_patched = NULL;

	StageSet stages = { .n = 0 };
	for (int i = 0; i < ns; i++)
		collect_stages(&stages, sinks[i]->src);

	size_t ring = stages_alloc(g_chunk);
	double t0 = now_sec();
	develop_sinks_streaming(sinks, ns, &stages, raw, &r);
	double dt = now_sec() - t0;

	atomic_store(&g_done, true);
	if (pt_ok) pthread_join(pt, NULL);
	atomic_store(&g_src, NULL);

	if (r.fd >= 0) {
		close(r.fd);
		r.fd = -1;
	}
	for (int i = 0; i < stages.n; i++)
		release_stage_ring(stages.list[i]);
	free(tone);
	for (int i = 0; i < ns; i++) {
		free(sinks[i]);
		sinks[i] = NULL;
	}
#ifdef __APPLE__
	if (g_malloc_pressure_relief)
		g_malloc_pressure_relief(NULL, 0);
#endif

	say(g_pfd, "T %dx%d%s cell %d, %d threads, chunk %d, rings %.1f MB, pipeline %.1f s, peak RSS %ld MB\n",
	    out_w, out_h, binned_fallback ? " (3x3 binned fallback)" : "", r.geo.n,
	    pool_threads(), g_chunk, ring / 1048576.0, dt, peak_rss_kb() / 1024);
	stages_report(stderr); /* per-stage times: camd's stderr (/var/tmp/camd.out) */
	say(g_pfd, "P 100\n");
	return 0;
}
