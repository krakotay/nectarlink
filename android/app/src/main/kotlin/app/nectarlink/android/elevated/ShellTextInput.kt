// SPDX-License-Identifier: GPL-3.0-or-later
package app.nectarlink.android.elevated

import android.accessibilityservice.AccessibilityServiceInfo
import android.annotation.SuppressLint
import android.app.KeyguardManager
import android.app.UiAutomation
import android.os.Build
import android.os.Bundle
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityWindowInfo
import java.util.ArrayDeque

/**
 * Text in an Elevated app display, without the device-wide clipboard. Android
 * blocks clipboard reads while the physical phone is locked, even though our
 * ALWAYS_UNLOCKED app display still takes keys. The shell may edit its focused
 * field through accessibility, without requiring a separate enabled service.
 */
@SuppressLint("PrivateApi", "DiscouragedPrivateApi", "BlockedPrivateApi")
internal object ShellTextInput {
    private var retryAfter = 0L
    private var automation: UiAutomation? = null
    private var callbacks: HandlerThread? = null
    private val displays = HashSet<Int>()
    private val failures = HashMap<Int, Pair<String, Long>>()

    /** Start window tracking before the app gains focus, rather than on its first letter. */
    @Synchronized
    fun prepare(display: Int) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        displays.add(display)
        connection()
    }

    @Synchronized
    fun release(display: Int) {
        displays.remove(display)
        failures.remove(display)
        if (displays.isEmpty()) close()
    }

    /** The default display is allowed only while unlocked; app displays have their own lock policy. */
    @Synchronized
    fun insert(display: Int, text: String): Boolean {
        val target = display.coerceAtLeast(0)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
        if (target == 0 && keyguardLocked()) return unavailable(target, "default display locked")
        val ui = connection() ?: return unavailable(target, "accessibility unavailable")
        return runCatching {
            val focused = focusedField(ui, target) ?: return unavailable(target, "no unambiguous editable field")
            val old = if (focused.isShowingHintText) "" else focused.text?.toString().orEmpty()
            val anchor = focused.textSelectionStart.takeIf { it in 0..old.length } ?: old.length
            val extent = focused.textSelectionEnd.takeIf { it in 0..old.length } ?: anchor
            val start = minOf(anchor, extent)
            val end = maxOf(anchor, extent)
            val replacement = old.replaceRange(start, end, text)
            val changed = focused.performAction(
                AccessibilityNodeInfo.ACTION_SET_TEXT,
                Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, replacement) },
            )
            if (!changed) return unavailable(target, "field rejected direct text")
            val cursor = start + text.length
            focused.performAction(
                AccessibilityNodeInfo.ACTION_SET_SELECTION,
                Bundle().apply {
                    putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, cursor)
                    putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, cursor)
                },
            )
            failures.remove(target)
            true
        }.onFailure {
            close()
            retryAfter = SystemClock.uptimeMillis() + 5_000
            unavailable(target, "accessibility error: ${it.javaClass.simpleName}")
        }.getOrDefault(false)
    }

    /** Send belongs to the editor on this display, never to the physical screen's active field. */
    @Synchronized
    fun send(display: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
        val target = display.coerceAtLeast(0)
        if (target == 0 && keyguardLocked()) return false
        val ui = connection() ?: return false
        return runCatching {
            focusedField(ui, target)?.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.id) == true
        }.getOrDefault(false)
    }

    /** Android denies apps clipboard reads when the device is locked, even on an unlocked app display. */
    fun canPaste(display: Int): Boolean = if (keyguardLocked()) {
        unavailable(display.coerceAtLeast(0), "clipboard unavailable while phone locked")
    } else {
        true
    }

    private fun keyguardLocked(): Boolean {
        val keyguard = ShellContext.get().getSystemService(KeyguardManager::class.java) ?: return true
        return keyguard.isDeviceLocked || keyguard.isKeyguardLocked
    }

    private fun focusedField(ui: UiAutomation, display: Int): AccessibilityNodeInfo? {
        // Do not rely on cached focus surviving the primary display's sleep transition.
        // Query fresh nodes, including custom chat editors such as Telegram's.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ui.clearCache()
        } else {
            ui.serviceInfo = ui.serviceInfo // Also clears UiAutomation's node/window cache.
        }
        val window = ui.windowsOnAllDisplays[display].orEmpty().firstOrNull {
            it.isFocused && it.type == AccessibilityWindowInfo.TYPE_APPLICATION
        } ?: return null
        val root = window.root ?: return null
        root.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)?.let {
            if (it.windowId == window.id && it.refresh()) return it.takeIf(::editable)
        }
        // If FOCUS_INPUT disappears while the app window still receives keys, accept its
        // focused editor, or its sole editable field.
        // Never guess between multiple fields, or search another display/window.
        val pending = ArrayDeque<AccessibilityNodeInfo>().apply { add(root) }
        var sole: AccessibilityNodeInfo? = null
        var ambiguous = false
        var truncated = false
        var visited = 0
        while (pending.isNotEmpty() && visited++ < MAX_NODES) {
            val node = pending.removeFirst()
            if (node.windowId != window.id) continue
            if (editor(node)) {
                if (node.isFocused && node.refresh()) return node.takeIf(::editable)
                if (sole != null) ambiguous = true
                sole = node
            }
            val remaining = (MAX_NODES - visited - pending.size).coerceAtLeast(0)
            val children = node.childCount.coerceAtMost(remaining)
            if (children < node.childCount) truncated = true
            for (i in 0 until children) {
                node.getChild(i)?.let { pending.addLast(it) }
            }
        }
        // A truncated tree may contain another field we have not visited.
        return sole?.takeIf { !ambiguous && !truncated && pending.isEmpty() && it.refresh() && editable(it) }
    }

    private fun editable(node: AccessibilityNodeInfo): Boolean = !node.isPassword && editor(node)

    private fun editor(node: AccessibilityNodeInfo): Boolean = node.isEnabled &&
        (node.isEditable || node.className == "android.widget.EditText" ||
            node.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT })

    @Synchronized
    private fun unavailable(display: Int, reason: String): Boolean {
        val now = SystemClock.uptimeMillis()
        val previous = failures[display]
        if (previous == null || now - previous.second >= FAILURE_LOG_MS) {
            failures[display] = reason to now
            // No field text, clipboard contents, or typed characters in diagnostics.
            System.err.println("text input on display $display: $reason")
        }
        return false
    }

    private fun connection(): UiAutomation? {
        automation?.let { return it }
        if (SystemClock.uptimeMillis() < retryAfter) return null
        retryAfter = SystemClock.uptimeMillis() + 5_000
        val thread = HandlerThread("app-text-input").apply { start() }
        return runCatching {
            val binderType = Class.forName("android.app.IUiAutomationConnection")
            val binder = Class.forName("android.app.UiAutomationConnection").getDeclaredConstructor().newInstance()
            val ui = UiAutomation::class.java.getConstructor(Looper::class.java, binderType)
                .newInstance(thread.looper, binder) as UiAutomation
            // Keep the user's existing accessibility services running.
            UiAutomation::class.java.getMethod("connect", Int::class.javaPrimitiveType)
                .invoke(ui, UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES)
            callbacks = thread
            automation = ui
            ui.serviceInfo = ui.serviceInfo.apply {
                flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS or
                    AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS
            }
            ui
        }.onFailure {
            close()
            thread.quitSafely()
            System.err.println("app text accessibility unavailable: ${it.javaClass.simpleName}")
        }.getOrNull()
    }

    @Synchronized
    fun close() {
        automation?.let { runCatching { UiAutomation::class.java.getMethod("disconnect").invoke(it) } }
        automation = null
        callbacks?.quitSafely()
        callbacks = null
    }

    private const val MAX_NODES = 256
    private const val FAILURE_LOG_MS = 5_000L
}
