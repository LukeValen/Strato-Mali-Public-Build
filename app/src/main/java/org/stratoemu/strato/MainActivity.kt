/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Dialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.res.use
import androidx.core.view.WindowCompat
import androidx.core.view.isInvisible
import androidx.core.view.isVisible
import androidx.documentfile.provider.DocumentFile
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.snackbar.Snackbar
import dagger.hilt.android.AndroidEntryPoint
import org.stratoemu.strato.adapter.*
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.data.GameCompatibilityRepository
import org.stratoemu.strato.databinding.MainActivityBinding
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.loader.LoaderResult
import org.stratoemu.strato.provider.DocumentsProvider
import org.stratoemu.strato.settings.AppSettings
import org.stratoemu.strato.settings.EmulationSettings
import org.stratoemu.strato.settings.SettingsActivity
import org.stratoemu.strato.utils.GpuDriverHelper
import org.stratoemu.strato.utils.WindowInsetsHelper
import javax.inject.Inject
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import kotlin.math.ceil
import com.google.android.material.R as MaterialR

@AndroidEntryPoint
class MainActivity : AppCompatActivity() {

    private val binding by lazy { MainActivityBinding.inflate(layoutInflater) }

    @Inject
    lateinit var appSettings : AppSettings

    private val adapter = GenericAdapter()

    private val layoutType get() = LayoutType.values()[appSettings.layoutType]

    private val viewModel by viewModels<MainViewModel>()

    private var appEntries : List<AppEntry>? = null

    enum class SortingOrder {
        AlphabeticalAsc,
        AlphabeticalDesc
    }

    private var selectedGame: AppItem? = null
    private var selectedGameKey: String? = null

    private val documentPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) {
        it?.let { uri ->
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)

                val usableDirectory = try {
                    val directory = DocumentFile.fromTreeUri(this, uri)
                    directory != null && directory.exists() && directory.isDirectory && directory.canRead()
                } catch (e : Exception) {
                    false
                }

                if (!usableDirectory)
                    throw IllegalArgumentException("Selected folder cannot be read")

                appSettings.searchLocation = uri.toString()
                loadRoms(false)
            } catch (e : Exception) {
                android.util.Log.e("StratoMali", "Failed to use selected ROM directory: $uri", e)
                Snackbar.make(
                    findViewById(android.R.id.content),
                    getString(R.string.error) + ": " + (e.localizedMessage ?: e.javaClass.simpleName),
                    Snackbar.LENGTH_LONG
                ).show()
            }
        }
    }

    private val setupCallback = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (appSettings.searchLocation.isBlank()) finish()
        else loadRoms(false)
    }

    private val settingsCallback = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (appSettings.refreshRequired) loadRoms(false)
    }

    private fun AppItem.toViewItem() = AppViewItem(layoutType, this, ::selectGame, ::selectShowGameDialog) { selectedGameKey == it.key() }

    override fun onCreate(savedInstanceState : Bundle?) {
        // Need to create new instance of settings, dependency injection happens
        AppCompatDelegate.setDefaultNightMode(
            when ((AppSettings(this).appTheme)) {
                0 -> AppCompatDelegate.MODE_NIGHT_NO
                1 -> AppCompatDelegate.MODE_NIGHT_YES
                2 -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
                else -> AppCompatDelegate.MODE_NIGHT_UNSPECIFIED
            }
        )
        super.onCreate(savedInstanceState)

        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsHelper.applyToActivity(binding.root, binding.appList)
        WindowInsetsHelper.addMargin(binding.libraryDock, consume = false, bottom = true)

        PreferenceManager.setDefaultValues(this, R.xml.app_preferences, false)
        PreferenceManager.setDefaultValues(this, R.xml.emulation_preferences, false)

        // The library design intentionally does not show the app version.
        WindowCompat.getInsetsController(window, window.decorView).isAppearanceLightStatusBars = false
        WindowCompat.getInsetsController(window, window.decorView).isAppearanceLightNavigationBars = false

        binding.appList.setHasFixedSize(true)

        setupAppList()

        binding.swipeRefreshLayout.apply {
            setProgressBackgroundColorSchemeColor(
                obtainStyledAttributes(intArrayOf(MaterialR.attr.colorSurfaceVariant)).use { it.getColor(0, Color.BLACK) })
            setColorSchemeColors(obtainStyledAttributes(intArrayOf(MaterialR.attr.colorPrimary)).use { it.getColor(0, Color.WHITE) })
            post { setDistanceToTriggerSync(binding.swipeRefreshLayout.height / 3) }
            setOnRefreshListener {
                loadRoms(false)
                refreshCompatibility(force = true)
            }
        }

        viewModel.stateData.observe(this, ::handleState)
        if (appSettings.searchLocation.isBlank()) {
            if (savedInstanceState == null)
                setupCallback.launch(Intent(this, org.stratoemu.strato.setup.SetupActivity::class.java))
        } else {
            loadRoms(!appSettings.refreshRequired)
            // The first-run setup is never interrupted. Show the optional
            // donation invitation on a fresh library launch only, not on
            // rotation, returning from a game, or completing SetupActivity.
            if (savedInstanceState == null) {
                binding.root.post {
                    if (!isFinishing && !isDestroyed) showKofiSupportDialog()
                }
            }
        }

        binding.searchBar.apply {
            binding.logIcon.setOnClickListener {
                val file = DocumentFile.fromSingleUri(this@MainActivity, DocumentsContract.buildDocumentUri(DocumentsProvider.AUTHORITY, "${DocumentsProvider.ROOT_ID}/logs/emulation.log"))!!
                if (file.exists() && file.length() != 0L) {
                    val intent = Intent(Intent.ACTION_SEND)
                        .setDataAndType(file.uri, "text/plain")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        .putExtra(Intent.EXTRA_STREAM, file.uri)
                    startActivity(Intent.createChooser(intent, getString(R.string.log_share_prompt)))
                } else {
                    Snackbar.make(this@MainActivity.findViewById(android.R.id.content), getString(R.string.logs_not_found), Snackbar.LENGTH_SHORT).show()
                }
            }
            binding.settingsIcon.setOnClickListener { settingsCallback.launch(Intent(context, SettingsActivity::class.java)) }
            binding.searchIcon.setOnClickListener { toggleSearch() }
            addTextChangedListener(afterTextChanged = { editable ->
                editable?.let { text -> adapter.filter.filter(text.toString()) }
            })
        }

        binding.playButton.setOnClickListener { selectedGame?.let(::launchGame) }
        binding.detailsButton.setOnClickListener { selectedGame?.let(::selectShowGameDialog) }

        binding.navGames.setOnClickListener {
            if (binding.searchBar.isVisible) toggleSearch()
            binding.searchBar.text = ""
            binding.appBarLayout.setExpanded(true)
            binding.appList.smoothScrollToPosition(0)
        }
        // Game details already contain the DLC/Update installation action.
        binding.navInstall.setOnClickListener { selectedGame?.let(::selectShowGameDialog) }
        binding.navAdd.setOnClickListener { documentPicker.launch(null) }
    }

    private fun showKofiSupportDialog() {
        val dialog = Dialog(this)
        dialog.setContentView(R.layout.mali_kofi_dialog)
        dialog.window?.setBackgroundDrawableResource(android.R.color.transparent)
        dialog.setCancelable(true)

        dialog.findViewById<View>(R.id.kofi_button).setOnClickListener {
            dialog.dismiss()
            try {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://ko-fi.com/lukevalen/donate")))
            } catch (_: ActivityNotFoundException) {
                Snackbar.make(binding.root, R.string.mali_kofi_open_failed, Snackbar.LENGTH_LONG).show()
            }
        }
        dialog.findViewById<View>(R.id.kofi_not_now).setOnClickListener { dialog.dismiss() }

        dialog.show()
        val screenWidth = resources.displayMetrics.widthPixels
        val maxWidth = (430 * resources.displayMetrics.density).toInt()
        val margins = (40 * resources.displayMetrics.density).toInt()
        dialog.window?.setLayout((screenWidth - margins).coerceAtMost(maxWidth).coerceAtLeast(1),
                                 WindowManager.LayoutParams.WRAP_CONTENT)
    }

    private fun setAppListDecoration() {
        binding.appList.apply {
            while (itemDecorationCount > 0) removeItemDecorationAt(0)
            when (layoutType) {
                LayoutType.List -> Unit

                LayoutType.Grid, LayoutType.GridCompact -> addItemDecoration(GridSpacingItemDecoration(resources.getDimensionPixelSize(R.dimen.grid_padding)))
            }
        }
    }

    /**
     * This layout manager handles situations where [onFocusSearchFailed] gets called, when possible we always want to focus on the item with the same span index
     */
    private inner class CustomLayoutManager(gridSpan : Int) : GridLayoutManager(this, gridSpan) {
        init {
            spanSizeLookup = object : SpanSizeLookup() {
                override fun getSpanSize(position : Int) = if (layoutType == LayoutType.List || adapter.currentItems[position].fullSpan) gridSpan else 1
            }
        }

        override fun onRequestChildFocus(parent : RecyclerView, state : RecyclerView.State, child : View, focused : View?) : Boolean {
            binding.appBarLayout.setExpanded(false)
            return super.onRequestChildFocus(parent, state, child, focused)
        }

        override fun onFocusSearchFailed(focused : View, focusDirection : Int, recycler : RecyclerView.Recycler, state : RecyclerView.State) : View? {
            val nextFocus = super.onFocusSearchFailed(focused, focusDirection, recycler, state)
            when (focusDirection) {
                View.FOCUS_DOWN -> {
                    return null
                }

                View.FOCUS_UP -> {
                    if (nextFocus?.isFocusable != true) {
                        binding.searchBar.requestFocus()
                        binding.appBarLayout.setExpanded(true)
                        binding.appList.smoothScrollToPosition(0)
                        return null
                    }
                }
            }
            return nextFocus
        }
    }

    private fun setupAppList() {
        binding.appList.adapter = adapter

        val itemWidth = 225
        val metrics = resources.displayMetrics
        val gridSpan = ceil((metrics.widthPixels / metrics.density) / itemWidth).toInt()

        binding.appList.layoutManager = CustomLayoutManager(gridSpan)
        setAppListDecoration()


    }

    private fun getAppItems() = mutableListOf<AppViewItem>().apply {
        appEntries?.let { entries ->
            sortGameList(entries.toList()).forEach { entry ->
                add(AppItem(entry).toViewItem())
            }
        }
    }

    private fun sortGameList(gameList : List<AppEntry>) : List<AppEntry> {
        val sortedApps : MutableList<AppEntry> = mutableListOf()
        gameList.forEach { entry ->
            if (!appSettings.filterInvalidFiles || entry.loaderResult != LoaderResult.ParsingError)
                sortedApps.add(entry)
        }
        when (appSettings.sortAppsBy) {
            SortingOrder.AlphabeticalAsc.ordinal -> sortedApps.sortBy { it.name }
            SortingOrder.AlphabeticalDesc.ordinal -> sortedApps.sortByDescending { it.name }
        }
        return sortedApps
    }

    private fun handleState(state : MainState) = when (state) {
        MainState.Loading -> {
            // Pull-to-refresh remains available; the top only has Search and Settings.
            binding.swipeRefreshLayout.isRefreshing = true
        }

        is MainState.Loaded -> {
            binding.swipeRefreshLayout.isRefreshing = false

            appEntries = state.items
            populateAdapter()
        }

        is MainState.Error -> Snackbar.make(findViewById(android.R.id.content), getString(R.string.error) + ": ${state.ex.localizedMessage}", Snackbar.LENGTH_SHORT).show()
    }

    private fun selectGame(appItem : AppItem) {
        if (binding.swipeRefreshLayout.isRefreshing) return
        if (selectedGameKey == appItem.key()) {
            if (appSettings.selectAction) selectShowGameDialog(appItem) else launchGame(appItem)
        } else {
            updateSelectedGame(appItem)
        }
    }

    private fun updateSelectedGame(appItem : AppItem?, refreshGrid : Boolean = true) {
        selectedGame = appItem
        selectedGameKey = appItem?.key()
        binding.featuredPanel.isVisible = appItem != null
        binding.featuredTitle.text = appItem?.title.orEmpty()
        val playable = appItem?.loaderResult == LoaderResult.Success
        binding.featuredStatus.text = if (playable) getString(R.string.mali_ready)
            else appItem?.loaderResultString(this).orEmpty()
        binding.playButton.isEnabled = playable
        binding.detailsButton.isEnabled = appItem != null
        binding.navInstall.isEnabled = appItem != null
        if (refreshGrid && adapter.itemCount > 0) {
            adapter.notifyItemRangeChanged(0, adapter.itemCount)
        }
    }

    private fun toggleSearch() {
        val show = !binding.searchBar.isVisible
        binding.searchBar.isVisible = show
        onBackPressedCallback.isEnabled = show
        if (show) {
            binding.appBarLayout.setExpanded(true)
            binding.searchBar.focusSearch()
        } else {
            binding.searchBar.text = ""
            binding.searchBar.clearFocus()
            val keyboard = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
            keyboard.hideSoftInputFromWindow(binding.searchBar.windowToken, 0)
        }
    }

    private fun launchGame(appItem : AppItem) {
        if (binding.swipeRefreshLayout.isRefreshing || appItem.loaderResult != LoaderResult.Success) return
        startActivity(Intent(this, EmulationActivity::class.java).apply {
            putExtra(AppItemTag, appItem)
            putExtra(EmulationActivity.ReturnToMainTag, true)
        })
    }

    private fun selectShowGameDialog(appItem : AppItem) {
        if (binding.swipeRefreshLayout.isRefreshing) return

        AppDialog.newInstance(appItem).show(supportFragmentManager, "game")
    }

    private fun loadRoms(loadFromFile : Boolean) {
        if (appSettings.searchLocation.isBlank()) {
            binding.swipeRefreshLayout.isRefreshing = false
            return
        }
        if (!loadFromFile) {
            binding.romPlaceholder.isVisible = true
            binding.romPlaceholder.text = getString(R.string.searching_roms)
        }
        viewModel.loadRoms(this, loadFromFile, Uri.parse(appSettings.searchLocation), EmulationSettings.global.systemLanguage)
        appSettings.refreshRequired = false
    }

    private fun populateAdapter() {
        val games = appEntries?.let(::sortGameList).orEmpty()
        val selectedEntry = games.firstOrNull { AppItem(it).key() == selectedGameKey } ?: games.firstOrNull()
        updateSelectedGame(selectedEntry?.let { AppItem(it) }, refreshGrid = false)

        val items = getAppItems()
        binding.romPlaceholder.isVisible = items.isEmpty()
        if (items.isEmpty())
            binding.romPlaceholder.text = getString(R.string.no_rom)
        adapter.setItems(items)
    }

    private val onBackPressedCallback = object : OnBackPressedCallback(true) {
        override fun handleOnBackPressed() {
            if (binding.searchBar.isVisible) {
                toggleSearch()
            } else {
                isEnabled = false
                onBackPressedDispatcher.onBackPressed()
            }
        }
    }

    override fun onStart() {
        super.onStart()

        binding.searchBar.addTextChangedListener { text ->
            if (!onBackPressedCallback.isEnabled && !text.isNullOrEmpty()) {
                onBackPressedCallback.isEnabled = true
            }
        }

        onBackPressedDispatcher.addCallback(this, onBackPressedCallback)
        onBackPressedCallback.isEnabled = binding.searchBar.hasFocus() || binding.searchBar.text.isNotEmpty()
    }

    private fun refreshCompatibility(force: Boolean = false) {
        lifecycleScope.launch {
            if (GameCompatibilityRepository.refresh(this@MainActivity, force) && !isFinishing && !isDestroyed)
                adapter.notifyItemRangeChanged(0, adapter.itemCount)
        }
    }

    override fun onResume() {
        super.onResume()

        // Try to return to normal GPU clocks upon resuming back to main activity, to avoid GPU being stuck at max clocks after a crash
        GpuDriverHelper.forceMaxGpuClocks(false)

        refreshCompatibility()

        var layoutTypeChanged = false
        for (appViewItem in adapter.allItems.filterIsInstance(AppViewItem::class.java)) {
            if (layoutType != appViewItem.layoutType) {
                appViewItem.layoutType = layoutType
                layoutTypeChanged = true
            } else {
                break
            }
        }

        if (layoutTypeChanged) {
            setAppListDecoration()
            adapter.notifyItemRangeChanged(0, adapter.currentItems.size)
        }

        if (appSettings.searchLocation.isNotBlank()) {
            viewModel.checkRomHash(Uri.parse(appSettings.searchLocation), EmulationSettings.global.systemLanguage)
        }
    }
}
