#include "sim/world.hpp"

#include "core/rng.hpp"

namespace cx {

namespace {

// Compiled-in defaults. This table is the seam where a data loader (data/biomes.*) plugs in later;
// when it does, a content hash must go into the save next to kWorldGenVersion.
struct BiomeDef {
    const char* name;
    const char* noun;  // appended to generated region names
    int qi_min, qi_max;
    int weight;        // how common the biome is
    int loc_w[6];      // location weights: Village, Cave, SpiritSpring, HerbField, Ruin, Shrine
};

const BiomeDef kBiomes[] = {
    {"Plains", "Binh Nguyen", 200, 450, 20, {5, 1, 1, 4, 1, 2}},
    {"Forest", "Moc Lam", 300, 600, 22, {2, 2, 2, 6, 1, 1}},
    {"Mountain", "Son Mach", 400, 800, 18, {1, 5, 3, 2, 2, 2}},
    {"Desert", "Hoang Mac", 100, 350, 10, {1, 2, 1, 0, 5, 1}},
    {"Sea", "Hai Vuc", 250, 600, 8, {1, 2, 2, 1, 3, 1}},
    {"Wasteland", "Tu Dia", 50, 300, 10, {0, 2, 0, 0, 6, 1}},
    {"ForbiddenLand", "Cam Dia", 700, 1000, 4, {0, 3, 2, 1, 4, 3}},
};
static_assert(sizeof(kBiomes) / sizeof(kBiomes[0]) == size_t(Biome::Count), "one def per biome");

const char* const kLocationNames[] = {"Village", "Cave", "SpiritSpring", "HerbField", "Ruin", "Shrine"};
static_assert(sizeof(kLocationNames) / sizeof(kLocationNames[0]) == size_t(LocationType::Count), "one name per type");

const char* const kNamePrefix[] = {"Thanh", "Huyen", "Xich", "Bach", "Hac", "Kim", "Thien", "Dia"};
const char* const kNameMid[] = {"Van", "Phong", "Lam", "Hai", "Sa", "Hoa", "Bang", "Loi"};

int pick_weighted(Rng& r, const int* w, int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0) return 0;
    int roll = int(r.below(uint64_t(total)));
    for (int i = 0; i < n; ++i) {
        if (roll < w[i]) return i;
        roll -= w[i];
    }
    return n - 1;
}

}  // namespace

const char* biome_name(Biome b) { return kBiomes[size_t(b)].name; }
const char* location_name(LocationType t) { return kLocationNames[size_t(t)]; }

RegionStatic generate_region(uint64_t world_seed, uint32_t index) {
    RegionStatic r;
    r.seed = derive_seed(world_seed, SeedDomain::Region, index);
    Rng rng(r.seed);
    int weights[size_t(Biome::Count)];
    for (size_t i = 0; i < size_t(Biome::Count); ++i) weights[i] = kBiomes[i].weight;
    const int b = pick_weighted(rng, weights, int(Biome::Count));
    const BiomeDef& def = kBiomes[b];
    r.biome = Biome(b);
    r.base_qi = int32_t(rng.range(def.qi_min, def.qi_max));
    const char* p = kNamePrefix[rng.below(8)];
    const char* m = kNameMid[rng.below(8)];
    r.name = std::string(p) + " " + m + " " + def.noun;
    return r;
}

Chunk generate_chunk(const RegionStatic& region, int cx, int cy) {
    Chunk c;
    c.cx = cx;
    c.cy = cy;
    Rng rng(derive_seed(region.seed, SeedDomain::Chunk, uint64_t(cy) * kChunkGrid + uint64_t(cx)));
    const BiomeDef& def = kBiomes[size_t(region.biome)];
    const uint64_t count = rng.below(4);  // 0..3 locations per chunk
    for (uint64_t i = 0; i < count; ++i) {
        Location l;
        l.type = LocationType(pick_weighted(rng, def.loc_w, int(LocationType::Count)));
        l.x = uint8_t(rng.below(256));
        l.y = uint8_t(rng.below(256));
        c.locations.push_back(l);
    }
    return c;
}

}  // namespace cx
