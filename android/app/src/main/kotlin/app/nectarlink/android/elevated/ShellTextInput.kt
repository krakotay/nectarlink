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
            val focused = focusedField(ui, target) ?: return false
            val snapshot = focused.text
            val old = when {
                focused.isShowingHintText -> ""
                snapshot != null -> snapshot.toString()
                else -> return unavailable(target, "editor text snapshot unavailable (${properties(focused)})")
            }
            val anchor = focused.textSelectionStart.takeIf { it in 0..old.length } ?: old.length
            val extent = focused.textSelectionEnd.takeIf { it in 0..old.length } ?: anchor
            val start = minOf(anchor, extent)
            val end = maxOf(anchor, extent)
            val replacement = old.replaceRange(start, end, text)
            val changed = focused.performAction(
                AccessibilityNodeInfo.ACTION_SET_TEXT,
                Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, replacement) },
            )
            if (!changed) return unavailable(target, "field rejected direct text (${properties(focused)})")
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
    fun canPaste(): Boolean = !keyguardLocked()

    /** Guard only a known collapsed selection at the end; unknown selections retain native handling. */
    @Synchronized
    fun forwardDeleteAtEnd(display: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
        val target = display.coerceAtLeast(0)
        if (target == 0 && keyguardLocked()) return false
        val ui = connection() ?: return false
        return runCatching {
            val field = focusedField(ui, target) ?: return false
            val length = if (field.isShowingHintText) 0 else field.text?.length ?: return false
            field.textSelectionStart == length && field.textSelectionEnd == length
        }.getOrDefault(false)
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
        val windows = ui.windowsOnAllDisplays[display].orEmpty()
        val focusedWindow = windows.firstOrNull { it.isFocused }
        if (focusedWindow != null && focusedWindow.type != AccessibilityWindowInfo.TYPE_APPLICATION) {
            return missing(display, "focused window is not an application (type=${focusedWindow.type})")
        }
        // A ROM can drop the window-focus flag too while the primary display sleeps. A sole
        // app window is still unambiguous, unless another focused window is covering it.
        val window = focusedWindow ?: windows.filter { it.type == AccessibilityWindowInfo.TYPE_APPLICATION }.singleOrNull()
            ?: return missing(display, "no unique application window (windows=${windows.size})")
        val root = window.root ?: return missing(display, "application window has no accessibility root (window=${window.id})")
        val focused = root.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)?.takeIf { it.windowId == window.id }
        // The cache was cleared before querying. Do not refresh these new snapshots again:
        // that adds a second node lookup and can replace the focus result with different data.
        if (focused != null) {
            if (focused.isPassword) return missing(display, "focused node is a password field")
            if (editable(focused)) return focused
            // Some custom widgets expose a focus container instead of the editor itself.
            // Its missing editable flags must not abort the descendant search.
            if (focused != root && focused.childCount > 0) {
                val inside = searchEditors(focused, window.id, allowDisabledSole = true)
                if (inside.passwordFocused) return missing(display, "focused descendant is a password field")
                inside.field?.let { return it }
            }
        }

        // A non-editor focus result can be a wrapper or placeholder. Search this application
        // window rather than rejecting it; never guess between multiple fields/displays.
        val found = searchEditors(root, window.id, allowDisabledSole = focused == root)
        if (found.passwordFocused) return missing(display, "focused editor is a password field")
        found.field?.let { return it }
        val focusState = focused?.let(::properties) ?: "none"
        return missing(
            display,
            "no unique editor (window=${window.id}, nodes=${found.visited}, editors=${found.editors}, " +
                "truncated=${found.truncated}); focus=[$focusState]; root=[${properties(root)}]",
        )
    }

    private data class EditorSearch(
        val field: AccessibilityNodeInfo?,
        val visited: Int,
        val editors: Int,
        val truncated: Boolean,
        val passwordFocused: Boolean = false,
    )

    private fun searchEditors(root: AccessibilityNodeInfo, window: Int, allowDisabledSole: Boolean): EditorSearch {
        val pending = ArrayDeque<AccessibilityNodeInfo>().apply { add(root) }
        var sole: AccessibilityNodeInfo? = null
        var ambiguous = false
        var truncated = false
        var visited = 0
        var editors = 0
        while (pending.isNotEmpty() && visited++ < MAX_NODES) {
            val node = pending.removeFirst()
            if (node.windowId != window) continue
            if (node.isFocused && node.isPassword) return EditorSearch(null, visited, editors, truncated, passwordFocused = true)
            if (editor(node)) {
                editors++
                if (node.isFocused && editable(node)) return EditorSearch(node, visited, editors, truncated)
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
        truncated = truncated || pending.isNotEmpty()
        val field = sole?.takeIf { !ambiguous && !truncated && (it.isEnabled || allowDisabledSole) && editable(it) }
        return EditorSearch(field, visited, editors, truncated)
    }

    private fun missing(display: Int, reason: String): AccessibilityNodeInfo? {
        unavailable(display, reason)
        return null
    }

    private fun editable(node: AccessibilityNodeInfo): Boolean = !node.isPassword && editor(node)

    // Enabled is advisory for a known focused editor (or its sole editable descendant).
    // Broader unfocused fallback fields still have to be enabled. The action itself decides
    // whether editing is accepted; password and display/window checks remain.
    private fun editor(node: AccessibilityNodeInfo): Boolean =
        node.isEditable || node.className == "android.widget.EditText" ||
            node.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }

    private fun properties(node: AccessibilityNodeInfo): String =
        "class=${node.className?.toString()?.takeIf { NODE_CLASS.matches(it) } ?: "unknown"}, " +
            "children=${node.childCount}, focused=${node.isFocused}, visible=${node.isVisibleToUser}, " +
            "hasText=${node.text != null}, enabled=${node.isEnabled}, editable=${node.isEditable}, " +
            "setText=${node.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }}, " +
            "editTextClass=${node.className == "android.widget.EditText"}, password=${node.isPassword}"

    @Synchronized
    private fun unavailable(display: Int, reason: String): Boolean {
        val now = SystemClock.uptimeMillis()
        val previous = failures[display]
        if (previous == null || now - previous.second >= FAILURE_LOG_MS) {
            failures[display] = reason to now
            // No field text, clipboard contents, or typed characters in diagnostics.
            val locked = runCatching { keyguardLocked() }.getOrNull()
            System.err.println("text input on display $display: $reason; phoneLocked=$locked")
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
    private val NODE_CLASS = Regex("[A-Za-z_][A-Za-z0-9_.$]{0,159}")
}
