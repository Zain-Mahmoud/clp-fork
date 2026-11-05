// Code from CLP
//
// Reads the block-DEFLATE format written by ZstdCompressor (see ZstdCompressor.cpp) and
// decompresses each block on the BlueField-3 decompression engine through dpKernels. Up to
// cReadAhead blocks are submitted at once, so the engine works on the next blocks while the caller
// reads the current one.
#include "ZstdDecompressor.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include "ZstdCompressor.hpp"


namespace clp_s {
ZstdDecompressor::ZstdDecompressor()
        : Decompressor(CompressorType::ZSTD),
          m_input_type(InputType::NotInitialized),
          m_file_reader(nullptr),
          m_reader(nullptr),
          m_file_reader_initial_pos(0),
          m_decompressed_stream_pos(0) {}

ZstdDecompressor::~ZstdDecompressor() {
    discard_blocks();
}

ErrorCode
ZstdDecompressor::read_compressed(char* dst, size_t num_bytes, size_t& num_bytes_read) {
    num_bytes_read = 0;
    switch (m_input_type) {
        case InputType::CompressedDataBuf:
        case InputType::MemoryMappedCompressedFile:
            num_bytes_read = std::min(num_bytes, m_compressed_buf_size - m_compressed_buf_pos);
            std::memcpy(dst, m_compressed_buf + m_compressed_buf_pos, num_bytes_read);
            m_compressed_buf_pos += num_bytes_read;
            break;
        case InputType::File:
            while (num_bytes_read < num_bytes) {
                size_t n = 0;
                auto const rc = m_file_reader->try_read(
                        dst + num_bytes_read,
                        num_bytes - num_bytes_read,
                        n
                );
                if (ErrorCodeEndOfFile == rc) {
                    break;
                }
                if (ErrorCodeSuccess != rc) {
                    return rc;
                }
                num_bytes_read += n;
            }
            break;
        case InputType::ClpReader:
            while (num_bytes_read < num_bytes) {
                size_t n = 0;
                auto const rc
                        = m_reader->try_read(dst + num_bytes_read, num_bytes - num_bytes_read, n);
                if (clp::ErrorCode::ErrorCode_EndOfFile == rc) {
                    break;
                }
                if (clp::ErrorCode::ErrorCode_Success != rc) {
                    return ErrorCodeFailure;
                }
                num_bytes_read += n;
            }
            break;
        default:
            throw OperationFailed(ErrorCodeUnsupported, __FILENAME__, __LINE__);
    }
    return ErrorCodeSuccess;
}

ErrorCode ZstdDecompressor::submit_next_block() {
    uint32_t header[2];
    size_t n = 0;
    if (auto rc = read_compressed(reinterpret_cast<char*>(header), sizeof(header), n);
        ErrorCodeSuccess != rc)
    {
        return rc;
    }
    if (0 == n || (0 == header[0] && 0 == header[1])) {
        return ErrorCodeEndOfFile;
    }
    size_t const uncompressed_size = header[0];
    size_t const compressed_size = header[1];
    if (sizeof(header) != n || 0 == uncompressed_size || uncompressed_size > cDeflateBlockSize) {
        SPDLOG_ERROR("ZstdDecompressor: corrupt block header");
        return ErrorCodeFailure;
    }

    m_compressed_block.resize(compressed_size);
    if (auto rc = read_compressed(m_compressed_block.data(), compressed_size, n);
        ErrorCodeSuccess != rc || compressed_size != n)
    {
        SPDLOG_ERROR("ZstdDecompressor: truncated block");
        return ErrorCodeFailure;
    }

    auto& block = m_blocks[(m_blocks_head + m_blocks_count) % cReadAhead];
    block.data.resize(uncompressed_size);
    if (false
        == dpk_deflate_submit(
                block.job,
                m_compressed_block.data(),
                compressed_size,
                block.data.data(),
                uncompressed_size
        ))
    {
        SPDLOG_ERROR(
                "ZstdDecompressor: dpKernels failed to submit a {} -> {} byte block",
                compressed_size,
                uncompressed_size
        );
        return ErrorCodeFailure;
    }
    ++m_blocks_count;
    return ErrorCodeSuccess;
}

ErrorCode ZstdDecompressor::next_block() {
    // Done with the current block: free its place in the ring.
    if (nullptr != m_block) {
        m_blocks_head = (m_blocks_head + 1) % cReadAhead;
        --m_blocks_count;
        m_block = nullptr;
    }

    // Keep the ring full.
    while (m_blocks_count < cReadAhead && false == m_end_of_stream) {
        auto const rc = submit_next_block();
        if (ErrorCodeEndOfFile == rc) {
            m_end_of_stream = true;
        } else if (ErrorCodeSuccess != rc) {
            return rc;
        }
    }
    if (0 == m_blocks_count) {
        return ErrorCodeEndOfFile;
    }

    auto& block = m_blocks[m_blocks_head];
    if (false == dpk_deflate_wait(block.job)) {
        SPDLOG_ERROR(
                "ZstdDecompressor: dpKernels failed to decompress a block to {} bytes",
                block.data.size()
        );
        return ErrorCodeFailure;
    }
    m_block = block.data.data();
    m_block_size = block.data.size();
    m_block_pos = 0;
    return ErrorCodeSuccess;
}

void ZstdDecompressor::discard_blocks() {
    for (auto& block : m_blocks) {
        if (DpkDeflateJob::State::Pending == block.job.state) {
            dpk_deflate_wait(block.job);
        }
    }
    m_blocks_head = 0;
    m_blocks_count = 0;
    m_end_of_stream = false;
    m_block = nullptr;
    m_block_size = 0;
    m_block_pos = 0;
}

ErrorCode
ZstdDecompressor::try_read(char const* buf, size_t num_bytes_to_read, size_t& num_bytes_read) {
    if (InputType::NotInitialized == m_input_type) {
        throw OperationFailed(ErrorCodeNotInit, __FILENAME__, __LINE__);
    }
    if (nullptr == buf) {
        throw OperationFailed(ErrorCodeBadParam, __FILENAME__, __LINE__);
    }

    auto* dst = const_cast<char*>(buf);
    num_bytes_read = 0;
    while (num_bytes_read < num_bytes_to_read) {
        if (m_block_pos == m_block_size) {
            auto const rc = next_block();
            if (ErrorCodeEndOfFile == rc) {
                break;
            }
            if (ErrorCodeSuccess != rc) {
                return rc;
            }
            continue;
        }
        size_t const n
                = std::min(num_bytes_to_read - num_bytes_read, m_block_size - m_block_pos);
        std::memcpy(dst + num_bytes_read, m_block + m_block_pos, n);
        m_block_pos += n;
        num_bytes_read += n;
    }

    m_decompressed_stream_pos += num_bytes_read;
    if (0 == num_bytes_read && num_bytes_to_read > 0) {
        return ErrorCodeEndOfFile;
    }
    return ErrorCodeSuccess;
}

ErrorCode ZstdDecompressor::try_read_string(size_t str_length, std::string& str) {
    str.resize(str_length);

    return try_read_exact_length(&str[0], str_length);
}

ErrorCode ZstdDecompressor::try_read_exact_length(char* buf, size_t num_bytes) {
    size_t num_bytes_read;
    auto error_code = try_read(buf, num_bytes, num_bytes_read);
    if (ErrorCodeSuccess != error_code) {
        return error_code;
    }
    if (num_bytes_read < num_bytes) {
        return ErrorCodeTruncated;
    }

    return ErrorCodeSuccess;
}

void ZstdDecompressor::open(char const* compressed_data_buf, size_t compressed_data_buf_size) {
    if (InputType::NotInitialized != m_input_type) {
        throw OperationFailed(ErrorCodeNotReady, __FILENAME__, __LINE__);
    }
    m_input_type = InputType::CompressedDataBuf;
    m_compressed_buf = compressed_data_buf;
    m_compressed_buf_size = compressed_data_buf_size;
    reset_stream();
}

void ZstdDecompressor::open(FileReader& file_reader, size_t /*file_read_buffer_capacity*/) {
    if (InputType::NotInitialized != m_input_type) {
        throw OperationFailed(ErrorCodeNotReady, __FILENAME__, __LINE__);
    }
    m_input_type = InputType::File;
    m_file_reader = &file_reader;
    m_file_reader_initial_pos = m_file_reader->get_pos();
    reset_stream();
}

void ZstdDecompressor::open(clp::ReaderInterface& reader, size_t /*file_read_buffer_capacity*/) {
    if (InputType::NotInitialized != m_input_type) {
        throw OperationFailed(ErrorCodeNotReady, __FILENAME__, __LINE__);
    }
    m_input_type = InputType::ClpReader;
    m_reader = &reader;
    m_file_reader_initial_pos = m_reader->get_pos();
    reset_stream();
}

void ZstdDecompressor::close() {
    switch (m_input_type) {
        case InputType::MemoryMappedCompressedFile:
            m_memory_mapped_file.reset();
            break;
        case InputType::File:
        case InputType::ClpReader:
            m_file_reader = nullptr;
            m_reader = nullptr;
            break;
        case InputType::CompressedDataBuf:
        case InputType::NotInitialized:
            // Do nothing
            break;
        default:
            throw OperationFailed(ErrorCodeUnsupported, __FILENAME__, __LINE__);
    }
    discard_blocks();
    m_input_type = InputType::NotInitialized;
}

void ZstdDecompressor::close_for_reuse() {
    if (false == (InputType::File == m_input_type || InputType::ClpReader == m_input_type)) {
        close();
        return;
    }
    m_file_reader = nullptr;
    discard_blocks();
    m_input_type = InputType::NotInitialized;
}

ErrorCode ZstdDecompressor::open(std::string const& compressed_file_path) {
    if (InputType::NotInitialized != m_input_type) {
        throw OperationFailed(ErrorCodeNotReady, __FILENAME__, __LINE__);
    }
    m_input_type = InputType::MemoryMappedCompressedFile;

    // Create read-only memory mapping for compressed_file_path
    auto result{clp::ReadOnlyMemoryMappedFile::create(compressed_file_path)};
    if (result.has_error()) {
        auto const error{result.error()};
        SPDLOG_ERROR(
                "ZstdDecompressor: Unable to memory map the compressed file with path: {}. Error: "
                "{} - {}",
                compressed_file_path.c_str(),
                error.category().name(),
                error.message()
        );
        return ErrorCodeFailure;
    }
    m_memory_mapped_file.emplace(std::move(result.value()));

    auto const file_view{m_memory_mapped_file.value().get_view()};
    m_compressed_buf = file_view.data();
    m_compressed_buf_size = file_view.size();
    reset_stream();

    return ErrorCodeSuccess;
}

void ZstdDecompressor::reset_stream() {
    if (InputType::File == m_input_type) {
        if (auto rc = m_file_reader->try_seek_from_begin(m_file_reader_initial_pos);
            ErrorCodeSuccess != rc && ErrorCodeEndOfFile != rc)
        {
            throw OperationFailed(rc, __FILENAME__, __LINE__);
        }
    } else if (InputType::ClpReader == m_input_type) {
        auto rc = m_reader->try_seek_from_begin(m_file_reader_initial_pos);
        if (false
            == (clp::ErrorCode::ErrorCode_Success == rc
                || clp::ErrorCode::ErrorCode_EndOfFile == rc))
        {
            throw OperationFailed(static_cast<ErrorCode>(rc), __FILENAME__, __LINE__);
        }
    }
    m_decompressed_stream_pos = 0;
    m_compressed_buf_pos = 0;
    discard_blocks();
}
}  // namespace clp_s
