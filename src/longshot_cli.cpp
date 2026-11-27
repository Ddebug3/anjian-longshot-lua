#include "jpeg_stitcher.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: %s <overlap> <output.jpg> <frame1.jpg> <frame2.jpg> [frameN.jpg ...]\n",
            argv[0]);
        return 2;
    }

    const int overlap = std::atoi(argv[1]);
    if (overlap <= 0) {
        std::fprintf(stderr, "ERROR:invalid overlap\n");
        return 2;
    }

    const std::string outputPath = argv[2];
    std::vector<std::string> frames;
    for (int i = 3; i < argc; ++i) {
        frames.emplace_back(argv[i]);
    }
    std::vector<int> overlaps(frames.size(), overlap);

    std::string error;
    const bool ok = stitchJpegStream(
        frames,
        overlaps,
        20,
        outputPath,
        90,
        error
    );
    if (!ok) {
        std::fprintf(stderr, "ERROR:%s\n", error.c_str());
        return 1;
    }

    std::printf("OK:%s\n", outputPath.c_str());
    return 0;
}