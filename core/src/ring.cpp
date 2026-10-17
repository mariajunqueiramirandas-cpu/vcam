#include "vcam/ring.h"

#include <cstring>
#include <limits>

namespace vcam {

using namespace shm;

namespace {

uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

}  // namespace

uint64_t SlotCapacityFor(uint32_t max_width, uint32_t max_height)
{
    // BGRA is the largest format per pixel; rows are padded to kPitchAlign.
    return AlignUp(AlignUp(static_cast<uint64_t>(max_width) * 4, kPitchAlign) * max_height, kSlotAlign);
}

uint64_t ShmBytesFor(uint64_t slot_capacity)
{
    return kHeaderBytes + kSlotCount * AlignUp(kSlotHeaderBytes + slot_capacity, kSlotAlign);
}

bool InitializeShm(void* base, uint64_t mapped_bytes, uint64_t slot_capacity)
{
    if (!base || slot_capacity == 0 || slot_capacity > kMaxSlotCapacity ||
        ShmBytesFor(slot_capacity) > mapped_bytes) {
        return false;
    }
    auto* h = static_cast<Header*>(base);
    h->version = kVersion;
    h->header_bytes = kHeaderBytes;
    h->slot_count = kSlotCount;
    h->slot_stride = AlignUp(kSlotHeaderBytes + slot_capacity, kSlotAlign);
    h->slot_capacity = slot_capacity;
    h->total_bytes = ShmBytesFor(slot_capacity);
    AtomicStore(h->magic, kMagic);
    return true;
}

bool LayoutSlot(PixelFormat format, uint32_t width, uint32_t height, SlotHeader* slot)
{
    PlaneLayout layout;
    if (!DefaultLayout(format, width, height, &layout, kPitchAlign) ||
        layout.total_bytes > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    slot->format = static_cast<uint32_t>(format);
    slot->width = width;
    slot->height = height;
    for (int p = 0; p < 3; p++) {
        slot->offset[p] = p < layout.count ? layout.offset[p] : 0;
        slot->pitch[p] = p < layout.count ? layout.pitch[p] : 0;
    }
    slot->data_bytes = static_cast<uint32_t>(layout.total_bytes);
    return true;
}

bool ValidateSlot(const SlotHeader& slot, uint64_t capacity, const uint8_t* data, ImageView* view)
{
    if (!IsValidFormat(slot.format) || !IsValidColorSpace(slot.colorspace) || slot.data_bytes > capacity) {
        return false;
    }
    const auto format = static_cast<PixelFormat>(slot.format);
    ImageView v;
    v.format = format;
    v.width = static_cast<int>(slot.width);
    v.height = static_cast<int>(slot.height);
    for (int p = 0; p < PlaneCount(format); p++) {
        uint32_t row_bytes = 0, rows = 0;
        if (!PlaneGeometry(format, slot.width, slot.height, p, &row_bytes, &rows)) {
            return false;  // also rejects zero or oversized dimensions
        }
        const uint64_t pitch = slot.pitch[p];
        if (pitch < row_bytes || pitch > capacity) {
            return false;
        }
        const uint64_t end = static_cast<uint64_t>(slot.offset[p]) + pitch * (rows - 1) + row_bytes;
        if (end > slot.data_bytes) {
            return false;
        }
        v.plane[p] = data + slot.offset[p];
        v.pitch[p] = static_cast<int>(pitch);
    }
    *view = v;
    return true;
}

bool ProducerAlive(const Header* h, int64_t now)
{
    return h && AtomicLoad(h->producer_pid) != 0 &&
           now - AtomicLoad(h->producer_heartbeat) < kHeartbeatTimeout;
}

bool ConsumerStreaming(const Header* h, int64_t now)
{
    return h && (AtomicLoad(h->consumer_flags) & kConsumerStreaming) != 0 &&
           now - AtomicLoad(h->consumer_heartbeat) < kHeartbeatTimeout;
}

bool Ring::Attach(void* base, uint64_t mapped_bytes)
{
    Detach();
    if (!base || mapped_bytes < kHeaderBytes) {
        return false;
    }
    auto* h = static_cast<Header*>(base);
    if (AtomicLoad(h->magic) != kMagic) {
        return false;  // not initialised yet (or not ours)
    }
    // Read the immutable fields once; never trust them again afterwards.
    const uint32_t version = h->version;
    const uint32_t header_bytes = h->header_bytes;
    const uint32_t slots = h->slot_count;
    const uint64_t stride = h->slot_stride;
    const uint64_t capacity = h->slot_capacity;
    if (version != kVersion || header_bytes != kHeaderBytes || slots < 2 || slots > 16 || capacity == 0 ||
        capacity > kMaxSlotCapacity || stride < kSlotHeaderBytes + capacity || stride > kMaxSlotCapacity * 2 ||
        kHeaderBytes + slots * stride > mapped_bytes) {
        return false;
    }
    base_ = static_cast<uint8_t*>(base);
    header_ = h;
    capacity_ = capacity;
    stride_ = stride;
    slots_ = slots;
    return true;
}

void Ring::Detach()
{
    base_ = nullptr;
    header_ = nullptr;
    capacity_ = stride_ = 0;
    slots_ = 0;
}

uint64_t Ring::latest_frame() const
{
    return header_ ? AtomicLoad(header_->latest_frame) : 0;
}

SlotHeader* Ring::slot_header(uint64_t frame) const
{
    return reinterpret_cast<SlotHeader*>(base_ + kHeaderBytes + (frame % slots_) * stride_);
}

uint8_t* Ring::slot_data(uint64_t frame) const
{
    return base_ + kHeaderBytes + (frame % slots_) * stride_ + kSlotHeaderBytes;
}

void Ring::CopySlotHeader(const SlotHeader* from, SlotHeader* to)
{
    constexpr auto r = std::memory_order_relaxed;
    to->seq = AtomicLoad(from->seq, r);
    to->format = AtomicLoad(from->format, r);
    to->width = AtomicLoad(from->width, r);
    to->height = AtomicLoad(from->height, r);
    to->colorspace = AtomicLoad(from->colorspace, r);
    for (int p = 0; p < 3; p++) {
        to->offset[p] = AtomicLoad(from->offset[p], r);
        to->pitch[p] = AtomicLoad(from->pitch[p], r);
    }
    to->data_bytes = AtomicLoad(from->data_bytes, r);
    to->flags = AtomicLoad(from->flags, r);
    to->timestamp = AtomicLoad(from->timestamp, r);
    to->frame = AtomicLoad(from->frame, r);
}

Ring::WriteTarget Ring::BeginWrite()
{
    WriteTarget t;
    if (!header_) {
        return t;
    }
    t.frame = AtomicLoad(header_->latest_frame) + 1;
    SlotHeader* slot = slot_header(t.frame);
    AtomicStore(slot->seq, 2 * t.frame - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    t.data = slot_data(t.frame);
    t.capacity = capacity_;
    return t;
}

void Ring::CommitWrite(const WriteTarget& t, const SlotHeader& info)
{
    if (!header_ || !t.data) {
        return;
    }
    constexpr auto r = std::memory_order_relaxed;
    SlotHeader* slot = slot_header(t.frame);
    AtomicStore(slot->format, info.format, r);
    AtomicStore(slot->width, info.width, r);
    AtomicStore(slot->height, info.height, r);
    AtomicStore(slot->colorspace, info.colorspace, r);
    for (int p = 0; p < 3; p++) {
        AtomicStore(slot->offset[p], info.offset[p], r);
        AtomicStore(slot->pitch[p], info.pitch[p], r);
    }
    AtomicStore(slot->data_bytes, info.data_bytes, r);
    AtomicStore(slot->flags, info.flags, r);
    AtomicStore(slot->timestamp, info.timestamp, r);
    AtomicStore(slot->frame, t.frame, r);
    AtomicStore(slot->seq, 2 * t.frame);
    AtomicStore(header_->latest_frame, t.frame);
    std::atomic_ref<uint64_t>(header_->frames_written).fetch_add(1, r);
}

void Ring::WriteRequest(const FrameRequest& request)
{
    if (!header_) {
        return;
    }
    constexpr auto r = std::memory_order_relaxed;
    const uint64_t seq = AtomicLoad(header_->request_seq, r);
    const uint64_t odd = (seq & 1) ? seq + 2 : seq + 1;  // recover from a writer that died mid-update
    AtomicStore(header_->request_seq, odd, r);
    std::atomic_thread_fence(std::memory_order_release);
    FrameRequest& out = header_->request;
    AtomicStore(out.format, request.format, r);
    AtomicStore(out.width, request.width, r);
    AtomicStore(out.height, request.height, r);
    AtomicStore(out.fps_num, request.fps_num, r);
    AtomicStore(out.fps_den, request.fps_den, r);
    AtomicStore(out.colorspace, request.colorspace, r);
    AtomicStore(header_->request_seq, odd + 1);
}

bool Ring::ReadRequest(FrameRequest* request) const
{
    if (!header_) {
        return false;
    }
    constexpr auto r = std::memory_order_relaxed;
    for (int attempt = 0; attempt < 16; attempt++) {
        const uint64_t seq = AtomicLoad(header_->request_seq);
        if (seq & 1) {
            continue;
        }
        const FrameRequest& in = header_->request;
        FrameRequest copy;
        copy.format = AtomicLoad(in.format, r);
        copy.width = AtomicLoad(in.width, r);
        copy.height = AtomicLoad(in.height, r);
        copy.fps_num = AtomicLoad(in.fps_num, r);
        copy.fps_den = AtomicLoad(in.fps_den, r);
        copy.colorspace = AtomicLoad(in.colorspace, r);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (AtomicLoad(header_->request_seq, r) == seq) {
            *request = copy;
            return true;
        }
    }
    return false;
}

}  // namespace vcam
