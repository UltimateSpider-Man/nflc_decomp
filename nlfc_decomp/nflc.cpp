#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>

// Include minilzo (adjust the path as needed)
extern "C" {
#include "minilzo.h"

#include "lzo1x.h"
}

// Helper function to read a “primitive” (int, short, etc.) from a stream
template<typename T>
bool readValue(std::istream& is, T& outValue) {
    is.read(reinterpret_cast<char*>(&outValue), sizeof(T));
    return static_cast<bool>(is);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " input.nFlC [output.bin]\n";
        return 1;
    }

    std::string inFilename = argv[1];
    std::string outFilename = (argc >= 3) ? argv[2] : "output.bin";

    std::ifstream ifs(inFilename, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open input file: " << inFilename << std::endl;
        return 1;
    }

    //--------------------------------------------------------------------------
    // 1) Validate the magic string "nFlC"
    //--------------------------------------------------------------------------
    char magic[4];
    ifs.read(magic, 4);
    if (std::memcmp(magic, "nFlC", 4) != 0) {
        std::cerr << "Error: this file is not an nFlC archive.\n";
        return 1;
    }

    //--------------------------------------------------------------------------
    // 2) Read fields in the same order as the QuickBMS script
    //--------------------------------------------------------------------------
    // get DUMMY long  # 0x80000001 or 0x8000000c
    uint32_t dummy1;
    readValue(ifs, dummy1);

    // get FLAGS long
    uint32_t flags;
    readValue(ifs, flags);

    // get ZERO long
    uint32_t zero1;
    readValue(ifs, zero1);

    // get DUMMY short
    uint16_t dummy2;
    readValue(ifs, dummy2);

    // get ZSIZE short
    uint16_t zsize;
    readValue(ifs, zsize);

    // get DUMMY long
    uint32_t dummy3;
    readValue(ifs, dummy3);

    // get SIZE long
    uint32_t sizeUncompressed;
    readValue(ifs, sizeUncompressed);

    // get DUMMY long
    uint32_t dummy4;
    readValue(ifs, dummy4);

    // get FULL_ZSIZE long
    uint32_t fullZSize;
    readValue(ifs, fullZSize);

    // get ZERO long
    uint32_t zero2;
    readValue(ifs, zero2);

    // get FULL_SIZE long  # no idea, all samples are 32kb
    uint32_t fullSize;
    readValue(ifs, fullSize);

    // get ZERO long
    uint32_t zero3;
    readValue(ifs, zero3);

    // getdstring ZERO 16
    char zeroStr[16];
    ifs.read(zeroStr, 16);

    //--------------------------------------------------------------------------
    // 3) Save current offset (like “savepos OFFSET” in QuickBMS)
    //--------------------------------------------------------------------------
    std::streamoff dataOffset = ifs.tellg();

    // We have all the fields; let's print them for debugging:
    std::cout << "Magic: nFlC\n";
    std::cout << "dummy1: 0x" << std::hex << dummy1 << std::dec << "\n";
    std::cout << "flags: 0x" << std::hex << flags << std::dec << "\n";
    std::cout << "zero1: " << zero1 << "\n";
    std::cout << "dummy2 (short): " << dummy2 << "\n";
    std::cout << "zsize (short): " << zsize << "\n";
    std::cout << "dummy3: 0x" << std::hex << dummy3 << std::dec << "\n";
    std::cout << "sizeUncompressed: " << sizeUncompressed << "\n";
    std::cout << "dummy4: 0x" << std::hex << dummy4 << std::dec << "\n";
    std::cout << "fullZSize: " << fullZSize << "\n";
    std::cout << "zero2: " << zero2 << "\n";
    std::cout << "fullSize: " << fullSize << "\n";
    std::cout << "zero3: " << zero3 << "\n";
    // zeroStr (16 bytes) is presumably all zero or unknown

    //--------------------------------------------------------------------------
    // 4) Read the compressed data from the file
    //    QuickBMS: clog NAME OFFSET ZSIZE SIZE
    //    => We'll read zsize bytes for the compressed block,
    //       then decompress it to sizeUncompressed
    //--------------------------------------------------------------------------
    ifs.seekg(dataOffset, std::ios::beg);
    if (!ifs.good()) {
        std::cerr << "Error: seek failed at offset: " << dataOffset << std::endl;
        return 1;
    }

    // In many nFlC files, "zsize" might be short but "fullZSize" is the real compressed size
    // QuickBMS uses the short "zsize" but your script also reads "FULL_ZSIZE" after that.
    // It's possible that the actual compressed block length is in "fullZSize."
    // We'll trust "fullZSize" for actual reading if it’s bigger than zsize.
    const uint32_t compressedSize = (fullZSize > zsize) ? fullZSize : zsize;

    std::vector<unsigned char> compressedData(compressedSize);
    ifs.read(reinterpret_cast<char*>(compressedData.data()), compressedSize);
    if (!ifs.good()) {
        std::cerr << "Error: could not read compressed data.\n";
        return 1;
    }

    //--------------------------------------------------------------------------
    // 5) Decompress via miniLZO
    //--------------------------------------------------------------------------
    // Initialize minilzo
    if (lzo_init() != LZO_E_OK) {
        std::cerr << "Error: minilzo init failed\n";
        return 1;
    }

    // Allocate a destination buffer. We use the “sizeUncompressed” from the script.
    // The script also sees "fullSize" at 32KB for all samples, but we’ll trust sizeUncompressed.
    std::vector<unsigned char> decompressedData(sizeUncompressed);

    // LZO needs a small work-memory for decompression (per minilzo docs).
    static unsigned char lzoWorkMem[LZO1X_999_MEM_DECOMPRESS];

    lzo_uint in_len = static_cast<lzo_uint>(compressedSize);
    lzo_uint out_len = static_cast<lzo_uint>(sizeUncompressed);

    int lzoError = lzo1x_decompress(
        compressedData.data(),
        in_len,
        decompressedData.data(),
        &out_len,
        lzoWorkMem
    );

    if (lzoError != LZO_E_OK) {
        std::cerr << "Error: LZO decompress returned " << lzoError << "\n";
        return 1;
    }

    if (out_len != sizeUncompressed) {
        std::cerr << "Warning: Decompressed size (" << out_len
            << ") != expected size (" << sizeUncompressed << ")\n";
        // Some nFlC may have padding or slight differences; handle as needed
    }

    //--------------------------------------------------------------------------
    // 6) Write out the decompressed data to a file
    //--------------------------------------------------------------------------
    std::ofstream ofs(outFilename, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Error: cannot create output file: " << outFilename << std::endl;
        return 1;
    }
    ofs.write(reinterpret_cast<const char*>(decompressedData.data()), out_len);
    ofs.close();

    std::cout << "Successfully decompressed to: " << outFilename
        << " (" << out_len << " bytes)\n";

    return 0;
}


