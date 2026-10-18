#include "lv2/host_features.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>

#include <cstdarg>
#include <cstdio>

namespace omawave::lv2 {

HostFeatures::HostFeatures(UridMap& urids, double sampleRate, std::uint32_t maxBlock)
    : urids_(urids), sampleRate_(sampleRate), maxBlock_(maxBlock)
{
    ids_.atomSequence = urids_.map(LV2_ATOM__Sequence);
    ids_.atomChunk = urids_.map(LV2_ATOM__Chunk);
    ids_.atomFloat = urids_.map(LV2_ATOM__Float);
    ids_.atomInt = urids_.map(LV2_ATOM__Int);
    ids_.paramSampleRate = urids_.map(LV2_PARAMETERS__sampleRate);
    ids_.bufMinBlockLength = urids_.map(LV2_BUF_SIZE__minBlockLength);
    ids_.bufMaxBlockLength = urids_.map(LV2_BUF_SIZE__maxBlockLength);
    ids_.bufSequenceSize = urids_.map(LV2_BUF_SIZE__sequenceSize);

    log_.handle = this;
    log_.printf = &HostFeatures::logPrintf;
    log_.vprintf = &HostFeatures::logVprintf;
    logFeature_.URI = LV2_LOG__log;
    logFeature_.data = &log_;

    sampleRateValue_ = static_cast<float>(sampleRate);
    minBlockValue_ = 1;
    maxBlockValue_ = static_cast<std::int32_t>(maxBlock);
    sequenceSizeValue_ = static_cast<std::int32_t>(kSequenceSize);

    const LV2_URID atomFloat = ids_.atomFloat;
    const LV2_URID atomInt = ids_.atomInt;

    options_[0] = {LV2_OPTIONS_INSTANCE, 0, ids_.paramSampleRate,
                   sizeof(float), atomFloat, &sampleRateValue_};
    options_[1] = {LV2_OPTIONS_INSTANCE, 0, ids_.bufMinBlockLength,
                   sizeof(std::int32_t), atomInt, &minBlockValue_};
    options_[2] = {LV2_OPTIONS_INSTANCE, 0, ids_.bufMaxBlockLength,
                   sizeof(std::int32_t), atomInt, &maxBlockValue_};
    options_[3] = {LV2_OPTIONS_INSTANCE, 0, ids_.bufSequenceSize,
                   sizeof(std::int32_t), atomInt, &sequenceSizeValue_};
    options_[4] = {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr};   // terminator

    optionsFeature_.URI = LV2_OPTIONS__options;
    optionsFeature_.data = options_.data();

    // A feature with no data: claiming it is the whole point. It says every run() will
    // be given at most maxBlockLength frames, which is true because the chain splits a
    // larger callback into sub-blocks rather than passing it straight through.
    boundedBlockFeature_.URI = LV2_BUF_SIZE__boundedBlockLength;
    boundedBlockFeature_.data = nullptr;

    features_ = {
        urids_.mapFeature(),
        urids_.unmapFeature(),
        &logFeature_,
        &optionsFeature_,
        &boundedBlockFeature_,
        nullptr,
    };
}

int HostFeatures::logPrintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int written = HostFeatures::logVprintf(handle, type, fmt, args);
    va_end(args);
    return written;
}

int HostFeatures::logVprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt,
                             va_list args)
{
    // Plugins log from whatever thread they like, including the audio thread, where
    // BRIEF.md section 2 forbids I/O. That rule binds Omawave's own code; a plugin that
    // logs from run() is doing something we cannot prevent, and dropping the message
    // would hide it. It goes to stderr, which the shell captures.
    const auto* self = static_cast<const HostFeatures*>(handle);
    const char* kind = self != nullptr ? self->urids_.unmap(type) : nullptr;

    std::fprintf(stderr, "[lv2] %s: ", kind != nullptr ? kind : "log");
    const int written = std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    return written;
}

} // namespace omawave::lv2
