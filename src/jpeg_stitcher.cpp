#include "jpeg_stitcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <cstdint>
#include <cmath>
#include <setjmp.h>
#include <algorithm>
#include <jpeglib.h>

namespace {

struct ErrorManager {
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void onError(j_common_ptr cinfo) {
    ErrorManager* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool readFileBytes(const std::string& path, std::vector<unsigned char>& bytes, std::string& error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "open failed: " + path;
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "seek failed: " + path;
        return false;
    }
    long size = std::ftell(file);
    if (size <= 0 || size > 64L * 1024L * 1024L) {
        std::fclose(file);
        error = "invalid frame file size: " + path;
        return false;
    }
    std::rewind(file);
    bytes.resize(static_cast<size_t>(size));
    size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        error = "short read: " + path;
        return false;
    }
    return true;
}

bool decodeBase64(const std::vector<unsigned char>& raw, std::vector<unsigned char>& jpeg, std::string& error) {
    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) start = 3;

    static const unsigned char prefix[] = "base64,";
    for (size_t i = start; i + sizeof(prefix) - 1 <= raw.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < sizeof(prefix) - 1; ++j) {
            if (raw[i + j] != prefix[j]) { match = false; break; }
        }
        if (match) { start = i + sizeof(prefix) - 1; break; }
    }

    jpeg.clear();
    jpeg.reserve((raw.size() - start) * 3 / 4 + 4);
    uint32_t value = 0;
    int bits = -8;
    for (size_t i = start; i < raw.size(); ++i) {
        unsigned char c = raw[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        if (c == '=') break;
        int digit = base64Value(c);
        if (digit < 0) {
            error = "invalid base64 frame data";
            return false;
        }
        value = (value << 6) | static_cast<uint32_t>(digit);
        bits += 6;
        if (bits >= 0) {
            jpeg.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    if (jpeg.size() < 4) {
        error = "empty base64 frame data";
        return false;
    }
    return true;
}

bool loadJpegBytes(const std::string& path, std::vector<unsigned char>& jpeg, std::string& error) {
    std::vector<unsigned char> raw;
    if (!readFileBytes(path, raw, error)) return false;

    size_t p = 0;
    while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\r' || raw[p] == '\n' || raw[p] == '\t')) ++p;
    if (p + 2 < raw.size() && raw[p] == 0xFF && raw[p + 1] == 0xD8) {
        jpeg.swap(raw);
    } else if (!decodeBase64(raw, jpeg, error)) {
        return false;
    }

    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        error = "frame is not JPEG: " + path;
        return false;
    }
    return true;
}

bool readJpegSize(const std::string& path, int& width, int& height, std::string& error) {
    std::vector<unsigned char> jpegBytes;
    if (!loadJpegBytes(path, jpegBytes, error)) return false;

    jpeg_decompress_struct cinfo;
    ErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = onError;
    jerr.message[0] = '\0';

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        error = std::string("jpeg header error: ") + jerr.message + " " + path;
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpegBytes.data(), static_cast<unsigned long>(jpegBytes.size()));
    jpeg_read_header(&cinfo, TRUE);
    width = static_cast<int>(cinfo.image_width);
    height = static_cast<int>(cinfo.image_height);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool decodeJpegRgb(
    const std::string& path,
    int expectedWidth,
    std::vector<unsigned char>& pixels,
    int& width,
    int& height,
    std::string& error
) {
    std::vector<unsigned char> jpegBytes;
    if (!loadJpegBytes(path, jpegBytes, error)) return false;

    jpeg_decompress_struct in;
    ErrorManager inErr;
    in.err = jpeg_std_error(&inErr.pub);
    inErr.pub.error_exit = onError;
    inErr.message[0] = '\0';

    if (setjmp(inErr.jump)) {
        jpeg_destroy_decompress(&in);
        error = std::string("jpeg pixel decode error: ") + inErr.message + " " + path;
        return false;
    }

    jpeg_create_decompress(&in);
    jpeg_mem_src(&in, jpegBytes.data(), static_cast<unsigned long>(jpegBytes.size()));
    jpeg_read_header(&in, TRUE);
    in.out_color_space = JCS_RGB;
    jpeg_start_decompress(&in);

    width = static_cast<int>(in.output_width);
    height = static_cast<int>(in.output_height);
    if (width <= 0 || height <= 0 || (expectedWidth > 0 && width != expectedWidth)) {
        jpeg_destroy_decompress(&in);
        error = "decoded frame size mismatch";
        return false;
    }

    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    while (in.output_scanline < in.output_height) {
        size_t offset = static_cast<size_t>(in.output_scanline) * static_cast<size_t>(width) * 3;
        JSAMPROW rowPointer = pixels.data() + offset;
        if (jpeg_read_scanlines(&in, &rowPointer, 1) != 1) {
            jpeg_destroy_decompress(&in);
            error = "failed reading decoded frame";
            return false;
        }
    }

    jpeg_finish_decompress(&in);
    jpeg_destroy_decompress(&in);
    return true;
}

int refineOverlap(
    const std::vector<unsigned char>& previous,
    const std::vector<unsigned char>& current,
    int width,
    int height,
    int initialOverlap,
    int searchRadius
) {
    if (previous.size() != current.size() ||
        previous.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3) {
        return initialOverlap;
    }

    int initial = std::max(1, std::min(height - 1, initialOverlap));
    int minOverlap = std::max(1, initial - searchRadius);
    int maxOverlap = std::min(height - 1, initial + searchRadius);
    int bestOverlap = initial;
    double bestScore = 1e300;

    const int rows = 30;
    const int cols = 56;
    int margin = std::max(2, width / 20);
    int usableWidth = std::max(1, width - margin * 2);
    std::vector<int> columnPositions(cols);
    for (int c = 0; c < cols; ++c) {
        columnPositions[c] = margin + static_cast<int>((c + 0.5) * usableWidth / cols);
    }

    for (int overlap = minOverlap; overlap <= maxOverlap; ++overlap) {
        int shift = height - overlap;
        int topSkip = std::max(1, static_cast<int>(height * 0.10));
        int bottomSkip = std::max(1, static_cast<int>(height * 0.06));
        int sampleStart = topSkip;
        int sampleEnd = std::min(overlap - 1, height - bottomSkip - 1);
        if (sampleEnd - sampleStart < rows) {
            sampleStart = 0;
            sampleEnd = overlap - 1;
        }
        if (sampleEnd < sampleStart) continue;

        double weightedDifference = 0.0;
        double totalWeight = 0.0;
        for (int r = 0; r < rows; ++r) {
            int currentY = sampleStart + static_cast<int>((r + 0.5) * (sampleEnd - sampleStart + 1) / rows);
            int previousY = shift + currentY;
            if (currentY < 0 || currentY >= height || previousY < 0 || previousY >= height) continue;

            int minLuma = 255;
            int maxLuma = 0;
            for (int c = 0; c < cols; ++c) {
                int x = columnPositions[c];
                size_t offset = (static_cast<size_t>(currentY) * width + x) * 3;
                int luma = (current[offset] * 30 + current[offset + 1] * 59 + current[offset + 2] * 11) / 100;
                minLuma = std::min(minLuma, luma);
                maxLuma = std::max(maxLuma, luma);
            }
            double weight = 1.0 + std::min(7.0, static_cast<double>(maxLuma - minLuma) / 28.0);

            for (int c = 0; c < cols; ++c) {
                int x = columnPositions[c];
                size_t currentOffset = (static_cast<size_t>(currentY) * width + x) * 3;
                size_t previousOffset = (static_cast<size_t>(previousY) * width + x) * 3;
                int difference =
                    std::abs(static_cast<int>(current[currentOffset]) - static_cast<int>(previous[previousOffset])) +
                    std::abs(static_cast<int>(current[currentOffset + 1]) - static_cast<int>(previous[previousOffset + 1])) +
                    std::abs(static_cast<int>(current[currentOffset + 2]) - static_cast<int>(previous[previousOffset + 2]));
                weightedDifference += weight * difference;
                totalWeight += weight * 3.0;
            }
        }

        if (totalWeight <= 0.0) continue;
        double score = weightedDifference / totalWeight;
        score += std::abs(overlap - initial) * 0.00001;
        if (score < bestScore) {
            bestScore = score;
            bestOverlap = overlap;
        }
    }

    return bestOverlap;
}

} // namespace

bool stitchJpegStream(
    const std::vector<std::string>& frames,
    const std::vector<int>& overlaps,
    int overlapSafety,
    const std::string& outputPath,
    int quality,
    std::string& error
) {
    if (frames.empty()) {
        error = "no frames";
        return false;
    }
    if (overlaps.size() != frames.size()) {
        error = "overlaps size mismatch";
        return false;
    }

    std::vector<int> widths(frames.size(), 0);
    std::vector<int> heights(frames.size(), 0);
    std::vector<int> refinedOverlaps = overlaps;
    int outputWidth = 0;

    for (size_t i = 0; i < frames.size(); ++i) {
        int w = 0, h = 0;
        if (!readJpegSize(frames[i], w, h, error)) return false;
        if (w <= 0 || h <= 0) {
            error = "invalid frame size";
            return false;
        }
        if (i == 0) outputWidth = w;
        if (w != outputWidth) {
            error = "frame width mismatch";
            return false;
        }
        widths[i] = w;
        heights[i] = h;
    }

    std::vector<unsigned char> previousPixels;
    std::vector<unsigned char> currentPixels;
    for (size_t i = 0; i < frames.size(); ++i) {
        int decodedWidth = 0;
        int decodedHeight = 0;
        if (!decodeJpegRgb(frames[i], outputWidth, currentPixels, decodedWidth, decodedHeight, error)) {
            return false;
        }
        if (i == 0) {
            previousPixels.swap(currentPixels);
            continue;
        }

        int initial = std::max(1, std::min(decodedHeight - 1, overlaps[i]));
        refinedOverlaps[i] = refineOverlap(
            previousPixels,
            currentPixels,
            decodedWidth,
            decodedHeight,
            initial,
            64
        );
        previousPixels.swap(currentPixels);
    }

    std::vector<int> starts(frames.size(), 0);
    long totalHeight = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        starts[i] = (i == 0) ? 0 : std::max(0, refinedOverlaps[i] - overlapSafety);
        if (starts[i] >= heights[i]) {
            error = "start row out of range";
            return false;
        }
        totalHeight += static_cast<long>(heights[i] - starts[i]);
    }

    if (outputWidth <= 0 || totalHeight <= 0 || totalHeight > 200000L) {
        error = "invalid output size";
        return false;
    }

    FILE* outFile = std::fopen(outputPath.c_str(), "wb");
    if (!outFile) {
        error = "cannot create output: " + outputPath;
        return false;
    }

    jpeg_compress_struct out;
    ErrorManager jerr;
    out.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = onError;
    jerr.message[0] = '\0';

    unsigned char* row = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(outputWidth) * 3));
    if (!row) {
        std::fclose(outFile);
        error = "row buffer allocation failed";
        return false;
    }

    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&out);
        std::fclose(outFile);
        std::free(row);
        std::remove(outputPath.c_str());
        error = std::string("jpeg encode error: ") + jerr.message;
        return false;
    }

    jpeg_create_compress(&out);
    jpeg_stdio_dest(&out, outFile);
    out.image_width = outputWidth;
    out.image_height = static_cast<JDIMENSION>(totalHeight);
    out.input_components = 3;
    out.in_color_space = JCS_RGB;
    jpeg_set_defaults(&out);
    int q = std::max(1, std::min(100, quality));
    jpeg_set_quality(&out, q, TRUE);
    out.optimize_coding = TRUE;
    jpeg_start_compress(&out, TRUE);

    for (size_t i = 0; i < frames.size(); ++i) {
        std::vector<unsigned char> jpegBytes;
        if (!loadJpegBytes(frames[i], jpegBytes, error)) {
            jpeg_destroy_compress(&out);
            std::fclose(outFile);
            std::free(row);
            std::remove(outputPath.c_str());
            return false;
        }

        jpeg_decompress_struct in;
        ErrorManager inErr;
        in.err = jpeg_std_error(&inErr.pub);
        inErr.pub.error_exit = onError;
        inErr.message[0] = '\0';

        if (setjmp(inErr.jump)) {
            jpeg_destroy_decompress(&in);
            jpeg_destroy_compress(&out);
            std::fclose(outFile);
            std::free(row);
            std::remove(outputPath.c_str());
            error = std::string("jpeg decode error: ") + inErr.message + " " + frames[i];
            return false;
        }

        jpeg_create_decompress(&in);
        jpeg_mem_src(&in, jpegBytes.data(), static_cast<unsigned long>(jpegBytes.size()));
        jpeg_read_header(&in, TRUE);
        in.out_color_space = JCS_RGB;
        jpeg_start_decompress(&in);

        if (static_cast<int>(in.output_width) != outputWidth) {
            jpeg_destroy_decompress(&in);
            jpeg_destroy_compress(&out);
            std::fclose(outFile);
            std::free(row);
            std::remove(outputPath.c_str());
            error = "decode width mismatch";
            return false;
        }

        JSAMPROW rowPtr = row;
        for (int skip = 0; skip < starts[i]; ++skip) {
            if (jpeg_read_scanlines(&in, &rowPtr, 1) != 1) {
                jpeg_destroy_decompress(&in);
                jpeg_destroy_compress(&out);
                std::fclose(outFile);
                std::free(row);
                std::remove(outputPath.c_str());
                error = "failed reading skip rows";
                return false;
            }
        }

        while (in.output_scanline < in.output_height) {
            if (jpeg_read_scanlines(&in, &rowPtr, 1) != 1) break;
            jpeg_write_scanlines(&out, &rowPtr, 1);
        }

        jpeg_finish_decompress(&in);
        jpeg_destroy_decompress(&in);
    }

    jpeg_finish_compress(&out);
    jpeg_destroy_compress(&out);
    std::fclose(outFile);
    std::free(row);
    return true;
}