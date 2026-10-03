// Dependency-free test runner. Build: make test
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/game_loop.hpp"
#include "core/bytes.hpp"
#include "core/entity.hpp"
#include "core/event_bus.hpp"
#include "core/rng.hpp"
#include "core/save.hpp"
#include "core/time.hpp"
#include "sim/simulation.hpp"

using namespace cx;
namespace fs = std::filesystem;

// ---- tiny framework ---------------------------------------------------------------------------
struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };
static int g_failures = 0;
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); } } while (0)

static std::unique_ptr<Simulation> mk(uint64_t seed = 42, uint32_t regions = 12) {
    Simulation::Config c;
    c.seed = seed;
    c.regions = regions;
    return Simulation::create(c);
}
static Event mkev(uint32_t type, int64_t a) { return Event{type, EntityId{}, EntityId{}, a, 0}; }
static constexpr int64_t kDay = calendar::kMinutesPerDay;

static fs::path test_dir() {
    static const fs::path d = fs::temp_directory_path() / "cx_phase1_tests";
    fs::create_directories(d);
    return d;
}

// ---- RNG --------------------------------------------------------------------------------------
TEST(rng_matches_independent_reference_values) {
    // Reference values computed with a separate Python implementation of the published algorithms.
    uint64_t sm = 0;
    CHECK(splitmix64(sm) == 0xe220a8397b1dcdafull);
    CHECK(splitmix64(sm) == 0x6e789e6aa1b965f4ull);
    CHECK(splitmix64(sm) == 0x06c45d188009454full);
    Rng a(0);
    CHECK(a.next_u64() == 0x99ec5f36cb75f2b4ull);
    CHECK(a.next_u64() == 0xbf6e1f784956452aull);
    CHECK(a.next_u64() == 0x1a5f849d4933e6e0ull);
    CHECK(a.next_u64() == 0x6aa594f1262d2d2cull);
    Rng b(123456789);
    CHECK(b.next_u64() == 0xd1eea10c836f0cc2ull);
    CHECK(b.next_u64() == 0xe1bb9dfa08f02548ull);
}

TEST(rng_below_is_in_range_and_roughly_uniform) {
    Rng r(12345);
    int counts[10] = {};
    for (int i = 0; i < 100000; ++i) {
        const uint64_t v = r.below(10);
        CHECK(v < 10);
        ++counts[v];
    }
    for (int c : counts) CHECK(c > 9500 && c < 10500);
    for (int i = 0; i < 1000; ++i) { const int64_t v = r.range(-5, 5); CHECK(v >= -5 && v <= 5); }
}

TEST(rng_state_roundtrip_continues_identically) {
    Rng a(77);
    for (int i = 0; i < 10; ++i) a.next_u64();
    Rng b(1);
    b.set_state(a.state());
    for (int i = 0; i < 100; ++i) CHECK(a.next_u64() == b.next_u64());
}

TEST(derive_seed_is_pure_and_separated) {
    CHECK(derive_seed(5, SeedDomain::Region, 3) == derive_seed(5, SeedDomain::Region, 3));
    CHECK(derive_seed(5, SeedDomain::Region, 3) != derive_seed(5, SeedDomain::Npc, 3));
    CHECK(derive_seed(5, SeedDomain::Region, 3) != derive_seed(6, SeedDomain::Region, 3));
    std::set<uint64_t> seen;
    for (uint64_t i = 0; i < 1000; ++i) seen.insert(derive_seed(5, SeedDomain::Region, i));
    CHECK(seen.size() == 1000);
}

// ---- bytes / checksums ------------------------------------------------------------------------
TEST(bytes_roundtrip_and_overrun) {
    ByteWriter w;
    w.u8(0xAB); w.u32(0xDEADBEEF); w.u64(0x0123456789ABCDEFull); w.i32(-5); w.i64(-9000000000ll);
    const uint64_t vs[] = {0, 1, 127, 128, 16383, 16384, 1ull << 63, ~0ull};
    for (uint64_t v : vs) w.varuint(v);
    ByteReader r(w.data());
    CHECK(r.u8() == 0xAB);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFull);
    CHECK(r.i32() == -5);
    CHECK(r.i64() == -9000000000ll);
    for (uint64_t v : vs) CHECK(r.varuint() == v);
    CHECK(r.ok() && r.remaining() == 0);
    CHECK(r.u32() == 0);  // past the end
    CHECK(!r.ok());
    // little-endian on the wire, regardless of host
    ByteWriter le;
    le.u32(0x01020304);
    CHECK(le.data()[0] == 4 && le.data()[3] == 1);
}

TEST(checksums_match_known_vectors) {
    const char* s = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
    CHECK(fnv1a64(nullptr, 0) == 0xcbf29ce484222325ull);
    const uint8_t a = 'a';
    CHECK(fnv1a64(&a, 1) == 0xaf63dc4c8601ec8cull);
}

// ---- time -------------------------------------------------------------------------------------
TEST(calendar_roundtrip_and_boundaries) {
    CHECK(calendar::from_date(1, 1, 1) == 0);
    for (int64_t t = 0; t < 3 * calendar::kMinutesPerYear; t += 97) {
        const Date d = calendar::to_date(t);
        CHECK(calendar::from_date(d.year, d.month, d.day, d.hour, d.minute) == t);
    }
    CHECK(calendar::to_date(calendar::from_date(1, 3, 30)).season == Season::Spring);
    CHECK(calendar::to_date(calendar::from_date(1, 4, 1)).season == Season::Summer);
    CHECK(calendar::to_date(calendar::from_date(1, 7, 1)).season == Season::Autumn);
    CHECK(calendar::to_date(calendar::from_date(1, 12, 30)).season == Season::Winter);
    CHECK(calendar::to_date(calendar::from_date(1000, 12, 30, 23, 59)).era == 1);
    CHECK(calendar::to_date(calendar::from_date(1001, 1, 1)).era == 2);
    CHECK(calendar::to_date(calendar::from_date(2, 1, 1)).year == 2);
}

TEST(duration_parsing) {
    int64_t m = 0;
    CHECK(calendar::parse_duration("30d", m) && m == 30 * kDay);
    CHECK(calendar::parse_duration("2mo", m) && m == 60 * kDay);
    CHECK(calendar::parse_duration("1y", m) && m == calendar::kMinutesPerYear);
    CHECK(calendar::parse_duration("90m", m) && m == 90);
    CHECK(calendar::parse_duration("12h", m) && m == 720);
    for (const char* bad : {"", "d", "10", "5x", "-3d", "3 d", "99999999999d"}) CHECK(!calendar::parse_duration(bad, m));
}

// ---- entities ---------------------------------------------------------------------------------
TEST(entity_generations_detect_stale_ids) {
    EntityAllocator a;
    const EntityId e1 = a.create(EntityKind::Npc);
    CHECK(e1.valid() && a.is_alive(e1) && a.kind(e1) == EntityKind::Npc && a.alive_count() == 1);
    CHECK(a.destroy(e1));
    CHECK(!a.is_alive(e1) && !a.destroy(e1) && a.alive_count() == 0);
    const EntityId e2 = a.create(EntityKind::Item);
    CHECK(e2.index() == e1.index() && e2.generation() == e1.generation() + 1);
    CHECK(!a.is_alive(e1) && a.is_alive(e2));
    CHECK(!a.is_alive(EntityId{}));
}

TEST(entity_save_load_continues_identically) {
    EntityAllocator a;
    EntityId ids[5];
    for (auto& i : ids) i = a.create(EntityKind::Npc);
    a.destroy(ids[1]);
    a.destroy(ids[3]);
    ByteWriter w;
    a.save(w);
    EntityAllocator b;
    ByteReader r(w.data());
    CHECK(b.load(r) && r.remaining() == 0);
    CHECK(b.alive_count() == a.alive_count());
    for (int i = 0; i < 4; ++i) CHECK(a.create(EntityKind::Item) == b.create(EntityKind::Item));
    // corrupt kind byte must be rejected, not trusted
    ByteWriter bad;
    bad.u32(2); bad.u32(1); bad.u8(99); bad.u8(1); bad.u32(0);
    EntityAllocator c;
    ByteReader rb(bad.data());
    CHECK(!c.load(rb));
}

// ---- event bus --------------------------------------------------------------------------------
TEST(bus_orders_by_time_then_fifo) {
    EventBus b;
    b.schedule(10, mkev(1, 2));
    b.schedule(5, mkev(1, 1));
    b.schedule(10, mkev(1, 3));
    int64_t when = 0;
    Event e;
    CHECK(!b.pop_due(4, when, e));
    CHECK(b.pop_due(9, when, e) && e.a == 1 && when == 5);
    CHECK(!b.pop_due(9, when, e));
    CHECK(b.pop_due(10, when, e) && e.a == 2);
    CHECK(b.pop_due(10, when, e) && e.a == 3);
    CHECK(!b.has_scheduled());
}

TEST(bus_handlers_run_in_registration_order_and_guard_misuse) {
    EventBus b;
    std::vector<int> order;
    b.subscribe(1, [&](const Event&) { order.push_back(1); });
    b.subscribe(1, [&](const Event&) { order.push_back(2); });
    b.publish(mkev(1, 0));
    CHECK((order == std::vector<int>{1, 2}));
    b.publish(mkev(99, 0));  // no subscribers: harmless

    EventBus loop;
    loop.subscribe(1, [&](const Event& e) { loop.publish(e); });
    bool threw = false;
    try { loop.publish(mkev(1, 0)); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    // the guard must unwind cleanly: the bus is usable again afterwards
    EventBus ok;
    int n = 0;
    ok.subscribe(2, [&](const Event&) { ++n; });
    ok.publish(mkev(2, 0));
    CHECK(n == 1);

    EventBus sub;
    bool threw2 = false;
    sub.subscribe(1, [&](const Event&) {
        try { sub.subscribe(2, [](const Event&) {}); } catch (const std::logic_error&) { threw2 = true; }
    });
    sub.publish(mkev(1, 0));
    CHECK(threw2);
}

TEST(bus_save_load_preserves_order_and_bytes) {
    EventBus a;
    Rng r(5);
    for (int i = 0; i < 60; ++i) a.schedule(int64_t(r.below(20)), mkev(1, i));
    ByteWriter w;
    a.save(w);
    EventBus b;
    ByteReader rd(w.data());
    CHECK(b.load(rd) && rd.remaining() == 0);
    ByteWriter w2;
    b.save(w2);
    CHECK(w.data() == w2.data());
    int64_t t1, t2;
    Event e1, e2;
    int popped = 0;
    while (a.pop_due(1000, t1, e1)) {
        CHECK(b.pop_due(1000, t2, e2) && t1 == t2 && e1.a == e2.a);
        ++popped;
    }
    CHECK(popped == 60 && !b.has_scheduled());
}

// ---- save container ---------------------------------------------------------------------------
TEST(container_detects_corruption_and_newer_versions) {
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    uint32_t v = 0;
    std::vector<uint8_t> out;
    std::string err;
    auto good = save::make_container(1, payload);
    CHECK(save::parse_container(good, v, out, err) == save::ParseStatus::Ok && v == 1 && out == payload);

    auto flipped = good; flipped.back() ^= 0x01;
    CHECK(save::parse_container(flipped, v, out, err) == save::ParseStatus::Corrupt);
    auto truncated = good; truncated.pop_back();
    CHECK(save::parse_container(truncated, v, out, err) == save::ParseStatus::Corrupt);
    auto trailing = good; trailing.push_back(0);
    CHECK(save::parse_container(trailing, v, out, err) == save::ParseStatus::Corrupt);
    auto badmagic = good; badmagic[0] = 'X';
    CHECK(save::parse_container(badmagic, v, out, err) == save::ParseStatus::Corrupt);
    CHECK(save::parse_container({}, v, out, err) == save::ParseStatus::Corrupt);
    auto newer = save::make_container(save::kSchemaVersion + 1, payload);
    CHECK(save::parse_container(newer, v, out, err) == save::ParseStatus::NewerVersion);
}

TEST(atomic_write_keeps_previous_save_as_backup) {
    const std::string p = (test_dir() / "atomic.bin").string();
    fs::remove(p); fs::remove(p + ".bak"); fs::remove(p + ".tmp");
    std::string err;
    CHECK(save::write_atomic(p, {1, 1, 1}, err));
    CHECK(save::write_atomic(p, {2, 2}, err));
    std::vector<uint8_t> cur, bak;
    CHECK(save::read_file(p, cur, err) && cur == std::vector<uint8_t>({2, 2}));
    CHECK(save::read_file(p + ".bak", bak, err) && bak == std::vector<uint8_t>({1, 1, 1}));
    CHECK(!fs::exists(p + ".tmp"));
}

// ---- world generation -------------------------------------------------------------------------
TEST(worldgen_is_pure_and_order_independent) {
    const RegionStatic r5a = generate_region(42, 5);
    for (uint32_t i = 0; i < 20; ++i) generate_region(42, i);  // unrelated calls in between
    const RegionStatic r5b = generate_region(42, 5);
    CHECK(r5a.seed == r5b.seed && r5a.name == r5b.name && r5a.biome == r5b.biome && r5a.base_qi == r5b.base_qi);

    const Chunk c1 = generate_chunk(r5a, 3, 4);
    generate_chunk(r5a, 0, 0);
    generate_chunk(r5a, 7, 7);
    const Chunk c2 = generate_chunk(r5a, 3, 4);
    CHECK(c1.locations.size() == c2.locations.size());
    for (size_t i = 0; i < c1.locations.size(); ++i)
        CHECK(c1.locations[i].type == c2.locations[i].type && c1.locations[i].x == c2.locations[i].x);
    CHECK(c1.locations.size() <= 3);

    std::set<Biome> biomes;
    for (uint32_t i = 0; i < 200; ++i) {
        const RegionStatic r = generate_region(7, i);
        biomes.insert(r.biome);
        CHECK(r.base_qi >= 50 && r.base_qi <= 1000);
    }
    CHECK(biomes.size() >= 5);
}

// ---- simulation -------------------------------------------------------------------------------
TEST(same_seed_same_world_different_seed_different_world) {
    CHECK(mk(42)->state_hash() == mk(42)->state_hash());
    CHECK(mk(42)->state_hash() != mk(43)->state_hash());
}

TEST(calendar_events_fire_exactly_once_per_boundary) {
    auto s = mk();
    int day = 0, month = 0, season = 0, year = 0;
    s->bus().subscribe(ev::DayStart, [&](const Event&) { ++day; });
    s->bus().subscribe(ev::MonthStart, [&](const Event&) { ++month; });
    s->bus().subscribe(ev::SeasonStart, [&](const Event&) { ++season; });
    s->bus().subscribe(ev::YearStart, [&](const Event&) { ++year; });
    s->advance_by(calendar::kMinutesPerYear);  // Y1 06:00 -> Y2 06:00 crosses Y2 M1 D1 exactly once
    CHECK(day == 360 && month == 12 && season == 4 && year == 1);
    CHECK(calendar::to_date(s->now()).year == 2);
}

TEST(world_is_event_driven_and_invariants_hold_over_1000_days) {
    auto s = mk(7, 16);
    s->advance_by(1000 * kDay);
    const World& w = s->world();
    for (const RegionState& st : w.states) CHECK(st.qi_fp >= 0 && st.qi_fp <= kQiMax * kQiScale);
    CHECK(s->entities().alive_count() == 16);               // nothing leaks or multiplies
    CHECK(s->bus().scheduled_count() == 16 + 1);            // one tick per region + the next midnight
    CHECK(w.states[0].ticks == 24000);                      // focus region: hourly
    CHECK(w.states[1].ticks == 1000);                       // everything else: daily
    CHECK(s->metrics().events_processed < 50000);           // ~16 regions * 1000 days, not per-frame work
}

TEST(batching_does_not_change_the_result) {
    auto a = mk(9);
    a->advance_by(1000 * kDay);
    auto b = mk(9);
    Rng r(99);
    const int64_t end = b->now() + 1000 * kDay;
    while (b->now() < end) b->advance_to(std::min<int64_t>(b->now() + int64_t(r.below(5000)) + 1, end));
    CHECK(a->state_hash() == b->state_hash());
}

TEST(save_load_midway_matches_uninterrupted_run) {
    const std::string p = (test_dir() / "mid.sav").string();
    fs::remove(p); fs::remove(p + ".bak");
    auto a = mk(21);
    a->advance_by(500 * kDay);
    const uint64_t before = a->state_hash();
    std::string err;
    CHECK(a->save(p, err));
    auto b = Simulation::load(p, err);
    CHECK(b != nullptr);
    if (!b) return;
    CHECK(b->state_hash() == before);  // RNG, queue, entities, regions all restored
    a->advance_by(500 * kDay);
    b->advance_by(500 * kDay);
    auto c = mk(21);
    c->advance_by(1000 * kDay);
    CHECK(a->state_hash() == b->state_hash());
    CHECK(a->state_hash() == c->state_hash());
}

TEST(load_falls_back_to_backup_but_never_past_a_newer_save) {
    const std::string p = (test_dir() / "fallback.sav").string();
    fs::remove(p); fs::remove(p + ".bak");
    auto s = mk(3);
    std::string err;
    s->advance_by(10 * kDay);
    CHECK(s->save(p, err));
    const int64_t first_time = s->now();
    s->advance_by(10 * kDay);
    CHECK(s->save(p, err));  // previous save becomes .bak

    bool used_backup = false;
    auto ok = Simulation::load(p, err, &used_backup);
    CHECK(ok && !used_backup && ok->now() == s->now());

    { std::FILE* f = std::fopen(p.c_str(), "wb"); std::fputs("garbage", f); std::fclose(f); }  // corrupt main
    auto fb = Simulation::load(p, err, &used_backup);
    CHECK(fb && used_backup && fb->now() == first_time);

    ByteWriter w;
    s->serialize(w);
    std::vector<uint8_t> newer = save::make_container(save::kSchemaVersion + 1, w.data());
    std::vector<uint8_t> valid = save::make_container(save::kSchemaVersion, w.data());
    CHECK(save::write_atomic(p, newer, err));
    CHECK(save::write_atomic(p + ".bak", valid, err));  // main = newer version, .bak = perfectly valid
    auto nv = Simulation::load(p, err);
    CHECK(nv == nullptr && err.find("newer") != std::string::npos);
}

TEST(focus_change_swaps_lod_and_drops_stale_ticks) {
    auto s = mk(11, 12);
    s->advance_by(10 * kDay);
    CHECK(s->world().states[0].ticks == 240 && s->world().states[3].ticks == 10);
    s->set_focus(3);
    CHECK(s->bus().scheduled_count() == 12 + 1 + 2);  // two now-stale ticks are still queued
    s->advance_by(10 * kDay);
    CHECK(s->world().states[3].ticks == 10 + 240);    // now hourly
    CHECK(s->world().states[0].ticks == 240 + 10);    // now daily
    CHECK(s->bus().scheduled_count() == 12 + 1);      // stale ones were dropped, not rescheduled
}

TEST(season_scales_qi_target_and_qi_regenerates) {
    auto s = mk(5);
    CHECK(s->target_qi_fp(0, Season::Winter) < s->target_qi_fp(0, Season::Autumn));
    CHECK(s->target_qi_fp(0, Season::Autumn) < s->target_qi_fp(0, Season::Summer));
    CHECK(s->target_qi_fp(0, Season::Summer) < s->target_qi_fp(0, Season::Spring));
    // far region (2) and near region (0), both drained: both recover toward the target at ~the same pace
    CHECK(s->set_region_qi(2, 0) && s->set_region_qi(0, 0));
    s->advance_by(20 * kDay);  // before the first MonthStart, so no random anomaly interferes
    for (uint32_t i : {0u, 2u}) {
        const int32_t q = s->world().states[i].qi_fp, target = s->target_qi_fp(i);
        CHECK(q > target * 6 / 10 && q <= target);
    }
    const int32_t near_q = s->world().states[0].qi_fp, far_q = s->world().states[2].qi_fp;
    const double rn = double(near_q) / s->target_qi_fp(0), rf = double(far_q) / s->target_qi_fp(2);
    CHECK(rn - rf < 0.05 && rf - rn < 0.05);
}

// ---- game loop --------------------------------------------------------------------------------
TEST(game_loop_is_exact_and_clamps_stalls) {
    auto s = mk();
    GameLoop g(*s);
    CHECK(g.paused() && g.update(500000) == 0);
    g.set_speed(7);
    int64_t total = 0;
    for (int i = 0; i < 10; ++i) total += g.update(100000);  // 10 * 0.1 s * 7 min/s = 7, no float drift
    CHECK(total == 7);
    CHECK(g.update(10 * 1000000) == 7 * 250000 / 1000000);  // a 10 s stall is clamped to 250 ms
    CHECK(g.update(-5) == 0);
}

TEST(game_loop_idle_hint_points_at_next_event) {
    auto s = mk();
    GameLoop g(*s);
    CHECK(g.idle_hint_us() == GameLoop::kMaxIdleUs);  // paused: sleep as long as allowed
    g.set_speed(600);
    // next event: the focus region's tick, 60 game minutes away = 0.1 real seconds at 600 min/s
    CHECK(g.idle_hint_us() == 100000);
    g.update(50000);  // 30 game minutes
    CHECK(g.idle_hint_us() == 50000);
}

// ---- runner -----------------------------------------------------------------------------------
int main() {
    int failed_tests = 0;
    for (const TestCase& t : registry()) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("    exception: %s\n", e.what());
        }
        const bool ok = g_failures == before;
        if (!ok) ++failed_tests;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::error_code ec;
    fs::remove_all(test_dir(), ec);
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
