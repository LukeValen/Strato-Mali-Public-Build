// SPDX-License-Identifier: MPL-2.0
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "gpu/resolution_blit.h"
#include "gpu/resolution_scaling.h"
#include "gpu/resolution_shader.h"
#include "gpu/render_resolution_state.h"

using namespace skyline::gpu;
static std::atomic<unsigned> validationErrors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::cerr << data->pMessage << '\n';
    }
    return VK_FALSE;
}
static std::vector<uint32_t> ReadShader(const char *path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    assert(stream);
    const auto size{stream.tellg()};
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    stream.seekg(0); stream.read(reinterpret_cast<char *>(words.data()), size);
    return words;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    vk::raii::Context context;
    const char *layer{"VK_LAYER_KHRONOS_validation"};
    const std::array<const char *, 2> extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const vk::ValidationFeatureEnableEXT syncFeature{vk::ValidationFeatureEnableEXT::eSynchronizationValidation};
    const vk::ValidationFeaturesEXT validation{.enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &syncFeature};
    const vk::ApplicationInfo app{.pApplicationName = "Strato resolution integration test", .apiVersion = VK_API_VERSION_1_1};
    vk::raii::Instance instance(context, vk::InstanceCreateInfo{.pNext = &validation, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions.data()});
    vk::raii::DebugUtilsMessengerEXT debug(instance, vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
        .pfnUserCallback = Debug});
    vk::raii::PhysicalDevice physical{instance.enumeratePhysicalDevices().front()};
    const auto families{physical.getQueueFamilyProperties()};
    uint32_t family{};
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) ++family;
    const float priority{1};
    const vk::DeviceQueueCreateInfo queueInfo{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    vk::raii::Device device(physical, vk::DeviceCreateInfo{.queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo});
    vk::raii::Queue queue(device, family, 0);
    const auto memoryProperties{physical.getMemoryProperties()};
    auto memoryType{[&](uint32_t bits, vk::MemoryPropertyFlags required) {
        for (uint32_t i{}; i < memoryProperties.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & required) == required) return i;
        throw std::runtime_error("Memory type unavailable");
    }};
    struct Image {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    auto createImage{[&](vk::Extent2D extent, vk::Format format, vk::ImageAspectFlags aspects) {
        Image result;
        result.image = vk::raii::Image(device, vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, .format = format,
            .extent = {extent.width, extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                (aspects & vk::ImageAspectFlagBits::eColor ? vk::ImageUsageFlagBits::eColorAttachment : vk::ImageUsageFlagBits::eDepthStencilAttachment)});
        const auto requirements{result.image.getMemoryRequirements()};
        result.memory = vk::raii::DeviceMemory(device, vk::MemoryAllocateInfo{.allocationSize = requirements.size,
            .memoryTypeIndex = memoryType(requirements.memoryTypeBits, {})});
        result.image.bindMemory(*result.memory, 0);
        result.view = vk::raii::ImageView(device, vk::ImageViewCreateInfo{.image = *result.image,
            .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = {aspects, 0, 1, 0, 1}});
        return result;
    }};
    const auto vertexWords{ReadShader(argv[1])}, fragmentWords{ReadShader(argv[2])};
    vk::raii::ShaderModule vertex(device, vk::ShaderModuleCreateInfo{.codeSize = vertexWords.size() * 4, .pCode = vertexWords.data()});
    vk::raii::ShaderModule fragment(device, vk::ShaderModuleCreateInfo{.codeSize = fragmentWords.size() * 4, .pCode = fragmentWords.data()});
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *vertex, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *fragment, .pName = "main"}}};
    const vk::DescriptorSetLayoutBinding textureBinding{.binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment};
    vk::raii::DescriptorSetLayout descriptorLayout(device, vk::DescriptorSetLayoutCreateInfo{.bindingCount = 1, .pBindings = &textureBinding});
    const vk::DescriptorSetLayout descriptorLayoutRaw{*descriptorLayout};
    const vk::PushConstantRange scalingRange{.stageFlags = vk::ShaderStageFlagBits::eAllGraphics, .offset = 0, .size = sizeof(resolution::ShaderScale)};
    vk::raii::PipelineLayout layout(device, vk::PipelineLayoutCreateInfo{.setLayoutCount = 1, .pSetLayouts = &descriptorLayoutRaw,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &scalingRange});
    vk::raii::Sampler sampler(device, vk::SamplerCreateInfo{.magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest});
    const vk::PipelineVertexInputStateCreateInfo vertexInput{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill, .lineWidth = 1};
    const vk::PipelineMultisampleStateCreateInfo samples{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::StencilOpState stencil{.failOp = vk::StencilOp::eKeep, .passOp = vk::StencilOp::eReplace,
        .depthFailOp = vk::StencilOp::eKeep, .compareOp = vk::CompareOp::eAlways, .compareMask = 255, .writeMask = 255, .reference = 7};
    const vk::PipelineDepthStencilStateCreateInfo depthState{.depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eLess, .stencilTestEnable = true, .front = stencil, .back = stencil};
    const vk::PipelineColorBlendAttachmentState colorBlend{.colorWriteMask = vk::ColorComponentFlagBits::eR |
        vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &colorBlend};
    const std::array<vk::DynamicState, 2> dynamicStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = 2, .pDynamicStates = dynamicStates.data()};
    const vk::Format colorFormat{vk::Format::eR8G8B8A8Unorm}, depthFormat{vk::Format::eD32SfloatS8Uint};
    const auto depthAspect{vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil};
    const auto features{physical.getFormatProperties(depthFormat).optimalTilingFeatures};
    assert((features & (vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst)) ==
        (vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst));
    const std::array<vk::AttachmentDescription, 2> descriptions{{
        {.format = colorFormat, .samples = vk::SampleCountFlagBits::e1, .loadOp = vk::AttachmentLoadOp::eLoad,
         .storeOp = vk::AttachmentStoreOp::eStore, .initialLayout = vk::ImageLayout::eGeneral, .finalLayout = vk::ImageLayout::eGeneral},
        {.format = depthFormat, .samples = vk::SampleCountFlagBits::e1, .loadOp = vk::AttachmentLoadOp::eLoad,
         .storeOp = vk::AttachmentStoreOp::eStore, .stencilLoadOp = vk::AttachmentLoadOp::eLoad,
         .stencilStoreOp = vk::AttachmentStoreOp::eStore, .initialLayout = vk::ImageLayout::eGeneral, .finalLayout = vk::ImageLayout::eGeneral}}};
    const vk::AttachmentReference colorRef{0, vk::ImageLayout::eGeneral}, depthRef{1, vk::ImageLayout::eGeneral};
    const vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1, .pColorAttachments = &colorRef, .pDepthStencilAttachment = &depthRef};
    vk::raii::RenderPass renderPass(device, vk::RenderPassCreateInfo{.attachmentCount = 2, .pAttachments = descriptions.data(),
        .subpassCount = 1, .pSubpasses = &subpass});
    vk::raii::Pipeline pipeline(device, nullptr, vk::GraphicsPipelineCreateInfo{.stageCount = 2, .pStages = stages.data(),
        .pVertexInputState = &vertexInput, .pInputAssemblyState = &assembly, .pViewportState = &viewportState,
        .pRasterizationState = &raster, .pMultisampleState = &samples, .pDepthStencilState = &depthState,
        .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = *layout, .renderPass = *renderPass});
    vk::raii::CommandPool pool(device, vk::CommandPoolCreateInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = family});
    for (auto nativeExtent : {vk::Extent2D{128, 72}, vk::Extent2D{129, 73}}) {
        for (int setting{}; setting < 7; setting++) {
            const auto scale{resolution::Scale::FromSetting(setting)};
            const auto e{*scale.TryExtent({nativeExtent.width, nativeExtent.height}, {4096, 4096})};
            const vk::Extent2D scaledExtent{e.width, e.height};
            auto originalColor{createImage(nativeExtent, colorFormat, vk::ImageAspectFlagBits::eColor)};
            auto originalDepth{createImage(nativeExtent, depthFormat, depthAspect)};
            auto scaledColor{createImage(scaledExtent, colorFormat, vk::ImageAspectFlagBits::eColor)};
            auto scaledDepth{createImage(scaledExtent, depthFormat, depthAspect)};
            auto sampledNative{createImage(nativeExtent, colorFormat, vk::ImageAspectFlagBits::eColor)};
            auto sampledScaled{createImage(scaledExtent, colorFormat, vk::ImageAspectFlagBits::eColor)};
            const vk::DescriptorPoolSize poolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 2};
            vk::raii::DescriptorPool descriptors(device, vk::DescriptorPoolCreateInfo{.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &poolSize});
            const std::array<vk::DescriptorSetLayout, 2> layouts{*descriptorLayout, *descriptorLayout};
            auto sets{device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{.descriptorPool = *descriptors,
                .descriptorSetCount = 2, .pSetLayouts = layouts.data()})};
            const std::array<vk::DescriptorImageInfo, 2> textureInfos{{
                {.sampler = *sampler, .imageView = *sampledScaled.view, .imageLayout = vk::ImageLayout::eGeneral},
                {.sampler = *sampler, .imageView = *sampledNative.view, .imageLayout = vk::ImageLayout::eGeneral}}};
            for (unsigned i{}; i < 2; i++)
                device.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = *sets[i], .dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &textureInfos[i]}, {});
            const std::array<vk::ImageView, 2> views{*scaledColor.view, *scaledDepth.view};
            vk::raii::Framebuffer framebuffer(device, vk::FramebufferCreateInfo{.renderPass = *renderPass,
                .attachmentCount = 2, .pAttachments = views.data(), .width = e.width, .height = e.height, .layers = 1});
            const size_t pixels{size_t(nativeExtent.width) * nativeExtent.height};
            const size_t secondBlock{(pixels * 9 + 3) & ~size_t{3}};
            const size_t bufferSize{secondBlock + pixels * 9};
            vk::raii::Buffer buffer(device, vk::BufferCreateInfo{.size = bufferSize, .usage = vk::BufferUsageFlagBits::eTransferDst});
            const auto requirements{buffer.getMemoryRequirements()};
            vk::raii::DeviceMemory memory(device, vk::MemoryAllocateInfo{.allocationSize = requirements.size,
                .memoryTypeIndex = memoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)});
            buffer.bindMemory(*memory, 0);
            vk::raii::CommandBuffer command{std::move(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
                .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}).front())};
            command.begin(vk::CommandBufferBeginInfo{});
            for (auto image : {*sampledNative.image, *sampledScaled.image}) {
                resolution::Barrier(command, image, vk::ImageAspectFlagBits::eColor, true);
                command.clearColorImage(image, vk::ImageLayout::eGeneral, vk::ClearColorValue{std::array<float, 4>{1, 0, 0, 1}},
                    vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
                resolution::Barrier(command, image, vk::ImageAspectFlagBits::eColor);
            }
            resolution::ShaderScale shaderScale;
            shaderScale.SetTexture(0);
            shaderScale.SetTexture(127);
            shaderScale.downFactor = 1.0f / scale.Factor();
            command.pushConstants(*layout, vk::ShaderStageFlagBits::eAllGraphics, 0, vk::ArrayProxy<const resolution::ShaderScale>{shaderScale});
            command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, *sets[0], {});
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor, true);
            resolution::Barrier(command, *originalDepth.image, depthAspect, true);
            command.clearColorImage(*originalColor.image, vk::ImageLayout::eGeneral,
                vk::ClearColorValue{std::array<float, 4>{0, 0, 1, 1}}, vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            command.clearDepthStencilImage(*originalDepth.image, vk::ImageLayout::eGeneral, vk::ClearDepthStencilValue{1, 0},
                vk::ImageSubresourceRange{depthAspect, 0, 1, 0, 1});
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            resolution::Barrier(command, *scaledColor.image, vk::ImageAspectFlagBits::eColor, true);
            resolution::Barrier(command, *scaledDepth.image, depthAspect, true);
            resolution::Blit(command, *originalColor.image, nativeExtent, *scaledColor.image, scaledExtent, vk::ImageAspectFlagBits::eColor, vk::Filter::eLinear);
            resolution::Blit(command, *originalDepth.image, nativeExtent, *scaledDepth.image, scaledExtent, depthAspect, vk::Filter::eNearest);
            resolution::Barrier(command, *scaledColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *scaledDepth.image, depthAspect);
            RenderResolutionState state;
            state.viewports[0] = vk::Viewport{0, float(nativeExtent.height), float(nativeExtent.width), -float(nativeExtent.height), 0, 1};
            state.scissors[0] = vk::Rect2D{{8, 8}, {nativeExtent.width - 16, nativeExtent.height - 16}};
            command.setViewport(0, *state.viewports[0]);
            command.setScissor(0, *state.scissors[0]);
            state.Begin(command, scale.Factor(), scale.Factor(), scaledExtent);
            assert(state.Transform(*state.viewports[0]).height < 0);
            assert(state.Transform(*state.viewports[0]).minDepth == 0);
            command.beginRenderPass(vk::RenderPassBeginInfo{.renderPass = *renderPass, .framebuffer = *framebuffer,
                .renderArea = {{0, 0}, scaledExtent}}, vk::SubpassContents::eInline);
            command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
            command.draw(3, 1, 0, 0);
            command.endRenderPass();
            resolution::Barrier(command, *scaledColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *scaledDepth.image, depthAspect);
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            resolution::Blit(command, *scaledColor.image, scaledExtent, *originalColor.image, nativeExtent, vk::ImageAspectFlagBits::eColor, vk::Filter::eLinear);
            resolution::Blit(command, *scaledDepth.image, scaledExtent, *originalDepth.image, nativeExtent, depthAspect, vk::Filter::eNearest);
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            // Restore native dynamic state without another guest state update.
            state.Begin(command, 1, 1, nativeExtent);
            shaderScale.textures[0] = 0;
            shaderScale.downFactor = 1.0f;
            command.pushConstants(*layout, vk::ShaderStageFlagBits::eAllGraphics, 0, vk::ArrayProxy<const resolution::ShaderScale>{shaderScale});
            command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, *sets[1], {});
            assert(state.Transform(*state.viewports[0]) == *state.viewports[0]);
            assert(state.Transform(*state.scissors[0]) == *state.scissors[0]);
            const vk::BufferImageCopy colorCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {nativeExtent.width, nativeExtent.height, 1}};
            auto depthCopy{colorCopy}; depthCopy.bufferOffset = pixels * 4; depthCopy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
            auto stencilCopy{colorCopy}; stencilCopy.bufferOffset = pixels * 8; stencilCopy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
            command.copyImageToBuffer(*originalColor.image, vk::ImageLayout::eGeneral, *buffer, colorCopy);
            command.copyImageToBuffer(*originalDepth.image, vk::ImageLayout::eGeneral, *buffer, std::array{depthCopy, stencilCopy});
            // Draw again into the native images using restored cached dynamic state.
            // This catches stale scaled viewport/scissor state in an actual GPU draw.
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            command.clearColorImage(*originalColor.image, vk::ImageLayout::eGeneral,
                vk::ClearColorValue{std::array<float, 4>{0, 0, 1, 1}}, vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            command.clearDepthStencilImage(*originalDepth.image, vk::ImageLayout::eGeneral, vk::ClearDepthStencilValue{1, 0},
                vk::ImageSubresourceRange{depthAspect, 0, 1, 0, 1});
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            const std::array<vk::ImageView, 2> nativeViews{*originalColor.view, *originalDepth.view};
            vk::raii::Framebuffer nativeFramebuffer(device, vk::FramebufferCreateInfo{.renderPass = *renderPass,
                .attachmentCount = 2, .pAttachments = nativeViews.data(), .width = nativeExtent.width, .height = nativeExtent.height, .layers = 1});
            command.beginRenderPass(vk::RenderPassBeginInfo{.renderPass = *renderPass, .framebuffer = *nativeFramebuffer,
                .renderArea = {{0, 0}, nativeExtent}}, vk::SubpassContents::eInline);
            command.draw(3, 1, 0, 0);
            command.endRenderPass();
            resolution::Barrier(command, *originalColor.image, vk::ImageAspectFlagBits::eColor);
            resolution::Barrier(command, *originalDepth.image, depthAspect);
            auto nativeColorCopy{colorCopy}; nativeColorCopy.bufferOffset = secondBlock;
            depthCopy.bufferOffset = secondBlock + pixels * 4;
            stencilCopy.bufferOffset = secondBlock + pixels * 8;
            command.copyImageToBuffer(*originalColor.image, vk::ImageLayout::eGeneral, *buffer, nativeColorCopy);
            command.copyImageToBuffer(*originalDepth.image, vk::ImageLayout::eGeneral, *buffer, std::array{depthCopy, stencilCopy});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
                vk::BufferMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eHostRead,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = *buffer, .offset = 0, .size = VK_WHOLE_SIZE}, {});
            command.end();
            vk::raii::Fence fence(device, vk::FenceCreateInfo{});
            const vk::CommandBuffer raw{*command};
            queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &raw}, *fence);
            assert(device.waitForFences(*fence, true, UINT64_MAX) == vk::Result::eSuccess);
            const auto data{static_cast<const uint8_t *>(memory.mapMemory(0, bufferSize))};
            auto verify{[&](size_t block, unsigned x, unsigned y, bool red) {
                const size_t p{size_t(y) * nativeExtent.width + x};
                assert(data[block + p * 4] == (red ? 255 : 0));
                assert(data[block + p * 4 + 2] == (red ? 0 : 255));
                if (red) assert(std::abs(int(data[block + p * 4 + 1]) - int(x)) <= 2);
                float depth; std::memcpy(&depth, data + block + pixels * 4 + p * 4, 4);
                assert(depth == (red ? 0.25f : 1.0f));
                assert(data[block + pixels * 8 + p] == (red ? 7 : 0));
            }};
            for (size_t block : {size_t{0}, secondBlock}) {
                verify(block, 20, 40, true); verify(block, 110, 20, false); verify(block, 3, 3, false);
            }
            memory.unmapMemory();
            std::cout << "PASS: " << scale.Factor() << "x " << nativeExtent.width << 'x' << nativeExtent.height
                << " -> " << e.width << 'x' << e.height << "; shader coordinates/descriptor bits/color/depth/stencil/readback/native restore\n";
        }
    }
    device.waitIdle();
    assert(validationErrors == 0);
    std::cout << "PASS: zero Vulkan validation errors\n";
}
