/*
 * The MTLB container (hakios_metallib.h) across Metallib v1.x, v2.x, and iPadOS 26.1 / Metal 3.2+ / Metal 4 v3+:
 *
 *   0x00  "MTLB"
 *   0x04  u16          platform and 64-bit/stub flags
 *   0x06  u16 u16      the container's format version (major, minor: 1.x, 2.x, 3.x+)
 *   0x0a  u8 u8        file type and target OS
 *   0x0c  u16 u16      target OS version
 *   0x10  u64          the file's size
 *   0x18  u64 u64      the function list: where it starts, and its length (either bytes after the u32 count, or total bytes)
 *   0x28  u64 u64      the public metadata (offset, length)
 *   0x38  u64 u64      the private metadata (offset, length)
 *   0x48  u64 u64      the modules (bitcode section): where they start, how long
 *
 * The function list is a u32 count, then one entry per function: a u32 size (counted from the size field itself) and
 * tags until ENDT. A tag is four letters, a u16 (or u32 for large reflection/v3 chunks) length and that many bytes.
 * NAME is the name with its NUL, TYPE one byte, VERS two or four u16, OFFT three u64 or three u32 (the 3rd is the
 * module's offset from the start of the modules, plus optional v3+ fields), MDSZ the module's u64 or u32 size.
 */
#include "hakios_metallib.h"

#include <string.h>

#define HEADER_SIZE             0x58
#define BITCODE_WRAPPER_MAGIC   0x0B17C0DEu

static uint16_t
u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t
u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
u64(const uint8_t *p)
{
	return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}

/* [offset, offset + length) inside a file of SIZE bytes, without the sum wrapping */
static int
inside(uint64_t offset, uint64_t length, uint64_t size)
{
	return offset <= size && length <= size - offset;
}

static int
is_fourcc(const uint8_t *p)
{
	for (int i = 0; i < 4; i++) {
		if (p[i] < 0x20 || p[i] > 0x7e) {
			return 0;
		}
	}
	return 1;
}

static int
is_large_chunk_tag(const uint8_t *p)
{
	return memcmp(p, "RFLT", 4) == 0 || memcmp(p, "DEBI", 4) == 0 ||
	       memcmp(p, "BSYM", 4) == 0 || memcmp(p, "SMTH", 4) == 0;
}

static int
parse_entry_tags(const uint8_t *data, uint64_t start, uint64_t end, int mode,
    struct hakios_metallib_function *out, int *have_offset, int *have_size, const char **why)
{
	struct hakios_metallib_function f = { .type = HAKIOS_METALLIB_NO_TYPE };
	int got_offset = 0, got_size = 0, saw_endt = 0;
	uint64_t q;

	for (q = start; inside(q, 4, end);) {
		const uint8_t *value;
		uint64_t w, length;

		if (!is_fourcc(data + q)) {
			*why = "a function's entry holds a malformed 4-CC tag";
			return -1;
		}
		if (memcmp(data + q, "ENDT", 4) == 0) {
			for (uint64_t pad = q + 4; pad < end; pad++) {
				if (data[pad] != 0) {
					*why = "trailing non-zero bytes follow ENDT in a function's entry";
					return -1;
				}
			}
			saw_endt = 1;
			break;
		}
		w = (mode == 2 || (mode == 1 && is_large_chunk_tag(data + q))) ? 4 : 2;
		if (!inside(q, 4 + w, end)) {
			*why = "a tag runs past the end of its function's entry";
			return -1;
		}
		length = (w == 4) ? (uint64_t)u32(data + q + 4) : (uint64_t)u16(data + q + 4);
		if (!inside(q + 4 + w, length, end)) {
			*why = "a tag's value runs past the end of its function's entry";
			return -1;
		}
		value = data + q + 4 + w;
		if (memcmp(data + q, "NAME", 4) == 0) {
			uint32_t n = (uint32_t)length;

			while (n > 0 && value[n - 1] == '\0') {
				n--;
			}
			if (n > 0) {
				f.name = (const char *)value;
				f.name_length = n;
			}
		} else if (memcmp(data + q, "TYPE", 4) == 0 && length >= 1) {
			f.type = value[0];
		} else if (memcmp(data + q, "VERS", 4) == 0 && length >= 4) {
			int nvers = length >= 8 ? 4 : 2;

			for (int k = 0; k < nvers; k++) {
				f.air[k] = u16(value + 2 * k);
			}
		} else if (memcmp(data + q, "OFFT", 4) == 0) {
			if (length >= 24) {
				f.module_offset = u64(value + 16);
				got_offset = 1;
			} else if (length >= 12) {
				f.module_offset = u32(value + 8);
				got_offset = 1;
			}
		} else if (memcmp(data + q, "MDSZ", 4) == 0) {
			if (length >= 8) {
				f.module_size = u64(value);
				got_size = 1;
			} else if (length >= 4) {
				f.module_size = u32(value);
				got_size = 1;
			}
		}
		q += 4 + w + length;
	}
	if (!saw_endt) {
		*why = "a function's entry has no ENDT terminator";
		return -1;
	}
	*out = f;
	*have_offset = got_offset;
	*have_size = got_size;
	return 0;
}

int
hakios_metallib_parse(const uint8_t *data, size_t size, struct hakios_metallib *info,
    struct hakios_metallib_function *out, uint32_t max, const char **why)
{
	uint64_t list, list_length, list_end, pub, pub_length, priv, priv_length, modules, modules_length, p;
	uint32_t count;

	if (data == NULL || info == NULL || size < HEADER_SIZE || memcmp(data, "MTLB", 4) != 0) {
		*why = "not a metallib (no MTLB at the start, or shorter than its header)";
		return -1;
	}
	memset(info, 0, sizeof(*info));
	info->version[0] = u16(data + 4);
	info->version[1] = u16(data + 6);
	info->format_version[0] = u16(data + 6);
	info->format_version[1] = u16(data + 8);
	if (info->format_version[0] == 0 && u16(data + 8) != 0 && u32(data + 12) == 0) {
		info->format_version[0] = u16(data + 8);
		info->format_version[1] = u16(data + 10);
	}
	info->file_size = u64(data + 0x10);
	if (info->file_size != 0 && (info->file_size < HEADER_SIZE || info->file_size > size)) {
		*why = "the metallib header's file size is not inside the buffer";
		return -1;
	}
	list = u64(data + 0x18);
	list_length = u64(data + 0x20);
	pub = u64(data + 0x28);
	pub_length = u64(data + 0x30);
	priv = u64(data + 0x38);
	priv_length = u64(data + 0x40);
	modules = u64(data + 0x48);
	modules_length = u64(data + 0x50);
	if (list < HEADER_SIZE || !inside(list, 4, size)) {
		*why = "the function list is not inside the file";
		return -1;
	}
	if (inside(list + 4, list_length, size)) {
		list_end = list + 4 + list_length;
	} else if (list_length >= 4 && inside(list, list_length, size)) {
		list_end = list + list_length;
	} else {
		*why = "the function list is not inside the file";
		return -1;
	}
	if ((pub_length > 0 && (pub < HEADER_SIZE || !inside(pub, pub_length, size))) ||
	    (priv_length > 0 && (priv < HEADER_SIZE || !inside(priv, priv_length, size)))) {
		*why = "the metadata sections are not inside the file";
		return -1;
	}
	if ((modules_length > 0 && modules < HEADER_SIZE) || !inside(modules, modules_length, size)) {
		*why = "the modules are not inside the file";
		return -1;
	}
	count = u32(data + list);
	info->count = count;
	p = list + 4;
	for (uint32_t i = 0; i < count; i++) {
		struct hakios_metallib_function f = { .type = HAKIOS_METALLIB_NO_TYPE };
		uint64_t entry, end;
		int have_offset = 0, have_size = 0;
		const char *tag_err = "a tag runs past the end of its function's entry";
		const char *unused_err = NULL;

		if (!inside(p, 4, list_end)) {
			*why = "the function list ends before its count of functions";
			return -1;
		}
		entry = u32(data + p);
		if (entry < 8 || !inside(p, entry, list_end)) {
			*why = "a function's entry is not inside the function list";
			return -1;
		}
		end = p + entry;
		if (parse_entry_tags(data, p + 4, end, 0, &f, &have_offset, &have_size, &tag_err) != 0 &&
		    parse_entry_tags(data, p + 4, end, 1, &f, &have_offset, &have_size, &unused_err) != 0 &&
		    parse_entry_tags(data, p + 4, end, 2, &f, &have_offset, &have_size, &unused_err) != 0) {
			*why = tag_err;
			return -1;
		}
		if (f.name == NULL || !have_offset || !have_size) {
			*why = "a function has no NAME, OFFT or MDSZ";
			return -1;
		}
		if (f.module_size == 0 || !inside(f.module_offset, f.module_size, modules_length)) {
			*why = "a function's module is not inside the modules";
			return -1;
		}
		f.module_offset += modules;
		if (out != NULL && i < max) {
			out[i] = f;
		}
		p = end;
	}
	return 0;
}

#define FAT_MAGIC       0xcafebabeu
#define FAT_MAGIC_64    0xcafebabfu
#define CPU_TYPE_AIR64  0x01000017u     /* the AIR slice's cputype, as Metal looks for it */

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t
be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | (uint64_t)be32(p + 4);
}

int
hakios_metallib_thin(const uint8_t *data, size_t size, const uint8_t **thin, size_t *thin_size, const char **why)
{
	uint32_t magic, count;
	uint64_t stride;
	int fat64;

	if (data == NULL) {
		*why = "not a metallib (null buffer)";
		return -1;
	}
	if (size < 8) {
		*thin = data;
		*thin_size = size;
		return 0;
	}
	magic = be32(data);
	if (magic != FAT_MAGIC && magic != FAT_MAGIC_64) {
		*thin = data;
		*thin_size = size;
		return 0;
	}
	fat64 = (magic == FAT_MAGIC_64);
	stride = fat64 ? 32 : 20;
	count = be32(data + 4);
	for (uint32_t i = 0; i < count && i < 64; i++) {
		uint64_t at = 8 + stride * (uint64_t)i;

		if (!inside(at, stride, size)) {
			break;
		}
		if (be32(data + at) == CPU_TYPE_AIR64) {
			uint64_t offset = fat64 ? be64(data + at + 8) : (uint64_t)be32(data + at + 8);
			uint64_t length = fat64 ? be64(data + at + 16) : (uint64_t)be32(data + at + 12);

			if (!inside(offset, length, size)) {
				*why = "a universal metallib whose AIR64 slice is not inside it";
				return -1;
			}
			*thin = data + offset;
			*thin_size = (size_t)length;
			return 0;
		}
	}
	*why = "a universal metallib with no AIR64 slice (only code for Apple's own GPUs)";
	return -1;
}

const char *
hakios_metallib_type_name(uint8_t type)
{
	static const char *const names[] = {
		"vertex", "fragment", "kernel", "unqualified", "visible", "extern", "intersection", "mesh", "object"
	};

	return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

int
hakios_metallib_bitcode(const uint8_t *data, size_t size, const struct hakios_metallib_function *function,
    const uint8_t **bitcode, uint64_t *length)
{
	const uint8_t *module;

	if (data == NULL || function == NULL || bitcode == NULL || length == NULL ||
	    !inside(function->module_offset, function->module_size, size)) {
		return -1;
	}
	module = data + function->module_offset;
	if (function->module_size >= 16 && u32(module) == BITCODE_WRAPPER_MAGIC) {
		uint64_t offset = u32(module + 8);
		uint64_t wrapped = u32(module + 12);

		if (wrapped < 4 || !inside(offset, wrapped, function->module_size)) {
			return -1;
		}
		if (memcmp(module + offset, "BC\xc0\xde", 4) != 0) {
			return -1;
		}
		*bitcode = module + offset;
		*length = wrapped;
		return 0;
	}
	if (function->module_size >= 4 && memcmp(module, "BC\xc0\xde", 4) == 0) {
		*bitcode = module;
		*length = function->module_size;
		return 0;
	}
	return -1;
}
