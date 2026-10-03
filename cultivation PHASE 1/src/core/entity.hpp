#pragma once
// Entity IDs: 32-bit index + 32-bit generation. A stale ID (entity destroyed, slot reused)
// is detected instead of silently pointing at the wrong thing. Raw 0 is "no entity".
#include <cstdint>
#include <vector>
#include "core/bytes.hpp"

namespace cx {

struct EntityId {
    uint64_t raw = 0;
    constexpr uint32_t index() const { return uint32_t(raw); }
    constexpr uint32_t generation() const { return uint32_t(raw >> 32); }
    constexpr bool valid() const { return raw != 0; }
    friend constexpr bool operator==(EntityId a, EntityId b) { return a.raw == b.raw; }
    friend constexpr bool operator!=(EntityId a, EntityId b) { return a.raw != b.raw; }
};

constexpr EntityId make_id(uint32_t index, uint32_t generation) {
    return EntityId{(uint64_t(generation) << 32) | index};
}

enum class EntityKind : uint8_t { None = 0, Region, Location, Npc, Item, Faction, Count };

class EntityAllocator {
public:
    EntityAllocator() : slots_(1) {}  // slot 0 is reserved so that raw 0 is never a valid id

    EntityId create(EntityKind kind) {
        uint32_t idx;
        if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
        else { idx = uint32_t(slots_.size()); slots_.emplace_back(); }
        Slot& s = slots_[idx];
        s.alive = true;
        s.kind = kind;
        ++alive_;
        return make_id(idx, s.gen);
    }

    bool destroy(EntityId id) {
        if (!is_alive(id)) return false;
        Slot& s = slots_[id.index()];
        s.alive = false;
        s.kind = EntityKind::None;
        if (++s.gen == 0) s.gen = 1;
        free_.push_back(id.index());
        --alive_;
        return true;
    }

    bool is_alive(EntityId id) const {
        const uint32_t i = id.index();
        return i != 0 && i < slots_.size() && slots_[i].alive && slots_[i].gen == id.generation();
    }
    EntityKind kind(EntityId id) const { return is_alive(id) ? slots_[id.index()].kind : EntityKind::None; }
    size_t alive_count() const { return alive_; }

    void save(ByteWriter& w) const {
        w.u32(uint32_t(slots_.size()));
        for (size_t i = 1; i < slots_.size(); ++i) {
            w.u32(slots_[i].gen);
            w.u8(uint8_t(slots_[i].kind));
            w.u8(slots_[i].alive ? 1 : 0);
        }
        w.u32(uint32_t(free_.size()));  // order matters: it decides which index is reused next
        for (uint32_t f : free_) w.u32(f);
    }

    bool load(ByteReader& r) {
        const uint32_t n = r.u32();
        if (!r.ok() || n < 1 || size_t(n - 1) > r.remaining() / 6) return false;
        std::vector<Slot> slots(n);
        size_t alive = 0;
        for (uint32_t i = 1; i < n; ++i) {
            slots[i].gen = r.u32();
            const uint8_t k = r.u8();
            const uint8_t a = r.u8();
            if (!r.ok() || k >= uint8_t(EntityKind::Count) || a > 1 || slots[i].gen == 0) return false;
            slots[i].kind = EntityKind(k);
            slots[i].alive = a == 1;
            alive += a;
        }
        const uint32_t nf = r.u32();
        if (!r.ok() || size_t(nf) > r.remaining() / 4) return false;
        std::vector<uint32_t> fr(nf);
        for (auto& f : fr) {
            f = r.u32();
            if (!r.ok() || f == 0 || f >= n || slots[f].alive) return false;
        }
        slots_ = std::move(slots);
        free_ = std::move(fr);
        alive_ = alive;
        return true;
    }

private:
    struct Slot { uint32_t gen = 1; EntityKind kind = EntityKind::None; bool alive = false; };
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    size_t alive_ = 0;
};

}  // namespace cx
