/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.adapter

import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.content.res.ColorStateList
import android.graphics.drawable.ColorDrawable
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.ImageView
import android.widget.RelativeLayout
import android.widget.TextView
import androidx.viewbinding.ViewBinding
import androidx.core.content.ContextCompat
import com.google.android.material.card.MaterialCardView
import org.stratoemu.strato.R
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.GameCompatibility
import org.stratoemu.strato.data.GameCompatibilityRepository
import java.util.Locale
import org.stratoemu.strato.databinding.AppItemGridBinding
import org.stratoemu.strato.databinding.AppItemGridCompactBinding
import org.stratoemu.strato.databinding.AppItemLinearBinding

sealed class LayoutType(val builder : (parent : ViewGroup) -> ViewBinding) {
    object List : LayoutType({ ListBinding(it) })
    object Grid : LayoutType({ GridBinding(it) })
    object GridCompact : LayoutType({ GridCompatBinding(it) })

    companion object {
        fun values() = arrayListOf(List, Grid, GridCompact)
    }
}

data class LayoutBindingFactory(private val layoutType : LayoutType) : ViewBindingFactory {
    override fun createBinding(parent : ViewGroup) = layoutType.builder(parent)
}

interface LayoutBinding<V : ViewBinding> : ViewBinding {
    val binding : V

    override fun getRoot() = binding.root

    val textTitle : TextView

    val textVersion : TextView

    val textAuthor : TextView

    val icon : ImageView

    val compatibilityBadge : TextView
}

class ListBinding(parent : ViewGroup) : LayoutBinding<AppItemLinearBinding> {
    override val binding = AppItemLinearBinding.inflate(parent.inflater(), parent, false)

    override val textTitle = binding.textTitle

    override val textVersion = binding.textVersion

    override val textAuthor = binding.textAuthor

    override val icon = binding.icon

    override val compatibilityBadge = binding.compatibilityBadge
}

class GridBinding(parent : ViewGroup) : LayoutBinding<AppItemGridBinding> {
    override val binding = AppItemGridBinding.inflate(parent.inflater(), parent, false)

    override val textTitle = binding.textTitle

    override val textVersion = binding.textVersion

    override val textAuthor = binding.textAuthor

    override val icon = binding.icon

    override val compatibilityBadge = binding.compatibilityBadge
}

class GridCompatBinding(parent : ViewGroup) : LayoutBinding<AppItemGridCompactBinding> {
    override val binding = AppItemGridCompactBinding.inflate(parent.inflater(), parent, false)

    override val textTitle = binding.textTitle

    override val textVersion = binding.textVersion

    override val textAuthor = binding.textAuthor

    override val icon = binding.icon

    override val compatibilityBadge = binding.compatibilityBadge
}

private typealias InteractionFunction = (appItem : AppItem) -> Unit

class AppViewItem(var layoutType : LayoutType, private val item : AppItem, private val onClick : InteractionFunction, private val onLongClick : InteractionFunction, private val isSelected : (AppItem) -> Boolean) : GenericListItem<LayoutBinding<*>>() {
    override fun getViewBindingFactory() = LayoutBindingFactory(layoutType)

    override fun bind(holder : GenericViewHolder<LayoutBinding<*>>, position : Int) {
        val binding = holder.binding
        binding.textTitle.text = item.title
        binding.textVersion.text = item.version ?: item.loaderResultString(binding.root.context)
        binding.textAuthor.text = item.author
        // Make text views selected for marquee to work
        binding.textTitle.isSelected = true
        binding.textVersion.isSelected = true
        binding.textAuthor.isSelected = true

        binding.icon.setImageBitmap(item.bitmapIcon)
        binding.icon.contentDescription = item.title
        // Every bind resets the label/color. RecyclerView reuses holders for
        // different games when the library is scrolled, searched or sorted.
        val summary = GameCompatibilityRepository.summaryFor(item.titleId)
        val compatibility = summary?.status ?: GameCompatibility.Unknown
        binding.compatibilityBadge.apply {
            text = if (summary == null) compatibility.label else
                context.getString(R.string.mali_compatibility_rating_badge, compatibility.label,
                    String.format(Locale.forLanguageTag("pt-BR"), "%.1f", summary.averageStars))
            backgroundTintList = ColorStateList.valueOf(
                ContextCompat.getColor(context, compatibility.backgroundColor)
            )
            setTextColor(ContextCompat.getColor(context, compatibility.foregroundColor))
            contentDescription = if (summary == null)
                context.getString(R.string.mali_compatibility_accessibility, compatibility.label)
            else context.getString(R.string.mali_compatibility_rating_accessibility,
                compatibility.label, String.format(Locale.ENGLISH, "%.1f", summary.averageStars), summary.reportCount)
        }

        if (layoutType != LayoutType.List) {
            binding.icon.scaleType = ImageView.ScaleType.CENTER_CROP
            val card = binding.root.findViewById<MaterialCardView>(R.id.item_card)
                ?: (binding.root as? MaterialCardView)
            card?.apply {
                strokeWidth = if (isSelected(item)) (3f * resources.displayMetrics.density).toInt() else 0
                strokeColor = ContextCompat.getColor(context, R.color.mali_cyan)
                cardElevation = if (isSelected(item)) 7f * resources.displayMetrics.density else 1f * resources.displayMetrics.density
            }
        }

        if (layoutType == LayoutType.List) {
            binding.icon.setOnClickListener { showIconDialog(it.context, item) }
        }

        val handleClicks = { view : View ->
            view.setOnClickListener { onClick.invoke(item) }
            view.setOnLongClickListener { true.also { onLongClick.invoke(item) } }
        }

        handleClicks(binding.root.findViewById(R.id.item_click_layout))
        binding.root.findViewById<View>(R.id.item_card)?.let { handleClicks(it) }
    }

    private fun showIconDialog(context : Context, appItem : AppItem) {
        val builder = Dialog(context)
        builder.requestWindowFeature(Window.FEATURE_NO_TITLE)
        builder.window!!.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))

        val imageView = ImageView(context)
        imageView.setImageBitmap(appItem.bitmapIcon)

        builder.addContentView(imageView, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        builder.show()
    }

    override fun key() = item.key()

    override fun areItemsTheSame(other : GenericListItem<LayoutBinding<*>>) = key() == other.key()

    override fun areContentsTheSame(other : GenericListItem<LayoutBinding<*>>) = other is AppViewItem && layoutType == other.layoutType && item == other.item
}
