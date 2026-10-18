#include "stream.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace omawave::vst3 {
using namespace Steinberg;

StateStream::StateStream(std::vector<std::uint8_t> bytes, std::size_t limit)
    : bytes_(std::move(bytes)), limit_(std::min(limit, kLimit))
{
    if (bytes_.size() > limit_) { bytes_.clear(); failed_ = true; }
}
tresult StateStream::fail(tresult result) noexcept { failed_ = true; return result; }
tresult StateStream::queryInterface(const TUID id, void** object)
{
    if (!object || !id) return kInvalidArgument;
    *object = nullptr;
    // TUID is a byte array; callers need not align it like a uint64. The SDK's
    // generic implementation assumes that alignment, so use byte comparisons at
    // this host boundary while retaining the unmodified SDK/refcount machinery.
    if (std::memcmp(id, IBStream::iid.toTUID(), 16) == 0 || std::memcmp(id, FUnknown::iid.toTUID(), 16) == 0)
        *object = static_cast<IBStream*>(this);
    else if (std::memcmp(id, ISizeableStream::iid.toTUID(), 16) == 0)
        *object = static_cast<ISizeableStream*>(this);
    else return kNoInterface;
    addRef(); return kResultOk;
}
tresult StateStream::read(void* output, int32 size, int32* done)
{
    if (done) *done = 0;
    if (size < 0 || (!output && size)) return fail(kInvalidArgument);
    const auto count = std::min(static_cast<std::size_t>(size), bytes_.size() - std::min(cursor_, bytes_.size()));
    if (count) std::memcpy(output, bytes_.data() + cursor_, count);
    cursor_ += count;
    if (done) *done = static_cast<int32>(count);
    return kResultOk; // Like the SDK memory stream: EOF is a successful short read.
}
tresult StateStream::write(void* input, int32 size, int32* done)
{
    if (done) *done = 0;
    if (size < 0 || (!input && size)) return fail(kInvalidArgument);
    if (static_cast<std::size_t>(size) > limit_ - cursor_) return fail(kOutOfMemory);
    if (!size) return kResultOk;
    const auto address = reinterpret_cast<std::uintptr_t>(input);
    const auto base = reinterpret_cast<std::uintptr_t>(bytes_.data());
    const bool aliases = !bytes_.empty() && address >= base && address - base <= bytes_.size();
    const auto sourceOffset = aliases ? static_cast<std::size_t>(address - base) : 0;
    if (aliases && static_cast<std::size_t>(size) > bytes_.size() - sourceOffset)
        return fail(kInvalidArgument);
    const auto end = cursor_ + static_cast<std::size_t>(size);
    try { if (end > bytes_.size()) bytes_.resize(end, 0); }
    catch (const std::bad_alloc&) { return fail(kOutOfMemory); }
    std::memmove(bytes_.data() + cursor_, aliases ? bytes_.data() + sourceOffset : input, static_cast<std::size_t>(size));
    cursor_ = end;
    if (done) *done = size;
    return kResultOk;
}
tresult StateStream::seek(int64 offset, int32 mode, int64* result)
{
    if (result) *result = static_cast<int64>(cursor_);
    int64 base = 0;
    if (mode == kIBSeekCur) base = static_cast<int64>(cursor_);
    else if (mode == kIBSeekEnd) base = static_cast<int64>(bytes_.size());
    else if (mode != kIBSeekSet) return fail(kInvalidArgument);
    // Compare before adding, including INT64_MIN, and never negate the offset.
    if (offset < -base || offset > static_cast<int64>(limit_) - base)
        return fail(kInvalidArgument);
    cursor_ = static_cast<std::size_t>(base + offset);
    if (result) *result = static_cast<int64>(cursor_);
    return kResultOk;
}
tresult StateStream::tell(int64* result)
{
    if (!result) return fail(kInvalidArgument);
    *result = static_cast<int64>(cursor_); return kResultOk;
}
tresult StateStream::getStreamSize(int64& result)
{
    result = static_cast<int64>(bytes_.size()); return kResultOk;
}
tresult StateStream::setStreamSize(int64 size)
{
    if (size < 0 || static_cast<std::uint64_t>(size) > limit_) return fail(kInvalidArgument);
    try { bytes_.resize(static_cast<std::size_t>(size), 0); }
    catch (const std::bad_alloc&) { return fail(kOutOfMemory); }
    cursor_ = std::min(cursor_, bytes_.size());
    return kResultOk;
}
}
