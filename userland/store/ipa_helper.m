/*
 * sapphire-ipa-helper: Native TrollStore-free IPA installer & uninstaller for
 * Sapphire iOS / iPadOS 26.1 (where XNU's sweet_amfi.c accepts ad-hoc ldid -S
 * signatures directly without needing CoreTrust CVE-2023-41991).
 *
 * Includes a self-contained native C arm64e -> ARMv8.0 (Cortex-A73/A53) De-PAC
 * instruction rewriter and in-place CS_CodeDirectory (SHA-1/SHA-256/SHA-384 +
 * SC_Scatter) page rehasher so third-party arm64e Mach-O binaries, frameworks,
 * dylibs, and PlugIn .appex extensions work on device without needing Python.
 *
 * CLI (drop-in compatible with trollstorehelper for Tienda.app & sapphire-ipa-install.sh):
 *   sapphire-ipa-helper install <path.ipa> [force]
 *   sapphire-ipa-helper uninstall <bundleID>
 */
#import <Foundation/Foundation.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fts.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

extern char **environ;

#define INSTALLD_UID 33
#define INSTALLD_GID 33
#define MOBILE_UID   501
#define MOBILE_GID   501
#define BUNDLE_ROOT  @"/private/var/containers/Bundle/Application"
#define DATA_ROOT    @"/private/var/mobile/Containers/Data/Application"

#ifndef FAT_MAGIC_64
#define FAT_MAGIC_64 0xcafebabfu
#endif
#ifndef FAT_CIGAM_64
#define FAT_CIGAM_64 0xbfbafecau
#endif

/* Apple CodeSigning blob constants (big-endian on disk) */
#define DEPAC_CSMAGIC_EMBEDDED_SIGNATURE  0xFADE0CC0U
#define DEPAC_CSMAGIC_CODEDIRECTORY       0xFADE0C02U
#define DEPAC_CS_SUPPORTSSCATTER          0x00020100U
#define DEPAC_CS_SUPPORTSCODELIMIT64      0x00020300U
#define DEPAC_CS_HASHTYPE_SHA1            1U
#define DEPAC_CS_HASHTYPE_SHA256          2U
#define DEPAC_CS_HASHTYPE_SHA256_TRUNC    3U
#define DEPAC_CS_HASHTYPE_SHA384          4U

/* Native ARMv8.0 replacement instruction encodings (exact match to tools/ipados26_depac.py) */
#define DEPAC_INSN_NOP              0xD503201FU
#define DEPAC_INSN_RET              0xD65F03C0U
#define DEPAC_INSN_ERET             0xD69F03E0U
/* XPACI: AND Xd, Xd, #0x0000ffffffffffff -> N=1, immr=0, imms=47 (0x2F): 0x9240BC00 | (Rd<<5) | Rd */
#define DEPAC_INSN_AND_48BIT_BASE   0x9240BC00U
/* XPACD: AND Xd, Xd, #0xff00ffffffffffff -> N=1, immr=8, imms=55 (0x37): 0x9248DC00 | (Rd<<5) | Rd
 * Preserves Darwin TBI top byte [63:56] (ObjC tagged/non-pointer isa, Swift, nanov2 malloc)
 * while stripping PAC bits [55:48]! */
#define DEPAC_INSN_AND_TBI_48BIT    0x9248DC00U

#define DEPAC_SECT_INSN_ATTRS       (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t rd32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64_be(const uint8_t *p)
{
	return ((uint64_t)rd32_be(p) << 32) | (uint64_t)rd32_be(p + 4);
}

/* --------------------------------------------------------------------------
 * Native arm64e -> ARMv8.0 De-PAC Rewriter & CS_CodeDirectory Page Rehasher
 * -------------------------------------------------------------------------- */

struct depac_range {
	uint64_t start;
	uint64_t end;
};

struct depac_scatter_run {
	uint32_t base;
	uint32_t count;
	uint32_t slot_base;
};

struct macho_slice {
	uint64_t off;
	uint64_t size;
	uint32_t cputype;
	uint32_t cpusubtype;
};

static uint32_t depac_rewrite_arm64e_insn(uint32_t insn)
{
	/* 1. Combined Return / ERET with PAC: RETAA / RETAB / ERETAA / ERETAB */
	if (insn == 0xD65F0BFFU || insn == 0xD65F0FFFU) {
		return DEPAC_INSN_RET;
	}
	if (insn == 0xD69F0BFFU || insn == 0xD69F0FFFU) {
		return DEPAC_INSN_ERET;
	}

	/* 2. Combined Branch with Link / Jump with PAC */
	uint32_t top = insn & 0xFFFFFC00U;
	if (top == 0xD61F0800U || top == 0xD61F0C00U) {
		if ((insn & 0x1FU) == 0x1FU) {
			uint32_t rn = (insn >> 5) & 0x1FU;
			return 0xD61F0000U | (rn << 5);
		}
	} else if (top == 0xD63F0800U || top == 0xD63F0C00U) {
		if ((insn & 0x1FU) == 0x1FU) {
			uint32_t rn = (insn >> 5) & 0x1FU;
			return 0xD63F0000U | (rn << 5);
		}
	} else if (top == 0xD71F0800U || top == 0xD71F0C00U) {
		uint32_t rn = (insn >> 5) & 0x1FU;
		return 0xD61F0000U | (rn << 5);
	} else if (top == 0xD73F0800U || top == 0xD73F0C00U) {
		uint32_t rn = (insn >> 5) & 0x1FU;
		return 0xD63F0000U | (rn << 5);
	}

	/* 3. Data-processing (1 source) PAC / AUT / XPACI / XPACD */
	if ((insn & 0xFFFF8000U) == 0xDAC10000U) {
		uint32_t op2 = (insn >> 10) & 0x1FU;
		uint32_t rn = (insn >> 5) & 0x1FU;
		uint32_t rd = insn & 0x1FU;
		if (op2 <= 7U || (op2 <= 15U && rn == 31U)) {
			return DEPAC_INSN_NOP;
		}
		if (op2 == 16U && rn == 31U) {
			if (rd == 31U) return DEPAC_INSN_NOP;
			return DEPAC_INSN_AND_48BIT_BASE | (rd << 5) | rd;
		}
		if (op2 == 17U && rn == 31U) {
			if (rd == 31U) return DEPAC_INSN_NOP;
			return DEPAC_INSN_AND_TBI_48BIT | (rd << 5) | rd;
		}
	}

	/* 4. ARMv8.3 LDAPR / LDAPRB / LDAPRH Wt/Xt, [Xn] -> ARMv8.0 LDAR / LDARB / LDARH Wt/Xt, [Xn] */
	if ((insn & 0x3FFFFC00U) == 0x38BFC000U) {
		uint32_t size_bits = insn & 0xC0000000U;
		uint32_t rn_rt = insn & 0x3FFU;
		return size_bits | 0x08DFFC00U | rn_rt;
	}

	/* 5. Non-writeback LDRAA / LDRAB Xt, [Xn, #simm10*8] -> LDR or LDUR */
	if ((insn & 0xFF200400U) == 0xF8200400U) {
		uint32_t s = (insn >> 22) & 1U;
		int32_t imm9 = (int32_t)((insn >> 12) & 0x1FFU);
		uint32_t w = (insn >> 11) & 1U;
		uint32_t rn = (insn >> 5) & 0x1FU;
		uint32_t rt = insn & 0x1FU;
		int32_t simm10 = s ? (imm9 - 512) : imm9;
		int32_t byte_off = simm10 * 8;
		if (w == 0U && byte_off >= 0) {
			return 0xF9400000U | ((uint32_t)simm10 << 10) | (rn << 5) | rt;
		}
		if (w == 0U && byte_off >= -256 && byte_off < 0) {
			return 0xF8400000U | (((uint32_t)byte_off & 0x1FFU) << 12) | (rn << 5) | rt;
		}
		return insn;
	}

	/* 6. ARMv8.2 DC CVAP / ARMv8.5 DC CVADP Xt -> ARMv8.0 DC CVAC Xt */
	uint32_t dc_top = insn & 0xFFFFFFE0U;
	if (dc_top == 0xD50B7C20U || dc_top == 0xD50B7D20U) {
		return 0xD50B7A20U | (insn & 0x1FU);
	}

	/* 7. ARMv8.3 XPACLRI (HINT #7) -> AND X30, X30, #0x0000ffffffffffff (0x9240BFDE) */
	if (insn == 0xD50320FFU) {
		return DEPAC_INSN_AND_48BIT_BASE | (30U << 5) | 30U;
	}

	/* 8. ARMv8.5 SB (0xD50330FF) -> ISB (0xD5033FDF): unallocated on ARMv8.0-A */
	if (insn == 0xD50330FFU) {
		return 0xD5033FDFU;
	}

	/* Note: HINT-space PAC instructions (PACIASP/AUTIASP/etc.) execute as architectural
	 * single-cycle NOPs on ARMv8.0-A and are intentionally left untouched (parity with
	 * tools/ipados26_depac.py). */

	return insn;
}

static bool depac_cs_hash(uint8_t htype, const uint8_t *data, size_t len, uint8_t *out, uint8_t hlen)
{
	uint8_t dg[CC_SHA384_DIGEST_LENGTH];
	if (hlen == 0 || len > 0x7FFFFFFFU) return false;
	if (htype == DEPAC_CS_HASHTYPE_SHA1) {
		if (hlen > CC_SHA1_DIGEST_LENGTH) return false;
		CC_SHA1(data, (CC_LONG)len, dg);
		memcpy(out, dg, hlen);
		return true;
	}
	if (htype == DEPAC_CS_HASHTYPE_SHA256 || htype == DEPAC_CS_HASHTYPE_SHA256_TRUNC) {
		if (hlen > CC_SHA256_DIGEST_LENGTH) return false;
		CC_SHA256(data, (CC_LONG)len, dg);
		memcpy(out, dg, hlen);
		return true;
	}
	if (htype == DEPAC_CS_HASHTYPE_SHA384) {
		if (hlen > CC_SHA384_DIGEST_LENGTH) return false;
		CC_SHA384(data, (CC_LONG)len, dg);
		memcpy(out, dg, hlen);
		return true;
	}
	return false;
}

static size_t depac_parse_scatter(const uint8_t *slice_buf, uint64_t cd_pos, uint64_t cd_end,
                                  uint32_t version, struct depac_scatter_run **runs_out)
{
	*runs_out = NULL;
	if (version < DEPAC_CS_SUPPORTSSCATTER || cd_pos + 48ULL > cd_end) return 0;
	uint32_t scatter_off = rd32_be(slice_buf + cd_pos + 44);
	if (scatter_off == 0) return 0;
	uint64_t pos = cd_pos + (uint64_t)scatter_off;
	if (pos + 24ULL > cd_end) return 0;

	size_t cap = 16, n = 0;
	struct depac_scatter_run *runs = calloc(cap, sizeof(*runs));
	if (!runs) return 0;
	uint32_t slot_cursor = 0;
	while (pos + 24ULL <= cd_end) {
		uint32_t count = rd32_be(slice_buf + pos);
		uint32_t base = rd32_be(slice_buf + pos + 4);
		if (count == 0) break;
		if (n == cap) {
			cap *= 2;
			struct depac_scatter_run * nr = realloc(runs, cap * sizeof(*runs));
			if (!nr) {
				free(runs);
				return 0;
			}
			runs = nr;
		}
		runs[n].base = base;
		runs[n].count = count;
		runs[n].slot_base = slot_cursor;
		n++;
		slot_cursor += count;
		pos += 24ULL;
	}
	if (n == 0) {
		free(runs);
		return 0;
	}
	*runs_out = runs;
	return n;
}

static bool depac_ranges_overlap(const struct depac_range *ranges, size_t n_ranges,
                                 uint64_t p_start, uint64_t p_end)
{
	for (size_t i = 0; i < n_ranges; i++) {
		if (ranges[i].start < p_end && ranges[i].end > p_start) return true;
	}
	return false;
}

static uint64_t depac_rehash_cd_page(uint8_t *slice_buf, uint64_t cd_pos, uint64_t cd_end,
                                     uint32_t hash_off, uint8_t hash_size, uint8_t hash_type,
                                     uint64_t page_size, uint64_t limit,
                                     uint64_t p_idx, uint32_t slot_idx,
                                     const struct depac_range *ranges, size_t n_ranges)
{
	uint64_t p_rel_start = p_idx * page_size;
	if (p_rel_start >= limit) return 0;
	uint64_t p_rel_end = p_rel_start + page_size;
	if (p_rel_end > limit) p_rel_end = limit;
	if (!depac_ranges_overlap(ranges, n_ranges, p_rel_start, p_rel_end)) return 0;

	uint64_t slot_pos = cd_pos + (uint64_t)hash_off + (uint64_t)slot_idx * (uint64_t)hash_size;
	if (slot_pos + (uint64_t)hash_size > cd_end) return 0;

	uint8_t dg[CC_SHA384_DIGEST_LENGTH];
	if (!depac_cs_hash(hash_type, slice_buf + p_rel_start,
	                   (size_t)(p_rel_end - p_rel_start), dg, hash_size)) {
		return 0;
	}
	memcpy(slice_buf + slot_pos, dg, hash_size);
	return 1;
}

static uint64_t depac_update_cs_superblob_hashes(uint8_t *slice_buf, uint64_t slice_size,
                                                 uint32_t cs_off, uint32_t cs_size,
                                                 const struct depac_range *ranges, size_t n_ranges)
{
	if (n_ranges == 0 || cs_size < 12U || (uint64_t)cs_off + 12ULL > slice_size) return 0;
	uint64_t sb_end = (uint64_t)cs_off + (uint64_t)cs_size;
	if (sb_end > slice_size) sb_end = slice_size;

	uint64_t cd_offsets[16];
	size_t n_cds = 0;
	uint32_t magic = rd32_be(slice_buf + cs_off);
	if (magic == DEPAC_CSMAGIC_CODEDIRECTORY) {
		if ((uint64_t)cs_off + 44ULL <= sb_end) {
			cd_offsets[n_cds++] = (uint64_t)cs_off;
		}
	} else if (magic == DEPAC_CSMAGIC_EMBEDDED_SIGNATURE) {
		uint32_t count = rd32_be(slice_buf + cs_off + 8);
		for (uint32_t i = 0; i < count && n_cds < 16; i++) {
			uint64_t idx_pos = (uint64_t)cs_off + 12ULL + (uint64_t)i * 8ULL;
			if (idx_pos + 8ULL > sb_end) break;
			uint32_t stype = rd32_be(slice_buf + idx_pos);
			uint32_t soff = rd32_be(slice_buf + idx_pos + 4);
			if (stype == 0U || (stype >= 0x1000U && stype <= 0x1005U)) {
				uint64_t cd_pos = (uint64_t)cs_off + (uint64_t)soff;
				if (cd_pos + 44ULL <= sb_end &&
				    rd32_be(slice_buf + cd_pos) == DEPAC_CSMAGIC_CODEDIRECTORY) {
					cd_offsets[n_cds++] = cd_pos;
				}
			}
		}
	} else {
		return 0;
	}

	uint64_t updated_pages = 0;
	for (size_t c = 0; c < n_cds; c++) {
		uint64_t cd_pos = cd_offsets[c];
		uint32_t cd_len = rd32_be(slice_buf + cd_pos + 4);
		uint32_t version = rd32_be(slice_buf + cd_pos + 8);
		uint32_t hash_off = rd32_be(slice_buf + cd_pos + 16);
		uint32_t ident_off = rd32_be(slice_buf + cd_pos + 20);
		uint32_t n_special = rd32_be(slice_buf + cd_pos + 24);
		uint32_t n_code = rd32_be(slice_buf + cd_pos + 28);
		uint32_t code_limit = rd32_be(slice_buf + cd_pos + 32);
		uint8_t hash_size = slice_buf[cd_pos + 36];
		uint8_t hash_type = slice_buf[cd_pos + 37];
		uint8_t page_shift = slice_buf[cd_pos + 39];

		if (cd_len < 44U || page_shift > 30U || hash_size == 0) continue;
		uint64_t cd_end = cd_pos + (uint64_t)cd_len;
		if (cd_end > sb_end) cd_end = sb_end;

		uint64_t limit = (uint64_t)code_limit;
		if (version >= DEPAC_CS_SUPPORTSCODELIMIT64 && cd_pos + 64ULL <= cd_end) {
			uint64_t first_payload = (uint64_t)hash_off >= (uint64_t)n_special * (uint64_t)hash_size ?
			    (uint64_t)hash_off - (uint64_t)n_special * (uint64_t)hash_size : 0ULL;
			uint32_t scatter_off = (version >= DEPAC_CS_SUPPORTSSCATTER && cd_pos + 48ULL <= cd_end) ?
			    rd32_be(slice_buf + cd_pos + 44) : 0U;
			uint32_t team_off = (version >= 0x20200U && cd_pos + 52ULL <= cd_end) ?
			    rd32_be(slice_buf + cd_pos + 48) : 0U;
			if (first_payload >= 64ULL &&
			    (ident_off == 0U || ident_off >= 64U) &&
			    (scatter_off == 0U || scatter_off >= 64U) &&
			    (team_off == 0U || team_off >= 64U)) {
				uint64_t lim64 = rd64_be(slice_buf + cd_pos + 56);
				if (lim64 > 0) limit = lim64;
			}
		}
		if (limit > slice_size) limit = slice_size;
		if (limit == 0 || n_code == 0) continue;
		uint64_t page_size = (page_shift > 0U) ? (1ULL << page_shift) : limit;

		uint64_t max_page = (limit + page_size - 1ULL) / page_size;
		struct depac_scatter_run *scatter = NULL;
		size_t n_scatter = depac_parse_scatter(slice_buf, cd_pos, cd_end, version, &scatter);
		if (n_scatter == 0) {
			uint64_t end_p = (uint64_t)n_code < max_page ? (uint64_t)n_code : max_page;
			for (uint64_t p_idx = 0; p_idx < end_p; p_idx++) {
				updated_pages += depac_rehash_cd_page(
				    slice_buf, cd_pos, cd_end, hash_off, hash_size, hash_type,
				    page_size, limit, p_idx, (uint32_t)p_idx, ranges, n_ranges);
			}
		} else {
			for (size_t r = 0; r < n_scatter; r++) {
				uint64_t r_base = scatter[r].base;
				uint64_t r_end = r_base + (uint64_t)scatter[r].count;
				if (r_end > max_page) r_end = max_page;
				for (uint64_t p_idx = r_base; p_idx < r_end; p_idx++) {
					uint32_t slot = scatter[r].slot_base + (uint32_t)(p_idx - r_base);
					if (slot >= n_code) break;
					updated_pages += depac_rehash_cd_page(
					    slice_buf, cd_pos, cd_end, hash_off, hash_size, hash_type,
					    page_size, limit, p_idx, slot, ranges, n_ranges);
				}
			}
			free(scatter);
		}
	}
	return updated_pages;
}

static bool depac_add_range(struct depac_range **arr, size_t *cnt, size_t *cap,
                            uint64_t rel_off)
{
	if (*cnt > 0 && rel_off <= (*arr)[*cnt - 1].end + 256ULL) {
		if (rel_off + 4ULL > (*arr)[*cnt - 1].end) {
			(*arr)[*cnt - 1].end = rel_off + 4ULL;
		}
		return true;
	}
	if (*cnt == *cap) {
		size_t ncap = (*cap == 0) ? 32 : (*cap * 2);
		struct depac_range *na = realloc(*arr, ncap * sizeof(**arr));
		if (!na) return false;
		*arr = na;
		*cap = ncap;
	}
	(*arr)[*cnt].start = rel_off;
	(*arr)[*cnt].end = rel_off + 4ULL;
	(*cnt)++;
	return true;
}

static size_t parse_macho_slices(const uint8_t *buf, size_t buf_avail, uint64_t file_size,
                                 struct macho_slice *out_slices, size_t max_slices,
                                 bool *is_macho_out)
{
	if (is_macho_out) *is_macho_out = false;
	if (buf_avail < 32U || file_size < 32ULL) return 0;
	uint32_t m_le = rd32(buf);
	uint32_t m_be = rd32_be(buf);

	/* Thin 64-bit or 32-bit Mach-O */
	if (m_le == MH_MAGIC_64 || m_le == MH_MAGIC) {
		if (is_macho_out) *is_macho_out = true;
		out_slices[0].off = 0;
		out_slices[0].size = file_size;
		out_slices[0].cputype = rd32(buf + 4);
		out_slices[0].cpusubtype = rd32(buf + 8);
		return 1;
	}

	/* Universal / FAT 32-bit or 64-bit (big-endian standard or little-endian cigam) */
	bool fat64 = false;
	bool fat_be = true;
	if (m_be == FAT_MAGIC) {
		fat64 = false; fat_be = true;
	} else if (m_be == FAT_MAGIC_64) {
		fat64 = true; fat_be = true;
	} else if (m_le == FAT_MAGIC) {
		fat64 = false; fat_be = false;
	} else if (m_le == FAT_MAGIC_64) {
		fat64 = true; fat_be = false;
	} else {
		return 0;
	}

	uint32_t nfat = fat_be ? rd32_be(buf + 4) : rd32(buf + 4);
	uint64_t entry_sz = fat64 ? 32ULL : 20ULL;
	uint64_t hdr_need = 8ULL + (uint64_t)nfat * entry_sz;
	if (nfat == 0 || nfat > 32 || hdr_need > (uint64_t)buf_avail || hdr_need > file_size) {
		return 0;
	}
	if (is_macho_out) *is_macho_out = true;

	size_t n = 0;
	for (uint32_t i = 0; i < nfat && n < max_slices; i++) {
		const uint8_t *ep = buf + 8ULL + (uint64_t)i * entry_sz;
		uint32_t ct = fat_be ? rd32_be(ep) : rd32(ep);
		uint32_t cs = fat_be ? rd32_be(ep + 4) : rd32(ep + 4);
		uint64_t off = fat64 ? (fat_be ? rd64_be(ep + 8) : rd64(ep + 8))
		                     : (uint64_t)(fat_be ? rd32_be(ep + 8) : rd32(ep + 8));
		uint64_t sz  = fat64 ? (fat_be ? rd64_be(ep + 16) : rd64(ep + 16))
		                     : (uint64_t)(fat_be ? rd32_be(ep + 12) : rd32(ep + 12));
		if (sz >= 32ULL && off <= file_size && sz <= file_size - off) {
			out_slices[n].off = off;
			out_slices[n].size = sz;
			out_slices[n].cputype = ct;
			out_slices[n].cpusubtype = cs;
			n++;
		}
	}
	return n;
}

static bool slice_check_encrypted(const uint8_t *slice_buf, uint64_t slice_size)
{
	if (slice_size < 32ULL) return false;
	uint32_t magic = rd32(slice_buf);
	uint64_t hdr_sz = 0;
	if (magic == MH_MAGIC_64) {
		hdr_sz = 32ULL;
	} else if (magic == MH_MAGIC) {
		hdr_sz = 28ULL;
	} else {
		return false;
	}
	uint32_t ncmds = rd32(slice_buf + 16);
	uint32_t sizeofcmds = rd32(slice_buf + 20);
	uint64_t lc_end = hdr_sz + (uint64_t)sizeofcmds;
	if (lc_end > slice_size) lc_end = slice_size;

	uint64_t pos = hdr_sz;
	for (uint32_t i = 0; i < ncmds && pos + 8ULL <= lc_end; i++) {
		uint32_t cmd = rd32(slice_buf + pos);
		uint32_t cmdsize = rd32(slice_buf + pos + 4);
		if (cmdsize < 8U || pos + (uint64_t)cmdsize > lc_end) break;
		if ((cmd == LC_ENCRYPTION_INFO_64 && cmdsize >= 24U) ||
		    (cmd == LC_ENCRYPTION_INFO && cmdsize >= 20U)) {
			uint32_t cryptid = rd32(slice_buf + pos + 16);
			if (cryptid != 0U) return true;
		}
		pos += (uint64_t)cmdsize;
	}
	return false;
}

static uint64_t depac_process_arm64_slice(uint8_t *slice_buf, uint64_t slice_size,
                                          uint64_t *pages_rehashed_out)
{
	if (slice_size < 32ULL) return 0;
	if (rd32(slice_buf) != MH_MAGIC_64 || rd32(slice_buf + 4) != (uint32_t)CPU_TYPE_ARM64) {
		return 0;
	}
	if (slice_check_encrypted(slice_buf, slice_size)) {
		return 0;
	}

	uint32_t ncmds = rd32(slice_buf + 16);
	uint32_t sizeofcmds = rd32(slice_buf + 20);
	uint64_t lc_end = 32ULL + (uint64_t)sizeofcmds;
	if (lc_end > slice_size) return 0;

	bool has_cs = false;
	uint32_t cs_off = 0, cs_size = 0;
	struct depac_range *mod_ranges = NULL;
	size_t n_mod = 0, cap_mod = 0;
	uint64_t changed = 0;

	uint64_t cmd_pos = 32ULL;
	for (uint32_t i = 0; i < ncmds && cmd_pos + 8ULL <= lc_end; i++) {
		uint32_t cmd = rd32(slice_buf + cmd_pos);
		uint32_t cmdsize = rd32(slice_buf + cmd_pos + 4);
		if (cmdsize < 8U || cmd_pos + (uint64_t)cmdsize > lc_end) break;

		if (cmd == LC_CODE_SIGNATURE && cmdsize >= 16U) {
			cs_off = rd32(slice_buf + cmd_pos + 8);
			cs_size = rd32(slice_buf + cmd_pos + 12);
			has_cs = true;
		} else if (cmd == LC_SEGMENT_64 && cmdsize >= 72U) {
			uint64_t fileoff = rd64(slice_buf + cmd_pos + 40);
			uint64_t filesize = rd64(slice_buf + cmd_pos + 48);
			uint32_t initprot = rd32(slice_buf + cmd_pos + 60);
			uint32_t nsects = rd32(slice_buf + cmd_pos + 64);

			if ((initprot & VM_PROT_EXECUTE) != 0 && filesize > 0) {
				if (nsects > 0) {
					uint64_t sect_pos = cmd_pos + 72ULL;
					for (uint32_t s = 0; s < nsects && sect_pos + 80ULL <= cmd_pos + (uint64_t)cmdsize; s++) {
						uint64_t s_size = rd64(slice_buf + sect_pos + 40);
						uint32_t s_offset = rd32(slice_buf + sect_pos + 48);
						uint32_t s_flags = rd32(slice_buf + sect_pos + 64);
						uint32_t s_type = s_flags & SECTION_TYPE;
						if ((s_flags & DEPAC_SECT_INSN_ATTRS) != 0 &&
						    s_type != S_ZEROFILL && s_type != S_GB_ZEROFILL &&
						    s_size >= 4ULL && (uint64_t)s_offset <= slice_size &&
						    s_size <= slice_size - (uint64_t)s_offset) {
							uint64_t code_end = (uint64_t)s_offset + (s_size & ~3ULL);
							for (uint64_t off = (uint64_t)s_offset; off < code_end; off += 4ULL) {
								uint32_t insn = rd32(slice_buf + off);
								uint32_t new_insn = depac_rewrite_arm64e_insn(insn);
								if (new_insn != insn) {
									wr32(slice_buf + off, new_insn);
									depac_add_range(&mod_ranges, &n_mod, &cap_mod, off);
									changed++;
								}
							}
						}
						sect_pos += 80ULL;
					}
				} else if (fileoff <= slice_size && filesize <= slice_size - fileoff) {
					uint64_t code_start = fileoff < lc_end ? ((lc_end + 3ULL) & ~3ULL) : fileoff;
					uint64_t seg_end = fileoff + (filesize & ~3ULL);
					for (uint64_t off = code_start; off + 4ULL <= seg_end; off += 4ULL) {
						uint32_t insn = rd32(slice_buf + off);
						uint32_t new_insn = depac_rewrite_arm64e_insn(insn);
						if (new_insn != insn) {
							wr32(slice_buf + off, new_insn);
							depac_add_range(&mod_ranges, &n_mod, &cap_mod, off);
							changed++;
						}
					}
				}
			}
		}
		cmd_pos += (uint64_t)cmdsize;
	}

	if (changed > 0 && has_cs && n_mod > 0) {
		uint64_t pages = depac_update_cs_superblob_hashes(slice_buf, slice_size, cs_off, cs_size,
		                                                  mod_ranges, n_mod);
		if (pages_rehashed_out) *pages_rehashed_out += pages;
	}
	free(mod_ranges);
	return changed;
}

static BOOL inspect_or_depac_macho(const char *path, BOOL do_depac, BOOL *encryptedOut,
                                   uint64_t *insnsOut, uint64_t *pagesOut)
{
	if (encryptedOut) *encryptedOut = NO;
	int flags = do_depac ? O_RDWR : O_RDONLY;
	int fd = open(path, flags);
	if (fd < 0 && do_depac) {
		chmod(path, 0755);
		fd = open(path, O_RDWR);
	}
	if (fd < 0) return NO;

	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 32) {
		close(fd);
		return NO;
	}

	uint8_t head[1040] = {0};
	ssize_t hn = pread(fd, head, sizeof(head), 0);
	if (hn < 32) {
		close(fd);
		return NO;
	}

	struct macho_slice slices[32];
	bool is_macho = false;
	size_t n_slices = parse_macho_slices(head, (size_t)hn, (uint64_t)st.st_size,
	                                     slices, 32, &is_macho);
	if (!is_macho || n_slices == 0) {
		close(fd);
		return NO;
	}

	size_t file_size = (size_t)st.st_size;
	int prot = do_depac ? (PROT_READ | PROT_WRITE) : PROT_READ;
	int mflags = do_depac ? MAP_SHARED : MAP_PRIVATE;
	bool used_heap = false;
	uint8_t *buf = mmap(NULL, file_size, prot, mflags, fd, 0);
	if (buf == MAP_FAILED) {
		buf = malloc(file_size);
		if (!buf) {
			close(fd);
			return YES;
		}
		size_t got = 0;
		while (got < file_size) {
			ssize_t r = pread(fd, buf + got, file_size - got, (off_t)got);
			if (r <= 0) break;
			got += (size_t)r;
		}
		if (got != file_size) {
			free(buf);
			close(fd);
			return YES;
		}
		used_heap = true;
	}

	BOOL any_encrypted = NO;
	uint64_t total_changed = 0;
	uint64_t total_pages = 0;
	for (size_t i = 0; i < n_slices; i++) {
		uint8_t *sl = buf + slices[i].off;
		uint64_t sl_sz = slices[i].size;
		if (slice_check_encrypted(sl, sl_sz)) {
			any_encrypted = YES;
			continue;
		}
		if (do_depac && slices[i].cputype == (uint32_t)CPU_TYPE_ARM64) {
			total_changed += depac_process_arm64_slice(sl, sl_sz, &total_pages);
		}
	}

	if (encryptedOut) *encryptedOut = any_encrypted;
	if (insnsOut) *insnsOut += total_changed;
	if (pagesOut) *pagesOut += total_pages;

	if (used_heap) {
		if (do_depac && total_changed > 0) {
			size_t wr = 0;
			while (wr < file_size) {
				ssize_t w = pwrite(fd, buf + wr, file_size - wr, (off_t)wr);
				if (w <= 0) break;
				wr += (size_t)w;
			}
		}
		free(buf);
	} else {
		if (do_depac && total_changed > 0) {
			msync(buf, file_size, MS_SYNC);
		}
		munmap(buf, file_size);
	}

	if (do_depac && total_changed > 0) {
		fchmod(fd, st.st_mode & 07777);
	}
	close(fd);
	return YES;
}

static BOOL is_macho_and_check_crypt(const char *path, BOOL *encryptedOut)
{
	return inspect_or_depac_macho(path, NO, encryptedOut, NULL, NULL);
}

/* --------------------------------------------------------------------------
 * Process / ZIP / Signing / LaunchServices Helpers
 * -------------------------------------------------------------------------- */

static int run_cmd(NSString *bin, NSArray<NSString *> *args, NSData **outData)
{
	if (![NSFileManager.defaultManager isExecutableFileAtPath:bin]) return -1;
	int fds[2] = {-1, -1};
	if (pipe(fds) != 0) return -1;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, fds[0]);

	NSUInteger argc = args.count + 1;
	char **argv = calloc(argc + 1, sizeof(char *));
	argv[0] = strdup(bin.fileSystemRepresentation);
	for (NSUInteger i = 0; i < args.count; i++) {
		argv[i + 1] = strdup(args[i].UTF8String);
	}
	pid_t pid = 0;
	int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	for (NSUInteger i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);

	NSMutableData *bufData = [NSMutableData data];
	char tmp[4096];
	ssize_t n;
	while (rc == 0 && (n = read(fds[0], tmp, sizeof(tmp))) > 0) {
		[bufData appendBytes:tmp length:(NSUInteger)n];
	}
	close(fds[0]);
	if (rc != 0) return -1;
	int st = 0;
	waitpid(pid, &st, 0);
	if (outData) *outData = bufData;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static NSString *find_tool(NSString *name)
{
	NSArray<NSString *> *dirs = @[@"/var/jb/usr/bin", @"/var/jb/bin", @"/redwood/bin", @"/usr/bin", @"/bin"];
	for (NSString *d in dirs) {
		NSString *p = [d stringByAppendingPathComponent:name];
		if ([NSFileManager.defaultManager isExecutableFileAtPath:p]) return p;
	}
	return nil;
}

static BOOL is_safe_relpath(NSString *rel)
{
	if (!rel.length || [rel hasPrefix:@"/"] || [rel containsString:@"\\"]) return NO;
	for (NSString *comp in [rel pathComponents]) {
		if ([comp isEqualToString:@".."]) return NO;
	}
	return YES;
}

static BOOL extract_ipa_zip(NSString *ipaPath, NSString *destDir, NSString **errOut)
{
	NSError *err = nil;
	NSData *zip = [NSData dataWithContentsOfFile:ipaPath options:NSDataReadingMappedIfSafe error:&err];
	if (!zip || zip.length < 22) {
		if (errOut) *errOut = @"Invalid or unreadable IPA archive";
		return NO;
	}
	const uint8_t *b = zip.bytes;
	size_t sz = zip.length;

	ssize_t eocd = -1;
	size_t minOff = sz > 65557 ? sz - 65557 : 0;
	for (ssize_t i = (ssize_t)sz - 22; i >= (ssize_t)minOff; i--) {
		if (rd32(b + i) == 0x06054b50u) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		if (errOut) *errOut = @"ZIP End-of-Central-Directory not found";
		return NO;
	}

	uint16_t totalEntries = rd16(b + eocd + 10);
	uint32_t cdSize = rd32(b + eocd + 12);
	uint32_t cdOff = rd32(b + eocd + 16);
	if ((uint64_t)cdOff + (uint64_t)cdSize > sz) {
		if (errOut) *errOut = @"Corrupt ZIP Central Directory bounds";
		return NO;
	}

	NSFileManager *fm = NSFileManager.defaultManager;
	size_t pos = cdOff;
	for (uint16_t idx = 0; idx < totalEntries && pos + 46 <= (size_t)cdOff + cdSize; idx++) {
		if (rd32(b + pos) != 0x02014b50u) break;
		uint16_t method = rd16(b + pos + 10);
		uint32_t compSz = rd32(b + pos + 20);
		uint32_t uncompSz = rd32(b + pos + 24);
		uint16_t nameLen = rd16(b + pos + 28);
		uint16_t extraLen = rd16(b + pos + 30);
		uint16_t commentLen = rd16(b + pos + 32);
		uint32_t extAttr = rd32(b + pos + 38);
		uint32_t lfhOff = rd32(b + pos + 42);
		if (pos + 46 + nameLen + extraLen + commentLen > sz) {
			if (errOut) *errOut = @"Truncated ZIP Central Directory entry";
			return NO;
		}
		NSString *name = [[NSString alloc] initWithBytes:b + pos + 46 length:nameLen encoding:NSUTF8StringEncoding];
		pos += 46 + nameLen + extraLen + commentLen;
		if (!name.length || ![name hasPrefix:@"Payload/"]) continue;
		if (!is_safe_relpath(name)) {
			if (errOut) *errOut = @"Refusing unsafe path in IPA";
			return NO;
		}

		NSString *outPath = [destDir stringByAppendingPathComponent:name];
		if ([name hasSuffix:@"/"]) {
			[fm createDirectoryAtPath:outPath withIntermediateDirectories:YES attributes:nil error:nil];
			continue;
		}

		if ((uint64_t)lfhOff + 30 > sz || rd32(b + lfhOff) != 0x04034b50u) {
			if (errOut) *errOut = @"Invalid ZIP Local File Header";
			return NO;
		}
		uint16_t lNameLen = rd16(b + lfhOff + 26);
		uint16_t lExtraLen = rd16(b + lfhOff + 28);
		size_t dataOff = (size_t)lfhOff + 30 + lNameLen + lExtraLen;
		if (dataOff + compSz > sz) {
			if (errOut) *errOut = @"ZIP entry data exceeds archive size";
			return NO;
		}

		[fm createDirectoryAtPath:[outPath stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
		mode_t unixMode = (mode_t)((extAttr >> 16) & 07777);

		if ((unixMode & S_IFMT) == S_IFLNK) {
			if (method == 0 && uncompSz < PATH_MAX) {
				NSString *target = [[NSString alloc] initWithBytes:b + dataOff length:uncompSz encoding:NSUTF8StringEncoding];
				if (target.length && ![target hasPrefix:@"/"] && ![target containsString:@".."]) {
					unlink(outPath.fileSystemRepresentation);
					symlink(target.fileSystemRepresentation, outPath.fileSystemRepresentation);
				}
			}
			continue;
		}

		FILE *fp = fopen(outPath.fileSystemRepresentation, "wb");
		if (!fp) {
			if (errOut) *errOut = [NSString stringWithFormat:@"Cannot write %@", name];
			return NO;
		}
		if (method == 0) {
			if (compSz > 0 && fwrite(b + dataOff, 1, compSz, fp) != compSz) {
				fclose(fp);
				if (errOut) *errOut = @"Disk write failed";
				return NO;
			}
		} else if (method == 8) {
			z_stream strm = {0};
			strm.next_in = (Bytef *)(b + dataOff);
			strm.avail_in = compSz;
			if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
				fclose(fp);
				if (errOut) *errOut = @"zlib init failed";
				return NO;
			}
			uint8_t obuf[32768];
			int zrc = Z_OK;
			while (zrc == Z_OK) {
				strm.next_out = obuf;
				strm.avail_out = sizeof(obuf);
				zrc = inflate(&strm, Z_NO_FLUSH);
				size_t have = sizeof(obuf) - strm.avail_out;
				if (have > 0 && fwrite(obuf, 1, have, fp) != have) {
					inflateEnd(&strm);
					fclose(fp);
					if (errOut) *errOut = @"Disk write failed during inflate";
					return NO;
				}
			}
			inflateEnd(&strm);
			if (zrc != Z_STREAM_END) {
				fclose(fp);
				if (errOut) *errOut = [NSString stringWithFormat:@"Corrupt DEFLATE stream in %@", name];
				return NO;
			}
		} else {
			fclose(fp);
			if (errOut) *errOut = [NSString stringWithFormat:@"Unsupported ZIP compression method %u", method];
			return NO;
		}
		fclose(fp);
		chmod(outPath.fileSystemRepresentation, (unixMode & 0777) ? (unixMode & 0777) : 0644);
	}
	return YES;
}

static void depac_and_sign_bundle_machos(NSString *appDir, NSString *ldidPath)
{
	NSString *codesignPath = ldidPath ? nil : find_tool(@"codesign");
	char *paths[] = {(char *)appDir.fileSystemRepresentation, NULL};
	FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!fts) return;

	uint64_t totalFiles = 0;
	uint64_t depacFiles = 0;
	uint64_t totalInsns = 0;
	uint64_t totalPages = 0;

	FTSENT *ent;
	while ((ent = fts_read(fts)) != NULL) {
		if (ent->fts_info != FTS_F) continue;
		BOOL enc = NO;
		uint64_t fileInsns = 0;
		uint64_t filePages = 0;
		/* 1. Run native De-PAC + in-place CS_CodeDirectory page rehash BEFORE ad-hoc signing */
		if (!inspect_or_depac_macho(ent->fts_path, YES, &enc, &fileInsns, &filePages)) continue;
		totalFiles++;
		if (fileInsns > 0) {
			depacFiles++;
			totalInsns += fileInsns;
			totalPages += filePages;
		}
		chmod(ent->fts_path, 0755);
		if (enc) continue;

		/* 2. Re-sign with ldid (preserving embedded XML entitlements) when available */
		NSString *filePath = [NSString stringWithUTF8String:ent->fts_path];
		if (!filePath) continue;
		if (ldidPath) {
			NSData *entXml = nil;
			int rc = run_cmd(ldidPath, @[@"-e", filePath], &entXml);
			if (rc == 0 && entXml.length > 32 &&
			    memmem(entXml.bytes, entXml.length, "<plist", 6) != NULL) {
				NSString *tmpEnt = [NSTemporaryDirectory() stringByAppendingPathComponent:
				    [NSString stringWithFormat:@"ent-%@.xml", NSUUID.UUID.UUIDString]];
				if ([entXml writeToFile:tmpEnt atomically:YES]) {
					run_cmd(ldidPath, @[[@"-S" stringByAppendingString:tmpEnt], filePath], NULL);
					unlink(tmpEnt.fileSystemRepresentation);
					continue;
				}
			}
			run_cmd(ldidPath, @[@"-S", filePath], NULL);
		} else if (codesignPath) {
			run_cmd(codesignPath, @[@"--force", @"--sign", @"-", filePath], NULL);
		}
	}
	fts_close(fts);

	if (totalInsns > 0) {
		fprintf(stderr, "sapphire-ipa-helper: De-PAC rewrote %llu instructions (%llu CS pages rehashed) across %llu/%llu Mach-O binaries\n",
		        (unsigned long long)totalInsns, (unsigned long long)totalPages,
		        (unsigned long long)depacFiles, (unsigned long long)totalFiles);
	}
}

static void chown_recursive(NSString *rootPath, uid_t uid, gid_t gid)
{
	char *paths[] = {(char *)rootPath.fileSystemRepresentation, NULL};
	FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!fts) return;
	FTSENT *ent;
	while ((ent = fts_read(fts)) != NULL) {
		if (ent->fts_info == FTS_DP) continue;
		lchown(ent->fts_path, uid, gid);
	}
	fts_close(fts);
}

static NSString *find_existing_container_in(NSString *rootDir, NSString *bundleID, NSString **uuidOut)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	for (NSString *d in [fm contentsOfDirectoryAtPath:rootDir error:nil]) {
		NSString *c = [rootDir stringByAppendingPathComponent:d];
		NSString *meta = [c stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
		NSDictionary *dict = [NSDictionary dictionaryWithContentsOfFile:meta];
		if ([dict[@"MCMMetadataIdentifier"] isKindOfClass:NSString.class] &&
		    [dict[@"MCMMetadataIdentifier"] isEqualToString:bundleID]) {
			if (uuidOut) *uuidOut = dict[@"MCMMetadataUUID"] ?: d;
			return c;
		}
	}
	return nil;
}

static NSString *find_existing_bundle_container(NSString *bundleID, NSString **uuidOut)
{
	return find_existing_container_in(BUNDLE_ROOT, bundleID, uuidOut);
}

static NSString *ensure_app_data_container(NSString *bundleID)
{
	/* Try MobileContainerManager dynamically first so containermanagerd tracks the sandbox */
	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	Class mcmCls = NSClassFromString(@"MCMAppDataContainer");
	SEL selCreate = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");
	SEL selUrl = sel_registerName("url");
	if (mcmCls && [mcmCls respondsToSelector:selCreate]) {
		id mcmObj = ((id (*)(id, SEL, id, BOOL, BOOL *, void *))objc_msgSend)(
		    (id)mcmCls, selCreate, bundleID, YES, NULL, NULL);
		if (mcmObj && [mcmObj respondsToSelector:selUrl]) {
			NSURL *u = ((id (*)(id, SEL))objc_msgSend)(mcmObj, selUrl);
			if ([u isKindOfClass:NSURL.class] && u.path.length) {
				return u.path;
			}
		}
	}

	/* Fallback: reuse or create /private/var/mobile/Containers/Data/Application/<UUID> */
	NSString *dataUUID = nil;
	NSString *existing = find_existing_container_in(DATA_ROOT, bundleID, &dataUUID);
	if (existing) return existing;

	NSFileManager *fm = NSFileManager.defaultManager;
	dataUUID = NSUUID.UUID.UUIDString.uppercaseString;
	NSString *dataDir = [DATA_ROOT stringByAppendingPathComponent:dataUUID];
	NSArray<NSString *> *subdirs = @[
		dataDir,
		[dataDir stringByAppendingPathComponent:@"Documents"],
		[dataDir stringByAppendingPathComponent:@"Library"],
		[dataDir stringByAppendingPathComponent:@"Library/Caches"],
		[dataDir stringByAppendingPathComponent:@"Library/Preferences"],
		[dataDir stringByAppendingPathComponent:@"SystemData"],
		[dataDir stringByAppendingPathComponent:@"tmp"],
	];
	for (NSString *sd in subdirs) {
		[fm createDirectoryAtPath:sd withIntermediateDirectories:YES attributes:nil error:nil];
		chmod(sd.fileSystemRepresentation, 0755);
	}
	NSDictionary *meta = @{
		@"MCMMetadataContentClass": @2,
		@"MCMMetadataIdentifier": bundleID,
		@"MCMMetadataUUID": dataUUID,
		@"MCMMetadataVersion": @1,
	};
	NSData *metaBin = [NSPropertyListSerialization dataWithPropertyList:meta
	                                                             format:NSPropertyListBinaryFormat_v1_0
	                                                            options:0
	                                                              error:nil];
	NSString *metaPath = [dataDir stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
	[metaBin writeToFile:metaPath atomically:YES];
	chmod(metaPath.fileSystemRepresentation, 0644);
	chown_recursive(dataDir, MOBILE_UID, MOBILE_GID);
	lchown(metaPath.fileSystemRepresentation, INSTALLD_UID, INSTALLD_GID);
	return dataDir;
}

static id get_ls_workspace(void)
{
	Class lsCls = NSClassFromString(@"LSApplicationWorkspace");
	if (!lsCls) {
		dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
		lsCls = NSClassFromString(@"LSApplicationWorkspace");
	}
	if (!lsCls) {
		dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
		lsCls = NSClassFromString(@"LSApplicationWorkspace");
	}
	SEL selDefault = sel_registerName("defaultWorkspace");
	if (lsCls && [lsCls respondsToSelector:selDefault]) {
		return ((id (*)(id, SEL))objc_msgSend)((id)lsCls, selDefault);
	}
	return nil;
}

static void post_launchservices_notifications(BOOL installed)
{
	if (installed) {
		notify_post("com.apple.LaunchServices.applicationRegistered");
		notify_post("com.apple.mobile.application_installed");
	} else {
		notify_post("com.apple.LaunchServices.applicationUnregistered");
		notify_post("com.apple.mobile.application_uninstalled");
	}
	notify_post("com.apple.LaunchServices.iconCache");
}

static BOOL register_with_launchservices(NSString *destApp, NSString *container, NSString *bundleID)
{
	id ws = get_ls_workspace();
	if (!ws) return NO;

	NSString *dataContainer = ensure_app_data_container(bundleID);
	NSMutableDictionary *regDict = [NSMutableDictionary dictionary];
	regDict[@"ApplicationType"] = @"User";
	regDict[@"CFBundleIdentifier"] = bundleID;
	regDict[@"CodeInfoIdentifier"] = bundleID;
	regDict[@"Path"] = destApp;
	regDict[@"BundleContainer"] = container;
	regDict[@"IsDeletable"] = @YES;
	if (dataContainer.length) {
		regDict[@"Container"] = dataContainer;
		regDict[@"EnvironmentVariables"] = @{
			@"CFFIXED_USER_HOME": dataContainer,
			@"HOME": dataContainer,
			@"TMPDIR": [dataContainer stringByAppendingPathComponent:@"tmp"],
		};
	}

	/* Collect PlugIns/*.appex descriptors if the bundle embeds extensions */
	NSString *pluginsDir = [destApp stringByAppendingPathComponent:@"PlugIns"];
	NSFileManager *fm = NSFileManager.defaultManager;
	NSArray<NSString *> *pluginItems = [fm contentsOfDirectoryAtPath:pluginsDir error:nil];
	if (pluginItems.count > 0) {
		NSMutableDictionary *pluginsMap = [NSMutableDictionary dictionary];
		Class mcmPluginCls = NSClassFromString(@"MCMPluginKitPluginDataContainer");
		SEL selCreate = sel_registerName("containerWithIdentifier:createIfNecessary:existed:error:");
		SEL selUrl = sel_registerName("url");
		for (NSString *item in pluginItems) {
			if (![item hasSuffix:@".appex"]) continue;
			NSString *pPath = [pluginsDir stringByAppendingPathComponent:item];
			NSDictionary *pInfo = [NSDictionary dictionaryWithContentsOfFile:
			    [pPath stringByAppendingPathComponent:@"Info.plist"]];
			NSString *pBid = [pInfo[@"CFBundleIdentifier"] isKindOfClass:NSString.class]
			                 ? pInfo[@"CFBundleIdentifier"] : nil;
			if (!pBid.length) continue;
			NSMutableDictionary *pDict = [NSMutableDictionary dictionaryWithDictionary:@{
				@"ApplicationType": @"PluginKitPlugin",
				@"CFBundleIdentifier": pBid,
				@"CodeInfoIdentifier": pBid,
				@"Path": pPath,
				@"PluginOwnerBundleID": bundleID,
			}];
			if (mcmPluginCls && [mcmPluginCls respondsToSelector:selCreate]) {
				id pCont = ((id (*)(id, SEL, id, BOOL, BOOL *, void *))objc_msgSend)(
				    (id)mcmPluginCls, selCreate, pBid, YES, NULL, NULL);
				if (pCont && [pCont respondsToSelector:selUrl]) {
					NSURL *pu = ((id (*)(id, SEL))objc_msgSend)(pCont, selUrl);
					if ([pu isKindOfClass:NSURL.class] && pu.path.length) {
						pDict[@"Container"] = pu.path;
					}
				}
			}
			pluginsMap[pBid] = pDict;
		}
		if (pluginsMap.count > 0) {
			regDict[@"_LSBundlePlugins"] = pluginsMap;
		}
	}

	BOOL ok = NO;
	SEL selRegDict = sel_registerName("registerApplicationDictionary:");
	if ([ws respondsToSelector:selRegDict]) {
		ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, selRegDict, regDict);
	}
	SEL selRegURL = sel_registerName("registerApplication:");
	if (!ok && [ws respondsToSelector:selRegURL]) {
		NSURL *appURL = [NSURL fileURLWithPath:destApp isDirectory:YES];
		ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, selRegURL, appURL);
	}
	return ok;
}

static int cmd_install(NSString *ipaPath)
{
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *work = [NSString stringWithFormat:@"/var/tmp/sapphire-ipa-%@", NSUUID.UUID.UUIDString];
	[fm createDirectoryAtPath:work withIntermediateDirectories:YES attributes:nil error:nil];

	NSString *err = nil;
	if (!extract_ipa_zip(ipaPath, work, &err)) {
		fprintf(stderr, "sapphire-ipa-helper: %s\n", err.UTF8String);
		[fm removeItemAtPath:work error:nil];
		return 1;
	}

	NSString *payload = [work stringByAppendingPathComponent:@"Payload"];
	NSString *appName = nil;
	for (NSString *item in [fm contentsOfDirectoryAtPath:payload error:nil]) {
		if ([item hasSuffix:@".app"]) {
			appName = item;
			break;
		}
	}
	if (!appName) {
		fprintf(stderr, "sapphire-ipa-helper: Payload/*.app not found in IPA\n");
		[fm removeItemAtPath:work error:nil];
		return 1;
	}

	NSString *stagedApp = [payload stringByAppendingPathComponent:appName];
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[stagedApp stringByAppendingPathComponent:@"Info.plist"]];
	NSString *bundleID = [info[@"CFBundleIdentifier"] isKindOfClass:NSString.class] ? info[@"CFBundleIdentifier"] : nil;
	NSString *execName = [info[@"CFBundleExecutable"] isKindOfClass:NSString.class] ? info[@"CFBundleExecutable"] : nil;
	if (!bundleID.length || !execName.length) {
		fprintf(stderr, "sapphire-ipa-helper: missing CFBundleIdentifier or CFBundleExecutable\n");
		[fm removeItemAtPath:work error:nil];
		return 1;
	}

	NSString *mainBin = [stagedApp stringByAppendingPathComponent:execName];
	if (![fm fileExistsAtPath:mainBin]) {
		fprintf(stderr, "sapphire-ipa-helper: main executable missing (%s)\n", execName.UTF8String);
		[fm removeItemAtPath:work error:nil];
		return 1;
	}
	BOOL encrypted = NO;
	if (is_macho_and_check_crypt(mainBin.fileSystemRepresentation, &encrypted) && encrypted) {
		fprintf(stderr, "sapphire-ipa-helper: FairPlay-encrypted binary (%s)\n", execName.UTF8String);
		[fm removeItemAtPath:work error:nil];
		return 180;
	}

	chmod(mainBin.fileSystemRepresentation, 0755);
	depac_and_sign_bundle_machos(stagedApp, find_tool(@"ldid"));

	[fm createDirectoryAtPath:BUNDLE_ROOT withIntermediateDirectories:YES attributes:nil error:nil];
	NSString *uuid = nil;
	NSString *container = find_existing_bundle_container(bundleID, &uuid);
	if (!container) {
		uuid = NSUUID.UUID.UUIDString.uppercaseString;
		container = [BUNDLE_ROOT stringByAppendingPathComponent:uuid];
		[fm createDirectoryAtPath:container withIntermediateDirectories:YES attributes:nil error:nil];
	} else {
		for (NSString *old in [fm contentsOfDirectoryAtPath:container error:nil]) {
			if ([old hasSuffix:@".app"]) {
				[fm removeItemAtPath:[container stringByAppendingPathComponent:old] error:nil];
			}
		}
	}

	NSString *destApp = [container stringByAppendingPathComponent:appName];
	NSError *mvErr = nil;
	if (![fm moveItemAtPath:stagedApp toPath:destApp error:&mvErr]) {
		fprintf(stderr, "sapphire-ipa-helper: move failed: %s\n", mvErr.localizedDescription.UTF8String);
		[fm removeItemAtPath:work error:nil];
		return 1;
	}

	NSDictionary *meta = @{
		@"MCMMetadataContentClass": @1,
		@"MCMMetadataIdentifier": bundleID,
		@"MCMMetadataUUID": uuid ?: NSUUID.UUID.UUIDString.uppercaseString,
		@"MCMMetadataVersion": @1,
	};
	NSData *metaBin = [NSPropertyListSerialization dataWithPropertyList:meta format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
	NSString *metaPath = [container stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"];
	[metaBin writeToFile:metaPath atomically:YES];
	chmod(container.fileSystemRepresentation, 0755);
	chmod(metaPath.fileSystemRepresentation, 0644);
	chown_recursive(container, INSTALLD_UID, INSTALLD_GID);
	[fm removeItemAtPath:work error:nil];

	/* 1. Direct LaunchServices registration via LSApplicationWorkspace */
	BOOL lsRegistered = register_with_launchservices(destApp, container, bundleID);

	/* 2. Also run /var/jb/usr/bin/uicache -p <destApp> when available */
	NSString *uicache = [fm isExecutableFileAtPath:@"/var/jb/usr/bin/uicache"]
	                    ? @"/var/jb/usr/bin/uicache"
	                    : find_tool(@"uicache");
	if (uicache) {
		NSData *ucOut = nil;
		int ucRc = run_cmd(uicache, @[@"-p", destApp], &ucOut);
		if (ucRc != 0) {
			fprintf(stderr, "sapphire-ipa-helper: uicache returned %d\n", ucRc);
			if (!lsRegistered) {
				return ucRc;
			}
		}
	}

	/* 3. Broadcast modern + legacy Darwin notifications to refresh SpringBoard icons */
	post_launchservices_notifications(YES);

	printf("Installed %s -> %s\n", bundleID.UTF8String, destApp.UTF8String);
	return 0;
}

static int cmd_uninstall(NSString *bundleID)
{
	NSString *container = find_existing_bundle_container(bundleID, NULL);
	if (!container) {
		fprintf(stderr, "sapphire-ipa-helper: bundle %s not found\n", bundleID.UTF8String);
		return 1;
	}
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *uicache = [fm isExecutableFileAtPath:@"/var/jb/usr/bin/uicache"]
	                    ? @"/var/jb/usr/bin/uicache"
	                    : find_tool(@"uicache");
	id ws = get_ls_workspace();
	SEL selUnreg = sel_registerName("unregisterApplication:");

	for (NSString *item in [fm contentsOfDirectoryAtPath:container error:nil]) {
		if (![item hasSuffix:@".app"]) continue;
		NSString *appPath = [container stringByAppendingPathComponent:item];
		if (uicache) {
			run_cmd(uicache, @[@"-u", appPath], NULL);
		}
		if (ws && [ws respondsToSelector:selUnreg]) {
			NSURL *appURL = [NSURL fileURLWithPath:appPath isDirectory:YES];
			((BOOL (*)(id, SEL, id))objc_msgSend)(ws, selUnreg, appURL);
		}
	}
	if (![fm removeItemAtPath:container error:nil]) {
		fprintf(stderr, "sapphire-ipa-helper: failed to remove %s\n", container.UTF8String);
		return 1;
	}
	post_launchservices_notifications(NO);
	printf("Uninstalled %s\n", bundleID.UTF8String);
	return 0;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		setuid(0);
		setgid(0);
		if (argc >= 3 && strcmp(argv[1], "install") == 0) {
			return cmd_install([NSString stringWithUTF8String:argv[2]]);
		}
		if (argc >= 3 && strcmp(argv[1], "uninstall") == 0) {
			return cmd_uninstall([NSString stringWithUTF8String:argv[2]]);
		}
		fprintf(stderr, "usage: sapphire-ipa-helper install <path.ipa> [force] | uninstall <bundleID>\n");
		return 2;
	}
}
