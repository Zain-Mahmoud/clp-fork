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
}  // namespace clp_s

#endif  // CLP_S_DPKDEFLATE_HPP
