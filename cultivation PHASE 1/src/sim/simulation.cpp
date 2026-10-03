#include "sim/simulation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "core/save.hpp"

namespace cx {

namespace {

constexpr int kSeasonPct[4] = {120, 100, 90, 70};  // Spring, Summer, Autumn, Winter
constexpr int64_t kMaxQiFp = int64_t(kQiMax) * kQiScale;

int32_t clamp_qi(int64_t v) { return int32_t(std::clamp<int64_t>(v, 0, kMaxQiFp)); }

void appendf(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, size_t(std::min(n, int(sizeof buf) - 1)));
}

}  // namespace

Simulation::Simulation(uint64_t seed, uint32_t regions) : seed_(seed), rng_(derive_seed(seed, SeedDomain::World, 0)) {
    world_.statics.reserve(regions);
    for (uint32_t i = 0; i < regions; ++i) world_.statics.push_back(generate_region(seed, i));
    // Internal systems subscribe first, so they always run before any later (user/content) subscriber.
    bus_.subscribe(ev::DayStart, [this](const Event& e) { on_day_start(e); });
    bus_.subscribe(ev::MonthStart, [this](const Event& e) { on_month_start(e); });
    bus_.subscribe(ev::RegionTick, [this](const Event& e) { on_region_tick(e); });
}

std::unique_ptr<Simulation> Simulation::create(const Config& cfg) {
    const uint32_t n = std::clamp<uint32_t>(cfg.regions, 1, kMaxRegions);
    std::unique_ptr<Simulation> s(new Simulation(cfg.seed, n));
    s->init_new();
    return s;
}

void Simulation::init_new() {
    now_ = calendar::from_date(1, 1, 1, 6, 0);  // a new world starts at dawn, Year 1
    const int64_t next_midnight = (now_ / calendar::kMinutesPerDay + 1) * calendar::kMinutesPerDay;
    bus_.schedule(next_midnight, Event{ev::DayStart});
    world_.states.assign(world_.statics.size(), RegionState{});
    for (uint32_t i = 0; i < world_.states.size(); ++i) {
        RegionState& s = world_.states[i];
        s.id = entities_.create(EntityKind::Region);
        s.qi_fp = world_.statics[i].base_qi * kQiScale;
        s.last_tick = now_;
        s.tick_epoch = 1;
        schedule_region_tick(i);
    }
}

void Simulation::schedule_region_tick(uint32_t region) {
    const RegionState& s = world_.states[region];
    bus_.schedule(now_ + period(region), Event{ev::RegionTick, s.id, EntityId{}, int64_t(region), int64_t(s.tick_epoch)});
}

void Simulation::advance_to(int64_t target) {
    if (target < now_) return;  // time never runs backwards
    const auto t0 = std::chrono::steady_clock::now();
    int64_t when = 0;
    Event e;
    while (bus_.pop_due(target, when, e)) {
        now_ = std::max(now_, when);
        bus_.publish(e);
        ++metrics_.events_processed;
    }
    now_ = target;
    const auto dt = std::chrono::steady_clock::now() - t0;
    metrics_.last_advance_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
    metrics_.sim_time_us += metrics_.last_advance_us;
}

void Simulation::on_day_start(const Event&) {
    bus_.schedule(now_ + calendar::kMinutesPerDay, Event{ev::DayStart});  // self-perpetuating, lives in the saved queue
    const Date d = calendar::to_date(now_);
    if (d.day != 1) return;
    bus_.publish(Event{ev::MonthStart, EntityId{}, EntityId{}, d.month, d.year});
    if ((d.month - 1) % 3 == 0) bus_.publish(Event{ev::SeasonStart, EntityId{}, EntityId{}, int64_t(d.season), d.year});
    if (d.month == 1) bus_.publish(Event{ev::YearStart, EntityId{}, EntityId{}, d.year, 0});
}

void Simulation::on_month_start(const Event&) {
    // Spiritual surges/droughts. Draw order is fixed (region index order), so it is deterministic.
    for (RegionState& s : world_.states) {
        if (!rng_.chance_permille(kAnomalyPermille)) continue;
        s.qi_fp = clamp_qi(int64_t(s.qi_fp) + rng_.range(-300, 300) * kQiScale);
        ++s.anomalies;
    }
}

int32_t Simulation::target_qi_fp(uint32_t region, Season s) const {
    return clamp_qi(int64_t(world_.statics[region].base_qi) * kQiScale * kSeasonPct[int(s)] / 100);
}

void Simulation::on_region_tick(const Event& e) {
    const uint32_t i = uint32_t(e.a);
    if (i >= world_.states.size()) return;
    RegionState& s = world_.states[i];
    // Stale: the region's LOD changed after this tick was scheduled, or the entity is gone.
    if (s.tick_epoch != uint32_t(e.b) || s.id != e.subject || !entities_.is_alive(s.id)) return;
    // Regen is scaled by elapsed time, so near (hourly) and far (daily) regions recover at the same rate.
    const int64_t elapsed = std::min<int64_t>(now_ - s.last_tick, kQiRegenWindow);
    const int64_t gap = int64_t(target_qi_fp(i)) - s.qi_fp;
    s.qi_fp = clamp_qi(int64_t(s.qi_fp) + gap * elapsed / kQiRegenWindow);
    s.last_tick = now_;
    ++s.ticks;
    schedule_region_tick(i);
}

void Simulation::set_focus(uint32_t region) {
    if (region >= world_.states.size() || region == world_.focus) return;
    const uint32_t old = world_.focus;
    world_.focus = region;
    for (uint32_t i : {old, region}) {
        ++world_.states[i].tick_epoch;  // invalidates the tick already in the queue
        schedule_region_tick(i);
    }
}

bool Simulation::set_region_qi(uint32_t region, int32_t qi_units) {
    if (region >= world_.states.size()) return false;
    world_.states[region].qi_fp = clamp_qi(int64_t(qi_units) * kQiScale);
    return true;
}

// ---- persistence ----------------------------------------------------------------------------
// Payload v1: worldgen_version, seed, region_count, now, rng[4], entities, event queue, focus,
// then per region {id, qi_fp, ticks, tick_epoch, last_tick, anomalies}.
// Generated world content (biomes, chunks, names) is NOT saved: it is regenerated from the seed.

void Simulation::serialize(ByteWriter& w) const {
    w.u32(kWorldGenVersion);
    w.u64(seed_);
    w.u32(uint32_t(world_.states.size()));
    w.i64(now_);
    for (uint64_t x : rng_.state()) w.u64(x);
    entities_.save(w);
    bus_.save(w);
    w.u32(world_.focus);
    for (const RegionState& s : world_.states) {
        w.u64(s.id.raw);
        w.i32(s.qi_fp);
        w.u32(s.ticks);
        w.u32(s.tick_epoch);
        w.i64(s.last_tick);
        w.u32(s.anomalies);
    }
}

uint64_t Simulation::state_hash() const {
    ByteWriter w;
    serialize(w);
    return fnv1a64(w.data().data(), w.size());
}

bool Simulation::save(const std::string& path, std::string& err) const {
    ByteWriter w;
    serialize(w);
    return save::write_atomic(path, save::make_container(save::kSchemaVersion, w.data()), err);
}

std::unique_ptr<Simulation> Simulation::restore(uint32_t version, const std::vector<uint8_t>& payload,
                                                std::string& err) {
    if (version != save::kSchemaVersion) {
        err = "no migration path from schema v" + std::to_string(version);
        return nullptr;
    }
    ByteReader r(payload);
    const uint32_t gen_version = r.u32();
    const uint64_t seed = r.u64();
    const uint32_t n = r.u32();
    if (!r.ok()) { err = "truncated save header"; return nullptr; }
    if (gen_version != kWorldGenVersion) {
        err = "save uses world generator v" + std::to_string(gen_version) + ", this build has v" +
              std::to_string(kWorldGenVersion);
        return nullptr;
    }
    if (n < 1 || n > kMaxRegions) { err = "invalid region count"; return nullptr; }

    std::unique_ptr<Simulation> s(new Simulation(seed, n));
    s->now_ = r.i64();
    std::array<uint64_t, 4> rs{};
    for (auto& x : rs) x = r.u64();
    if (!r.ok() || s->now_ < 0) { err = "invalid time/RNG state"; return nullptr; }
    s->rng_.set_state(rs);
    if (!s->entities_.load(r)) { err = "invalid entity table"; return nullptr; }
    if (!s->bus_.load(r)) { err = "invalid event queue"; return nullptr; }
    s->world_.focus = r.u32();
    s->world_.states.assign(n, RegionState{});
    for (RegionState& st : s->world_.states) {
        st.id.raw = r.u64();
        st.qi_fp = r.i32();
        st.ticks = r.u32();
        st.tick_epoch = r.u32();
        st.last_tick = r.i64();
        st.anomalies = r.u32();
    }
    if (!r.ok() || r.remaining() != 0) { err = "truncated or oversized payload"; return nullptr; }
    if (s->world_.focus >= n) { err = "invalid focus region"; return nullptr; }
    for (const RegionState& st : s->world_.states) {
        if (s->entities_.kind(st.id) != EntityKind::Region || st.qi_fp < 0 || st.qi_fp > kMaxQiFp) {
            err = "invalid region state";
            return nullptr;
        }
    }
    return s;
}

std::unique_ptr<Simulation> Simulation::load(const std::string& path, std::string& err, bool* used_backup) {
    if (used_backup) *used_backup = false;
    std::string problems;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string p = attempt == 0 ? path : path + ".bak";
        std::vector<uint8_t> file;
        std::string e;
        if (!save::read_file(p, file, e)) {
            problems += e + "; ";
            continue;
        }
        uint32_t version = 0;
        std::vector<uint8_t> payload;
        const save::ParseStatus st = save::parse_container(file, version, payload, e);
        if (st == save::ParseStatus::NewerVersion) {  // never fall back past a newer save
            err = e;
            return nullptr;
        }
        if (st == save::ParseStatus::Ok) {
            std::unique_ptr<Simulation> s = restore(version, payload, e);
            if (s) {
                if (used_backup) *used_backup = attempt == 1;
                return s;
            }
        }
        problems += p + ": " + e + "; ";
    }
    err = problems;
    return nullptr;
}

std::string Simulation::dump_json() const {
    std::string o;
    appendf(o, "{\n  \"schema_version\": %u,\n  \"worldgen_version\": %u,\n", save::kSchemaVersion, kWorldGenVersion);
    appendf(o, "  \"seed\": %llu,\n  \"now_minutes\": %lld,\n  \"date\": \"%s\",\n", (unsigned long long)seed_,
            (long long)now_, calendar::format(now_).c_str());
    const auto rs = rng_.state();
    appendf(o, "  \"rng_state\": [\"%016llx\", \"%016llx\", \"%016llx\", \"%016llx\"],\n", (unsigned long long)rs[0],
            (unsigned long long)rs[1], (unsigned long long)rs[2], (unsigned long long)rs[3]);
    appendf(o, "  \"entities_alive\": %zu,\n  \"pending_events\": %zu,\n  \"focus_region\": %u,\n  \"state_hash\": \"%016llx\",\n",
            entities_.alive_count(), bus_.scheduled_count(), world_.focus, (unsigned long long)state_hash());
    o += "  \"regions\": [\n";
    for (size_t i = 0; i < world_.states.size(); ++i) {
        const RegionState& s = world_.states[i];
        const RegionStatic& g = world_.statics[i];
        appendf(o, "    {\"index\": %zu, \"entity\": \"%llx\", \"name\": \"%s\", \"biome\": \"%s\", \"base_qi\": %d, "
                   "\"qi\": %.2f, \"ticks\": %u, \"epoch\": %u, \"anomalies\": %u}%s\n",
                i, (unsigned long long)s.id.raw, g.name.c_str(), biome_name(g.biome), g.base_qi,
                double(s.qi_fp) / kQiScale, s.ticks, s.tick_epoch, s.anomalies,
                i + 1 < world_.states.size() ? "," : "");
    }
    o += "  ]\n}\n";
    return o;
}

}  // namespace cx
