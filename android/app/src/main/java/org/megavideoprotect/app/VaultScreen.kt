package org.megavideoprotect.app

import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
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
import androidx.compose.ui.window.Dialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/**
 * Rows, thumbnails *and* the toolbar's own state (view mode, tag filter, search
 * text) survive leaving the gallery — the screen is destroyed while the player
 * is open, so anything remembered inside it was lost on the way back and Icons
 * mode snapped to Details. Cleared on lock, so a vault is never shown from a
 * stale cache after being re-opened.
 */
object GalleryCache {
    var vaultLocation: String? = null
    var videos: List<VideoEntry> = emptyList()
    var tags: List<TagEntry> = emptyList()
    var loaded = false
    // Toolbar state — deliberately *not* reset by clear(): which view you were
    // using is a preference, not vault data.
    var viewMode = 0
    var filterTagId = 0L
    var search = ""
    // Scroll position per view mode. The gallery screen is torn down while the
    // player is open, so a scroll state remembered inside it reset to the top:
    // playing something from the middle of the list dropped you back to item 1.
    val detailsScroll = androidx.compose.foundation.lazy.LazyListState()
    val listScroll = androidx.compose.foundation.lazy.LazyListState()
    val gridScroll = androidx.compose.foundation.lazy.grid.LazyGridState()
    private val thumbs = HashMap<String, android.graphics.Bitmap>()

    fun thumb(id: Long, size: Int): android.graphics.Bitmap? = thumbs["$id:$size"]

    fun putThumb(id: Long, size: Int, bitmap: android.graphics.Bitmap) {
        if (thumbs.size > 64) thumbs.clear()
        thumbs["$id:$size"] = bitmap
    }

    fun clear() {
        vaultLocation = null
        videos = emptyList()
        tags = emptyList()
        loaded = false
        thumbs.clear()
    }
}

/** Per-row container metadata. Probing opens the package and decrypts container
 *  bytes, so it is done lazily for visible rows only — the desktop fills these
 *  columns the same way. */
data class MediaInfo(val durationMs: Long, val width: Int, val height: Int, val codec: String) {
    val resolution: String get() = if (width > 0 && height > 0) "${width}×${height}" else "…"
    val durationText: String
        get() {
            if (durationMs <= 0) return "…"
            val sec = durationMs / 1000
            return "%d:%02d".format(sec / 60, sec % 60)
        }
    val codecText: String get() = codec.ifBlank { "…" }
}

@Composable
private fun rememberMediaInfo(id: Long): MediaInfo? {
    var info by remember(id) { mutableStateOf<MediaInfo?>(null) }
    LaunchedEffect(id) {
        info = withContext(Dispatchers.Default) {
            runCatching {
                val o = JSONObject(CoreBridge.nativeMediaInfo(id))
                if (o.has("width") || o.has("durationMs")) {
                    MediaInfo(
                        durationMs = o.optLong("durationMs", 0),
                        width = o.optInt("width", 0),
                        height = o.optInt("height", 0),
                        codec = o.optString("codec", ""),
                    )
                } else {
                    null
                }
            }.getOrNull()
        }
    }
    return info
}

/** Gallery: two-row toolbar (view mode / tag filter / search above Import /
 *  Files / Settings / Lock), Explorer-style details grid, icon grid, list, a
 *  per-item action sheet (remove / export) and a status bar.
 *
 *  Listing is database-only; duration / resolution / codec are probed by the
 *  visible rows on demand. Probing every row up front made a large vault take
 *  seconds to appear. */
@Composable
fun VaultScreen(
    vaultLocation: String,
    onLock: () -> Unit,
    onPlay: (Long, String) -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var videos by remember { mutableStateOf(GalleryCache.videos) }
    var tags by remember { mutableStateOf(GalleryCache.tags) }
    // Initialised from the cache so the chosen view / filter / query come back
    // exactly as they were when the player is dismissed.
    var viewMode by remember { mutableStateOf(GalleryCache.viewMode) }
    var filterTagId by remember { mutableStateOf(GalleryCache.filterTagId) }
    var search by remember { mutableStateOf(GalleryCache.search) }
    var error by remember { mutableStateOf<String?>(null) }
    var showSettings by remember { mutableStateOf(false) }
    // Batch import progress: fraction of the bytes of the whole selection.
    var importProgress by remember { mutableStateOf<Float?>(null) }
    var importLabel by remember { mutableStateOf("") }
    // Per-item actions (opened with the ⋮ at the end of a row).
    var actionTarget by remember { mutableStateOf<VideoEntry?>(null) }
    var actionBusy by remember { mutableStateOf(false) }
    var actionMessage by remember { mutableStateOf<String?>(null) }

    fun refresh() {
        scope.launch {
            val (v, t) = withContext(Dispatchers.Default) {
                val list = runCatching { CoreBridge.nativeListVideos() }.getOrNull()
                val tagList = runCatching { CoreBridge.nativeListTags() }.getOrNull()
                Json.videos(list ?: "[]") to Json.tags(tagList ?: "[]")
            }
            videos = v
            tags = t
            GalleryCache.vaultLocation = vaultLocation
            GalleryCache.videos = v
            GalleryCache.tags = t
            GalleryCache.loaded = true
        }
    }

    // Cached rows render instantly when the same vault is re-entered (returning
    // from the player); the list is refetched only when the vault really changed.
    LaunchedEffect(vaultLocation) {
        if (GalleryCache.loaded && GalleryCache.vaultLocation == vaultLocation) {
            videos = GalleryCache.videos
            tags = GalleryCache.tags
        } else {
            GalleryCache.clear()
            refresh()
        }
    }

    // Import: multi-select, streamed into the vault one file at a time with a
    // byte-level progress bar for the whole batch (no per-file jumps).
    fun startImport(uris: List<Uri>) {
        if (uris.isEmpty()) return
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

    // Removal is permanent: the core deletes the encrypted package, not just the
    // database row.
    fun removeVideo(target: VideoEntry) {
        actionBusy = true
        actionMessage = null
        scope.launch {
            val res = withContext(Dispatchers.IO) {
                runCatching { CoreBridge.Result.parse(CoreBridge.nativeRemoveVideo(target.id)) }
                    .getOrElse { CoreBridge.Result(false, "Remove failed", it.message ?: "") }
            }
            actionBusy = false
            if (res.ok) {
                actionTarget = null
                refresh()
            } else {
                actionMessage = res.error.ifBlank { res.detail }
            }
        }
    }

    // Export: the core streams the plaintext into the app cache, which is then
    // copied to the document the user picked and deleted again.
    fun exportVideo(target: VideoEntry, destination: Uri) {
        actionBusy = true
        actionMessage = null
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val raw = CoreBridge.nativeRestoreVideo(target.id, context.cacheDir.absolutePath)
                    val obj = JSONObject(raw)
                    if (!obj.optBoolean("ok")) {
                        obj.optString("error").ifBlank { obj.optString("detail", "Export failed") }
                    } else {
                        val plain = File(obj.optString("path"))
                        val opened = context.contentResolver.openOutputStream(destination)
                        if (opened == null) {
                            "Could not open the destination"
                        } else {
                            opened.use { out -> plain.inputStream().use { it.copyTo(out) } }
                            plain.delete()
                            null
                        }
                    }
                } catch (e: Exception) {
                    "Export failed: ${e.message}"
                }
            }
            actionBusy = false
            if (failure == null) actionTarget = null else actionMessage = failure
        }
    }

    // "Import" opens the system photo picker — the gallery the user expects.
    // Android 13+ uses the real picker; older devices fall back to a
    // compatible system picker. No storage permission is involved.
    val galleryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.PickMultipleVisualMedia(32)
    ) { uris: List<Uri> -> startImport(uris) }

    // "Files" keeps the document picker for anything the gallery does not index
    // (Matroska .mkv, SD-card or cloud-provider files).
    val fileLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris: List<Uri> -> startImport(uris) }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri: Uri? ->
        val target = actionTarget
        if (uri != null && target != null) exportVideo(target, uri)
    }

    Column(Modifier.fillMaxSize().background(Mvp.window)) {
        // The desktop's single-row toolbar does not fit a phone: clipping it made
        // Import / Settings / Lock unreachable. Two rows, every action visible.
        Column(
            Modifier
                .fillMaxWidth()
                .background(Mvp.headerBg)
                .padding(horizontal = 12.dp, vertical = 8.dp),
        ) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                MvpCombo(
                    selected = listOf("Details", "Icons", "List")[viewMode],
                    items = listOf("Details", "Icons", "List"),
                    onSelect = { s ->
                        viewMode = listOf("Details", "Icons", "List").indexOf(s)
                        GalleryCache.viewMode = viewMode
                    },
                )
                MvpCombo(
                    selected = if (filterTagId == 0L) "All tags" else (tags.firstOrNull { it.id == filterTagId }?.name ?: "All tags"),
                    items = listOf("All tags") + tags.map { it.name },
                    onSelect = { s ->
                        filterTagId = if (s == "All tags") 0L else (tags.firstOrNull { it.name == s }?.id ?: 0L)
                        GalleryCache.filterTagId = filterTagId
                    },
                )
                MvpInput(
                    value = search,
                    onValueChange = {
                        search = it
                        GalleryCache.search = it
                    },
                    placeholder = "Search",
                    modifier = Modifier.weight(1f),
                )
            }
            Spacer(Modifier.height(6.dp))
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                MvpButton("Import", onClick = {
                    // Photos *and* videos: the vault stores opaque packages, and
                    // the core's probe/thumbnail path handles single-frame media
                    // (images) by decoding the first frame.
                    galleryLauncher.launch(
                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo)
                    )
                }, primary = true, modifier = Modifier.weight(1f))
                MvpButton("Files", onClick = {
                    fileLauncher.launch(
                        arrayOf("video/*", "image/*", "application/octet-stream")
                    )
                }, modifier = Modifier.weight(1f))
                MvpButton("Settings…", onClick = { showSettings = true }, modifier = Modifier.weight(1f))
                MvpButton("Lock", onClick = {
                    scope.launch { withContext(Dispatchers.Default) { CoreBridge.nativeLock() } }
                    GalleryCache.clear()
                    onLock()
                }, modifier = Modifier.weight(1f))
            }
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
        val openActions: (VideoEntry) -> Unit = { v ->
            actionMessage = null
            actionTarget = v
        }

        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (viewMode) {
                0 -> DetailsView(filtered, busy, onPlay, openActions)
                1 -> IconGridView(filtered, busy, onPlay, openActions)
                else -> ListView(filtered, busy, onPlay, openActions)
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

    // Per-item action sheet: remove (permanent) or export a plaintext copy.
    actionTarget?.let { target ->
        Dialog(onDismissRequest = { if (!actionBusy) actionTarget = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(target.name)
                MvpDescription("${target.sizeText} · ${target.tags.joinToString(", ")}")
                MvpError(actionMessage)
                if (actionBusy) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                        Text("Working…", color = Mvp.description, fontSize = 12.sp)
                    }
                }
                MvpButton(
                    "Remove from vault",
                    onClick = { removeVideo(target) },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpDescription("Removal deletes the encrypted package — this cannot be undone.")
                MvpButton(
                    "Export a plaintext copy…",
                    onClick = { exportLauncher.launch(target.name) },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Cancel",
                    onClick = { actionTarget = null },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
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
        // Stage each file in its own directory but keep the *original* file
        // name: the core records the gallery name from the path it imports, so
        // prefixing the temp file would rename the user's video in the vault.
        val staging = File(context.cacheDir, "import_$index").apply { mkdirs() }
        val tmp = File(staging, name)
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
            staging.delete()
        }
        results.add(result)
    }
    return results
}

@Composable
private fun Thumb(id: Long, size: Int) {
    // Thumbnails are cached across screens too — returning from the player must
    // not re-decode every visible frame.
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, size)) }
    LaunchedEffect(id) {
        if (bmp != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            CoreBridge.nativeThumbnail(id, 320)?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
        if (decoded != null) GalleryCache.putThumb(id, size, decoded)
        bmp = decoded
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

/** The ⋮ affordance shared by every view: opens the per-item action sheet. */
@Composable
private fun RowActions(onClick: () -> Unit) {
    Box(
        Modifier
            .width(40.dp)
            .clickable(onClick = onClick)
            .padding(vertical = 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text("⋮", color = Mvp.headerText, fontSize = 20.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun DetailsView(
    videos: List<VideoEntry>,
    busy: Boolean,
    onPlay: (Long, String) -> Unit,
    onAction: (VideoEntry) -> Unit,
) {
    LazyColumn(state = GalleryCache.detailsScroll, modifier = Modifier.fillMaxSize().background(Mvp.inputBg)) {
        item {
            Row(
                Modifier.fillMaxWidth().background(Mvp.headerBg).padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
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
                Spacer(Modifier.width(40.dp))
            }
        }
        items(videos, key = { it.id }) { v ->
            val media = rememberMediaInfo(v.id)
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
                Text(media?.durationText ?: "…", color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(media?.resolution ?: "…", color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                Text(media?.codecText ?: "…", color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Text(v.tags.joinToString(", "), color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Text(v.importedText, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
                RowActions { onAction(v) }
            }
        }
    }
}

@Composable
private fun IconGridView(
    videos: List<VideoEntry>,
    busy: Boolean,
    onPlay: (Long, String) -> Unit,
    onAction: (VideoEntry) -> Unit,
) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 172.dp),
        state = GalleryCache.gridScroll,
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
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    RowActions { onAction(v) }
                }
                Thumb(v.id, 128)
                Text(v.name, color = Mvp.text, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 6.dp))
            }
        }
    }
}

@Composable
private fun ListView(
    videos: List<VideoEntry>,
    busy: Boolean,
    onPlay: (Long, String) -> Unit,
    onAction: (VideoEntry) -> Unit,
) {
    LazyColumn(state = GalleryCache.listScroll, modifier = Modifier.fillMaxSize().background(Mvp.inputBg)) {
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
                Column(Modifier.weight(1f).padding(start = 10.dp)) {
                    Text(v.name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${v.sizeText} · ${v.tags.joinToString(", ")}", color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                RowActions { onAction(v) }
            }
        }
    }
}
