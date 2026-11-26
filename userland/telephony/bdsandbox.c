/*
 * libsapphire-bdsandbox.dylib: incoming SMS for Messages.
 *
 * imagent hands every incoming SMS to MessagesBlastDoorService (BlastDoor's sandboxed parser) before
 * Messages sees it. BlastDoor checks at start that it runs in a sandbox (Sandbox.swift: sandbox_check on
 * its own pid must say 1, "BlastDoor cannot run unsandboxed.", a Swift fatal error); there is no
 * sandbox here (no Sandbox.kext), so the service died at every start, imagent retried for ever, and
 * the SMS never arrived. Loaded into the service via DYLD_INSERT_LIBRARIES (kernel patch 0244), this:
 *   1. Interposes sandbox_check() (handling SANDBOX_FILTER_NONE even when SANDBOX_CHECK_NO_REPORT or
 *      companion high-word flags are OR'd into type, using proper ARM64 variadic stack forwarding).
 *   2. Patches BlastDoor's __bd_sandbox_check and __bd_sandbox_enable_state_flag calls in place:
 *      first checking the known iOS 16.7 (20H392) offsets, and if the binary differs (e.g. iPadOS 26),
 *      locating the "BlastDoor cannot run unsandboxed." string reference in __TEXT and patching the
 *      preceding sandbox-check BL calls that gate that fatal-error block.
 * Log: /var/tmp/bdsandbox.log.
 */
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BL_CHECK_16_7   0x940000a5u     /* iOS 16.7 (20H392): both calls encode alike (same distance) */
#define MOV_W0_1        0x52800020u     /* mov w0, #1 */

extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

static int
my_sandbox_check(pid_t pid, const char *operation, int type, ...)
{
	/* BlastDoor's Sandbox.swift checks sandbox_check(getpid(), NULL, SANDBOX_FILTER_NONE | flags) == 1 */
	if ((pid == 0 || pid == getpid()) && operation == NULL && (type & 0xFFFF) == 0) {
		return 1;
	}
	va_list ap;
	va_start(ap, type);
	void *arg1 = va_arg(ap, void *);
	void *arg2 = va_arg(ap, void *);
	va_end(ap);
	return sandbox_check(pid, operation, type, arg1, arg2);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} bd_interposers[] = {
	{ (const void *)my_sandbox_check, (const void *)sandbox_check },
};

static void
say(const char *s)
{
	int fd = open("/var/tmp/bdsandbox.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
	if (fd >= 0) {
		char line[192];
		int n = snprintf(line, sizeof(line), "[%d] %s\n", getpid(), s);
		if (n > 0) {
			size_t len = (size_t)n >= sizeof(line) ? sizeof(line) - 1 : (size_t)n;
			(void)write(fd, line, len);
		}
		close(fd);
	}
}

static int
patch_insn(uint32_t *p, uint32_t expected_mask, uint32_t expected_val)
{
	if ((*p & expected_mask) != expected_val) {
		return -1;
	}
	vm_address_t page = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		return -2;
	}
	*p = MOV_W0_1;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(p, 4);
	return 0;
}

static int
branches_to_panic(const uint32_t *text, size_t j, size_t panic_idx)
{
	uint32_t br = text[j];
	long dst = -1;
	if ((br & 0xff00001fu) == 0x34000000u || (br & 0xff00001fu) == 0x54000000u) {
		/* cbz w0, rel19 or b.eq rel19 */
		int32_t off19 = (int32_t)((br >> 5) & 0x7ffffu);
		if (off19 & (1 << 18)) off19 -= (1 << 19);
		dst = (long)j + off19;
	} else if ((br & 0xfff8001fu) == 0x36000000u) {
		/* tbz w0, #0, rel14 */
		int32_t off14 = (int32_t)((br >> 5) & 0x3fffu);
		if (off14 & (1 << 13)) off14 -= (1 << 14);
		dst = (long)j + off14;
	} else if (j + 6 >= panic_idx && j < panic_idx) {
		/* Inverted last check: cbnz/tbnz w0, L_ok falling straight into L_unsandboxed */
		if ((br & 0xff00001fu) == 0x35000000u) {
			int32_t off19 = (int32_t)((br >> 5) & 0x7ffffu);
			if (off19 & (1 << 18)) off19 -= (1 << 19);
			if ((long)j + off19 > (long)panic_idx) return 1;
		} else if ((br & 0xfff8001fu) == 0x37000000u) {
			int32_t off14 = (int32_t)((br >> 5) & 0x3fffu);
			if (off14 & (1 << 13)) off14 -= (1 << 14);
			if ((long)j + off14 > (long)panic_idx) return 1;
		}
	}
	return dst >= (long)panic_idx - 8 && dst <= (long)panic_idx + 2;
}

/* Scan BlastDoor's __TEXT around the "BlastDoor cannot run unsandboxed." string reference (iPadOS 26+). */
static int
patch_by_unsandboxed_anchor(const struct mach_header_64 *mh)
{
	static const char needle[] = "BlastDoor cannot run unsandboxed.";
	unsigned long cstr_sz = 0, text_sz = 0;
	const uint8_t *cstr = getsectiondata(mh, "__TEXT", "__cstring", &cstr_sz);
	uint32_t *text = (uint32_t *)getsectiondata(mh, "__TEXT", "__text", &text_sz);
	if (cstr == NULL || text == NULL || cstr_sz < sizeof(needle) || text_sz < 64) {
		return 0;
	}
	const uint8_t *str_ptr = memmem(cstr, cstr_sz, needle, sizeof(needle) - 1);
	if (str_ptr == NULL) {
		return 0;
	}
	uintptr_t str_addr = (uintptr_t)str_ptr;
	size_t n_insns = text_sz / 4;
	int patched = 0;

	for (size_t idx = 0; idx + 2 < n_insns; idx++) {
		uint32_t insn = text[idx];
		if ((insn & 0x9f000000u) != 0x90000000u) {
			continue; /* not ADRP */
		}
		uint32_t rd = insn & 0x1fu;
		int64_t imm21 = (int64_t)((((insn >> 5) & 0x7ffffu) << 2) | ((insn >> 29) & 3u));
		if (imm21 & (1LL << 20)) imm21 -= (1LL << 21);
		uintptr_t page = ((uintptr_t)&text[idx] & ~(uintptr_t)0xfffULL) + (uintptr_t)(imm21 << 12);
		if ((str_addr & ~(uintptr_t)0xfffULL) != page) {
			continue;
		}
		int match = 0;
		for (size_t a = 1; a <= 4 && idx + a < n_insns; a++) {
			uint32_t add = text[idx + a];
			if ((add & 0xffc00000u) == 0x91000000u && ((add >> 5) & 0x1fu) == rd) {
				uint32_t imm12 = (add >> 10) & 0xfffu;
				if (page + imm12 == str_addr) {
					match = 1;
					break;
				}
			}
		}
		if (!match) {
			continue;
		}
		size_t lo = idx > 64 ? idx - 64 : 0;
		for (size_t k = idx; k > lo;) {
			k--;
			if (text[k] == 0xd65f03c0u || text[k] == 0xd65f0fffu) {
				break; /* previous function's ret */
			}
			if ((text[k] & 0xfc000000u) == 0x94000000u) {
				if ((k + 1 < idx && branches_to_panic(text, k + 1, idx)) ||
				    (k + 2 < idx && branches_to_panic(text, k + 2, idx))) {
					if (patch_insn(&text[k], 0xfc000000u, 0x94000000u) == 0) {
						patched++;
					}
				}
			}
		}
		if (patched > 0) {
			break;
		}
	}
	return patched;
}

__attribute__((constructor)) static void
install(void)
{
	unsetenv("DYLD_INSERT_LIBRARIES");
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t n = name != NULL ? strlen(name) : 0;
		if (n < 30 || strcmp(name + n - 30, "/BlastDoor.framework/BlastDoor") != 0) {
			continue;
		}
		uint8_t *base = (uint8_t *)_dyld_get_image_header(i);
		int a = patch_insn((uint32_t *)(base + 0x99b64), 0xffffffffu, BL_CHECK_16_7);
		int b = patch_insn((uint32_t *)(base + 0x99b70), 0xffffffffu, BL_CHECK_16_7);
		int dyn = 0;
		if (a != 0 || b != 0) {
			dyn = patch_by_unsandboxed_anchor((const struct mach_header_64 *)base);
		}
		char line[128];
		snprintf(line, sizeof(line), "BlastDoor sandbox check: %s, state flag: %s, anchor_patched=%d",
		    a == 0 ? "patched" : "left alone", b == 0 ? "patched" : "left alone", dyn);
		say(line);
		return;
	}
	say("no BlastDoor image (interposer active)");
}
