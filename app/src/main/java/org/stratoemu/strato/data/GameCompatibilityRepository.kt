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
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** Reads the same reviewed, static JSON feed that the public compatibility site displays. */
object GameCompatibilityRepository {
    private const val TAG = "GameCompatibility"
    private const val FEED_URL = "https://lukevalen.github.io/Strato-Mali-Compatibility/data/reviews.json"
    private const val CACHE_FILE = "community_compatibility.json"
    private const val MAX_BYTES = 2 * 1024 * 1024
    private const val SUCCESS_REFRESH_MS = 60 * 1000L
    private const val RETRY_REFRESH_MS = 30 * 1000L
    private val mutex = Mutex()

    @Volatile private var reports: Map<String, CommunityCompatibility> = emptyMap()
    private var cacheLoaded = false
    private var nextRefreshElapsed = 0L

    /** Same community summary as the website without a GPU filter. */
    fun summaryFor(titleId: String?): CommunityCompatibility? =
        CompatibilityFeed.normalizeTitleId(titleId)?.let { reports[it] }

    fun statusFor(titleId: String?): GameCompatibility =
        summaryFor(titleId)?.status ?: GameCompatibility.Unknown

    /** Return true when cached or downloaded reports changed the visible badges. */
    suspend fun refresh(context: Context, force: Boolean = false): Boolean = mutex.withLock {
        withContext(Dispatchers.IO) {
            val cache = AtomicFile(context.applicationContext.filesDir.resolve(CACHE_FILE))
            var changed = false
            if (!cacheLoaded) {
                cacheLoaded = true
                try {
                    val cached = CompatibilityFeed.parse(cache.openRead().use(::readLimited))
                    if (cached != null && cached != reports) {
                        reports = cached
                        changed = true
                    }
                } catch (_: Exception) {
                    // The published feed is fetched below; an invalid cache has no authority.
                }
            }

            val now = SystemClock.elapsedRealtime()
            if (!force && nextRefreshElapsed != 0L && now < nextRefreshElapsed) return@withContext changed
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
                val updated = CompatibilityFeed.parse(bytes) ?: throw IOException("Invalid compatibility feed")
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

}
