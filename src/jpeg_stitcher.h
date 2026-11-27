#pragma once
#include <string>
#include <vector>

bool stitchJpegStream(
    const std::vector<std::string>& frames,
    const std::vector<int>& overlaps,
    int overlapSafety,
    const std::string& outputPath,
    int quality,
    std::string& error
);