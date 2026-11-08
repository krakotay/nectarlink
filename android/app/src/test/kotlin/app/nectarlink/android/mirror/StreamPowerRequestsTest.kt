// SPDX-License-Identifier: GPL-3.0-or-later
package app.nectarlink.android.mirror

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class StreamPowerRequestsTest {
    @Test
    fun failedStartupCannotWakeAnotherStreamsPanel() {
        val power = StreamPowerRequests()
        power.set("screen", true, screenOff = true)
        power.set("app-start:pc:1", true)
        power.set("app-start:pc:1", false)
        assertTrue(power.streaming)
        assertTrue(power.screenOff)
        assertTrue(power.stayAwake)
    }

    @Test
    fun startupHandsPowerToItsStreamWithoutRestoringThePanel() {
        val power = StreamPowerRequests()
        power.set("app-start:pc:1", true, screenOff = true)
        power.set("apps", true, screenOff = true)
        power.set("app-start:pc:1", false)
        assertTrue(power.screenOff)
        power.set("screen", true)
        power.set("apps", false)
        assertTrue(power.streaming)
        assertFalse(power.screenOff)
        assertFalse(power.stayAwake)
    }

    @Test
    fun closingScreenMirrorKeepsAppsPowerRequestUntilLastOwnerCloses() {
        val power = StreamPowerRequests()
        power.set("screen", true, stayAwake = true)
        power.set("apps", true, screenOff = true)
        power.set("screen", false)
        assertTrue(power.streaming)
        assertTrue(power.screenOff)
        assertTrue(power.stayAwake)
        power.set("apps", false)
        assertFalse(power.streaming)
        assertFalse(power.screenOff)
        assertFalse(power.stayAwake)
    }
}
