/*
 * Main screen: start/stop, Bluetooth and server state, connected N-Gages.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import de.ngagesdk.phantomlink.AppSettings
import de.ngagesdk.phantomlink.BridgeUi
import de.ngagesdk.phantomlink.BtState
import de.ngagesdk.phantomlink.BridgeEngine
import de.ngagesdk.phantomlink.NativeBridge

@Composable
fun HomeScreen(
    ui: BridgeUi,
    settings: AppSettings,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onRestart: () -> Unit,
    onMakeVisible: () -> Unit,
    contentPadding: PaddingValues,
) {
    val sc = LocalStatusColors.current
    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(contentPadding).padding(horizontal = 20.dp)
            .padding(bottom = 24.dp),
    ) {
        LogoHeader()
        Spacer(Modifier.height(12.dp))
        PowerCard(ui, onStart, onStop)

        val active = ui.activeSettings
        if (ui.running && active != null && active.bridgeDiffers(settings)) {
            Spacer(Modifier.height(12.dp))
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer),
                shape = MaterialTheme.shapes.large, modifier = Modifier.fillMaxWidth(),
            ) {
                Row(Modifier.padding(start = 18.dp, end = 8.dp, top = 6.dp, bottom = 6.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("Settings changed", style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSecondaryContainer, modifier = Modifier.weight(1f))
                    TextButton(onClick = onRestart) { Text("Restart bridge") }
                }
            }
        }

        SectionTitle("Status")
        PlainCard {
            val (btText, btColor) = when {
                !ui.running -> "Not started" to sc.off
                ui.bluetooth == BtState.OFF -> "Bluetooth is off" to sc.bad
                ui.bluetooth == BtState.ERROR -> "Problem, retrying" to sc.bad
                ui.bluetooth == BtState.STARTING -> "Starting" to sc.busy
                else -> (ui.phoneName?.let { "Visible as “$it”" } ?: "Ready") to sc.ok
            }
            StatusRow("Bluetooth", btText, btColor, action = if (ui.running && ui.bluetooth == BtState.LISTENING) {
                { TextButton(onClick = onMakeVisible) { Text("Make visible") } }
            } else null)
            HorizontalDivider(Modifier.padding(horizontal = 18.dp), color = MaterialTheme.colorScheme.outlineVariant)
            val online = ui.devices.any { it.serverState == NativeBridge.SRV_UP }
            val connecting = ui.devices.any { it.serverState == NativeBridge.SRV_CONNECTING }
            val auto = if (ui.running) ui.autoServer else settings.autoServer
            val (srvText, srvColor) = when {
                !ui.running -> (if (auto) "Find automatically" else "${settings.serverHost}:${settings.port}") to sc.off
                !ui.networkUp -> "No network" to sc.bad
                online -> "Connected to ${ui.serverAddress ?: "server"}" to sc.ok
                connecting -> "Connecting to ${ui.serverAddress ?: "server"}" to sc.busy
                auto && ui.serverAddress == null -> "Searching the local network" to sc.busy
                auto -> "Found ${ui.serverAddress}" to sc.ok
                else -> "${ui.serverAddress}" to sc.ok
            }
            StatusRow("Server", srvText, srvColor)
        }

        SectionTitle("N-Gages", trailing = if (ui.running) "${ui.devices.count { it.present }} / ${settings.maxDevices}" else null)
        if (ui.devices.isEmpty()) {
            HowTo(ui, settings)
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                ui.devices.forEach { DeviceCard(it) }
            }
        }
    }
}

@Composable
private fun PowerCard(ui: BridgeUi, onStart: () -> Unit, onStop: () -> Unit) {
    val running = ui.running
    val container by animateColorAsState(
        if (running) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerLow,
        label = "power",
    )
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = container),
        border = if (running) null else androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(20.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    if (running) "Bridge is running" else "Bridge is off",
                    style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    ui.error ?: if (running) "N-Gages can connect to this phone." else "Start to let N-Gages play online through this phone.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (ui.error != null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.width(16.dp))
            FilledIconButton(
                onClick = if (running) onStop else onStart,
                modifier = Modifier.size(68.dp),
                shape = CircleShape,
                colors = if (running) {
                    IconButtonDefaults.filledIconButtonColors(
                        containerColor = MaterialTheme.colorScheme.surfaceContainerLowest,
                        contentColor = MaterialTheme.colorScheme.primary,
                    )
                } else IconButtonDefaults.filledIconButtonColors(),
            ) {
                Icon(
                    if (running) Icons.Filled.Close else Icons.Filled.PlayArrow,
                    contentDescription = if (running) "Stop bridge" else "Start bridge",
                    modifier = Modifier.size(32.dp),
                )
            }
        }
    }
}

@Composable
private fun HowTo(ui: BridgeUi, settings: AppSettings) {
    PlainCard {
        Column(Modifier.padding(horizontal = 18.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            val name = ui.phoneName?.let { "“$it”" } ?: "this phone"
            Step(1, if (ui.running) "Bridge started." else "Tap the start button above.")
            Step(2, "On the N-Gage, start a multiplayer game and select $name in the Bluetooth list.")
            Step(3, if (settings.pin.isNotEmpty()) "If the N-Gage asks for a PIN, enter ${settings.pin}."
                    else "If the N-Gage asks for a PIN, choose one and confirm it on the phone.")
        }
    }
}

@Composable
private fun Step(n: Int, text: String) {
    Row(verticalAlignment = Alignment.Top) {
        Text("$n", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Bold, modifier = Modifier.width(22.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun DeviceCard(d: BridgeEngine.DeviceInfo) {
    val sc = LocalStatusColors.current
    PlainCard {
        Column(Modifier.padding(horizontal = 18.dp, vertical = 10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(d.name?.takeIf { it.isNotBlank() } ?: "N-Gage", style = MaterialTheme.typography.titleMedium)
                    Text(d.address, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Column(horizontalAlignment = Alignment.End) {
                    Text("↑ ${d.toServer}", style = MaterialTheme.typography.labelMedium, fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text("↓ ${d.fromServer}", style = MaterialTheme.typography.labelMedium, fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                when {
                    !d.present -> Pill("Away", sc.busy)
                    d.linkUp -> Pill("Connected", sc.ok)
                    else -> Pill("Connecting", sc.busy)
                }
                when (d.serverState) {
                    NativeBridge.SRV_UP -> Pill(if (d.registered) "Online" else "Server", sc.ok)
                    NativeBridge.SRV_CONNECTING -> Pill("Server connecting", sc.busy)
                    else -> Pill("Server offline", sc.bad)
                }
            }
        }
    }
}
