/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.setup

import android.os.Bundle
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.isVisible
import org.stratoemu.strato.R
import org.stratoemu.strato.databinding.SetupActivityBinding

class SetupActivity : AppCompatActivity() {
    private val model by viewModels<SetupViewModel>()
    private lateinit var binding: SetupActivityBinding
    private val keysPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> uri?.let(model::importKeys) }
    private val firmwarePicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> uri?.let(model::importFirmware) }
    private val folderPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri -> uri?.let(model::selectFolder) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = SetupActivityBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.keysButton.setOnClickListener { keysPicker.launch(arrayOf("*/*")) }
        binding.firmwareButton.setOnClickListener { firmwarePicker.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream")) }
        binding.gamesButton.setOnClickListener { folderPicker.launch(null) }
        binding.startButton.setOnClickListener {
            if (model.complete()) {
                setResult(RESULT_OK)
                finish()
            }
        }
        binding.backButton.setOnClickListener { finish() }
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() { if (model.busy.value != true) finish() }
        })
        model.busy.observe(this) { render() }
        model.revision.observe(this) { render() }
        model.message.observe(this) { binding.status.text = it }
    }

    private fun render() {
        val idle = model.busy.value != true
        binding.keysButton.isEnabled = idle
        binding.firmwareButton.isEnabled = idle && model.hasKeys
        binding.gamesButton.isEnabled = idle
        binding.startButton.isEnabled = idle && model.hasKeys && model.folder.isNotBlank()
        binding.backButton.isEnabled = idle
        binding.progress.isVisible = !idle
        binding.keysStatus.setText(if (model.hasKeys) R.string.setup_keys_ready else R.string.setup_keys_help)
        binding.firmwareStatus.text = if (model.firmwareVersion.isNotBlank()) getString(R.string.setup_firmware_installed, model.firmwareVersion) else getString(R.string.setup_firmware_help)
        binding.gamesStatus.setText(if (model.folder.isNotBlank()) R.string.setup_folder_ready else R.string.setup_games_help)
    }
}
