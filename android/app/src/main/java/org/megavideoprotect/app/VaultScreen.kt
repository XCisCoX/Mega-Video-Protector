package org.megavideoprotect.app

import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** Gallery: toolbar (view mode / tag filter / search / Import / Settings /
 *  Lock), Explorer-style details grid, icon grid, list, and a status bar. */
@Composable
fun VaultScreen(
    vaultLocation: String,
    onLock: () -> Unit,
    onPlay: (Long, String) -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var videos by remember { mutableStateOf<List<VideoEntry>>(emptyList()) }
    var tags by remember { mutableStateOf<List<TagEntry>>(emptyList()) }
    var viewMode by remember { mutableStateOf(0) } // 0 Details, 1 Icons, 2 List
    var filterTagId by remember { mutableStateOf(0L) }
    var search by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var showSettings by remember { mutableStateOf(false) }
    // Batch import progress: fraction of the bytes of the whole selection.
    var importProgress by remember { mutableStateOf<Float?>(null) }
    var importLabel by remember { mutableStateOf("") }

    fun refresh() {
        scope.launch {
            val (v, t) = withContext(Dispatchers.Default) {
                val list = runCatching { CoreBridge.nativeListVideos() }.getOrNull()
                val tagList = runCatching { CoreBridge.nativeListTags() }.getOrNull()
                Json.videos(list ?: "[]") to Json.tags(tagList ?: "[]")
            }
            videos = v
            tags = t
        }
    }

    LaunchedEffect(Unit) { refresh() }

    // Import: multi-select, streamed into the vault one file at a time with a
    // byte-level progress bar for the whole batch (no per-file jumps).
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris: List<Uri> ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        error = null
        importProgress = 0f
        importLabel = ""
        scope.launch {
            val results = withContext(Dispatchers.IO) {
                importBatch(context, uris) { done, total, label ->
                    importProgress = if (total > 0L) (done.toFloat() / total.toFloat()) else 0f
                    importLabel = label
                }
            }
            importProgress = null
            importLabel = ""
            val failures = results.filter { !it.ok }
            if (failures.isNotEmpty()) {
                error = failures.joinToString("\n") { it.error.ifBlank { it.detail } }
            }
            refresh()
        }
    }

    Column(Modifier.fillMaxSize().background(Mvp.window)) {
        // Toolbar (Qt: viewModeCombo, tagFilterCombo, searchEdit, Import video…,
        // Import folder…, Settings…, Lock).
        Row(
            Modifier
                .fillMaxWidth()
                .background(Mvp.headerBg)
                .padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            MvpCombo(
                selected = listOf("Details", "Icons", "List")[viewMode],
                items = listOf("Details", "Icons", "List"),
                onSelect = { s -> viewMode = listOf("Details", "Icons", "List").indexOf(s) },
            )
            MvpCombo(
                selected = if (filterTagId == 0L) "All tags" else (tags.firstOrNull { it.id == filterTagId }?.name ?: "All tags"),
                items = listOf("All tags") + tags.map { it.name },
                onSelect = { s ->
                    filterTagId = if (s == "All tags") 0L else (tags.firstOrNull { it.name == s }?.id ?: 0L)
                },
            )
            MvpInput(
                value = search,
                onValueChange = { search = it },
                placeholder = "Search",
                modifier = Modifier.weight(1f),
            )
            MvpButton("Import", onClick = { importLauncher.launch(arrayOf("video/*", "application/octet-stream")) }, primary = true)
            MvpButton("Settings…", onClick = { showSettings = true })
            MvpButton("Lock", onClick = {
                scope.launch { withContext(Dispatchers.Default) { CoreBridge.nativeLock() } }
                onLock()
            })
        }

        // Import progress (byte-level, whole batch) + error banner.
        importProgress?.let { fraction ->
            Column(Modifier.fillMaxWidth().background(Mvp.headerBg).padding(horizontal = 12.dp, vertical = 6.dp)) {
                LinearProgressIndicator(
                    progress = { fraction.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                    color = Mvp.primary,
                    trackColor = Mvp.sliderGroove,
                )
                Text(
                    "${(fraction * 100).toInt()}%  $importLabel",
                    color = Mvp.description,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        MvpError(error, Modifier.padding(horizontal = 12.dp))

        if (showSettings) {
            SettingsDialog(
                currentTags = tags,
                onDismiss = { showSettings = false },
                onChanged = { refresh() },
            )
        }

        val filtered = videos.filter { v ->
            (filterTagId == 0L || v.tags.any { t -> tags.firstOrNull { it.id == filterTagId }?.name?.equals(t, true) == true }) &&
                (search.isBlank() || v.name.contains(search, ignoreCase = true))
        }

        val busy = importProgress != null

        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (viewMode) {
                0 -> DetailsView(filtered, busy, onPlay)
                1 -> IconGridView(filtered, busy, onPlay)
                else -> ListView(filtered, busy, onPlay)
            }
        }

        // Status bar (Qt: location + count, #666666).
        Row(
            Modifier
                .fillMaxWidth()
                .background(Mvp.window)
                .padding(horizontal = 12.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(vaultLocation, color = Mvp.statusText, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Text("${filtered.size} items", color = Mvp.statusText, fontSize = 12.sp)
        }
    }
}

private fun queryName(resolver: android.content.ContentResolver, uri: Uri): String? {
    return runCatching {
        resolver.query(uri, null, null, null, null)?.use { c ->
            val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && c.moveToFirst()) c.getString(idx) else null
        }
    }.getOrNull()
}

/**
 * Copies each picked document into the app cache (streamed in 64 KiB blocks so
 * the progress bar tracks real bytes) and imports it into the vault. Files are
 * processed one at a time — the vault allows a single writer — while the bar
 * spans the whole selection, so it never jumps per file.
 */
private suspend fun importBatch(
    context: android.content.Context,
    uris: List<Uri>,
    onProgress: (done: Long, total: Long, label: String) -> Unit,
): List<CoreBridge.Result> {
    // Sizes up front (when the provider reports them) so the aggregate bar is
    // proportional across the batch.
    val declared = uris.map { uri ->
        runCatching {
            context.contentResolver.openAssetFileDescriptor(uri, "r")?.use { it.length }
        }.getOrNull() ?: -1L
    }
    val knownTotal = declared.filter { it > 0L }.sum()
    var done = 0L
    val results = ArrayList<CoreBridge.Result>(uris.size)

    uris.forEachIndexed { index, uri ->
        val name = queryName(context.contentResolver, uri) ?: "import_$index.mp4"
        val tmp = File(context.cacheDir, "import_$index-$name")
        val result = try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                tmp.outputStream().use { output ->
                    val block = ByteArray(1 shl 16)
                    while (true) {
                        val read = input.read(block)
                        if (read < 0) break
                        output.write(block, 0, read)
                        done += read.toLong()
                        onProgress(done, maxOf(knownTotal, done), name)
                    }
                }
            } ?: run {
                results.add(CoreBridge.Result(false, "Could not open $name", ""))
                return@forEachIndexed
            }
            CoreBridge.Result.parse(CoreBridge.nativeImportFile(tmp.absolutePath))
        } catch (e: Exception) {
            CoreBridge.Result(false, "Import failed: $name", e.message ?: "")
        } finally {
            tmp.delete()
        }
        results.add(result)
    }
    return results
}

@Composable
private fun Thumb(id: Long, size: Int) {
    var bmp by remember { mutableStateOf<android.graphics.Bitmap?>(null) }
    LaunchedEffect(id) {
        bmp = withContext(Dispatchers.Default) {
            CoreBridge.nativeThumbnail(id, 320)?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
    }
    val image = bmp
    if (image == null) {
        Box(
            Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)).background(Mvp.card),
            contentAlignment = Alignment.Center,
        ) { CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp) }
    } else {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)),
            contentScale = ContentScale.Crop,
        )
    }
}

@Composable
private fun DetailsView(videos: List<VideoEntry>, busy: Boolean, onPlay: (Long, String) -> Unit) {
    LazyColumn(Modifier.fillMaxSize().background(Mvp.inputBg)) {
        item {
            Row(
                Modifier.fillMaxWidth().background(Mvp.headerBg).padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                listOf("Name", "Size", "Duration", "Resolution", "Codec", "Tags", "Imported").forEachIndexed { i, h ->
                    Text(
                        h,
                        color = Mvp.headerText,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.weight(if (i == 0) 2.2f else 1f),
                    )
                }
            }
        }
        items(videos, key = { it.id }) { v ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(if (videos.indexOf(v) % 2 == 0) Mvp.inputBg else Mvp.alternateRow)
                    .clickable { onPlay(v.id, v.name) }
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Thumb(v.id, 44)
                Text(v.name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(2.2f))
                Text(v.sizeText, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(v.durationText, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(v.resolution, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(v.codec, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(v.tags.joinToString(", "), color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Text(v.importedText, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun IconGridView(videos: List<VideoEntry>, busy: Boolean, onPlay: (Long, String) -> Unit) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 172.dp),
        modifier = Modifier.fillMaxSize().background(Mvp.inputBg),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(videos, key = { it.id }) { v ->
            Column(
                Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .background(Mvp.card)
                    .clickable { onPlay(v.id, v.name) }
                    .padding(8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Thumb(v.id, 128)
                Text(v.name, color = Mvp.text, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 6.dp))
            }
        }
    }
}

@Composable
private fun ListView(videos: List<VideoEntry>, busy: Boolean, onPlay: (Long, String) -> Unit) {
    LazyColumn(Modifier.fillMaxSize().background(Mvp.inputBg)) {
        items(videos, key = { it.id }) { v ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(if (videos.indexOf(v) % 2 == 0) Mvp.inputBg else Mvp.alternateRow)
                    .clickable { onPlay(v.id, v.name) }
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Thumb(v.id, 36)
                Column(Modifier.padding(start = 10.dp)) {
                    Text(v.name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${v.sizeText} · ${v.durationText} · ${v.tags.joinToString(", ")}", color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}
