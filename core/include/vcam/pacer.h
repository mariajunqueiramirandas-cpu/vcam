// Decides when the camera emits a sample. Portable and clock-agnostic (ticks of
// 100 ns), so it can be unit tested.
//
// A real camera delivers at its nominal rate. A virtual one also wants the
// lowest latency, so a fresh producer frame goes out as soon as it arrives.
// The rate is bounded with a credit scheme: credit accrues in real time and
// each delivery spends one frame interval of it. A fresh frame may go out a
// little early (absorbing producer jitter), but the long-run rate can never
// exceed the nominal one. When the producer is slower than the camera, or
// absent, the last frame (or the placeholder) is repeated on a steady schedule.
// Credit is capped, so after a stall delivery resumes without a burst.
#pragma once

#include <cstdint>

namespace vcam {

class FramePacer {
public:
    void Start(int64_t interval, int64_t now);

    // latest: newest producer frame number (0 if none); producer_alive: the
    // producer is attached and its heartbeat is fresh. Returns true when a
    // sample should be delivered now; otherwise *wake_at is when to ask again
    // (a new frame arriving earlier is also a reason to ask again).
    bool Poll(int64_t now, uint64_t latest, bool producer_alive, int64_t* wake_at);

    // Records a delivery. frame is the producer frame sent, 0 for a placeholder.
    void Delivered(int64_t now, uint64_t frame);

    int64_t interval() const { return interval_; }
    uint64_t last_frame() const { return last_frame_; }

private:
    void Accrue(int64_t now);

    int64_t interval_ = 333333;
    int64_t credit_ = 0;  // in ticks; one frame costs interval_
    int64_t last_time_ = 0;
    uint64_t last_frame_ = 0;
    uint64_t seen_frame_ = 0;
    int64_t seen_time_ = 0;
};

}  // namespace vcam
