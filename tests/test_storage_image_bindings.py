#!/usr/bin/env python3
"""Static regression checks for Vulkan storage image descriptor plumbing.

These checks are intentionally executable without building the Android app.
They protect the binding/counting and lifetime paths until device tests run.
"""
from pathlib import Path
import re
import unittest

GPU = Path(__file__).resolve().parents[1] / "app/src/main/cpp/skyline/gpu"


def source(path: str) -> str:
    return (GPU / path).read_text()


class StorageImageBindingTests(unittest.TestCase):
    def test_all_guest_image_formats_have_a_host_mapping(self):
        header = (GPU.parents[4] / "libraries/shader-compiler/include/shader_compiler/shader_info.h").read_text()
        image_enum = re.search(r"enum class ImageFormat\s*:\s*u32\s*\{([^}]+)\}", header)
        self.assertIsNotNone(image_enum)
        names = re.findall(r"^\s*([A-Za-z0-9_]+)\s*[,=]", image_enum.group(1), re.M)
        mapping = source("interconnect/common/textures.h")
        for name in names:
            with self.subTest(name=name):
                self.assertIn("Shader::ImageFormat::" + name, mapping)

    def test_graphics_full_and_quick_bind_writes(self):
        graphics = source("interconnect/maxwell_3d/pipeline_manager.cpp")
        self.assertIn("usage.storageImages.push_back", graphics)
        self.assertIn("for (const auto &desc : stage.storageImageDescs)", graphics)
        self.assertIn(".descriptorCount = desc.count", graphics)
        self.assertIn("cbufUsageInfo.storageImages", graphics)
        self.assertGreaterEqual(graphics.count("GetStorageImageBinding("), 2)
        self.assertIn(".dstBinding = bindingIdx++", graphics)

    def test_compute_writes_and_preserves_descriptor_offsets(self):
        compute = source("interconnect/kepler_compute/pipeline_manager.cpp")
        self.assertIn("writeImageDescs(vk::DescriptorType::eStorageImage", compute)
        self.assertIn("shaderStage.info.image_buffer_descriptors.size()", compute)
        self.assertIn("GetStorageImageBinding(", compute)

    def test_storage_views_and_image_lifetimes(self):
        path = source("interconnect/common/pipeline.inc")
        self.assertIn("vk::ComponentMapping{}", path)  # identity swizzle
        self.assertIn("AttachDependency(identityView)", path)
        self.assertIn("AttachTexture(view)", path)
        self.assertIn("vk::ImageLayout::eGeneral", path)
        self.assertIn("GetNullStorageTexture(", path)
        self.assertIn("view->GetView(true)", path)
        # fmt forwarding references cannot bind directly to bit-fields.
        self.assertIn("static_cast<u32>(handle.textureIndex)", path)
        image_views = source("texture/texture.cpp")
        self.assertIn("vk::ImageViewUsageCreateInfo", image_views)
        self.assertIn("view.storageOnly == storageOnly", image_views)

    def test_writes_and_barriers_do_not_touch_other_games(self):
        texture = source("texture/texture.cpp")
        self.assertIn("gpu.storageImageShadersSeen.load(std::memory_order_relaxed)", texture)
        self.assertIn("gpu.SupportsStorageImageFormat(format->vkFormat)", texture)
        gpu = (GPU.parents[0] / "gpu.h").read_text()
        self.assertIn("storageFormatSupportCache", gpu)
        graphics = source("interconnect/maxwell_3d/pipeline_manager.cpp")
        compute = source("interconnect/kepler_compute/pipeline_manager.cpp")
        self.assertIn("gpu.storageImageShadersSeen.store(true", graphics)
        self.assertIn("ctx.gpu.storageImageShadersSeen.store(true", compute)
        textures = source("interconnect/common/textures.cpp")
        self.assertIn("textureHeaderCache[index] = {textureHeader, nullTextureView.get()", textures)
        self.assertIn("RecordStorageWrite", texture)
        self.assertIn("pendingStorageWriteStages", texture)
        draws = source("interconnect/maxwell_3d/maxwell_3d.cpp")
        self.assertEqual(draws.count("HasWritableStorageImages()"), 2)
        self.assertEqual(draws.count("const bool hasStorageWrites{activeState.GetPipeline()->HasWritableStorageImages()}"), 2)
        self.assertEqual(draws.count("if (hasStorageWrites)"), 2)
        self.assertIn("ctx.executor.AddFullBarrier()", draws)


if __name__ == "__main__":
    unittest.main()
