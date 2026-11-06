#ifndef CLP_S_DPKDEFLATE_HPP
#define CLP_S_DPKDEFLATE_HPP

#include <cstddef>

namespace clp_s {
/**
 * Decompresses one raw-DEFLATE block on the BlueField-3 decompression engine through dpKernels,
 * waiting for it to finish. The dpKernels runtime is started on first use.
 * @return Whether the block decompressed to exactly `out_size` bytes.
 */
bool dpk_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size);

/**
 * If the dpKernels runtime was started, writes out all buffered output and ends the process with
 * `exit_code` right away, skipping exit-time cleanup. dpKernels' threads are still running then,
 * and its library's global destructors crash the process (dpk_embed_runtime_stop() would stop
 * them, but it hangs in the BF-3 kernel's cleanup). Returns normally if dpKernels never started.
 */
void dpk_deflate_exit_if_started(int exit_code);
}  // namespace clp_s

#endif  // CLP_S_DPKDEFLATE_HPP
