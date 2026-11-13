/*
 * Bridge log, newest at the bottom.
 *
 * SPDX-License-Identifier: MIT
 */
package de.ngagesdk.phantomlink.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import de.ngagesdk.phantomlink.LogLine

@Composable
fun LogScreen(lines: List<LogLine>, onShare: () -> Unit, onClear: () -> Unit, contentPadding: PaddingValues) {
    val sc = LocalStatusColors.current
    val state = rememberLazyListState()
    val atEnd by remember {
        derivedStateOf {
            val last = state.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: -1
            last >= state.layoutInfo.totalItemsCount - 3
        }
    }
    LaunchedEffect(lines.size) {
        if (lines.isNotEmpty() && atEnd) state.scrollToItem(lines.size - 1)
    }
    Column(Modifier.fillMaxSize().padding(contentPadding)) {
        LogoHeader()
        Row(Modifier.fillMaxWidth().padding(start = 24.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Log", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            IconButton(onClick = onShare, enabled = lines.isNotEmpty()) { Icon(Icons.Filled.Share, "Share log") }
            IconButton(onClick = onClear, enabled = lines.isNotEmpty()) { Icon(Icons.Filled.Delete, "Clear log") }
        }
        if (lines.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Nothing logged yet", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            return@Column
        }
        LazyColumn(state = state, contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 16.dp)) {
            itemsIndexed(lines) { _, l ->
                Row(Modifier.padding(vertical = 3.dp)) {
                    Text(l.time, fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = MaterialTheme.colorScheme.outline)
                    Spacer(Modifier.width(10.dp))
                    Text(
                        l.text, fontFamily = FontFamily.Monospace, fontSize = 12.sp, lineHeight = 16.sp,
                        color = when (l.level) {
                            0 -> sc.bad
                            1 -> sc.busy
                            3 -> MaterialTheme.colorScheme.onSurfaceVariant
                            else -> MaterialTheme.colorScheme.onSurface
                        },
                    )
                }
            }
        }
    }
}
