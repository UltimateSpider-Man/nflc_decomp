#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <iomanip>
#include <algorithm>

extern "C" {
#include "minilzo.h"
}

// Constants
constexpr uint32_t BLOCK_SIZE = 32768;  // 32KB blocks
constexpr uint32_t HEADER_SIZE = 64;    // 64-byte header per block

// NFLC Block Header structure (64 bytes)
#pragma pack(push, 1)
struct NflcBlockHeader {
    char magic[4];              // 0x00: "nFlC"
    uint16_t version;           // 0x04: Version (usually 0x0101)
    uint16_t blockIndex;        // 0x06: Block index (0, 1, 2, ...)
    uint32_t flags;             // 0x08: Flags (0x80000012 for LZO)
    uint32_t flags2;            // 0x0C: Additional flags
    uint16_t dummy1;            // 0x10: Unknown
    uint16_t zsize;             // 0x12: Compressed size in this block
    uint32_t checksum1;         // 0x14: Checksum or identifier
    uint32_t blockUncompSize;   // 0x18: Uncompressed size of this block
    uint32_t checksum2;         // 0x1C: Another checksum
    uint32_t totalZSize;        // 0x20: Total compressed size (all blocks)
    uint32_t prevZOffset;       // 0x24: Cumulative compressed offset (previous blocks)
    uint32_t totalUncompSize;   // 0x28: Total uncompressed size (all blocks)
    uint32_t prevUncompOffset;  // 0x2C: Cumulative uncompressed offset (previous blocks)
    char padding[16];           // 0x30: Padding zeros
};
#pragma pack(pop)

void printUsage(const char* programName) {
    std::cerr << "NFLC Multi-Block Compress/Decompress Tool for Ultimate Spider-Man\n";
    std::cerr << "Usage:\n";
    std::cerr << "  Decompress: " << programName << " -d input.nflc output.bin\n";
    std::cerr << "  Compress:   " << programName << " -c input.bin output.nflc\n";
    std::cerr << "  Info:       " << programName << " -i input.nflc\n";
}

void printBlockHeader(const NflcBlockHeader& hdr, uint32_t blockNum) {
    std::cout << "--- Block " << blockNum << " ---\n";
    std::cout << "  Block Index: " << hdr.blockIndex << "\n";
    std::cout << "  Compressed size (zsize): " << hdr.zsize << " bytes\n";
    std::cout << "  Block uncompressed size: " << hdr.blockUncompSize << " bytes\n";
    std::cout << "  Prev Z offset: " << hdr.prevZOffset << "\n";
    std::cout << "  Prev uncomp offset: " << hdr.prevUncompOffset << "\n";
}

int showInfo(const std::string& inputFile) {
    std::ifstream ifs(inputFile, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Cannot open input file: " << inputFile << "\n";
        return 1;
    }

    // Get file size
    ifs.seekg(0, std::ios::end);
    std::streamoff fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::cout << "=== NFLC File Info ===\n";
    std::cout << "File: " << inputFile << "\n";
    std::cout << "File size: " << fileSize << " bytes\n";

    uint32_t numBlocks = (fileSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    std::cout << "Number of blocks: " << numBlocks << "\n\n";

    // Read first header for total sizes
    NflcBlockHeader firstHdr;
    ifs.read(reinterpret_cast<char*>(&firstHdr), sizeof(firstHdr));

    if (std::memcmp(firstHdr.magic, "nFlC", 4) != 0) {
        std::cerr << "Error: Not an NFLC file\n";
        return 1;
    }

    std::cout << "Total uncompressed size: " << firstHdr.totalUncompSize << " bytes\n";
    std::cout << "Total compressed size: " << firstHdr.totalZSize << " bytes\n\n";

    // Scan all blocks
    uint32_t totalBlockUncomp = 0;
    for (uint32_t i = 0; i < numBlocks; i++) {
        ifs.seekg(i * BLOCK_SIZE, std::ios::beg);

        NflcBlockHeader hdr;
        ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

        if (std::memcmp(hdr.magic, "nFlC", 4) != 0) {
            std::cout << "Block " << i << ": Invalid header (not nFlC)\n";
            continue;
        }

        printBlockHeader(hdr, i);
        totalBlockUncomp += hdr.blockUncompSize;
    }

    std::cout << "\nSum of block uncompressed sizes: " << totalBlockUncomp << " bytes\n";

    return 0;
}

int decompress(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream ifs(inputFile, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Cannot open input file: " << inputFile << "\n";
        return 1;
    }

    // Initialize LZO
    if (lzo_init() != LZO_E_OK) {
        std::cerr << "Error: LZO initialization failed\n";
        return 1;
    }

    // Get file size and calculate number of blocks
    ifs.seekg(0, std::ios::end);
    std::streamoff fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    uint32_t numBlocks = (fileSize + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Read first header to get total uncompressed size
    NflcBlockHeader firstHdr;
    ifs.read(reinterpret_cast<char*>(&firstHdr), sizeof(firstHdr));
    ifs.seekg(0, std::ios::beg);

    if (std::memcmp(firstHdr.magic, "nFlC", 4) != 0) {
        std::cerr << "Error: Not an NFLC file\n";
        return 1;
    }

    uint32_t totalUncompSize = firstHdr.totalUncompSize;
    std::cout << "Decompressing " << inputFile << "...\n";
    std::cout << "File size: " << fileSize << " bytes\n";
    std::cout << "Number of blocks: " << numBlocks << "\n";
    std::cout << "Expected uncompressed size: " << totalUncompSize << " bytes\n\n";

    // Allocate output buffer
    std::vector<unsigned char> outputData(totalUncompSize);
    uint32_t currentOutOffset = 0;

    // Process each block
    for (uint32_t blockNum = 0; blockNum < numBlocks; blockNum++) {
        std::streamoff blockOffset = blockNum * BLOCK_SIZE;
        ifs.seekg(blockOffset, std::ios::beg);

        // Read block header
        NflcBlockHeader hdr;
        ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

        if (std::memcmp(hdr.magic, "nFlC", 4) != 0) {
            std::cerr << "Warning: Block " << blockNum << " has invalid header, skipping\n";
            continue;
        }

        // Determine compressed data size for this block
        uint32_t compSize = hdr.zsize;
        uint32_t uncompSize = hdr.blockUncompSize;

        if (uncompSize == 0) {
            std::cout << "Block " << blockNum << ": Empty block, skipping\n";
            continue;
        }

        // Read compressed data
        std::vector<unsigned char> compData(compSize);
        ifs.read(reinterpret_cast<char*>(compData.data()), compSize);

        if (!ifs) {
            std::streamsize bytesRead = ifs.gcount();
            std::cerr << "Warning: Block " << blockNum << " - could only read "
                << bytesRead << " of " << compSize << " bytes\n";
            compSize = static_cast<uint32_t>(bytesRead);
        }

        // Decompress this block
        lzo_uint outLen = uncompSize;

        // Make sure we don't overflow output buffer
        if (currentOutOffset + uncompSize > outputData.size()) {
            std::cerr << "Error: Output buffer overflow at block " << blockNum << "\n";
            break;
        }

        int result = lzo1x_decompress_safe(
            compData.data(),
            compSize,
            outputData.data() + currentOutOffset,
            &outLen,
            nullptr
        );

        if (result != LZO_E_OK) {
            // Try unsafe version
            outLen = uncompSize;
            result = lzo1x_decompress(
                compData.data(),
                compSize,
                outputData.data() + currentOutOffset,
                &outLen,
                nullptr
            );

            if (result != LZO_E_OK) {
                std::cerr << "Error: Block " << blockNum << " decompression failed (code " << result << ")\n";
                return 1;
            }
        }

        std::cout << "Block " << blockNum << ": " << compSize << " -> " << outLen << " bytes\n";
        currentOutOffset += outLen;
    }

    std::cout << "\nTotal decompressed: " << currentOutOffset << " bytes\n";

    // Write output
    std::ofstream ofs(outputFile, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Error: Cannot create output file: " << outputFile << "\n";
        return 1;
    }

    ofs.write(reinterpret_cast<const char*>(outputData.data()), currentOutOffset);
    ofs.close();

    std::cout << "Successfully decompressed to: " << outputFile << "\n";
    return 0;
}

int compress(const std::string& inputFile, const std::string& outputFile) {
    std::ifstream ifs(inputFile, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Cannot open input file: " << inputFile << "\n";
        return 1;
    }

    // Initialize LZO
    if (lzo_init() != LZO_E_OK) {
        std::cerr << "Error: LZO initialization failed\n";
        return 1;
    }

    // Get input file size
    ifs.seekg(0, std::ios::end);
    std::streamoff inputSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::cout << "Compressing " << inputFile << " (" << inputSize << " bytes)...\n";

    // Read entire input
    std::vector<unsigned char> inputData(inputSize);
    ifs.read(reinterpret_cast<char*>(inputData.data()), inputSize);
    ifs.close();

    // Determine block sizes
    // Each block can hold up to (BLOCK_SIZE - HEADER_SIZE) bytes of compressed data
    constexpr uint32_t MAX_COMPRESSED_PER_BLOCK = BLOCK_SIZE - HEADER_SIZE;

    // We'll compress in chunks, targeting reasonable output chunks
    // A good heuristic: input chunks of ~40KB often compress to <32KB
    constexpr uint32_t TARGET_UNCOMP_CHUNK = 40960;  // 40KB chunks

    // Calculate number of chunks we'll need
    uint32_t numChunks = (inputSize + TARGET_UNCOMP_CHUNK - 1) / TARGET_UNCOMP_CHUNK;

    // Prepare output file
    std::ofstream ofs(outputFile, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Error: Cannot create output file: " << outputFile << "\n";
        return 1;
    }

    // Work memory for compression
    std::vector<unsigned char> workMem(LZO1X_1_MEM_COMPRESS);

    // Track cumulative offsets
    uint32_t totalZSize = 0;
    uint32_t prevZOffset = 0;
    uint32_t prevUncompOffset = 0;

    // First pass: compress all chunks and calculate total compressed size
    struct ChunkInfo {
        std::vector<unsigned char> compData;
        uint32_t uncompSize;
        uint32_t compSize;
    };
    std::vector<ChunkInfo> chunks;

    uint32_t offset = 0;
    while (offset < inputSize) {
        uint32_t chunkSize = std::min(static_cast<uint32_t>(TARGET_UNCOMP_CHUNK),
            static_cast<uint32_t>(inputSize - offset));

        // Compress this chunk
        size_t maxCompSize = chunkSize + chunkSize / 16 + 64 + 3;
        std::vector<unsigned char> compData(maxCompSize);

        lzo_uint compLen = maxCompSize;
        int result = lzo1x_1_compress(
            inputData.data() + offset,
            chunkSize,
            compData.data(),
            &compLen,
            workMem.data()
        );

        if (result != LZO_E_OK) {
            std::cerr << "Error: Compression failed at offset " << offset << "\n";
            return 1;
        }

        compData.resize(compLen);

        ChunkInfo ci;
        ci.compData = std::move(compData);
        ci.uncompSize = chunkSize;
        ci.compSize = static_cast<uint32_t>(compLen);

        totalZSize += ci.compSize;
        chunks.push_back(std::move(ci));

        offset += chunkSize;
    }

    std::cout << "Compressed into " << chunks.size() << " blocks\n";
    std::cout << "Total compressed size: " << totalZSize << " bytes\n";

    // Second pass: write blocks
    prevZOffset = 0;
    prevUncompOffset = 0;

    for (size_t i = 0; i < chunks.size(); i++) {
        const ChunkInfo& ci = chunks[i];

        // Prepare block header
        NflcBlockHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));

        std::memcpy(hdr.magic, "nFlC", 4);
        hdr.version = 0x0101;
        hdr.blockIndex = static_cast<uint16_t>(i);
        hdr.flags = 0x80000012;
        hdr.flags2 = 0x80000080;
        hdr.dummy1 = 0x0901;
        hdr.zsize = static_cast<uint16_t>(std::min(ci.compSize, (uint32_t)0xFFFF));
        hdr.checksum1 = 0xCB3E47E2;  // Placeholder
        hdr.blockUncompSize = ci.uncompSize;
        hdr.checksum2 = 0xA309C008;  // Placeholder
        hdr.totalZSize = totalZSize;
        hdr.prevZOffset = prevZOffset;
        hdr.totalUncompSize = static_cast<uint32_t>(inputSize);
        hdr.prevUncompOffset = prevUncompOffset;

        // Write header
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // Write compressed data
        ofs.write(reinterpret_cast<const char*>(ci.compData.data()), ci.compSize);

        // Pad to 32KB block boundary
        std::streamoff currentPos = ofs.tellp();
        std::streamoff blockEnd = ((currentPos + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;

        if (i < chunks.size() - 1) {  // Don't pad last block
            std::streamoff paddingSize = blockEnd - currentPos;
            std::vector<char> padding(paddingSize, 0);
            ofs.write(padding.data(), paddingSize);
        }

        std::cout << "Block " << i << ": " << ci.uncompSize << " -> " << ci.compSize << " bytes\n";

        prevZOffset += ci.compSize;
        prevUncompOffset += ci.uncompSize;
    }

    ofs.close();

    // Get final file size
    std::ifstream checkSize(outputFile, std::ios::binary | std::ios::ate);
    std::streamoff outputSize = checkSize.tellg();
    checkSize.close();

    std::cout << "\nSuccessfully compressed to: " << outputFile << "\n";
    std::cout << "Output file size: " << outputSize << " bytes\n";
    std::cout << "Compression ratio: " << std::fixed << std::setprecision(1)
        << (100.0 * outputSize / inputSize) << "%\n";

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "-d" || mode == "--decompress") {
        if (argc < 4) {
            std::cerr << "Error: Missing output file\n";
            printUsage(argv[0]);
            return 1;
        }
        return decompress(argv[2], argv[3]);
    }
    else if (mode == "-c" || mode == "--compress") {
        if (argc < 4) {
            std::cerr << "Error: Missing output file\n";
            printUsage(argv[0]);
            return 1;
        }
        return compress(argv[2], argv[3]);
    }
    else if (mode == "-i" || mode == "--info") {
        return showInfo(argv[2]);
    }
    else {
        std::cerr << "Error: Unknown mode '" << mode << "'\n";
        printUsage(argv[0]);
        return 1;
    }
}
