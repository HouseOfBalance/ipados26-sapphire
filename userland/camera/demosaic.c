/* Bayer demosaic: bilinear and Malvar-He-Cutler (gradient-corrected linear, 5x5) */
#include "remosaic.h"

/* Whole-sample symmetric reflection across boundary pixels 0 and n - 1.
 * Every reflection step (-i or 2*(n - 1) - i) shifts i by an even integer,
 * preserving exact Bayer CFA parity (i & 1) for all n >= 2 (both even and odd n)
 * while keeping every read strictly inside [0, n - 1] (zero out-of-bounds reads
 * even without horizontal ring padding or when bayer->period == 1).
 * Unlike periodic phase_clamp (which maps -2 -> 0 and n + 1 -> n - 1), symmetric
 * reflection maps -2 -> 2 and n + 1 -> n - 3, eliminating 5-tap Laplacian border
 * ringing and dark/false-colour 1-2px borders in Malvar-He-Cutler demosaic. */
static inline int bayer_reflect(int i, int n)
{
	if (n <= 1) return 0;
	if (i < 0) i = -i;
	if (i >= n) i = 2 * (n - 1) - i;
	if (i < 0) i = -i;
	return i < n ? i : n - 1;
}

static inline void bil_px(const Cfa *cf, const float *pm, const float *p0, const float *pp,
			  int x, int y, int xm1, int xp1, float *out)
{
	int c = cf->c[y & 1][x & 1];
	float C = p0[x], rgb[3];
	if (c == CG) {
		int hc = cf->c[y & 1][(x + 1) & 1];
		rgb[CG] = C;
		rgb[hc] = 0.5f * (p0[xm1] + p0[xp1]);
		rgb[2 - hc] = 0.5f * (pm[x] + pp[x]);
	} else {
		rgb[c] = C;
		rgb[CG] = 0.25f * (p0[xm1] + p0[xp1] + pm[x] + pp[x]);
		rgb[2 - c] = 0.25f * (pm[xm1] + pm[xp1] + pp[xm1] + pp[xp1]);
	}
	out[0] = rgb[0];
	out[1] = rgb[1];
	out[2] = rgb[2];
}

static void bil_rows(Stage *s, int y0, int y1)
{
	const Cfa *cf = s->p;
	Stage *in = s->in[0];
	const int w = s->w, h = s->h;
	if (w <= 0 || h <= 0) return;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	for (int y = y0; y < y1; y++) {
		const int ym1 = (y > 0) ? (y - 1) : bayer_reflect(-1, h);
		const int yp1 = (y < h - 1) ? (y + 1) : bayer_reflect(h, h);
		const float *pm = srow(in, ym1), *p0 = srow(in, y), *pp = srow(in, yp1);
		float *o = srow_w(s, y);

		/* Left boundary pixel (x = 0) */
		bil_px(cf, pm, p0, pp, 0, y, bayer_reflect(-1, w), bayer_reflect(1, w), o);

		/* Fast unbranched interior loop: x = 1 .. w - 2 */
		for (int x = 1; x <= w - 2; x++) {
			bil_px(cf, pm, p0, pp, x, y, x - 1, x + 1, o + 3 * x);
		}

		/* Right boundary pixel (x = w - 1) */
		if (w > 1) {
			bil_px(cf, pm, p0, pp, w - 1, y, bayer_reflect(w - 2, w), bayer_reflect(w, w), o + 3 * (w - 1));
		}
	}
}

static inline void mhc_px(const Cfa *cf, const float *m2, const float *m1, const float *p0,
			  const float *p1, const float *p2, int x, int y,
			  int xm2, int xm1, int xp1, int xp2, float *out)
{
	int c = cf->c[y & 1][x & 1];
	float C = p0[x], rgb[3];
	float H1 = p0[xm1] + p0[xp1], V1 = m1[x] + p1[x];
	float H2 = p0[xm2] + p0[xp2], V2 = m2[x] + p2[x];
	float D = m1[xm1] + m1[xp1] + p1[xm1] + p1[xp1];
	if (c == CG) {
		int hc = cf->c[y & 1][(x + 1) & 1];
		rgb[CG] = C;
		rgb[hc] = (5.0f * C + 4.0f * H1 - H2 - D + 0.5f * V2) * 0.125f;
		rgb[2 - hc] = (5.0f * C + 4.0f * V1 - V2 - D + 0.5f * H2) * 0.125f;
	} else {
		rgb[c] = C;
		rgb[CG] = (4.0f * C + 2.0f * (H1 + V1) - (H2 + V2)) * 0.125f;
		rgb[2 - c] = (6.0f * C + 2.0f * D - 1.5f * (H2 + V2)) * 0.125f;
	}
	out[0] = rgb[0];
	out[1] = rgb[1];
	out[2] = rgb[2];
}

static void mhc_rows(Stage *s, int y0, int y1)
{
	const Cfa *cf = s->p;
	Stage *in = s->in[0];
	const int w = s->w, h = s->h;
	if (w <= 0 || h <= 0) return;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	for (int y = y0; y < y1; y++) {
		const int ym2 = (y >= 2) ? (y - 2) : bayer_reflect(y - 2, h);
		const int ym1 = (y >= 1) ? (y - 1) : bayer_reflect(y - 1, h);
		const int yp1 = (y <= h - 2) ? (y + 1) : bayer_reflect(y + 1, h);
		const int yp2 = (y <= h - 3) ? (y + 2) : bayer_reflect(y + 2, h);
		const float *m2 = srow(in, ym2), *m1 = srow(in, ym1), *p0 = srow(in, y),
			    *p1 = srow(in, yp1), *p2 = srow(in, yp2);
		float *o = srow_w(s, y);

		if (w <= 4) {
			for (int x = 0; x < w; x++) {
				mhc_px(cf, m2, m1, p0, p1, p2, x, y,
				       bayer_reflect(x - 2, w), bayer_reflect(x - 1, w),
				       bayer_reflect(x + 1, w), bayer_reflect(x + 2, w), o + 3 * x);
			}
			continue;
		}

		/* Left boundary columns (x = 0, 1) */
		mhc_px(cf, m2, m1, p0, p1, p2, 0, y,
		       bayer_reflect(-2, w), bayer_reflect(-1, w), 1, 2, o);
		mhc_px(cf, m2, m1, p0, p1, p2, 1, y,
		       bayer_reflect(-1, w), 0, 2, 3, o + 3);

		/* Fast unbranched interior loop: x = 2 .. w - 3 */
		for (int x = 2; x <= w - 3; x++) {
			mhc_px(cf, m2, m1, p0, p1, p2, x, y, x - 2, x - 1, x + 1, x + 2, o + 3 * x);
		}

		/* Right boundary columns (x = w - 2, w - 1) */
		mhc_px(cf, m2, m1, p0, p1, p2, w - 2, y,
		       w - 4, w - 3, w - 1, bayer_reflect(w, w), o + 3 * (w - 2));
		mhc_px(cf, m2, m1, p0, p1, p2, w - 1, y,
		       w - 3, w - 2, bayer_reflect(w, w), bayer_reflect(w + 1, w), o + 3 * (w - 1));
	}
}

static Stage *mk(const char *name, Stage *bayer, const Cfa *cfa, stage_fn fn, int ctx)
{
	Cfa *c = xmalloc(sizeof *c);
	*c = *cfa;
	Stage *s = stage_new(name, bayer->w, bayer->h, 3, 1, fn, c);
	stage_add_input(s, bayer);
	stage_set_map(s, 1, ctx);
	return s;
}

Stage *demosaic_bilinear(Stage *bayer, const Cfa *cfa) { return mk("bilinear", bayer, cfa, bil_rows, 1); }
Stage *demosaic_mhc(Stage *bayer, const Cfa *cfa) { return mk("mhc", bayer, cfa, mhc_rows, 2); }
