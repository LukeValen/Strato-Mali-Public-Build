/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.preference

import android.content.Context
import android.content.Intent
import android.util.AttributeSet
import androidx.preference.Preference
import org.stratoemu.strato.setup.SetupActivity

class SetupPreference @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : Preference(context, attrs) {
    override fun onClick() { context.startActivity(Intent(context, SetupActivity::class.java)) }
}
