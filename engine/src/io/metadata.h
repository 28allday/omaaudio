#pragma once

#include "session/metadata.h"

#include <cstdint>
#include <string>
#include <vector>

typedef struct sf_private_tag SNDFILE;

namespace omawave::io {

// The metadata libsndfile can carry in a WAV, BWF, RF64, W64 or AIFF file:
// the `bext` chunk, cue points with labels, an iXML chunk and the INFO strings.
// Reading never fails on absence; a file without a chunk yields an empty field.
[[nodiscard]] session::AudioMetadata readMetadata(SNDFILE* file);
[[nodiscard]] session::AudioMetadata readMetadata(const std::string& path);

// Reads the bext, iXML and INFO strings through libsndfile, and the markers
// from the file's own `cue ` and `LIST/adtl` chunks when a path is given (a
// RIFF walker of ours: libsndfile writes no labels and reads no cues in RF64).
[[nodiscard]] session::AudioMetadata readMetadata(SNDFILE* file, const std::string& path);

// Writes bext, iXML and INFO to a file open for writing, before any samples:
// libsndfile rejects a bext or chunk once audio has been written. Markers are
// not written here. A returned string is the first thing that could not be written.
[[nodiscard]] std::string writeMetadata(SNDFILE* file, const session::AudioMetadata& metadata);

// Appends standard `cue ` and `LIST/adtl` (labl, ltxt for ranges) chunks to a
// finished WAV, BWF or RF64 and patches its RIFF or ds64 size. Call after
// libsndfile has closed the file. Empty when nothing to write; a returned
// string is the reason it could not be done.
[[nodiscard]] std::string appendMarkers(const std::string& path, const std::vector<session::FileMarker>& markers);

// The markers a RIFF/RF64 file carries, from its own chunks (empty otherwise).
// Seeks over audio; retains at most 16 MiB of marker payloads and 65536 ids.
// Malformed or oversized metadata never causes a whole-audio allocation.
[[nodiscard]] std::vector<session::FileMarker> readMarkerChunks(const std::string& path);

// "yyyy-mm-dd" / "hh:mm:ss" for now, in local time, the way bext wants them.
[[nodiscard]] std::string todayForBext();
[[nodiscard]] std::string nowForBext();

// One EBU coding-history line: "A=PCM,F=48000,W=24,M=stereo,T=OmaAudio 0.1.0".
[[nodiscard]] std::string codingHistoryLine(std::uint32_t rate, int bits, std::uint32_t channels,
                                            const std::string& tool);

} // namespace omawave::io
