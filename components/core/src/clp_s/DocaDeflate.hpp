#ifndef CLP_S_DOCADEFLATE_HPP
#define CLP_S_DOCADEFLATE_HPP

#include <cstddef>

namespace clp_s {
/**
 * Decompresses one raw-DEFLATE block on the BlueField-3 engine with DOCA Compress.
 * @return Whether the block decompressed to exactly `out_size` bytes.
 */
bool doca_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size);
}  // namespace clp_s

#endif  // CLP_S_DOCADEFLATE_HPP
