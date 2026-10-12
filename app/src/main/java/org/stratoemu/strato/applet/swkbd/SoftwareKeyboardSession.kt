/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.applet.swkbd

import java.util.concurrent.CompletableFuture

data class SoftwareKeyboardResult(val cancelled : Boolean, val text : String)

/** Cross-thread handoff: a result can arrive before the native thread starts waiting. */
class SoftwareKeyboardSession {
    private var pending = CompletableFuture<SoftwareKeyboardResult>()
    private var closed = false

    @Synchronized
    fun submit(result : SoftwareKeyboardResult) : Boolean = !closed && pending.complete(result)

    fun awaitResult() : SoftwareKeyboardResult {
        val future = synchronized(this) {
            if (closed) return SoftwareKeyboardResult(true, "")
            pending
        }
        return future.get()
    }

    /** Only a rejected text check starts a new input round. */
    @Synchronized
    fun retry() {
        if (!closed && pending.isDone) pending = CompletableFuture()
    }

    @Synchronized
    fun close() {
        closed = true
        pending.complete(SoftwareKeyboardResult(true, ""))
    }
}
