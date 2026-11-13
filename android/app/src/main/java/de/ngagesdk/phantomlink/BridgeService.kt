/*
 * Foreground service running the bridge: Bluetooth listener, bridge engine,
 * automatic PIN answer, network state and wake locks.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import de.ngagesdk.phantomlink.BridgeEngine
import de.ngagesdk.phantomlink.NativeBridge

class BridgeService : Service() {

    private val main = Handler(Looper.getMainLooper())
    @Volatile private var engine: BridgeEngine? = null
    private var listener: BluetoothListener? = null
    private var adapter: BluetoothAdapter? = null
    private var settings = AppSettings()
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var receiverRegistered = false
    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private var lastNotification: String? = null
    private val retryListen = Runnable { if (engine != null && adapter?.isEnabled == true) startListener() }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutdown()
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_RESTART && engine != null) {
            BridgeState.log(BridgeEngine.LOG_INFO, "restarting with the new settings")
            shutdown()
            begin()
            return START_NOT_STICKY
        }
        // every startForegroundService() needs a startForeground(), even when running
        if (!enterForeground()) {
            if (engine == null) stopSelf()
            return START_NOT_STICKY
        }
        if (engine == null) {
            begin()
        } else {
            lastNotification = null
            engine?.refresh()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        shutdown()
        super.onDestroy()
    }

    /* ------------------------------------------------------------------ */

    private fun enterForeground(): Boolean {
        createChannel()
        return try {
            ServiceCompat.startForeground(
                this, NOTIFICATION_ID, buildNotification(getString(R.string.waiting_short)),
                if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0,
            )
            true
        } catch (e: Exception) { // missing permission, background start not allowed
            fail("Cannot start the bridge: ${e.message}")
            false
        }
    }

    private fun fail(message: String) {
        BridgeState.log(BridgeEngine.LOG_ERROR, message)
        BridgeState.update { it.copy(running = false, bluetooth = BtState.OFF, error = message) }
    }

    @SuppressLint("MissingPermission")
    private fun begin() {
        settings = SettingsStore.get(this).settings.value
        if (!hasBluetoothPermission(this)) {
            fail("Bluetooth permission is missing")
            stopSelf()
            return
        }
        val a = getSystemService(BluetoothManager::class.java)?.adapter
        if (a == null) {
            fail("This phone has no Bluetooth")
            stopSelf()
            return
        }
        adapter = a

        val self = arrayOfNulls<BridgeEngine>(1) // the engine a status comes from
        val e = BridgeEngine(settings.toBridgeConfig(), object : BridgeEngine.Listener {
            override fun onLog(level: Int, line: String) = BridgeState.log(level, line)
            override fun onStatus(status: BridgeEngine.Status) = onEngineStatus(self[0], status)
        })
        self[0] = e
        engine = e
        BridgeState.update {
            it.copy(
                running = true, error = null, activeSettings = settings, autoServer = settings.autoServer,
                phoneName = runCatching { a.name }.getOrNull(), devices = emptyList(),
                bluetooth = if (a.isEnabled) BtState.STARTING else BtState.OFF,
            )
        }
        e.start()
        acquireLocks()
        watchNetwork()
        registerReceivers()
        if (a.isEnabled) {
            startListener()
        } else {
            BridgeState.log(BridgeEngine.LOG_WARN, "Bluetooth is off")
        }
        if (settings.pin.isNotEmpty()) {
            BridgeState.log(BridgeEngine.LOG_INFO, "pairing requests are answered with PIN ${settings.pin}")
        }
    }

    private fun onEngineStatus(source: BridgeEngine?, s: BridgeEngine.Status) {
        if (source == null || source !== engine) {
            return
        }
        BridgeState.update {
            it.copy(networkUp = s.networkUp, autoServer = s.autoServer, serverAddress = s.serverAddress, devices = s.devices)
        }
        main.post { updateNotification(s) }
    }

    private fun shutdown() {
        val e = engine ?: return
        engine = null
        main.removeCallbacks(retryListen)
        listener?.stop()
        listener = null
        e.stop()
        if (receiverRegistered) {
            unregisterReceiver(receiver)
            receiverRegistered = false
        }
        networkCallback?.let { cb ->
            runCatching { getSystemService(ConnectivityManager::class.java).unregisterNetworkCallback(cb) }
        }
        networkCallback = null
        wakeLock?.let { if (it.isHeld) it.release() }
        wifiLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        wifiLock = null
        lastNotification = null
        BridgeState.update {
            it.copy(running = false, bluetooth = BtState.OFF, devices = emptyList(), activeSettings = null)
        }
    }

    /* ------------------------------------------------------------------ */
    /* Bluetooth                                                          */
    /* ------------------------------------------------------------------ */

    private fun startListener() {
        val a = adapter ?: return
        listener?.stop()
        BridgeState.update { it.copy(bluetooth = BtState.STARTING) }
        val l = BluetoothListener(
            a, settings.serviceName.ifBlank { "GameComms Bridge" },
            onConnection = { c -> engine?.deviceConnected(c) ?: c.close() },
            onState = { ok, err -> main.post { onListenerState(ok, err) } },
        )
        listener = l
        l.start()
    }

    private fun onListenerState(ok: Boolean, err: String?) {
        if (engine == null) return
        if (ok) {
            BridgeState.update { it.copy(bluetooth = BtState.LISTENING) }
            val name = BridgeState.ui.value.phoneName
            BridgeState.log(BridgeEngine.LOG_INFO, "Bluetooth Serial Port service '${settings.serviceName}' ready" +
                (if (name != null) ", visible as '$name'" else ""))
            BridgeState.log(BridgeEngine.LOG_INFO, "ready: on the N-Gage, start a multiplayer game and select this phone")
        } else if (adapter?.isEnabled == true) {
            BridgeState.update { it.copy(bluetooth = BtState.ERROR) }
            BridgeState.log(BridgeEngine.LOG_WARN, "Bluetooth service stopped (${err ?: "unknown error"}), retrying")
            main.removeCallbacks(retryListen)
            main.postDelayed(retryListen, 3000)
        }
    }

    private val receiver = object : BroadcastReceiver() {
        @SuppressLint("MissingPermission")
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                        BluetoothAdapter.STATE_ON -> {
                            BridgeState.log(BridgeEngine.LOG_INFO, "Bluetooth switched on")
                            BridgeState.update { it.copy(phoneName = runCatching { adapter?.name }.getOrNull()) }
                            startListener()
                        }
                        BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                            if (listener != null) BridgeState.log(BridgeEngine.LOG_WARN, "Bluetooth switched off")
                            main.removeCallbacks(retryListen)
                            listener?.stop()
                            listener = null
                            BridgeState.update { it.copy(bluetooth = BtState.OFF) }
                        }
                    }
                }
                BluetoothAdapter.ACTION_LOCAL_NAME_CHANGED -> {
                    BridgeState.update { it.copy(phoneName = intent.getStringExtra(BluetoothAdapter.EXTRA_LOCAL_NAME)) }
                }
                BluetoothDevice.ACTION_PAIRING_REQUEST -> answerPairing(intent)
            }
        }

        @SuppressLint("MissingPermission")
        private fun answerPairing(intent: Intent) {
            val pin = settings.pin
            val device = IntentCompat.getParcelableExtra(intent, BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            val variant = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_VARIANT, -1)
            if (pin.isEmpty() || device == null || variant != BluetoothDevice.PAIRING_VARIANT_PIN) {
                return // numeric comparison etc.: the system asks the user
            }
            val ok = try {
                device.setPin(pin.toByteArray(Charsets.US_ASCII))
            } catch (e: SecurityException) {
                false
            }
            val who = runCatching { device.name }.getOrNull() ?: device.address
            if (ok) {
                BridgeState.log(BridgeEngine.LOG_INFO, "pairing request from $who answered with the PIN")
                if (isOrderedBroadcast) abortBroadcast() // no system dialog
            } else {
                BridgeState.log(BridgeEngine.LOG_WARN, "pairing request from $who: enter PIN $pin on the phone")
            }
        }
    }

    private fun registerReceivers() {
        val f = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(BluetoothAdapter.ACTION_LOCAL_NAME_CHANGED)
            addAction(BluetoothDevice.ACTION_PAIRING_REQUEST)
            priority = IntentFilter.SYSTEM_HIGH_PRIORITY - 1
        }
        ContextCompat.registerReceiver(this, receiver, f, ContextCompat.RECEIVER_EXPORTED)
        receiverRegistered = true
    }

    /* ------------------------------------------------------------------ */
    /* network, locks, notification                                       */
    /* ------------------------------------------------------------------ */

    private fun watchNetwork() {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                engine?.setNetwork(true)
            }

            override fun onLost(network: Network) {
                engine?.setNetwork(cm.activeNetwork != null && cm.activeNetwork != network)
            }
        }
        engine?.setNetwork(cm.activeNetwork != null)
        runCatching { cm.registerDefaultNetworkCallback(cb) }.onSuccess { networkCallback = cb }
    }

    private fun acquireLocks() {
        // Without these the phone delays packets in power save, which the
        // N-Gage's link sees as an unreliable connection.
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhantomLink:bridge").apply {
                setReferenceCounted(false)
                acquire()
            }
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = applicationContext.getSystemService(WifiManager::class.java)
            ?.createWifiLock(mode, "PhantomLink:bridge")?.apply {
                setReferenceCounted(false)
                acquire()
            }
    }

    private fun createChannel() {
        val ch = NotificationChannel(CHANNEL_ID, getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW)
        ch.description = getString(R.string.notification_channel_desc)
        ch.setShowBadge(false)
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun buildNotification(text: String) = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(R.drawable.ic_stat_ghost)
        .setContentTitle(getString(R.string.notification_title))
        .setContentText(text)
        .setColor(ContextCompat.getColor(this, R.color.brand_purple))
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .setSilent(true)
        .setCategory(NotificationCompat.CATEGORY_SERVICE)
        .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
        .setContentIntent(
            PendingIntent.getActivity(
                this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            ),
        )
        .addAction(
            0, getString(R.string.stop),
            PendingIntent.getService(
                this, 1, Intent(this, BridgeService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            ),
        )
        .build()

    @SuppressLint("MissingPermission") // checked below
    private fun updateNotification(s: BridgeEngine.Status) {
        if (engine == null) return
        val n = s.presentCount()
        val online = s.devices.count { it.present && it.serverState == NativeBridge.SRV_UP }
        val text = when {
            n == 0 -> getString(R.string.waiting_short)
            else -> resources.getQuantityString(R.plurals.ngages_connected, n, n) +
                if (online == n) " · " + getString(R.string.online) else ""
        }
        if (text == lastNotification) return
        lastNotification = text
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) return
        NotificationManagerCompat.from(this).notify(NOTIFICATION_ID, buildNotification(text))
    }

    companion object {
        private const val CHANNEL_ID = "bridge"
        private const val NOTIFICATION_ID = 1
        const val ACTION_START = "de.ngagesdk.phantomlink.START"
        const val ACTION_STOP = "de.ngagesdk.phantomlink.STOP"
        const val ACTION_RESTART = "de.ngagesdk.phantomlink.RESTART"

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, BridgeService::class.java).setAction(ACTION_START))
        }

        /** only while the service runs (it is already in the foreground) */
        fun restart(context: Context) {
            context.startService(Intent(context, BridgeService::class.java).setAction(ACTION_RESTART))
        }

        fun stop(context: Context) {
            context.startService(Intent(context, BridgeService::class.java).setAction(ACTION_STOP))
        }

        fun hasBluetoothPermission(context: Context): Boolean =
            Build.VERSION.SDK_INT < 31 ||
                ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }
}
