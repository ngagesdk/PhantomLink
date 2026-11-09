/*
 * App settings. The bridge part mirrors pcbridge/gcbridge.ini.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink

import android.content.Context
import android.content.SharedPreferences
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.ngagesdk.phantomlink.bridge.BridgeConfig

enum class ThemeMode { LIGHT, DARK, SYSTEM }

data class AppSettings(
    /** true = find the server on the local network */
    val autoServer: Boolean = true,
    val serverHost: String = "",
    val port: Int = BridgeConfig.DEFAULT_PORT,
    val accessKey: String = "",
    /** answered automatically when an N-Gage pairs; empty = confirm on the phone */
    val pin: String = "1234",
    val serviceName: String = "GameComms Bridge",
    val maxDevices: Int = 4,
    val logLevel: Int = 2,
    val theme: ThemeMode = ThemeMode.LIGHT,
) {
    fun toBridgeConfig() = BridgeConfig(
        if (autoServer || serverHost.isBlank()) "auto" else serverHost.trim(),
        port, accessKey, maxDevices, logLevel,
    )

    /** true if a running bridge needs a restart for [other] to take effect */
    fun bridgeDiffers(other: AppSettings) = copy(theme = other.theme) != other
}

class SettingsStore private constructor(context: Context) {
    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("settings", Context.MODE_PRIVATE)
    private val state = MutableStateFlow(load())
    val settings: StateFlow<AppSettings> = state.asStateFlow()

    fun update(transform: (AppSettings) -> AppSettings) {
        val s = transform(state.value)
        state.value = s
        prefs.edit()
            .putBoolean("auto_server", s.autoServer)
            .putString("server", s.serverHost)
            .putInt("port", s.port)
            .putString("key", s.accessKey)
            .putString("pin", s.pin)
            .putString("name", s.serviceName)
            .putInt("max_devices", s.maxDevices)
            .putInt("log_level", s.logLevel)
            .putString("theme", s.theme.name)
            .apply()
    }

    private fun load(): AppSettings {
        val d = AppSettings()
        return AppSettings(
            autoServer = prefs.getBoolean("auto_server", d.autoServer),
            serverHost = prefs.getString("server", d.serverHost) ?: d.serverHost,
            port = prefs.getInt("port", d.port),
            accessKey = prefs.getString("key", d.accessKey) ?: d.accessKey,
            pin = prefs.getString("pin", d.pin) ?: d.pin,
            serviceName = prefs.getString("name", d.serviceName) ?: d.serviceName,
            maxDevices = prefs.getInt("max_devices", d.maxDevices),
            logLevel = prefs.getInt("log_level", d.logLevel),
            theme = runCatching { ThemeMode.valueOf(prefs.getString("theme", null) ?: "") }.getOrDefault(d.theme),
        )
    }

    companion object {
        @Volatile private var instance: SettingsStore? = null
        fun get(context: Context): SettingsStore =
            instance ?: synchronized(this) { instance ?: SettingsStore(context).also { instance = it } }
    }
}
