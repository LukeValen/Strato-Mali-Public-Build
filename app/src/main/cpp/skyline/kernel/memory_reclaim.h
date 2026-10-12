// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cerrno>
#include <cstddef>
#include <sys/mman.h>

namespace skyline::kernel::memoryReclaim {
    // MADV_REMOVE needs VM_WRITE even for shared backing we originally allocated
    // writable. A trap or guest permission change may have made this VMA read-only.
    // Use a temporary writable alias instead of weakening the live guest mapping.
    inline int ReclaimSharedPages(void *address, std::size_t size) {
        if (!size) return 0;
        if (madvise(address, size, MADV_REMOVE) == 0) return 0;
        const int error{errno};
        if (error != EACCES) return error;

        void *alias{mremap(address, 0, size, MREMAP_MAYMOVE)};
        if (alias == MAP_FAILED) return errno;
        int result{};
        if (mprotect(alias, size, PROT_READ | PROT_WRITE) != 0)
            result = errno;
        else if (madvise(alias, size, MADV_REMOVE) != 0)
            result = errno;
        if (munmap(alias, size) != 0 && !result) result = errno;
        return result;
    }
}
