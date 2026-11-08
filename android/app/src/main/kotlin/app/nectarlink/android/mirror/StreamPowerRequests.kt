// SPDX-License-Identifier: GPL-3.0-or-later
package app.nectarlink.android.mirror

/** Requests include streams still starting; only releasing the last owner restores normal power. */
internal class StreamPowerRequests {
    private data class Want(val stayAwake: Boolean, val screenOff: Boolean)
    private val wants = HashMap<String, Want>()

    val streaming: Boolean get() = wants.isNotEmpty()
    val screenOff: Boolean get() = wants.values.any { it.screenOff }
    val stayAwake: Boolean get() = screenOff || wants.values.any { it.stayAwake }

    fun set(source: String, streaming: Boolean, stayAwake: Boolean = false, screenOff: Boolean = false) {
        if (streaming) wants[source] = Want(stayAwake, screenOff) else wants.remove(source)
    }
}
