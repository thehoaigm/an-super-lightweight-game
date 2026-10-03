#pragma once
// Deterministic RNG: splitmix64 (seeding) + xoshiro256** (stream).
// No <random>: std distributions are NOT portable across standard libraries,
// which would break reproducible worlds and saves.
#include <array>
#include <cstdint>

namespace cx {

constexpr uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

enum class SeedDomain : uint64_t { World = 1, Region = 2, Chunk = 3, Npc = 4, Dungeon = 5, Loot = 6, Event = 7 };

// Pure function (parent, domain, index) -> child seed. Never depends on call order,
// so any region/chunk/NPC can be regenerated on demand and always comes out identical.
constexpr uint64_t derive_seed(uint64_t parent, SeedDomain domain, uint64_t index) {
    uint64_t s = parent ^ (static_cast<uint64_t>(domain) * 0xD6E8FEB86659FD93ull);
    const uint64_t mixed = splitmix64(s);
    s = mixed ^ (index * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull);
    return splitmix64(s);
}

class Rng {
public:
    explicit Rng(uint64_t seed = 0) { reseed(seed); }

    void reseed(uint64_t seed) {
        uint64_t sm = seed;
        for (auto& w : s_) w = splitmix64(sm);
    }

    uint64_t next_u64() {
        const uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, n). Rejection sampling, no modulo bias. n must be > 0.
    uint64_t below(uint64_t n) {
        const uint64_t threshold = (0 - n) % n;  // 2^64 mod n
        for (;;) {
            const uint64_t r = next_u64();
            if (r >= threshold) return r % n;
        }
    }

    // Uniform in [lo, hi], inclusive. Requires lo <= hi.
    int64_t range(int64_t lo, int64_t hi) { return lo + int64_t(below(uint64_t(hi - lo) + 1)); }

    bool chance_permille(uint32_t p) { return below(1000) < p; }

    std::array<uint64_t, 4> state() const { return {s_[0], s_[1], s_[2], s_[3]}; }
    void set_state(const std::array<uint64_t, 4>& st) {
        for (int i = 0; i < 4; ++i) s_[i] = st[i];
        if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) s_[0] = 1;  // all-zero is a fixed point
    }

private:
    static constexpr uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t s_[4];
};

}  // namespace cx
