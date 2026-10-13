/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.data

import android.content.Context
import android.os.SystemClock
import android.util.AtomicFile
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale

/** Reads the same reviewed, static JSON feed that the public compatibility site displays. */
object GameCompatibilityRepository {
    private const val TAG = "GameCompatibility"
    private const val FEED_URL = "https://lukevalen.github.io/Strato-Mali-Compatibility/data/reviews.json"
    private const val CACHE_FILE = "community_compatibility.json"
    private const val MAX_BYTES = 2 * 1024 * 1024
    private const val SUCCESS_REFRESH_MS = 30 * 60 * 1000L
    private const val RETRY_REFRESH_MS = 5 * 60 * 1000L
    private val titleIdPattern = Regex("[0-9A-F]{16}")
    private val datePattern = Regex("\\d{4}-\\d{2}-\\d{2}")
    private val gpuPattern = Regex("\\b(?:Mali-|Immortalis-)?G(\\d{2,4})\\b", RegexOption.IGNORE_CASE)
    private val issuePattern = Regex("https://github\\.com/LukeValen/Strato-Mali-Compatibility/issues/(\\d+)")
    private val mutex = Mutex()

    private data class Report(val gpuModel: String, val status: GameCompatibility, val date: String, val issue: Long)

    @Volatile private var reports: Map<String, List<Report>> = emptyMap()
    @Volatile private var deviceGpuModel: String? = null
    private var cacheLoaded = false
    private var nextRefreshElapsed = 0L

    fun setDeviceGpuName(name: String?) {
        deviceGpuModel = gpuPattern.find(name.orEmpty())?.groupValues?.get(1)
    }

    /** A report from another GPU must never be presented as this device's compatibility. */
    fun statusFor(titleId: String?): GameCompatibility {
        val id = titleId?.trim()?.uppercase(Locale.ROOT) ?: return GameCompatibility.Unknown
        val gpu = deviceGpuModel ?: return GameCompatibility.Unknown
        if (!titleIdPattern.matches(id)) return GameCompatibility.Unknown
        return reports[id]?.asSequence()?.filter { it.gpuModel == gpu }
            ?.maxWithOrNull(compareBy<Report> { it.date }.thenBy { it.issue })?.status
            ?: GameCompatibility.Unknown
    }

    /** Return true when cached or downloaded reports changed the visible badges. */
    suspend fun refresh(context: Context): Boolean = mutex.withLock {
        withContext(Dispatchers.IO) {
            val cache = AtomicFile(context.applicationContext.filesDir.resolve(CACHE_FILE))
            var changed = false
            if (!cacheLoaded) {
                cacheLoaded = true
                try {
                    val cached = parse(cache.openRead().use(::readLimited))
                    if (cached != null && cached != reports) {
                        reports = cached
                        changed = true
                    }
                } catch (_: Exception) {
                    // The published feed is fetched below; an invalid cache has no authority.
                }
            }

            val now = SystemClock.elapsedRealtime()
            if (nextRefreshElapsed != 0L && now < nextRefreshElapsed) return@withContext changed
            nextRefreshElapsed = now + RETRY_REFRESH_MS
            try {
                val connection = (URL(FEED_URL).openConnection() as HttpURLConnection).apply {
                    connectTimeout = 5000
                    readTimeout = 5000
                    instanceFollowRedirects = false
                    useCaches = false
                    setRequestProperty("Accept", "application/json")
                    setRequestProperty("Cache-Control", "no-cache")
                }
                val bytes = try {
                    if (connection.responseCode != HttpURLConnection.HTTP_OK) throw IOException("HTTP ${connection.responseCode}")
                    connection.inputStream.use(::readLimited)
                } finally {
                    connection.disconnect()
                }
                val updated = parse(bytes) ?: throw IOException("Invalid compatibility feed")
                if (updated != reports) {
                    reports = updated
                    changed = true
                }
                val output = cache.startWrite()
                try {
                    output.write(bytes)
                    cache.finishWrite(output)
                } catch (error: Exception) {
                    cache.failWrite(output)
                    throw error
                }
                nextRefreshElapsed = SystemClock.elapsedRealtime() + SUCCESS_REFRESH_MS
            } catch (error: Exception) {
                Log.w(TAG, "Could not refresh reviewed compatibility reports; keeping cache", error)
            }
            changed
        }
    }

    private fun readLimited(stream: java.io.InputStream): ByteArray {
        val output = ByteArrayOutputStream()
        val chunk = ByteArray(8192)
        while (true) {
            val read = stream.read(chunk)
            if (read < 0) break
            if (output.size() + read > MAX_BYTES) throw IOException("Compatibility feed too large")
            output.write(chunk, 0, read)
        }
        return output.toByteArray()
    }

    private fun parse(bytes: ByteArray): Map<String, List<Report>>? = try {
        val entries = JSONObject(bytes.toString(Charsets.UTF_8)).getJSONArray("reviews")
        if (entries.length() > 10000) null else {
            val result = mutableMapOf<String, MutableList<Report>>()
            for (index in 0 until entries.length()) {
                val review = entries.optJSONObject(index) ?: continue
                val id = review.optString("titleId")
                val gpu = gpuPattern.find(review.optString("gpu"))?.groupValues?.get(1)
                val status = GameCompatibility.fromSiteStatus(review.optString("status"))
                val date = review.optString("date")
                val issue = issuePattern.matchEntire(review.optString("url"))?.groupValues?.get(1)?.toLongOrNull()
                if (!titleIdPattern.matches(id) || gpu == null || status == null ||
                    !datePattern.matches(date) || issue == null) continue
                result.getOrPut(id) { mutableListOf() }.add(Report(gpu, status, date, issue))
            }
            result
        }
    } catch (_: Exception) {
        null
    }
}
