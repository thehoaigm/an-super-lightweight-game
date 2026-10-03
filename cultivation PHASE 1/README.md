# Cultivation sandbox: Phase 1 (Core)

Headless C++17 simulation core. No dependencies beyond the standard library. UI/renderer come later.

## Build / run / test

```bash
make            # builds ./cultivation (debug console) and ./cx_tests
make test       # 26 tests, ~0.1 s
printf '/time skip 1000d\n/world\n/save\n/hash\n' | ./cultivation --seed 42 --regions 12
./cultivation        # interactive; type "help"
```

Without make: `g++ -std=c++17 -O2 -Isrc src/app/main.cpp src/core/save.cpp src/sim/world.cpp src/sim/simulation.cpp -o cultivation`

Debug console commands: `time`, `time skip 30d`, `world`, `inspect region 0 [cx cy]`, `focus N`,
`region qi N V`, `save`, `load`, `json`, `hash`, `stats`, `run <real_s> <game_min_per_s>`.

## Layout

```
src/core   rng, bytes (binary I/O, crc32, fnv), entity ids, time/calendar, event bus, save container
src/sim    world (Region -> Chunk -> Location, generation), simulation (owns all mutable state)
src/app    game_loop (real time -> game time), main (debug console)
tests/     test_main.cpp
```

Dependency direction: `app -> sim -> core`. Core knows nothing about the game.

## Rules every new system must follow

1. **Integers only in the simulation** (fixed-point, e.g. qi is `qi * 256`). No floats, no `<random>`.
2. **All randomness through `Rng`**, and per-object generation through `derive_seed(parent, domain, index)`.
   Generation is a pure function of the seed, never of call order.
3. **Nothing runs per frame.** Schedule work with `bus.schedule(when, event)`; handle it in a subscriber.
   Far-from-player objects tick at low frequency; scale their effect by elapsed time so LOD does not change rates.
4. **State that changes goes into `serialize()` and `restore()`** (and `schema_version` bumps if the layout changes).
   Generated content is not saved; it is regenerated from the seed.
5. **A new system lists how it interacts with existing ones** (see the header comment in `simulation.hpp`).
6. **Add a test** next to the matching section in `tests/test_main.cpp`. `state_hash()` equality after
   save/load and after different batching is the determinism check.

## Known limits (Phase 1)

- Biome/location tables are compiled in (`world.cpp`); the data loader (`data/*`) arrives with Content. When it does,
  a content hash must be stored in the save next to `kWorldGenVersion`.
- Cross-platform determinism is by construction (integer math, explicit little-endian), but has only been run on
  one machine (Linux, g++ 13). Run `make test` on every target compiler/OS you care about.
- Region qi regen is exact in rate but not in compounding: hourly and daily steps differ by about 1% (tested within 5%).
- No `Config` module yet: there are no user settings to load.
