#pragma once
#include "session/disk_audio.h"
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace omawave::spectral {
// A heal needs the far end of even an hours-long covered run before it can
// interpolate its beginning. The first analysis pass records only the endpoints;
// the second streams the original spectrum again. Linked records on checked,
// unlinked disk scratch keep even masks with many separate runs out of RAM.
class HealRuns {
public:
    static constexpr auto none = std::numeric_limits<std::uint64_t>::max();
    struct Run {
        std::uint64_t next = none, start = none, end = none;
        float left = 0, right = 0;
    };
    static_assert(sizeof(Run) == 8 * sizeof(float));
    HealRuns(std::uint32_t bins, std::size_t columns)
        : first_(bins, none), last_(bins, none), current_(bins) {
        const auto perBin = columns / 2 + columns % 2;
        if (perBin > std::numeric_limits<std::size_t>::max() / 8 / bins)
            throw std::length_error("heal scratch geometry is too large");
        capacity_ = std::max<std::size_t>(8, perBin * bins * 8);
    }
    void add(std::uint32_t bin, Run run) {
        if (!disk_) disk_ = session::DiskAudio::create(1, capacity_);
        if (last_[bin] != none) {
            const auto link = std::bit_cast<std::array<float, 2>>(next_);
            disk_->writePlane(0, last_[bin], link);
        } else first_[bin] = next_;
        disk_->writePlane(0, next_, std::bit_cast<std::array<float, 8>>(run));
        last_[bin] = next_;
        next_ += 8;
    }
    void rewind() {
        for (std::size_t k = 0; k < first_.size(); ++k) current_[k] = read(first_[k]);
    }
    const Run& at(std::uint32_t bin, std::size_t column) {
        auto& run = current_[bin];
        while (run.end <= column) run = read(run.next);
        return run;
    }
private:
    Run read(std::uint64_t offset) const {
        if (offset == none) return {};
        std::array<float, 8> data;
        if (!disk_->readPlane(0, offset, data)) throw std::runtime_error("could not read heal scratch");
        return std::bit_cast<Run>(data);
    }
    std::shared_ptr<session::DiskAudio> disk_;
    std::vector<std::uint64_t> first_, last_;
    std::vector<Run> current_;
    std::uint64_t next_ = 0;
    std::size_t capacity_ = 0;
};
} // namespace omawave::spectral
