/*
 * PhantomLink bridge for Android: lets N-Gages play online through the
 * phone, like the ESP32 board or the PC bridge.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.ActivityNotFoundException
import android.content.ClipData
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
import androidx.core.content.FileProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import de.ngagesdk.phantomlink.ui.HomeScreen
import de.ngagesdk.phantomlink.ui.LogScreen
import de.ngagesdk.phantomlink.ui.PhantomTheme
import de.ngagesdk.phantomlink.ui.SettingsScreen
import de.ngagesdk.phantomlink.ui.isDark
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {

    private lateinit var store: SettingsStore
    private var message by mutableStateOf<String?>(null)
    private var downloadingInstaller by mutableStateOf(false)

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
                    onSendInstaller = ::sendInstaller,
                    downloadingInstaller = downloadingInstaller,
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

    private fun sendInstaller() {
        if (downloadingInstaller) return
        downloadingInstaller = true
        lifecycleScope.launch {
            try {
                val file = withContext(Dispatchers.IO) { downloadInstaller() }
                val uri = FileProvider.getUriForFile(this@MainActivity, "$packageName.files", file)
                // The system Bluetooth share filter may omit application/octet-stream.
                val send = Intent(Intent.ACTION_SEND).setType("application/octet-stream")
                    .setClassName("com.android.bluetooth", "com.android.bluetooth.opp.BluetoothOppLauncherActivity")
                    .putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    .apply { clipData = ClipData.newRawUri("PhantomLink.sis", uri) }
                startActivity(send)
            } catch (e: IOException) {
                message = "Could not download PhantomLink.sis: ${e.message}"
            } catch (_: ActivityNotFoundException) {
                message = "Bluetooth file sharing is not available on this Android device."
            } catch (_: SecurityException) {
                message = "This Android device does not allow Bluetooth file sharing from PhantomLink."
            } finally {
                downloadingInstaller = false
            }
        }
    }

    private fun downloadInstaller(): File {
        val directory = File(cacheDir, "installer/${UUID.randomUUID()}")
        if (!directory.mkdirs()) throw IOException("Could not create download directory")
        val file = File(directory, "PhantomLink.sis")
        try {
            val connection = URL("https://github.com/ngagesdk/PhantomLink/releases/download/continuous-integration/PhantomLink.sis")
                .openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = 15000
                connection.readTimeout = 30000
                val status = connection.responseCode
                if (status !in 200..299) throw IOException("HTTP $status")
                val expectedLength = connection.contentLengthLong
                val maxSize = 50L * 1024 * 1024
                if (expectedLength > maxSize) throw IOException("Installer is too large")
                var total = 0L
                connection.inputStream.use { input ->
                    file.outputStream().use { output ->
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count == -1) break
                            total += count
                            if (total > maxSize) throw IOException("Installer is too large")
                            output.write(buffer, 0, count)
                        }
                    }
                }
                if (total == 0L || (expectedLength >= 0 && total != expectedLength)) {
                    throw IOException("Incomplete installer download")
                }
            } finally {
                connection.disconnect()
            }
            return file
        } catch (e: IOException) {
            file.delete()
            directory.delete()
            throw e
        }
    }

    private fun shareLog() {
        val send = Intent(Intent.ACTION_SEND).setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, "PhantomLink bridge log")
            .putExtra(Intent.EXTRA_TEXT, BridgeState.logText())
        startActivity(Intent.createChooser(send, "Share log"))
    }
}
