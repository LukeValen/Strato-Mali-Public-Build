/*
 * SPDX-License-Identifier: MPL-2.0
 * Compatibility labels are a small, curated Strato Mali library feature.
 */
package org.stratoemu.strato.data

import androidx.annotation.ColorRes
import org.stratoemu.strato.R
import java.util.Locale

enum class GameCompatibility(
    val label: String,
    @ColorRes val backgroundColor: Int,
    @ColorRes val foregroundColor: Int,
) {
    Perfect("Perfect", R.color.compat_perfect, R.color.compat_text_dark),
    Playable("Playable", R.color.compat_playable, R.color.compat_text_light),
    InGame("In-game", R.color.compat_ingame, R.color.compat_text_dark),
    Nothing("Nothing", R.color.compat_nothing, R.color.compat_text_light),
    Unknown("Unknown", R.color.compat_unknown, R.color.compat_text_dark);

    companion object {
        // Switch application (base-game) Title IDs. Updates/DLC use the game's
        // base entry metadata, not their own independent compatibility rating.
        private val byTitleId = mapOf(
            "01002B30028F6000" to Perfect,   // Celeste
            "01005CA01580E000" to Perfect,   // Persona 5 Royal (global)
            "01004B10157F2000" to Perfect,   // Persona 5 Royal (Asia)
            "0100B880154FC000" to Perfect,   // Persona 5 Royal (Japan)
            "010062B01525C000" to Playable,  // Persona 4 Golden
            "0100B8E016F76000" to InGame,   // NieR:Automata
            "010003F003A34000" to Perfect,   // Pokémon Let's Go Pikachu
        )

        // Only use an exact title match when the ROM could not supply a title
        // ID (e.g. partially parsed homebrew metadata or a file entry).
        // No fuzzy/substring matching, and never override a valid Title ID.
        private val withoutTitleId = mapOf(
            "celeste" to Perfect,
            "persona 5 royal" to Perfect,
            "persona 4 golden" to Playable,
            "nier:automata the end of yorha edition" to InGame,
            "nier automata" to InGame,
            "pokémon: let's go, pikachu!" to Perfect,
            "pokemon: let's go, pikachu!" to Perfect,
            "pokémon lets go pikachu" to Perfect,
            "pokemon lets go pikachu" to Perfect,
        )

        fun fromGame(titleId: String?, title: String?): GameCompatibility {
            val id = titleId?.trim()?.uppercase(Locale.ROOT)?.removePrefix("0X")
            if (id != null && id.matches(Regex("[0-9A-F]{16}")))
                return byTitleId[id] ?: Unknown
            val normalizedTitle = title?.trim()?.lowercase(Locale.ROOT) ?: return Unknown
            return withoutTitleId[normalizedTitle] ?: Unknown
        }
    }
}
