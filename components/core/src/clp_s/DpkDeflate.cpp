// Decompresses DEFLATE blocks through a dp_manager that is already running (dpKernels' run.sh), so
// each clp-s run connects to it instead of starting its own copy of dpKernels.
#include "DpkDeflate.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <dpm_interface.hpp>
#include <fcntl.h>
#include <libdeflate.h>
#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ZstdCompressor.hpp"

namespace clp_s {
namespace {
// dp_manager only reads and writes its own shared buffers, so each block is copied into one input
// buffer, decompressed into one output buffer, and copied back out.
constexpr uint32_t cBufIn = cDeflateBlockSize + cDeflateBlockSize / 8 + 4096;
constexpr uint32_t cBufOut = cDeflateBlockSize;
constexpr int cSubmitThread = 0;
constexpr auto cTimeout = std::chrono::seconds(10);

bool g_connected = false;  // whether this process got a task and buffers from dp_manager
dpkernel_task* g_task = nullptr;
shm_ptr g_in = 0;  // the buffers, as offsets into dp_manager's shared memory
shm_ptr g_out = 0;
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

// Waits for dp_manager to finish `status`, giving up after cTimeout (e.g. if it was stopped).
template <typename Status>
dpkernel_error wait_for(Status const& status) {
    auto const deadline = std::chrono::steady_clock::now() + cTimeout;
    for (uint32_t i = 1;; ++i) {
        dpkernel_error const s = status.load(std::memory_order_acquire);
        if (DPK_ONGOING != s) {
            return s;
        }
        if (0 == i % 4096 && std::chrono::steady_clock::now() > deadline) {
            return DPK_ERROR_FAILED;
        }
    }
}

bool connect_dpk() {
    // Without a running dp_manager, dpm_frontend_initialize() would create (and fill) its 5 GB of
    // shared memory itself before failing, so first check that the manager's queues exist.
    std::string const ring = std::string{RING_BUFFER_SHM_NAME} + "0";
    int const fd = shm_open(ring.c_str(), O_RDWR, 0);
    if (fd < 0) {
        SPDLOG_ERROR("dpKernels: no dp_manager running (start it with DP-Kernels' run.sh)");
        return false;
    }
    close(fd);

    // dpKernels prints with printf; send that to stderr so it doesn't mix with clp-s's results.
    std::fflush(stdout);
    int const saved_stdout = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    bool ok = dpm_frontend_initialize() && app_alloc_task_request(&g_task);
    if (ok) {
        // the same setup as dpKernels' embedded API (dpk_embed_task_prepare_for_thread)
        g_task->base.task = TASK_DECOMPRESS_DEFLATE;
        g_task->base.device = DEVICE_NONE;
        g_task->bf3.callback_owner_thread_id = cSubmitThread;  // DPM thread that handles our tasks
        // reuse the same DOCA buffers for every block, only updating their data
        dpm_set_task_lazy_bind_mode(g_task, true);
        dpm_mem_req* req = dpm_alloc_mem_buf_async_on_thread(cBufIn, cBufOut, g_task, cSubmitThread);
        ok = nullptr != req && DPK_SUCCESS == wait_for(req->completion);
    }
    if (ok) {
        g_in = g_task->base.in;
        g_out = g_task->base.out;
        // the BF-3 kernel only runs tasks that have a DOCA task preallocated for them
        g_task->bf3.callback_owner_thread_id = cSubmitThread;
        dpm_mem_req* req = dpm_alloc_task_async_on_thread(g_task, cSubmitThread);
        ok = nullptr != req && DPK_SUCCESS == wait_for(req->completion);
    }
    std::fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    if (false == ok) {
        SPDLOG_ERROR("dpKernels: couldn't get a task and buffers from dp_manager");
        return false;
    }

    g_connected = true;
    return true;
}
}  // namespace

bool dpk_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size) {
    static bool const connected = connect_dpk();
    if (false == connected || 0 == in_size || in_size > cBufIn || 0 == out_size
        || out_size > cBufOut)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock{g_mutex};
    std::memcpy(app_get_input_ptr_from_shmptr(g_in), in, in_size);
    g_task->base.in = g_in;
    g_task->base.out = g_out;
    g_task->base.in_size = static_cast<uint32_t>(in_size);
    g_task->base.out_size = static_cast<uint32_t>(out_size);
    g_task->base.actual_out_size = 0;
    g_task->bf3.callback_owner_thread_id = cSubmitThread;
    dpm_submit_task_msgq_blocking(cSubmitThread, g_task, DEVICE_NONE);
    if (DPK_SUCCESS != wait_for(g_task->base.completion) || g_task->base.actual_out_size != out_size)
    {
        return false;
    }
    std::memcpy(out, app_get_output_ptr_from_shmptr(g_out), out_size);
    if (g_verify) {
        verify(in, in_size, out, out_size);
    }
    return true;
}

void dpk_deflate_exit_if_started(int exit_code) {
    if (false == g_connected) {
        return;
    }
    if (g_verify) {
        std::fprintf(
                stderr,
                "[clp-s dpk verify] %llu blocks, %llu differed from software decompression\n",
                static_cast<unsigned long long>(g_blocks),
                static_cast<unsigned long long>(g_mismatches)
        );
    }
    // give the DOCA task and the buffers back, or every clp-s run would use up some of dp_manager's
    // memory (same order as dpKernels' embedded API)
    g_task->base.in = g_in;
    g_task->base.out = g_out;
    if (dpm_mem_req* req = dpm_free_task_async_on_thread(g_task, cSubmitThread); nullptr != req) {
        wait_for(req->completion);
    }
    if (dpm_mem_req* req = dpm_free_mem_buf_async_on_thread(g_task, cSubmitThread); nullptr != req)
    {
        wait_for(req->completion);
    }
    app_free_task_request(g_task);

    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _exit(exit_code);
}
}  // namespace clp_s
