/*
 * nFlC Multi-Chunk LZO1X Decompressor
 *
 * Decompresses files using the nFlC container format with LZO1X compression.
 * Supports multi-chunk files commonly found in PS2/Xbox game archives.
 *
 * Build:
 *   g++ -O2 -o nflc_decompress nflc_decompress.cpp -std=c++17
 *
 * Usage:
 *   ./nflc_decompress input.ps2pack output.bin
 *   ./nflc_decompress -a input.ps2pack          # Analyze only
 *   ./nflc_decompress -r input.ps2pack out.bin  # Raw extraction
 *
 * Author: Claude (Anthropic)
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <algorithm>

 // ============================================================================
 // Constants and Types
 // ============================================================================

constexpr uint32_t NFLC_MAGIC = 0x436C466E;  // "nFlC" as little-endian
constexpr size_t CHUNK_SIZE = 0x8000;         // 32KB chunks
constexpr size_t MAIN_HEADER_SIZE = 64;
constexpr size_t CHUNK_HEADER_SIZE = 16;

#pragma pack(push, 1)
struct NFlCMainHeader {
    uint32_t magic;           // 0x00: "nFlC"
    uint32_t version;         // 0x04: Version/flags
    uint32_t flags1;          // 0x08: Chunk count + compression flag (high bit)
    uint32_t flags2;          // 0x0C: Additional flags
    uint32_t hash1;           // 0x10: Hash/checksum
    uint32_t hash2;           // 0x14: Hash/checksum
    uint32_t compressedSize;  // 0x18: Total compressed data size
    uint32_t extra1;          // 0x1C: Additional info
    uint32_t extra2;          // 0x20: Additional info
    uint32_t padding1;        // 0x24: Padding
    uint32_t decompressedSize;// 0x28: Total decompressed size
    uint32_t padding2;        // 0x2C: Padding
    uint8_t  reserved[16];    // 0x30-0x3F: Reserved
};

struct NFlCChunkHeader {
    uint32_t magic;           // 0x00: "nFlC"
    uint32_t version;         // 0x04: Version (contains chunk index)
    uint32_t flags1;          // 0x08: Flags
    uint32_t flags2;          // 0x0C: Flags
};
#pragma pack(pop)

struct ChunkInfo {
    size_t offset;
    size_t dataOffset;
    size_t dataSize;
    uint32_t chunkIndex;
    uint32_t version;
};

// ============================================================================
// LZO1X Decompression Implementation
// ============================================================================

class LZO1XDecompressor {
public:
    enum class Result {
        OK = 0,
        INPUT_OVERRUN,
        OUTPUT_OVERRUN,
        LOOKBEHIND_OVERRUN,
        INVALID_DATA,
        ERROR
    };

    static Result decompress(const uint8_t* src, size_t srcLen,
        uint8_t* dst, size_t* dstLen,
        size_t maxDstLen) {
        if (!src || !dst || !dstLen || srcLen == 0) {
            return Result::INVALID_DATA;
        }

        const uint8_t* ip = src;
        const uint8_t* const ipEnd = src + srcLen;
        uint8_t* op = dst;
        uint8_t* const opEnd = dst + maxDstLen;

        *dstLen = 0;

        // First byte handling
        if (ip >= ipEnd) {
            return Result::INPUT_OVERRUN;
        }

        uint32_t t = *ip++;

        if (t > 17) {
            t -= 17;
            if (t < 4) {
                goto match_next;
            }
            if (op + t > opEnd) return Result::OUTPUT_OVERRUN;
            if (ip + t > ipEnd) return Result::INPUT_OVERRUN;
            do {
                *op++ = *ip++;
            } while (--t > 0);
            goto first_literal_run;
        }

        while (true) {
            // Literal run
            if (t < 16) {
                if (t == 0) {
                    while (*ip == 0) {
                        t += 255;
                        ip++;
                        if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                    }
                    t += 15 + *ip++;
                }
                t += 3;
                if (op + t > opEnd) return Result::OUTPUT_OVERRUN;
                if (ip + t > ipEnd) return Result::INPUT_OVERRUN;

                // Copy literals
                do {
                    *op++ = *ip++;
                } while (--t > 0);

            first_literal_run:
                if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                t = *ip++;
                if (t < 16) {
                    // M1 match
                    size_t mPos = (op - dst) - 1 - 0x800;
                    mPos -= t >> 2;
                    if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                    mPos -= (*ip++) << 2;

                    if (mPos >= (size_t)(op - dst)) {
                        return Result::LOOKBEHIND_OVERRUN;
                    }

                    if (op + 3 > opEnd) return Result::OUTPUT_OVERRUN;
                    *op++ = dst[mPos];
                    *op++ = dst[mPos + 1];
                    *op++ = dst[mPos + 2];

                    goto match_done;
                }
            }

            // Match handling
            while (true) {
                size_t mPos;
                size_t mLen;

                if (t >= 64) {
                    // M2 match
                    mPos = (op - dst) - 1;
                    mPos -= (t >> 2) & 7;
                    if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                    mPos -= (*ip++) << 3;
                    mLen = (t >> 5) - 1 + 3;
                }
                else if (t >= 32) {
                    // M3 match
                    mLen = t & 31;
                    if (mLen == 0) {
                        while (*ip == 0) {
                            mLen += 255;
                            ip++;
                            if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                        }
                        mLen += 31 + *ip++;
                    }
                    mLen += 2;
                    if (ip + 2 > ipEnd) return Result::INPUT_OVERRUN;
                    mPos = (op - dst) - 1;
                    mPos -= (ip[0] >> 2) + (ip[1] << 6);
                    ip += 2;
                }
                else if (t >= 16) {
                    // M4 match
                    mPos = (op - dst);
                    mPos -= (t & 8) << 11;
                    mLen = t & 7;
                    if (mLen == 0) {
                        while (*ip == 0) {
                            mLen += 255;
                            ip++;
                            if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                        }
                        mLen += 7 + *ip++;
                    }
                    mLen += 2;
                    if (ip + 2 > ipEnd) return Result::INPUT_OVERRUN;
                    mPos -= (ip[0] >> 2) + (ip[1] << 6);
                    ip += 2;
                    if (mPos == (size_t)(op - dst)) {
                        // End of stream marker
                        *dstLen = op - dst;
                        return Result::OK;
                    }
                    mPos -= 0x4000;
                }
                else {
                    // M1 match (from literal)
                    mPos = (op - dst) - 1;
                    mPos -= t >> 2;
                    if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                    mPos -= (*ip++) << 2;

                    if (mPos >= (size_t)(op - dst)) {
                        return Result::LOOKBEHIND_OVERRUN;
                    }

                    if (op + 2 > opEnd) return Result::OUTPUT_OVERRUN;
                    *op++ = dst[mPos];
                    *op++ = dst[mPos + 1];

                    goto match_done;
                }

                // Validate match position
                if (mPos >= (size_t)(op - dst)) {
                    return Result::LOOKBEHIND_OVERRUN;
                }

                // Copy match
                if (op + mLen > opEnd) return Result::OUTPUT_OVERRUN;

                {
                    const uint8_t* mSrc = dst + mPos;
                    do {
                        *op++ = *mSrc++;
                    } while (--mLen > 0);
                }

            match_done:
                t = ip[-2] & 3;
                if (t == 0) break;

            match_next:
                // Copy trailing literals
                if (op + t > opEnd) return Result::OUTPUT_OVERRUN;
                if (ip + t > ipEnd) return Result::INPUT_OVERRUN;
                *op++ = *ip++;
                if (t > 1) {
                    *op++ = *ip++;
                    if (t > 2) {
                        *op++ = *ip++;
                    }
                }

                if (ip >= ipEnd) return Result::INPUT_OVERRUN;
                t = *ip++;
            }
        }

        *dstLen = op - dst;
        return Result::OK;
    }

    static const char* resultString(Result r) {
        switch (r) {
        case Result::OK: return "OK";
        case Result::INPUT_OVERRUN: return "Input overrun";
        case Result::OUTPUT_OVERRUN: return "Output overrun";
        case Result::LOOKBEHIND_OVERRUN: return "Lookbehind overrun";
        case Result::INVALID_DATA: return "Invalid data";
        case Result::ERROR: return "Error";
        default: return "Unknown";
        }
    }
};

// ============================================================================
// nFlC File Handler
// ============================================================================

class NFlCFile {
public:
    bool load(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "Error: Cannot open file: " << path << std::endl;
            return false;
        }

        fileSize_ = file.tellg();
        file.seekg(0);

        data_.resize(fileSize_);
        file.read(reinterpret_cast<char*>(data_.data()), fileSize_);

        if (!file) {
            std::cerr << "Error: Cannot read file" << std::endl;
            return false;
        }

        return parseChunks();
    }

    bool parseChunks() {
        chunks_.clear();

        if (data_.size() < MAIN_HEADER_SIZE) {
            std::cerr << "Error: File too small" << std::endl;
            return false;
        }

        // Find all chunk headers
        for (size_t offset = 0; offset < data_.size(); offset += CHUNK_SIZE) {
            if (offset + sizeof(NFlCChunkHeader) > data_.size()) break;

            const auto* hdr = reinterpret_cast<const NFlCChunkHeader*>(&data_[offset]);
            if (hdr->magic != NFLC_MAGIC) {
                if (offset == 0) {
                    std::cerr << "Error: Invalid nFlC magic at start" << std::endl;
                    return false;
                }
                continue;
            }

            ChunkInfo info;
            info.offset = offset;
            info.version = hdr->version;
            info.chunkIndex = (hdr->version >> 8) & 0xFFFF;

            // Determine data offset based on chunk position
            if (offset == 0) {
                info.dataOffset = MAIN_HEADER_SIZE;
            }
            else {
                info.dataOffset = offset + CHUNK_HEADER_SIZE;
            }

            chunks_.push_back(info);
        }

        // Calculate data sizes
        for (size_t i = 0; i < chunks_.size(); i++) {
            size_t nextOffset = (i + 1 < chunks_.size())
                ? chunks_[i + 1].offset
                : data_.size();
            chunks_[i].dataSize = nextOffset - chunks_[i].dataOffset;
        }

        // Parse main header
        if (!chunks_.empty()) {
            const auto* main = reinterpret_cast<const NFlCMainHeader*>(data_.data());
            mainCompressedSize_ = main->compressedSize;
            mainDecompressedSize_ = main->decompressedSize;
            mainFlags1_ = main->flags1;
            mainFlags2_ = main->flags2;
        }

        return !chunks_.empty();
    }

    void analyze() const {
        std::cout << "=== nFlC File Analysis ===" << std::endl;
        std::cout << "File size:        " << fileSize_ << " bytes ("
            << (fileSize_ / 1024.0) << " KB)" << std::endl;
        std::cout << "Chunk count:      " << chunks_.size() << std::endl;
        std::cout << std::endl;

        if (!chunks_.empty()) {
            std::cout << "Main Header:" << std::endl;
            std::cout << "  Flags1:           0x" << std::hex << std::setfill('0')
                << std::setw(8) << mainFlags1_ << std::dec << std::endl;
            std::cout << "  Flags2:           0x" << std::hex << std::setfill('0')
                << std::setw(8) << mainFlags2_ << std::dec << std::endl;
            std::cout << "  Compressed size:  " << mainCompressedSize_ << " bytes" << std::endl;
            std::cout << "  Decompressed size:" << mainDecompressedSize_ << " bytes" << std::endl;

            if (mainCompressedSize_ > 0 && mainDecompressedSize_ > 0) {
                double ratio = (double)mainCompressedSize_ / mainDecompressedSize_ * 100.0;
                std::cout << "  Compression ratio:" << std::fixed << std::setprecision(1)
                    << ratio << "%" << std::endl;
            }
            std::cout << std::endl;

            std::cout << "Chunks:" << std::endl;
            std::cout << std::setw(4) << "#" << " "
                << std::setw(10) << "Offset" << " "
                << std::setw(10) << "DataOff" << " "
                << std::setw(10) << "DataSize" << " "
                << std::setw(6) << "Index" << " "
                << std::setw(10) << "Version" << std::endl;
            std::cout << std::string(56, '-') << std::endl;

            size_t maxShow = std::min(chunks_.size(), (size_t)30);
            for (size_t i = 0; i < maxShow; i++) {
                const auto& c = chunks_[i];
                std::cout << std::setw(4) << i << " "
                    << "0x" << std::hex << std::setfill('0') << std::setw(8) << c.offset << " "
                    << "0x" << std::setw(8) << c.dataOffset << " "
                    << std::dec << std::setfill(' ') << std::setw(10) << c.dataSize << " "
                    << std::setw(6) << c.chunkIndex << " "
                    << "0x" << std::hex << std::setfill('0') << std::setw(8) << c.version
                    << std::dec << std::endl;
            }

            if (chunks_.size() > 30) {
                std::cout << "... (" << (chunks_.size() - 30) << " more chunks)" << std::endl;
            }
        }

        // Hex dump
        std::cout << std::endl << "Hex dump (first 128 bytes):" << std::endl;
        hexDump(0, 128);

        if (chunks_.size() > 0 && chunks_[0].dataOffset < data_.size()) {
            std::cout << std::endl << "Hex dump at data offset 0x"
                << std::hex << chunks_[0].dataOffset << std::dec << ":" << std::endl;
            hexDump(chunks_[0].dataOffset, 64);
        }
    }

    void hexDump(size_t offset, size_t length) const {
        size_t end = std::min(offset + length, data_.size());
        for (size_t i = offset; i < end; i += 16) {
            std::cout << "  " << std::hex << std::setfill('0') << std::setw(4) << i << ": ";

            // Hex bytes
            for (size_t j = 0; j < 16 && i + j < end; j++) {
                std::cout << std::setw(2) << (int)data_[i + j] << " ";
            }

            // Padding if needed
            for (size_t j = end - i; j < 16; j++) {
                std::cout << "   ";
            }

            // ASCII
            std::cout << " ";
            for (size_t j = 0; j < 16 && i + j < end; j++) {
                char c = data_[i + j];
                std::cout << (c >= 32 && c < 127 ? c : '.');
            }
            std::cout << std::dec << std::endl;
        }
    }

    std::vector<uint8_t> extractRaw() const {
        std::vector<uint8_t> result;
        result.reserve(data_.size());

        for (const auto& chunk : chunks_) {
            if (chunk.dataOffset + chunk.dataSize <= data_.size()) {
                result.insert(result.end(),
                    data_.begin() + chunk.dataOffset,
                    data_.begin() + chunk.dataOffset + chunk.dataSize);
            }
        }

        return result;
    }

    std::vector<uint8_t> decompressSingle() const {
        if (chunks_.empty() || mainDecompressedSize_ == 0) {
            std::cerr << "Error: No valid header or decompressed size" << std::endl;
            return {};
        }

        size_t dataStart = chunks_[0].dataOffset;
        size_t compSize = std::min((size_t)mainCompressedSize_, data_.size() - dataStart);

        std::vector<uint8_t> result(mainDecompressedSize_);
        size_t resultLen = 0;

        auto status = LZO1XDecompressor::decompress(
            &data_[dataStart], compSize,
            result.data(), &resultLen, result.size()
        );

        if (status != LZO1XDecompressor::Result::OK) {
            std::cerr << "Decompression failed: "
                << LZO1XDecompressor::resultString(status) << std::endl;
            return {};
        }

        result.resize(resultLen);
        return result;
    }

    std::vector<uint8_t> decompressChunked(size_t chunkDecompSize = CHUNK_SIZE) const {
        std::vector<uint8_t> result;
        result.reserve(mainDecompressedSize_ > 0 ? mainDecompressedSize_ : chunks_.size() * chunkDecompSize);

        std::vector<uint8_t> chunkBuffer(chunkDecompSize * 2);

        for (size_t i = 0; i < chunks_.size(); i++) {
            const auto& chunk = chunks_[i];

            if (chunk.dataOffset + chunk.dataSize > data_.size()) {
                std::cerr << "Warning: Chunk " << i << " extends beyond file" << std::endl;
                continue;
            }

            const uint8_t* chunkData = &data_[chunk.dataOffset];
            size_t chunkLen = chunk.dataSize;

            // Try LZO decompression
            size_t decompLen = 0;
            auto status = LZO1XDecompressor::decompress(
                chunkData, chunkLen,
                chunkBuffer.data(), &decompLen, chunkBuffer.size()
            );

            if (status == LZO1XDecompressor::Result::OK && decompLen > 0) {
                result.insert(result.end(), chunkBuffer.begin(), chunkBuffer.begin() + decompLen);
            }
            else {
                // Use raw data if decompression fails
                result.insert(result.end(), chunkData, chunkData + chunkLen);
            }
        }

        return result;
    }

    std::vector<uint8_t> decompress(bool verbose = false) const {
        std::vector<uint8_t> result;

        // Try single-block decompression first
        if (verbose) std::cout << "Trying single-block decompression..." << std::endl;
        result = decompressSingle();

        if (!result.empty()) {
            if (verbose) std::cout << "Single-block: " << result.size() << " bytes" << std::endl;
            return result;
        }

        // Try chunked decompression
        if (verbose) std::cout << "Trying chunked decompression..." << std::endl;
        result = decompressChunked();

        if (!result.empty()) {
            if (verbose) std::cout << "Chunked: " << result.size() << " bytes" << std::endl;
            return result;
        }

        // Fall back to raw extraction
        if (verbose) std::cout << "Falling back to raw extraction..." << std::endl;
        return extractRaw();
    }

    size_t getFileSize() const { return fileSize_; }
    size_t getChunkCount() const { return chunks_.size(); }
    uint32_t getDecompressedSize() const { return mainDecompressedSize_; }
    uint32_t getCompressedSize() const { return mainCompressedSize_; }

private:
    std::vector<uint8_t> data_;
    std::vector<ChunkInfo> chunks_;
    size_t fileSize_ = 0;
    uint32_t mainCompressedSize_ = 0;
    uint32_t mainDecompressedSize_ = 0;
    uint32_t mainFlags1_ = 0;
    uint32_t mainFlags2_ = 0;
};

// ============================================================================
// Main
// ============================================================================

void printUsage(const char* prog) {
    std::cout << "nFlC Multi-Chunk LZO1X Decompressor" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " [options] <input> [output]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -a, --analyze    Analyze file structure only" << std::endl;
    std::cout << "  -r, --raw        Extract raw data (no decompression)" << std::endl;
    std::cout << "  -s, --single     Force single-block decompression" << std::endl;
    std::cout << "  -c, --chunked    Force chunked decompression" << std::endl;
    std::cout << "  -v, --verbose    Verbose output" << std::endl;
    std::cout << "  -h, --help       Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog << " input.ps2pack output.bin" << std::endl;
    std::cout << "  " << prog << " -a input.ps2pack" << std::endl;
    std::cout << "  " << prog << " -r input.ps2pack raw.bin" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    bool analyzeOnly = false;
    bool rawExtract = false;
    bool forceSingle = false;
    bool forceChunked = false;
    bool verbose = false;
    std::string inputPath;
    std::string outputPath;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-a" || arg == "--analyze") {
            analyzeOnly = true;
        }
        else if (arg == "-r" || arg == "--raw") {
            rawExtract = true;
        }
        else if (arg == "-s" || arg == "--single") {
            forceSingle = true;
        }
        else if (arg == "-c" || arg == "--chunked") {
            forceChunked = true;
        }
        else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
        else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg[0] != '-') {
            if (inputPath.empty()) {
                inputPath = arg;
            }
            else if (outputPath.empty()) {
                outputPath = arg;
            }
        }
    }

    if (inputPath.empty()) {
        std::cerr << "Error: No input file specified" << std::endl;
        return 1;
    }

    // Load file
    NFlCFile nflc;
    if (!nflc.load(inputPath)) {
        return 1;
    }

    // Analyze mode
    if (analyzeOnly) {
        nflc.analyze();
        return 0;
    }

    // Need output path for extraction
    if (outputPath.empty()) {
        // Generate default output name
        size_t dot = inputPath.rfind('.');
        if (dot != std::string::npos) {
            outputPath = inputPath.substr(0, dot) + ".bin";
        }
        else {
            outputPath = inputPath + ".bin";
        }
    }

    // Decompress/extract
    std::vector<uint8_t> result;

    if (rawExtract) {
        if (verbose) std::cout << "Extracting raw data..." << std::endl;
        result = nflc.extractRaw();
    }
    else if (forceSingle) {
        if (verbose) std::cout << "Single-block decompression..." << std::endl;
        result = nflc.decompressSingle();
    }
    else if (forceChunked) {
        if (verbose) std::cout << "Chunked decompression..." << std::endl;
        result = nflc.decompressChunked();
    }
    else {
        result = nflc.decompress(verbose);
    }

    if (result.empty()) {
        std::cerr << "Error: No data to write" << std::endl;
        return 1;
    }

    // Write output
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Cannot create output file: " << outputPath << std::endl;
        return 1;
    }

    outFile.write(reinterpret_cast<const char*>(result.data()), result.size());
    outFile.close();

    std::cout << "Output: " << outputPath << " (" << result.size() << " bytes)" << std::endl;

    return 0;
}