#include <gpu.h>
#include "bcn_compute.h"
#include "bc_decoder.h"
#include <iostream>
#include <random>
#include <cmath>
#include <fstream>
#include <unistd.h>
namespace skyline::gpu {
    GPU::GPU() {
        const vk::ApplicationInfo application{.pApplicationName="bcn-compute-test", .apiVersion=VK_API_VERSION_1_1};
        instance = context.createInstance(vk::InstanceCreateInfo{.pApplicationInfo=&application});
        vkPhysicalDevice = std::move(instance.enumeratePhysicalDevices().front());
        const auto queues = vkPhysicalDevice.getQueueFamilyProperties();
        while (!(queues.at(vkQueueFamilyIndex).queueFlags & vk::QueueFlagBits::eCompute)) ++vkQueueFamilyIndex;
        const float priority = 1;
        const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex=vkQueueFamilyIndex, .queueCount=1, .pQueuePriorities=&priority};
        vkDevice = vkPhysicalDevice.createDevice(vk::DeviceCreateInfo{.queueCreateInfoCount=1, .pQueueCreateInfos=&queue});
    }
    std::shared_ptr<memory::StagingBuffer> memory::MemoryManager::AllocateStagingBuffer(vk::DeviceSize size, bool storage) {
        auto result = std::make_shared<StagingBuffer>();
        result->length = size;
        result->buffer = gpu.vkDevice.createBuffer(vk::BufferCreateInfo{.size=size,
            .usage=vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
                (storage ? vk::BufferUsageFlagBits::eStorageBuffer : vk::BufferUsageFlags{}), .sharingMode=vk::SharingMode::eExclusive});
        result->vkBuffer = *result->buffer;
        const auto requirements = result->buffer.getMemoryRequirements();
        const auto properties = gpu.vkPhysicalDevice.getMemoryProperties();
        u32 type = 0;
        const auto flags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        while (!(requirements.memoryTypeBits & (1U << type)) || (properties.memoryTypes[type].propertyFlags & flags) != flags) ++type;
        result->memory = gpu.vkDevice.allocateMemory(vk::MemoryAllocateInfo{.allocationSize=requirements.size, .memoryTypeIndex=type});
        result->buffer.bindMemory(*result->memory, 0);
        result->mapped = static_cast<u8*>(result->memory.mapMemory(0, size));
        return result;
    }
}
using namespace skyline;
using namespace skyline::gpu;
void CheckFormat(GPU &gpu, vk::Format format, std::mt19937 &random) {
    const bool bc6 = format == vk::Format::eBc6HUfloatBlock || format == vk::Format::eBc6HSfloatBlock;
    const size_t block = format <= vk::Format::eBc1RgbaSrgbBlock ? 8 : 16;
    std::vector<BcnComputeLevel> levels;
    size_t inputSize{}, outputSize{};
    for (auto dimensions : std::array<std::array<u32,3>,4>{{{17,9,3},{7,3,2},{2,1,4},{1,1,1}}}) {
        const auto [w,h,d] = dimensions;
        const size_t compressed = ((w+3)/4)*((h+3)/4)*d*block;
        const size_t decoded = w*h*d*(bc6 ? 8 : 4);
        levels.push_back({w,h,d,inputSize,compressed,outputSize,decoded});
        inputSize += compressed; outputSize += decoded;
    }
    std::vector<u8> input(inputSize), expected(outputSize);
    for (auto &byte : input) byte = random() & 255;
    // Cover all BC7 modes and all 14 valid BC6H modes, not only frequent modes.
    const std::array<u8,14> bc6Modes{0,1,2,6,10,14,18,22,26,30,3,7,11,15};
    for (size_t i=0; i<inputSize/block; ++i) {
        if (bc6) input[i*block] = (input[i*block] & 0xe0) | bc6Modes[i % bc6Modes.size()];
        if (format >= vk::Format::eBc7UnormBlock) {
            const unsigned mode = i % 8;
            input[i*block] = (input[i*block] & ~((1U << (mode+1))-1)) | (1U << mode);
        }
    }
    // BC7 random data includes invalid blocks; both decoders define them as black.
    for (const auto &level : levels) for (u32 slice=0; slice<level.depth; ++slice) {
        auto source = input.data()+level.inputOffset+slice*level.inputSize/level.depth;
        auto output = expected.data()+level.outputOffset+slice*level.outputSize/level.depth;
        switch(format) {
            case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbaSrgbBlock: bcn::DecodeBc1(source,output,level.width,level.height,true); break;
            case vk::Format::eBc2UnormBlock: case vk::Format::eBc2SrgbBlock: bcn::DecodeBc2(source,output,level.width,level.height); break;
            case vk::Format::eBc3UnormBlock: case vk::Format::eBc3SrgbBlock: bcn::DecodeBc3(source,output,level.width,level.height); break;
            case vk::Format::eBc6HUfloatBlock: case vk::Format::eBc6HSfloatBlock: bcn::DecodeBc6(source,output,level.width,level.height,format==vk::Format::eBc6HSfloatBlock); break;
            default: bcn::DecodeBc7(source,output,level.width,level.height); break;
        }
    }
    auto output = gpu.memory.AllocateStagingBuffer(outputSize, true);
    auto job = std::make_shared<BcnComputeJob>(gpu,format,span<const u8>{input},levels,output);
    auto pool = gpu.vkDevice.createCommandPool(vk::CommandPoolCreateInfo{.queueFamilyIndex=gpu.vkQueueFamilyIndex});
    auto commands = gpu.vkDevice.allocateCommandBuffers(vk::CommandBufferAllocateInfo{.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
    auto &command = commands.front();
    command.begin(vk::CommandBufferBeginInfo{});
    job->Record(command);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},
        vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,.dstAccessMask=vk::AccessFlagBits::eHostRead},{},{});
    command.end();
    vk::CommandBuffer raw = *command;
    auto queue = gpu.vkDevice.getQueue(gpu.vkQueueFamilyIndex,0);
    queue.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&raw},nullptr);
    queue.waitIdle();
    size_t differences{}; int maxDelta{};
    for (size_t i=0; i<outputSize; ++i) {
        int delta = std::abs(int(expected[i])-int(output->data()[i]));
        maxDelta = std::max(maxDelta,delta);
        if (delta > (bc6 || format>=vk::Format::eBc7UnormBlock ? 0 : 1)) ++differences;
    }
    if (differences) throw exception("CPU/GPU decoder mismatch " + std::to_string(static_cast<int>(format)) + " differences=" + std::to_string(differences) + " max=" + std::to_string(maxDelta));
    std::cout << vk::to_string(format) << ": passed (max byte delta " << maxDelta << ")\n";
}
int main() {
    try {
        GPU gpu;
        const auto cacheDir{std::filesystem::temp_directory_path() /
            ("strato-bcn-cache-test-" + std::to_string(getpid()))};
        gpu.bcnShaderCacheEnabled = true;
        gpu.bcnPipelineCacheDir = cacheDir;
        std::mt19937 random(0xBC01237);
        for (auto format : {vk::Format::eBc1RgbaUnormBlock,vk::Format::eBc1RgbaSrgbBlock,vk::Format::eBc2UnormBlock,vk::Format::eBc2SrgbBlock,
            vk::Format::eBc3UnormBlock,vk::Format::eBc3SrgbBlock,vk::Format::eBc6HUfloatBlock,vk::Format::eBc6HSfloatBlock,
            vk::Format::eBc7UnormBlock,vk::Format::eBc7SrgbBlock}) CheckFormat(gpu,format,random);
        if (BcnComputeJob::Supports(vk::Format::eBc4SnormBlock) || BcnComputeJob::Supports(vk::Format::eBc5UnormBlock)) return 2;
        std::vector<u8> input(8);
        bool rejected=false;
        auto output = gpu.memory.AllocateStagingBuffer(64, true);
        try { BcnComputeJob invalid(gpu,vk::Format::eBc1RgbaUnormBlock,span<const u8>{input},{{4,4,1,0,9,0,64}},output); }
        catch (const std::exception&) { rejected=true; }
        if (!rejected) return 3;
        rejected=false;
        try { BcnComputeJob invalid(gpu,vk::Format::eBc1RgbaUnormBlock,span<const u8>{input},{{4,4,1,0,8,4,64}},output); }
        catch (const BcnComputeIneligible&) { rejected=true; }
        if (!rejected) return 4;
        rejected=false;
        try { BcnComputeJob invalid(gpu,vk::Format::eBc1RgbaUnormBlock,span<const u8>{input},{{4,4,1,0,8,1,64}},output); }
        catch (const BcnComputeIneligible&) { rejected=true; }
        if (!rejected) return 5;
        std::cout << "Invalid input and output ranges rejected; RGTC stays on CPU\n";
        auto cacheFile{std::filesystem::directory_iterator(cacheDir)->path()};
        if (std::filesystem::file_size(cacheFile) < 32) return 6;
        // A damaged cache must be discarded without disabling BCn decoding.
        { std::ofstream invalid{cacheFile, std::ios::binary | std::ios::trunc}; invalid << "broken"; }
        {
            GPU retry;
            retry.bcnShaderCacheEnabled = true;
            retry.bcnPipelineCacheDir = cacheDir;
            CheckFormat(retry, vk::Format::eBc1RgbaUnormBlock, random);
        }
        if (std::filesystem::file_size(cacheFile) < 32) return 7;
        std::filesystem::remove_all(cacheDir);
        std::cout << "BCn pipeline cache survives damage and rebuilds\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
