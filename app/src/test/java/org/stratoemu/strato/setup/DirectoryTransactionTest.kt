/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.setup

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException

class DirectoryTransactionTest {
    @get:Rule val temp = TemporaryFolder()
    private fun directory(name: String, contents: String): File = temp.newFolder(name).also {
        File(it, "contents").writeText(contents)
    }
    private fun contents(directory: File) = File(directory, "contents").readText()

    @Test fun replacesBothDirectoriesAfterValidation() {
        val firmware = directory("firmware", "old firmware")
        val fonts = directory("fonts", "old fonts")
        val newFirmware = directory("newFirmware", "new firmware")
        val newFonts = directory("newFonts", "new fonts")
        DirectoryTransaction.replace(listOf(newFirmware to firmware, newFonts to fonts), "test")
        assertEquals("new firmware", contents(firmware))
        assertEquals("new fonts", contents(fonts))
        assertFalse(File(temp.root, ".firmware-backup-test").exists())
        assertFalse(File(temp.root, ".fonts-backup-test").exists())
    }

    @Test fun secondFailureRestoresBothPreviousDirectories() {
        val firmware = directory("firmware", "old firmware")
        val fonts = directory("fonts", "old fonts")
        val newFirmware = directory("newFirmware", "new firmware")
        val missingFonts = File(temp.root, "missingFonts")
        assertThrows(IOException::class.java) {
            DirectoryTransaction.replace(listOf(newFirmware to firmware, missingFonts to fonts), "test")
        }
        assertEquals("old firmware", contents(firmware))
        assertEquals("old fonts", contents(fonts))
    }

    @Test fun failedFirstInstallationDoesNotLeavePartialFirmware() {
        val firmware = File(temp.root, "firmware")
        val fonts = File(temp.root, "fonts")
        val newFirmware = directory("newFirmware", "new firmware")
        assertThrows(IOException::class.java) {
            DirectoryTransaction.replace(listOf(newFirmware to firmware, File(temp.root, "missing") to fonts), "test")
        }
        assertFalse(firmware.exists())
        assertFalse(fonts.exists())
    }

    @Test fun failedBackupDoesNotTouchExistingData() {
        val firmware = directory("firmware", "old firmware")
        directory(".firmware-backup-test", "occupied backup")
        val newFirmware = directory("newFirmware", "new firmware")
        assertThrows(IOException::class.java) {
            DirectoryTransaction.replace(listOf(newFirmware to firmware), "test")
        }
        assertEquals("old firmware", contents(firmware))
        assertEquals("new firmware", contents(newFirmware))
    }
}
