/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.settings

import android.annotation.SuppressLint
import android.os.Bundle
import android.text.TextUtils
import android.view.KeyEvent
import android.view.Menu
import android.view.View
import android.view.ViewTreeObserver
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SearchView
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.view.WindowCompat
import androidx.preference.EditTextPreference
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.forEach
import com.google.android.material.appbar.AppBarLayout
import com.google.android.material.tabs.TabLayout
import com.google.android.material.internal.ToolbarUtils
import org.stratoemu.strato.BuildConfig
import org.stratoemu.strato.R
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.databinding.SettingsActivityBinding
import org.stratoemu.strato.preference.IntegerListPreference
import org.stratoemu.strato.preference.dialog.EditTextPreferenceMaterialDialogFragmentCompat
import org.stratoemu.strato.preference.dialog.IntegerListPreferenceMaterialDialogFragmentCompat
import org.stratoemu.strato.preference.dialog.ListPreferenceMaterialDialogFragmentCompat
import org.stratoemu.strato.utils.WindowInsetsHelper

private const val PREFERENCE_DIALOG_FRAGMENT_TAG = "androidx.preference.PreferenceFragment.DIALOG"

class SettingsActivity : AppCompatActivity(), PreferenceFragmentCompat.OnPreferenceDisplayDialogCallback {
    private var searchView: SearchView? = null
    private val sectionDescriptions = listOf(R.string.settings_graphics_summary, R.string.settings_gpu_summary,
        R.string.settings_folders_summary, R.string.settings_drivers_summary, R.string.settings_general_summary)

    val binding by lazy { SettingsActivityBinding.inflate(layoutInflater) }
    val hiddenCategoriesFromSearch = if (BuildConfig.BUILD_TYPE == "release") {
        arrayOf("category_debug", "category_credits", "category_licenses")
    } else {
        arrayOf("category_credits", "category_licenses")
    }

    /**
     * The instance of [PreferenceFragmentCompat] that is shown inside [R.id.settings]
     * Retrieves extras from the intent if any and instantiates the appropriate fragment
     */
    private val preferenceFragment by lazy {
        (supportFragmentManager.findFragmentById(R.id.settings) as? PreferenceFragmentCompat) ?: if (intent.hasExtra(AppItemTag))
            GameSettingsFragment().apply { arguments = intent.extras }
        else
            GlobalSettingsFragment()
    }

    /**
     * This initializes all of the elements in the activity and displays the settings fragment
     */
    override fun onCreate(savedInstanceState : Bundle?) {
        theme.applyStyle(R.style.SettingsBlueTheme, true)
        super.onCreate(savedInstanceState)

        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsHelper.applyToActivity(binding.root)

        setSupportActionBar(binding.titlebar.toolbar)
        if (!intent.hasExtra(AppItemTag)) {
            supportActionBar?.title = getString(R.string.settings_hub_title)
            val sections = listOf("graphics", "gpu", "folders", "drivers", "general")
            val titles = listOf(R.string.settings_tab_graphics, R.string.settings_tab_gpu,
                R.string.settings_tab_folders, R.string.settings_tab_drivers, R.string.settings_tab_general)
            val selected = savedInstanceState?.getInt("settings_tab", 0) ?: 0
            titles.forEachIndexed { index, title ->
                binding.settingsTabs.addTab(binding.settingsTabs.newTab().setText(title), index == selected)
            }
            binding.settingsTabs.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
                override fun onTabSelected(tab: TabLayout.Tab) {
                    (preferenceFragment as? GlobalSettingsFragment)?.showSection(sections[tab.position], "")
                    binding.sectionDescription.setText(sectionDescriptions[tab.position])
                    // Searching is scoped to the selected tab.
                    searchView?.setQuery("", false)
                }
                override fun onTabUnselected(tab: TabLayout.Tab) {}
                override fun onTabReselected(tab: TabLayout.Tab) {}
            })
            binding.sectionDescription.setText(sectionDescriptions[selected])
        } else {
            binding.settingsHeader.visibility = View.GONE
        }
        supportActionBar?.setDisplayHomeAsUpEnabled(true)

        var layoutDone = false // Tracks if the layout is complete to avoid retrieving invalid attributes
        binding.coordinatorLayout.viewTreeObserver.addOnTouchModeChangeListener { isTouchMode ->
            val layoutUpdate = {
                val params = binding.settingsContainer.layoutParams as CoordinatorLayout.LayoutParams
                if (!isTouchMode) {
                    binding.titlebar.appBarLayout.setExpanded(true)
                    params.height = binding.coordinatorLayout.height - binding.titlebar.toolbar.height
                } else {
                    params.height = CoordinatorLayout.LayoutParams.MATCH_PARENT
                }

                binding.settingsContainer.layoutParams = params
                binding.settingsContainer.requestLayout()
            }

            if (!layoutDone) {
                binding.coordinatorLayout.viewTreeObserver.addOnGlobalLayoutListener(object : ViewTreeObserver.OnGlobalLayoutListener {
                    override fun onGlobalLayout() {
                        // We need to wait till the layout is done to get the correct height of the toolbar
                        binding.coordinatorLayout.viewTreeObserver.removeOnGlobalLayoutListener(this)
                        layoutUpdate()
                        layoutDone = true
                    }
                })
            } else {
                layoutUpdate()
            }
        }

        fun enableMarquee(textView : TextView) {
            textView.ellipsize = TextUtils.TruncateAt.MARQUEE
            textView.isSelected = true
            textView.marqueeRepeatLimit = -1
        }

        // Set a temporary subtitle because the retrieval of the subtitle TextView fails if subtitle is null
        supportActionBar?.subtitle = "sub"

        @SuppressLint("RestrictedApi")
        val toolbarTitleTextView = ToolbarUtils.getTitleTextView(binding.titlebar.toolbar)

        @SuppressLint("RestrictedApi")
        val toolbarSubtitleTextView = ToolbarUtils.getSubtitleTextView(binding.titlebar.toolbar)
        toolbarTitleTextView?.let { enableMarquee(it) }
        toolbarSubtitleTextView?.let { enableMarquee(it) }

        // Reset the subtitle to null
        supportActionBar?.subtitle = null

        if (savedInstanceState == null) {
            supportFragmentManager
                .beginTransaction()
                .replace(R.id.settings, preferenceFragment)
                .commit()
        }
    }

    override fun onCreateOptionsMenu(menu : Menu?) : Boolean {
        menuInflater.inflate(R.menu.settings_menu, menu)
        val menuItem = menu!!.findItem(R.id.app_bar_search)
        val searchView = menuItem.actionView as SearchView
        this.searchView = searchView
        searchView.queryHint = getString(R.string.search)

        searchView.setOnQueryTextFocusChangeListener { _, focus ->
            (binding.titlebar.toolbar.layoutParams as AppBarLayout.LayoutParams).scrollFlags =
                if (focus)
                    AppBarLayout.LayoutParams.SCROLL_FLAG_NO_SCROLL
                else
                    AppBarLayout.LayoutParams.SCROLL_FLAG_SCROLL
        }

        searchView.setOnQueryTextListener(object : SearchView.OnQueryTextListener {
            override fun onQueryTextSubmit(query : String) : Boolean {
                return false
            }

            override fun onQueryTextChange(newText : String) : Boolean {
                val global = preferenceFragment as? GlobalSettingsFragment
                if (global != null) {
                    val sections = listOf("graphics", "gpu", "folders", "drivers", "general")
                    global.showSection(sections[binding.settingsTabs.selectedTabPosition.coerceAtLeast(0)], newText)
                    return true
                }
                val queries = newText.split(",")
                if (newText.isNotEmpty()) {
                    preferenceFragment.preferenceScreen.forEach { preferenceCategory ->
                        if (hiddenCategoriesFromSearch.contains(preferenceCategory.key)) {
                            preferenceCategory.isVisible = false
                            return@forEach
                        }
                        val queryMatchesCategory = queries.any { preferenceCategory.title?.contains(it, true) ?: false }
                        // Tracks whether all preferences under this category are hidden
                        var areAllPrefsHidden = true
                        (preferenceCategory as PreferenceCategory).forEach { preference ->
                            preference.isVisible = queryMatchesCategory || queries.any { preference.title?.contains(it, true) ?: false }
                            if (preference.isVisible && areAllPrefsHidden)
                                areAllPrefsHidden = false
                        }
                        // Hide PreferenceCategory if none of its preferences match the search and neither the category title
                        preferenceCategory.isVisible = !areAllPrefsHidden || queryMatchesCategory
                    }
                } else { // If user input is empty, show all preferences
                    preferenceFragment.preferenceScreen.forEach { preferenceCategory ->
                        preferenceCategory.isVisible = true
                        (preferenceCategory as PreferenceCategory).forEach { preference ->
                            preference.isVisible = true
                        }
                    }
                }
                return true
            }
        })
        return super.onCreateOptionsMenu(menu)
    }

    /**
     * This handles on calling [onBackPressed] when [KeyEvent.KEYCODE_BUTTON_B] is lifted
     */
    override fun onKeyUp(keyCode : Int, event : KeyEvent?) : Boolean {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_B) {
            onBackPressedDispatcher.onBackPressed()
            return true
        }

        return super.onKeyUp(keyCode, event)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putInt("settings_tab", binding.settingsTabs.selectedTabPosition.coerceAtLeast(0))
        super.onSaveInstanceState(outState)
    }

    override fun finish() {
        setResult(RESULT_OK)
        super.finish()
    }

    override fun onPreferenceDisplayDialog(caller : PreferenceFragmentCompat, pref : Preference) : Boolean {
        when (pref) {
            is IntegerListPreference -> {
                // Check if dialog is already showing
                if (supportFragmentManager.findFragmentByTag(PREFERENCE_DIALOG_FRAGMENT_TAG) != null)
                    return true

                val dialogFragment = IntegerListPreferenceMaterialDialogFragmentCompat.newInstance(pref.getKey())
                @Suppress("DEPRECATION")
                dialogFragment.setTargetFragment(caller, 0) // androidx.preference.PreferenceDialogFragmentCompat depends on the target fragment being set correctly even though it's deprecated
                dialogFragment.show(supportFragmentManager, PREFERENCE_DIALOG_FRAGMENT_TAG)
                return true
            }

            is EditTextPreference -> {
                if (supportFragmentManager.findFragmentByTag(PREFERENCE_DIALOG_FRAGMENT_TAG) != null)
                    return true

                val dialogFragment = EditTextPreferenceMaterialDialogFragmentCompat.newInstance(pref.getKey())
                @Suppress("DEPRECATION")
                dialogFragment.setTargetFragment(caller, 0)
                dialogFragment.show(supportFragmentManager, PREFERENCE_DIALOG_FRAGMENT_TAG)
                return true
            }

            is ListPreference -> {
                if (supportFragmentManager.findFragmentByTag(PREFERENCE_DIALOG_FRAGMENT_TAG) != null)
                    return true

                val dialogFragment = ListPreferenceMaterialDialogFragmentCompat.newInstance(pref.getKey())
                @Suppress("DEPRECATION")
                dialogFragment.setTargetFragment(caller, 0)
                dialogFragment.show(supportFragmentManager, PREFERENCE_DIALOG_FRAGMENT_TAG)
                return true
            }

            else -> return false
        }
    }
}

