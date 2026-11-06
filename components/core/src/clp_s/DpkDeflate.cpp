#include "DpkDeflate.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <dpk_embed_c_api.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "ZstdCompressor.hpp"

namespace clp_s {
namespace {
// Each dpKernels (task handle, submit thread) pair owns one task object, so it can have one job in
// flight. The pool is cSlotsPerThread handles on each submit thread, and each slot has its own
// input and output area in the I/O region (dpKernels only accepts buffers inside that region).
constexpr int cSlotsPerThread = 2;
constexpr size_t cSlotIn = cDeflateBlockSize + cDeflateBlockSize / 8 + 4096;
constexpr size_t cSlotOut = cDeflateBlockSize;
constexpr auto cTimeout = std::chrono::seconds{10};

struct Slot {
    dpk_embed_task_handle* task{nullptr};
    uint32_t submit_thread{0};
    char* in{nullptr};
    char* out{nullptr};
    DpkDeflateJob* job{nullptr};  // nullptr when the slot is free
};

std::mutex g_mutex;
std::vector<Slot> g_slots;

bool start_dpk() {
    int const num_threads = dpk_embed_get_max_submit_threads();
    size_t const num_slots = static_cast<size_t>(num_threads > 0 ? num_threads : 1) * cSlotsPerThread;
    size_t const region_size = (num_slots * (cSlotIn + cSlotOut) + 4095) / 4096 * 4096;
    auto* region = static_cast<char*>(std::aligned_alloc(4096, region_size));
    if (nullptr == region) {
        return false;
    }

    dpk_embed_runtime_config config{};
    config.io_base = region;
    config.io_bytes = region_size;
    config.enable_thread_pinning = 0;
    config.startup_timeout_ms = 30'000;

    // dpKernels logs with printf; send that to stderr so it doesn't mix with clp-s's output.
    std::fflush(stdout);
    int const saved_stdout = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    int const rc = dpk_embed_runtime_start(&config);
    std::fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    if (0 != rc) {
        return false;
    }
    // No dpk_embed_runtime_stop() at exit: it hangs in the BF-3 kernel's cleanup, which waits for
    // the DOCA contexts to drain. The OS frees the runtime's memory and device handles on exit.

    g_slots.resize(num_slots);
    for (size_t i = 0; i < num_slots; ++i) {
        auto& slot = g_slots[i];
        slot.submit_thread = static_cast<uint32_t>(i % (num_slots / cSlotsPerThread));
        slot.in = region + i * (cSlotIn + cSlotOut);
        slot.out = slot.in + cSlotIn;
        if (0 != dpk_embed_task_create(DPK_EMBED_STAGE_DECOMPRESS_DEFLATE, &slot.task)
            || 0 != dpk_embed_task_bind_io_region(
                    slot.task,
                    reinterpret_cast<uint64_t>(region),
                    static_cast<uint32_t>(region_size)
            )
            || 0 != dpk_embed_task_prepare_for_thread(slot.task, slot.submit_thread))
        {
            return false;
        }
    }
    return true;
}

// If the slot's job has finished, copies its output to the job, marks the job finished and frees
// the slot. Must be called with g_mutex held.
void reap(int slot_index) {
    auto& slot = g_slots[slot_index];
    auto* job = slot.job;
    uint32_t actual = 0;
    uint8_t state = DPK_EMBED_TASK_COMPLETION_PENDING;
    if (0 != dpk_embed_task_check_completion(slot.task, slot.submit_thread, &actual, &state)) {
        state = DPK_EMBED_TASK_COMPLETION_ERROR;
    }
    if (DPK_EMBED_TASK_COMPLETION_PENDING == state) {
        return;
    }
    slot.job = nullptr;
    if (nullptr == job->out) {
        delete job;  // orphaned by a timed-out wait
        return;
    }
    if (DPK_EMBED_TASK_COMPLETION_SUCCESS == state && actual == job->out_size) {
        std::memcpy(job->out, slot.out, job->out_size);
        job->state = DpkDeflateJob::State::Done;
    } else {
        job->state = DpkDeflateJob::State::Failed;
    }
    job->slot = -1;
}
}  // namespace

bool dpk_deflate_submit(DpkDeflateJob& job, char const* in, size_t in_size, char* out, size_t out_size) {
    static bool const started = start_dpk();
    job.state = DpkDeflateJob::State::Failed;
    job.slot = -1;
    if (false == started) {
        SPDLOG_ERROR("dpKernels: failed to start the runtime");
        return false;
    }
    if (0 == in_size || in_size > cSlotIn || 0 == out_size || out_size > cSlotOut) {
        return false;
    }

    std::unique_lock<std::mutex> lock{g_mutex};
    // Find a free slot, collecting finished jobs (anyone's) until one frees up.
    int free_slot = -1;
    auto const deadline = std::chrono::steady_clock::now() + cTimeout;
    while (-1 == free_slot) {
        for (int i = 0; i < static_cast<int>(g_slots.size()) && -1 == free_slot; ++i) {
            if (nullptr != g_slots[i].job) {
                reap(i);
            }
            if (nullptr == g_slots[i].job) {
                free_slot = i;
            }
        }
        if (-1 == free_slot) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
    }

    auto& slot = g_slots[free_slot];
    std::memcpy(slot.in, in, in_size);
    if (0 != dpk_embed_task_submit_async(
                slot.task,
                slot.submit_thread,
                reinterpret_cast<uint64_t>(slot.in),
                static_cast<uint32_t>(in_size),
                reinterpret_cast<uint64_t>(slot.out),
                static_cast<uint32_t>(out_size)
        ))
    {
        return false;
    }
    job.state = DpkDeflateJob::State::Pending;
    job.out = out;
    job.out_size = out_size;
    job.slot = free_slot;
    slot.job = &job;
    return true;
}

bool dpk_deflate_poll(DpkDeflateJob& job) {
    std::lock_guard<std::mutex> lock{g_mutex};
    if (DpkDeflateJob::State::Pending == job.state) {
        reap(job.slot);
    }
    return DpkDeflateJob::State::Pending != job.state;
}

bool dpk_deflate_wait(DpkDeflateJob& job) {
    auto const deadline = std::chrono::steady_clock::now() + cTimeout;
    while (false == dpk_deflate_poll(job)) {
        if (std::chrono::steady_clock::now() > deadline) {
            // The engine may still write into the slot, so keep it claimed by an orphan job and
            // release the caller's.
            std::lock_guard<std::mutex> lock{g_mutex};
            if (DpkDeflateJob::State::Pending == job.state) {
                g_slots[job.slot].job = new DpkDeflateJob{DpkDeflateJob::State::Pending};
                job.state = DpkDeflateJob::State::Failed;
                job.slot = -1;
            }
            SPDLOG_ERROR("dpKernels: decompression timed out");
            return false;
        }
        std::this_thread::yield();
    }
    return DpkDeflateJob::State::Done == job.state;
}
}  // namespace clp_s
