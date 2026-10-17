// The consumer side of the shared-memory ring, used by both camera backends:
// the Media Foundation media source (one consumer per camera, inside Frame
// Server) and the DirectShow filter (one consumer per application using the
// camera).
#pragma once

#include <vector>

#include "common.h"
#include "vcam/convert.h"

namespace vcam::win {

class FrameFeed {
public:
    // `allow_local`: also look for the ring in the session's Local\ namespace
    // (DirectShow backend, see SharedFrames::Open).
    explicit FrameFeed(int camera, bool allow_local = false);

    // Sets the format the stream delivers and renders the placeholder for it.
    void Configure(PixelFormat format, int width, int height, uint32_t fps_num, uint32_t fps_den);
    // Attaches to the ring when possible and keeps the request and heartbeat
    // fresh. With several consumers, the first one that streams owns the
    // request; the others take over when it stops.
    void Tick(int64_t now);
    // The stream stopped: tell the producer nobody is watching.
    void Stop();

    HANDLE frame_event() const { return shm_.is_open() ? shm_.frame_event() : nullptr; }
    uint64_t latest() const;
    bool producer_alive(int64_t now) const;
    bool owns_request() const { return owner_; }

    // Writes the newest frame, or the placeholder, into dst (flipped upside
    // down if `flip`: bottom-up DirectShow RGB). Returns the producer frame
    // number written, 0 for the placeholder.
    uint64_t Render(const ImageSpan& dst, int64_t now, bool flip = false);
    void CountDelivery(bool fresh);

private:
    void LogProducerState(int64_t now);

    int camera_;
    bool allow_local_;
    Config config_;
    SharedFrames shm_;
    Converter converter_;
    shm::FrameRequest request_;
    std::vector<uint8_t> placeholder_;
    PixelFormat format_ = PixelFormat::Unknown;
    int width_ = 0, height_ = 0;
    int64_t next_attach_ = 0;
    bool request_published_ = false;
    bool owner_ = false;
    bool producer_was_alive_ = false;
};

}  // namespace vcam::win
