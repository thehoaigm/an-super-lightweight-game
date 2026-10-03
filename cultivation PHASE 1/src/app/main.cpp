// Headless debug console for the simulation. Reads commands from stdin, so it also works in scripts:
//   printf '/time skip 1000d\n/world\n/hash\n' | ./cultivation --seed 42
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/game_loop.hpp"
#include "sim/simulation.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
static bool interactive() { return ::isatty(0) != 0; }
#else
static bool interactive() { return true; }
#endif

using namespace cx;
using Clock = std::chrono::steady_clock;

namespace {

struct Session {
    std::unique_ptr<Simulation> sim;
    double last_save_ms = -1, last_load_ms = -1;
    uintmax_t last_save_bytes = 0;
};

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> t;
    for (std::string w; in >> w;) t.push_back(w);
    if (!t.empty() && t[0][0] == '/') t[0].erase(0, 1);
    return t;
}

bool parse_i64(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || *end != '\0') return false;
    out = v;
    return true;
}

void help() {
    std::puts(
        "commands (leading / optional):\n"
        "  time                         show current game time\n"
        "  time skip <90m|12h|30d|2mo|1y>   advance the world\n"
        "  world                        list regions\n"
        "  inspect region <i> [cx cy]   region detail; with cx cy also lists that chunk's locations\n"
        "  focus <i>                    move the player to region i (changes simulation detail)\n"
        "  region qi <i> <value>        set a region's qi (0..1500)\n"
        "  save [path] | load [path]    binary save (default world.sav)\n"
        "  json [path]                  write a human-readable debug dump (default world.json)\n"
        "  hash                         hash of the full simulation state\n"
        "  stats                        metrics\n"
        "  run <real_seconds> <speed>   real-time demo: speed = game minutes per real second\n"
        "  quit");
}

void cmd_world(const Simulation& s) {
    const World& w = s.world();
    std::printf("%3s  %-13s %8s %8s %7s %5s  %s\n", "#", "biome", "qi", "target", "ticks", "lod", "name");
    for (uint32_t i = 0; i < w.states.size(); ++i) {
        const RegionState& st = w.states[i];
        std::printf("%3u  %-13s %8.1f %8.1f %7u %5s  %s\n", i, biome_name(w.statics[i].biome),
                    double(st.qi_fp) / kQiScale, double(s.target_qi_fp(i)) / kQiScale, st.ticks,
                    i == w.focus ? "near" : "far", w.statics[i].name.c_str());
    }
}

void cmd_inspect(const Simulation& s, const std::vector<std::string>& t) {
    int64_t i = 0, cx = -1, cy = -1;
    if (t.size() < 3 || t[1] != "region" || !parse_i64(t[2], i) || i < 0 || i >= int64_t(s.world().states.size())) {
        std::puts("usage: inspect region <i> [cx cy]");
        return;
    }
    const RegionStatic& g = s.world().statics[size_t(i)];
    const RegionState& st = s.world().states[size_t(i)];
    std::printf("region %lld  %s\n  biome %s, base qi %d, qi %.1f (target %.1f)\n  entity %llx (index %u, gen %u)\n"
                "  ticks %u, epoch %u, anomalies %u, last tick %s\n",
                (long long)i, g.name.c_str(), biome_name(g.biome), g.base_qi, double(st.qi_fp) / kQiScale,
                double(s.target_qi_fp(uint32_t(i))) / kQiScale, (unsigned long long)st.id.raw, st.id.index(),
                st.id.generation(), st.ticks, st.tick_epoch, st.anomalies, calendar::format(st.last_tick).c_str());
    if (t.size() >= 5 && parse_i64(t[3], cx) && parse_i64(t[4], cy) && cx >= 0 && cy >= 0 && cx < kChunkGrid &&
        cy < kChunkGrid) {
        const Chunk c = generate_chunk(g, int(cx), int(cy));
        std::printf("  chunk (%lld,%lld): %zu location(s)\n", (long long)cx, (long long)cy, c.locations.size());
        for (const Location& l : c.locations) std::printf("    %-12s at (%u,%u)\n", location_name(l.type), l.x, l.y);
    }
}

void cmd_run(Session& s, double seconds, int64_t speed) {
    GameLoop loop(*s.sim);
    loop.set_speed(speed);
    const uint64_t ev0 = s.sim->metrics().events_processed;
    const int64_t t_start = s.sim->now();
    const auto t0 = Clock::now();
    auto last = t0;
    while (ms_since(t0) < seconds * 1000.0) {
        const auto now = Clock::now();
        loop.update(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
        last = now;
        const int64_t nap = std::min<int64_t>(loop.idle_hint_us(), 20000);
        if (nap > 0) std::this_thread::sleep_for(std::chrono::microseconds(nap));
    }
    std::printf("advanced %lld game minutes, %llu events, now %s\n", (long long)(s.sim->now() - t_start),
                (unsigned long long)(s.sim->metrics().events_processed - ev0), calendar::format(s.sim->now()).c_str());
}

// Returns false when the session should end.
bool handle(Session& s, const std::vector<std::string>& t, std::string& default_path) {
    if (t.empty()) return true;
    const std::string& c = t[0];
    Simulation& sim = *s.sim;
    int64_t a = 0, b = 0;
    std::string err;

    if (c == "quit" || c == "exit" || c == "q") return false;
    if (c == "help" || c == "?") { help(); return true; }
    if (c == "time" && t.size() == 1) { std::printf("%s\n", calendar::format(sim.now()).c_str()); return true; }
    if (c == "time" && t.size() == 3 && t[1] == "skip") {
        int64_t minutes = 0;
        if (!calendar::parse_duration(t[2], minutes)) { std::puts("bad duration (examples: 90m 12h 30d 2mo 1y)"); return true; }
        const uint64_t ev0 = sim.metrics().events_processed;
        sim.advance_by(minutes);
        std::printf("%s  (%llu events, %.2f ms)\n", calendar::format(sim.now()).c_str(),
                    (unsigned long long)(sim.metrics().events_processed - ev0), double(sim.metrics().last_advance_us) / 1000.0);
        return true;
    }
    if (c == "world") { cmd_world(sim); return true; }
    if (c == "inspect") { cmd_inspect(sim, t); return true; }
    if (c == "focus" && t.size() == 2 && parse_i64(t[1], a) && a >= 0 && a < int64_t(sim.world().states.size())) {
        sim.set_focus(uint32_t(a));
        std::printf("player is now in region %lld\n", (long long)a);
        return true;
    }
    if (c == "region" && t.size() == 4 && t[1] == "qi" && parse_i64(t[2], a) && parse_i64(t[3], b) && a >= 0 && b >= 0 &&
        b <= kQiMax && sim.set_region_qi(uint32_t(a), int32_t(b))) {
        std::printf("region %lld qi set to %lld\n", (long long)a, (long long)b);
        return true;
    }
    if (c == "save") {
        if (t.size() > 1) default_path = t[1];
        const auto t0 = Clock::now();
        if (!sim.save(default_path, err)) { std::printf("save failed: %s\n", err.c_str()); return true; }
        s.last_save_ms = ms_since(t0);
        std::error_code ec;
        s.last_save_bytes = std::filesystem::file_size(default_path, ec);
        std::printf("saved %s (%ju bytes, %.2f ms)\n", default_path.c_str(), s.last_save_bytes, s.last_save_ms);
        return true;
    }
    if (c == "load") {
        if (t.size() > 1) default_path = t[1];
        const auto t0 = Clock::now();
        bool backup = false;
        std::unique_ptr<Simulation> loaded = Simulation::load(default_path, err, &backup);
        if (!loaded) { std::printf("load failed: %s\n", err.c_str()); return true; }
        s.last_load_ms = ms_since(t0);
        s.sim = std::move(loaded);
        std::printf("loaded %s%s, now %s (%.2f ms)\n", default_path.c_str(), backup ? " [from .bak: main save was bad]" : "",
                    calendar::format(s.sim->now()).c_str(), s.last_load_ms);
        return true;
    }
    if (c == "json") {
        const std::string path = t.size() > 1 ? t[1] : "world.json";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            const std::string j = sim.dump_json();
            std::fwrite(j.data(), 1, j.size(), f);
            std::fclose(f);
            std::printf("wrote %s (%zu bytes)\n", path.c_str(), j.size());
        } else {
            std::printf("cannot write %s\n", path.c_str());
        }
        return true;
    }
    if (c == "hash") { std::printf("%016llx\n", (unsigned long long)sim.state_hash()); return true; }
    if (c == "stats") {
        const Metrics& m = sim.metrics();
        std::printf("events processed %llu, sim time %.2f ms total, pending events %zu, entities %zu\n",
                    (unsigned long long)m.events_processed, double(m.sim_time_us) / 1000.0,
                    sim.bus().scheduled_count(), sim.entities().alive_count());
        std::printf("last save %.2f ms (%ju bytes), last load %.2f ms (-1 = none yet)\n", s.last_save_ms,
                    s.last_save_bytes, s.last_load_ms);
        return true;
    }
    if (c == "run" && t.size() == 3 && parse_i64(t[2], b) && b >= 0) {
        const double secs = std::atof(t[1].c_str());
        if (secs > 0 && secs <= 3600) { cmd_run(s, secs, b); return true; }
    }
    std::printf("unknown or malformed command: %s  (try: help)\n", c.c_str());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Simulation::Config cfg;
    std::string load_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        int64_t v = 0;
        if (a == "--seed" && i + 1 < argc && parse_i64(argv[i + 1], v)) { cfg.seed = uint64_t(v); ++i; }
        else if (a == "--regions" && i + 1 < argc && parse_i64(argv[i + 1], v) && v > 0) { cfg.regions = uint32_t(v); ++i; }
        else if (a == "--load" && i + 1 < argc) { load_path = argv[++i]; }
        else { std::fprintf(stderr, "usage: %s [--seed N] [--regions N] [--load file]\n", argv[0]); return 2; }
    }

    Session s;
    std::string default_path = load_path.empty() ? "world.sav" : load_path;
    if (!load_path.empty()) {
        std::string err;
        s.sim = Simulation::load(load_path, err);
        if (!s.sim) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    } else {
        s.sim = Simulation::create(cfg);
    }
    if (interactive()) std::printf("cultivation sandbox, phase 1 core. %s. type 'help'.\n", calendar::format(s.sim->now()).c_str());

    std::string line;
    for (;;) {
        if (interactive()) { std::fputs("cx> ", stdout); std::fflush(stdout); }
        if (!std::getline(std::cin, line)) break;
        if (!handle(s, tokenize(line), default_path)) break;
    }
    return 0;
}
