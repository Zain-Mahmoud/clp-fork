// Code from CLP

#ifndef CLP_S_ZSTDCOMPRESSOR_HPP
#define CLP_S_ZSTDCOMPRESSOR_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "Compressor.hpp"
#include "FileWriter.hpp"
#include "TraceableException.hpp"

namespace clp_s {
constexpr int cDefaultCompressionLevel = 3;
// Uncompressed size of each DEFLATE block. Must not exceed the BF-3 engine's max buffer size
// (printed by dpKernels at startup as [BF3-DECOMP-CAP] max_buf_size).
constexpr size_t cDeflateBlockSize = 64 * 1024;

class ZstdCompressor : public Compressor {
public:
    // Types
    class OperationFailed : public TraceableException {
    public:
        // Constructors
        OperationFailed(ErrorCode error_code, char const* const filename, int line_number)
                : TraceableException(error_code, filename, line_number) {}
    };

    // Constructor
    ZstdCompressor();

    // Destructor
    ~ZstdCompressor() override;

    // Explicitly disable copy and move constructor/assignment
    ZstdCompressor(ZstdCompressor const&) = delete;

    ZstdCompressor& operator=(ZstdCompressor const&) = delete;

    // Methods implementing the WriterInterface
    /**
     * Writes the given data to the compressor
     * @param data
     * @param data_length
     */
    void write(char const* data, size_t data_length);

    /**
     * Writes the given numeric value to the compressor
     * @param val
     * @tparam ValueType
     */
    template <typename ValueType>
    void write_numeric_value(ValueType val) {
        write(reinterpret_cast<char*>(&val), sizeof(val));
    }

    /**
     * Writes the given string to the compressor
     * @param str
     */
    void write_string(std::string const& str) { write(str.c_str(), str.length()); }

    /**
     * Writes any internally buffered data to file and ends the current frame
     */
    void flush();

    // Methods implementing the Compressor interface
    /**
     * Closes the compressor
     */
    void close() override;

    /**
     * Initialize streaming compressor
     * @param file_writer
     * @param compression_level
     */
    void open(FileWriter& file_writer, int compression_level = cDefaultCompressionLevel);

private:
    // Variables
    FileWriter* m_compressed_stream_file_writer{};

    // DEFLATE blocks (see ZstdCompressor.cpp for the format)
    void compress_block();

    int m_compression_level{cDefaultCompressionLevel};
    std::vector<char> m_block;
    size_t m_block_size{0};
    std::vector<char> m_compressed_block;

    size_t m_uncompressed_stream_pos{};
};
}  // namespace clp_s

#endif  // CLP_S_ZSTDCOMPRESSOR_HPP
