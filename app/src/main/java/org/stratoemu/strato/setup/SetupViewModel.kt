/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.setup

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.stratoemu.strato.KeyReader
import org.stratoemu.strato.R
import org.stratoemu.strato.settings.AppSettings
import java.io.File
import java.io.IOException

class SetupViewModel(application: Application, private val savedState: SavedStateHandle) : AndroidViewModel(application) {
    private val context = application
    val busy = MutableLiveData(false)
    val message = MutableLiveData("")
    val revision = MutableLiveData(0)
    val hasKeys get() = File(context.filesDir, "keys/prod.keys").let { it.isFile && it.length() > 0 }
    val firmwareVersion get() = PreferenceManager.getDefaultSharedPreferences(context).getString("firmware", "").orEmpty()
    var folder: String
        get() = savedState["folder"] ?: AppSettings(context).searchLocation
        private set(value) { savedState["folder"] = value }

    private fun runImport(progress: Int, task: suspend () -> String) {
        if (busy.value == true) return
        busy.value = true
        message.value = context.getString(progress)
        viewModelScope.launch {
            try {
                message.value = withContext(Dispatchers.IO) { task() }
            } catch (error: Exception) {
                Log.e("StratoSetup", "Import failed", error)
                message.value = context.getString(R.string.setup_failed, error.localizedMessage ?: error.javaClass.simpleName)
            } finally {
                busy.value = false
                revision.value = (revision.value ?: 0) + 1
            }
        }
    }

    fun importKeys(uri: Uri) = runImport(R.string.setup_importing_keys) {
        val result = KeyReader.import(context, uri, KeyReader.KeyType.Prod)
        if (result != KeyReader.ImportResult.Success) throw IOException(context.getString(R.string.import_keys_invalid_keys))
        context.getString(R.string.import_keys_success)
    }

    fun importFirmware(uri: Uri) = runImport(R.string.import_firmware_in_progress) {
        if (!hasKeys) throw IOException(context.getString(R.string.firmware_keys_needed))
        val version = FirmwareInstaller.install(context, uri)
        PreferenceManager.getDefaultSharedPreferences(context).edit().putString("firmware", version).apply()
        context.getString(R.string.setup_firmware_installed, version)
    }

    fun selectFolder(uri: Uri) = runImport(R.string.setup_checking_folder) {
        val directory = DocumentFile.fromTreeUri(context, uri)
        if (directory == null || !directory.isDirectory || !directory.canRead())
            throw IOException(context.getString(R.string.setup_unreadable_folder))
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        withContext(Dispatchers.Main) { folder = uri.toString() }
        context.getString(R.string.setup_folder_ready)
    }

    fun complete(): Boolean {
        if (busy.value == true || !hasKeys || folder.isBlank()) return false
        AppSettings(context).apply {
            searchLocation = folder
            refreshRequired = true
        }
        return true
    }
}
