/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.setup

import java.io.File
import java.io.IOException

/** Stage on the destination filesystem, then replace; roll back if either rename fails. */
internal object DirectoryTransaction {
    fun replace(entries: List<Pair<File, File>>, id: String) {
        val backups = mutableListOf<Pair<File, File>>()
        val installed = mutableListOf<File>()
        try {
            for ((source, target) in entries) {
                if (target.exists()) {
                    val backup = File(target.parentFile, ".${target.name}-backup-$id")
                    if (!target.renameTo(backup)) throw IOException("Unable to preserve installed ${target.name}")
                    backups += backup to target
                }
                if (!source.renameTo(target)) throw IOException("Unable to install ${target.name}")
                installed += target
            }
        } catch (error: Exception) {
            installed.asReversed().forEach { it.deleteRecursively() }
            backups.asReversed().forEach { (backup, target) ->
                if (!backup.renameTo(target)) error.addSuppressed(IOException("Previous files preserved at ${backup.path}"))
            }
            throw error
        }
        backups.forEach { (backup, _) -> backup.deleteRecursively() }
    }

}
