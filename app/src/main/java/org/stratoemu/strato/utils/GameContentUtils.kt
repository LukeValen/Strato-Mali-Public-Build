/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.utils

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.StatFs
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import org.stratoemu.strato.getPublicFilesDir
import org.stratoemu.strato.loader.getRomFormat
import java.io.File
import java.io.FileOutputStream
import java.security.MessageDigest
import kotlin.coroutines.coroutineContext

object GameContentUtils {
    private external fun validatePackage(contentFd : Int, gameFd : Int, gameFormat : Int, keysPath : String, gameId : String) : String?

    private fun directory(context : Context, titleId : String) : File {
        require(titleId.matches(Regex("[0-9a-fA-F]{16}"))) { "Invalid game title ID" }
        return File(context.getPublicFilesDir(), "contents/${titleId.uppercase()}")
    }

    fun installed(context : Context, titleId : String) : List<File> =
        directory(context, titleId).listFiles()?.filter {
            it.isFile && !java.nio.file.Files.isSymbolicLink(it.toPath()) &&
                it.extension.equals("nsp", ignoreCase = true) && it.length() > 0L
        }?.sortedBy { it.name } ?: emptyList()

    fun label(context : Context, file : File) : String =
        context.getSharedPreferences("game_content_labels", Context.MODE_PRIVATE).getString(file.name, null) ?: file.name

    fun remove(context : Context, file : File) {
        val contentRoot = File(context.getPublicFilesDir(), "contents").canonicalFile
        require(file.isFile && file.extension.equals("nsp", ignoreCase = true) &&
                    !java.nio.file.Files.isSymbolicLink(file.toPath()) &&
                    file.canonicalPath.startsWith(contentRoot.path + File.separator)) {
            "Cannot remove a file outside installed game content"
        }
        check(file.delete()) { "Could not remove installed content" }
        context.getSharedPreferences("game_content_labels", Context.MODE_PRIVATE).edit().remove(file.name).apply()
    }

    suspend fun install(context : Context, game : Uri, titleId : String, uri : Uri, progress : (Long, Long) -> Unit) : Boolean = withContext(Dispatchers.IO) {
        val destination = directory(context, titleId)
        check(destination.mkdirs() || destination.isDirectory) { "Could not create content directory" }
        var name = "content.nsp"
        var expected = -1L
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                val sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE)
                if (nameColumn >= 0 && !cursor.isNull(nameColumn)) name = cursor.getString(nameColumn)
                if (sizeColumn >= 0 && !cursor.isNull(sizeColumn)) expected = cursor.getLong(sizeColumn)
            }
        }
        require(name.endsWith(".nsp", ignoreCase = true)) { "Select an uncompressed Update or DLC NSP" }
        if (expected > 0) check(StatFs(destination.absolutePath).availableBytes >= expected) { "Not enough free space to install this content" }
        val temporary = File.createTempFile("install-", ".part", destination)
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            var copied = 0L
            context.contentResolver.openInputStream(uri)?.use { input ->
                FileOutputStream(temporary).use { output ->
                    val buffer = ByteArray(1024 * 1024)
                    while (true) {
                        coroutineContext.ensureActive()
                        val count = input.read(buffer)
                        if (count < 0) break
                        if (count == 0) continue
                        output.write(buffer, 0, count)
                        digest.update(buffer, 0, count)
                        copied += count
                        progress(copied, expected)
                    }
                    output.fd.sync()
                }
            } ?: error("Could not read the selected content file")
            check(copied > 0 && temporary.length() == copied && (expected < 0 || copied == expected)) {
                "The content file could not be copied completely"
            }
            coroutineContext.ensureActive()
            progress(-1, expected)
            ParcelFileDescriptor.open(temporary, ParcelFileDescriptor.MODE_READ_ONLY).use { contentFd ->
                context.contentResolver.openFileDescriptor(game, "r")?.use { gameFd ->
                    val format = getRomFormat(game, context.contentResolver)
                    val error = validatePackage(contentFd.fd, gameFd.fd, format.ordinal, "${context.filesDir.canonicalPath}/keys/", titleId)
                    check(error == null) { error ?: "Invalid content package" }
                } ?: error("Could not open the base game; select the game folder again")
            }
            coroutineContext.ensureActive()
            val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
            val target = File(destination, "$hash.nsp")
            if (target.exists()) return@withContext false
            check(temporary.renameTo(target)) { "Could not finish installing content" }
            context.getSharedPreferences("game_content_labels", Context.MODE_PRIVATE).edit().putString(target.name, name).apply()
            true
        } finally {
            temporary.delete()
        }
    }
}
