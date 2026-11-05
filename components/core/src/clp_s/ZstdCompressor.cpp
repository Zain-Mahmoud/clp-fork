// Code from CLP
//
// The class keeps its name so the rest of clp-s is unchanged, but it now writes DEFLATE instead of
// zstd, so that the BlueField-3 decompression engine can decompress it (BF-3 has no zstd engine and
// no compression engine, so compression is done in software with libdeflate).
//
// Stream format: a sequence of independently compressed blocks, each
//   [uint32 uncompressed_size][uint32 compressed_size][compressed_size bytes of raw DEFLATE]
// ended by a [0][0] header, so the decompressor can read ahead without running past the stream.
#include "ZstdCompressor.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <libdeflate.h>
#include <spdlog/spdlog.h>

namespace clp_s {
ZstdCompressor::ZstdCompressor()
        : Compressor{CompressorType::ZSTD},
          m_compressed_stream_file_writer{nullptr} {}

ZstdCompressor::~ZstdCompressor() = default;

void ZstdCompressor::open(FileWriter& file_writer, int const compression_level) {
    if (nullptr != m_compressed_stream_file_writer) {
        throw OperationFailed(ErrorCodeNotReady, __FILENAME__, __LINE__);
    }
    m_compression_level = std::clamp(compression_level, 1, 12);
    m_block.resize(cDeflateBlockSize);
    m_block_size = 0;
    m_compressed_stream_file_writer = &file_writer;
    m_uncompressed_stream_pos = 0;
}

void ZstdCompressor::close() {
    if (nullptr == m_compressed_stream_file_writer) {
        throw OperationFailed(ErrorCodeNotInit, __FILENAME__, __LINE__);
    }
    flush();
    uint32_t const end_of_stream[2] = {0, 0};
    m_compressed_stream_file_writer->write(
            reinterpret_cast<char const*>(end_of_stream),
            sizeof(end_of_stream)
    );
    m_compressed_stream_file_writer = nullptr;
}

void ZstdCompressor::write(char const* data, size_t data_length) {
    if (nullptr == m_compressed_stream_file_writer) {
        throw OperationFailed(ErrorCodeNotInit, __FILENAME__, __LINE__);
    }
    if (0 == data_length) {
        return;
    }
    if (nullptr == data) {
        throw OperationFailed(ErrorCodeBadParam, __FILENAME__, __LINE__);
    }

    m_uncompressed_stream_pos += data_length;
    while (data_length > 0) {
        size_t const n = std::min(data_length, cDeflateBlockSize - m_block_size);
        std::memcpy(m_block.data() + m_block_size, data, n);
        m_block_size += n;
        data += n;
        data_length -= n;
        if (cDeflateBlockSize == m_block_size) {
            compress_block();
        }
    }
}

void ZstdCompressor::flush() {
    if (m_block_size > 0) {
        compress_block();
    }
}

void ZstdCompressor::compress_block() {
    thread_local libdeflate_compressor* compressor = nullptr;
    thread_local int compressor_level = 0;
    if (compressor_level != m_compression_level) {
        libdeflate_free_compressor(compressor);
        compressor = libdeflate_alloc_compressor(m_compression_level);
        compressor_level = m_compression_level;
    }
    if (nullptr == compressor) {
        SPDLOG_ERROR("ZstdCompressor: libdeflate_alloc_compressor() failed");
        throw OperationFailed(ErrorCodeFailure, __FILENAME__, __LINE__);
    }

    m_compressed_block.resize(libdeflate_deflate_compress_bound(compressor, m_block_size));
    size_t const compressed_size = libdeflate_deflate_compress(
            compressor,
            m_block.data(),
            m_block_size,
            m_compressed_block.data(),
            m_compressed_block.size()
    );
    if (0 == compressed_size) {
        SPDLOG_ERROR("ZstdCompressor: libdeflate_deflate_compress() failed");
        throw OperationFailed(ErrorCodeFailure, __FILENAME__, __LINE__);
    }

    uint32_t const header[2]
            = {static_cast<uint32_t>(m_block_size), static_cast<uint32_t>(compressed_size)};
    m_compressed_stream_file_writer->write(reinterpret_cast<char const*>(header), sizeof(header));
    m_compressed_stream_file_writer->write(m_compressed_block.data(), compressed_size);
    m_block_size = 0;
}
}  // namespace clp_s
