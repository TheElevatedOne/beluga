// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the details of audio and video files (format, streams, tags), read with FFmpeg's libavformat, the
// library ffprobe is built on. Optional at build time (BELUGA_FFMPEG); without it, nothing is read.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace beluga::mediainfo {

// Whether Beluga was built with FFmpeg.
bool available();

// Label and value rows for the Information panel, like ffprobe's: duration, container, bit rate, the tags
// (title, artist, album …) and each stream (codec, size, frame rate, sample rate, channels). Empty when the
// file can't be read. Blocks: call it off the main thread.
std::vector<std::pair<std::string, std::string>> probe(const std::string &path);

} // namespace beluga::mediainfo
