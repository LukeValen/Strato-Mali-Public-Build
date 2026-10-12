/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.utils

import android.content.Context
import android.os.Build
import android.util.Log
import org.stratoemu.strato.R
import org.stratoemu.strato.data.GpuDriverMetadata
import org.stratoemu.strato.getPublicFilesDir
import kotlinx.serialization.SerializationException
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream

private const val GPU_DRIVER_DIRECTORY = "gpu_drivers"
private const val GPU_DRIVER_FILE_REDIRECT_DIR = "gpu/vk_file_redirect"
private const val GPU_DRIVER_INSTALL_TEMP_DIR = "driver_temp"
private const val GPU_DRIVER_META_FILE = "meta.json"
private const val BUNDLED_PANVK_ASSET = "gpu_drivers/PanVK-G615-Strato-Mali-latest.zip"
private const val TAG = "GPUDriverHelper"

interface GpuDriverHelper {
    companion object {
        /**
         * Returns information about the system GPU driver.
         * @return Driver ID, driver version and GPU device name, or `null` if an error occurred.
         */
        private external fun getSystemDriverInfo() : Array<String>?

        /**
         * Queries the driver for custom driver loading support.
         * @return `true` if the device supports loading custom drivers, `false` otherwise
         */
        external fun supportsCustomDriverLoading() : Boolean

        /**
         * Returns true when the Mali Kbase device node is present.
         */
        external fun isMaliDevice() : Boolean

        /**
         * Queries the driver for manual max clock forcing support
         */
        external fun supportsForceMaxGpuClocks() : Boolean

        /**
         * Calls into the driver to force the GPU to run at the maximum possible clock speed
         * @param force Whether to enable or disable the forced clocks
         */
        external fun forceMaxGpuClocks(enable : Boolean)

        private const val BUNDLED_PANVK_NAME = "PanVK G615 Strato Mali"

        fun getSystemGpuName() : String? = getSystemDriverInfo()?.getOrNull(2)

        private fun getBundledPanVkMetadata(context : Context) : GpuDriverMetadata? {
            try {
                context.assets.open(BUNDLED_PANVK_ASSET).use { stream ->
                    ZipInputStream(stream).use { zip ->
                        var entry = zip.nextEntry
                        while (entry != null) {
                            if (!entry.isDirectory && entry.name == GPU_DRIVER_META_FILE) {
                                val metadataText = zip.readBytes().toString(Charsets.UTF_8)
                                return GpuDriverMetadata.deserialize(metadataText)
                            }
                            entry = zip.nextEntry
                        }
                    }
                }
            } catch (e : Exception) {
                Log.w(TAG, "Failed to read bundled PanVK metadata: " + e.message)
            }
            return null
        }

        /**
         * Ensures the exact PanVK version bundled into this APK is installed
         * and returns its driver label. Older PanVK packages remain installed
         * so a user can manually roll back if a newer build regresses.
         */
        fun ensureBundledPanVk(context : Context) : String? {
            val bundledMetadata = getBundledPanVkMetadata(context) ?: return null
            if (bundledMetadata.name != BUNDLED_PANVK_NAME) {
                Log.w(TAG, "Unexpected bundled Vulkan driver: " + bundledMetadata.name)
                return null
            }

            val exactLabel = bundledMetadata.label
            if (getInstalledDrivers(context).values.any { it.label == exactLabel })
                return exactLabel

            val result = try {
                context.assets.open(BUNDLED_PANVK_ASSET).use { installDriver(context, it) }
            } catch (e : IOException) {
                Log.w(TAG, "Bundled PanVK package unavailable: " + e.message)
                return null
            }

            return when (result) {
                GpuDriverInstallResult.Success, GpuDriverInstallResult.AlreadyInstalled ->
                    if (getInstalledDrivers(context).values.any { it.label == exactLabel }) exactLabel else null
                else -> null
            }
        }

        /**
         * Returns the list of installed gpu drivers.
         * @return A map from the folder the driver is installed to the metadata of the driver
         */
        fun getInstalledDrivers(context : Context) : Map<File, GpuDriverMetadata> {
            val gpuDriverDir = getDriversDirectory(context)

            // A map between the driver location and its metadata
            val driverMap = mutableMapOf<File, GpuDriverMetadata>()

            gpuDriverDir.listFiles()?.forEach { entry ->
                // Delete any files that aren't a directory
                if (!entry.isDirectory) {
                    entry.delete()
                    return@forEach
                }

                val metadataFile = File(entry.canonicalPath, GPU_DRIVER_META_FILE)
                // Delete entries without metadata
                if (!metadataFile.exists()) {
                    entry.delete()
                    return@forEach
                }

                try {
                    driverMap[entry] = GpuDriverMetadata.deserialize(metadataFile)
                } catch (e : SerializationException) {
                    Log.w(TAG, "Failed to load gpu driver metadata for ${entry.name}, skipping\n${e.message}")
                }
            }

            return driverMap
        }

        /**
         * Fetches metadata about the system driver.
         * @return A [GpuDriverMetadata] object containing data about the system driver
         */
        fun getSystemDriverMetadata(context : Context) : GpuDriverMetadata {
            val systemDriverInfo = getSystemDriverInfo()

            return GpuDriverMetadata(
                name = context.getString(R.string.system_driver),
                author = "",
                packageVersion = "",
                vendor = systemDriverInfo?.get(0) ?: "",
                driverVersion = systemDriverInfo?.get(1) ?: "",
                minApi = 0,
                description = context.getString(R.string.system_driver_desc),
                libraryName = ""
            )
        }

        /**
         * Installs the given driver to the emulator's drivers directory.
         * @param stream The input stream to read the driver from
         * @return The exit status of the installation process
         */
        fun installDriver(context : Context, stream : InputStream) : GpuDriverInstallResult {
            val installTempDir = File(context.cacheDir.canonicalPath, GPU_DRIVER_INSTALL_TEMP_DIR).apply {
                deleteRecursively()
            }

            try {
                ZipUtils.unzip(stream, installTempDir)
            } catch (e : Exception) {
                e.printStackTrace()
                installTempDir.deleteRecursively()
                return GpuDriverInstallResult.InvalidArchive
            }

            return installUnpackedDriver(context, installTempDir)
        }

        /**
         * Installs the given driver to the emulator's drivers directory.
         * @param file The file to read the driver from
         * @return The exit status of the installation process
         */
        fun installDriver(context : Context, file : File) : GpuDriverInstallResult {
            val installTempDir = File(context.cacheDir.canonicalPath, GPU_DRIVER_INSTALL_TEMP_DIR).apply {
                deleteRecursively()
            }

            try {
                ZipUtils.unzip(file, installTempDir)
            } catch (e : Exception) {
                e.printStackTrace()
                installTempDir.deleteRecursively()
                return GpuDriverInstallResult.InvalidArchive
            }

            return installUnpackedDriver(context, installTempDir)
        }

        /**
         * Installs the given unpacked driver to the emulator's drivers directory.
         * @param unpackDir The location of the unpacked driver
         * @return The exit status of the installation process
         */
        private fun installUnpackedDriver(context : Context, unpackDir : File) : GpuDriverInstallResult {
            val cleanup = {
                unpackDir.deleteRecursively()
            }

            // Check that the metadata file exists
            val metadataFile = File(unpackDir, GPU_DRIVER_META_FILE)
            if (!metadataFile.isFile) {
                cleanup()
                return GpuDriverInstallResult.MissingMetadata
            }

            // Check that the driver metadata is valid
            val driverMetadata = try {
                GpuDriverMetadata.deserialize(metadataFile)
            } catch (e : SerializationException) {
                cleanup()
                return GpuDriverInstallResult.InvalidMetadata
            }

            // Check that the device satisfies the driver's minimum Android version requirements
            if (Build.VERSION.SDK_INT < driverMetadata.minApi) {
                cleanup()
                return GpuDriverInstallResult.UnsupportedAndroidVersion
            }

            // Check that the driver is not already installed
            val installedDrivers = getInstalledDrivers(context)
            val finalInstallDir = File(getDriversDirectory(context), driverMetadata.label)
            if (installedDrivers[finalInstallDir] != null) {
                cleanup()
                return GpuDriverInstallResult.AlreadyInstalled
            }

            // Move the driver files to the final location
            if (!unpackDir.renameTo(finalInstallDir)) {
                cleanup()
                throw IOException("Failed to create directory ${finalInstallDir.name}")
            }

            return GpuDriverInstallResult.Success
        }

        /**
         * Retrieves the library name from the driver metadata for the given driver.
         */
        fun getLibraryName(context : Context, driverLabel : String) : String {
            val driverDir = File(getDriversDirectory(context), driverLabel)
            val metadataFile = File(driverDir, GPU_DRIVER_META_FILE)
            return try {
                GpuDriverMetadata.deserialize(metadataFile).libraryName
            } catch (e : SerializationException) {
                Log.w(TAG, "Failed to load library name for driver ${driverLabel}, driver may not exist or have invalid metadata")
                ""
            }
        }

        fun ensureFileRedirectDir(context : Context) {
            File(context.getPublicFilesDir(), GPU_DRIVER_FILE_REDIRECT_DIR).apply {
                if (!isDirectory) {
                    delete()
                    mkdirs()
                }
            }
        }

        /**
         * Returns the drivers' folder from
         */
        private fun getDriversDirectory(context : Context) = File(context.filesDir.canonicalPath, GPU_DRIVER_DIRECTORY).apply {
            // Create the directory if it doesn't exist
            if (!isDirectory) {
                delete()
                mkdirs()
            }
        }
    }
}

enum class GpuDriverInstallResult {
    Success,
    InvalidArchive,
    MissingMetadata,
    InvalidMetadata,
    UnsupportedAndroidVersion,
    AlreadyInstalled,
}
