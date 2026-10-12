/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.util.ArrayDeque

object KeyReader {
    private val Tag = KeyReader::class.java.simpleName

    enum class ImportResult {
        Success,
        InvalidInputPath,
        InvalidKeys,
        DeletePreviousFailed,
        MoveFailed,
    }

    enum class KeyType(val keyName : String, val fileName : String) {
        Title("title_keys", "title.keys"), Prod("prod_keys", "prod.keys");

        companion object {
            fun parse(keyName : String) = values().first { it.keyName == keyName }
            fun parse(documentFile : DocumentFile) = values().first { it.fileName == documentFile.name }
            fun parseOrNull(documentFile : DocumentFile) = values().find { it.fileName == documentFile.name }
        }
    }

    fun importFromLocation(context : Context, searchLocation : Uri) {
        val directory = DocumentFile.fromTreeUri(context, searchLocation)
        if (directory == null || !directory.exists() || !directory.isDirectory) {
            Log.w(Tag, "Ignoring invalid key import directory: $searchLocation")
            return
        }
        importFromDirectory(context, directory)
    }

    private fun importFromDirectory(context : Context, rootDirectory : DocumentFile) {
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
                Log.w(Tag, "Cannot enumerate directory ${directory.uri}: ${e.message}")
                continue
            }

            children.forEach { file ->
                try {
                    if (file.isDirectory) {
                        pendingDirectories.addLast(file)
                    } else {
                        KeyType.parseOrNull(file)?.let { import(context, file.uri, it) }
                    }
                } catch (e : Exception) {
                    Log.w(Tag, "Skipping inaccessible key candidate ${file.uri}: ${e.message}")
                }
            }
        }
    }

    /**
     * Reads keys file, trims and writes to internal app data storage, it makes sure file is properly formatted
     */
    fun import(context : Context, uri : Uri, keyType : KeyType) : ImportResult {
        Log.i(Tag, "Parsing ${keyType.name} $uri")

        if (!DocumentFile.isDocumentUri(context, uri))
            return ImportResult.InvalidInputPath

        val outputDirectory = File("${context.filesDir.canonicalFile}/keys/")
        if (!outputDirectory.exists())
            outputDirectory.mkdirs()

        val outputFile = File(outputDirectory, keyType.fileName)
        val tmpOutputFile = File("${outputFile}.tmp")
        var valid = false
        var keyCount = 0

        context.contentResolver.openInputStream(uri).use { inputStream ->
            tmpOutputFile.bufferedWriter().use { writer ->
                valid = inputStream!!.bufferedReader().useLines {
                    for (line in it) {
                        if (line.startsWith(";") || line.isBlank()) continue

                        val pair = line.split("=")
                        if (pair.size != 2)
                            return@useLines false

                        val key = pair[0].trim()
                        val value = pair[1].trim()
                        when (keyType) {
                            KeyType.Title -> {
                                if (key.length != 32 && !isHexString(key))
                                    return@useLines false
                                if (value.length != 32 && !isHexString(value))
                                    return@useLines false
                            }
                            KeyType.Prod -> {
                                if (!key.contains("_"))
                                    return@useLines false
                                if (value.isEmpty() || !isHexString(value))
                                    return@useLines false
                            }
                        }

                        keyCount++
                        writer.append("$key=$value\n")
                    }
                    keyCount > 0
                }
            }
        }

        val cleanup = {
            try {
                tmpOutputFile.delete()
            } catch (_ : Exception) {
            }
        }

        if (!valid) {
            cleanup()
            return ImportResult.InvalidKeys
        }

        // Both files are in the same directory. Android rename replaces the old file atomically;
        // keep the previous keys intact if the replacement fails.
        if (!tmpOutputFile.renameTo(outputFile)) {
            cleanup()
            return ImportResult.MoveFailed
        }

        return ImportResult.Success
    }

    private fun isHexString(str : String) : Boolean {
        for (c in str)
            if (!(c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F')) return false
        return true
    }
}
