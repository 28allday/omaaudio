#include "peak_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace omawave::io {
namespace {

// A cap, so a corrupt header cannot ask for an allocation the size of the disk. Far
// above what a 30-minute take needs: at 256 samples per peak that is ~5.6M floats.
constexpr std::uint64_t kMaxPeakValues = 1ull << 30;

class File {
public:
    File(const std::string& path, const char* mode) noexcept
        : file_(std::fopen(path.c_str(), mode))
    {
    }
    ~File()
    {
        if (file_ != nullptr)
            (void)std::fclose(file_);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    [[nodiscard]] std::FILE* get() const noexcept { return file_; }
    [[nodiscard]] bool valid() const noexcept { return file_ != nullptr; }

    // Closes early so the caller can check the result of flushing rather than
    // discovering a full disk in the destructor, where there is nothing to be done.
    [[nodiscard]] bool close() noexcept
    {
        if (file_ == nullptr)
            return true;
        const bool ok = std::fclose(file_) == 0;
        file_ = nullptr;
        return ok;
    }

private:
    std::FILE* file_ = nullptr;
};

} // namespace

std::string writePeakCache(const std::string& path, const PeakLevel& level)
{
    if (level.empty())
        return "refusing to write an empty peak cache";

    // Written beside the target and renamed over it: a cache file is either
    // whole or absent, so an interrupted save cannot leave one that is refused
    // on every load and, existing, never rewritten.
    const std::string staged = path + ".part";
    struct Abandon { const std::string& name; bool keep = false; ~Abandon() { if (!keep) std::remove(name.c_str()); } } abandon{staged};
    File file(staged, "wb");
    if (!file.valid())
        return "could not write " + path;

    const std::uint32_t samplesPerPeak = level.samplesPerPeak;
    const std::uint32_t channels = level.channels;
    const auto count = static_cast<std::uint64_t>(level.count);

    const bool headerOk =
        std::fwrite(kPeakMagic, 1, sizeof(kPeakMagic), file.get()) == sizeof(kPeakMagic)
        && std::fwrite(&kPeakVersion, sizeof(kPeakVersion), 1, file.get()) == 1
        && std::fwrite(&samplesPerPeak, sizeof(samplesPerPeak), 1, file.get()) == 1
        && std::fwrite(&channels, sizeof(channels), 1, file.get()) == 1
        && std::fwrite(&count, sizeof(count), 1, file.get()) == 1;
    if (!headerOk)
        return "short write to " + path;

    if (!level.data.empty()) {
        const std::size_t wrote =
            std::fwrite(level.data.data(), sizeof(float), level.data.size(), file.get());
        if (wrote != level.data.size())
            return "short write to " + path;
    }

    if (!file.close())
        return "could not flush " + path;
    if (std::rename(staged.c_str(), path.c_str()) != 0)
        return "could not write " + path;
    abandon.keep = true;
    return {};
}

PeakLevel readPeakCache(const std::string& path)
{
    const File file(path, "rb");
    if (!file.valid())
        return {};

    char magic[sizeof(kPeakMagic)]{};
    std::uint32_t version = 0;
    std::uint32_t samplesPerPeak = 0;
    std::uint32_t channels = 0;
    std::uint64_t count = 0;

    const bool headerOk =
        std::fread(magic, 1, sizeof(magic), file.get()) == sizeof(magic)
        && std::fread(&version, sizeof(version), 1, file.get()) == 1
        && std::fread(&samplesPerPeak, sizeof(samplesPerPeak), 1, file.get()) == 1
        && std::fread(&channels, sizeof(channels), 1, file.get()) == 1
        && std::fread(&count, sizeof(count), 1, file.get()) == 1;
    if (!headerOk)
        return {};

    if (std::memcmp(magic, kPeakMagic, sizeof(magic)) != 0)
        return {};
    if (version != kPeakVersion || samplesPerPeak == 0 || channels == 0 || count == 0)
        return {};

    const std::uint64_t values = count * channels * 2ull;
    if (values > kMaxPeakValues)
        return {};

    PeakLevel level;
    level.samplesPerPeak = samplesPerPeak;
    level.channels = channels;
    level.count = static_cast<std::size_t>(count);
    level.data.resize(static_cast<std::size_t>(values));

    const std::size_t got =
        std::fread(level.data.data(), sizeof(float), level.data.size(), file.get());
    if (got != level.data.size())
        return {};                  // truncated: rebuild rather than draw half a take

    return level;
}

} // namespace omawave::io
