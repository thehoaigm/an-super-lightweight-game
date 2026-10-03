#pragma once
// Simulation owns all mutable game state: time, RNG, entity table, event bus/schedule, world.
// It is headless (no I/O except save/load) so it can run in tests, a CLI, or behind Tauri/SDL.
//
// Interactions in Phase 1 (each system must explain how it touches the others):
//   Time    -> Event bus : midnight DayStart; month/season/year boundaries are derived from it
//   Time    -> Ecology   : season scales each region's equilibrium qi (Winter 70% .. Spring 120%)
//   Player focus -> LOD  : focus region ticks hourly, all others daily (same regen rate, less CPU)
//   RNG     -> World     : monthly qi anomalies; RNG state is saved, so replays stay identical
//   Entity IDs -> World  : every region is an entity; stale scheduled ticks are dropped by ID/epoch
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/bytes.hpp"
#include "core/entity.hpp"
#include "core/event_bus.hpp"
#include "core/rng.hpp"
#include "core/time.hpp"
#include "sim/world.hpp"

namespace cx {

struct Metrics {
    uint64_t events_processed = 0;  // scheduled events popped and dispatched
    uint64_t sim_time_us = 0;       // total wall time spent in advance_to()
    uint64_t last_advance_us = 0;
};

class Simulation {
public:
    struct Config {
        uint64_t seed = 1;
        uint32_t regions = 12;
    };

    static constexpr int64_t kNearPeriod = 60;                          // minutes between ticks near the player
    static constexpr int64_t kFarPeriod = calendar::kMinutesPerDay;     // minutes between ticks far away
    static constexpr int64_t kQiRegenWindow = 16 * calendar::kMinutesPerDay;  // qi closes 1/16 of its gap per day
    static constexpr uint32_t kAnomalyPermille = 60;                    // monthly chance per region
    static constexpr uint32_t kMaxRegions = 4096;

    static std::unique_ptr<Simulation> create(const Config& cfg);
    // Tries `path`, then `path.bak` if the main file is missing or corrupt. A save from a newer
    // version is reported as an error and never silently replaced by the backup.
    static std::unique_ptr<Simulation> load(const std::string& path, std::string& err, bool* used_backup = nullptr);

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // --- time -------------------------------------------------------------------------------
    int64_t now() const { return now_; }
    void advance_to(int64_t target);  // processes every event due up to and including `target`
    void advance_by(int64_t minutes) { advance_to(now_ + minutes); }
    bool has_pending() const { return bus_.has_scheduled(); }
    int64_t next_event_time() const { return bus_.next_time(); }

    // --- world ------------------------------------------------------------------------------
    const World& world() const { return world_; }
    uint64_t seed() const { return seed_; }
    void set_focus(uint32_t region);
    bool set_region_qi(uint32_t region, int32_t qi_units);  // debug
    int32_t target_qi_fp(uint32_t region, Season s) const;
    int32_t target_qi_fp(uint32_t region) const { return target_qi_fp(region, calendar::to_date(now_).season); }

    // --- plumbing ---------------------------------------------------------------------------
    EventBus& bus() { return bus_; }
    const EventBus& bus() const { return bus_; }
    Rng& rng() { return rng_; }
    const EntityAllocator& entities() const { return entities_; }
    const Metrics& metrics() const { return metrics_; }

    // --- persistence ------------------------------------------------------------------------
    void serialize(ByteWriter& w) const;  // payload only, canonical bytes
    uint64_t state_hash() const;          // FNV-1a over the canonical payload
    bool save(const std::string& path, std::string& err) const;
    std::string dump_json() const;        // human-readable debug dump, write-only

private:
    Simulation(uint64_t seed, uint32_t regions);
    void init_new();
    static std::unique_ptr<Simulation> restore(uint32_t version, const std::vector<uint8_t>& payload,
                                               std::string& err);

    int64_t period(uint32_t region) const { return region == world_.focus ? kNearPeriod : kFarPeriod; }
    void schedule_region_tick(uint32_t region);
    void on_day_start(const Event& e);
    void on_month_start(const Event& e);
    void on_region_tick(const Event& e);

    uint64_t seed_;
    Rng rng_;
    int64_t now_ = 0;
    EntityAllocator entities_;
    EventBus bus_;
    World world_;
    Metrics metrics_;
};

}  // namespace cx
