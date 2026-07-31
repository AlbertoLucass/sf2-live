package com.sf2live.app

import android.content.Context
import java.io.File

class SoundFontLibrary(context: Context) {
    data class Entry(
        val file: File,
        val displayName: String,
        val sizeBytes: Long,
        val modifiedAt: Long,
    ) {
        val stablePath: String get() = file.absolutePath
    }

    private val folder = File(context.filesDir, "soundfonts").apply { mkdirs() }

    fun folder(): File = folder

    fun entries(): List<Entry> {
        return folder.listFiles()
            .orEmpty()
            .asSequence()
            .filter { it.isFile && it.extension.equals("sf2", ignoreCase = true) }
            .map {
                Entry(
                    file = it,
                    displayName = displayName(it.name),
                    sizeBytes = it.length(),
                    modifiedAt = it.lastModified(),
                )
            }
            .sortedByDescending { it.modifiedAt }
            .toList()
    }

    fun uniqueDestination(originalName: String): File {
        val safeName = originalName.replace(Regex("[^A-Za-z0-9._ -]"), "_")
        val base = safeName.substringBeforeLast('.', safeName).ifBlank { "soundfont" }
        val extension = safeName.substringAfterLast('.', "sf2")
        var candidate = File(folder, "$base.$extension")
        var index = 2
        while (candidate.exists()) {
            candidate = File(folder, "$base ($index).$extension")
            index += 1
        }
        return candidate
    }

    fun remove(entry: Entry): Boolean = entry.file.delete()

    private fun displayName(fileName: String): String {
        // Compatibilidade com arquivos importados pelas versões 0.1.x, que usavam timestamp.
        return fileName.replaceFirst(Regex("^\\d{10,}_"), "")
    }
}
