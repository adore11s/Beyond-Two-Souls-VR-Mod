#pragma once
#include <deque>
#include <cstdint>
namespace b2s::xr {
class ReferenceChangeSchedule {
public:
    void Reset() { entries_.clear(); }
    void Add(uint64_t session, int32_t spaceType, int64_t changeTime) {
        if (!session) return;
        entries_.push_back({session, spaceType, changeTime});
    }
    bool ConsumeReady(uint64_t session, int32_t spaceType, int64_t now) {
        bool ready=false;
        for (auto it=entries_.begin(); it!=entries_.end();) {
            if (it->session == session && it->spaceType == spaceType && it->changeTime <= now) {
                it = entries_.erase(it); ready = true;
            } else ++it;
        }
        return ready;
    }
private:
    struct Entry { uint64_t session=0; int32_t spaceType=0; int64_t changeTime=0; };
    std::deque<Entry> entries_;
};
}
