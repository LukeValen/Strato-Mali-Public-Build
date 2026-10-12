/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.app.PictureInPictureParams
import android.app.RemoteAction
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.content.res.AssetManager
import android.content.res.Configuration
import android.content.res.Resources
import android.graphics.PointF
import android.graphics.drawable.Icon
import android.hardware.display.DisplayManager
import android.net.DhcpInfo
import android.net.wifi.WifiManager
import android.os.*
import android.util.Log
import android.util.Rational
import android.util.TypedValue
import android.view.*
import android.widget.Toast
import androidx.annotation.Keep
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.getSystemService
import androidx.core.view.GravityCompat
import androidx.drawerlayout.widget.DrawerLayout
import androidx.core.view.isGone
import androidx.core.view.isInvisible
import androidx.core.view.updateMargins
import androidx.core.view.updatePadding
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import androidx.window.layout.WindowLayoutInfo
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.stratoemu.strato.applet.swkbd.SoftwareKeyboardConfig
import org.stratoemu.strato.applet.swkbd.SoftwareKeyboardDialog
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.databinding.EmuActivityBinding
import org.stratoemu.strato.emulation.PipelineLoadingFragment
import org.stratoemu.strato.input.*
import org.stratoemu.strato.loader.RomFile
import org.stratoemu.strato.loader.getRomFormat
import org.stratoemu.strato.settings.AppSettings
import org.stratoemu.strato.settings.EmulationSettings
import org.stratoemu.strato.settings.NativeSettings
import org.stratoemu.strato.utils.ByteBufferSerializable
import org.stratoemu.strato.utils.GpuDriverHelper
import org.stratoemu.strato.utils.serializable
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.FutureTask
import javax.inject.Inject
import kotlin.math.abs


private const val ActionPause = "${BuildConfig.APPLICATION_ID}.ACTION_EMULATOR_PAUSE"
private const val ActionMute = "${BuildConfig.APPLICATION_ID}.ACTION_EMULATOR_MUTE"

private val Number.toPx get() = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, this.toFloat(), Resources.getSystem().displayMetrics).toInt()

@AndroidEntryPoint
class EmulationActivity : AppCompatActivity(), SurfaceHolder.Callback, View.OnTouchListener, DisplayManager.DisplayListener {
    companion object {
        private val Tag = EmulationActivity::class.java.simpleName
        const val ReturnToMainTag = "returnToMain"

        /**
         * The Kotlin thread on which emulation code executes
         */
        private var emulationThread : Thread? = null
    }

    private val binding by lazy { EmuActivityBinding.inflate(layoutInflater) }

    /**
     * The [AppItem] of the app that is being emulated
     */
    lateinit var item : AppItem

    /**
     * The built-in [Vibrator] of the device
     */
    private lateinit var builtinVibrator : Vibrator

    /**
     * A map of [Vibrator]s that correspond to [InputManager.controllers]
     */
    private var vibrators = HashMap<Int, Vibrator>()

    /**
     * If the emulation thread should call [returnToMain] or not
     */
    @Volatile
    private var shouldFinish : Boolean = true

    /**
     * If the activity should return to [MainActivity] or just call [finishAffinity]
     */
    private var returnToMain : Boolean = false

    /**
     * The desired refresh rate to present at in Hz
     */
    private var desiredRefreshRate = 60f

    private var isEmulatorPaused = false
    private var menuOpen = false
    private var resumeAfterMenu = false
    private var audioMuted = false
    private var keyboardDialog : SoftwareKeyboardDialog? = null
    private var exitConfirmation : androidx.appcompat.app.AlertDialog? = null

    private lateinit var pictureInPictureParamsBuilder : PictureInPictureParams.Builder

    @Inject
    lateinit var appSettings : AppSettings

    private lateinit var emulationSettings : EmulationSettings

    @Inject
    lateinit var inputManager : InputManager

    lateinit var inputHandler : InputHandler

    private var gameSurface : Surface? = null

    /**
     * This is the entry point into the emulation code for libskyline
     *
     * @param romUri The URI of the ROM as a string, used to print out in the logs
     * @param romType The type of the ROM as an enum value
     * @param romFd The file descriptor of the ROM object
     * @param nativeSettings The settings to be used by libskyline
     * @param publicAppFilesPath The full path to the public app files directory
     * @param privateAppFilesPath The full path to the private app files directory
     * @param nativeLibraryPath The full path to the app native library directory
     * @param assetManager The asset manager used for accessing app assets
     */
    private external fun executeApplication(romUri : String, romType : Int, romFd : Int, nativeSettings : NativeSettings, publicAppFilesPath : String, privateAppFilesPath : String, nativeLibraryPath : String, assetManager : AssetManager)

    /**
     * @param join If the function should only return after all the threads join or immediately
     * @return If it successfully caused [emulationThread] to gracefully stop or do so asynchronously when not joined
     */
    private external fun stopEmulation(join : Boolean) : Boolean

    /**
     * This sets the surface object in libskyline to the provided value, emulation is halted if set to null
     *
     * @param surface The value to set surface to
     * @return If the value was successfully set
     */
    private external fun setSurface(surface : Surface?) : Boolean

    /**
     * @param play If the audio should be playing or be stopped till it is resumed by calling this again
     */
    private external fun changeAudioStatus(play : Boolean)

    var fps : Int = 0
    var averageFrametime : Float = 0.0f
    var averageFrametimeDeviation : Float = 0.0f
    var ramUsage : Long = 0

    /**
     * Writes the current performance statistics into [fps], [averageFrametime] and [averageFrametimeDeviation] fields
     */
    private external fun updatePerformanceStatistics()

    /**
     * @see [InputHandler.initializeControllers]
     */
    @Suppress("unused")
    private fun initializeControllers() {
        inputHandler.initializeControllers()
        inputHandler.initialiseMotionSensors(this)
    }

    /**
     * Forces a 60Hz refresh rate for the primary display when [enable] is true, otherwise selects the highest available refresh rate
     */
    private fun force60HzRefreshRate(enable : Boolean) {
        // Hack for MIUI devices since they don't support the standard Android APIs
        try {
            val setFpsIntent = Intent("com.miui.powerkeeper.SET_ACTIVITY_FPS")
            setFpsIntent.putExtra("package_name", "skyline.emu")
            setFpsIntent.putExtra("isEnter", enable)
            sendBroadcast(setFpsIntent)
        } catch (_ : Exception) {
        }

        @Suppress("DEPRECATION") val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display!! else windowManager.defaultDisplay
        if (enable)
            display?.supportedModes?.minByOrNull { abs(it.refreshRate - 60f) }?.let { window.attributes.preferredDisplayModeId = it.modeId }
        else
            display?.supportedModes?.maxByOrNull { it.refreshRate }?.let { window.attributes.preferredDisplayModeId = it.modeId }
    }

    /**
     * Return from emulation to either [MainActivity] or the activity on the back stack
     */
    @SuppressWarnings("WeakerAccess")
    fun returnFromEmulation() {
        if (shouldFinish) {
            runOnUiThread {
                if (shouldFinish) {
                    shouldFinish = false
                    if (returnToMain)
                        startActivity(Intent(applicationContext, MainActivity::class.java).setFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
                    Process.killProcess(Process.myPid())
                }
            }
        }
    }

    /**
     * @note Any caller has to handle the application potentially being restarted with the supplied intent
     */
    private fun executeApplication(intent : Intent) {
        if (emulationThread?.isAlive == true) {
            shouldFinish = false
            if (stopEmulation(false))
                emulationThread!!.join(250)

            if (emulationThread!!.isAlive) {
                finishAffinity()
                startActivity(intent)
                Runtime.getRuntime().exit(0)
            }
        }

        shouldFinish = true
        returnToMain = intent.getBooleanExtra(ReturnToMainTag, false)

        val rom = item.uri
        val romType = item.format.ordinal

        @SuppressLint("Recycle")
        val romFd = contentResolver.openFileDescriptor(rom, "r")!!

        GpuDriverHelper.ensureFileRedirectDir(this)
        emulationThread = Thread {
            executeApplication(rom.toString(), romType, romFd.detachFd(), NativeSettings(this, emulationSettings), applicationContext.getPublicFilesDir().canonicalPath + "/", applicationContext.filesDir.canonicalPath + "/", applicationInfo.nativeLibraryDir + "/", assets)
            returnFromEmulation()
        }

        emulationThread!!.start()
    }

    /**
     * Populates the [item] member with data from the intent
     */
    private fun populateAppItem() {
        val intentItem = intent.serializable(AppItemTag) as AppItem?
        if (intentItem != null) {
            item = intentItem
            return
        }

        // The intent did not contain an app item, fall back to the data URI
        val uri = intent.data!!
        val romFormat = getRomFormat(uri, contentResolver)
        val romFile = RomFile(this, romFormat, uri, EmulationSettings.global.systemLanguage)

        item = AppItem(romFile.takeIf { it.valid }!!.appEntry)
    }

    @SuppressLint("SetTextI18n", "ClickableViewAccessibility")
    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        populateAppItem()
        emulationSettings = EmulationSettings.forEmulation(item.titleId ?: item.key())

        requestedOrientation = emulationSettings.orientation
        window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        inputHandler = InputHandler(inputManager, emulationSettings)
        setContentView(binding.root)
        audioMuted = emulationSettings.isAudioOutputDisabled
        setupEmulationMenu()
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (keyboardDialog != null) return
                if (menuOpen) closeEmulationMenu() else openEmulationMenu()
            }
        })

        builtinVibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val vibratorManager = getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager
            vibratorManager.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android might not allow child views to overlap the system bars
            // Override this behavior and force content to extend into the cutout area
            window.setDecorFitsSystemWindows(false)

            window.insetsController?.let {
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                it.hide(WindowInsets.Type.systemBars())
            }
        }

        if (emulationSettings.respectDisplayCutout) {
            binding.perfStats.setOnApplyWindowInsetsListener(insetsOrMarginHandler)
            binding.onScreenControllerToggle.setOnApplyWindowInsetsListener(insetsOrMarginHandler)
        }

        pictureInPictureParamsBuilder = getPictureInPictureBuilder()
        setPictureInPictureParams(pictureInPictureParamsBuilder.build())

        binding.gameView.holder.addCallback(this)

        binding.gameView.setAspectRatio(
            when (emulationSettings.aspectRatio) {
                0 -> Rational(16, 9)
                1 -> Rational(21, 9)
                else -> null
            }
        )

        lifecycleScope.launch(Dispatchers.Main) {
            lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                WindowInfoTracker.getOrCreate(this@EmulationActivity)
                    .windowLayoutInfo(this@EmulationActivity)
                    .collect { updateCurrentLayout(it) }
            }
        }

        binding.perfStats.isGone = !emulationSettings.perfStats
        run {
            if (emulationSettings.disableFrameThrottling)
                binding.perfStats.setTextColor(getColor(R.color.colorPerfStatsSecondary))

            binding.perfStats.apply {
                postDelayed(object : Runnable {
                    override fun run() {
                        updatePerformanceStatistics()
                        // We read the `VmRSS` value from the kernel
                        ramUsage = File("/proc/self/statm").readLines()[0].split(' ')[1].toLong() * 4096 / 1000000
                        text = "$fps FPS • $ramUsage MB"
                        postDelayed(this, 250)
                    }
                }, 250)
            }
        }

        force60HzRefreshRate(!emulationSettings.maxRefreshRate)
        getSystemService<DisplayManager>()?.registerDisplayListener(this, null)

        if (!emulationSettings.isGlobal && emulationSettings.useCustomSettings)
            Toast.makeText(this, getString(R.string.per_game_settings_active_message), Toast.LENGTH_SHORT).show()

        binding.gameView.setOnTouchListener(this)

        // Hide on screen controls when first controller is not set
        binding.onScreenControllerView.apply {
            vibrator = builtinVibrator
            controllerType = inputHandler.getFirstControllerType()
            isGone = controllerType == ControllerType.None || !appSettings.onScreenControl
            setOnButtonStateChangedListener(::onButtonStateChanged)
            setOnStickStateChangedListener(::onStickStateChanged)
            hapticFeedback = appSettings.onScreenControl && appSettings.onScreenControlFeedback
            recenterSticks = appSettings.onScreenControlRecenterSticks
            stickRegions = appSettings.onScreenControlUseStickRegions
        }

        binding.onScreenControllerToggle.apply {
            isGone = binding.onScreenControllerView.isGone
            setOnClickListener { binding.onScreenControllerView.isInvisible = !binding.onScreenControllerView.isInvisible }
        }

        binding.onScreenControllerView.isInvisible = isControllerConnected()

        binding.onScreenPauseToggle.apply {
            isGone = !emulationSettings.showPauseButton
            setOnClickListener { openEmulationMenu() }
            contentDescription = getString(R.string.emulation_menu)
        }

        executeApplication(intent!!)
    }

    private fun setupEmulationMenu() {
        // Only Back/the menu button opens the drawer: edge swipes belong to the game.
        binding.emulationDrawer.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED)
        binding.emulationDrawer.addDrawerListener(object : DrawerLayout.SimpleDrawerListener() {
            override fun onDrawerClosed(drawerView : View) = finishClosingMenu()

            override fun onDrawerStateChanged(newState : Int) {
                // Back can interrupt opening before DrawerLayout marks it fully open.
                if (newState == DrawerLayout.STATE_IDLE && !binding.emulationDrawer.isDrawerVisible(GravityCompat.START))
                    finishClosingMenu()
            }
        })
        binding.emulationMenu.setNavigationItemSelectedListener { selected ->
            when (selected.itemId) {
                R.id.menu_resume -> closeEmulationMenu()
                R.id.menu_controls -> {
                    selected.isChecked = !selected.isChecked
                    binding.onScreenControllerView.isInvisible = !selected.isChecked
                }
                R.id.menu_fps -> {
                    selected.isChecked = !selected.isChecked
                    binding.perfStats.isGone = !selected.isChecked
                }
                R.id.menu_mute -> {
                    audioMuted = !audioMuted
                    selected.isChecked = audioMuted
                }
                R.id.menu_exit -> confirmExit()
            }
            // Checked state is managed above, including turning an item off.
            false
        }
    }

    private fun finishClosingMenu() {
        if (!menuOpen) return
        menuOpen = false
        if (resumeAfterMenu && lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) resumeEmulator()
        resumeAfterMenu = false
        pictureInPictureParamsBuilder = getPictureInPictureBuilder()
        binding.gameView.requestFocus()
    }

    private fun releaseGameInput() {
        InputHandler.setTouchState(intArrayOf())
        inputManager.controllers.keys.forEach { index ->
            InputHandler.setButtonState(index, ButtonId.All.value, false)
            AxisId.values().forEach { InputHandler.setAxisValue(index, it.ordinal, 0) }
        }
    }

    private fun openEmulationMenu() {
        if (menuOpen || keyboardDialog != null || isFinishing) return
        resumeAfterMenu = !isEmulatorPaused
        menuOpen = true
        releaseGameInput()
        pauseEmulator()
        binding.emulationMenu.menu.apply {
            findItem(R.id.menu_controls).apply {
                isChecked = binding.onScreenControllerView.visibility == View.VISIBLE
                isEnabled = !binding.onScreenControllerView.isGone
            }
            findItem(R.id.menu_fps).isChecked = !binding.perfStats.isGone
            findItem(R.id.menu_mute).isChecked = audioMuted
        }
        pictureInPictureParamsBuilder = getPictureInPictureBuilder()
        binding.emulationDrawer.openDrawer(GravityCompat.START)
    }

    private fun closeEmulationMenu() {
        if (menuOpen) binding.emulationDrawer.closeDrawer(GravityCompat.START)
    }

    private fun confirmExit() {
        if (exitConfirmation?.isShowing == true) return
        exitConfirmation = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.emulation_exit)
            .setMessage(R.string.emulation_exit_message)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.emulation_exit) { _, _ -> returnFromEmulation() }
            .create()
        exitConfirmation?.show()
    }

    @SuppressWarnings("WeakerAccess")
    fun pauseEmulator() {
        if (isEmulatorPaused) return
        setSurface(null)
        changeAudioStatus(false)
        isEmulatorPaused = true
    }

    @SuppressWarnings("WeakerAccess")
    fun resumeEmulator() {
        gameSurface?.let { setSurface(it) }
        changeAudioStatus(!audioMuted)
        isEmulatorPaused = false
    }

    override fun onPause() {
        super.onPause()

        if (emulationSettings.forceMaxGpuClocks)
            GpuDriverHelper.forceMaxGpuClocks(false)

        pauseEmulator()
    }

    override fun onResume() {
        super.onResume()

        if (!menuOpen) resumeEmulator()

        GpuDriverHelper.forceMaxGpuClocks(emulationSettings.forceMaxGpuClocks)

        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.R) {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN)
        }
    }

    private fun getPictureInPictureBuilder() : PictureInPictureParams.Builder {
        val pictureInPictureParamsBuilder = PictureInPictureParams.Builder()

        val pictureInPictureActions : MutableList<RemoteAction> = mutableListOf()
        val pendingFlags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE

        val pauseIcon = Icon.createWithResource(this, R.drawable.ic_pause)
        val pausePendingIntent = PendingIntent.getBroadcast(this, R.drawable.ic_pause, Intent(ActionPause), pendingFlags)
        val pauseRemoteAction = RemoteAction(pauseIcon, getString(R.string.pause), getString(R.string.pause), pausePendingIntent)
        pictureInPictureActions.add(pauseRemoteAction)

        val muteIcon = Icon.createWithResource(this, R.drawable.ic_volume_mute)
        val mutePendingIntent = PendingIntent.getBroadcast(this, R.drawable.ic_volume_mute, Intent(ActionMute), pendingFlags)
        val muteRemoteAction = RemoteAction(muteIcon, getString(R.string.mute), getString(R.string.mute), mutePendingIntent)
        pictureInPictureActions.add(muteRemoteAction)

        pictureInPictureParamsBuilder.setActions(pictureInPictureActions)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            pictureInPictureParamsBuilder.setAutoEnterEnabled(!menuOpen && keyboardDialog == null)

        setPictureInPictureParams(pictureInPictureParamsBuilder.build())

        return pictureInPictureParamsBuilder
    }

    private var pictureInPictureReceiver = object : BroadcastReceiver() {
        override fun onReceive(context : Context?, intent : Intent) {
            if (intent.action == ActionPause)
                pauseEmulator()
            else if (intent.action == ActionMute)
                changeAudioStatus(false)
        }
    }

    override fun onPictureInPictureModeChanged(isInPictureInPictureMode : Boolean, newConfig : Configuration) {
        super.onPictureInPictureModeChanged(isInPictureInPictureMode, newConfig)
        if (isInPictureInPictureMode) {

            IntentFilter().apply {
                addAction(ActionPause)
                addAction(ActionMute)
            }.also {
                registerReceiver(pictureInPictureReceiver, it)
            }

            binding.onScreenControllerView.isGone = true
            binding.onScreenControllerToggle.isGone = true
            binding.onScreenPauseToggle.isGone = true
        } else {
            try {
                unregisterReceiver(pictureInPictureReceiver)
            } catch (ignored : Exception) {
            }

            if (!menuOpen) resumeEmulator()

            binding.onScreenControllerView.apply {
                controllerType = inputHandler.getFirstControllerType()
                isGone = controllerType == ControllerType.None || !appSettings.onScreenControl
            }
            binding.onScreenControllerToggle.apply {
                isGone = binding.onScreenControllerView.isGone
            }
            binding.onScreenPauseToggle.apply {
                isGone = !emulationSettings.showPauseButton
            }
        }
    }

    /**
     * Updating the layout depending on type and state of device
     */
    private fun updateCurrentLayout(newLayoutInfo : WindowLayoutInfo) {
        if (!emulationSettings.enableFoldableLayout) return
        val isFolding = (newLayoutInfo.displayFeatures.find { it is FoldingFeature } as? FoldingFeature)?.let {
            if (it.isSeparating) {
                requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                if (it.orientation == FoldingFeature.Orientation.HORIZONTAL) {
                    binding.gameViewContainer.layoutParams.height = it.bounds.top
                    binding.overlayViewContainer.layoutParams.height = it.bounds.bottom - 48.toPx
                    binding.overlayViewContainer.updatePadding(0, 0, 0, 24.toPx)
                }
            }
            it.isSeparating
        } ?: false
        if (!isFolding) {
            binding.gameViewContainer.layoutParams.height = ViewGroup.LayoutParams.MATCH_PARENT
            binding.overlayViewContainer.updatePadding(0, 0, 0, 0)
            binding.overlayViewContainer.layoutParams.height = ViewGroup.LayoutParams.MATCH_PARENT
            requestedOrientation = emulationSettings.orientation
        }
        binding.gameViewContainer.requestLayout()
        binding.overlayViewContainer.requestLayout()
    }

    /**
     * Stop the currently executing ROM and replace it with the one specified in the new intent
     */
    override fun onNewIntent(intent : Intent?) {
        super.onNewIntent(intent!!)
        if (getIntent().data != intent.data) {
            setIntent(intent)
            executeApplication(intent)
        }
    }

    override fun onUserLeaveHint() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S && !isInPictureInPictureMode && !menuOpen && keyboardDialog == null)
            enterPictureInPictureMode(pictureInPictureParamsBuilder.build())
    }

    override fun onDestroy() {
        keyboardDialog?.cancelForShutdown()
        keyboardDialog = null
        exitConfirmation?.dismiss()
        super.onDestroy()
        shouldFinish = false

        // Stop forcing 60Hz on exit to allow the skyline UI to run at high refresh rates
        getSystemService<DisplayManager>()?.unregisterDisplayListener(this)
        force60HzRefreshRate(false)
        if (emulationSettings.forceMaxGpuClocks)
            GpuDriverHelper.forceMaxGpuClocks(false)

        stopEmulation(false)
        vibrators.forEach { (_, vibrator) -> vibrator.cancel() }
        vibrators.clear()
    }

    private lateinit var pipelineLoadingFragment : PipelineLoadingFragment

    /**
     * Called by native code to show the pipeline loading progress screen
     */
    @Suppress("unused")
    fun showPipelineLoadingScreen(totalPipelineCount : Int) {
        pipelineLoadingFragment = PipelineLoadingFragment.newInstance(item, totalPipelineCount)

        supportFragmentManager
            .beginTransaction()
            .setCustomAnimations(android.R.anim.fade_in, android.R.anim.fade_out)
            .replace(R.id.emulation_fragment, pipelineLoadingFragment)
            .commit()
    }

    /**
     * Called by native code to update the pipeline loading progress
     */
    @Suppress("unused")
    fun updatePipelineLoadingProgress(progress : Int) {
        runOnUiThread {
            pipelineLoadingFragment.updateProgress(progress)
        }
    }

    /**
     * Called by native code to hide the pipeline loading progress screen
     */
    @Suppress("unused")
    fun hidePipelineLoadingScreen() {
        supportFragmentManager
            .beginTransaction()
            .setCustomAnimations(android.R.anim.fade_in, android.R.anim.fade_out)
            .remove(pipelineLoadingFragment)
            .commit()
    }

    override fun surfaceCreated(holder : SurfaceHolder) {
        Log.d(Tag, "surfaceCreated Holder: $holder")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
        // Note: We need FRAME_RATE_COMPATIBILITY_FIXED_SOURCE as there will be a degradation of user experience with FRAME_RATE_COMPATIBILITY_DEFAULT due to game speed alterations when the frame rate doesn't match the display refresh rate
            holder.surface.setFrameRate(desiredRefreshRate, if (emulationSettings.maxRefreshRate) Surface.FRAME_RATE_COMPATIBILITY_DEFAULT else Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)

        while (emulationThread!!.isAlive)
            if (setSurface(holder.surface)) {
                gameSurface = holder.surface
                if (menuOpen) setSurface(null)
                return
            }
    }

    /**
     * This is purely used for debugging surface changes
     */
    override fun surfaceChanged(holder : SurfaceHolder, format : Int, width : Int, height : Int) {
        Log.d(Tag, "surfaceChanged Holder: $holder, Format: $format, Width: $width, Height: $height")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            holder.surface.setFrameRate(desiredRefreshRate, if (emulationSettings.maxRefreshRate) Surface.FRAME_RATE_COMPATIBILITY_DEFAULT else Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
    }

    override fun surfaceDestroyed(holder : SurfaceHolder) {
        Log.d(Tag, "surfaceDestroyed Holder: $holder")
        while (emulationThread!!.isAlive)
            if (setSurface(null)) {
                gameSurface = null
                return
            }
    }

    private fun isControllerConnected() : Boolean {
        val deviceIds = InputDevice.getDeviceIds()

        deviceIds.forEach { deviceId ->
            InputDevice.getDevice(deviceId)?.apply {
                // Verify that the device has gamepad buttons, control sticks, or both.
                if (sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) {
                    // This device is a game controller.
                    return true
                }
            }
        }
        return false
    }

    override fun dispatchKeyEvent(event : KeyEvent) : Boolean {
        if (keyboardDialog != null || menuOpen || event.keyCode == KeyEvent.KEYCODE_BACK)
            return super.dispatchKeyEvent(event)
        val mapped = event.device?.let { inputManager.eventMap[KeyHostEvent(it.descriptor, event.keyCode)] }
        if (mapped is ButtonGuestEvent && mapped.button == ButtonId.Menu) {
            if (event.action == KeyEvent.ACTION_UP) openEmulationMenu()
            return true
        }
        return if (inputHandler.handleKeyEvent(event)) true else super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event : MotionEvent) : Boolean {
        if (menuOpen || keyboardDialog != null) return super.dispatchGenericMotionEvent(event)
        return if (inputHandler.handleMotionEvent(event)) true else super.dispatchGenericMotionEvent(event)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouch(view : View, event : MotionEvent) : Boolean {
        if (menuOpen || keyboardDialog != null) return true
        return inputHandler.handleTouchEvent(view, event)
    }

    private fun onButtonStateChanged(buttonId : ButtonId, state : ButtonState) {
        if (buttonId == ButtonId.Menu) {
            if (state == ButtonState.Released) openEmulationMenu()
        } else if (!menuOpen && keyboardDialog == null) {
            InputHandler.setButtonState(0, buttonId.value, state.state)
        }
    }

    private fun onStickStateChanged(stickId : StickId, position : PointF) {
        if (menuOpen || keyboardDialog != null) return
        InputHandler.setAxisValue(0, stickId.xAxis.ordinal, (position.x * Short.MAX_VALUE).toInt())
        InputHandler.setAxisValue(0, stickId.yAxis.ordinal, (-position.y * Short.MAX_VALUE).toInt()) // Y is inverted, since drawing starts from top left
    }

    @SuppressLint("WrongConstant")
    @Suppress("unused")
    fun vibrateDevice(index : Int, timing : LongArray, amplitude : IntArray) {
        val vibrator = if (vibrators[index] != null) {
            vibrators[index]
        } else {
            inputManager.controllers[index]!!.rumbleDeviceDescriptor?.let {
                if (it == Controller.BuiltinRumbleDeviceDescriptor) {
                    vibrators[index] = builtinVibrator
                    builtinVibrator
                } else {
                    for (id in InputDevice.getDeviceIds()) {
                        val device = InputDevice.getDevice(id)
                        if (device?.descriptor == inputManager.controllers[index]!!.rumbleDeviceDescriptor) {
                            val vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                                device?.vibratorManager!!.defaultVibrator
                            } else {
                                @Suppress("DEPRECATION")
                                device?.vibrator!!
                            }
                            vibrators[index] = vibrator
                            return@let vibrator
                        }
                    }
                    return@let null
                }
            }
        }

        vibrator?.let {
            val effect = VibrationEffect.createWaveform(timing, amplitude, 0)
            it.vibrate(effect)
        }
    }

    @Suppress("unused")
    fun clearVibrationDevice(index : Int) {
        vibrators[index]?.cancel()
    }

    @Suppress("unused")
    fun showKeyboard(buffer : ByteBuffer, initialText : String) : SoftwareKeyboardDialog? {
        buffer.order(ByteOrder.LITTLE_ENDIAN)
        val config = ByteBufferSerializable.createFromByteBuffer(SoftwareKeyboardConfig::class, buffer) as SoftwareKeyboardConfig

        val dialog = SoftwareKeyboardDialog.newInstance(config, initialText)
        presentKeyboard(dialog)
        return dialog
    }

    @Keep
    fun showInlineKeyboard(buffer : ByteBuffer, initialText : String, token : Long, cursor : Int, revision : Long) : SoftwareKeyboardDialog {
        buffer.order(ByteOrder.LITTLE_ENDIAN)
        val config = ByteBufferSerializable.createFromByteBuffer(SoftwareKeyboardConfig::class, buffer) as SoftwareKeyboardConfig
        val dialog = SoftwareKeyboardDialog.newInlineInstance(config, initialText, token, cursor, revision)
        presentKeyboard(dialog)
        return dialog
    }

    @Keep
    fun updateInlineKeyboard(dialog : SoftwareKeyboardDialog, text : String, cursor : Int, revision : Long) {
        runOnUiThread {
            if (keyboardDialog === dialog) dialog.updateFromGuest(text, cursor, revision)
        }
    }

    private fun presentKeyboard(dialog : SoftwareKeyboardDialog) {
        runOnUiThread {
            if (isFinishing || isDestroyed) {
                dialog.cancelForShutdown()
                return@runOnUiThread
            }
            closeEmulationMenu()
            releaseGameInput()
            keyboardDialog = dialog
            pictureInPictureParamsBuilder = getPictureInPictureBuilder()
            if (!supportFragmentManager.isStateSaved && lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
                dialog.showNow(supportFragmentManager, "software_keyboard")
            // Otherwise onPostResume presents it when the activity can accept a dialog.
        }
    }

    override fun onPostResume() {
        super.onPostResume()
        keyboardDialog?.let {
            if (!it.isAdded && !supportFragmentManager.isStateSaved)
                it.showNow(supportFragmentManager, "software_keyboard")
        }
    }

    @Suppress("unused")
    fun waitForSubmitOrCancel(dialog : SoftwareKeyboardDialog) : Array<Any?> {
        return dialog.waitForSubmitOrCancel().let { arrayOf(if (it.cancelled) 1 else 0, it.text) }
    }

    @Suppress("unused")
    fun getDhcpInfo() : DhcpInfo {
        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        return wifiManager.dhcpInfo
    }

    @Suppress("unused")
    fun closeKeyboard(dialog : SoftwareKeyboardDialog) {
        runOnUiThread {
            if (keyboardDialog === dialog) keyboardDialog = null
            dialog.finishFromGuest()
            if (dialog.isAdded) dialog.dismissAllowingStateLoss() else dialog.cancelForShutdown()
            if (!isDestroyed) pictureInPictureParamsBuilder = getPictureInPictureBuilder()
        }
    }

    @Suppress("unused")
    fun showValidationResult(dialog : SoftwareKeyboardDialog, validationResult : Int, message : String) : Int {
        val confirm = validationResult == SoftwareKeyboardDialog.validationConfirm
        var accepted = false
        val validatorResult = FutureTask { return@FutureTask accepted }
        runOnUiThread {
            if (isFinishing || isDestroyed || !dialog.isAdded) {
                dialog.cancelForShutdown()
                validatorResult.run()
                return@runOnUiThread
            }
            val builder = MaterialAlertDialogBuilder(dialog.requireContext())
            builder.setMessage(message)
            builder.setPositiveButton(if (confirm) getString(android.R.string.ok) else getString(android.R.string.cancel)) { _, _ -> accepted = confirm }
            if (confirm)
                builder.setNegativeButton(getString(android.R.string.cancel)) { _, _ -> }
            builder.setOnDismissListener {
                if (!accepted) dialog.prepareForRetry()
                validatorResult.run()
            }
            builder.show()
        }
        return if (validatorResult.get()) 0 else 1
    }

    /**
     * @return A version code in Vulkan's format with 14-bit patch + 10-bit major and minor components
     */
    @ExperimentalUnsignedTypes
    @Suppress("unused")
    fun getVersionCode() : Int {
        val (major, minor, patch) = try {
            BuildConfig.VERSION_NAME.split('-')[0].split('.').map { it.toUInt() }
        } catch (_ : Exception) {
            listOf(0u, 0u, 0u)
        }
        return ((major shl 22) or (minor shl 12) or (patch)).toInt()
    }

    @Suppress("unused")
    fun reportCrash() {
        if (BuildConfig.BUILD_TYPE != "release")
            return
        runOnUiThread {
            MaterialAlertDialogBuilder(this)
                .setTitle(getString(R.string.game_crash))
                .setMessage(getString(R.string.game_crash_message))
                .setPositiveButton(getString(android.R.string.ok)) { _, _ ->
                    shouldFinish = true
                    returnFromEmulation()
                }.show()
        }
    }

    private val insetsOrMarginHandler = View.OnApplyWindowInsetsListener { view, insets ->
        insets.displayCutout?.let {
            val defaultHorizontalMargin = view.resources.getDimensionPixelSize(R.dimen.onScreenItemHorizontalMargin)
            val left = if (it.safeInsetLeft == 0) defaultHorizontalMargin else it.safeInsetLeft
            val right = if (it.safeInsetRight == 0) defaultHorizontalMargin else it.safeInsetRight

            val params = view.layoutParams as ViewGroup.MarginLayoutParams
            params.updateMargins(left = left, right = right)
            view.layoutParams = params
        }
        insets
    }

    override fun onDisplayChanged(displayId : Int) {
        @Suppress("DEPRECATION")
        val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display!! else windowManager.defaultDisplay
        if (display.displayId == displayId)
            force60HzRefreshRate(!emulationSettings.maxRefreshRate)
    }

    override fun onDisplayAdded(displayId : Int) {}

    override fun onDisplayRemoved(displayId : Int) {}
}
