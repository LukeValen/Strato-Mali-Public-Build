/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.data

import kotlinx.serialization.json.*
import java.util.Locale

data class CommunityCompatibility(
    val status: GameCompatibility,
    val averageStars: Double,
    val reportCount: Int,
)

/** Website semantics: newest approved status and mean rating across all GPUs. */
object CompatibilityFeed {
    private val titleIdPattern = Regex("[0-9A-F]{16}")
    private val datePattern = Regex("\\d{4}-\\d{2}-\\d{2}")
    private val issuePattern = Regex("https://github\\.com/LukeValen/Strato-Mali-Compatibility/issues/(\\d+)")

    private data class Report(val status: GameCompatibility, val stars: Int, val date: String, val issue: Long)

    fun normalizeTitleId(value: String?): String? = value?.trim()?.uppercase(Locale.ROOT)
        ?.removePrefix("0X")?.takeIf { titleIdPattern.matches(it) }

    /** Invalid documents preserve the previous cache; a valid empty feed removes old ratings. */
    fun parse(bytes: ByteArray): Map<String, CommunityCompatibility>? {
        return try {
            val root = Json.parseToJsonElement(bytes.toString(Charsets.UTF_8)) as? JsonObject ?: return null
            val entries = root["reviews"] as? JsonArray ?: return null
            if (entries.size > 10000) return null
            val reports = mutableMapOf<String, MutableList<Report>>()
            for (entry in entries) {
                val review = entry as? JsonObject ?: continue
                fun string(key: String): String? = (review[key] as? JsonPrimitive)
                    ?.takeIf { it.isString }?.content
                val id = normalizeTitleId(string("titleId")) ?: continue
                val status = GameCompatibility.fromSiteStatus(string("status").orEmpty()) ?: continue
                val stars = (review["stars"] as? JsonPrimitive)?.takeUnless { it.isString }?.intOrNull ?: continue
                val gpu = string("gpu") ?: continue
                val date = string("date") ?: continue
                val issue = issuePattern.matchEntire(string("url").orEmpty())?.groupValues?.get(1)?.toLongOrNull() ?: continue
                if (stars !in 0..5 || gpu.length > 100 || !datePattern.matches(date)) continue
                reports.getOrPut(id) { mutableListOf() }.add(Report(status, stars, date, issue))
            }
            reports.mapValues { (_, list) ->
                val newest = list.maxWithOrNull(compareBy<Report> { it.date }.thenBy { it.issue })!!
                CommunityCompatibility(newest.status, list.map { it.stars }.average(), list.size)
            }
        } catch (_: Exception) {
            null
        }
    }
}
