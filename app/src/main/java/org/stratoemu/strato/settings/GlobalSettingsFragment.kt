/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.settings

import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.forEach
import androidx.preference.SwitchPreferenceCompat
import androidx.preference.TwoStatePreference
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import org.stratoemu.strato.BuildConfig
import org.stratoemu.strato.MainActivity
import org.stratoemu.strato.R
import org.stratoemu.strato.utils.GpuDriverHelper
import org.stratoemu.strato.utils.WindowInsetsHelper
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * This fragment is used to display the global preferences
 */
class GlobalSettingsFragment : PreferenceFragmentCompat() {
    private var section = "graphics"
    private var searchQuery = ""
    private val availability = mutableMapOf<String, Boolean>()
    private val graphicsKeys = setOf("perf_stats", "max_refresh_rate", "orientation", "aspect_ratio", "resolution_scale",
        "respect_display_cutout", "enable_foldable_layout", "show_pause_button", "disable_shader_cache")
    private val folderKeys = setOf("search_location", "filter_invalid_files")
    private val gpuKeys = setOf("force_triple_buffering", "disable_frame_throttling", "executor_slot_count_scale",
        "executor_flush_threshold", "use_direct_memory_import", "force_max_gpu_clocks", "free_guest_texture_memory",
        "enable_fast_gpu_readback_hack", "enable_fast_readback_writes", "disable_subgroup_shuffle", "validation_layer")

    fun showSection(value: String, query: String = searchQuery) {
        section = value
        searchQuery = query
        if (preferenceScreen == null) return
        val queries = query.split(",").map { it.trim() }.filter { it.isNotEmpty() }
        preferenceScreen.forEach { group ->
            val category = group as? PreferenceCategory ?: return@forEach
            val categoryMatches = queries.any { category.title?.contains(it, true) == true }
            var visible = false
            category.forEach { preference ->
                val key = preference.key ?: ""
                val owner = when {
                    key == "bcn_gpu_decode" || key == "bcn_shader_cache" -> "bcn"
                    key == "gpu_driver" -> "drivers"
                    key in folderKeys -> "folders"
                    key in graphicsKeys -> "graphics"
                    key in gpuKeys -> "gpu"
                    else -> "general"
                }
                val matches = queries.isEmpty() || categoryMatches || queries.any {
                    preference.title?.contains(it, true) == true || preference.summary?.contains(it, true) == true
                }
                preference.isVisible = owner == section && availability[key] != false && matches
                visible = visible || preference.isVisible
            }
            category.isVisible = visible
        }
    }

    override fun onViewCreated(view : View, savedInstanceState : Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val recyclerView = view.findViewById<View>(androidx.preference.R.id.recycler_view)
        WindowInsetsHelper.setPadding(recyclerView, bottom = true)
        (recyclerView as? androidx.recyclerview.widget.RecyclerView)?.apply {
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            addItemDecoration(MaliPreferenceCards(requireContext()))
        }
    }

    /**
     * This constructs the preferences from XML preference resources
     */
    override fun onCreatePreferences(savedInstanceState : Bundle?, rootKey : String?) {
        addPreferencesFromResource(R.xml.app_preferences)
        addPreferencesFromResource(R.xml.emulation_preferences)
        addPreferencesFromResource(R.xml.input_preferences)
        addPreferencesFromResource(R.xml.credits_preferences)

        // Re-launch the app if Material You is toggled
        findPreference<Preference>("use_material_you")?.setOnPreferenceChangeListener { _, _ ->
            requireActivity().finishAffinity()
            startActivity(Intent(requireContext(), MainActivity::class.java))
            true
        }


        // Uncheck `disable_frame_throttling` if `force_triple_buffering` gets disabled
        val disableFrameThrottlingPref = findPreference<TwoStatePreference>("disable_frame_throttling")!!
        findPreference<TwoStatePreference>("force_triple_buffering")?.setOnPreferenceChangeListener { _, newValue ->
            if (newValue == false)
                disableFrameThrottlingPref.isChecked = false
            true
        }

        // Only show validation layer setting in debug builds
        @Suppress("SENSELESS_COMPARISON")
        if (BuildConfig.BUILD_TYPE != "release")
            findPreference<Preference>("validation_layer")?.isVisible = true

        if (!GpuDriverHelper.supportsForceMaxGpuClocks()) {
            val forceMaxGpuClocksPref = findPreference<TwoStatePreference>("force_max_gpu_clocks")!!
            forceMaxGpuClocksPref.isSelectable = false
            forceMaxGpuClocksPref.isChecked = false
            forceMaxGpuClocksPref.summary = context!!.getString(R.string.force_max_gpu_clocks_desc_unsupported)
        }

        // Direct host-memory import is a KGSL/libgsl feature.  PanVK on Mali
        // uses Kbase and must stay on Strato's normal buffer upload path.
        if (GpuDriverHelper.isMaliDevice()) {
            val directMemoryImportPref = findPreference<TwoStatePreference>("use_direct_memory_import")!!
            directMemoryImportPref.isSelectable = false
            directMemoryImportPref.isChecked = false
        }
        preferenceScreen.forEach { group ->
            (group as? PreferenceCategory)?.forEach { preference ->
                preference.key?.let { availability[it] = preference.isVisible }
            }
        }
        section = savedInstanceState?.getString("settings_section") ?: "graphics"
        searchQuery = savedInstanceState?.getString("settings_query") ?: ""
        showSection(section, searchQuery)

        lifecycleScope.launch {
            WindowInfoTracker.getOrCreate(requireContext()).windowLayoutInfo(requireActivity()).collect { newLayoutInfo ->
                withContext(Dispatchers.Main) {
                    availability["enable_foldable_layout"] = newLayoutInfo.displayFeatures.any { it is FoldingFeature }
                    showSection(section)
                }
            }
        }

    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString("settings_section", section)
        outState.putString("settings_query", searchQuery)
    }
}

