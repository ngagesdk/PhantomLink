/*
 * Settings: server, Bluetooth, appearance. Bridge settings take effect
 * when the bridge is (re)started.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import de.ngagesdk.phantomlink.AppSettings
import de.ngagesdk.phantomlink.ThemeMode
import de.ngagesdk.phantomlink.BridgeConfig
import de.ngagesdk.phantomlink.BridgeEngine

@Composable
fun SettingsScreen(settings: AppSettings, onChange: ((AppSettings) -> AppSettings) -> Unit, contentPadding: PaddingValues) {
    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(contentPadding).padding(horizontal = 20.dp)
            .padding(bottom = 24.dp),
    ) {
        LogoHeader()

        SectionTitle("Server")
        PlainCard {
            SwitchRow("Find server automatically", "Looks for a PhantomLink server on the local network",
                settings.autoServer) { v -> onChange { it.copy(autoServer = v) } }
            Divider()
            Column(Modifier.padding(horizontal = 18.dp, vertical = 10.dp)) {
                if (!settings.autoServer) {
                    Field("Server address", settings.serverHost, placeholder = "example.com or 192.168.1.10",
                        keyboardType = KeyboardType.Uri) { v ->
                        onChange { it.copy(serverHost = v.trim()) }
                    }
                    Spacer(Modifier.height(10.dp))
                }
                PortField(settings.port) { p -> onChange { it.copy(port = p) } }
                Spacer(Modifier.height(10.dp))
                OutlinedTextField(
                    value = settings.accessKey, onValueChange = { v -> onChange { it.copy(accessKey = v.take(32)) } },
                    label = { Text("Access key") }, supportingText = { Text("Only if the server requires one") },
                    singleLine = true, visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }

        SectionTitle("Bluetooth")
        PlainCard {
            Column(Modifier.padding(horizontal = 18.dp, vertical = 10.dp)) {
                OutlinedTextField(
                    value = settings.pin,
                    onValueChange = { v -> onChange { it.copy(pin = v.filter(Char::isDigit).take(16)) } },
                    label = { Text("Pairing PIN") },
                    supportingText = { Text("Answered automatically when an N-Gage pairs. Empty: confirm on the phone.") },
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(6.dp))
                Field("Service name", settings.serviceName) { v -> onChange { it.copy(serviceName = v.take(32)) } }
                Spacer(Modifier.height(14.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("N-Gages at the same time", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                    Text("${settings.maxDevices}", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.primary)
                }
                Slider(
                    value = settings.maxDevices.toFloat(),
                    onValueChange = { v -> onChange { it.copy(maxDevices = v.toInt()) } },
                    valueRange = 1f..BridgeConfig.MAX_DEVICES.toFloat(), steps = BridgeConfig.MAX_DEVICES - 2,
                )
            }
        }

        SectionTitle("Appearance")
        PlainCard {
            Column(Modifier.padding(horizontal = 18.dp, vertical = 12.dp)) {
                Choice(listOf("Light", "Dark", "System"), settings.theme.ordinal) { i ->
                    onChange { it.copy(theme = ThemeMode.entries[i]) }
                }
            }
        }

        SectionTitle("Log detail")
        PlainCard {
            Column(Modifier.padding(horizontal = 18.dp, vertical = 12.dp)) {
                Choice(listOf("Errors", "Warnings", "Info", "Debug"), settings.logLevel) { i ->
                    onChange { it.copy(logLevel = i) }
                }
            }
        }

        Spacer(Modifier.height(20.dp))
        Text(
            "Bridge engine ${BridgeEngine.version()} · same engine as the ESP32 and PC bridges",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
        )
    }
}

@Composable
private fun Divider() = HorizontalDivider(Modifier.padding(horizontal = 18.dp), color = MaterialTheme.colorScheme.outlineVariant)

@Composable
private fun SwitchRow(title: String, subtitle: String, checked: Boolean, onChecked: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 18.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onChecked)
    }
}

@Composable
private fun Field(
    label: String,
    value: String,
    placeholder: String? = null,
    keyboardType: KeyboardType = KeyboardType.Text,
    onValue: (String) -> Unit,
) {
    OutlinedTextField(
        value = value, onValueChange = onValue, label = { Text(label) }, singleLine = true,
        placeholder = if (placeholder != null) ({ Text(placeholder) }) else null,
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun PortField(port: Int, onPort: (Int) -> Unit) {
    var text by rememberSaveable { mutableStateOf(port.toString()) }
    val valid = text.toIntOrNull()?.let { it in 1..65535 } == true
    OutlinedTextField(
        value = text,
        onValueChange = { v ->
            text = v.filter(Char::isDigit).take(5)
            text.toIntOrNull()?.takeIf { it in 1..65535 }?.let(onPort)
        },
        label = { Text("Port") }, singleLine = true, isError = !valid,
        supportingText = if (!valid) ({ Text("1 to 65535") }) else null,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = Modifier.fillMaxWidth(),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Choice(options: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
        options.forEachIndexed { i, label ->
            SegmentedButton(
                selected = i == selected, onClick = { onSelect(i) },
                shape = SegmentedButtonDefaults.itemShape(index = i, count = options.size),
            ) { Text(label, maxLines = 1) }
        }
    }
}
