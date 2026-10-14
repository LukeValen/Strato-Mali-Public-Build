/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.data

import org.junit.Assert.*
import org.junit.Test

class CompatibilityFeedTest {
    private val id = "01002B30028F6000"
    private fun review(stars: Int = 5, status: String = "playable", gpu: String = "Mali-G615 MC6",
                       issue: Int = 2, date: String = "2026-10-13", titleId: String = id) =
        """{"titleId":"$titleId","status":"$status","stars":$stars,"gpu":"$gpu","date":"$date",
            "url":"https://github.com/LukeValen/Strato-Mali-Compatibility/issues/$issue"}"""
    private fun parse(vararg reports: String) = CompatibilityFeed.parse(
        """{"reviews":[${reports.joinToString(",") }]}""".toByteArray())!!

    @Test fun celesteApprovedReportHasFiveStarsWithoutDeviceGpuDetection() {
        assertEquals(CommunityCompatibility(GameCompatibility.Playable, 5.0, 1), parse(review())[id])
    }

    @Test fun matchesWebsiteAverageAndLatestStatusAcrossGpuModels() {
        val result = parse(review(5, "playable", "Mali-G615 MC6", 9),
            review(0, "ingame", "Mali G720 MC8", 10),
            review(1, "boots", "Unrecognized GPU", 99, "2026-10-12"))[id]!!
        assertEquals(GameCompatibility.InGame, result.status)
        assertEquals(2.0, result.averageStars, 0.0001)
        assertEquals(3, result.reportCount)
    }

    @Test fun acceptsNormalizedIdsAndKeepsDifferentGamesSeparate() {
        assertEquals(id, CompatibilityFeed.normalizeTitleId(" 0x01002b30028f6000 "))
        assertNull(CompatibilityFeed.normalizeTitleId("Celeste"))
        assertNull(CompatibilityFeed.normalizeTitleId(null))
        val result = parse(review(titleId = id.lowercase()), review(titleId = "01007EF00011E000", stars = 0))
        assertEquals(5.0, result[id]!!.averageStars, 0.0)
        assertEquals(0.0, result["01007EF00011E000"]!!.averageStars, 0.0)
        assertNull(result["0100B8E016F76000"])
    }

    @Test fun rejectsInvalidRatingsAndUntrustedReportLinks() {
        assertTrue(parse(review(-1), review(6), review(status = "invalid"),
            review().replace("github.com", "example.com"),
            review().replace("\"stars\":5", "\"stars\":\"5\""),
            review().replace("\"stars\":5", "\"stars\":1.5"),
            review(titleId = "bad")).isEmpty())
    }

    @Test fun distinguishesBrokenFeedFromValidEmptyFeedAndSkipsMalformedEntries() {
        assertNull(CompatibilityFeed.parse("not JSON".toByteArray()))
        assertNull(CompatibilityFeed.parse("{}".toByteArray()))
        assertNull(CompatibilityFeed.parse("{\"reviews\":{}}".toByteArray()))
        assertTrue(parse().isEmpty())
        assertEquals(1, parse("null", "{}", review()).size)
    }
}
