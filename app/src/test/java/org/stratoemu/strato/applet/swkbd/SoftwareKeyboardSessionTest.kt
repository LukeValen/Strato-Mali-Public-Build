package org.stratoemu.strato.applet.swkbd

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class SoftwareKeyboardSessionTest {
    @Test fun inputBeforeNativeWaitIsNotLost() {
        val session = SoftwareKeyboardSession()
        val entered = SoftwareKeyboardResult(false, "João 🫐")
        assertTrue(session.submit(entered))
        assertFalse(session.submit(SoftwareKeyboardResult(false, "duplicate")))
        assertEquals(entered, session.awaitResult())
    }

    @Test fun rejectedTextCanBeCorrected() {
        val session = SoftwareKeyboardSession()
        session.submit(SoftwareKeyboardResult(false, "bad"))
        assertEquals("bad", session.awaitResult().text)
        session.retry()
        session.submit(SoftwareKeyboardResult(false, "Luke"))
        assertEquals("Luke", session.awaitResult().text)
    }

    @Test fun destroyUnblocksNativeWaitAndCannotBeReopened() {
        val worker = Executors.newSingleThreadExecutor()
        try {
            val session = SoftwareKeyboardSession()
            val waiting = worker.submit<SoftwareKeyboardResult> { session.awaitResult() }
            session.close()
            assertTrue(waiting.get(2, TimeUnit.SECONDS).cancelled)
            session.retry()
            assertFalse(session.submit(SoftwareKeyboardResult(false, "late")))
            assertTrue(session.awaitResult().cancelled)
        } finally { worker.shutdownNow() }
    }
}
