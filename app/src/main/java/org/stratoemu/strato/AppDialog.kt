/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.content.ComponentName
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.lifecycle.lifecycleScope
import androidx.appcompat.app.AlertDialog
import androidx.browser.customtabs.CustomTabsClient
import androidx.browser.customtabs.CustomTabsIntent
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialogFragment
import com.google.android.material.snackbar.Snackbar
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.databinding.AppDialogBinding
import org.stratoemu.strato.loader.LoaderResult
import org.stratoemu.strato.settings.SettingsActivity
import org.stratoemu.strato.utils.SaveManagementUtils
import org.stratoemu.strato.utils.GameContentUtils
import org.stratoemu.strato.utils.GameModUtils
import org.stratoemu.strato.loader.RomFormat
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import android.widget.ProgressBar
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale
import org.stratoemu.strato.utils.serializable

/**
 * This dialog is used to show extra game metadata and provide extra options such as pinning the game to the home screen
 */
class AppDialog : BottomSheetDialogFragment() {
    companion object {
        /**
         * @param item This is used to hold the [AppItem] between instances
         */
        fun newInstance(item : AppItem) : AppDialog {
            val args = Bundle()
            args.putSerializable(AppItemTag, item)

            val fragment = AppDialog()
            fragment.arguments = args
            return fragment
        }
    }

    private lateinit var binding : AppDialogBinding

    private val item by lazy { requireArguments().serializable<AppItem>(AppItemTag)!! }

    /**
     * Used to manage save files
     */
    private lateinit var documentPicker : ActivityResultLauncher<Array<String>>
    private lateinit var startForResultExportSave : ActivityResultLauncher<String>
    private var installingContent = false
    private val contentPicker = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) installContent(uris.distinct())
    }
    private val modPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) installMod(uri)
    }
    private val modFolderPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) installMod(uri, true)
    }

    private fun refreshContent() {
        val files = item.titleId?.let { GameContentUtils.installed(requireContext(), it) } ?: emptyList()
        binding.contentStatus.text = getString(R.string.game_content_count, files.size)
        binding.manageContent.isEnabled = files.isNotEmpty() && !installingContent
    }

    private fun installContent(uris : List<android.net.Uri>) {
        if (installingContent) return
        val titleId = item.titleId ?: return
        val context = requireContext().applicationContext
        installingContent = true
        binding.installContent.isEnabled = false
        binding.manageContent.isEnabled = false
        binding.gamePlay.isEnabled = false
        val text = TextView(requireContext()).apply { setText(R.string.game_content_copying) }
        val progress = ProgressBar(requireContext(), null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true; max = 100 }
        val layout = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (24 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
            addView(text)
            addView(progress)
        }
        val dialog = AlertDialog.Builder(requireContext()).setTitle(R.string.game_content_installing).setView(layout).setCancelable(false).create()
        dialog.show()
        lifecycleScope.launch {
            try {
                var installedCount = 0
                var existingCount = 0
                val failures = mutableListOf<String>()
                for ((index, uri) in uris.withIndex()) {
                    var lastUpdate = 0L
                    try {
                        val installed = GameContentUtils.install(context, item.uri, titleId, uri) { copied, total ->
                            val now = android.os.SystemClock.elapsedRealtime()
                            if (copied < 0 || now - lastUpdate >= 150) {
                                lastUpdate = now
                                progress.post {
                                    text.text = "${index + 1}/${uris.size} • " + getString(if (copied < 0) R.string.game_content_verifying else R.string.game_content_copying)
                                    progress.isIndeterminate = total <= 0 || copied < 0
                                    if (copied >= 0 && total > 0) progress.progress = (copied.toDouble() * 100 / total).toInt().coerceIn(0, 100)
                                }
                            }
                        }
                        if (installed) installedCount++ else existingCount++
                    } catch (e : CancellationException) {
                        throw e
                    } catch (e : Exception) {
                        failures.add("${index + 1}: ${e.message ?: "Unknown error"}")
                    }
                }
                if (isAdded && view != null) {
                    Snackbar.make(binding.root, getString(R.string.game_content_batch_summary, installedCount, existingCount, failures.size), Snackbar.LENGTH_LONG).show()
                    if (failures.isNotEmpty()) AlertDialog.Builder(requireContext())
                        .setTitle(R.string.game_content_batch_failed).setMessage(failures.take(12).joinToString("\n"))
                        .setPositiveButton(android.R.string.ok, null).show()
                }
            } catch (e : CancellationException) {
                throw e
            } catch (e : Exception) {
                if (isAdded && view != null) AlertDialog.Builder(requireContext()).setTitle(R.string.game_content_install_failed).setMessage(e.message).setPositiveButton(android.R.string.ok, null).show()
            } finally {
                dialog.dismiss()
                installingContent = false
                if (isAdded && view != null) {
                    binding.installContent.isEnabled = item.loaderResult == LoaderResult.Success && (item.format == RomFormat.NSP || item.format == RomFormat.XCI)
                    binding.gamePlay.isEnabled = item.loaderResult == LoaderResult.Success
                    refreshContent()
                }
            }
        }
    }

    private fun refreshMods() {
        val mods = item.titleId?.let { GameModUtils.list(requireContext(), it) } ?: emptyList()
        binding.modStatus.text = getString(R.string.game_mods_count, mods.size)
        binding.manageMods.isEnabled = mods.isNotEmpty()
    }

    private fun installMod(uri: android.net.Uri, folder: Boolean = false) {
        val titleId = item.titleId ?: return
        val context = requireContext().applicationContext
        binding.installMod.isEnabled = false
        binding.installModZip.isEnabled = false
        lifecycleScope.launch {
            try {
                if (folder) GameModUtils.installFolder(context, titleId, uri)
                else GameModUtils.install(context, titleId, uri)
                if (isAdded && view != null) Snackbar.make(binding.root, R.string.game_mods_imported, Snackbar.LENGTH_LONG).show()
            } catch (e : Exception) {
                if (isAdded && view != null) AlertDialog.Builder(requireContext())
                    .setTitle(R.string.game_mods_fail).setMessage(e.message)
                    .setPositiveButton(android.R.string.ok, null).show()
            } finally {
                if (isAdded && view != null) {
                    binding.installMod.isEnabled = true
                    binding.installModZip.isEnabled = true
                    refreshMods()
                }
            }
        }
    }

    private fun manageMods() {
        val titleId = item.titleId ?: return
        val mods = GameModUtils.list(requireContext(), titleId)
        if (mods.isEmpty()) return
        val labels = mods.map { it.name }.toTypedArray()
        val enabled = mods.map { GameModUtils.isEnabled(it) }.toBooleanArray()
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.game_mods_manage)
            .setMessage(R.string.game_mods_notes)
            .setMultiChoiceItems(labels, enabled) { _, which, checked ->
                try { GameModUtils.setEnabled(mods[which], checked); refreshMods() }
                catch (e : Exception) { Snackbar.make(binding.root, e.message ?: "Failed to update mod", Snackbar.LENGTH_LONG).show() }
            }
            .setNeutralButton(R.string.game_mods_remove) { _, _ ->
                val updated = GameModUtils.list(requireContext(), titleId)
                AlertDialog.Builder(requireContext()).setTitle(R.string.game_mods_remove)
                    .setItems(updated.map { it.name }.toTypedArray()) { _, index ->
                        AlertDialog.Builder(requireContext()).setTitle(R.string.game_mods_remove)
                            .setMessage(updated[index].name)
                            .setNegativeButton(android.R.string.cancel, null)
                            .setPositiveButton(R.string.yes) { _, _ ->
                                try { GameModUtils.remove(requireContext(), titleId, updated[index]); refreshMods() }
                                catch (e : Exception) { Snackbar.make(binding.root, e.message ?: "Could not delete mod", Snackbar.LENGTH_LONG).show() }
                            }.show()
                    }.setNegativeButton(android.R.string.cancel, null).show()
            }
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        documentPicker = SaveManagementUtils.registerDocumentPicker(requireActivity()) {
            val isSaveFileOfThisGame = SaveManagementUtils.saveFolderGameExists(item.titleId)
            binding.deleteSave.isEnabled = isSaveFileOfThisGame
            binding.exportSave.isEnabled = isSaveFileOfThisGame
        }
        startForResultExportSave = SaveManagementUtils.registerStartForResultExportSave(requireActivity())
    }

    /**
     * This inflates the layout of the dialog after initial view creation
     */
    override fun onCreateView(inflater : LayoutInflater, container : ViewGroup?, savedInstanceState : Bundle?) = AppDialogBinding.inflate(inflater).also { binding = it }.root

    override fun onViewCreated(view : View, savedInstanceState : Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        // Set the peek height after the root view has been laid out
        view.apply {
            post {
                val behavior = BottomSheetBehavior.from(parent as View)
                behavior.peekHeight = height
            }
        }

        binding.gameIcon.setImageBitmap(item.bitmapIcon)
        binding.gameTitle.text = item.title
        binding.gameVersion.text = item.version ?: item.loaderResultString(requireContext())
        binding.gameTitleId.text = item.titleId
        binding.gameAuthor.text = item.author

        val reportTitleId = item.titleId?.trim()?.uppercase(Locale.ROOT)
        binding.submitCompatibility.isEnabled = reportTitleId?.matches(Regex("[0-9A-F]{16}")) == true
        binding.submitCompatibility.setOnClickListener {
            val id = reportTitleId ?: return@setOnClickListener
            val name = item.title.replace(Regex("[\\r\\n]"), " ").trim().take(120).ifBlank { id }
            val report = Uri.parse("https://github.com/LukeValen/Strato-Mali-Compatibility/issues/new")
                .buildUpon()
                .appendQueryParameter("template", "compatibility-report.yml")
                .appendQueryParameter("title", "[Compatibility]: $name")
                .appendQueryParameter("game-name", name)
                .appendQueryParameter("title-id", id)
                .appendQueryParameter("emulator", BuildConfig.VERSION_NAME)
            org.stratoemu.strato.utils.GpuDriverHelper.getSystemGpuName()
                ?.takeIf { it.isNotBlank() }?.take(100)?.let { report.appendQueryParameter("gpu", it) }
            try {
                // Keep the issue form in a browser: some GitHub app versions discard URL field prefills.
                val browserPackage = CustomTabsClient.getPackageName(requireContext(), null)
                if (browserPackage != null) {
                    CustomTabsIntent.Builder().build().apply { intent.setPackage(browserPackage) }
                        .launchUrl(requireContext(), report.build())
                } else {
                    startActivity(Intent(Intent.ACTION_VIEW, report.build()).addCategory(Intent.CATEGORY_BROWSABLE))
                }
            } catch (_: ActivityNotFoundException) {
                Snackbar.make(binding.root, R.string.compatibility_open_failed, Snackbar.LENGTH_LONG).show()
            }
        }

        binding.gamePlay.isEnabled = item.loaderResult == LoaderResult.Success
        binding.gamePlay.setOnClickListener {
            startActivity(Intent(activity, EmulationActivity::class.java).apply {
                putExtras(requireArguments())
            })
        }

        binding.gameSettings.isEnabled = item.loaderResult == LoaderResult.Success
        binding.gameSettings.setOnClickListener {
            startActivity(Intent(activity, SettingsActivity::class.java).apply {
                putExtras(requireArguments())
            })
        }

        val shortcutManager = requireActivity().getSystemService(ShortcutManager::class.java)
        binding.gamePin.isEnabled = shortcutManager.isRequestPinShortcutSupported

        binding.gamePin.setOnClickListener {
            val info = ShortcutInfo.Builder(context, item.title)
            info.setShortLabel(item.title)
            info.setActivity(ComponentName(requireContext(), EmulationActivity::class.java))
            info.setIcon(Icon.createWithAdaptiveBitmap(item.bitmapIcon))

            val intent = Intent(context, EmulationActivity::class.java)
            intent.data = item.uri
            intent.action = Intent.ACTION_VIEW

            info.setIntent(intent)

            shortcutManager.requestPinShortcut(info.build(), null)
        }

        val saveExists = SaveManagementUtils.saveFolderGameExists(item.titleId)

        binding.deleteSave.isEnabled = saveExists
        binding.deleteSave.setOnClickListener {
            AlertDialog.Builder(requireContext())
                .setTitle(getString(R.string.delete_save_confirmation_message))
                .setMessage(getString(R.string.action_irreversible))
                .setNegativeButton(getString(R.string.no), null)
                .setPositiveButton(getString(R.string.yes)) { _, _ ->
                    SaveManagementUtils.deleteSaveFile(item.titleId)
                    binding.deleteSave.isEnabled = false
                    binding.exportSave.isEnabled = false
                }.show()
        }

        binding.importSave.setOnClickListener {
            SaveManagementUtils.importSave(documentPicker)
        }

        binding.exportSave.isEnabled = saveExists
        binding.exportSave.setOnClickListener {
            SaveManagementUtils.exportSave(requireContext(), startForResultExportSave, item.titleId, "${item.title} (v${binding.gameVersion.text}) [${item.titleId}]")
        }

        binding.installContent.isEnabled = item.loaderResult == LoaderResult.Success && (item.format == RomFormat.NSP || item.format == RomFormat.XCI)
        binding.installContent.setOnClickListener { contentPicker.launch(arrayOf("*/*")) }
        binding.installMod.isEnabled = item.titleId?.matches(Regex("[0-9a-fA-F]{16}")) == true
        binding.installMod.setOnClickListener { modFolderPicker.launch(null) }
        binding.installModZip.isEnabled = binding.installMod.isEnabled
        binding.installModZip.setOnClickListener { modPicker.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) }
        binding.manageMods.setOnClickListener { manageMods() }
        refreshMods()
        refreshContent()
        binding.manageContent.setOnClickListener {
            val titleId = item.titleId ?: return@setOnClickListener
            val files = GameContentUtils.installed(requireContext(), titleId)
            AlertDialog.Builder(requireContext()).setTitle(R.string.game_content_manage)
                .setItems(files.map { GameContentUtils.label(requireContext(), it) }.toTypedArray()) { _, index ->
                    AlertDialog.Builder(requireContext()).setTitle(R.string.game_content_remove)
                        .setMessage(GameContentUtils.label(requireContext(), files[index]))
                        .setNegativeButton(android.R.string.cancel, null)
                        .setPositiveButton(R.string.yes) { _, _ ->
                            try { GameContentUtils.remove(requireContext(), files[index]); refreshContent() }
                            catch (e : Exception) { Snackbar.make(binding.root, e.message ?: getString(R.string.game_content_install_failed), Snackbar.LENGTH_LONG).show() }
                        }.show()
                }.setNegativeButton(android.R.string.cancel, null).show()
        }

        binding.gameTitleId.setOnLongClickListener {
            val clipboard = requireActivity().getSystemService(android.content.Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
            clipboard.setPrimaryClip(android.content.ClipData.newPlainText("Title ID", item.titleId))
            Snackbar.make(binding.root, getString(R.string.copied_to_clipboard), Snackbar.LENGTH_SHORT).show()
            true
        }

        dialog?.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_BUTTON_B && event.action == KeyEvent.ACTION_UP) {
                dialog?.onBackPressed()
                true
            } else {
                false
            }
        }
    }
}
