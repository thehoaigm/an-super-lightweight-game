#include "core/save.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "core/bytes.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cx::save {

namespace {
constexpr uint32_t kMagic = 0x56535843;  // bytes 'C','X','S','V' little-endian
constexpr size_t kHeaderBytes = 16;
}  // namespace

std::vector<uint8_t> make_container(uint32_t version, const std::vector<uint8_t>& payload) {
    ByteWriter w;
    w.u32(kMagic);
    w.u32(version);
    w.u32(uint32_t(payload.size()));
    w.u32(crc32(payload.data(), payload.size()));
    w.bytes(payload.data(), payload.size());
    return w.take();
}

ParseStatus parse_container(const std::vector<uint8_t>& file, uint32_t& version, std::vector<uint8_t>& payload,
                            std::string& err) {
    ByteReader r(file);
    const uint32_t magic = r.u32();
    const uint32_t ver = r.u32();
    const uint32_t size = r.u32();
    const uint32_t crc = r.u32();
    if (!r.ok() || magic != kMagic) {
        err = "not a save file (bad header)";
        return ParseStatus::Corrupt;
    }
    if (file.size() != kHeaderBytes + size_t(size)) {
        err = "save file is truncated or has trailing data";
        return ParseStatus::Corrupt;
    }
    if (crc32(file.data() + kHeaderBytes, size) != crc) {
        err = "save file checksum mismatch (corrupted)";
        return ParseStatus::Corrupt;
    }
    if (ver == 0 || ver > kSchemaVersion) {
        err = "save was written by a newer version (schema v" + std::to_string(ver) +
              "); this build supports up to v" + std::to_string(kSchemaVersion);
        return ParseStatus::NewerVersion;
    }
    version = ver;
    payload.assign(file.begin() + kHeaderBytes, file.end());
    return ParseStatus::Ok;
}

bool write_atomic(const std::string& path, const std::vector<uint8_t>& data, std::string& err) {
    namespace fs = std::filesystem;
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot open " + tmp + " for writing";
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = ok && std::fflush(f) == 0;
#if defined(__unix__) || defined(__APPLE__)
    ok = ok && ::fsync(::fileno(f)) == 0;  // data must be on disk before it replaces the old save
#endif
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (!ok) {
        fs::remove(tmp, ec);
        err = "write failed (disk full or I/O error)";
        return false;
    }
    if (fs::exists(path, ec)) {
        fs::rename(path, path + ".bak", ec);  // keep the previous save as the fallback
        if (ec) {
            fs::remove(tmp, ec);
            err = "cannot rotate old save to .bak";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        err = "cannot move new save into place: " + ec.message();
        return false;
    }
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) err = "read error on " + path;
    return ok;
}

}  // namespace cx::save
