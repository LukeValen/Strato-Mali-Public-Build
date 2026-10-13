/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.data

import androidx.annotation.ColorRes
import org.stratoemu.strato.R

/** Status values published by Strato Mali Compatibility's reviewed reports. */
enum class GameCompatibility(
    val label: String,
    @ColorRes val backgroundColor: Int,
    @ColorRes val foregroundColor: Int,
) {
    Playable("Playable", R.color.compat_playable, R.color.compat_text_light),
    InGame("In-game", R.color.compat_ingame, R.color.compat_text_dark),
    Boots("Boots", R.color.compat_boots, R.color.compat_text_light),
    Nothing("Nothing", R.color.compat_nothing, R.color.compat_text_light),
    Unplayable("Unplayable", R.color.compat_unplayable, R.color.compat_text_light),
    Unknown("Unknown", R.color.compat_unknown, R.color.compat_text_dark);

    companion object {
        fun fromSiteStatus(status: String): GameCompatibility? = when (status) {
            "playable" -> Playable
            "ingame" -> InGame
            "boots" -> Boots
            "nothing" -> Nothing
            "unplayable" -> Unplayable
            else -> null
        }
    }
}
