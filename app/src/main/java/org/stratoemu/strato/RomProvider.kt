package org.stratoemu.strato

import android.annotation.SuppressLint
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.loader.RomFile
import org.stratoemu.strato.loader.RomFormat
import org.stratoemu.strato.loader.RomFormat.*
import java.util.ArrayDeque
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class RomProvider @Inject constructor(@ApplicationContext private val context : Context) {
    companion object {
        private val TAG = RomProvider::class.java.simpleName
    }

    /**
     * Adds supported ROM files without recursive calls so deeply nested or broken
     * document-provider trees cannot overflow the stack or abort the entire scan.
     */
    @SuppressLint("DefaultLocale")
    private fun addEntries(fileFormats : Map<String, RomFormat>, rootDirectory : DocumentFile, entries : ArrayList<AppEntry>, systemLanguage : Int) {
        val pendingDirectories = ArrayDeque<DocumentFile>()
        val visitedDirectories = HashSet<String>()
        pendingDirectories.add(rootDirectory)

        while (pendingDirectories.isNotEmpty()) {
            val directory = pendingDirectories.removeFirst()
            if (!visitedDirectories.add(directory.uri.toString()))
                continue

            val children = try {
                directory.listFiles()
            } catch (e : Exception) {
                Log.w(TAG, "Cannot enumerate ROM directory ${directory.uri}: ${e.message}")
                continue
            }

            children.forEach { file ->
                try {
                    if (file.isDirectory) {
                        pendingDirectories.addLast(file)
                    } else {
                        val fileName = file.name ?: return@forEach
                        val extension = fileName.substringAfterLast(".", "").lowercase()
                        fileFormats[extension]?.let { romFormat ->
                            try {
                                entries.add(RomFile(context, romFormat, file.uri, systemLanguage).appEntry)
                            } catch (e : Exception) {
                                Log.w(TAG, "Skipping unreadable ROM ${file.uri}: ${e.message}")
                            }
                        }
                    }
                } catch (e : Exception) {
                    Log.w(TAG, "Skipping inaccessible document ${file.uri}: ${e.message}")
                }
            }
        }
    }

    fun loadRoms(searchLocation : Uri, systemLanguage : Int) : ArrayList<AppEntry> {
        if (searchLocation.toString().isBlank())
            return arrayListOf()

        val documentFile = try {
            DocumentFile.fromTreeUri(context, searchLocation)
        } catch (e : Exception) {
            Log.w(TAG, "Invalid ROM directory URI: $searchLocation", e)
            null
        } ?: return arrayListOf()

        val usableDirectory = try {
            documentFile.exists() && documentFile.isDirectory && documentFile.canRead()
        } catch (e : Exception) {
            Log.w(TAG, "Cannot access ROM directory: $searchLocation", e)
            false
        }

        if (!usableDirectory)
            return arrayListOf()

        return arrayListOf<AppEntry>().apply {
            addEntries(mapOf("nro" to NRO, "nso" to NSO, "nca" to NCA, "nsp" to NSP, "xci" to XCI), documentFile, this, systemLanguage)
        }
    }
}
