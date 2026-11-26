/*
 * SapphireMetal: a Metal driver for the Adreno 610, step 0 — a device that tells us what iOS asks
 * of one.
 *
 * Metal finds its driver through the accelerator service in the IORegistry
 * (getMetalPluginClassForService): MetalPluginName names a bundle in /System/Library/Extensions,
 * MetalPluginClassName a class in it, which must be a subclass of _MTLDevice, and the device is
 * [[class alloc] initWithAcceleratorPort:port]. _MTLDevice's own initWithAcceleratorPort: is
 * abstract; Apple's IOGPUMetalDevice does [super init] and then opens its kernel interface. This
 * one does [super init] only.
 *
 * Every instance method of the MTLDeviceSPI protocol (which takes in MTLDevice) is routed through
 * forwardInvocation:, which writes the selector to stderr (the kernel console shows it, prefixed
 * with the process name), then hands the call to _MTLDevice when it has an implementation, and
 * otherwise answers zero. Abstract methods end in doesNotRecognizeSelector:, which logs and
 * answers zero instead of raising. The result is the list, in order, of what CoreAnimation and
 * the rest of iOS want from a Metal device, which is what the real driver has to implement.
 *
 * Built on Linux: clang -target arm64-apple-ios16.0, ld64.lld against the stubs in ../tbd, ad-hoc
 * signed (see build.sh). No Apple headers: what is needed is declared here.
 */
/* Darwin's libc, declared here (Linux's headers do not describe it) */
typedef unsigned long size_t;
int sgpu_selftest(unsigned int service);   /* sgpu_selftest.c */
void *dlopen(const char *, int);
char *dlerror(void);
const char *getprogname(void);
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
char *strstr(const char *, const char *);
unsigned int IOServiceGetMatchingService(unsigned int, void *);
void *IOServiceMatching(const char *);
int IOObjectRelease(unsigned int);
int usleep(unsigned int);
typedef struct __sFILE FILE;
extern FILE *__stderrp;
#define stderr __stderrp
#define NULL ((void *)0)
int fprintf(FILE *, const char *, ...) __attribute__((format(printf, 2, 3)));
int fputc(int, FILE *);
int getpid(void);
void *calloc(size_t, size_t);
void free(void *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strstr(const char *, const char *);

typedef struct objc_method *Method;
typedef struct objc_protocol_def Protocol_t;
struct objc_method_description {
	SEL name;
	const char *types;
};
typedef void (*IMP_t)(void);

extern id objc_msgSend(id, SEL, ...);
struct objc_super {
	id receiver;
	Class super_class;
};
extern id objc_msgSendSuper(struct objc_super *, SEL, ...);
extern Class objc_getClass(const char *);
extern void *objc_getProtocol(const char *);
extern struct objc_method_description *protocol_copyMethodDescriptionList(void *p, int required, int instance,
    unsigned int *count);
extern void **protocol_copyProtocolList(void *p, unsigned int *count);
extern const char *protocol_getName(void *p);
extern int class_addMethod(Class cls, SEL name, IMP_t imp, const char *types);
extern int class_addProtocol(Class cls, void *protocol);
extern Class objc_allocateClassPair(Class superclass, const char *name, size_t extra);
extern void objc_registerClassPair(Class cls);
extern const char *class_getName(Class cls);
extern Class object_getClass(id obj);
int snprintf(char *, size_t, const char *, ...);
extern Method class_getInstanceMethod(Class cls, SEL name);
extern IMP_t method_getImplementation(Method m);
extern Class class_getSuperclass(Class cls);
extern const char *sel_getName(SEL sel);
extern SEL sel_registerName(const char *str);
extern void _objc_msgForward(void);

__attribute__((objc_root_class))
@interface NSObject
@end

@interface _MTLDevice : NSObject
- (id)init;
@end

@interface SapphireMTLDevice : _MTLDevice
@end

static unsigned int sapphire_port;
static id sapphire_device;
static int sapphire_logged;

/* one fprintf per line: the console splits a line written in several pieces */
#define SLOG(fmt, ...) fprintf(stderr, "SAPPHIRE MTL[%d]: " fmt "\n", getpid(), ##__VA_ARGS__)

static id
msg0(id obj, const char *sel)
{
	return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel));
}

static void
route_protocol(Class cls, void *proto, int depth)
{
	unsigned int n = 0;

	for (int required = 1; required >= 0; required--) {
		struct objc_method_description *m = protocol_copyMethodDescriptionList(proto, required, 1, &n);
		for (unsigned int i = 0; i < n; i++) {
			const char *name = sel_getName(m[i].name);
			if (strcmp(name, "initWithAcceleratorPort:") == 0 || strncmp(name, "init", 4) == 0 ||
			    strcmp(name, "dealloc") == 0 || strcmp(name, "retain") == 0 || strcmp(name, "release") == 0 ||
			    strcmp(name, "autorelease") == 0 || strcmp(name, "retainCount") == 0 ||
			    strcmp(name, "respondsToSelector:") == 0 || strcmp(name, "conformsToProtocol:") == 0 ||
			    strcmp(name, "isKindOfClass:") == 0 || strcmp(name, "class") == 0 || strcmp(name, "hash") == 0 ||
			    strcmp(name, "isEqual:") == 0 || strcmp(name, "description") == 0 ||
			    strcmp(name, "debugDescription") == 0 || strcmp(name, "zone") == 0) {
				continue;
			}
			(void)class_addMethod(cls, m[i].name, (IMP_t)_objc_msgForward, m[i].types);
		}
		free(m);
	}
	if (depth < 4) {
		unsigned int np = 0;
		void **sub = protocol_copyProtocolList(proto, &np);
		for (unsigned int i = 0; i < np; i++) {
			if (strcmp(protocol_getName(sub[i]), "NSObject") != 0) {
				route_protocol(cls, sub[i], depth + 1);
			}
		}
		free(sub);
	}
}

/*
 * Step 0b: objects of the right kind. When a routed method returns an object, the selector tells
 * what CoreAnimation expects back (newCommandQueue -> an MTLCommandQueue, commandBuffer -> an
 * MTLCommandBuffer, newTexture... -> an MTLTexture, ...), and it gets a stub of that kind: a
 * class made at run time per protocol, declaring the protocol (and its SPI twin) and routing all
 * of its methods through the same logging forwardInvocation. Nothing is drawn; the point is to
 * see the whole of what is asked, not only up to the first nil.
 */
static const struct {
	const char *needle, *proto;
} stub_kinds[] = {
	{ "parallelRenderCommandEncoder", "MTLParallelRenderCommandEncoder" },
	{ "renderCommandEncoder", "MTLRenderCommandEncoder" },
	{ "blitCommandEncoder", "MTLBlitCommandEncoder" },
	{ "computeCommandEncoder", "MTLComputeCommandEncoder" },
	{ "newCommandQueue", "MTLCommandQueue" },
	{ "commandBuffer", "MTLCommandBuffer" },
	{ "newRenderPipelineState", "MTLRenderPipelineState" },
	{ "newComputePipelineState", "MTLComputePipelineState" },
	{ "newDepthStencilState", "MTLDepthStencilState" },
	{ "newSamplerState", "MTLSamplerState" },
	{ "newTexture", "MTLTexture" },
	{ "newBuffer", "MTLBuffer" },
	{ "newDefaultLibrary", "MTLLibrary" },
	{ "newLibrary", "MTLLibrary" },
	{ "newFunction", "MTLFunction" },
	{ "newSharedEvent", "MTLSharedEvent" },
	{ "newEvent", "MTLEvent" },
	{ "newFence", "MTLFence" },
	{ "newHeap", "MTLHeap" },
	{ "newIndirectCommandBuffer", "MTLIndirectCommandBuffer" },
	{ "newArgumentEncoder", "MTLArgumentEncoder" },
	{ "newPipelineLibrary", "MTLPipelineLibrary" },
	{ "newBinaryArchive", "MTLBinaryArchive" },
	{ "newDynamicLibrary", "MTLDynamicLibrary" },
	{ "newRasterizationRateMap", "MTLRasterizationRateMap" },
	{ "newCounterSampleBuffer", "MTLCounterSampleBuffer" },
	{ "newAccelerationStructure", "MTLAccelerationStructure" },
	{ "newResidencySet", "MTLResidencySet" },
	{ NULL, NULL }
};

static void stub_forward(id self, SEL _cmd, id inv);
void *memcpy(void *, const void *, size_t);

/*
 * Buffers and labels: on this GPU a Metal buffer is memory the CPU and the GPU share, and callers
 * write into [buffer contents] (CoreAnimation's gamma LUT is bigger than any shared scratch).
 * Each stub's state is kept in a hash bucket keyed by object identity and torn down in -dealloc,
 * so freed objects never leak owned memory or alias a later object allocated at the same address.
 */
struct stub_block {
	void *isa;
	int flags;
	int reserved;
	void (*invoke)(void *, void *, unsigned long);
};

struct stub_info {
	id obj;
	void *mem;
	unsigned long len;
	int owns_mem;
	id deallocator;
	char name[120];
	id label;
	struct stub_info *next;
};

#define STUB_BUCKETS 1024
static struct stub_info *stub_buckets[STUB_BUCKETS];
static volatile char stub_lock;

static void
stub_spin_lock(void)
{
	while (__atomic_test_and_set(&stub_lock, __ATOMIC_ACQUIRE)) {
	}
}

static void
stub_spin_unlock(void)
{
	__atomic_clear(&stub_lock, __ATOMIC_RELEASE);
}

static unsigned int
stub_bucket_idx(id obj)
{
	unsigned long p = (unsigned long)obj;
	return (unsigned int)(((p >> 4) ^ (p >> 16)) & (STUB_BUCKETS - 1));
}

static struct stub_info *
stub_info_get(id obj, int create)
{
	unsigned int b = stub_bucket_idx(obj);
	for (struct stub_info *it = stub_buckets[b]; it != NULL; it = it->next) {
		if (it->obj == obj) {
			return it;
		}
	}
	if (!create) {
		return NULL;
	}
	struct stub_info *it = calloc(1, sizeof(*it));
	if (it != NULL) {
		it->obj = obj;
		it->next = stub_buckets[b];
		stub_buckets[b] = it;
	}
	return it;
}

static void
stub_dealloc(id self, SEL _cmd)
{
	unsigned int b = stub_bucket_idx(self);
	struct stub_info *found = NULL;

	(void)_cmd;
	stub_spin_lock();
	for (struct stub_info **pp = &stub_buckets[b]; *pp != NULL; pp = &(*pp)->next) {
		if ((*pp)->obj == self) {
			found = *pp;
			*pp = found->next;
			break;
		}
	}
	stub_spin_unlock();

	if (found != NULL) {
		if (found->owns_mem && found->mem != NULL) {
			free(found->mem);
		} else if (found->deallocator != (id)0) {
			struct stub_block *blk = (struct stub_block *)found->deallocator;
			if (blk->invoke != NULL) {
				blk->invoke(found->deallocator, found->mem, found->len);
			}
			(void)msg0(found->deallocator, "release");
		}
		if (found->label != (id)0) {
			(void)msg0(found->label, "release");
		}
		free(found);
	}
	struct objc_super sup = { self, objc_getClass("NSObject") };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel_registerName("dealloc"));
}

static void
name_set(id obj, const char *n)
{
	if (obj == (id)0 || n == NULL) {
		return;
	}
	stub_spin_lock();
	struct stub_info *info = stub_info_get(obj, 1);
	if (info != NULL) {
		snprintf(info->name, sizeof(info->name), "%s", n);
	}
	stub_spin_unlock();
}

static int
name_copy(id obj, char *out, size_t out_len)
{
	int ok = 0;
	stub_spin_lock();
	struct stub_info *info = stub_info_get(obj, 0);
	if (info != NULL && info->name[0] != 0) {
		snprintf(out, out_len, "%s", info->name);
		ok = 1;
	}
	stub_spin_unlock();
	return ok;
}

static id
stub_label(id self, SEL _cmd)
{
	id l = (id)0;
	(void)_cmd;
	stub_spin_lock();
	struct stub_info *info = stub_info_get(self, 0);
	if (info != NULL && info->label != (id)0) {
		l = msg0(info->label, "retain");
	}
	stub_spin_unlock();
	return l != (id)0 ? msg0(l, "autorelease") : (id)0;
}

static void
stub_set_label(id self, SEL _cmd, id label)
{
	id copy = label != (id)0 ? msg0(label, "copy") : (id)0;
	id old = (id)0;
	(void)_cmd;
	stub_spin_lock();
	struct stub_info *info = stub_info_get(self, copy != (id)0);
	if (info != NULL) {
		old = info->label;
		info->label = copy;
		copy = (id)0;
	}
	stub_spin_unlock();
	if (old != (id)0) {
		(void)msg0(old, "release");
	}
	if (copy != (id)0) {
		(void)msg0(copy, "release");
	}
}

/* A stub describes itself by kind and, when known, name: pipeline descriptors print it. */
static id
stub_description(id self, SEL _cmd)
{
	char d[200], nbuf[120];
	int has_n = name_copy(self, nbuf, sizeof(nbuf));
	(void)_cmd;
	snprintf(d, sizeof(d), "<%s%s%s>", class_getName(object_getClass(self)) + 13, has_n ? " " : "", has_n ? nbuf : "");
	return ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), d);
}

static const char *
utf8_of(id obj, char *buf, size_t len)
{
	id d = msg0(obj, "description");
	const char *u = d != (id)0 ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : NULL;
	size_t i = 0;
	for (; u != NULL && u[i] != 0 && i + 1 < len; i++) {
		buf[i] = (u[i] == '\n' || u[i] == '\t') ? ' ' : u[i];
	}
	buf[i] = 0;
	return buf;
}

static void
buffer_made(id obj, void *mem, unsigned long len, int owns_mem, id deallocator)
{
	id copy_blk = deallocator != (id)0 ? msg0(deallocator, "copy") : (id)0;
	void *old_mem = NULL;
	id old_blk = (id)0;

	stub_spin_lock();
	struct stub_info *info = stub_info_get(obj, 1);
	if (info != NULL) {
		if (info->owns_mem) {
			old_mem = info->mem;
		} else {
			old_blk = info->deallocator;
		}
		info->mem = mem;
		info->len = len;
		info->owns_mem = owns_mem;
		info->deallocator = copy_blk;
		copy_blk = (id)0;
	}
	stub_spin_unlock();
	if (old_mem != NULL) {
		free(old_mem);
	}
	if (old_blk != (id)0) {
		(void)msg0(old_blk, "release");
	}
	if (copy_blk != (id)0) {
		(void)msg0(copy_blk, "release");
	}
}

static int
buffer_get(id obj, void **mem_out, unsigned long *len_out)
{
	int found = 0;
	stub_spin_lock();
	struct stub_info *info = stub_info_get(obj, 0);
	if (info != NULL && (info->mem != NULL || info->owns_mem || info->len != 0)) {
		if (mem_out != NULL) {
			*mem_out = info->mem;
		}
		if (len_out != NULL) {
			*len_out = info->len;
		}
		found = 1;
	}
	stub_spin_unlock();
	return found;
}
static id stub_signature(id self, SEL _cmd, SEL sel);
static void stub_unrecognized(id self, SEL _cmd, SEL sel);

static const char *
protocol_types(void *proto, SEL sel, int depth)
{
	if (proto == NULL || depth > 5) {
		return NULL;
	}
	for (int required = 1; required >= 0; required--) {
		unsigned int n = 0;
		struct objc_method_description *m = protocol_copyMethodDescriptionList(proto, required, 1, &n);
		for (unsigned int i = 0; i < n; i++) {
			if (m[i].name == sel && m[i].types != NULL) {
				const char *t = m[i].types;
				free(m);
				return t;
			}
		}
		free(m);
	}
	unsigned int np = 0;
	void **sub = protocol_copyProtocolList(proto, &np);
	for (unsigned int i = 0; i < np; i++) {
		const char *t = protocol_types(sub[i], sel, depth + 1);
		if (t != NULL) {
			free(sub);
			return t;
		}
	}
	free(sub);
	return NULL;
}

static int
contains(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

static Class
stub_class(const char *proto_name)
{
	char name[160], spi_name[160], impl_name[160];
	snprintf(name, sizeof(name), "SapphireStub_%s", proto_name);
	Class cls = objc_getClass(name);
	if (cls != (Class)0) {
		return cls;
	}
	cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
	if (cls == (Class)0) {
		return objc_getClass(name);
	}
	snprintf(spi_name, sizeof(spi_name), "%sSPI", proto_name);
	snprintf(impl_name, sizeof(impl_name), "%sImplementation", proto_name);
	void *impl = objc_getProtocol(impl_name), *spi = objc_getProtocol(spi_name), *pub = objc_getProtocol(proto_name);
	if (impl != NULL) {
		(void)class_addProtocol(cls, impl);
		route_protocol(cls, impl, 0);
	}
	if (spi != NULL) {
		(void)class_addProtocol(cls, spi);
		route_protocol(cls, spi, 0);
	}
	if (pub != NULL) {
		(void)class_addProtocol(cls, pub);
		route_protocol(cls, pub, 0);
	}
	(void)class_addMethod(cls, sel_registerName("dealloc"), (IMP_t)stub_dealloc, "v@:");
	(void)class_addMethod(cls, sel_registerName("label"), (IMP_t)stub_label, "@@:");
	(void)class_addMethod(cls, sel_registerName("setLabel:"), (IMP_t)stub_set_label, "v@:@");
	(void)class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP_t)stub_forward, "v@:@");
	(void)class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP_t)stub_signature, "@@::");
	(void)class_addMethod(cls, sel_registerName("doesNotRecognizeSelector:"), (IMP_t)stub_unrecognized, "v@::");
	(void)class_addMethod(cls, sel_registerName("description"), (IMP_t)stub_description, "@@:");
	/* Metal's own descriptors describe what they point at with this private one */
	(void)class_addMethod(cls, sel_registerName("formattedDescription:"), (IMP_t)stub_description, "@@:Q");
	objc_registerClassPair(cls);
	SLOG("made %s (%s%s%s)", name, pub ? proto_name : "no protocol", spi ? " + SPI" : "", impl ? " + Impl" : "");
	return cls;
}

static id
stub_for(SEL sel)
{
	const char *n = sel_getName(sel);
	if (strcmp(n, "device") == 0) {
		return sapphire_device;
	}
	for (int i = 0; stub_kinds[i].needle != NULL; i++) {
		if (contains(n, stub_kinds[i].needle)) {
			id obj = msg0((id)stub_class(stub_kinds[i].proto), "alloc");
			return msg0(obj, "init");
		}
	}
	return (id)0;
}

/* The routed call: log, then answer with a stub, _MTLDevice's implementation, or zero. */
static void
forward_common(id self, id inv, Class super_cls)
{
	SEL sel = ((SEL (*)(id, SEL))objc_msgSend)(inv, sel_registerName("selector"));
	id sig = msg0(inv, "methodSignature");
	unsigned long rlen = ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_registerName("methodReturnLength"));
	const char *rtype = ((const char *(*)(id, SEL))objc_msgSend)(sig, sel_registerName("methodReturnType"));
	Method m = super_cls != (Class)0 ? class_getInstanceMethod(super_cls, sel) : (Method)0;
	id stub = (rtype != NULL && rtype[0] == '@') ? stub_for(sel) : (id)0;
	/*
	 * A raw pointer (targetDeviceInfo, ...) gets zeroed memory rather than NULL, so a caller
	 * that reads a field of it carries on and the trace goes further.
	 */
	static char zeros[65536];
	int ptr = rtype != NULL && (rtype[0] == '^' || rtype[0] == '*') && rlen == sizeof(void *);
	const char *who = class_getName(object_getClass(self));

	if (sapphire_logged++ < 6000) {
		SLOG("%s %s%s", strncmp(who, "SapphireStub_", 13) == 0 ? who + 13 : "device", sel_getName(sel),
		    stub != (id)0 ? "  -> stub" : (ptr ? "  -> zeroed memory" : (m != NULL ? "  (to _MTLDevice)" : "")));
	}
	if (rlen > 0) {
		void *zero = calloc(1, rlen < 64 ? 64 : rlen);
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
	const char *sn = sel_getName(sel);
	/* creation calls: log every object argument's description (names, paths, descriptors) */
	if (strncmp(sn, "new", 3) == 0 && sapphire_logged < 6000) {
		unsigned long nargs = ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_registerName("numberOfArguments"));
		for (unsigned long a = 2; a < nargs; a++) {
			const char *at = ((const char *(*)(id, SEL, unsigned long))objc_msgSend)(sig,
			    sel_registerName("getArgumentTypeAtIndex:"), a);
			if (at == NULL || at[0] != '@') {
				continue;
			}
			id arg = (id)0;
			((void (*)(id, SEL, void *, long))objc_msgSend)(inv, sel_registerName("getArgument:atIndex:"), &arg, (long)a);
			if (arg == (id)0) {
				continue;
			}
			char b[600];
			SLOG("  arg %lu: %s", a - 2, utf8_of(arg, b, sizeof(b)));
			if (stub != (id)0 && a == 2 && (strncmp(sn, "newFunctionWithName", 19) == 0 ||
			    strncmp(sn, "newRenderPipelineStateWithName", 30) == 0 ||
			    strncmp(sn, "newComputePipelineStateWithName", 31) == 0)) {
				name_set(stub, b);
			}
			if (stub != (id)0 && a == 2 && strncmp(sn, "newFunctionWithDescriptor", 25) == 0) {
				id fn = msg0(arg, "name");
				char nb[120];
				if (fn != (id)0) {
					name_set(stub, utf8_of(fn, nb, sizeof(nb)));
				}
			}
		}
	}
	if (stub != (id)0 && strncmp(sn, "newBuffer", 9) == 0) {
		unsigned long len = 0;
		void *bytes = NULL;
		id dealloc_blk = (id)0;
		unsigned long nargs = ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_registerName("numberOfArguments"));
		int no_copy = strncmp(sn, "newBufferWithBytesNoCopy:", 25) == 0;
		if (strncmp(sn, "newBufferWithLength:", 20) == 0) {
			((void (*)(id, SEL, void *, long))objc_msgSend)(inv, sel_registerName("getArgument:atIndex:"), &len, 2);
		} else {
			((void (*)(id, SEL, void *, long))objc_msgSend)(inv, sel_registerName("getArgument:atIndex:"), &bytes, 2);
			((void (*)(id, SEL, void *, long))objc_msgSend)(inv, sel_registerName("getArgument:atIndex:"), &len, 3);
			if (no_copy && nargs >= 6) {
				const char *at = ((const char *(*)(id, SEL, unsigned long))objc_msgSend)(sig,
				    sel_registerName("getArgumentTypeAtIndex:"), 5);
				if (at != NULL && at[0] == '@') {
					((void (*)(id, SEL, void *, long))objc_msgSend)(inv,
					    sel_registerName("getArgument:atIndex:"), &dealloc_blk, 5);
				}
			}
		}
		void *mem = no_copy ? bytes : calloc(1, len ? len : 16);
		if (!no_copy && mem != NULL && bytes != NULL && len != 0) {
			memcpy(mem, bytes, len);
		}
		buffer_made(stub, mem, len, !no_copy, dealloc_blk);
		SLOG("  buffer %p: %lu bytes at %p", (void *)stub, len, mem);
	}
	void *bmem = NULL;
	unsigned long blen = 0;
	int is_buf = strncmp(who, "SapphireStub_MTLBuffer", 22) == 0 && buffer_get(self, &bmem, &blen);
	if (is_buf && strcmp(sn, "contents") == 0) {
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &bmem);
	} else if (is_buf && strcmp(sn, "length") == 0) {
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &blen);
	} else if (is_buf && strcmp(sn, "gpuAddress") == 0) {
		unsigned long addr = (unsigned long)bmem;
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &addr);
	} else if (stub != (id)0) {
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &stub);
	} else if (ptr) {
		void *z = zeros;
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &z);
	} else if (m != NULL) {
		((void (*)(id, SEL, IMP_t))objc_msgSend)(inv, sel_registerName("invokeUsingIMP:"), method_getImplementation(m));
	}
}

static void
stub_forward(id self, SEL _cmd, id inv)
{
	(void)_cmd;
	forward_common(self, inv, (Class)0);
}

static id
stub_signature(id self, SEL _cmd, SEL sel)
{
	(void)_cmd;
	struct objc_super sup = { self, objc_getClass("NSObject") };
	id sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup,
	    sel_registerName("methodSignatureForSelector:"), sel);
	if (sig == (id)0) {
		const char *cname = class_getName(object_getClass(self));
		if (strncmp(cname, "SapphireStub_", 13) == 0) {
			char spi[160], impl[160];
			snprintf(spi, sizeof(spi), "%sSPI", cname + 13);
			snprintf(impl, sizeof(impl), "%sImplementation", cname + 13);
			const char *types = protocol_types(objc_getProtocol(impl), sel, 0);
			if (types == NULL) {
				types = protocol_types(objc_getProtocol(spi), sel, 0);
			}
			if (types == NULL) {
				types = protocol_types(objc_getProtocol(cname + 13), sel, 0);
			}
			if (types != NULL) {
				sig = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
				    sel_registerName("signatureWithObjCTypes:"), types);
			}
		}
	}
	/* Never spoof "@@:" for an unrecognized selector: on ARM64 a struct > 16 bytes returns via x8,
	 * so forwarding with a guessed scalar signature leaves *x8 uninitialized or corrupts memory. */
	return sig;
}

static void
stub_unrecognized(id self, SEL _cmd, SEL sel)
{
	if (sapphire_logged++ < 6000) {
		SLOG("%s %s: in no protocol (unknown return ABI), raising", class_getName(object_getClass(self)), sel_getName(sel));
	}
	struct objc_super sup = { self, objc_getClass("NSObject") };
	((void (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
}

struct sapphire_target_arch {
	unsigned long target;
	unsigned int version;
	unsigned int pad;
	const char *name;
};

@implementation SapphireMTLDevice

+ (void)load
{
	void *impl = objc_getProtocol("MTLDeviceImplementation");
	void *spi = objc_getProtocol("MTLDeviceSPI"), *pub = objc_getProtocol("MTLDevice");
	Class cls = objc_getClass("SapphireMTLDevice");
	/*
	 * MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)], and Apple's
	 * device classes declare <MTLDeviceSPI> (which takes in MTLDevice): declare both.
	 */
	if (impl != NULL) {
		(void)class_addProtocol(cls, impl);
		route_protocol(cls, impl, 0);
	}
	if (spi != NULL) {
		(void)class_addProtocol(cls, spi);
		route_protocol(cls, spi, 0);
	}
	if (pub != NULL) {
		(void)class_addProtocol(cls, pub);
		route_protocol(cls, pub, 0);
	}
	SLOG("loaded, MTLDeviceSPI %s, MTLDevice %s", spi ? "found" : "MISSING", pub ? "found" : "MISSING");
}

/*
 * Boot-arg sweetgl=1: CoreAnimation draws with OpenGL ES (Mesa's freedreno on the Adreno, xnu
 * 0149) instead of Metal. In backboardd this bundle -- which Metal loads there because
 * IOAcceleratorES names it -- loads the EAGL bridge (/redwood/lib/libredwood-gl.dylib, HackiOS's
 * eagl.c, userland/), whose constructor takes over EAGLContext; and in every process it makes
 * no Metal device, so CoreAnimation's render server falls back to OpenGL ES (EAGLServer).
 */
#define SAPPHIRE_GL "/redwood/lib/libredwood-gl.dylib"

static int
gl_mode(void)
{
	static int mode = -1;
	if (mode < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		mode = sysctlbyname("kern.bootargs", args, &size, 0, 0) == 0 && strstr(args, "sweetgl=1") != 0;
	}
	return mode;
}

- (id)initWithAcceleratorPort:(unsigned int)port
{
	if (gl_mode()) {
		static int loaded;
		const char *name = getprogname();
		if (!loaded && name != 0 && strcmp(name, "backboardd") == 0) {
			loaded = 1;
			/*
			 * Mesa takes the GPU only if SapphireGPU (xnu 0149) is there when the first
			 * context is made, and stays on the CPU (softpipe) for good otherwise; the service
			 * comes up when the kernel's GPU tests end (gpu_delay). So wait for it, 90 s at most.
			 */
			int waited = 0;
			for (; waited < 180; waited++) {
				unsigned int gpu = IOServiceGetMatchingService(0, IOServiceMatching("SapphireGPU"));
				if (gpu != 0) {
					IOObjectRelease(gpu);
					break;
				}
				usleep(500000);
			}
			SLOG("sweetgl=1: SapphireGPU %s after %d.%d s", waited < 180 ? "found" : "NOT found (Mesa draws on the CPU)",
			    waited / 2, (waited % 2) * 5);
			void *gl = dlopen(SAPPHIRE_GL, 2 /* RTLD_NOW */);
			SLOG("sweetgl=1: %s %s", SAPPHIRE_GL, gl != 0 ? "loaded: CoreAnimation is to draw with OpenGL ES" : dlerror());
		}
		SLOG("sweetgl=1: no Metal device (port %u): OpenGL ES instead", port);
		return (id)0;
	}
	self = [super init];
	sapphire_port = port;
	sapphire_device = self;
	SLOG("initWithAcceleratorPort: %u -> %p", port, (void *)self);
	static int tested;
	if (!tested) {
		tested = 1;
		(void)sgpu_selftest(port);              /* once per process; logs its own result */
	}
	return self;
}

/*
 * MetalPerformanceShaders (SpringBoard's blurs) identifies the GPU before anything else
 * (MPSDevice::GetMPSDevice: supportsFamily: per family, name compared with isEqualToString:,
 * gpuCoreCount), and asserts when it knows none of them. For the trace to go on, the device
 * answers as this phone's iOS expects its GPU to: an A11, GPU family Apple4 and below, the
 * common families, three cores. The real driver has to draw those blurs itself anyway.
 */
- (id)name
{
	static id n;
	if (n == (id)0) {
		n = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
		    sel_registerName("stringWithUTF8String:"), "Apple A11 GPU");
		n = msg0(n, "retain");
	}
	SLOG("device name -> Apple A11 GPU");
	return n;
}

- (signed char)supportsFamily:(long)family
{
	signed char yes = (family >= 1001 && family <= 1004) || (family >= 3001 && family <= 3003);
	SLOG("device supportsFamily: %ld -> %s", family, yes ? "YES" : "NO");
	return yes;
}

- (unsigned long)gpuCoreCount
{
	SLOG("device gpuCoreCount -> 3");
	return 3;
}

- (const struct sapphire_target_arch *)targetDeviceInfo
{
	static const struct sapphire_target_arch arch = { 0, 1, 0, "sapphire" };
	return &arch;
}

- (const void *)limits
{
	static const unsigned char zero_limits[4096];
	return zero_limits;
}

/* Everything routed to _objc_msgForward ends up here, as an NSInvocation. */
- (void)forwardInvocation:(id)inv
{
	forward_common(self, inv, class_getSuperclass(objc_getClass("SapphireMTLDevice")));
}

/*
 * Resolve signatures strictly from super or the device's Metal protocols; never spoof "@@:" for an
 * unknown selector whose ARM64 struct-return (x8) ABI is unknown.
 */
- (id)methodSignatureForSelector:(SEL)sel
{
	struct objc_super sup = { self, class_getSuperclass(objc_getClass("SapphireMTLDevice")) };
	id sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup,
	    sel_registerName("methodSignatureForSelector:"), sel);
	if (sig == (id)0) {
		const char *types = protocol_types(objc_getProtocol("MTLDeviceImplementation"), sel, 0);
		if (types == NULL) {
			types = protocol_types(objc_getProtocol("MTLDeviceSPI"), sel, 0);
		}
		if (types == NULL) {
			types = protocol_types(objc_getProtocol("MTLDevice"), sel, 0);
		}
		if (types != NULL) {
			sig = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
			    sel_registerName("signatureWithObjCTypes:"), types);
		} else if (sapphire_logged++ < 4000) {
			SLOG("%s: in no protocol (unknown return ABI)", sel_getName(sel));
		}
	}
	return sig;
}

/* _MTLDevice's abstract methods call this; log before super raises. */
- (void)doesNotRecognizeSelector:(SEL)sel
{
	if (sapphire_logged++ < 4000) {
		SLOG("%s: not implemented (abstract or unknown), raising", sel_getName(sel));
	}
	struct objc_super sup = { self, class_getSuperclass(objc_getClass("SapphireMTLDevice")) };
	((void (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
}

@end
