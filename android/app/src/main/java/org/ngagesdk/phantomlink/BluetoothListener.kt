/*
 * Publishes the Serial Port (SPP) service and accepts the N-Gages' RFCOMM
 * connections, like pcb_bt_linux.c / the ESP32's SPP server. The DLL finds
 * the service through SDP (UUID 0x1101), so the RFCOMM channel does not
 * matter.
 *
 * SPDX-License-Identifier: MIT
 */
package org.ngagesdk.phantomlink

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import org.ngagesdk.phantomlink.bridge.BridgeEngine
import org.ngagesdk.phantomlink.bridge.DeviceConnection
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

@SuppressLint("MissingPermission") // checked by BridgeService before start()
class BluetoothListener(
    private val adapter: BluetoothAdapter,
    private val serviceName: String,
    private val onConnection: (DeviceConnection) -> Unit,
    /** listening (true) or failed/stopped (false, with the error if any) */
    private val onState: (Boolean, String?) -> Unit,
) {
    @Volatile private var server: BluetoothServerSocket? = null
    @Volatile private var closed = false

    fun start() {
        closed = false
        Thread({ run() }, "gc-bt-accept").apply { isDaemon = true }.start()
    }

    fun stop() {
        closed = true
        try {
            server?.close()
        } catch (_: IOException) {
        }
        server = null
    }

    private fun run() {
        val s = try {
            // Insecure: the N-Gage (Bluetooth 1.1) pairs with a PIN if it wants
            // to, but the link must not require Secure Simple Pairing.
            adapter.listenUsingInsecureRfcommWithServiceRecord(serviceName, SPP_UUID)
        } catch (e: Exception) { // IOException, SecurityException
            if (!closed) onState(false, e.message ?: e.toString())
            return
        }
        server = s
        if (closed) {
            stop()
            return
        }
        onState(true, null)
        while (!closed) {
            val sock = try {
                s.accept()
            } catch (e: IOException) {
                if (!closed) onState(false, e.message ?: "accept failed")
                break
            }
            onConnection(BtConnection(sock))
        }
    }

    private class BtConnection(private val sock: BluetoothSocket) : DeviceConnection {
        private val addr: ByteArray = BridgeEngine.parseAddress(sock.remoteDevice.address) ?: ByteArray(6)
        private val devName: String? = try {
            sock.remoteDevice.name
        } catch (_: SecurityException) {
            null
        }

        override fun address(): ByteArray = addr
        override fun name(): String? = devName
        override fun input(): InputStream = sock.inputStream
        override fun output(): OutputStream = sock.outputStream
        override fun close() {
            try {
                sock.close()
            } catch (_: IOException) {
            }
        }
    }

    companion object {
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805f9b34fb")
    }
}
