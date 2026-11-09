/*
 * PhantomLink bridge for Android: lets N-Gages play online through the
 * phone, like the ESP32 board or the PC bridge.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.ui.unit.dp
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.ngagesdk.phantomlink.ui.HomeScreen
import org.ngagesdk.phantomlink.ui.LogScreen
import org.ngagesdk.phantomlink.ui.PhantomTheme
import org.ngagesdk.phantomlink.ui.SettingsScreen
import org.ngagesdk.phantomlink.ui.isDark

class MainActivity : ComponentActivity() {

    private lateinit var store: SettingsStore
    private var message by mutableStateOf<String?>(null)

    private val startPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        if (BridgeService.hasBluetoothPermission(this)) {
            ensureBluetoothThenStart()
        } else {
            message = "PhantomLink needs the Nearby devices permission to accept the N-Gage's connection."
        }
    }

    private val enableBluetooth = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        // Start either way; the bridge waits until Bluetooth is switched on.
        BridgeService.start(this)
    }

    private val advertisePermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        if (ok) requestDiscoverable() else message = "Permission needed to make the phone visible."
    }

    private val discoverable = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (it.resultCode != RESULT_CANCELED) message = "Visible to the N-Gage for 5 minutes."
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = SettingsStore.get(this)
        setContent {
            val settings by store.settings.collectAsStateWithLifecycle()
            val dark = isDark(settings.theme)
            DisposableEffect(dark) {
                enableEdgeToEdge(
                    statusBarStyle = if (dark) SystemBarStyle.dark(Color.TRANSPARENT)
                    else SystemBarStyle.light(Color.TRANSPARENT, Color.TRANSPARENT),
                    navigationBarStyle = if (dark) SystemBarStyle.dark(Color.TRANSPARENT)
                    else SystemBarStyle.light(Color.TRANSPARENT, Color.TRANSPARENT),
                )
                onDispose { }
            }
            PhantomTheme(dark) { App(settings) }
        }
    }

    @Composable
    private fun App(settings: AppSettings) {
        val ui by BridgeState.ui.collectAsStateWithLifecycle()
        val log by BridgeState.log.collectAsStateWithLifecycle()
        var tab by rememberSaveable { mutableIntStateOf(0) }
        val snackbar = remember { SnackbarHostState() }
        LaunchedEffect(message) {
            message?.let {
                snackbar.showSnackbar(it)
                message = null
            }
        }
        Scaffold(
            containerColor = MaterialTheme.colorScheme.background,
            snackbarHost = { SnackbarHost(snackbar) },
            bottomBar = {
                NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainerLowest, tonalElevation = 0.dp) {
                    listOf(
                        Triple("Bridge", Icons.Filled.Home, 0),
                        Triple("Log", Icons.AutoMirrored.Filled.List, 1),
                        Triple("Settings", Icons.Filled.Settings, 2),
                    ).forEach { (label, icon, i) ->
                        NavigationBarItem(
                            selected = tab == i, onClick = { tab = i },
                            icon = { Icon(icon, contentDescription = null) }, label = { Text(label) },
                            colors = NavigationBarItemDefaults.colors(indicatorColor = MaterialTheme.colorScheme.primaryContainer),
                        )
                    }
                }
            },
        ) { padding ->
            when (tab) {
                0 -> HomeScreen(
                    ui, settings,
                    onStart = ::requestStart,
                    onStop = { BridgeService.stop(this) },
                    onRestart = { BridgeService.restart(this) },
                    onMakeVisible = ::makeVisible,
                    contentPadding = padding,
                )
                1 -> LogScreen(log, onShare = ::shareLog, onClear = BridgeState::clearLog, contentPadding = padding)
                else -> SettingsScreen(settings, onChange = store::update, contentPadding = padding)
            }
        }
    }

    /* ------------------------------------------------------------------ */

    private fun requestStart() {
        val needed = buildList {
            if (Build.VERSION.SDK_INT >= 31) add(Manifest.permission.BLUETOOTH_CONNECT)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isEmpty()) ensureBluetoothThenStart() else startPermissions.launch(needed.toTypedArray())
    }

    private fun ensureBluetoothThenStart() {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) {
            message = "This phone has no Bluetooth."
            return
        }
        if (!adapter.isEnabled) {
            try {
                enableBluetooth.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
                return
            } catch (_: Exception) {
                // no dialog available: start and wait for Bluetooth
            }
        }
        BridgeService.start(this)
    }

    private fun makeVisible() {
        if (Build.VERSION.SDK_INT >= 31 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_ADVERTISE) != PackageManager.PERMISSION_GRANTED
        ) {
            advertisePermission.launch(Manifest.permission.BLUETOOTH_ADVERTISE)
        } else {
            requestDiscoverable()
        }
    }

    private fun requestDiscoverable() {
        try {
            discoverable.launch(
                Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE).putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 300),
            )
        } catch (_: Exception) {
            message = "Open the phone's Bluetooth settings to make it visible."
        }
    }

    private fun shareLog() {
        val send = Intent(Intent.ACTION_SEND).setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, "PhantomLink bridge log")
            .putExtra(Intent.EXTRA_TEXT, BridgeState.logText())
        startActivity(Intent.createChooser(send, "Share log"))
    }
}
