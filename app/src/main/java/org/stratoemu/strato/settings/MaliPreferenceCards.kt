package org.stratoemu.strato.settings

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import androidx.preference.PreferenceCategory
import androidx.preference.PreferenceGroupAdapter
import androidx.recyclerview.widget.RecyclerView

/**
 * Lightweight settings-only decoration: navy rounded rows with blue outlines.
 * Does not alter Preference IDs, click listeners, switches, or saved settings.
 */
class MaliPreferenceCards(context: Context) : RecyclerView.ItemDecoration() {
    private val density = context.resources.displayMetrics.density
    private val gap = (6 * density).toInt()
    private val side = 12 * density
    private val radius = 18 * density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = android.graphics.Color.rgb(15, 38, 77) }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = android.graphics.Color.rgb(39, 101, 170)
        style = Paint.Style.STROKE
        strokeWidth = density
    }

    private fun category(parent: RecyclerView, position: Int): Boolean {
        if (position == RecyclerView.NO_POSITION) return true
        val adapter = parent.adapter as? PreferenceGroupAdapter ?: return false
        return adapter.getItem(position) is PreferenceCategory
    }

    override fun getItemOffsets(outRect: Rect, view: android.view.View, parent: RecyclerView,
                                state: RecyclerView.State) {
        if (!category(parent, parent.getChildAdapterPosition(view))) {
            outRect.top = gap
            outRect.bottom = gap
        }
    }

    override fun onDraw(canvas: Canvas, parent: RecyclerView, state: RecyclerView.State) {
        for (i in 0 until parent.childCount) {
            val child = parent.getChildAt(i)
            if (category(parent, parent.getChildAdapterPosition(child))) continue
            val rect = RectF(child.left.toFloat() + side, child.top.toFloat(),
                child.right.toFloat() - side, child.bottom.toFloat())
            canvas.drawRoundRect(rect, radius, radius, fill)
            canvas.drawRoundRect(rect, radius, radius, stroke)
        }
    }
}
