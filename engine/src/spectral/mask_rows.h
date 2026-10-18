#pragma once
#include "mask.h"
#include <algorithm>

namespace omawave::spectral {
// Worker-owned rows with overlap on both sides of a rasterisation block. The
// halo is clipped against the complete mask, not against each block, so feathered
// edges have exactly the same weights as a whole-grid rasterisation.
class MaskRows {
public:
    MaskRows(const MaskSource& source, MaskFrame frame)
        : source_(source), frame_(frame), halo_(maskColumnHalo(source)) { load(0); }
    bool uniform() const { return grid_.uniform; }
    bool valid(bool strict = false) const {
        return grid_.bins == frame_.bins && (grid_.uniform || grid_.columns == end_ - begin_)
            && (strict ? grid_.weight.size() == (grid_.uniform ? 1 : end_ - begin_) * frame_.bins
                       : grid_.weight.size() >= (grid_.uniform ? 1 : end_ - begin_) * frame_.bins);
    }
    const float* row(std::size_t column) {
        if (column < usableBegin_ || column >= usableEnd_) load(column);
        return grid_.row(column - begin_);
    }
private:
    void load(std::size_t column) {
        constexpr std::size_t block = 128;
        usableBegin_ = halo_ ? column / block * block : 0;
        usableEnd_ = halo_ ? std::min(frame_.columns, usableBegin_ + block) : frame_.columns;
        begin_ = halo_ && usableBegin_ > *halo_ ? usableBegin_ - *halo_ : 0;
        end_ = halo_ ? usableEnd_ + std::min<std::size_t>(*halo_, frame_.columns - usableEnd_) : frame_.columns;
        auto part = frame_;
        part.firstFrame += begin_ * frame_.hop;
        part.columns = end_ - begin_;
        grid_ = source_(part);
        if (grid_.uniform) { usableBegin_ = 0; usableEnd_ = frame_.columns; }
    }
    const MaskSource& source_;
    MaskFrame frame_;
    std::optional<std::uint32_t> halo_;
    MaskGrid grid_;
    std::size_t begin_ = 0, end_ = 0, usableBegin_ = 0, usableEnd_ = 0;
};
} // namespace omawave::spectral
