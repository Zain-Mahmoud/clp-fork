#ifndef CLP_S_DPKDEFLATE_HPP
#define CLP_S_DPKDEFLATE_HPP

#include <cstddef>

namespace clp_s {
/**
 * Decompresses one raw-DEFLATE block on the BlueField-3 decompression engine through dpKernels,
 * waiting for it to finish. Needs dpKernels' dp_manager to be running already (DP-Kernels' run.sh);
 * clp-s connects to it on first use.
 * @return Whether the block decompressed to exactly `out_size` bytes.
 */
bool dpk_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size);

/**
 * If clp-s connected to dp_manager, gives its buffers back, writes out all buffered output and
 * ends the process with `exit_code` right away, skipping exit-time cleanup (dpKernels' library's
 * global destructors crashed the process at exit). Returns normally if clp-s never connected.
 */
void dpk_deflate_exit_if_started(int exit_code);
}  // namespace clp_s

#endif  // CLP_S_DPKDEFLATE_HPP
