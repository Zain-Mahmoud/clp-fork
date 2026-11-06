// Code from CLP
//
// Reads the block-DEFLATE format written by ZstdCompressor (see ZstdCompressor.cpp) and
// decompresses each block on the BlueField-3 decompression engine through dpKernels, one block at
// a time.
#include "ZstdDecompressor.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include "DpkDeflate.hpp"
#include "ZstdCompressor.hpp"


namespace clp_s {
ZstdDecompressor::ZstdDecompressor()
        : Decompressor(CompressorType::ZSTD),
          m_input_type(InputType::NotInitialized),
          m_file_reader(nullptr),
          m_reader(nullptr),
          m_file_reader_initial_pos(0),
          m_decompressed_stream_pos(0) {}

ZstdDecompressor::~ZstdDecompressor() = default;

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

ErrorCode ZstdDecompressor::load_block() {
    uint32_t header[2];
    size_t n = 0;
    if (auto rc = read_compressed(reinterpret_cast<char*>(header), sizeof(header), n);
        ErrorCodeSuccess != rc)
    {
        return rc;
    }
    // No more data, or the [0][0] end-of-stream header.
    if (0 == n || (sizeof(header) == n && 0 == header[0] && 0 == header[1])) {
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

    m_block.resize(uncompressed_size);
    if (false
        == dpk_deflate_decompress(
                m_compressed_block.data(),
                compressed_size,
                m_block.data(),
                uncompressed_size
        ))
    {
        SPDLOG_ERROR(
                "ZstdDecompressor: dpKernels failed to decompress a {} -> {} byte block",
                compressed_size,
                uncompressed_size
        );
        return ErrorCodeFailure;
    }
    m_block_size = uncompressed_size;
    m_block_pos = 0;
    return ErrorCodeSuccess;
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
            auto const rc = load_block();
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
        std::memcpy(dst + num_bytes_read, m_block.data() + m_block_pos, n);
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
    m_block_pos = m_block_size = 0;
    m_input_type = InputType::NotInitialized;
}

void ZstdDecompressor::close_for_reuse() {
    if (false == (InputType::File == m_input_type || InputType::ClpReader == m_input_type)) {
        close();
        return;
    }
    m_file_reader = nullptr;
    m_block_pos = m_block_size = 0;
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
    m_block_pos = m_block_size = 0;
}
}  // namespace clp_s
