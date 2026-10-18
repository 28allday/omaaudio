#pragma once

#include "export.h"

namespace omawave::render {

// One independent file, decoded/converted privately before the caller creates
// plugins at this rate and channel count on its owning GUI thread.
struct PreparedFile {
    bool ok = false;
    bool cancelled = false;
    std::string error, warning;
    session::AudioBuffer audio;
    std::uint32_t sampleRate = 0;
    ExportSpec output;
};

// Worker only. channels: 0 preserves, 1 averages, 2 duplicates mono or keeps
// stereo. Larger source layouts require preservation or explicit mono folding.
// Metadata belongs to this input, with checked rescaling and fresh loudness.
[[nodiscard]] PreparedFile prepareBatchFile(const std::string& path, ExportSpec output,
    const std::atomic<bool>* cancel = nullptr, const ProgressFn& progress = {});

} // namespace omawave::render
