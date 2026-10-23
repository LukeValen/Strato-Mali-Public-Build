// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <android/native_window_jni.h>
#include <common/settings.h>
#include <common/signal.h>
#include <jvm.h>
#include <gpu.h>
#include <soc.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include "presentation_engine.h"
#include "native_window.h"
#include "texture/format.h"

extern jint Fps;
extern jfloat AverageFrametimeMs;
extern jfloat AverageFrametimeDeviationMs;

namespace skyline::gpu {
    using namespace service::hosbinder;

    PresentationEngine::PresentationEngine(const DeviceState &state, GPU &gpu)
        : state{state},
          gpu{gpu},
          presentSemaphores{util::MakeFilledArray<vk::raii::Semaphore, MaxSwapchainImageCount>(gpu.vkDevice, vk::SemaphoreCreateInfo{})},
          acquireSemaphores{util::MakeFilledArray<vk::raii::Semaphore, MaxSwapchainImageCount>(gpu.vkDevice, vk::SemaphoreCreateInfo{})},
          presentationTrack{static_cast<u64>(trace::TrackIds::Presentation), perfetto::ProcessTrack::Current()},
          personaFastPresent{state.loader && state.loader->programId == 0x01005CA01580E000ULL},
          vsyncEvent{std::make_shared<kernel::type::KEvent>(state, true)},
          vsyncThread{&PresentationEngine::VsyncThread, this},
          presentationThread{&PresentationEngine::PresentationThread, this} {
        auto desc{presentationTrack.Serialize()};
        desc.set_name("Presentation");
        perfetto::TrackEvent::SetTrackDescriptor(presentationTrack, desc);
    }

    PresentationEngine::~PresentationEngine() {
        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr))
            env->DeleteGlobalRef(jSurface);

        {
            std::scoped_lock lock{vsyncMutex};
            vsyncStop = true;
        }
        vsyncCondition.notify_all();
        if (vsyncThread.joinable())
            vsyncThread.join();
    }

    void PresentationEngine::VsyncThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-VSync")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            std::unique_lock lock{vsyncMutex};
            auto deadline{GuestFrameClock::Clock::now() + GuestFrameClock::VsyncPeriod};
            while (!vsyncCondition.wait_until(lock, deadline, [this] { return vsyncStop; })) {
                lock.unlock();
                vsyncEvent->Signal();
                lock.lock();
                deadline = GuestFrameClock::NextVsync(deadline, GuestFrameClock::Clock::now());
            }
        } catch (const signal::SignalException &e) {
            LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        }
    }

    void PresentationEngine::PresentFrame(const PresentableFrame &frame) {
        using Clock = std::chrono::steady_clock;
        auto mark = personaFastPresent ? Clock::now() : Clock::time_point{};
        const auto elapsedUs = [&mark, this]() -> i64 {
            if (!personaFastPresent) return 0;
            const auto now = Clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - mark).count();
            mark = now;
            return elapsed;
        };
        std::unique_lock lock(mutex);
        surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });

        frame.fence.Wait(state.soc->host1x);
        if (personaFastPresent) personaStats.hostFenceUs += elapsedUs();

        std::scoped_lock textureLock(*frame.textureView);

        auto texture{frame.textureView->texture};
        if (frame.textureView->format != swapchainFormat || texture->dimensions != swapchainExtent)
            UpdateSwapchain(frame.textureView->format, texture->dimensions);

        int result;
        if (frame.crop && frame.crop != windowCrop) {
            if ((result = window->perform(window, NATIVE_WINDOW_SET_CROP, &frame.crop)))
                throw exception("Setting the layer crop to ({}-{})x({}-{}) failed with {}", frame.crop.left, frame.crop.right, frame.crop.top, frame.crop.bottom, result);
            windowCrop = frame.crop;
        }

        if (frame.scalingMode != NativeWindowScalingMode::Freeze && windowScalingMode != frame.scalingMode) {
            if ((result = window->perform(window, NATIVE_WINDOW_SET_SCALING_MODE, static_cast<i32>(frame.scalingMode))))
                throw exception("Setting the layer scaling mode to '{}' failed with {}", ToString(frame.scalingMode), result);
            windowScalingMode = frame.scalingMode;
        }

        if (!personaFastPresent || frame.transform != windowTransform) {
            if ((result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TRANSFORM, static_cast<i32>(frame.transform))))
                throw exception("Setting the buffer transform to '{}' failed with {}", ToString(frame.transform), result);
        }
        windowTransform = frame.transform;

        auto &acquireSemaphore{acquireSemaphores[frameIndex]};
        auto &frameFence{frameFences[frameIndex]};
        if (frameFence)
            frameFence->Wait();
        if (personaFastPresent) personaStats.frameFenceUs += elapsedUs();

        frameIndex = (frameIndex + 1) % swapchainImageCount;

        std::pair<vk::Result, u32> nextImage;
        while (nextImage = vkSwapchain->acquireNextImage(std::numeric_limits<u64>::max(), *acquireSemaphore, {}), nextImage.first != vk::Result::eSuccess) [[unlikely]] {
            if (nextImage.first == vk::Result::eSuboptimalKHR)
                surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });
            else
                throw exception("vkAcquireNextImageKHR returned an unhandled result '{}'", vk::to_string(nextImage.first));
        }
        if (personaFastPresent) personaStats.acquireUs += elapsedUs();

        auto &nextImageTexture{images.at(nextImage.second)};
        auto &presentSemaphore{presentSemaphores[nextImage.second]};

        texture->SynchronizeHost();
        nextImageTexture->CopyFrom(texture, *acquireSemaphore, *presentSemaphore, swapchainFormat, vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        });
        if (personaFastPresent) personaStats.copyUs += elapsedUs();

        frameFence = nextImageTexture->cycle;

        auto getMonotonicNsNow{[]() -> i64 {
            timespec time;
            if (clock_gettime(CLOCK_MONOTONIC, &time))
                throw exception("Failed to clock_gettime with '{}'", strerror(errno));
            return (time.tv_sec * constant::NsInSecond) + time.tv_nsec;
        }};

        // Pace actual submissions as well as guest VSync. Native-window timestamps
        // alone do not throttle buffer release consistently across Vulkan drivers.
        // The producer supplies the game's interval (1 = 60 FPS, 2 = 30 FPS).
        std::this_thread::sleep_until(guestFrameClock.SchedulePresent(
            GuestFrameClock::Clock::now(), static_cast<u32>(frame.swapInterval),
            !*state.settings->disableFrameThrottling));
        if (personaFastPresent) personaStats.throttleUs += elapsedUs();

        i64 timestamp{frame.timestamp};
        if (timestamp) {
            // If the timestamp is specified, we need to convert it from the util::GetTimeNs base to the CLOCK_MONOTONIC one
            // We do so by getting an offset from the current time in nanoseconds and then adding it to the current time in CLOCK_MONOTONIC
            // Note: It's important we do this right before present as going past the timestamp could lead to fewer Binder IPC calls
            i64 current{util::GetTimeNs()};
            if (current < timestamp) {
                timestamp = getMonotonicNsNow() + (timestamp - current);
            } else {
                timestamp = 0;
            }
        }

        i64 lastTimestamp{std::exchange(windowLastTimestamp, timestamp)};
        if (!timestamp && lastTimestamp)
            // We need to nullify the timestamp if it transitioned from being specified (non-zero) to unspecified (zero)
            timestamp = NativeWindowTimestampAuto;

        if (timestamp && (result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP, timestamp)))
            throw exception("Setting the buffer timestamp to {} failed with {}", timestamp, result);

        // The value was discarded. Avoid one native-window round trip per
        // Persona frame; retain the original path for every other title.
        if (!personaFastPresent) {
            u64 frameId{};
            if ((result = window->perform(window, NATIVE_WINDOW_GET_NEXT_FRAME_ID, &frameId)))
                throw exception("Retrieving the next frame's ID failed with {}", result);
        }

        {
            std::scoped_lock queueLock{gpu.queueMutex};
            std::ignore = gpu.vkQueue.presentKHR(vk::PresentInfoKHR{
                .swapchainCount = 1,
                .pSwapchains = &**vkSwapchain,
                .pImageIndices = &nextImage.second,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &*presentSemaphore,
            }); // We don't care about suboptimal images as they are caused by not respecting the transform hint, we handle transformations externally
        }
        if (personaFastPresent) personaStats.presentUs += elapsedUs();

        if (personaFastPresent) {
            auto &stats = personaStats;
            const auto now = Clock::now();
            if (!stats.frames) stats.start = now;
            ++stats.frames;
            if (frame.swapInterval == 1) ++stats.swapIntervalOne;
            else if (frame.swapInterval == 2) ++stats.swapIntervalTwo;
            else ++stats.otherIntervals;
            const auto seconds = std::chrono::duration<double>(now - stats.start).count();
            if (seconds >= 5.0) {
                const auto msPerFrame = [&stats](i64 us) { return static_cast<double>(us) / stats.frames / 1000.0; };
                LOGI("PERSONA_PERF fps={:.1f} frames={} interval_1={} interval_2={} other={} hostFenceMs={:.2f} frameFenceMs={:.2f} acquireMs={:.2f} copySubmitMs={:.2f} throttleMs={:.2f} presentMs={:.2f}",
                     stats.frames / seconds, stats.frames, stats.swapIntervalOne, stats.swapIntervalTwo, stats.otherIntervals,
                     msPerFrame(stats.hostFenceUs), msPerFrame(stats.frameFenceUs), msPerFrame(stats.acquireUs),
                     msPerFrame(stats.copyUs), msPerFrame(stats.throttleUs), msPerFrame(stats.presentUs));
                stats = {};
            }
        }

        timestamp = (timestamp && !*state.settings->disableFrameThrottling) ? timestamp : getMonotonicNsNow(); // We tie FPS to the submission time rather than presentation timestamp, if we don't have the presentation timestamp available or if frame throttling is disabled as we want the maximum measured FPS to not be restricted to the refresh rate
        if (frameTimestamp) {
            i64 sampleWeight{Fps ? Fps : 1}; //!< The weight of each sample in calculating the average, we want to roughly average the past second

            auto weightedAverage{[](auto weight, auto previousAverage, auto current) {
                return (((weight - 1) * previousAverage) + current) / weight;
            }}; //!< Modified moving average (https://en.wikipedia.org/wiki/Moving_average#Modified_moving_average)

            i64 currentFrametime{timestamp - frameTimestamp};
            averageFrametimeNs = weightedAverage(sampleWeight, averageFrametimeNs, currentFrametime);
            AverageFrametimeMs = static_cast<jfloat>(averageFrametimeNs) / constant::NsInMillisecond;

            i64 currentFrametimeDeviation{std::abs(averageFrametimeNs - currentFrametime)};
            averageFrametimeDeviationNs = weightedAverage(sampleWeight, averageFrametimeDeviationNs, currentFrametimeDeviation);
            AverageFrametimeDeviationMs = static_cast<jfloat>(averageFrametimeDeviationNs) / constant::NsInMillisecond;

            Fps = static_cast<jint>(std::round(static_cast<float>(constant::NsInSecond) / static_cast<float>(averageFrametimeNs)));

            TRACE_EVENT_INSTANT("gpu", "Present", presentationTrack, "FrameTimeNs", timestamp - frameTimestamp, "Fps", Fps);

            frameTimestamp = timestamp;
        } else {
            frameTimestamp = timestamp;
        }
    }

    void PresentationEngine::PresentationThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-Present")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            presentQueue.Process([this](const PresentableFrame &frame) {
                PresentFrame(frame);
                frame.presentCallback(); // We're calling the callback here as it's outside of all the locks in PresentFrame
            }, [] {});
        } catch (const signal::SignalException &e) {
            LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        }
    }

    NativeWindowTransform GetAndroidTransform(vk::SurfaceTransformFlagBitsKHR transform) {
        using NativeWindowTransform = NativeWindowTransform;
        switch (transform) {
            case vk::SurfaceTransformFlagBitsKHR::eIdentity:
            case vk::SurfaceTransformFlagBitsKHR::eInherit:
                return NativeWindowTransform::Identity;
            case vk::SurfaceTransformFlagBitsKHR::eRotate90:
                return NativeWindowTransform::Rotate90;
            case vk::SurfaceTransformFlagBitsKHR::eRotate180:
                return NativeWindowTransform::Rotate180;
            case vk::SurfaceTransformFlagBitsKHR::eRotate270:
                return NativeWindowTransform::Rotate270;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirror:
                return NativeWindowTransform::MirrorHorizontal;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate90:
                return NativeWindowTransform::MirrorHorizontalRotate90;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate180:
                return NativeWindowTransform::MirrorVertical;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate270:
                return NativeWindowTransform::MirrorVerticalRotate90;
        }
    }

    void PresentationEngine::UpdateSwapchain(texture::Format format, texture::Dimensions extent) {
        auto minImageCount{std::max(vkSurfaceCapabilities.minImageCount, *state.settings->forceTripleBuffering ? 3U : 2U)};
        if (minImageCount > MaxSwapchainImageCount)
            throw exception("Requesting swapchain with higher image count ({}) than maximum slot count ({})", minImageCount, MaxSwapchainImageCount);

        const auto &capabilities{vkSurfaceCapabilities};
        if (minImageCount < capabilities.minImageCount || (capabilities.maxImageCount && minImageCount > capabilities.maxImageCount))
            throw exception("Cannot update swapchain to accomodate image count: {} ({}-{})", minImageCount, capabilities.minImageCount, capabilities.maxImageCount);
        else if (capabilities.minImageExtent.height > extent.height || capabilities.minImageExtent.width > extent.width || capabilities.maxImageExtent.height < extent.height || capabilities.maxImageExtent.width < extent.width)
            throw exception("Cannot update swapchain to accomodate image extent: {}x{} ({}x{}-{}x{})", extent.width, extent.height, capabilities.minImageExtent.width, capabilities.minImageExtent.height, capabilities.maxImageExtent.width, capabilities.maxImageExtent.height);

        vk::Format vkFormat{*format};
        texture::Format underlyingFormat{format};
        if (swapchainFormat != format) {
            auto formats{gpu.vkPhysicalDevice.getSurfaceFormatsKHR(**vkSurface)};
            if (std::find(formats.begin(), formats.end(), vk::SurfaceFormatKHR{vkFormat, vk::ColorSpaceKHR::eSrgbNonlinear}) == formats.end()) {
                LOGD("Surface doesn't support requested image format '{}' with colorspace '{}'", vk::to_string(vkFormat), vk::to_string(vk::ColorSpaceKHR::eSrgbNonlinear));
                underlyingFormat = format::R8G8B8A8Unorm;
            }
        }

        constexpr vk::ImageUsageFlags presentUsage{vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
        if ((capabilities.supportedUsageFlags & presentUsage) != presentUsage)
            throw exception("Swapchain doesn't support image usage '{}': {}", vk::to_string(presentUsage), vk::to_string(capabilities.supportedUsageFlags));

        auto requestedMode{*state.settings->disableFrameThrottling ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo};
        auto modes{gpu.vkPhysicalDevice.getSurfacePresentModesKHR(**vkSurface)};
        if (std::find(modes.begin(), modes.end(), requestedMode) == modes.end())
            throw exception("Swapchain doesn't support present mode: {}", vk::to_string(requestedMode));

        vkSwapchain.emplace(gpu.vkDevice, vk::SwapchainCreateInfoKHR{
            .surface = **vkSurface,
            .minImageCount = minImageCount,
            .imageFormat = *underlyingFormat,
            .imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = presentUsage,
            .imageSharingMode = vk::SharingMode::eExclusive,
            .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eInherit,
            .presentMode = requestedMode,
            .clipped = true,
        });

        auto vkImages{vkSwapchain->getImages()};
        if (vkImages.size() > MaxSwapchainImageCount)
            throw exception("Swapchain has higher image count ({}) than maximum slot count ({})", minImageCount, MaxSwapchainImageCount);

        for (size_t index{}; index < vkImages.size(); index++) {
            auto &slot{images[index]};
            slot = std::make_shared<Texture>(*state.gpu, vkImages[index], extent, underlyingFormat, vk::ImageLayout::eUndefined, vk::ImageTiling::eOptimal, vk::ImageCreateFlags{}, presentUsage);
            slot->TransitionLayout(vk::ImageLayout::ePresentSrcKHR);
        }
        for (size_t index{vkImages.size()}; index < MaxSwapchainImageCount; index++)
            // We need to clear all the slots which aren't filled, keeping around stale slots could lead to issues
            images[index] = {};

        swapchainFormat = format;
        swapchainExtent = extent;
        swapchainImageCount = vkImages.size();
    }

    void PresentationEngine::UpdateSurface(jobject newSurface) {
        std::scoped_lock guard{mutex};

        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr)) {
            env->DeleteGlobalRef(jSurface);
            jSurface = nullptr;
        }
        if (!env->IsSameObject(newSurface, nullptr))
            jSurface = env->NewGlobalRef(newSurface);

        vkSwapchain.reset();

        if (jSurface) {
            window = ANativeWindow_fromSurface(env, jSurface);
            vkSurface.emplace(gpu.vkInstance, vk::AndroidSurfaceCreateInfoKHR{
                .window = window,
            });
            if (!gpu.vkPhysicalDevice.getSurfaceSupportKHR(gpu.vkQueueFamilyIndex, **vkSurface))
                throw exception("Vulkan Queue doesn't support presentation with surface");
            vkSurfaceCapabilities = gpu.vkPhysicalDevice.getSurfaceCapabilitiesKHR(**vkSurface);

            if (swapchainExtent && swapchainFormat)
                UpdateSwapchain(swapchainFormat, swapchainExtent);

            if (window->common.magic != AndroidNativeWindowMagic)
                throw exception("ANativeWindow* has unexpected magic: {} instead of {}", span(&window->common.magic, 1).as_string(true), span<const u8>(reinterpret_cast<const u8 *>(&AndroidNativeWindowMagic), sizeof(u32)).as_string(true));
            if (window->common.version != sizeof(ANativeWindow))
                throw exception("ANativeWindow* has unexpected version: {} instead of {}", window->common.version, sizeof(ANativeWindow));

            int result;
            if (windowCrop && (result = window->perform(window, NATIVE_WINDOW_SET_CROP, &windowCrop)))
                throw exception("Setting the layer crop to ({}-{})x({}-{}) failed with {}", windowCrop.left, windowCrop.right, windowCrop.top, windowCrop.bottom, result);

            if (windowScalingMode != NativeWindowScalingMode::ScaleToWindow && (result = window->perform(window, NATIVE_WINDOW_SET_SCALING_MODE, static_cast<i32>(windowScalingMode))))
                throw exception("Setting the layer scaling mode to '{}' failed with {}", ToString(windowScalingMode), result);

            if (windowTransform != NativeWindowTransform::Identity && (result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TRANSFORM, static_cast<i32>(windowTransform))))
                throw exception("Setting the buffer transform to '{}' failed with {}", ToString(windowTransform), result);

            if ((result = window->perform(window, NATIVE_WINDOW_ENABLE_FRAME_TIMESTAMPS, true)))
                throw exception("Enabling frame timestamps failed with {}", result);

            surfaceCondition.notify_all();
        } else {
            vkSurface.reset();
            window = nullptr;
        }
    }

    u64 PresentationEngine::Present(const std::shared_ptr<TextureView> &texture, i64 timestamp, i64 swapInterval, AndroidRect crop, NativeWindowScalingMode scalingMode, NativeWindowTransform transform, skyline::service::hosbinder::AndroidFence fence, const std::function<void()> &presentCallback) {
        if (!vkSurface.has_value()) {
            // We want this function to generally (not necessarily always) block when a surface is not present to implicitly pause the game
            std::unique_lock lock{mutex};
            surfaceCondition.wait(lock, [this] { return vkSurface.has_value(); });
        }

        presentQueue.Push(PresentableFrame{
            texture,
            fence,
            timestamp,
            swapInterval,
            presentCallback,
            nextFrameId,
            crop,
            scalingMode,
            transform
        });

        return nextFrameId++;
    }

    NativeWindowTransform PresentationEngine::GetTransformHint() {
        if (!vkSurface.has_value()) {
            std::unique_lock lock{mutex};
            surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });
        }

        return GetAndroidTransform(vkSurfaceCapabilities.currentTransform);
    }
}
