package dev.omalauncher

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.SurfaceHolder
import android.view.SurfaceView
import dev.omalauncher.wayland.AXIS_SOURCE_WHEEL
import dev.omalauncher.wayland.BTN_LEFT
import dev.omalauncher.wayland.BTN_RIGHT
import dev.omalauncher.wayland.BTN_MIDDLE
import kotlin.math.roundToInt
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imeAnimationTarget
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import dev.omalauncher.ui.ExtraKeys
import dev.omalauncher.ui.ImagePicker
import dev.omalauncher.ui.KeyboardSink
import dev.omalauncher.ui.MicPermission
import dev.omalauncher.ui.Mono
import dev.omalauncher.ui.Palette
import dev.omalauncher.ui.linuxWindowInput
import dev.omalauncher.wayland.WindowRenderer
import dev.omalauncher.wayland.XdgToplevel
import kotlinx.coroutines.flow.first

/**
 * Shows a single Linux window as its own Android task, so it gets its own
 * Recents card and behaves like a separate app.
 */
class LinuxWindowActivity : InstantActivity() {
    private var window: XdgToplevel? = null
    private val imagePicker = ImagePicker(this) { runtime }
    private val mic = MicPermission(this)
    private var surfaceView: WindowSurfaceView? = null
    private val isPointerCaptured = mutableStateOf(false)

    fun togglePointerCapture() {
        val sv = surfaceView ?: return
        if (sv.hasPointerCapture()) {
            sv.releasePointerCapture()
        } else {
            sv.requestPointerCapture()
        }
    }

    fun showDeXTaskbar() {
        val sv = surfaceView
        if (sv?.hasPointerCapture() == true) {
            sv.releasePointerCapture()
        }
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().insetsController?.show(
                android.view.WindowInsets.Type.navigationBars() or android.view.WindowInsets.Type.statusBars()
            )
        } else {
            @Suppress("DEPRECATION")
            getWindow().decorView.systemUiVisibility = android.view.View.SYSTEM_UI_FLAG_VISIBLE
        }
    }

    override fun onPointerCaptureChanged(hasCapture: Boolean) {
        super.onPointerCaptureChanged(hasCapture)
        isPointerCaptured.value = hasCapture
        surfaceView?.onCaptureChanged(hasCapture)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) {
            surfaceView?.releasePointerCapture()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val window = runtime.wm.windowByHandle(intent.data?.lastPathSegment)
        if (window == null) {
            // The window is gone, e.g. the process restarted since Recents was built.
            finishAndRemoveTask()
            return
        }
        this.window = window
        getWindow().decorView.setOnHoverListener { _, event ->
            if (event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
                surfaceView?.handleHover(event) ?: false
            } else {
                false
            }
        }
        setInstantContent {
            AppWindow(
                runtime = runtime,
                window = window,
                isPointerCaptured = isPointerCaptured.value,
                onTogglePointerCapture = { togglePointerCapture() },
                onShowDeXTaskbar = { showDeXTaskbar() },
            ) { sv ->
                surfaceView = sv
                sv.onRequestToggleCapture = { togglePointerCapture() }
                sv.onRequestShowDeXTaskbar = { showDeXTaskbar() }
            }
            BackHandler { backInstantly() }
            LaunchedEffect(window) {
                snapshotFlow { window in runtime.wm.appWindows }.first { !it }
                closeInstantly()
            }
            // Terminals retitle constantly; only a new label changes the Recents card.
            val label = runtime.displayName(window.appId, window.title)
            val appIcon = runtime.iconNamed(runtime.agentOf(window.appId, label)?.name ?: label)
            LaunchedEffect(label, appIcon, Palette.accent) { describeTask(label, appIcon?.asAndroidBitmap()) }
        }
    }

    override fun onStart() {
        super.onStart()
        runtime.screenStarted()
    }

    override fun onResume() {
        super.onResume()
        window?.let(runtime.wm::focus)
        runtime.keyboard.imagePicker = imagePicker
        runtime.dictation.askMic = mic::ask
    }

    override fun onStop() {
        super.onStop()
        runtime.screenStopped()
    }

    /** Swiping the card away in Recents closes the Linux window, like closing an app. */
    override fun onDestroy() {
        super.onDestroy()
        val w = window ?: return
        if (isFinishing && w in runtime.wm.appWindows) runtime.wm.close(w)
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        surfaceView?.post { surfaceView?.layoutWindow() }
    }

    // Lint flags super.dispatchKeyEvent: ComponentActivity marks its override @RestrictTo, but Activity's is public API.
    @SuppressLint("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Super+Esc leaves the window as Back does, where Esc itself is the window's: it keeps running in Recents.
        if (event.keyCode == KeyEvent.KEYCODE_ESCAPE && event.isMetaPressed && event.action == KeyEvent.ACTION_DOWN) {
            if (event.repeatCount == 0) backInstantly()
            return true
        }
        if (event.action == KeyEvent.ACTION_DOWN && event.isCtrlPressed && event.isAltPressed &&
            (event.keyCode == KeyEvent.KEYCODE_M || event.keyCode == KeyEvent.KEYCODE_P)
        ) {
            togglePointerCapture()
            return true
        }
        if (runtime.keyboard.handle(event)) return true
        return super.dispatchKeyEvent(event)
    }

    /** Keys held as the window goes, like Super, would stay down for it: Android sends their lift elsewhere. */
    override fun onPause() {
        super.onPause()
        runtime.keyboard.releaseHeld()
        if (runtime.keyboard.imagePicker === imagePicker) runtime.keyboard.imagePicker = null
        runtime.dictation.askMic = null
        surfaceView?.releasePointerCapture()
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
            val sv = surfaceView
            if (sv != null && !sv.hasPointerCapture() && sv.handleMouseTouch(event)) {
                return true
            }
        } else if (event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)) {
            if (surfaceView?.hasPointerCapture() == true) {
                surfaceView?.releasePointerCapture()
            }
        }
        return super.dispatchTouchEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            val sv = surfaceView
            if (sv != null) {
                if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
                    if (sv.handleScroll(event)) return true
                } else if (event.actionMasked == MotionEvent.ACTION_HOVER_MOVE ||
                    event.actionMasked == MotionEvent.ACTION_HOVER_ENTER ||
                    event.actionMasked == MotionEvent.ACTION_HOVER_EXIT) {
                    if (sv.handleHover(event)) return true
                }
            }
        }
        return super.dispatchGenericMotionEvent(event)
    }
}

/**
 * Recents shows the manifest label ("Linux", or an alias's), but it does use the
 * task icon: the app's drawer icon, else its initials in the theme colors.
 */
internal fun taskIcon(
    name: String,
    background: Int,
    accent: Int,
): Bitmap {
    val size = 192
    val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bmp)
    canvas.drawColor(background)
    val words = name.split(Regex("[^A-Za-z0-9]+")).filter { it.isNotEmpty() }
    val initials =
        when {
            words.size >= 2 -> "${words[0][0]}${words[1][0]}".uppercase()
            words.isNotEmpty() -> words[0].take(2).replaceFirstChar(Char::uppercase)
            else -> "◆"
        }
    val paint =
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = accent
            textSize = size * 0.42f
            typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
            textAlign = Paint.Align.CENTER
        }
    val y = size / 2f - (paint.descent() + paint.ascent()) / 2
    canvas.drawText(initials, size / 2f, y, paint)
    return bmp
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AppWindow(
    runtime: Runtime,
    window: XdgToplevel,
    isPointerCaptured: Boolean,
    onTogglePointerCapture: () -> Unit,
    onShowDeXTaskbar: () -> Unit,
    onSurfaceViewCreated: (WindowSurfaceView) -> Unit = {},
) {
    var sink by remember { mutableStateOf<KeyboardSink?>(null) }
    var surfaceView by remember { mutableStateOf<WindowSurfaceView?>(null) }
    // Shows wherever the window has yet to draw, as when it grows; the terminal's own background, not a lighter one.
    val background = Palette.background.toArgb()
    val density = LocalDensity.current
    // Where the keyboard ends up, not each frame of its slide, and read here so the extra keys come and go in the same
    // frame: the window then resizes once, and a terminal reflows once.
    val keyboard = (WindowInsets.imeAnimationTarget.getBottom(density) - WindowInsets.navigationBars.getBottom(density)).coerceAtLeast(0)
    Box(
        Modifier
            .fillMaxSize()
            .background(Palette.background)
            .systemBarsPadding(),
    ) {
        AndroidView(
            factory = { KeyboardSink(it, runtime.keyboard).also { v -> sink = v } },
            modifier = Modifier.size(1.dp),
        )
        // Always the whole screen: a SurfaceView that changes size shows its last frame stretched until the next one,
        // so the keyboard and the extra keys go over it, and only the Linux window gets smaller.
        AndroidView(
            factory = { context ->
                WindowSurfaceView(context, runtime, window).also {
                    surfaceView = it
                    onSurfaceViewCreated(it)
                }
            },
            update = { it.background = background },
            modifier = Modifier.fillMaxSize(),
        )
        val keyboardHeight = with(density) { keyboard.toDp() }
        // Where the keyboard is still sliding in, so the window's old frame doesn't show beneath it.
        Box(Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(keyboardHeight).background(Palette.background))
        Column(Modifier.fillMaxSize().padding(bottom = keyboardHeight)) {
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .onSizeChanged { surfaceView?.visibleHeight = it.height },
            ) {
                // Over the SurfaceView, which Compose's pointer input doesn't reach.
                // A tap on the prompt line brings the keyboard back, as the ⌨ button does.
                Box(Modifier.fillMaxSize().linuxWindowInput(runtime.wm, window) { sink?.takeUnless { it.showing }?.show() })

                // Floating Toolbar
                Row(
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(12.dp)
                        .background(Palette.overlay, CircleShape)
                        .padding(horizontal = 6.dp, vertical = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    // 1. DeX Taskbar on demand button
                    Box(
                        Modifier
                            .size(32.dp)
                            .background(Color.Transparent, CircleShape)
                            .clickable { onShowDeXTaskbar() }
                            .onGloballyPositioned { coordinates ->
                                val bounds = coordinates.boundsInWindow()
                                surfaceView?.dexTaskbarButtonRect = android.graphics.Rect(
                                    bounds.left.toInt(), bounds.top.toInt(),
                                    bounds.right.toInt(), bounds.bottom.toInt(),
                                )
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        BasicText("⊞", style = Mono.copy(color = Palette.foreground, fontSize = 16.sp))
                    }

                    // 2. Pointer Capture Toggle button
                    Box(
                        Modifier
                            .size(32.dp)
                            .background(
                                if (isPointerCaptured) Palette.accent else Color.Transparent,
                                CircleShape,
                            )
                            .clickable { onTogglePointerCapture() }
                            .onGloballyPositioned { coordinates ->
                                val bounds = coordinates.boundsInWindow()
                                surfaceView?.captureButtonRect = android.graphics.Rect(
                                    bounds.left.toInt(), bounds.top.toInt(),
                                    bounds.right.toInt(), bounds.bottom.toInt(),
                                )
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        BasicText(
                            if (isPointerCaptured) "🎯" else "🖱",
                            style = Mono.copy(
                                color = if (isPointerCaptured) Palette.background else Palette.foreground,
                                fontSize = 14.sp,
                            ),
                        )
                    }

                    // 3. Virtual Keyboard toggle button
                    Box(
                        Modifier
                            .size(32.dp)
                            .background(Color.Transparent, CircleShape)
                            .clickable { sink?.toggle() },
                        contentAlignment = Alignment.Center,
                    ) {
                        BasicText("⌨", style = Mono.copy(color = Palette.foreground, fontSize = 14.sp))
                    }
                }
            }
            // Below the window rather than over it, so the window shrinks and nothing hides behind the keys; only over a
            // docked keyboard (a hardware keyboard's toolbar takes no height, and that keyboard has these keys).
            if (keyboard > 0) ExtraKeys(runtime, Modifier.background(Palette.background))
        }
    }
}

/**
 * The window's pixels: a [WindowRenderer] draws the Linux surfaces straight
 * into this view's surface, without Compose or bitmaps in between. The
 * window is as wide as the view and [visibleHeight] tall, at its top.
 */
private class WindowSurfaceView(
    context: Context,
    private val runtime: Runtime,
    private val window: XdgToplevel,
) : SurfaceView(context),
    SurfaceHolder.Callback {
    private val renderer = WindowRenderer(runtime.server, window)

    var background by renderer::background

    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private var cursorX = 0f
    private var cursorY = 0f
    private var capturedButtons = 0

    var captureButtonRect: android.graphics.Rect? = null
    var dexTaskbarButtonRect: android.graphics.Rect? = null
    var onRequestToggleCapture: (() -> Unit)? = null
    var onRequestShowDeXTaskbar: (() -> Unit)? = null

    /** How much of the view, from its top, the keyboard and the extra keys leave the window; 0 for all of it. */
    var visibleHeight = 0
        set(value) {
            if (value == field) return
            field = value
            layoutWindow()
        }

    fun onCaptureChanged(hasCapture: Boolean) {
        if (!hasCapture) {
            capturedButtons = 0
            updateButtons(0)
        }
    }

    override fun onCapturedPointerEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_MOVE -> {
                val dx = event.getAxisValue(MotionEvent.AXIS_RELATIVE_X)
                val dy = event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y)
                val maxH = if (visibleHeight in 1..surfaceHeight) visibleHeight else surfaceHeight
                cursorX = (cursorX + dx).coerceIn(0f, surfaceWidth.toFloat())
                cursorY = (cursorY + dy).coerceIn(0f, maxH.toFloat())
                val hit = renderer.hitTest(cursorX, cursorY)
                if (hit != null) {
                    runtime.wm.pointerMove(hit)
                }
            }
            MotionEvent.ACTION_SCROLL -> {
                val vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                val hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
                if (vScroll != 0f || hScroll != 0f) {
                    val dy = -vScroll * 15f
                    val dx = -hScroll * 15f
                    val steps = (-vScroll).roundToInt()
                    runtime.wm.scroll(dx, dy, AXIS_SOURCE_WHEEL, steps)
                }
            }
        }

        val currentButtons = event.buttonState
        val pressed = currentButtons and capturedButtons.inv()
        val released = capturedButtons and currentButtons.inv()

        if (pressed != 0) {
            if (pressed and MotionEvent.BUTTON_PRIMARY != 0) {
                val xInt = cursorX.toInt()
                val yInt = cursorY.toInt()
                if (captureButtonRect?.contains(xInt, yInt) == true) {
                    onRequestToggleCapture?.invoke()
                    return true
                }
                if (dexTaskbarButtonRect?.contains(xInt, yInt) == true) {
                    onRequestShowDeXTaskbar?.invoke()
                    return true
                }
            }
            val hit = renderer.hitTest(cursorX, cursorY)
            if (hit != null) {
                runtime.wm.onPress(window, hit)
            }
            if (pressed and MotionEvent.BUTTON_PRIMARY != 0) runtime.wm.pointerButton(BTN_LEFT, true)
            if (pressed and MotionEvent.BUTTON_SECONDARY != 0) runtime.wm.pointerButton(BTN_RIGHT, true)
            if (pressed and MotionEvent.BUTTON_TERTIARY != 0) runtime.wm.pointerButton(BTN_MIDDLE, true)
        }
        if (released != 0) {
            if (released and MotionEvent.BUTTON_PRIMARY != 0) runtime.wm.pointerButton(BTN_LEFT, false)
            if (released and MotionEvent.BUTTON_SECONDARY != 0) runtime.wm.pointerButton(BTN_RIGHT, false)
            if (released and MotionEvent.BUTTON_TERTIARY != 0) runtime.wm.pointerButton(BTN_MIDDLE, false)
        }

        capturedButtons = currentButtons
        return true
    }

    fun handleHover(event: MotionEvent): Boolean {
        cursorX = event.x
        cursorY = event.y
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_ENTER -> {
                val hit = renderer.hitTest(event.x, event.y)
                if (hit != null) {
                    runtime.wm.pointerMove(hit)
                }
                return true
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                runtime.wm.pointerMove(null)
                return true
            }
        }
        return false
    }

    fun handleScroll(event: MotionEvent): Boolean {
        val hit = renderer.hitTest(event.x, event.y)
        if (hit != null) {
            runtime.wm.pointerMove(hit)
        }
        val vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
        val hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
        if (vScroll != 0f || hScroll != 0f) {
            val dy = -vScroll * 15f
            val dx = -hScroll * 15f
            val steps = (-vScroll).roundToInt()
            runtime.wm.scroll(dx, dy, AXIS_SOURCE_WHEEL, steps)
            return true
        }
        return false
    }

    fun handleMouseTouch(event: MotionEvent): Boolean {
        cursorX = event.x
        cursorY = event.y
        val hit = renderer.hitTest(event.x, event.y)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (hit != null) {
                    runtime.wm.onPress(window, hit)
                    runtime.wm.pointerMove(hit)
                }
                updateButtons(event.buttonState)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (hit != null) {
                    runtime.wm.pointerMove(hit)
                }
                updateButtons(event.buttonState)
                return true
            }
            MotionEvent.ACTION_UP -> {
                updateButtons(0)
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                updateButtons(0)
                runtime.wm.pointerMove(null)
                return true
            }
        }
        return false
    }

    private fun updateButtons(currentButtons: Int) {
        val pressed = currentButtons and capturedButtons.inv()
        val released = capturedButtons and currentButtons.inv()

        if (pressed and MotionEvent.BUTTON_PRIMARY != 0) runtime.wm.pointerButton(BTN_LEFT, true)
        if (pressed and MotionEvent.BUTTON_SECONDARY != 0) runtime.wm.pointerButton(BTN_RIGHT, true)
        if (pressed and MotionEvent.BUTTON_TERTIARY != 0) runtime.wm.pointerButton(BTN_MIDDLE, true)

        if (released and MotionEvent.BUTTON_PRIMARY != 0) runtime.wm.pointerButton(BTN_LEFT, false)
        if (released and MotionEvent.BUTTON_SECONDARY != 0) runtime.wm.pointerButton(BTN_RIGHT, false)
        if (released and MotionEvent.BUTTON_TERTIARY != 0) runtime.wm.pointerButton(BTN_MIDDLE, false)

        capturedButtons = currentButtons
    }

    override fun onHoverEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            if (handleHover(event)) return true
        }
        return super.onHoverEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
                if (handleScroll(event)) return true
            } else if (handleHover(event)) {
                return true
            }
        }
        return super.onGenericMotionEvent(event)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
            if (handleMouseTouch(event)) return true
        }
        return super.onTouchEvent(event)
    }

    private fun currentScale(): Int {
        if (isExternalDisplay()) return 1
        return (context.resources.displayMetrics.density.toInt()).coerceIn(1, 4)
    }

    private fun isExternalDisplay(): Boolean {
        return if (Build.VERSION.SDK_INT >= 30) {
            (display?.displayId ?: 0) != 0
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(android.view.WindowManager::class.java)?.defaultDisplay?.displayId ?: 0) != 0
        }
    }

    fun layoutWindow() {
        if (surfaceWidth == 0) return
        val h = if (visibleHeight in 1..surfaceHeight) visibleHeight else surfaceHeight
        renderer.visibleHeight = h
        val s = currentScale()
        renderer.scale = s
        val external = isExternalDisplay()
        runtime.wm.applyAppLayout(window, surfaceWidth, h, s, external)
    }

    init {
        holder.addCallback(this)
        holder.setFormat(android.graphics.PixelFormat.RGBX_8888)
        pointerIcon = PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        window.renderer = renderer
        pointerIcon = PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL)
    }

    override fun onDetachedFromWindow() {
        if (window.renderer === renderer) window.renderer = null
        renderer.release()
        super.onDetachedFromWindow()
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int,
    ) {
        surfaceWidth = width
        surfaceHeight = height
        val s = currentScale()
        val external = isExternalDisplay()
        renderer.scale = s
        renderer.attach(holder.surface, width, height, s)
        if (external) {
            val metrics = context.resources.displayMetrics
            val mmPerPx = if (metrics.xdpi > 0) 25.4f / metrics.xdpi else 0.15f
            runtime.server.updateOutput(
                dev.omalauncher.wayland.OutputInfo(
                    width,
                    height,
                    s,
                    (width * mmPerPx).toInt(),
                    (height * mmPerPx).toInt(),
                ),
            )
        }
        layoutWindow()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) = renderer.detach()
}
