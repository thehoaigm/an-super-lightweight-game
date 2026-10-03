#pragma once
// Explicit little-endian binary I/O + checksums. Output is byte-identical on every platform.
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cx {

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back(uint8_t(v >> (8 * i))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void i64(int64_t v) { u64(uint64_t(v)); }
    // LEB128: small numbers cost 1 byte.
    void varuint(uint64_t v) {
        while (v >= 0x80) { buf_.push_back(uint8_t(v) | 0x80); v >>= 7; }
        buf_.push_back(uint8_t(v));
    }
    void bytes(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// Never throws, never reads out of bounds: on overrun ok() turns false and reads return 0.
class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return n_ - pos_; }
    uint8_t u8() { return uint8_t(read(1)); }
    uint32_t u32() { return uint32_t(read(4)); }
    uint64_t u64() { return read(8); }
    int32_t i32() { return int32_t(u32()); }
    int64_t i64() { return int64_t(u64()); }
    uint64_t varuint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const uint8_t b = u8();
            if (!ok_) return 0;
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }

private:
    uint64_t read(int n) {
        if (!ok_ || remaining() < size_t(n)) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(p_[pos_ + size_t(i)]) << (8 * i);
        pos_ += size_t(n);
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

inline uint32_t crc32(const uint8_t* d, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline uint64_t fnv1a64(const uint8_t* d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 0x100000001b3ull; }
    return h;
}

}  // namespace cx
