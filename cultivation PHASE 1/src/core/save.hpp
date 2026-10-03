#pragma once
// Save file container: [magic "CXSV"][schema_version][payload_size][crc32(payload)][payload].
// Writes are crash-safe: tmp file -> fsync -> old save becomes .bak -> tmp renamed into place.
// A crash at any point leaves either the new save, or the old one (as path or path.bak).
#include <cstdint>
#include <string>
#include <vector>

namespace cx::save {

constexpr uint32_t kSchemaVersion = 1;

enum class ParseStatus { Ok, Corrupt, NewerVersion };

std::vector<uint8_t> make_container(uint32_t version, const std::vector<uint8_t>& payload);
ParseStatus parse_container(const std::vector<uint8_t>& file, uint32_t& version, std::vector<uint8_t>& payload,
                            std::string& err);

bool write_atomic(const std::string& path, const std::vector<uint8_t>& data, std::string& err);
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err);

}  // namespace cx::save
