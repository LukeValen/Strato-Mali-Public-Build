/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Application
import android.content.Context
import com.google.android.material.color.DynamicColors
import com.google.android.material.color.DynamicColorsOptions
import dagger.hilt.android.HiltAndroidApp
import org.stratoemu.strato.di.getSettings
import org.stratoemu.strato.settings.EmulationSettings
import org.stratoemu.strato.utils.GpuDriverHelper
import java.io.File

/**
 * @return The optimal directory for putting public files inside, this may return a private directory if a public directory cannot be retrieved
 */
fun Context.getPublicFilesDir() : File = getExternalFilesDir(null) ?: filesDir

@HiltAndroidApp
class StratoApplication : Application() {
    init {
        instance = this
    }

    companion object {
        lateinit var instance : StratoApplication
            private set

        val context : Context get() = instance.applicationContext
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        System.loadLibrary("skyline")

        // Keep the exact PanVK package bundled into this APK installed on the
        // target G615. Auto-upgrade the selection only while the user is still
        // following our previous automatic choice; manual driver choices win.
        val bootstrapPrefs = getSharedPreferences("strato_mali_bootstrap", MODE_PRIVATE)
        if (GpuDriverHelper.isMaliDevice() &&
            GpuDriverHelper.getSystemGpuName()?.contains("Mali-G615", ignoreCase = true) == true) {
            GpuDriverHelper.ensureBundledPanVk(this)?.let { bundledLabel ->
                val previousAutoLabel = bootstrapPrefs.getString("panvk_auto_label", null)
                val currentLabel = EmulationSettings.global.gpuDriver

                if (currentLabel == bundledLabel) {
                    bootstrapPrefs.edit().putString("panvk_auto_label", bundledLabel).apply()
                } else if (previousAutoLabel == null || currentLabel == previousAutoLabel) {
                    EmulationSettings.global.gpuDriver = bundledLabel
                    bootstrapPrefs.edit().putString("panvk_auto_label", bundledLabel).apply()
                }
            }
        }

        val dynamicColorsOptions = DynamicColorsOptions.Builder().setPrecondition { _, _ -> getSettings().useMaterialYou }.build()
        DynamicColors.applyToActivitiesIfAvailable(this, dynamicColorsOptions)
    }
}
