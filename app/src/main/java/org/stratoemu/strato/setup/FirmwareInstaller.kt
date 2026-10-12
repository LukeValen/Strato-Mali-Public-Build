/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.setup

import android.content.Context
import android.net.Uri
import org.stratoemu.strato.getPublicFilesDir
import org.stratoemu.strato.utils.ZipUtils
import java.io.File
import java.io.IOException
import java.util.UUID

/** Uses the existing NCA/version/font readers; validates everything before replacing installed data. */
object FirmwareInstaller {
    fun install(context: Context, uri: Uri): String {
        val root = context.getPublicFilesDir()
        val firmware = File(root, "switch/nand/system/Contents/registered")
        val fonts = File(root, "fonts")
        val id = UUID.randomUUID().toString()
        val stagedFirmware = File(firmware.parentFile, ".firmware-$id")
        val stagedFonts = File(root, ".fonts-$id")
        val keys = File(context.filesDir, "keys").path
        try {
            if (!stagedFirmware.mkdirs() || !stagedFonts.mkdirs())
                throw IOException("Unable to create installation directories")
            val stream = context.contentResolver.openInputStream(uri)
                ?: throw IOException("Unable to read the selected archive")
            stream.use { ZipUtils.unzip(it, stagedFirmware) }
            val files = stagedFirmware.listFiles()
            if (files.isNullOrEmpty() || files.any { !it.isFile || !it.name.endsWith(".nca", true) || it.length() == 0L })
                throw IOException("Select a ZIP containing firmware NCA files")
            val version = fetchFirmwareVersion(stagedFirmware.path, keys)
            if (version.isBlank()) throw IOException("Firmware could not be verified with the installed prod.keys")
            extractFonts(stagedFirmware.path, keys, stagedFonts.path)
            DirectoryTransaction.replace(listOf(stagedFirmware to firmware, stagedFonts to fonts), id)
            return version
        } finally {
            stagedFirmware.deleteRecursively()
            stagedFonts.deleteRecursively()
        }
    }

    private external fun fetchFirmwareVersion(systemArchivesPath: String, keysPath: String): String
    private external fun extractFonts(systemArchivesPath: String, keysPath: String, fontsPath: String)
}
