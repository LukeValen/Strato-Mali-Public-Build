/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.annotation.SuppressLint
import android.content.Context
import android.content.DialogInterface
import android.os.Bundle
import android.text.InputType
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import androidx.annotation.Keep
import androidx.core.widget.doOnTextChanged
import androidx.fragment.app.DialogFragment
import org.stratoemu.strato.databinding.KeyboardDialogBinding
import org.stratoemu.strato.utils.parcelable
import org.stratoemu.strato.utils.stringFromChars

@Keep
class SoftwareKeyboardDialog : DialogFragment() {
    private val config by lazy { requireArguments().parcelable<SoftwareKeyboardConfig>("config")!! }
    private val initialText by lazy { requireArguments().getString("initialText")!! }
    private val session = SoftwareKeyboardSession()
    private val inlineToken by lazy { requireArguments().getLong("inlineToken") }
    private var inlineRevision = 0L
    private var inlineFinished = false
    private var suppressInlineEvents = false
    private var lastInlineText = ""
    private var lastInlineCursor = 0
    private val sendInlineUpdate = Runnable {
        if (inlineToken != 0L && !inlineFinished && !suppressInlineEvents && isAdded) {
            val text = binding.textInput.text.toString()
            val cursor = binding.textInput.selectionEnd.coerceAtLeast(0)
            val changed = text != lastInlineText
            if (changed || cursor != lastInlineCursor) {
                lastInlineText = text
                lastInlineCursor = cursor
                submitInlineInput(inlineToken, if (changed) 2 else 3, text, cursor, inlineRevision)
            }
        }
    }

    private external fun submitInlineInput(token : Long, type : Int, text : String, cursor : Int, revision : Long)

    companion object {
        /**
         * @param config Holds the [SoftwareKeyboardConfig] that will be used to create the keyboard  between instances of this dialog
         * @param initialText Holds the text that was set by the guest when the dialog was created
         */
        fun newInstance(config : SoftwareKeyboardConfig, initialText : String) : SoftwareKeyboardDialog {
            val args = Bundle()
            args.putParcelable("config", config)
            args.putString("initialText", initialText)
            val fragment = SoftwareKeyboardDialog()
            fragment.arguments = args
            return fragment
        }

        fun newInlineInstance(config : SoftwareKeyboardConfig, initialText : String, token : Long, cursor : Int, revision : Long) =
            newInstance(config, initialText).apply {
                requireArguments().putLong("inlineToken", token)
                requireArguments().putInt("inlineCursor", cursor)
                requireArguments().putLong("inlineRevision", revision)
            }

        const val validationConfirm = 2
        const val validationError = 1
    }

    private lateinit var binding : KeyboardDialogBinding

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        isCancelable = !config.isCancelButtonDisabled
        inlineRevision = requireArguments().getLong("inlineRevision")
    }

    override fun onCreateView(inflater : LayoutInflater, container : ViewGroup?, savedInstanceState : Bundle?) =
        KeyboardDialogBinding.inflate(inflater, container, false).also { binding = it }.root

    @SuppressLint("SetTextI18n")
    override fun onViewCreated(view : View, savedInstanceState : Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        if (config.inputFormMode == InputFormMode.OneLine) {
            binding.header.text = stringFromChars(config.headerText)
            binding.header.visibility = View.VISIBLE
            binding.sub.text = stringFromChars(config.subText)
            binding.sub.visibility = View.VISIBLE
        }

        if (config.keyboardMode == KeyboardMode.Numeric) {
            binding.textInput.inputType = if (config.passwordMode == PasswordMode.Hide) InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD else InputType.TYPE_CLASS_NUMBER
        } else {
            binding.textInput.inputType = if (config.passwordMode == PasswordMode.Hide) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD else InputType.TYPE_CLASS_TEXT
            if (config.invalidCharsFlags.outsideOfDownloadCode)
                binding.textInput.inputType = binding.textInput.inputType or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
            if (config.isUseNewLine)
                binding.textInput.inputType = binding.textInput.inputType or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            else
                binding.textInput.inputType = binding.textInput.inputType and InputType.TYPE_TEXT_FLAG_MULTI_LINE.inv()
        }

        val okText = stringFromChars(config.okText)
        if (okText.isNotBlank())
            binding.okButton.text = okText
        val guideText = stringFromChars(config.guideText)
        if (guideText.isNotBlank())
            binding.inputLayout.hint = guideText

        binding.textInput.filters = arrayOf(SoftwareKeyboardFilter(config))
        binding.textInput.setText(initialText)
        binding.textInput.setSelection(if (config.initialCursorPos == InitialCursorPos.First) 0 else binding.textInput.length())
        binding.textInput.doOnTextChanged { text, _, _, _ ->
            binding.okButton.isEnabled = config.isValid(text!!)
            binding.lengthStatus.text = "${text.length}/${config.textMaxLength}"
            queueInlineUpdate()
        }
        binding.textInput.selectionChanged = ::queueInlineUpdate
        if (inlineToken != 0L)
            binding.textInput.setSelection(requireArguments().getInt("inlineCursor").coerceIn(0, binding.textInput.length()))
        lastInlineText = binding.textInput.text.toString()
        lastInlineCursor = binding.textInput.selectionEnd.coerceAtLeast(0)
        binding.lengthStatus.text = "${binding.textInput.length()}/${config.textMaxLength}"
        binding.okButton.isEnabled = config.isValid(binding.textInput.text.toString())
        binding.okButton.setOnClickListener { submit(false) }
        binding.cancelButton.visibility = if (config.isCancelButtonDisabled) View.GONE else View.VISIBLE
        binding.cancelButton.setOnClickListener { submit(true) }
        if (!config.isUseNewLine) {
            binding.textInput.imeOptions = binding.textInput.imeOptions or EditorInfo.IME_ACTION_DONE
            binding.textInput.setOnEditorActionListener { _, action, _ ->
                if (action == EditorInfo.IME_ACTION_DONE) {
                    submit(false)
                    true
                } else false
            }
        }
    }

    private fun submit(cancelled : Boolean) {
        val text = binding.textInput.text.toString()
        if (!cancelled && !config.isValid(text)) return
        if (inlineToken != 0L) {
            if (inlineFinished) return
            binding.textInput.removeCallbacks(sendInlineUpdate)
            inlineFinished = true
            // Final packets include the full text; never rely on a queued change event.
            submitInlineInput(inlineToken, if (cancelled) 6 else 5, text,
                binding.textInput.selectionEnd.coerceAtLeast(0), inlineRevision)
            return
        }
        if (session.submit(SoftwareKeyboardResult(cancelled, text))) {
            binding.okButton.isEnabled = false
            binding.cancelButton.isEnabled = false
            binding.textInput.isEnabled = false
            (requireContext().getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
                .hideSoftInputFromWindow(binding.textInput.windowToken, 0)
        }
    }

    private fun queueInlineUpdate() {
        if (inlineToken == 0L || suppressInlineEvents || inlineFinished) return
        binding.textInput.removeCallbacks(sendInlineUpdate)
        binding.textInput.post(sendInlineUpdate)
    }

    fun updateFromGuest(text : String, cursor : Int, revision : Long) {
        // Native updates may be posted before showNow has created the view.
        requireArguments().putString("initialText", text)
        requireArguments().putInt("inlineCursor", cursor)
        requireArguments().putLong("inlineRevision", revision)
        inlineRevision = revision
        inlineFinished = false
        if (!::binding.isInitialized) return
        binding.textInput.removeCallbacks(sendInlineUpdate)
        suppressInlineEvents = true
        binding.textInput.setText(text)
        binding.textInput.setSelection(cursor.coerceIn(0, binding.textInput.length()))
        lastInlineText = binding.textInput.text.toString()
        lastInlineCursor = binding.textInput.selectionEnd.coerceAtLeast(0)
        suppressInlineEvents = false
    }

    /** Native hide/finalize must not turn into a user cancel callback. */
    fun finishFromGuest() {
        inlineFinished = true
        if (::binding.isInitialized) binding.textInput.removeCallbacks(sendInlineUpdate)
        session.close()
    }

    private fun showIme() {
        binding.textInput.requestFocus()
        binding.textInput.post {
            if (isAdded && binding.textInput.isEnabled) {
                (requireContext().getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
                    .showSoftInput(binding.textInput, InputMethodManager.SHOW_IMPLICIT)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        dialog?.window?.apply {
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        }
        dialog?.setCanceledOnTouchOutside(false)
        showIme()
    }

    fun prepareForRetry() {
        session.retry()
        if (!isAdded) return
        binding.textInput.isEnabled = true
        binding.okButton.isEnabled = config.isValid(binding.textInput.text.toString())
        binding.cancelButton.isEnabled = !config.isCancelButtonDisabled
        showIme()
    }

    override fun onCancel(dialog : DialogInterface) {
        cancelForShutdown()
        super.onCancel(dialog)
    }

    override fun onDestroy() {
        cancelForShutdown()
        super.onDestroy()
    }

    fun cancelForShutdown() {
        session.close()
        if (inlineToken != 0L && !inlineFinished) {
            inlineFinished = true
            if (::binding.isInitialized) binding.textInput.removeCallbacks(sendInlineUpdate)
            submitInlineInput(inlineToken, 6, "", 0, inlineRevision)
        }
    }

    fun waitForSubmitOrCancel() : SoftwareKeyboardResult = session.awaitResult()
}
