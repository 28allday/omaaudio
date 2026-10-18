#pragma once

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace omawave::vst3 {

// Control-thread state I/O, never a callback buffer. A plugin can seek past EOF
// before writing; gaps are zero-filled. Invalid operations leave bytes/cursor
// intact and record a sticky failure so ignored write errors cannot save a
// truncated state as if it were complete.
class StateStream final : public Steinberg::U::Implements<
    Steinberg::U::Directly<Steinberg::IBStream, Steinberg::ISizeableStream>> {
public:
    static constexpr std::size_t kLimit = 64 * 1024 * 1024;
    explicit StateStream(std::vector<std::uint8_t> bytes = {}, std::size_t limit = kLimit);
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override;
    Steinberg::tresult PLUGIN_API read(void*, Steinberg::int32, Steinberg::int32*) override;
    Steinberg::tresult PLUGIN_API write(void*, Steinberg::int32, Steinberg::int32*) override;
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64, Steinberg::int32, Steinberg::int64*) override;
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64*) override;
    Steinberg::tresult PLUGIN_API getStreamSize(Steinberg::int64&) override;
    Steinberg::tresult PLUGIN_API setStreamSize(Steinberg::int64) override;
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    Steinberg::tresult fail(Steinberg::tresult result) noexcept;
    std::vector<std::uint8_t> bytes_;
    std::size_t cursor_ = 0, limit_ = kLimit;
    bool failed_ = false;
};
}
