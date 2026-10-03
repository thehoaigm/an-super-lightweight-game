#pragma once
// Event bus = (1) synchronous publish/subscribe + (2) a time-ordered schedule.
// The schedule is what makes the simulation event-driven: nothing runs per frame,
// work happens only at the minute something is due. Ordering is total: (when, seq),
// where seq is a monotonically increasing counter, so equal-time events fire FIFO
// and a save/load round trip cannot reorder anything.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "core/bytes.hpp"
#include "core/entity.hpp"

namespace cx {

struct Event {
    uint32_t type = 0;
    EntityId subject{};
    EntityId object{};
    int64_t a = 0;  // payload, meaning depends on type
    int64_t b = 0;
};

// Built-in event types. Content/mod events start at UserBase.
namespace ev {
enum : uint32_t {
    None = 0,
    DayStart = 1,
    MonthStart = 2,
    SeasonStart = 3,
    YearStart = 4,
    RegionTick = 10,
    UserBase = 1000,
};
}

class EventBus {
public:
    using Handler = std::function<void(const Event&)>;
    static constexpr int kMaxDepth = 16;

    // Handlers run in registration order. Not allowed during dispatch (would invalidate
    // the handler list that is being iterated).
    void subscribe(uint32_t type, Handler h) {
        if (depth_ > 0) throw std::logic_error("EventBus::subscribe during dispatch");
        handlers_[type].push_back(std::move(h));
    }

    // Synchronous. Handlers may publish further events (bounded by kMaxDepth) and schedule.
    void publish(const Event& e) {
        if (depth_ >= kMaxDepth) throw std::logic_error("EventBus: event recursion too deep");
        auto it = handlers_.find(e.type);
        if (it == handlers_.end()) return;
        DepthGuard guard(depth_);
        const std::vector<Handler>& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) list[i](e);
    }

    uint64_t schedule(int64_t when, const Event& e) {
        heap_.push_back(Slot{when, next_seq_, e});
        std::push_heap(heap_.begin(), heap_.end(), later);
        return next_seq_++;
    }

    bool has_scheduled() const { return !heap_.empty(); }
    size_t scheduled_count() const { return heap_.size(); }
    int64_t next_time() const { return heap_.front().when; }  // precondition: has_scheduled()

    // Pops the earliest event if it is due at or before `until`.
    bool pop_due(int64_t until, int64_t& when, Event& out) {
        if (heap_.empty() || heap_.front().when > until) return false;
        std::pop_heap(heap_.begin(), heap_.end(), later);
        when = heap_.back().when;
        out = heap_.back().ev;
        heap_.pop_back();
        return true;
    }

    // Canonical (sorted) form: identical logical state -> identical bytes, whatever the heap layout.
    void save(ByteWriter& w) const {
        std::vector<Slot> sorted = heap_;
        std::sort(sorted.begin(), sorted.end(), [](const Slot& a, const Slot& b) {
            return a.when != b.when ? a.when < b.when : a.seq < b.seq;
        });
        w.u64(next_seq_);
        w.u32(uint32_t(sorted.size()));
        for (const Slot& s : sorted) {
            w.i64(s.when);
            w.u64(s.seq);
            w.u32(s.ev.type);
            w.u64(s.ev.subject.raw);
            w.u64(s.ev.object.raw);
            w.i64(s.ev.a);
            w.i64(s.ev.b);
        }
    }

    // Replaces the schedule only. Subscribers are code, not data, and are never saved.
    bool load(ByteReader& r) {
        const uint64_t next = r.u64();
        const uint32_t n = r.u32();
        if (!r.ok() || size_t(n) > r.remaining() / kSlotBytes) return false;
        std::vector<Slot> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Slot s;
            s.when = r.i64();
            s.seq = r.u64();
            s.ev.type = r.u32();
            s.ev.subject.raw = r.u64();
            s.ev.object.raw = r.u64();
            s.ev.a = r.i64();
            s.ev.b = r.i64();
            if (!r.ok() || s.seq >= next) return false;
            v.push_back(s);
        }
        heap_ = std::move(v);
        std::make_heap(heap_.begin(), heap_.end(), later);
        next_seq_ = next;
        return true;
    }

private:
    struct Slot {
        int64_t when = 0;
        uint64_t seq = 0;
        Event ev;
    };
    static constexpr size_t kSlotBytes = 8 + 8 + 4 + 8 + 8 + 8 + 8;
    static bool later(const Slot& a, const Slot& b) { return a.when != b.when ? a.when > b.when : a.seq > b.seq; }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    };

    std::vector<Slot> heap_;
    std::unordered_map<uint32_t, std::vector<Handler>> handlers_;
    uint64_t next_seq_ = 1;
    int depth_ = 0;
};

}  // namespace cx
