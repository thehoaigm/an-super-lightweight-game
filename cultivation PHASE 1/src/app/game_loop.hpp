#pragma once
// Real time -> game time adapter. Integer math only: speed is whole game-minutes per real second,
// real time is microseconds, so the fractional remainder is carried exactly (no float drift).
// The simulation itself never sees real time, only whole minutes, and because it is event-driven the
// result is identical however those minutes are batched into update() calls.
#include <algorithm>
#include <cstdint>
#include "sim/simulation.hpp"

namespace cx {

class GameLoop {
public:
    static constexpr int64_t kMaxFrameUs = 250000;  // a stall must not turn into a huge catch-up burst
    static constexpr int64_t kMaxIdleUs = 1000000;

    explicit GameLoop(Simulation& sim) : sim_(sim) {}

    void set_speed(int64_t game_minutes_per_real_second) { speed_ = std::max<int64_t>(0, game_minutes_per_real_second); }
    int64_t speed() const { return speed_; }
    bool paused() const { return speed_ == 0; }

    // Returns the number of game minutes advanced.
    int64_t update(int64_t real_dt_us) {
        if (speed_ == 0) return 0;
        real_dt_us = std::clamp<int64_t>(real_dt_us, 0, kMaxFrameUs);
        acc_ += real_dt_us * speed_;  // unit: game-minute * microsecond
        const int64_t whole = acc_ / 1000000;
        acc_ %= 1000000;
        if (whole > 0) sim_.advance_by(whole);
        return whole;
    }

    // How long the caller may sleep before the next simulation event is due. This is what keeps idle CPU near zero.
    int64_t idle_hint_us() const {
        if (speed_ == 0 || !sim_.has_pending()) return kMaxIdleUs;
        const int64_t need = (sim_.next_event_time() - sim_.now()) * 1000000 - acc_;
        if (need <= 0) return 0;
        return std::min((need + speed_ - 1) / speed_, kMaxIdleUs);
    }

private:
    Simulation& sim_;
    int64_t speed_ = 0;
    int64_t acc_ = 0;
};

}  // namespace cx
