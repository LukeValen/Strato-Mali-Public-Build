// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <fstream>
#include <asm-generic/unistd.h>
#include <fcntl.h>
#include "memory.h"
#include "memory_reclaim.h"
#include "types/KProcess.h"

namespace skyline::kernel {
    MemoryManager::MemoryManager(const DeviceState &state) noexcept : state{state}, processHeapSize{}, memRefs{} {}

    MemoryManager::~MemoryManager() noexcept {
        if (base.valid() && !base.empty())
            munmap(reinterpret_cast<void *>(base.data()), base.size());
        if (codeBase36Bit.valid() && !codeBase36Bit.empty())
            munmap(reinterpret_cast<void *>(codeBase36Bit.data()), codeBase36Bit.size());
    }

    void MemoryManager::MapInternal(const std::pair<u8 *, ChunkDescriptor> &newDesc) {
        // The chunk that contains / precedes the new chunk base address
        auto firstChunkBase{chunks.lower_bound(newDesc.first)};
        if (newDesc.first <= firstChunkBase->first && firstChunkBase != chunks.begin())
            --firstChunkBase;

        // The chunk that contains / follows the end address of the new chunk
        auto lastChunkBase{chunks.lower_bound(newDesc.first + newDesc.second.size)};
        if ((newDesc.first + newDesc.second.size) < lastChunkBase->first)
            --lastChunkBase;

        ChunkDescriptor firstChunk{firstChunkBase->second};
        ChunkDescriptor lastChunk{lastChunkBase->second};

        bool needsReprotection{false};
        bool isUnmapping{newDesc.second.state == memory::states::Unmapped};

        // We cut a hole in a single chunk
        if (firstChunkBase->first == lastChunkBase->first) {
            if (firstChunk.IsCompatible(newDesc.second)) [[unlikely]]
                // No editing necessary
                return;

            if ((firstChunk.state == memory::states::Unmapped) != isUnmapping)
                needsReprotection = true;

            // We reduce the size of the first half
            firstChunk.size = static_cast<size_t>(newDesc.first - firstChunkBase->first);
            chunks.insert_or_assign(firstChunkBase->first, firstChunk);

            // We create the chunk's second half
            lastChunk.size = static_cast<size_t>((lastChunkBase->first + lastChunk.size) - (newDesc.first + newDesc.second.size));
            chunks.insert_or_assign(newDesc.first + newDesc.second.size, lastChunk);

            // Insert new chunk in between
            chunks.insert_or_assign(newDesc.first, newDesc.second);
        } else {
            // If there are descriptors between first and last chunk, delete them
            if ((firstChunkBase->first + firstChunk.size) != lastChunkBase->first) {
                auto tempChunkBase{std::next(firstChunkBase)};

                while (tempChunkBase->first != lastChunkBase->first) {
                    auto tmp{tempChunkBase++};
                    if ((tmp->second.state == memory::states::Unmapped) != isUnmapping)
                        needsReprotection = true;
                }
                chunks.erase(std::next(firstChunkBase), lastChunkBase);
            }

            bool shouldInsert{true};

            // We check if the new chunk and the first chunk is mergeable
            if (firstChunk.IsCompatible(newDesc.second)) {
                shouldInsert = false;

                firstChunk.size = static_cast<size_t>((newDesc.first + newDesc.second.size) - firstChunkBase->first);
                chunks.insert_or_assign(firstChunkBase->first, firstChunk);
            } else if ((firstChunkBase->first + firstChunk.size) != newDesc.first) { // If it's not mergeable check if it needs resizing
                firstChunk.size = static_cast<size_t>(newDesc.first - firstChunkBase->first);

                chunks.insert_or_assign(firstChunkBase->first, firstChunk);

                if ((firstChunk.state == memory::states::Unmapped) != isUnmapping)
                    needsReprotection = true;
            }

            // We check if the new chunk and the last chunk is mergeable
            if (lastChunk.IsCompatible(newDesc.second)) {
                u8 *oldBase{lastChunkBase->first};
                chunks.erase(lastChunkBase);

                if (shouldInsert) {
                    shouldInsert = false;

                    lastChunk.size = static_cast<size_t>((lastChunk.size + oldBase) - (newDesc.first));

                    chunks.insert_or_assign(newDesc.first, lastChunk);
                } else {
                    firstChunk.size = static_cast<size_t>((lastChunk.size + oldBase) - firstChunkBase->first);
                    chunks.insert_or_assign(firstChunkBase->first, firstChunk);
                }
            } else if ((newDesc.first + newDesc.second.size) != lastChunkBase->first) { // If it's not mergeable check if it needs resizing
                lastChunk.size = static_cast<size_t>((lastChunk.size + lastChunkBase->first) - (newDesc.first + newDesc.second.size));

                chunks.erase(lastChunkBase);
                chunks.insert_or_assign(newDesc.first + newDesc.second.size, lastChunk);

                if ((lastChunk.state == memory::states::Unmapped) != isUnmapping)
                    needsReprotection = true;
            }

            // Insert if not merged
            if (shouldInsert)
                chunks.insert_or_assign(newDesc.first, newDesc.second);
        }

        if (needsReprotection) {
            // Retrieve the host region to re-protect
            span<u8> hostSpan{GetHostSpan({newDesc.first, newDesc.second.size})};
            if (mprotect(hostSpan.data(), hostSpan.size(), !isUnmapping ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_NONE)) [[unlikely]]
                LOGW("Reprotection failed: {}", strerror(errno));
        }
    }

    void MemoryManager::ForeachChunkInRange(span<u8> memory, auto editCallback) {
        auto chunkBase{chunks.lower_bound(memory.data())};
        if (memory.data() < chunkBase->first)
            --chunkBase;

        size_t sizeLeft{memory.size()};

        if (chunkBase->first < memory.data()) [[unlikely]] {
            size_t chunkSize{std::min<size_t>(chunkBase->second.size - (static_cast<size_t>(memory.data() - chunkBase->first)), memory.size())};

            std::pair<u8 *, ChunkDescriptor> temp{memory.data(), chunkBase->second};
            temp.second.size = chunkSize;
            editCallback(temp);

            ++chunkBase;
            sizeLeft -= chunkSize;
        }

        while (sizeLeft) {
            if (sizeLeft < chunkBase->second.size) {
                std::pair<u8 *, ChunkDescriptor> temp(*chunkBase);
                temp.second.size = sizeLeft;
                editCallback(temp);
                break;
            } else [[likely]] {
                std::pair<u8 *, ChunkDescriptor> temp(*chunkBase);

                editCallback(temp);

                sizeLeft = sizeLeft - chunkBase->second.size;
                ++chunkBase;
            }
        }
    }

    static span<u8> AllocateMappedRange(size_t minSize, size_t align, size_t minAddress, size_t maxAddress, bool findLargest) {
        span<u8> region{};
        size_t size{minSize};

        std::ifstream mapsFile("/proc/self/maps");
        std::string maps((std::istreambuf_iterator<char>(mapsFile)), std::istreambuf_iterator<char>());
        size_t line{}, start{minAddress}, alignedStart{minAddress};
        do {
            auto end{util::HexStringToInt<u64>(std::string_view(maps.data() + line, sizeof(u64) * 2))};
            if (end < start)
                continue;
            if (end - start > size + (alignedStart - start)) { // We don't want to overflow if alignedStart > start
                if (findLargest)
                    size = end - start;

                region = span<u8>{reinterpret_cast<u8 *>(alignedStart), size};

                if (!findLargest)
                    break;
            }

            start = util::HexStringToInt<u64>(std::string_view(maps.data() + maps.find_first_of('-', line) + 1, sizeof(u64) * 2));
            alignedStart = util::AlignUp(start, align);
            if (alignedStart + size > maxAddress) // We don't want to map past the end of the address space
                break;
        } while ((line = maps.find_first_of('\n', line)) != std::string::npos && line++);

        if (!region.valid()) [[unlikely]]
            throw exception("Allocation failed");

        auto result{mmap(reinterpret_cast<void *>(region.data()), size, PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_SHARED, -1, 0)};
        if (result == MAP_FAILED) [[unlikely]]
            throw exception("Failed to mmap guest address space: {}", strerror(errno));

        return region;
    }

    namespace {
        constexpr size_t RegionAlignment{1ULL << 21}; //!< The minimum alignment of a HOS memory region

        namespace AS36bit {
            constexpr size_t CodeRegionStart{0x8000000}; //!< The start address of the code region (128MiB)
            constexpr size_t CodeRegionSize{0x78000000}; //!< The size of the code region (2GiB - 128MiB)
            constexpr size_t AliasRegionSize{0x180000000}; //!< The size of the alias region (6GiB)
            constexpr size_t HeapRegionSize{0x180000000}; //!< The size of the heap region (6GiB)

            constexpr size_t TotalSize{AliasRegionSize + HeapRegionSize};
        }

        namespace AS39bit {
            constexpr size_t MaxCodeRegionSize{4ULL * 1024 * 1024 * 1024}; //!< The assumed maximum size of the code region (4GiB)
            constexpr size_t AliasRegionSize{0x1000000000}; //!< The size of the alias region (64GiB)
            constexpr size_t HeapRegionSize{0x180000000}; //!< The size of the heap region (6GiB)
            constexpr size_t StackRegionSize{0x80000000}; //!< The size of the stack region (2GiB)
            constexpr size_t TlsIoRegionSize{0x1000000000}; //!< The size of the TLS/IO region (64GiB)

            constexpr size_t TotalSize{MaxCodeRegionSize + AliasRegionSize + HeapRegionSize + StackRegionSize + TlsIoRegionSize};
        }
    }

    void MemoryManager::InitializeVmm(memory::AddressSpaceType type) {
        addressSpaceType = type;

        LOGD("Initializing VMM for {}", to_string(addressSpaceType));

        size_t baseSize{}, maxAddress{};
        switch (type) {
            case memory::AddressSpaceType::AddressSpace32Bit:
            case memory::AddressSpaceType::AddressSpace32BitNoReserved:
                throw exception("32-bit address spaces are not supported");

            case memory::AddressSpaceType::AddressSpace36Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), (1ULL << 36)};
                baseSize = AS36bit::TotalSize;
                maxAddress = addressSpace.size();
                break;
            }

            case memory::AddressSpaceType::AddressSpace39Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 39};
                baseSize = AS39bit::TotalSize;
                maxAddress = addressSpace.size();
                break;
            }

            default:
                throw exception("VMM initialization with unknown address space");
        }

        // Qualcomm KGSL (Kernel Graphic Support Layer/Kernel GPU driver) maps below 35-bits, reserving it causes KGSL to go OOM
        static constexpr size_t KgslReservedRegionSize{1ULL << 35};

        base = AllocateMappedRange(baseSize, RegionAlignment, KgslReservedRegionSize, maxAddress, false);

        switch (type) {
            case memory::AddressSpaceType::AddressSpace36Bit: {
                code = codeBase36Bit = AllocateMappedRange(AS36bit::CodeRegionSize, RegionAlignment, AS36bit::CodeRegionStart, KgslReservedRegionSize, false);

                if ((reinterpret_cast<u64>(base.data()) + baseSize) > (1ULL << 36)) {
                    LOGW("Couldn't fit regions into 36 bit AS! Resizing AS to 39 bits!");
                    addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 39};
                }

                break;
            }

            default: {
                // Will be resized later when initializing regions
                code = base;
                break;
            }
        }

        // Insert a placeholder element at the end of the map to make sure upper_bound/lower_bound never triggers std::map::end() which is broken
        chunks = {{addressSpace.data(), {
            .size = addressSpace.size(),
            .state = memory::states::Unmapped,
        }}, {addressSpace.end().base(), {
            .state = memory::states::Reserved,
        }}};
    }

    void MemoryManager::InitializeRegions(span<u8> codeRegion) {
        // Get the host address of the code region
        codeRegion = GetHostSpan(codeRegion);

        if (!util::IsAligned(codeRegion.data(), RegionAlignment)) [[unlikely]]
            throw exception("Non-aligned code region was used to initialize regions: {} - {}", fmt::ptr(codeRegion.data()), fmt::ptr(codeRegion.end().base()));

        switch (addressSpaceType) {
            case memory::AddressSpaceType::AddressSpace36Bit: {
                // As a workaround if we can't place the code region at the base of the AS we mark it as inaccessible heap so rtld doesn't crash
                if (codeBase36Bit.data() != reinterpret_cast<u8 *>(AS36bit::CodeRegionStart)) {
                    MapInternal(std::pair<u8 *, ChunkDescriptor>(reinterpret_cast<u8 *>(AS36bit::CodeRegionStart), {
                        .size = reinterpret_cast<size_t>(codeBase36Bit.data() - AS36bit::CodeRegionStart),
                        .state = memory::states::Heap
                    }));
                }

                // Place code, stack and TLS/IO in the lower 36-bits of the host AS and heap and alias past that
                code = span<u8>{codeBase36Bit.data(), codeBase36Bit.data() + AS36bit::CodeRegionSize};
                stack = code; // stack is shared with code on 36-bit
                tlsIo = stack; // TLS/IO is shared with stack on 36-bit
                alias = span<u8>{base.data(), AS36bit::AliasRegionSize};
                heap = span<u8>{alias.host.end().base(), AS36bit::HeapRegionSize};
                break;
            }

            case memory::AddressSpaceType::AddressSpace39Bit: {
                code = span<u8>{base.data(), util::AlignUp(codeRegion.size(), RegionAlignment)};
                alias = span<u8>{code.host.end().base(), AS39bit::AliasRegionSize};
                heap = span<u8>{alias.host.end().base(), AS39bit::HeapRegionSize};
                stack = span<u8>{heap.host.end().base(), AS39bit::StackRegionSize};
                tlsIo = span<u8>{stack.host.end().base(), AS39bit::TlsIoRegionSize};

                u64 newSize{code.size() + alias.size() + stack.size() + heap.size() + tlsIo.size()};

                if (newSize > base.size()) [[unlikely]]
                    throw exception("Guest VMM size has exceeded host carveout size: 0x{:X}/0x{:X} (Code: 0x{:X}/0x{:X})", newSize, base.size(), code.size(), AS39bit::MaxCodeRegionSize);

                if (newSize != base.size()) [[likely]]
                    munmap(base.end().base(), newSize - base.size());

                break;
            }

            default:
                throw exception("Regions initialized without VMM initialization");
        }

        if (codeRegion.size() > code.size()) [[unlikely]]
            throw exception("Code region ({}) is smaller than mapped code size ({})", code.size(), codeRegion.size());

        LOGD("Region Map:\n"
             "VMM Base: {}\n"
             "Code Region: {} - {} (Size: 0x{:X})\n"
             "Alias Region: {} - {} (Size: 0x{:X})\n"
             "Heap Region: {} - {} (Size: 0x{:X})\n"
             "Stack Region: {} - {} (Size: 0x{:X})\n"
             "TLS/IO Region: {} - {} (Size: 0x{:X})",
             fmt::ptr(base.data()),
             fmt::ptr(code.guest.data()), fmt::ptr(code.guest.end().base()), code.size(),
             fmt::ptr(alias.guest.data()), fmt::ptr(alias.guest.end().base()), alias.size(),
             fmt::ptr(heap.guest.data()), fmt::ptr(heap.guest.end().base()), heap.size(),
             fmt::ptr(stack.guest.data()), fmt::ptr(stack.guest.end().base()), stack.size(),
             fmt::ptr(tlsIo.guest.data()), fmt::ptr(tlsIo.guest.end().base()), tlsIo.size());
    }

    span<u8> MemoryManager::CreateMirror(span<u8> mapping) {
        // Get the host address of the mapping
        mapping = GetHostSpan(mapping);

        if (!base.contains(mapping)) [[unlikely]]
            throw exception("Mapping is outside of VMM base: {} - {}", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()));

        auto offset{static_cast<size_t>(mapping.data() - base.data())};
        if (!util::IsPageAligned(offset) || !util::IsPageAligned(mapping.size())) [[unlikely]]
            throw exception("Mapping is not aligned to a page: {} - {} (0x{:X})", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()), offset);

        auto mirror{mremap(mapping.data(), 0, mapping.size(), MREMAP_MAYMOVE)};
        if (mirror == MAP_FAILED) [[unlikely]]
            throw exception("Failed to create mirror mapping at {} - {} (0x{:X}): {}", fmt::ptr(mapping.data()), fmt::ptr(mapping.end().base()), offset, strerror(errno));

        mprotect(mirror, mapping.size(), PROT_READ | PROT_WRITE);

        return span<u8>{reinterpret_cast<u8 *>(mirror), mapping.size()};
    }

    span<u8> MemoryManager::CreateMirrors(const std::vector<span<u8>> &regions) {
        size_t totalSize{};
        for (const auto &region : regions)
            totalSize += region.size();

        auto mirrorBase{mmap(nullptr, totalSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}; // Reserve address space for all mirrors
        if (mirrorBase == MAP_FAILED) [[unlikely]]
            throw exception("Failed to create mirror base: {} (0x{:X} bytes)", strerror(errno), totalSize);

        size_t mirrorOffset{};
        for (const auto &region : regions) {
            // Get the host address of the region
            auto hostRegion{GetHostSpan(region)};

            if (!base.contains(hostRegion)) [[unlikely]]
                throw exception("Mapping is outside of VMM base: {} - {}", fmt::ptr(hostRegion.data()), fmt::ptr(hostRegion.end().base()));

            auto offset{static_cast<size_t>(hostRegion.data() - base.data())};
            if (!util::IsPageAligned(offset) || !util::IsPageAligned(hostRegion.size())) [[unlikely]]
                throw exception("Mapping is not aligned to a page: {} - {} (0x{:X})", fmt::ptr(hostRegion.data()), fmt::ptr(hostRegion.end().base()), offset);

            auto mirror{mremap(hostRegion.data(), 0, hostRegion.size(), MREMAP_FIXED | MREMAP_MAYMOVE, reinterpret_cast<u8 *>(mirrorBase) + mirrorOffset)};
            if (mirror == MAP_FAILED) [[unlikely]]
                throw exception("Failed to create mirror mapping at {} - {} (0x{:X}): {}", fmt::ptr(hostRegion.data()), fmt::ptr(hostRegion.end().base()), offset, strerror(errno));

            mprotect(mirror, hostRegion.size(), PROT_READ | PROT_WRITE);

            mirrorOffset += hostRegion.size();
        }

        if (mirrorOffset != totalSize) [[unlikely]]
            throw exception("Mirror size mismatch: 0x{:X} != 0x{:X}", mirrorOffset, totalSize);

        return span<u8>{reinterpret_cast<u8 *>(mirrorBase), totalSize};
    }

    bool MemoryManager::CollectSvcMemoryRange(span<u8> memory, u32 stateMask, u8 attributeMask,
                                              std::vector<std::pair<u8 *, ChunkDescriptor>> &ranges) {
        if (memory.empty()) return false;
        auto it = chunks.upper_bound(memory.data());
        if (it == chunks.begin()) return false;
        --it;
        const auto first = it->second;
        auto address = memory.data();
        size_t remaining = memory.size();
        while (remaining) {
            if (it == chunks.end() || it->first > address) return false;
            const auto &chunk = it->second;
            auto offset = static_cast<size_t>(address - it->first);
            if (offset >= chunk.size || chunk.state != first.state || chunk.permission != first.permission ||
                (chunk.state.value & stateMask) != stateMask || (chunk.attributes.value & attributeMask))
                return false;
            auto length = std::min(remaining, chunk.size - offset);
            auto desc = chunk;
            desc.size = length;
            ranges.emplace_back(address, desc);
            remaining -= length;
            address += length;
            ++it;
        }
        return true;
    }

    bool MemoryManager::SvcSetMemoryAttribute(span<u8> memory, u32 mask, u32 value) {
        constexpr u32 uncached = 0x08, permissionLocked = 0x10;
        if ((mask & ~(uncached | permissionLocked)) || (value & ~mask) ||
            ((mask & permissionLocked) != (value & permissionLocked))) return false;
        std::unique_lock lock{mutex};
        u32 stateMask = (mask & uncached ? 1U << 24 : 0) | (mask & permissionLocked ? 1U << 27 : 0);
        // Horizon ignores device sharing and settable attributes for this check.
        constexpr u8 attributeMask = static_cast<u8>(~(0x08 | 0x10 | 0x04));
        std::vector<std::pair<u8 *, ChunkDescriptor>> ranges;
        if (!CollectSvcMemoryRange(memory, stateMask, attributeMask, ranges)) return false;
        // Use snapshots: MapInternal may split, merge or erase map iterators.
        for (auto &desc : ranges) {
            desc.second.attributes.value = static_cast<u8>((desc.second.attributes.value & ~mask) | value);
            MapInternal(desc);
        }
        return true;
    }

    bool MemoryManager::SvcSetMemoryPermission(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};
        std::vector<std::pair<u8 *, ChunkDescriptor>> ranges;
        // All attributes, including PermissionLocked, must be clear, even when
        // the requested permission is identical to the current permission.
        if (!CollectSvcMemoryRange(memory, 1U << 8, 0xFF, ranges)) return false;
        for (auto &desc : ranges) {
            desc.second.permission = permission;
            MapInternal(desc);
        }
        return true;
    }

    void MemoryManager::SetRegionBorrowed(span<u8> memory, bool value) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.attributes.isBorrowed = value;
            MapInternal(desc);
        });
    }

    void MemoryManager::SetRegionCpuCaching(span<u8> memory, bool value) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.attributes.isUncached = value;
            MapInternal(desc);
        });
    }

    void MemoryManager::SetRegionPermission(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](std::pair<u8 *, ChunkDescriptor> &desc) __attribute__((always_inline)) {
            desc.second.permission = permission;
            MapInternal(desc);
        });
    }

    std::optional<std::pair<u8 *, ChunkDescriptor>> MemoryManager::GetChunk(u8 *addr) {
        std::shared_lock lock{mutex};

        if (!addressSpace.contains(addr)) [[unlikely]]
            return std::nullopt;

        auto chunkBase{chunks.lower_bound(addr)};
        if (addr < chunkBase->first)
            --chunkBase;

        return std::make_optional(*chunkBase);
    }

    __attribute__((always_inline)) void MemoryManager::MapCodeMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = memory::states::Code
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapMutableCodeMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::CodeMutable
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapStackMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::Stack,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapHeapMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::Heap
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapSharedMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = memory::states::SharedMemory,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapTransferMemory(span<u8> memory, memory::Permission permission) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = permission,
                .state = permission.raw ? memory::states::TransferMemory : memory::states::TransferMemoryIsolated,
                .isSrcMergeDisallowed = true
            }));
    }

    __attribute__((always_inline)) void MemoryManager::MapThreadLocalMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {true, true, false},
                .state = memory::states::ThreadLocal
            }));
    }

    __attribute__((always_inline)) void MemoryManager::Reserve(span<u8> memory) {
        std::unique_lock lock{mutex};

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {false, false, false},
                .state = memory::states::Reserved
            }));
    }

    __attribute__((always_inline)) void MemoryManager::UnmapMemory(span<u8> memory) {
        std::unique_lock lock{mutex};

        ForeachChunkInRange(memory, [&](const std::pair<u8 *, ChunkDescriptor> &desc) {
            if (desc.second.state != memory::states::Unmapped)
                FreeMemory(span<u8>(desc.first, desc.second.size));
        });

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            memory.data(), {
                .size = memory.size(),
                .permission = {false, false, false},
                .state = memory::states::Unmapped
            }));
    }

    __attribute__((always_inline)) void MemoryManager::FreeMemory(span<u8> memory) {
        // Get the host address of memory
        memory = GetHostSpan(memory);

        u8 *alignedStart{util::AlignUp(memory.data(), constant::PageSize)};
        u8 *alignedEnd{util::AlignDown(memory.end().base(), constant::PageSize)};

        if (alignedStart < alignedEnd) [[likely]]
            if (const int error{memoryReclaim::ReclaimSharedPages(alignedStart, static_cast<size_t>(alignedEnd - alignedStart))}; error) [[unlikely]]
                LOGE("Failed to free memory: {}", strerror(error));
    }

    bool MemoryManager::SvcMapMemory(span<u8> source, span<u8> destination) {
        std::unique_lock lock{mutex};
        std::vector<std::pair<u8 *, ChunkDescriptor>> ranges;
        if (!CollectSvcMemoryRange(source, 1U << 15, 0xFF, ranges) ||
            ranges.front().second.permission != memory::Permission{true, true, false}) return false;

        MapInternal(std::pair<u8 *, ChunkDescriptor>(
            destination.data(), {
                .isSrcMergeDisallowed = true,
                .permission = {true, true, false},
                .state = memory::states::Stack,
                .size = destination.size()
            }));

        auto sourceHost{GetHostSpan(source)};
        auto destinationHost{GetHostSpan(destination)};
        std::memcpy(destinationHost.data(), sourceHost.data(), sourceHost.size());

        for (auto &desc : ranges) {
            desc.second.permission = {false, false, false};
            desc.second.attributes.isBorrowed = true;
            MapInternal(desc);
        }
        return true;
    }

    bool MemoryManager::SvcMapTransferMemory(span<u8> memory, memory::Permission permission,
                                             ChunkDescriptor &original, const std::function<void()> &map) {
        std::unique_lock lock{mutex};
        std::vector<std::pair<u8 *, ChunkDescriptor>> ranges;
        if (!CollectSvcMemoryRange(memory, 1U << 17, 0xFF, ranges) ||
            ranges.front().second.permission != memory::Permission{true, true, false}) return false;
        original = ranges.front().second;
        original.size = memory.size();
        // Validate before replacing any host mapping, while holding the VMM lock.
        map();
        auto desc = original;
        desc.permission = permission;
        desc.attributes = memory::MemoryAttribute{1}; // Borrowed until released.
        desc.state = permission.raw ? memory::states::TransferMemory : memory::states::TransferMemoryIsolated;
        desc.isSrcMergeDisallowed = true;
        MapInternal({memory.data(), desc});
        return true;
    }

    bool MemoryManager::SvcUnmapMemory(span<u8> source, span<u8> destination) {
        std::unique_lock lock{mutex};
        std::vector<std::pair<u8 *, ChunkDescriptor>> sourceRanges, destinationRanges;
        // Validate both complete ranges before restoring any permissions. In
        // particular, a second unmap must not walk past the chunk-map sentinel.
        if (!CollectSvcMemoryRange(source, 0, 0, sourceRanges) ||
            !CollectSvcMemoryRange(destination, 0, 0, destinationRanges) ||
            sourceRanges.front().second.permission != memory::Permission{false, false, false} ||
            !sourceRanges.front().second.state.mapAllowed ||
            destinationRanges.front().second.state != memory::states::Stack ||
            destinationRanges.front().second.permission != memory::Permission{true, true, false})
            return false;

        for (const auto &desc : sourceRanges)
            if (!desc.second.attributes.isBorrowed) return false;

        // MapInternal can split or merge the map, so use snapshots instead of
        // keeping an iterator into chunks while the source is being restored.
        for (auto &desc : sourceRanges) {
            desc.second.permission = destinationRanges.front().second.permission;
            desc.second.attributes.isBorrowed = false;
            MapInternal(desc);
        }

        auto sourceHost{GetHostSpan(source)};
        auto destinationHost{GetHostSpan(destination)};
        std::memcpy(sourceHost.data(), destinationHost.data(), source.size());
        return true;
    }

    void MemoryManager::AddRef(std::shared_ptr<type::KMemory> ptr) {
        memRefs.push_back(std::move(ptr));
    }

    void MemoryManager::RemoveRef(const std::shared_ptr<type::KMemory> &ptr) {
        auto i = std::find(memRefs.begin(), memRefs.end(), ptr);

        if (*i == ptr) [[likely]]
            memRefs.erase(i);
    }

    size_t MemoryManager::GetUserMemoryUsage() {
        std::shared_lock lock{mutex};
        size_t size{};

        auto currChunk = chunks.lower_bound(heap.guest.data());

        while (currChunk->first < heap.guest.end().base()) {
            if (currChunk->second.state == memory::states::Heap)
                size += currChunk->second.size;
            ++currChunk;
        }

        return size + code.size() + state.process->mainThreadStack.size();
    }

    size_t MemoryManager::GetSystemResourceUsage() {
        std::shared_lock lock{mutex};
        constexpr size_t KMemoryBlockSize{0x40};
        return std::min(static_cast<size_t>(state.process->npdm.meta.systemResourceSize), util::AlignUp(chunks.size() * KMemoryBlockSize, constant::PageSize));
    }

    __attribute__((always_inline)) span<u8> MemoryManager::GetHostSpan(span<u8> guestSpan) const {
        return {guestSpan.data() + guestOffset, guestSpan.size()};
    }
}
