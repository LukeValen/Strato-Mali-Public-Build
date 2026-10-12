/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.text.InputFilter
import android.text.SpannableStringBuilder
import android.text.Spanned

class SoftwareKeyboardFilter(val config : SoftwareKeyboardConfig) : InputFilter {
    override fun filter(source : CharSequence, start : Int, end : Int, dest : Spanned, dstart : Int, dend : Int) : CharSequence? {
        val output = SpannableStringBuilder()
        val positions = IntArray(end - start + 1)
        val prefix = dest.subSequence(0, dstart).toString()
        val suffix = dest.subSequence(dend, dest.length).toString()
        var index = start
        while (index < end) {
            val original = Character.codePointAt(source, index)
            val count = Character.charCount(original).coerceAtMost(end - index)
            val codepoint = if (config.invalidCharsFlags.outsideOfDownloadCode) Character.toUpperCase(original) else original
            val character = String(Character.toChars(codepoint))
            for (i in 0 until count) positions[index - start + i] = output.length
            val candidate = prefix + output + character + suffix
            if (config.isValid(codepoint) && candidate.length <= config.textMaxLength.toInt() &&
                (!config.isUseUtf8 || candidate.toByteArray(Charsets.UTF_8).size < 0x7D4)) {
                output.append(character)
            }
            index += count
            positions[index - start] = output.length
        }
        if (output.toString() == source.subSequence(start, end).toString()) return null
        if (source is Spanned) {
            for (span in source.getSpans(start, end, Any::class.java)) {
                val from = source.getSpanStart(span).coerceIn(start, end) - start
                val to = source.getSpanEnd(span).coerceIn(start, end) - start
                if (positions[from] < positions[to])
                    output.setSpan(span, positions[from], positions[to], source.getSpanFlags(span))
            }
        }
        return output
    }
}
