#pragma once
// World = Region -> Chunk -> Location.
//   RegionStatic : derived purely from (world seed, region index). Never saved.
//   RegionState  : the only mutable per-region data. This is what a save stores.
//   Chunk        : generated on demand from the region seed and thrown away. Never stored,
//                  so RAM does not grow with world size. (Later: save only player-made deltas.)
#include <cstdint>
#include <string>
#include <vector>
#include "core/entity.hpp"

namespace cx {

constexpr uint32_t kWorldGenVersion = 1;  // bump whenever generation output changes; saves check it

constexpr int kChunkGrid = 8;        // chunks per region side
constexpr int32_t kQiScale = 256;    // qi is fixed-point (qi_fp = qi * 256): no floats in simulation
constexpr int32_t kQiMax = 1500;     // hard clamp, in qi units

enum class Biome : uint8_t { Plains, Forest, Mountain, Desert, Sea, Wasteland, ForbiddenLand, Count };
enum class LocationType : uint8_t { Village, Cave, SpiritSpring, HerbField, Ruin, Shrine, Count };

struct Location {
    LocationType type;
    uint8_t x, y;  // position inside the chunk, 0..255
};

struct Chunk {
    int cx = 0, cy = 0;
    std::vector<Location> locations;
};

struct RegionStatic {
    uint64_t seed = 0;
    Biome biome = Biome::Plains;
    int32_t base_qi = 0;  // equilibrium qi density in qi units (before season modifier)
    std::string name;
};

struct RegionState {
    EntityId id;
    int32_t qi_fp = 0;
    uint32_t ticks = 0;       // how many times this region has been simulated
    uint32_t tick_epoch = 0;  // bumped when the region's LOD changes; stale scheduled ticks are dropped
    int64_t last_tick = 0;    // minute of the last simulation step (for elapsed-time scaling)
    uint32_t anomalies = 0;   // monthly qi surges/droughts this region has had
};

struct World {
    std::vector<RegionStatic> statics;
    std::vector<RegionState> states;
    uint32_t focus = 0;  // region the player is in: simulated in high detail
};

const char* biome_name(Biome b);
const char* location_name(LocationType t);

RegionStatic generate_region(uint64_t world_seed, uint32_t index);
Chunk generate_chunk(const RegionStatic& region, int cx, int cy);

}  // namespace cx
