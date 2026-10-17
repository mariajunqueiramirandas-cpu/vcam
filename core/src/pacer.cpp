#include "vcam/pacer.h"

#include <algorithm>

namespace vcam {

void FramePacer::Start(int64_t interval, int64_t now)
{
    interval_ = interval > 0 ? interval : 333333;
    credit_ = interval_;  // the first sample goes out immediately
    last_time_ = now;
    last_frame_ = 0;
    seen_frame_ = 0;
    seen_time_ = now - 10 * interval_;
}

void FramePacer::Accrue(int64_t now)
{
    if (now > last_time_) {
        credit_ = std::min(credit_ + (now - last_time_), interval_ * 3 / 2);
        last_time_ = now;
    }
}

bool FramePacer::Poll(int64_t now, uint64_t latest, bool producer_alive, int64_t* wake_at)
{
    Accrue(now);
    if (latest != seen_frame_) {
        seen_frame_ = latest;
        seen_time_ = now;
    }
    const bool fresh = producer_alive && latest != 0 && latest != last_frame_;
    // Frames arrived recently: give a late one some slack before repeating, so a
    // producer running at the camera's rate keeps driving the timing.
    const bool producer_streaming = producer_alive && now - seen_time_ < 2 * interval_;
    int64_t needed;
    if (fresh) {
        needed = interval_ * 4 / 5;
    } else {
        needed = producer_streaming ? interval_ * 3 / 2 : interval_;
    }
    if (credit_ >= needed) {
        return true;
    }
    *wake_at = now + (needed - credit_);
    return false;
}

void FramePacer::Delivered(int64_t now, uint64_t frame)
{
    Accrue(now);
    credit_ -= interval_;
    last_frame_ = frame;
}

}  // namespace vcam
