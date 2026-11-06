#include "DpkDeflate.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#include <dpk_embed_c_api.h>
#include <libdeflate.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "ZstdCompressor.hpp"

namespace clp_s {
namespace {
// dpKernels only accepts buffers inside a memory region registered with it, so each block is
// copied into this region (input area, then output area), decompressed, and copied back out.
constexpr size_t cRegionIn = cDeflateBlockSize + cDeflateBlockSize / 8 + 4096;
constexpr size_t cRegionOut = cDeflateBlockSize;
constexpr size_t cRegionSize = (cRegionIn + cRegionOut + 4095) / 4096 * 4096;
constexpr uint32_t cSubmitThread = 0;
constexpr uint32_t cTimeoutMs = 10'000;

char* g_region = nullptr;
dpk_embed_task_handle* g_task = nullptr;
std::mutex g_mutex;

// CLP_S_DPK_VERIFY=1 (debugging): also decompress every block with libdeflate and report blocks
// where dpKernels' output differs. The (correct) software output is then used.
bool const g_verify = nullptr != std::getenv("CLP_S_DPK_VERIFY");
uint64_t g_blocks = 0;
uint64_t g_mismatches = 0;

void verify(char const* in, size_t in_size, char* out, size_t out_size) {
    static libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    static std::vector<char> expected;
    expected.resize(out_size);
    size_t actual = 0;
    ++g_blocks;
    if (LIBDEFLATE_SUCCESS
                != libdeflate_deflate_decompress(
                        decompressor,
                        in,
                        in_size,
                        expected.data(),
                        out_size,
                        &actual
                )
        || actual != out_size || 0 == std::memcmp(expected.data(), out, out_size))
    {
        return;
    }
    size_t first = 0;
    size_t num_diff = 0;
    for (size_t i = out_size; i-- > 0;) {
        if (expected[i] != out[i]) {
            first = i;
            ++num_diff;
        }
    }
    if (++g_mismatches <= 20) {
        std::fprintf(
                stderr,
                "[clp-s dpk verify] block %llu: %zu -> %zu bytes, %zu bytes differ, first at %zu\n",
                static_cast<unsigned long long>(g_blocks),
                in_size,
                out_size,
                num_diff,
                first
        );
    }
    std::memcpy(out, expected.data(), out_size);
}

bool start_dpk() {
    g_region = static_cast<char*>(std::aligned_alloc(4096, cRegionSize));
    if (nullptr == g_region) {
        return false;
    }
    dpk_embed_runtime_config config{};
    config.io_base = g_region;
    config.io_bytes = cRegionSize;
    config.enable_thread_pinning = 0;
    config.startup_timeout_ms = 30'000;

    // dpKernels logs with printf, partly from its own threads; send that to stderr so it doesn't
    // mix with clp-s's results. Its threads may still be starting (and printing) after
    // dpk_embed_runtime_start() returns, so keep the redirect until a task is prepared on every
    // submit thread: each prepare waits for that thread to answer.
    std::fflush(stdout);
    int const saved_stdout = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    bool ok = 0 == dpk_embed_runtime_start(&config)
              && 0 == dpk_embed_task_create(DPK_EMBED_STAGE_DECOMPRESS_DEFLATE, &g_task)
              && 0 == dpk_embed_task_bind_io_region(
                      g_task,
                      reinterpret_cast<uint64_t>(g_region),
                      static_cast<uint32_t>(cRegionSize)
              );
    int const num_threads = ok ? dpk_embed_get_max_submit_threads() : 0;
    for (int i = 0; ok && i < num_threads; ++i) {
        ok = 0 == dpk_embed_task_prepare_for_thread(g_task, static_cast<uint32_t>(i));
    }
    std::fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    // No dpk_embed_runtime_stop() at exit: it hangs in the BF-3 kernel's cleanup, which waits for
    // the DOCA contexts to drain. Instead the OS frees the runtime's memory and device handles.
    // dpKernels' threads keep running while the process exits and can crash it during teardown,
    // which would lose clp-s's still-buffered output. This handler is registered after clp-s's
    // own globals exist, so it runs before they're torn down: write everything out first.
    std::atexit([] {
        std::cout.flush();
        std::fflush(nullptr);
    });
    if (g_verify) {
        std::atexit([] {
            std::fprintf(
                    stderr,
                    "[clp-s dpk verify] %llu blocks, %llu differed from software decompression\n",
                    static_cast<unsigned long long>(g_blocks),
                    static_cast<unsigned long long>(g_mismatches)
            );
        });
    }
    return ok;
}
}  // namespace

bool dpk_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size) {
    static bool const started = start_dpk();
    if (false == started) {
        SPDLOG_ERROR("dpKernels: failed to start the runtime");
        return false;
    }
    if (0 == in_size || in_size > cRegionIn || 0 == out_size || out_size > cRegionOut) {
        return false;
    }

    std::lock_guard<std::mutex> lock{g_mutex};
    char* hw_in = g_region;
    char* hw_out = g_region + cRegionIn;
    std::memcpy(hw_in, in, in_size);
    uint32_t actual = 0;
    if (0 != dpk_embed_task_submit_and_wait(
                g_task,
                cSubmitThread,
                reinterpret_cast<uint64_t>(hw_in),
                static_cast<uint32_t>(in_size),
                reinterpret_cast<uint64_t>(hw_out),
                static_cast<uint32_t>(out_size),
                cTimeoutMs,
                &actual
        )
        || actual != out_size)
    {
        return false;
    }
    std::memcpy(out, hw_out, out_size);
    if (g_verify) {
        verify(hw_in, in_size, out, out_size);
    }
    return true;
}
}  // namespace clp_s
