#include "hakios-metal/metallib.hpp"

#include <cstring>

namespace hakios::metal {

namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | uint64_t(be32(p + 4)); }
// [offset, offset + length) inside SIZE, without the sum wrapping
bool inside(uint64_t offset, uint64_t length, uint64_t size) { return offset <= size && length <= size - offset; }

constexpr size_t kHeader = 0x58;
constexpr uint32_t kFat = 0xcafebabeu;
constexpr uint32_t kFat64 = 0xcafebabfu;
constexpr uint32_t kAir64 = 0x01000017u;
constexpr uint32_t kBitcodeWrapper = 0x0B17C0DEu;

bool isFourCC(const uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        if (p[i] < 0x20 || p[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

bool isLargeChunkTag(const uint8_t* p) {
    return memcmp(p, "RFLT", 4) == 0 || memcmp(p, "DEBI", 4) == 0 ||
           memcmp(p, "BSYM", 4) == 0 || memcmp(p, "SMTH", 4) == 0;
}

// Parses one function entry's TLV tags in [start, entryEnd).
// mode 0: standard u16 tag lengths; mode 1: u16 with u32 on reflection/debug chunks; mode 2: u32 tag lengths.
bool parseEntryTags(const uint8_t* data, uint64_t start, uint64_t entryEnd, int mode, MetalFunction* out,
                    bool* haveName, bool* haveOffset, bool* haveSize, const char** why) {
    MetalFunction f;
    bool gotName = false, gotOffset = false, gotSize = false, sawEndt = false;
    for (uint64_t q = start; inside(q, 4, entryEnd);) {
        if (!isFourCC(data + q)) {
            *why = "a function's entry holds a malformed 4-CC tag";
            return false;
        }
        if (memcmp(data + q, "ENDT", 4) == 0) {
            for (uint64_t pad = q + 4; pad < entryEnd; pad++) {
                if (data[pad] != 0) {
                    *why = "trailing non-zero bytes follow ENDT in a function's entry";
                    return false;
                }
            }
            sawEndt = true;
            break;
        }
        uint64_t w = (mode == 2 || (mode == 1 && isLargeChunkTag(data + q))) ? 4 : 2;
        if (!inside(q, 4 + w, entryEnd)) {
            *why = "a tag runs past the end of its function's entry";
            return false;
        }
        uint64_t length = (w == 4) ? uint64_t(u32(data + q + 4)) : uint64_t(u16(data + q + 4));
        if (!inside(q + 4 + w, length, entryEnd)) {
            *why = "a tag's value runs past the end of its function's entry";
            return false;
        }
        const uint8_t* v = data + q + 4 + w;
        if (memcmp(data + q, "NAME", 4) == 0) {
            size_t n = size_t(length);
            while (n > 0 && v[n - 1] == 0) {
                n--;
            }
            if (n > 0) {
                f.name.assign(reinterpret_cast<const char*>(v), n);
                gotName = true;
            }
        } else if (memcmp(data + q, "TYPE", 4) == 0 && length >= 1) {
            f.type = v[0];
        } else if (memcmp(data + q, "VERS", 4) == 0 && length >= 4) {
            int count = length >= 8 ? 4 : 2;
            for (int k = 0; k < count; k++) {
                f.air[k] = u16(v + 2 * k);
            }
        } else if (memcmp(data + q, "OFFT", 4) == 0) {
            if (length >= 24) {
                f.moduleOffset = u64(v + 16);
                gotOffset = true;
            } else if (length >= 12) {
                f.moduleOffset = u32(v + 8);
                gotOffset = true;
            }
        } else if (memcmp(data + q, "MDSZ", 4) == 0) {
            if (length >= 8) {
                f.moduleSize = u64(v);
                gotSize = true;
            } else if (length >= 4) {
                f.moduleSize = u32(v);
                gotSize = true;
            }
        }
        q += 4 + w + length;
    }
    if (!sawEndt) {
        *why = "a function's entry has no ENDT terminator";
        return false;
    }
    *out = std::move(f);
    *haveName = gotName;
    *haveOffset = gotOffset;
    *haveSize = gotSize;
    return true;
}
}  // namespace

const MetalFunction* MetalLibrary::find(std::string_view name) const {
    for (const MetalFunction& f : functions_) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

bool MetalLibrary::parse(const uint8_t* data, size_t size) {
    functions_.clear();
    why_.clear();
    data_ = nullptr;
    size_ = 0;
    version_[0] = version_[1] = 0;
    formatVersion_[0] = formatVersion_[1] = 0;
    if (data == nullptr) {
        return refuse("not a metallib (no MTLB at the start, or shorter than its header)");
    }
    // a universal file (32-bit 0xCAFEBABE or 64-bit 0xCAFEBABF): the AIR64 slice, which a non-Apple GPU can compile
    if (size >= 8 && (be32(data) == kFat || be32(data) == kFat64)) {
        bool fat64 = be32(data) == kFat64;
        uint32_t count = be32(data + 4);
        uint64_t stride = fat64 ? 32 : 20;
        bool found = false;
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint64_t at = 8 + stride * i;
            if (!inside(at, stride, size)) {
                break;
            }
            if (be32(data + at) == kAir64) {
                uint64_t offset = fat64 ? be64(data + at + 8) : uint64_t(be32(data + at + 8));
                uint64_t length = fat64 ? be64(data + at + 16) : uint64_t(be32(data + at + 12));
                if (!inside(offset, length, size)) {
                    return refuse("a universal metallib whose AIR64 slice is not inside it");
                }
                data += offset;
                size = size_t(length);
                found = true;
                break;
            }
        }
        if (!found) {
            return refuse("a universal metallib with no AIR64 slice (only code for Apple's own GPUs)");
        }
    }
    data_ = data;
    size_ = size;
    if (size < kHeader || memcmp(data, "MTLB", 4) != 0) {
        return refuse("not a metallib (no MTLB at the start, or shorter than its header)");
    }
    // Bytes 4..15 carry platform/flags and container/OS versions across MTLB v1.x, v2.x, and v3+ (Metal 3.2+ / Metal 4).
    // Accept all container major versions >= 1 (and synthetic 0) as long as structural section bounds hold.
    version_[0] = u16(data + 4);
    version_[1] = u16(data + 6);
    formatVersion_[0] = u16(data + 6);
    formatVersion_[1] = u16(data + 8);
    if (formatVersion_[0] == 0 && u16(data + 8) != 0 && u32(data + 12) == 0) {
        formatVersion_[0] = u16(data + 8);
        formatVersion_[1] = u16(data + 10);
    }

    uint64_t fileSize = u64(data + 0x10);
    if (fileSize != 0 && (fileSize < kHeader || fileSize > size)) {
        return refuse("the metallib header's file size is not inside the buffer");
    }
    uint64_t list = u64(data + 0x18), listLength = u64(data + 0x20);
    uint64_t pub = u64(data + 0x28), pubLength = u64(data + 0x30);
    uint64_t priv = u64(data + 0x38), privLength = u64(data + 0x40);
    uint64_t modules = u64(data + 0x48), modulesLength = u64(data + 0x50);
    if (list < kHeader || !inside(list, 4, size)) {
        return refuse("the function list is not inside the file");
    }
    uint64_t end = 0;
    if (inside(list + 4, listLength, size)) {
        end = list + 4 + listLength;
    } else if (listLength >= 4 && inside(list, listLength, size)) {
        end = list + listLength;
    } else {
        return refuse("the function list is not inside the file");
    }
    if ((pubLength > 0 && (pub < kHeader || !inside(pub, pubLength, size))) ||
        (privLength > 0 && (priv < kHeader || !inside(priv, privLength, size)))) {
        return refuse("the metadata sections are not inside the file");
    }
    if ((modulesLength > 0 && modules < kHeader) || !inside(modules, modulesLength, size)) {
        return refuse("the modules are not inside the file");
    }
    uint64_t p = list + 4;
    uint32_t count = u32(data + list);
    for (uint32_t i = 0; i < count; i++) {
        if (!inside(p, 4, end)) {
            return refuse("the function list ends before its count of functions");
        }
        uint64_t entry = u32(data + p);
        if (entry < 8 || !inside(p, entry, end)) {
            return refuse("a function's entry is not inside the function list");
        }
        uint64_t entryEnd = p + entry;
        MetalFunction f;
        bool haveOffset = false, haveSize = false, haveName = false;
        const char* tagErr = "a tag runs past the end of its function's entry";
        bool ok = parseEntryTags(data, p + 4, entryEnd, 0, &f, &haveName, &haveOffset, &haveSize, &tagErr);
        if (!ok) {
            const char* unusedErr = nullptr;
            ok = parseEntryTags(data, p + 4, entryEnd, 1, &f, &haveName, &haveOffset, &haveSize, &unusedErr) ||
                 parseEntryTags(data, p + 4, entryEnd, 2, &f, &haveName, &haveOffset, &haveSize, &unusedErr);
        }
        if (!ok) {
            return refuse(tagErr);
        }
        if (!haveName || !haveOffset || !haveSize) {
            return refuse("a function has no NAME, OFFT or MDSZ");
        }
        if (f.moduleSize == 0 || !inside(f.moduleOffset, f.moduleSize, modulesLength)) {
            return refuse("a function's module is not inside the modules");
        }
        f.moduleOffset += modules;
        functions_.push_back(std::move(f));
        p = entryEnd;
    }
    return true;
}

bool MetalLibrary::bitcode(const MetalFunction& f, const uint8_t** bytes, uint64_t* length) const {
    if (data_ == nullptr || bytes == nullptr || length == nullptr || !inside(f.moduleOffset, f.moduleSize, size_)) {
        return false;
    }
    const uint8_t* m = data_ + f.moduleOffset;
    if (f.moduleSize >= 16 && u32(m) == kBitcodeWrapper) {
        uint64_t offset = u32(m + 8), wrapped = u32(m + 12);
        if (wrapped < 4 || !inside(offset, wrapped, f.moduleSize)) {
            return false;
        }
        if (memcmp(m + offset, "BC\xc0\xde", 4) != 0) {
            return false;
        }
        *bytes = m + offset;
        *length = wrapped;
        return true;
    }
    if (f.moduleSize >= 4 && memcmp(m, "BC\xc0\xde", 4) == 0) {
        *bytes = m;
        *length = f.moduleSize;
        return true;
    }
    return false;
}

}  // namespace hakios::metal
