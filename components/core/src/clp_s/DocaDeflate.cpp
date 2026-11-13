// Decompresses raw-DEFLATE blocks on the BlueField-3 engine with DOCA Compress, one at a time.
// Each block is copied into a buffer registered with the device, decompressed, and copied back.
#include "DocaDeflate.hpp"

#include <cstdlib>
#include <cstring>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_compress.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "ZstdCompressor.hpp"

namespace clp_s {
namespace {
constexpr size_t cInSize = 2 * cDeflateBlockSize;  // room for blocks that don't compress
constexpr size_t cBufSize = cInSize + cDeflateBlockSize;

doca_dev* g_dev = nullptr;
doca_compress* g_compress = nullptr;
doca_pe* g_pe = nullptr;
doca_mmap* g_mmap = nullptr;
doca_buf_inventory* g_inventory = nullptr;
char* g_buf = nullptr;  // input at g_buf, output at g_buf + cInSize

bool g_done = false;
doca_error_t g_status = DOCA_SUCCESS;

void on_done(doca_compress_task_decompress_deflate* task, doca_data, doca_data) {
    g_status = doca_task_get_status(doca_compress_task_decompress_deflate_as_task(task));
    g_done = true;
}

bool init() {
    doca_devinfo** list = nullptr;
    uint32_t n = 0;
    if (DOCA_SUCCESS != doca_devinfo_create_list(&list, &n)) {
        return false;
    }
    for (uint32_t i = 0; i < n && nullptr == g_dev; ++i) {
        if (DOCA_SUCCESS == doca_compress_cap_task_decompress_deflate_is_supported(list[i])) {
            doca_dev_open(list[i], &g_dev);
        }
    }
    doca_devinfo_destroy_list(list);

    g_buf = static_cast<char*>(std::aligned_alloc(4096, cBufSize));
    return nullptr != g_dev && nullptr != g_buf
           && DOCA_SUCCESS == doca_compress_create(g_dev, &g_compress)
           && DOCA_SUCCESS == doca_pe_create(&g_pe)
           && DOCA_SUCCESS == doca_pe_connect_ctx(g_pe, doca_compress_as_ctx(g_compress))
           && DOCA_SUCCESS
                      == doca_compress_task_decompress_deflate_set_conf(
                              g_compress,
                              on_done,
                              on_done,
                              1
                      )
           && DOCA_SUCCESS == doca_ctx_start(doca_compress_as_ctx(g_compress))
           && DOCA_SUCCESS == doca_mmap_create(&g_mmap)
           && DOCA_SUCCESS == doca_mmap_set_memrange(g_mmap, g_buf, cBufSize)
           && DOCA_SUCCESS == doca_mmap_set_permissions(g_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE)
           && DOCA_SUCCESS == doca_mmap_add_dev(g_mmap, g_dev)
           && DOCA_SUCCESS == doca_mmap_start(g_mmap)
           && DOCA_SUCCESS == doca_buf_inventory_create(2, &g_inventory)
           && DOCA_SUCCESS == doca_buf_inventory_start(g_inventory);
}
}  // namespace

bool doca_deflate_decompress(char const* in, size_t in_size, char* out, size_t out_size) {
    static bool const ready = init();
    if (false == ready || in_size > cInSize || out_size > cDeflateBlockSize) {
        return false;
    }
    std::memcpy(g_buf, in, in_size);

    doca_buf* src = nullptr;
    doca_buf* dst = nullptr;
    doca_compress_task_decompress_deflate* task = nullptr;
    g_done = false;
    bool ok = DOCA_SUCCESS
                      == doca_buf_inventory_buf_get_by_data(g_inventory, g_mmap, g_buf, in_size, &src)
              && DOCA_SUCCESS
                         == doca_buf_inventory_buf_get_by_addr(
                                 g_inventory,
                                 g_mmap,
                                 g_buf + cInSize,
                                 out_size,
                                 &dst
                         )
              && DOCA_SUCCESS
                         == doca_compress_task_decompress_deflate_alloc_init(
                                 g_compress,
                                 src,
                                 dst,
                                 {},
                                 &task
                         )
              && DOCA_SUCCESS
                         == doca_task_submit(doca_compress_task_decompress_deflate_as_task(task));
    while (ok && false == g_done) {
        doca_pe_progress(g_pe);
    }

    size_t len = 0;
    ok = ok && DOCA_SUCCESS == g_status && DOCA_SUCCESS == doca_buf_get_data_len(dst, &len)
         && len == out_size;
    if (ok) {
        std::memcpy(out, g_buf + cInSize, out_size);
    }
    if (nullptr != task) {
        doca_task_free(doca_compress_task_decompress_deflate_as_task(task));
    }
    if (nullptr != src) {
        doca_buf_dec_refcount(src, nullptr);
    }
    if (nullptr != dst) {
        doca_buf_dec_refcount(dst, nullptr);
    }
    return ok;
}
}  // namespace clp_s
