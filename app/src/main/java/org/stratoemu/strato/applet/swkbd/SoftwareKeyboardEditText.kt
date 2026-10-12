/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.applet.swkbd

import android.content.Context
import android.util.AttributeSet
import com.google.android.material.textfield.TextInputEditText

class SoftwareKeyboardEditText @JvmOverloads constructor(
    context : Context, attrs : AttributeSet? = null, defStyleAttr : Int = android.R.attr.editTextStyle
) : TextInputEditText(context, attrs, defStyleAttr) {
    var selectionChanged : (() -> Unit)? = null
    override fun onSelectionChanged(selStart : Int, selEnd : Int) {
        super.onSelectionChanged(selStart, selEnd)
        selectionChanged?.invoke()
    }
}
