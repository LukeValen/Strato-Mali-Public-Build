/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.utils

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.FragmentActivity
import org.stratoemu.strato.R
import org.stratoemu.strato.StratoApplication
import org.stratoemu.strato.getPublicFilesDir
import org.stratoemu.strato.provider.DocumentsProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedOutputStream
import java.io.File
import java.io.FilenameFilter
import java.io.IOException
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

interface SaveManagementUtils {

    companion object {
        private val savesFolderRoot = "${StratoApplication.instance.getPublicFilesDir().canonicalPath}/switch/nand/user/save/0000000000000000/00000000000000000000000000000001"

        fun registerDocumentPicker(context : Context) : ActivityResultLauncher<Array<String>> {
            return (context as ComponentActivity).registerForActivityResult(ActivityResultContracts.OpenDocument()) {
                it?.let { uri -> importSave(context, uri) }
            }
        }

        fun registerDocumentPicker(fragmentAct : FragmentActivity, onImportComplete : () -> Unit = {}) : ActivityResultLauncher<Array<String>> {
            val activity = fragmentAct as AppCompatActivity
            val activityResultRegistry = fragmentAct.activityResultRegistry

            return activityResultRegistry.register("documentPickerKey", ActivityResultContracts.OpenDocument()) {
                it?.let { uri -> importSave(activity, uri, onImportComplete) }
            }
        }

        // ACTION_CREATE_DOCUMENT writes a real, durable ZIP directly to the
        // destination selected by the user. Sharing a temporary archive and
        // deleting it on the share-sheet callback could truncate exports.
        fun registerStartForResultExportSave(context : Context) : ActivityResultLauncher<String> {
            return (context as ComponentActivity).registerForActivityResult(
                ActivityResultContracts.CreateDocument("application/zip")
            ) { uri -> onExportDestinationSelected(context, uri) }
        }

        fun registerStartForResultExportSave(fragmentAct : FragmentActivity) : ActivityResultLauncher<String> {
            return fragmentAct.activityResultRegistry.register(
                "startForResultExportSaveKey",
                ActivityResultContracts.CreateDocument("application/zip")
            ) { uri -> onExportDestinationSelected(fragmentAct, uri) }
        }

        private fun onExportDestinationSelected(context : Context, uri : Uri?) {
            val prefs = context.getSharedPreferences("save_export_pending", Context.MODE_PRIVATE)
            val gameId = prefs.getString("title_id", null)
            prefs.edit().remove("title_id").apply()
            if (uri == null || gameId == null) return // User cancelled the picker.
            CoroutineScope(Dispatchers.IO).launch {
                val result = try {
                    val folder = if (gameId.isEmpty()) File(savesFolderRoot) else File(savesFolderRoot, gameId)
                    require(folder.isDirectory) { "Save folder does not exist" }
                    context.contentResolver.openOutputStream(uri, "w")?.use { output ->
                        zipSave(folder, output)
                    } ?: throw IOException("Could not create save ZIP at the selected location")
                    true
                } catch (e : Exception) {
                    android.util.Log.e("StratoMali", "Save export failed", e)
                    false
                }
                withContext(Dispatchers.Main) {
                    Toast.makeText(context, if (result) R.string.save_export_success else R.string.save_export_failed, Toast.LENGTH_LONG).show()
                }
            }
        }

        /**
         * Checks if the saves folder exists.
         */
        fun savesFolderRootExists() : Boolean {
            return File(savesFolderRoot).exists()
        }

        /**
         * Checks if the save folder for the given game exists.
         */
        fun saveFolderGameExists(titleId : String?) : Boolean {
            if (titleId == null) return false
            return File(savesFolderRoot, titleId).exists()
        }

        /**
         * @return The folder containing the save file for the given game.
         */
        fun getSaveFolderGame(titleId : String) : File {
            return File(savesFolderRoot, titleId)
        }

        /**
         * Export a save archive directly to the user-selected SAF document.
         * Every entry stays inside the save root; symlinks are never followed.
         * The top-level title ID is preserved for round-trip import.
         */
        private fun zipSave(saveFolder : File, output : java.io.OutputStream) {
            val root = File(savesFolderRoot).canonicalFile
            check(saveFolder.canonicalFile == root || saveFolder.canonicalPath.startsWith(root.path + File.separator))
            var entries = 0
            ZipOutputStream(BufferedOutputStream(output)).use { zip ->
                saveFolder.walkTopDown().onEnter { !java.nio.file.Files.isSymbolicLink(it.toPath()) }
                    .forEach { file ->
                        if (java.nio.file.Files.isSymbolicLink(file.toPath())) return@forEach
                        if (file == root) return@forEach
                        require(file.canonicalPath.startsWith(root.path + File.separator)) { "Unsafe save path" }
                        val name = file.relativeTo(root).invariantSeparatorsPath
                        val entry = ZipEntry(name + if (file.isDirectory) "/" else "")
                        zip.putNextEntry(entry)
                        if (file.isFile) {
                            file.inputStream().use { it.copyTo(zip) }
                            entries++
                        }
                        zip.closeEntry()
                    }
            }
            check(entries > 0) { "No save data files to export" }
        }

        /**
         * Open Android's Save As dialog instead of sharing a temporary ZIP.
         * The selected title ID survives Activity recreation while the picker is open.
         */
        fun exportSave(context : Context, startForResultExportSave : ActivityResultLauncher<String>, titleId : String?, outputZipName : String) {
            if (titleId == null || (titleId.isNotEmpty() && !titleId.matches(Regex("[0-9A-Fa-f]{16}")))) return
            val folder = if (titleId.isEmpty()) File(savesFolderRoot) else File(savesFolderRoot, titleId)
            if (!folder.isDirectory) {
                Toast.makeText(context, R.string.save_export_failed, Toast.LENGTH_LONG).show()
                return
            }
            val safeName = outputZipName.replace(Regex("[\\\\/:*?\"<>|\\n\\r]"), "_").take(120).trim()
            val name = (safeName.ifBlank { "Strato Mali Saves" }) + ".zip"
            context.getSharedPreferences("save_export_pending", Context.MODE_PRIVATE).edit()
                .putString("title_id", titleId).commit()
            startForResultExportSave.launch(name)
        }

        /**
         * Launches the document picker to import a save file.
         */
        fun importSave(documentPicker : ActivityResultLauncher<Array<String>>) {
            documentPicker.launch(arrayOf("application/zip"))
        }

        /**
         * Imports the save files contained in the zip file, and replaces any existing ones with the new save file.
         * @param zipUri The Uri of the zip file containing the save file(s) to import.
         */
        private fun importSave(context : Context, zipUri : Uri, onImportComplete : () -> Unit = {}) {
            val inputZip = StratoApplication.instance.contentResolver.openInputStream(zipUri)
            // A zip needs to have at least one subfolder named after a TitleId in order to be considered valid.
            var validZip = false
            val savesFolder = File(savesFolderRoot)
            val cacheSaveDir = File("${StratoApplication.instance.cacheDir.path}/saves/")
            cacheSaveDir.mkdir()

            if (inputZip == null) {
                Toast.makeText(context, R.string.error, Toast.LENGTH_LONG).show()
                return
            }

            val filterTitleId = FilenameFilter { _, dirName -> dirName.matches(Regex("^0100[\\dA-Fa-f]{12}$")) }

            CoroutineScope(Dispatchers.IO).launch {
                try {
                    ZipUtils.unzip(inputZip, cacheSaveDir)
                    cacheSaveDir.list(filterTitleId)?.forEach { savePath ->
                        File(savesFolder, savePath).deleteRecursively()
                        File(cacheSaveDir, savePath).copyRecursively(File(savesFolder, savePath), true)
                        validZip = true
                    }

                    withContext(Dispatchers.Main) {
                        if (!validZip) {
                            Toast.makeText(context, R.string.save_file_invalid_zip_structure, Toast.LENGTH_LONG).show()
                            return@withContext
                        }
                        onImportComplete()
                        Toast.makeText(context, R.string.save_file_imported_ok, Toast.LENGTH_LONG).show()
                    }
                } catch (e : IOException) {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(context, R.string.error, Toast.LENGTH_LONG).show()
                    }
                } finally {
                    cacheSaveDir.deleteRecursively()
                }
            }
        }

        /**
         * Deletes the save file for a given game.
         */
        fun deleteSaveFile(titleId : String?) : Boolean {
            if (titleId == null) return false
            File("$savesFolderRoot/$titleId").deleteRecursively()
            return true
        }
    }
}