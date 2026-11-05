#ifndef CLP_S_DPKDEFLATE_HPP
#define CLP_S_DPKDEFLATE_HPP

#include <cstddef>

namespace clp_s {
/**
 * One raw-DEFLATE block submitted to the BlueField-3 decompression engine through dpKernels.
 * Submit it, then poll or wait on it; the decompressed bytes land in `out` once it's done.
 * A pending job must stay at the same address (don't move or destroy it) until it's finished.
 */
struct DpkDeflateJob {
    enum class State {
        Idle,
        Pending,
        Done,
        Failed
    };

    State state{State::Idle};
    char* out{nullptr};
    size_t out_size{0};
    int slot{-1};
};

/**
 * Copies the block into dpKernels' I/O region and submits it without waiting. If every engine
 * slot is busy, this first waits for one to finish. The dpKernels runtime is started on first use.
 * @return Whether the job was submitted (otherwise job.state is Failed).
 */
bool dpk_deflate_submit(DpkDeflateJob& job, char const* in, size_t in_size, char* out, size_t out_size);

/**
 * @return Whether the job has finished (job.state is Done or Failed).
 */
bool dpk_deflate_poll(DpkDeflateJob& job);

/**
 * Blocks until the job has finished.
 * @return Whether it decompressed to exactly `out_size` bytes.
 */
bool dpk_deflate_wait(DpkDeflateJob& job);
}  // namespace clp_s

#endif  // CLP_S_DPKDEFLATE_HPP
