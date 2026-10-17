// The shared-memory frame ring between a producer (an app feeding the camera)
// and a consumer (the camera's media source inside Windows Frame Server).
//
// Layout: a 4 KiB header followed by kSlotCount slots. Each slot is a 256-byte
// slot header and its pixel data. The producer writes frame f into slot
// f % slot_count under a per-slot seqlock (seq is odd while writing, 2f when
// complete) and then publishes f in Header::latest_frame. Readers never block
// the writer: they read the newest slot in place and check afterwards that it
// was not rewritten meanwhile. A crashed process can therefore never wedge the
// other side.
//
// The consumer publishes the format it is streaming (FrameRequest, also under a
// seqlock) so the producer can render and convert to exactly that, which makes
// the consumer's work a plain copy.
//
// Everything in the mapping is untrusted: the consumer runs as a service and any
// user may write the mapping, so readers copy immutable fields once at Attach()
// and validate every slot header before touching pixel data.
#pragma once

#include <atomic>
#include <cstdint>

#include "vcam/format.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded because of alignas: intended, it keeps sides on separate cache lines
#endif

namespace vcam {
namespace shm {

constexpr uint32_t kMagic = 0x4D414356u;  // "VCAM"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSlotCount = 3;
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kSlotHeaderBytes = 256;
constexpr uint32_t kSlotAlign = 4096;
constexpr uint32_t kPitchAlign = 64;
constexpr uint64_t kMaxSlotCapacity = 512ull << 20;

// Clock ticks are 100 ns units of the system's monotonic clock (QPC on Windows).
constexpr int64_t kTicksPerSecond = 10'000'000;
constexpr int64_t kHeartbeatTimeout = 2 * kTicksPerSecond;

constexpr uint32_t kProducerAttached = 1;
constexpr uint32_t kConsumerStreaming = 1;

struct FrameRequest {
    uint32_t format = 0;  // PixelFormat
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 0;
    uint32_t colorspace = 0;  // ColorSpace
    uint32_t reserved[2] = {};
};

struct alignas(64) Header {
    // Set once by the creator; `magic` is stored last.
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_count;
    uint64_t slot_stride;
    uint64_t slot_capacity;
    uint64_t total_bytes;
    uint8_t reserved0[24];

    // Producer side.
    alignas(64) uint64_t latest_frame;  // newest complete frame number, 0 = none yet
    int64_t producer_heartbeat;         // clock ticks; refreshed at least every 250 ms
    uint32_t producer_pid;              // 0 when no producer is attached
    uint32_t producer_flags;
    uint64_t frames_written;
    uint8_t reserved1[32];

    // Consumer side.
    alignas(64) uint64_t request_seq;  // seqlock around `request`
    FrameRequest request;
    int64_t consumer_heartbeat;
    uint32_t consumer_pid;
    uint32_t consumer_flags;
    uint64_t frames_delivered;
    uint64_t frames_repeated;
};

struct alignas(64) SlotHeader {
    uint64_t seq;  // 2f - 1 while frame f is being written, 2f once complete
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t colorspace;
    uint32_t offset[3];  // plane offsets from the start of the slot's pixel data
    uint32_t pitch[3];
    uint32_t data_bytes;
    uint32_t flags;
    uint32_t content[4];  // x, y, width, height of the picture inside padding; all 0 = whole frame
    int64_t timestamp;    // capture time in clock ticks, 0 if unknown
    uint64_t frame;
};

static_assert(sizeof(Header) <= kHeaderBytes, "header too large");
static_assert(sizeof(SlotHeader) <= kSlotHeaderBytes, "slot header too large");

}  // namespace shm
}  // namespace vcam

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vcam {

// Pixel bytes one slot needs to hold any format up to max_width x max_height.
uint64_t SlotCapacityFor(uint32_t max_width, uint32_t max_height);
// Size of the whole mapping for a given slot capacity.
uint64_t ShmBytesFor(uint64_t slot_capacity);

// Lays out a freshly created, zero-filled mapping and publishes it (magic last).
bool InitializeShm(void* base, uint64_t mapped_bytes, uint64_t slot_capacity);

// Fills offsets/pitches/data_bytes of `slot` for a tightly packed frame.
bool LayoutSlot(PixelFormat format, uint32_t width, uint32_t height, shm::SlotHeader* slot);

// Checks a (local copy of a) slot header against the slot capacity and, on
// success, builds a view of its pixels at `data`.
bool ValidateSlot(const shm::SlotHeader& slot, uint64_t capacity, const uint8_t* data, ImageView* view);

// The picture area of a validated slot (the whole frame unless it was padded).
ImageView SlotContent(const shm::SlotHeader& slot, const ImageView& view);

bool ProducerAlive(const shm::Header* header, int64_t now);
bool ConsumerStreaming(const shm::Header* header, int64_t now);

class Ring {
public:
    // Attaches to an initialised mapping; false if it is not (yet) valid.
    bool Attach(void* base, uint64_t mapped_bytes);
    void Detach();
    bool attached() const { return header_ != nullptr; }
    shm::Header* header() const { return header_; }
    uint64_t capacity() const { return capacity_; }
    uint64_t latest_frame() const;

    // ---- writer (a single producer) ----
    struct WriteTarget {
        uint8_t* data = nullptr;
        uint64_t capacity = 0;
        uint64_t frame = 0;
    };
    // Claims the next slot. Its previous contents are invalid from here on.
    WriteTarget BeginWrite();
    // Publishes the slot. `info` supplies format, size, layout and timestamp.
    void CommitWrite(const WriteTarget& target, const shm::SlotHeader& info);

    // ---- reader ----
    enum class ReadStatus { NoFrame, Ok, Busy, Invalid };
    // Calls fn(const ImageView&, const shm::SlotHeader&) on the newest frame while
    // it sits in shared memory, then verifies it was not overwritten during the
    // call. Returns Ok only when fn saw a consistent frame.
    template <typename Fn>
    ReadStatus ReadLatest(Fn&& fn, uint64_t* frame_out = nullptr);

    // ---- format request: consumer writes, producer reads ----
    void WriteRequest(const shm::FrameRequest& request);
    bool ReadRequest(shm::FrameRequest* request) const;

private:
    shm::SlotHeader* slot_header(uint64_t frame) const;
    uint8_t* slot_data(uint64_t frame) const;
    static void CopySlotHeader(const shm::SlotHeader* from, shm::SlotHeader* to);

    uint8_t* base_ = nullptr;
    shm::Header* header_ = nullptr;
    uint64_t capacity_ = 0;
    uint64_t stride_ = 0;
    uint32_t slots_ = 0;
};

// Atomic access to fields of the mapping.
template <typename T>
inline T AtomicLoad(const T& field, std::memory_order order = std::memory_order_acquire)
{
    return std::atomic_ref<T>(const_cast<T&>(field)).load(order);
}

template <typename T>
inline void AtomicStore(T& field, T value, std::memory_order order = std::memory_order_release)
{
    std::atomic_ref<T>(field).store(value, order);
}

template <typename Fn>
Ring::ReadStatus Ring::ReadLatest(Fn&& fn, uint64_t* frame_out)
{
    if (!header_) {
        return ReadStatus::NoFrame;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint64_t frame = AtomicLoad(header_->latest_frame);
        if (frame == 0) {
            return ReadStatus::NoFrame;
        }
        shm::SlotHeader* slot = slot_header(frame);
        const uint64_t seq = AtomicLoad(slot->seq);
        if (seq != 2 * frame) {
            continue;  // the writer lapped us; a newer frame is published by now
        }
        shm::SlotHeader copy;
        CopySlotHeader(slot, &copy);
        ImageView view;
        const bool valid = ValidateSlot(copy, capacity_, slot_data(frame), &view);
        if (valid) {
            fn(static_cast<const ImageView&>(view), static_cast<const shm::SlotHeader&>(copy));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (AtomicLoad(slot->seq, std::memory_order_relaxed) != seq) {
            continue;
        }
        if (!valid) {
            return ReadStatus::Invalid;
        }
        if (frame_out) {
            *frame_out = frame;
        }
        return ReadStatus::Ok;
    }
    return ReadStatus::Busy;
}

}  // namespace vcam
