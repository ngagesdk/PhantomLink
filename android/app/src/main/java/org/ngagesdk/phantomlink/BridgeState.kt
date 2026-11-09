/*
 * State shared between the service and the UI.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.ngagesdk.phantomlink.bridge.BridgeEngine
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

enum class BtState { OFF, STARTING, LISTENING, ERROR }

data class BridgeUi(
    val running: Boolean = false,
    val bluetooth: BtState = BtState.OFF,
    /** the phone's Bluetooth name, as the N-Gage shows it */
    val phoneName: String? = null,
    val networkUp: Boolean = true,
    val autoServer: Boolean = true,
    val serverAddress: String? = null,
    val devices: List<BridgeEngine.DeviceInfo> = emptyList(),
    /** settings the running bridge was started with */
    val activeSettings: AppSettings? = null,
    val error: String? = null,
)

data class LogLine(val time: String, val level: Int, val text: String)

object BridgeState {
    private const val MAX_LOG = 600
    private val fmt = SimpleDateFormat("HH:mm:ss", Locale.ROOT)

    private val _ui = MutableStateFlow(BridgeUi())
    val ui: StateFlow<BridgeUi> = _ui.asStateFlow()

    private val _log = MutableStateFlow<List<LogLine>>(emptyList())
    val log: StateFlow<List<LogLine>> = _log.asStateFlow()

    fun update(transform: (BridgeUi) -> BridgeUi) = _ui.update(transform)

    fun log(level: Int, text: String) {
        val line = LogLine(synchronized(fmt) { fmt.format(Date()) }, level, text)
        _log.update { (if (it.size >= MAX_LOG) it.drop(it.size - MAX_LOG + 1) else it) + line }
    }

    fun clearLog() {
        _log.value = emptyList()
    }

    fun logText(): String = _log.value.joinToString("\n") { "${it.time} ${"EWID"[it.level.coerceIn(0, 3)]} ${it.text}" }
}
