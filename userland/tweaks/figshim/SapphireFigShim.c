/*
 * SapphireFigShim.dylib — injected into /usr/sbin/mediaserverd (CMCapture / MediaToolbox /
 * VideoToolbox) and safe in any process loading VideoToolbox.
 *
 * 1. VideoToolbox Software Fallback Hooks (P1-8):
 *    Snapdragon 685 (SM6225Pro) has no AppleAVE hardware encoder or AppleAVD hardware decoder.
 *    Hooks VTCompressionSessionCreate, VTDecompressionSessionCreate, VTIsHardwareDecodeSupported,
 *    and VTCopyVideoEncoderList dynamically via dlsym + MSHookFunction (no link-time VideoToolbox
 *    or CoreMedia symbol dependencies) so MediaToolbox, AVAssetWriter, ReplayKit, WebRTC, and
 *    camera/screen recording cleanly select Apple's software H.264/HEVC codecs and uncompressed
 *    IOSurface pixel formats instead of failing or crashing probing nonexistent Apple AVE/AVD.
 *
 * 2. CMCapture Runtime Probe (mediaserverd only):
 *    Logs CMCapture / BWFigVideoCaptureDevice / BWFigVideoCaptureStream classes & symbols into
 *    /var/tmp/figshim.log for bridging camd's zero-copy IOSurface frames.
 *
 * Create /var/jb/var/sapphire-figshim-off to disable at next process launch.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIGSHIM_LOG_PATH "/var/tmp/figshim.log"
#define FIGSHIM_OFF_PATH "/var/jb/var/sapphire-figshim-off"

/* VideoToolbox OSStatus error codes (<VideoToolbox/VTErrors.h>) */
enum {
	kFigVTParameterErr           = -12902,
	kFigVTCouldNotFindDecoderErr = -12906,
	kFigVTCouldNotFindEncoderErr = -12908,
};

/* Standard uncompressed CoreVideo pixel format FourCCs (<CoreVideo/CVPixelBuffer.h>) */
enum {
	kFigPixelFormat_420v = 0x34323076u, /* '420v' kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange */
	kFigPixelFormat_420f = 0x34323066u, /* '420f' kCVPixelFormatType_420YpCbCr8BiPlanarFullRange */
	kFigPixelFormat_BGRA = 0x42475241u, /* 'BGRA' kCVPixelFormatType_32BGRA */
};

typedef void (*MSHookFunction_t)(void *symbol, void *replace, void **result);

typedef OSStatus (*VTCompressionSessionCreate_f)(
    CFAllocatorRef allocator,
    int32_t width,
    int32_t height,
    uint32_t codecType,
    CFDictionaryRef encoderSpecification,
    CFDictionaryRef sourceImageBufferAttributes,
    CFAllocatorRef compressedDataAllocator,
    void *outputCallback,
    void *outputCallbackRefCon,
    void **compressionSessionOut);

typedef OSStatus (*VTDecompressionSessionCreate_f)(
    CFAllocatorRef allocator,
    const void *videoFormatDescription,
    CFDictionaryRef videoDecoderSpecification,
    CFDictionaryRef destinationImageBufferAttributes,
    const void *outputCallback,
    void **decompressionSessionOut);

typedef Boolean (*VTIsHardwareDecodeSupported_f)(uint32_t codecType);

typedef OSStatus (*VTCopyVideoEncoderList_f)(
    CFDictionaryRef options,
    CFArrayRef *listOfVideoEncodersOut);

static VTCompressionSessionCreate_f orig_VTCompressionSessionCreate = NULL;
static VTDecompressionSessionCreate_f orig_VTDecompressionSessionCreate = NULL;
static VTIsHardwareDecodeSupported_f orig_VTIsHardwareDecodeSupported = NULL;
static VTCopyVideoEncoderList_f orig_VTCopyVideoEncoderList = NULL;

/* Resolved VideoToolbox CFStringRef keys (with literal CFSTR fallbacks) */
static CFStringRef g_kVTRequireHWEncoder = NULL;
static CFStringRef g_kVTEnableHWEncoder  = NULL;
static CFStringRef g_kVTEncoderID        = NULL;
static CFStringRef g_kVTRequireHWDecoder = NULL;
static CFStringRef g_kVTEnableHWDecoder  = NULL;
static CFStringRef g_kVTListIsHW         = NULL;
static CFStringRef g_kVTListEncoderID    = NULL;

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t g_vt_session_logs = 0;
static volatile uint32_t g_vt_query_logs = 0;

/* ============================================================================
 * Bounded, thread-safe logging
 * ============================================================================ */

static void
fig_log_raw(const char *line, size_t len)
{
	int fd = open(FIGSHIM_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
	if (fd >= 0) {
		(void)write(fd, line, len);
		close(fd);
	}
}

static void
fig_log(const char *msg)
{
	if (msg == NULL) {
		return;
	}
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "%s\n", msg);
	if (n <= 0) {
		return;
	}
	size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
	pthread_mutex_lock(&g_log_mu);
	fig_log_raw(buf, len);
	pthread_mutex_unlock(&g_log_mu);
}

static void __attribute__((format(printf, 1, 2)))
fig_logf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1u, fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	size_t len = (size_t)n < sizeof(buf) - 1u ? (size_t)n : sizeof(buf) - 2u;
	buf[len++] = '\n';
	buf[len] = '\0';
	pthread_mutex_lock(&g_log_mu);
	fig_log_raw(buf, len);
	pthread_mutex_unlock(&g_log_mu);
}

static void __attribute__((format(printf, 3, 4)))
fig_logf_limited(volatile uint32_t *counter, uint32_t limit, const char *fmt, ...)
{
	if (*counter >= limit) {
		return;
	}
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1u, fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	size_t len = (size_t)n < sizeof(buf) - 1u ? (size_t)n : sizeof(buf) - 2u;
	buf[len++] = '\n';
	buf[len] = '\0';
	pthread_mutex_lock(&g_log_mu);
	if (*counter < limit) {
		(*counter)++;
		fig_log_raw(buf, len);
	}
	pthread_mutex_unlock(&g_log_mu);
}

static void
format_fourcc(uint32_t cc, char *out, size_t cap)
{
	uint8_t c0 = (uint8_t)((cc >> 24) & 0xffu);
	uint8_t c1 = (uint8_t)((cc >> 16) & 0xffu);
	uint8_t c2 = (uint8_t)((cc >> 8) & 0xffu);
	uint8_t c3 = (uint8_t)(cc & 0xffu);
	if (c0 >= 0x20u && c0 <= 0x7eu && c1 >= 0x20u && c1 <= 0x7eu &&
	    c2 >= 0x20u && c2 <= 0x7eu && c3 >= 0x20u && c3 <= 0x7eu) {
		snprintf(out, cap, "'%c%c%c%c'", (char)c0, (char)c1, (char)c2, (char)c3);
	} else {
		snprintf(out, cap, "0x%08x", (unsigned int)cc);
	}
}

/* ============================================================================
 * CoreFoundation dictionary & pixel-buffer attribute sanitization
 * ============================================================================ */

static CFStringRef
dlsym_cfstr(void *handle, const char *sym, CFStringRef fallback)
{
	const CFStringRef *p = NULL;
	if (handle != NULL) {
		p = (const CFStringRef *)dlsym(handle, sym);
	}
	if (p == NULL) {
		p = (const CFStringRef *)dlsym(RTLD_DEFAULT, sym);
	}
	if (p != NULL && *p != NULL && CFGetTypeID(*p) == CFStringGetTypeID()) {
		return *p;
	}
	return fallback;
}

static void
dict_remove_key_pair(CFMutableDictionaryRef d, CFStringRef k1, CFStringRef k2)
{
	if (k1 != NULL) {
		CFDictionaryRemoveValue(d, k1);
	}
	if (k2 != NULL && (k1 == NULL || !CFEqual(k1, k2))) {
		CFDictionaryRemoveValue(d, k2);
	}
}

static void
dict_set_false_pair(CFMutableDictionaryRef d, CFStringRef k1, CFStringRef k2)
{
	if (k1 != NULL) {
		CFDictionarySetValue(d, k1, kCFBooleanFalse);
	}
	if (k2 != NULL && (k1 == NULL || !CFEqual(k1, k2))) {
		CFDictionarySetValue(d, k2, kCFBooleanFalse);
	}
}

static bool
cfstr_is_ave_encoder_id(CFTypeRef val)
{
	if (val == NULL || CFGetTypeID(val) != CFStringGetTypeID()) {
		return false;
	}
	CFStringRef s = (CFStringRef)val;
	return CFStringFind(s, CFSTR(".ave."), kCFCompareCaseInsensitive).location != kCFNotFound ||
	       CFStringFind(s, CFSTR("AppleAVE"), kCFCompareCaseInsensitive).location != kCFNotFound;
}

/*
 * Returns a +1 retained CFMutableDictionaryRef forcing software video encoding:
 *   - removes RequireHardwareAcceleratedVideoEncoder
 *   - removes hardware-only ".ave." / "AppleAVE" EncoderID
 *   - sets EnableHardwareAcceleratedVideoEncoder = kCFBooleanFalse
 */
static CFMutableDictionaryRef
sanitize_encoder_spec(CFDictionaryRef spec)
{
	CFMutableDictionaryRef m = NULL;
	if (spec != NULL && CFGetTypeID(spec) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, spec);
	} else {
		m = CFDictionaryCreateMutable(kCFAllocatorDefault, 4,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	if (m == NULL) {
		return NULL;
	}

	dict_remove_key_pair(m, CFSTR("RequireHardwareAcceleratedVideoEncoder"), g_kVTRequireHWEncoder);
	dict_set_false_pair(m, CFSTR("EnableHardwareAcceleratedVideoEncoder"), g_kVTEnableHWEncoder);

	CFTypeRef eid = CFDictionaryGetValue(m, CFSTR("EncoderID"));
	if (eid == NULL && g_kVTEncoderID != NULL) {
		eid = CFDictionaryGetValue(m, g_kVTEncoderID);
	}
	if (cfstr_is_ave_encoder_id(eid)) {
		dict_remove_key_pair(m, CFSTR("EncoderID"), g_kVTEncoderID);
	}
	return m;
}

/*
 * Returns a +1 retained CFMutableDictionaryRef forcing software video decoding:
 *   - removes RequireHardwareAcceleratedVideoDecoder
 *   - sets EnableHardwareAcceleratedVideoDecoder = kCFBooleanFalse
 */
static CFMutableDictionaryRef
sanitize_decoder_spec(CFDictionaryRef spec)
{
	CFMutableDictionaryRef m = NULL;
	if (spec != NULL && CFGetTypeID(spec) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, spec);
	} else {
		m = CFDictionaryCreateMutable(kCFAllocatorDefault, 4,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	if (m == NULL) {
		return NULL;
	}

	dict_remove_key_pair(m, CFSTR("RequireHardwareAcceleratedVideoDecoder"), g_kVTRequireHWDecoder);
	dict_set_false_pair(m, CFSTR("EnableHardwareAcceleratedVideoDecoder"), g_kVTEnableHWDecoder);
	return m;
}

static bool
is_apple_compressed_pixfmt(uint32_t fmt, uint32_t *replacement_out)
{
	/*
	 * Apple AGX / AVE / AVD lossless ('&' = 0x26) and lossy ('~' = 0x7e) compressed
	 * pixel formats ('&8v0', '&8f0', '~8v0', '~8f0', '&BGA', etc.) require Apple GPU/AVE
	 * tile hardware and fail in software VideoToolbox / CoreVideo pools.
	 */
	uint32_t top = (fmt >> 24) & 0xffu;
	if (top != 0x26u && top != 0x7eu) {
		return false;
	}
	if (replacement_out != NULL) {
		uint32_t b2 = (fmt >> 16) & 0xffu;
		uint32_t b1 = (fmt >> 8) & 0xffu;
		uint32_t b0 = fmt & 0xffu;
		if (b2 == 'B' || b1 == 'G') {
			*replacement_out = kFigPixelFormat_BGRA;
		} else if (b1 == 'f' || b0 == 'f') {
			*replacement_out = kFigPixelFormat_420f;
		} else {
			*replacement_out = kFigPixelFormat_420v;
		}
	}
	return true;
}

static bool
cfarray_contains_u32(CFArrayRef arr, uint32_t val)
{
	CFIndex n = CFArrayGetCount(arr);
	for (CFIndex i = 0; i < n; i++) {
		const void *item = CFArrayGetValueAtIndex(arr, i);
		if (item != NULL && CFGetTypeID(item) == CFNumberGetTypeID()) {
			int64_t v = 0;
			if (CFNumberGetValue((CFNumberRef)item, kCFNumberSInt64Type, &v) &&
			    (uint32_t)v == val) {
				return true;
			}
		}
	}
	return false;
}

static bool
iosurface_props_has_agx_compression(CFDictionaryRef d)
{
	return CFDictionaryContainsKey(d, CFSTR("IOSurfaceCompressionType")) ||
	       CFDictionaryContainsKey(d, CFSTR("CompressionType")) ||
	       CFDictionaryContainsKey(d, CFSTR("IOSurfaceCompressedTileWidth")) ||
	       CFDictionaryContainsKey(d, CFSTR("IOSurfaceCompressedTileHeight")) ||
	       CFDictionaryContainsKey(d, CFSTR("CompressedTileWidth")) ||
	       CFDictionaryContainsKey(d, CFSTR("CompressedTileHeight"));
}

/*
 * Sanitizes CVPixelBuffer / IOSurface attribute dictionaries for both
 * VTCompressionSessionCreate (sourceImageBufferAttributes) and
 * VTDecompressionSessionCreate (destinationImageBufferAttributes).
 *
 * Ownership: returns NULL when attrs is NULL/invalid; otherwise returns a +1
 * retained CFDictionaryRef that the caller MUST CFRelease().
 */
static CFDictionaryRef
sanitize_image_buffer_attrs(CFDictionaryRef attrs)
{
	if (attrs == NULL || CFGetTypeID(attrs) != CFDictionaryGetTypeID()) {
		return NULL;
	}

	bool needs_gles_strip =
	    CFDictionaryContainsKey(attrs, CFSTR("OpenGLESCompatibility")) ||
	    CFDictionaryContainsKey(attrs, CFSTR("IOSurfaceOpenGLESTextureCompatibility")) ||
	    CFDictionaryContainsKey(attrs, CFSTR("IOSurfaceOpenGLESFBOCompatibility"));

	bool needs_pixfmt_fix = false;
	CFTypeRef pf = CFDictionaryGetValue(attrs, CFSTR("PixelFormatType"));
	if (pf != NULL) {
		CFTypeID tid = CFGetTypeID(pf);
		if (tid == CFNumberGetTypeID()) {
			int64_t v = 0;
			if (CFNumberGetValue((CFNumberRef)pf, kCFNumberSInt64Type, &v) &&
			    is_apple_compressed_pixfmt((uint32_t)v, NULL)) {
				needs_pixfmt_fix = true;
			}
		} else if (tid == CFArrayGetTypeID()) {
			CFArrayRef arr = (CFArrayRef)pf;
			CFIndex cnt = CFArrayGetCount(arr);
			for (CFIndex i = 0; i < cnt; i++) {
				const void *item = CFArrayGetValueAtIndex(arr, i);
				if (item != NULL && CFGetTypeID(item) == CFNumberGetTypeID()) {
					int64_t v = 0;
					if (CFNumberGetValue((CFNumberRef)item, kCFNumberSInt64Type, &v) &&
					    is_apple_compressed_pixfmt((uint32_t)v, NULL)) {
						needs_pixfmt_fix = true;
						break;
					}
				}
			}
		}
	}

	bool needs_iosurf_fix = false;
	CFTypeRef ios_props = CFDictionaryGetValue(attrs, CFSTR("IOSurfaceProperties"));
	if (ios_props != NULL && CFGetTypeID(ios_props) == CFDictionaryGetTypeID() &&
	    iosurface_props_has_agx_compression((CFDictionaryRef)ios_props)) {
		needs_iosurf_fix = true;
	}

	if (!needs_gles_strip && !needs_pixfmt_fix && !needs_iosurf_fix) {
		return (CFDictionaryRef)CFRetain(attrs);
	}

	CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, attrs);
	if (m == NULL) {
		return (CFDictionaryRef)CFRetain(attrs);
	}

	if (needs_gles_strip) {
		CFDictionaryRemoveValue(m, CFSTR("OpenGLESCompatibility"));
		CFDictionaryRemoveValue(m, CFSTR("IOSurfaceOpenGLESTextureCompatibility"));
		CFDictionaryRemoveValue(m, CFSTR("IOSurfaceOpenGLESFBOCompatibility"));
	}

	if (needs_pixfmt_fix && pf != NULL) {
		CFTypeID tid = CFGetTypeID(pf);
		if (tid == CFNumberGetTypeID()) {
			int64_t v = 0;
			uint32_t rep = kFigPixelFormat_420v;
			if (CFNumberGetValue((CFNumberRef)pf, kCFNumberSInt64Type, &v) &&
			    is_apple_compressed_pixfmt((uint32_t)v, &rep)) {
				SInt32 s32 = (SInt32)rep;
				CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &s32);
				if (num != NULL) {
					CFDictionarySetValue(m, CFSTR("PixelFormatType"), num);
					CFRelease(num);
				}
			}
		} else if (tid == CFArrayGetTypeID()) {
			CFArrayRef arr = (CFArrayRef)pf;
			CFIndex cnt = CFArrayGetCount(arr);
			CFMutableArrayRef new_arr = CFArrayCreateMutable(
			    kCFAllocatorDefault, cnt > 0 ? cnt : 1, &kCFTypeArrayCallBacks);
			if (new_arr != NULL) {
				for (CFIndex i = 0; i < cnt; i++) {
					const void *item = CFArrayGetValueAtIndex(arr, i);
					if (item == NULL || CFGetTypeID(item) != CFNumberGetTypeID()) {
						continue;
					}
					int64_t v = 0;
					if (!CFNumberGetValue((CFNumberRef)item, kCFNumberSInt64Type, &v)) {
						continue;
					}
					uint32_t fmt = (uint32_t)v;
					uint32_t rep = kFigPixelFormat_420v;
					if (is_apple_compressed_pixfmt(fmt, &rep)) {
						fmt = rep;
					}
					if (!cfarray_contains_u32(new_arr, fmt)) {
						SInt32 s32 = (SInt32)fmt;
						CFNumberRef num = CFNumberCreate(
						    kCFAllocatorDefault, kCFNumberSInt32Type, &s32);
						if (num != NULL) {
							CFArrayAppendValue(new_arr, num);
							CFRelease(num);
						}
					}
				}
				if (CFArrayGetCount(new_arr) == 0) {
					SInt32 s32 = (SInt32)kFigPixelFormat_420v;
					CFNumberRef num = CFNumberCreate(
					    kCFAllocatorDefault, kCFNumberSInt32Type, &s32);
					if (num != NULL) {
						CFArrayAppendValue(new_arr, num);
						CFRelease(num);
					}
				}
				CFDictionarySetValue(m, CFSTR("PixelFormatType"), new_arr);
				CFRelease(new_arr);
			}
		}
	}

	if (needs_iosurf_fix && ios_props != NULL) {
		CFMutableDictionaryRef sub_m = CFDictionaryCreateMutableCopy(
		    kCFAllocatorDefault, 0, (CFDictionaryRef)ios_props);
		if (sub_m != NULL) {
			CFDictionaryRemoveValue(sub_m, CFSTR("IOSurfaceCompressionType"));
			CFDictionaryRemoveValue(sub_m, CFSTR("CompressionType"));
			CFDictionaryRemoveValue(sub_m, CFSTR("IOSurfaceCompressedTileWidth"));
			CFDictionaryRemoveValue(sub_m, CFSTR("IOSurfaceCompressedTileHeight"));
			CFDictionaryRemoveValue(sub_m, CFSTR("CompressedTileWidth"));
			CFDictionaryRemoveValue(sub_m, CFSTR("CompressedTileHeight"));
			CFDictionarySetValue(m, CFSTR("IOSurfaceProperties"), sub_m);
			CFRelease(sub_m);
		}
	}

	return (CFDictionaryRef)m;
}

/* ============================================================================
 * VideoToolbox Hook Implementations
 * ============================================================================ */

static OSStatus
hook_VTCompressionSessionCreate(
    CFAllocatorRef allocator,
    int32_t width,
    int32_t height,
    uint32_t codecType,
    CFDictionaryRef encoderSpecification,
    CFDictionaryRef sourceImageBufferAttributes,
    CFAllocatorRef compressedDataAllocator,
    void *outputCallback,
    void *outputCallbackRefCon,
    void **compressionSessionOut)
{
	if (compressionSessionOut == NULL) {
		return kFigVTParameterErr;
	}
	if (orig_VTCompressionSessionCreate == NULL) {
		*compressionSessionOut = NULL;
		return kFigVTCouldNotFindEncoderErr;
	}

	CFMutableDictionaryRef clean_spec = sanitize_encoder_spec(encoderSpecification);
	CFDictionaryRef clean_attrs = sanitize_image_buffer_attrs(sourceImageBufferAttributes);

	OSStatus status = orig_VTCompressionSessionCreate(
	    allocator,
	    width,
	    height,
	    codecType,
	    clean_spec != NULL ? (CFDictionaryRef)clean_spec : encoderSpecification,
	    clean_attrs,
	    compressedDataAllocator,
	    outputCallback,
	    outputCallbackRefCon,
	    compressionSessionOut);

	/*
	 * If extra caller-supplied private AVE keys in encoderSpecification caused encoder
	 * lookup to fail (-12908), retry once with a minimal software-only specification.
	 */
	if (status == kFigVTCouldNotFindEncoderErr &&
	    clean_spec != NULL && CFDictionaryGetCount(clean_spec) > 1) {
		CFMutableDictionaryRef min_spec = CFDictionaryCreateMutable(
		    kCFAllocatorDefault, 2,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (min_spec != NULL) {
			dict_set_false_pair(min_spec,
			    CFSTR("EnableHardwareAcceleratedVideoEncoder"), g_kVTEnableHWEncoder);
			*compressionSessionOut = NULL;
			status = orig_VTCompressionSessionCreate(
			    allocator,
			    width,
			    height,
			    codecType,
			    (CFDictionaryRef)min_spec,
			    clean_attrs,
			    compressedDataAllocator,
			    outputCallback,
			    outputCallbackRefCon,
			    compressionSessionOut);
			CFRelease(min_spec);
		}
	}

	if (clean_spec != NULL) {
		CFRelease(clean_spec);
	}
	if (clean_attrs != NULL) {
		CFRelease(clean_attrs);
	}

	if (g_vt_session_logs < 64u) {
		char cc[16];
		format_fourcc(codecType, cc, sizeof(cc));
		fig_logf_limited(&g_vt_session_logs, 64u,
		    "[figshim] VTCompressionSessionCreate(%dx%d, codec=%s, spec=%p, srcAttrs=%p) -> %d (session=%p)",
		    (int)width, (int)height, cc, (const void *)encoderSpecification,
		    (const void *)sourceImageBufferAttributes, (int)status,
		    *compressionSessionOut);
	}

	return status;
}

static OSStatus
hook_VTDecompressionSessionCreate(
    CFAllocatorRef allocator,
    const void *videoFormatDescription,
    CFDictionaryRef videoDecoderSpecification,
    CFDictionaryRef destinationImageBufferAttributes,
    const void *outputCallback,
    void **decompressionSessionOut)
{
	if (decompressionSessionOut == NULL) {
		return kFigVTParameterErr;
	}
	if (orig_VTDecompressionSessionCreate == NULL) {
		*decompressionSessionOut = NULL;
		return kFigVTCouldNotFindDecoderErr;
	}

	CFMutableDictionaryRef clean_spec = sanitize_decoder_spec(videoDecoderSpecification);
	CFDictionaryRef clean_attrs = sanitize_image_buffer_attrs(destinationImageBufferAttributes);

	OSStatus status = orig_VTDecompressionSessionCreate(
	    allocator,
	    videoFormatDescription,
	    clean_spec != NULL ? (CFDictionaryRef)clean_spec : videoDecoderSpecification,
	    clean_attrs,
	    outputCallback,
	    decompressionSessionOut);

	if (status == kFigVTCouldNotFindDecoderErr &&
	    clean_spec != NULL && CFDictionaryGetCount(clean_spec) > 1) {
		CFMutableDictionaryRef min_spec = CFDictionaryCreateMutable(
		    kCFAllocatorDefault, 2,
		    &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		if (min_spec != NULL) {
			dict_set_false_pair(min_spec,
			    CFSTR("EnableHardwareAcceleratedVideoDecoder"), g_kVTEnableHWDecoder);
			*decompressionSessionOut = NULL;
			status = orig_VTDecompressionSessionCreate(
			    allocator,
			    videoFormatDescription,
			    (CFDictionaryRef)min_spec,
			    clean_attrs,
			    outputCallback,
			    decompressionSessionOut);
			CFRelease(min_spec);
		}
	}

	if (clean_spec != NULL) {
		CFRelease(clean_spec);
	}
	if (clean_attrs != NULL) {
		CFRelease(clean_attrs);
	}

	if (g_vt_session_logs < 64u) {
		fig_logf_limited(&g_vt_session_logs, 64u,
		    "[figshim] VTDecompressionSessionCreate(fmt=%p, spec=%p, dstAttrs=%p) -> %d (session=%p)",
		    videoFormatDescription, (const void *)videoDecoderSpecification,
		    (const void *)destinationImageBufferAttributes, (int)status,
		    *decompressionSessionOut);
	}

	return status;
}

static Boolean
hook_VTIsHardwareDecodeSupported(uint32_t codecType)
{
	(void)orig_VTIsHardwareDecodeSupported;
	if (g_vt_query_logs < 8u) {
		char cc[16];
		format_fourcc(codecType, cc, sizeof(cc));
		fig_logf_limited(&g_vt_query_logs, 8u,
		    "[figshim] VTIsHardwareDecodeSupported(%s) -> false", cc);
	}
	return (Boolean)0;
}

static bool
is_hw_encoder_entry(CFDictionaryRef entry)
{
	if (entry == NULL || CFGetTypeID(entry) != CFDictionaryGetTypeID()) {
		return false;
	}
	CFTypeRef hw = g_kVTListIsHW != NULL ? CFDictionaryGetValue(entry, g_kVTListIsHW) : NULL;
	if (hw == NULL) {
		hw = CFDictionaryGetValue(entry, CFSTR("IsHardwareAccelerated"));
	}
	if (hw != NULL && CFGetTypeID(hw) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)hw)) {
		return true;
	}
	CFTypeRef eid = g_kVTListEncoderID != NULL ? CFDictionaryGetValue(entry, g_kVTListEncoderID) : NULL;
	if (eid == NULL) {
		eid = CFDictionaryGetValue(entry, CFSTR("EncoderID"));
	}
	return cfstr_is_ave_encoder_id(eid);
}

static OSStatus
hook_VTCopyVideoEncoderList(CFDictionaryRef options, CFArrayRef *listOfVideoEncodersOut)
{
	if (listOfVideoEncodersOut == NULL) {
		return kFigVTParameterErr;
	}
	if (orig_VTCopyVideoEncoderList == NULL) {
		*listOfVideoEncodersOut = NULL;
		return kFigVTCouldNotFindEncoderErr;
	}

	OSStatus status = orig_VTCopyVideoEncoderList(options, listOfVideoEncodersOut);
	if (status != 0 || *listOfVideoEncodersOut == NULL ||
	    CFGetTypeID(*listOfVideoEncodersOut) != CFArrayGetTypeID()) {
		return status;
	}

	CFArrayRef orig_list = *listOfVideoEncodersOut;
	CFIndex count = CFArrayGetCount(orig_list);
	if (count <= 0) {
		return status;
	}

	bool any_hw = false;
	for (CFIndex i = 0; i < count; i++) {
		const void *val = CFArrayGetValueAtIndex(orig_list, i);
		if (val != NULL && is_hw_encoder_entry((CFDictionaryRef)val)) {
			any_hw = true;
			break;
		}
	}
	if (!any_hw) {
		return status;
	}

	CFMutableArrayRef sw_only = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
	if (sw_only == NULL) {
		return status;
	}
	for (CFIndex i = 0; i < count; i++) {
		const void *val = CFArrayGetValueAtIndex(orig_list, i);
		if (val != NULL && !is_hw_encoder_entry((CFDictionaryRef)val)) {
			CFArrayAppendValue(sw_only, val);
		}
	}

	if (CFArrayGetCount(sw_only) == 0) {
		/*
		 * Fallback: if VideoToolbox only listed AVE-tagged entries, keep them with
		 * IsHardwareAccelerated=false so callers still discover H.264/HEVC support;
		 * hook_VTCompressionSessionCreate strips any ".ave." EncoderID on create.
		 */
		for (CFIndex i = 0; i < count; i++) {
			const void *val = CFArrayGetValueAtIndex(orig_list, i);
			if (val != NULL && CFGetTypeID(val) == CFDictionaryGetTypeID()) {
				CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(
				    kCFAllocatorDefault, 0, (CFDictionaryRef)val);
				if (m != NULL) {
					dict_set_false_pair(m, CFSTR("IsHardwareAccelerated"), g_kVTListIsHW);
					CFArrayAppendValue(sw_only, m);
					CFRelease(m);
				}
			}
		}
	}

	if (CFArrayGetCount(sw_only) > 0) {
		*listOfVideoEncodersOut = sw_only;
		CFRelease(orig_list);
		fig_logf_limited(&g_vt_query_logs, 8u,
		    "[figshim] VTCopyVideoEncoderList -> %d (sanitized %ld -> %ld encoders)",
		    (int)status, (long)count, (long)CFArrayGetCount(*listOfVideoEncodersOut));
	} else {
		CFRelease(sw_only);
	}
	return status;
}

/* ============================================================================
 * Safe ARM64 Prologue Check & ElleKit / Substrate Hooking
 * ============================================================================ */

static MSHookFunction_t
resolve_mshook(void)
{
	MSHookFunction_t fn = (MSHookFunction_t)dlsym(RTLD_DEFAULT, "MSHookFunction");
	const char *libs[] = {
		"/var/jb/usr/lib/libellekit.dylib",
		"/var/jb/usr/lib/libsubstrate.dylib",
		"/var/jb/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
	};
	for (size_t i = 0; fn == NULL && i < sizeof(libs) / sizeof(libs[0]); i++) {
		void *h = dlopen(libs[i], RTLD_LAZY | RTLD_NOLOAD);
		if (h == NULL) {
			h = dlopen(libs[i], RTLD_LAZY);
		}
		if (h != NULL) {
			fn = (MSHookFunction_t)dlsym(h, "MSHookFunction");
		}
	}
	return fn;
}

/*
 * Verify a target function is >= 4 instructions (16 bytes) before calling MSHookFunction
 * so ElleKit always uses a 4-insn branch trampoline and never plants a BRK exception trap
 * (which crashes daemons without an exception handler with EXC_BREAKPOINT).
 * Follows pure 1-instruction unconditional tail-call stubs ("b target") with identical ABI.
 */
static void *
resolve_hookable_target(void *sym, const char *name)
{
	if (sym == NULL) {
		return NULL;
	}
	uintptr_t addr = (uintptr_t)sym & 0x0000ffffffffffffULL;
	if (addr == 0 || (addr & 3u) != 0) {
		return NULL;
	}
	const uint32_t *insn = (const uint32_t *)addr;

	for (int hop = 0; hop < 2; hop++) {
		uint32_t op0 = insn[0];
		if ((op0 & 0xfc000000u) == 0x14000000u) {
			int32_t imm26 = ((int32_t)(op0 << 6)) >> 6;
			insn = (const uint32_t *)((const uint8_t *)insn + (intptr_t)imm26 * 4);
		} else {
			break;
		}
	}

	int max_reach = 0;
	for (int i = 0; i < 3; i++) {
		uint32_t op = insn[i];
		if ((op & 0xff000010u) == 0x54000000u || /* B.cond */
		    (op & 0x7e000000u) == 0x34000000u) { /* CBZ / CBNZ */
			int32_t imm19 = ((int32_t)(op << 8)) >> 13;
			if (imm19 > 0 && i + imm19 > max_reach) {
				max_reach = i + imm19;
			}
		} else if ((op & 0x7e000000u) == 0x36000000u) { /* TBZ / TBNZ */
			int32_t imm14 = ((int32_t)(op << 13)) >> 18;
			if (imm14 > 0 && i + imm14 > max_reach) {
				max_reach = i + imm14;
			}
		}
		bool is_ret = (op == 0xd65f03c0u || op == 0xd65f0bffu || op == 0xd65f0fffu);
		bool is_b   = ((op & 0xfc000000u) == 0x14000000u);
		bool is_br  = ((op & 0xfefff000u) == 0xd61f0000u);
		bool is_brk = ((op & 0xffe0001fu) == 0xd4200000u);
		if (is_brk || ((is_ret || is_b || is_br) && max_reach <= i)) {
			fig_logf("[figshim] %s at %p too short or already trapped (insn[%d]=0x%08x), skipping hook",
			    name, (const void *)insn, i, (unsigned int)op);
			return NULL;
		}
	}
	return (void *)insn;
}

static void
install_videotoolbox_hooks(void)
{
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox",
	    RTLD_LAZY | RTLD_NOLOAD);
	if (vt == NULL) {
		vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_LAZY);
	}

	g_kVTRequireHWEncoder = dlsym_cfstr(vt,
	    "kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder",
	    CFSTR("RequireHardwareAcceleratedVideoEncoder"));
	g_kVTEnableHWEncoder = dlsym_cfstr(vt,
	    "kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder",
	    CFSTR("EnableHardwareAcceleratedVideoEncoder"));
	g_kVTEncoderID = dlsym_cfstr(vt,
	    "kVTVideoEncoderSpecification_EncoderID",
	    CFSTR("EncoderID"));
	g_kVTRequireHWDecoder = dlsym_cfstr(vt,
	    "kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder",
	    CFSTR("RequireHardwareAcceleratedVideoDecoder"));
	g_kVTEnableHWDecoder = dlsym_cfstr(vt,
	    "kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder",
	    CFSTR("EnableHardwareAcceleratedVideoDecoder"));
	g_kVTListIsHW = dlsym_cfstr(vt,
	    "kVTVideoEncoderList_IsHardwareAccelerated",
	    CFSTR("IsHardwareAccelerated"));
	g_kVTListEncoderID = dlsym_cfstr(vt,
	    "kVTVideoEncoderList_EncoderID",
	    CFSTR("EncoderID"));

	MSHookFunction_t mshook = resolve_mshook();
	if (mshook == NULL) {
		fig_log("[figshim] MSHookFunction unavailable; VideoToolbox hooks skipped");
		return;
	}

	struct {
		const char *name;
		void *replace;
		void **orig;
	} hooks[] = {
		{
			"VTCompressionSessionCreate",
			(void *)hook_VTCompressionSessionCreate,
			(void **)&orig_VTCompressionSessionCreate,
		},
		{
			"VTDecompressionSessionCreate",
			(void *)hook_VTDecompressionSessionCreate,
			(void **)&orig_VTDecompressionSessionCreate,
		},
		{
			"VTIsHardwareDecodeSupported",
			(void *)hook_VTIsHardwareDecodeSupported,
			(void **)&orig_VTIsHardwareDecodeSupported,
		},
		{
			"VTCopyVideoEncoderList",
			(void *)hook_VTCopyVideoEncoderList,
			(void **)&orig_VTCopyVideoEncoderList,
		},
	};

	for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		void *sym = vt != NULL ? dlsym(vt, hooks[i].name) : NULL;
		if (sym == NULL) {
			sym = dlsym(RTLD_DEFAULT, hooks[i].name);
		}
		void *target = resolve_hookable_target(sym, hooks[i].name);
		if (target == NULL) {
			continue;
		}
		mshook(target, hooks[i].replace, hooks[i].orig);
		if (*hooks[i].orig != NULL) {
			fig_logf("[figshim] hooked %s at %p (orig=%p)",
			    hooks[i].name, target, *hooks[i].orig);
		} else {
			fig_logf("[figshim] failed to hook %s at %p",
			    hooks[i].name, target);
		}
	}
}

/* ============================================================================
 * CMCapture Runtime Probe (mediaserverd)
 * ============================================================================ */

static void
dump_class_methods(const char *clsName)
{
	Class c = objc_getClass(clsName);
	if (c == Nil) {
		fig_logf("[figshim] class %s: NOT FOUND", clsName);
		return;
	}
	unsigned int cnt = 0;
	Method *ms = class_copyMethodList(c, &cnt);
	fig_logf("[figshim] class %s: FOUND (%u methods)", clsName, cnt);
	for (unsigned int i = 0; i < cnt && i < 40; i++) {
		const char *sname = sel_getName(method_getName(ms[i]));
		if (sname && (strstr(sname, "start") || strstr(sname, "stop") || strstr(sname, "Property") ||
		    strstr(sname, "Sample") || strstr(sname, "Buffer") || strstr(sname, "Stream") ||
		    strstr(sname, "Source") || strstr(sname, "init") || strstr(sname, "Device"))) {
			fig_logf("  - %s", sname);
		}
	}
	if (ms) free(ms);
}

__attribute__((constructor)) static void
figshim_init(void)
{
	if (access(FIGSHIM_OFF_PATH, F_OK) == 0) {
		return;
	}
	const char *prog = getprogname();
	bool is_mediaserverd = (prog != NULL && strcmp(prog, "mediaserverd") == 0);
	fig_logf("[figshim] loaded in %s (pid=%d)", prog != NULL ? prog : "?", (int)getpid());

	install_videotoolbox_hooks();

	if (is_mediaserverd) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			dump_class_methods("BWFigVideoCaptureDevice");
			dump_class_methods("BWFigVideoCaptureStream");
			dump_class_methods("BWFigCaptureDevice");
			dump_class_methods("BWFigCaptureStream");
			dump_class_methods("BWFigCaptureDeviceClient");
			dump_class_methods("FigCaptureSourceCompanionFormat");
			dump_class_methods("BWStillImageCoordinatorNode");
			dump_class_methods("BWMovieFileSinkNode");

			void *sym1 = dlsym(RTLD_DEFAULT, "FigCaptureSourceRemoteCopyCaptureSources");
			void *sym2 = dlsym(RTLD_DEFAULT, "FigCaptureSourceCreate");
			void *sym3 = dlsym(RTLD_DEFAULT, "FigCaptureServerStart");
			fig_logf("[figshim] syms: RemoteCopy=%p SourceCreate=%p ServerStart=%p",
			    sym1, sym2, sym3);
		});
	}
}
