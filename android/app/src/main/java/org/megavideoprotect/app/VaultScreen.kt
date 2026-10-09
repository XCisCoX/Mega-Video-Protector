package org.megavideoprotect.app

import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
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
                val raw = if (RemoteVault.connected()) {
                    RemoteVault.getText("/v1/media?id=$id") ?: "{}"
                } else {
                    CoreBridge.nativeMediaInfo(id)
                }
                val o = JSONObject(raw)
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

/** Gallery: a title row with icon actions, search, a view switch and tag
 *  chips, then an Explorer-style details grid, icon grid, list, a per-item
 *  action sheet (remove / export) and a status bar.
 *
 *  Listing is database-only; duration / resolution / codec are probed by the
 *  visible rows on demand. Probing every row up front made a large vault take
 *  seconds to appear. */
@Composable
fun VaultScreen(
    vaultLocation: String,
    onLock: () -> Unit,
    onPlay: (Long, String, List<VideoEntry>) -> Unit,
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
    // Long-press starts a selection; further taps add or remove items. Empty
    // means a tap plays the item again.
    var selectedIds by remember { mutableStateOf(setOf<Long>()) }
    var confirmRemove by remember { mutableStateOf(false) }
    var showTags by remember { mutableStateOf(false) }
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
                val list = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/videos") else CoreBridge.nativeListVideos()
                }.getOrNull()
                val tagList = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/tags") else CoreBridge.nativeListTags()
                }.getOrNull()
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
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Import on the PC."
            return
        }
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
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Remove it on the PC."
            return
        }
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
                selectedIds = selectedIds - target.id
                refresh()
            } else {
                actionMessage = res.error.ifBlank { res.detail }
            }
        }
    }

    fun removeSelected() {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Remove it on the PC."
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) {
            selectedIds = emptySet()
            return
        }
        actionBusy = true
        error = null
        scope.launch {
            var failure: String? = null
            for (target in targets) {
                val res = withContext(Dispatchers.IO) {
                    runCatching { CoreBridge.Result.parse(CoreBridge.nativeRemoveVideo(target.id)) }
                        .getOrElse { CoreBridge.Result(false, "Remove failed", it.message ?: "") }
                }
                if (!res.ok) {
                    failure = res.error.ifBlank { res.detail }
                    break
                }
            }
            actionBusy = false
            selectedIds = emptySet()
            confirmRemove = false
            if (failure != null) error = failure
            refresh()
        }
    }

    // Writes each selected package out as a plaintext file in the folder the
    // user picked. The core decrypts into the app cache; that copy is removed
    // after it has been written into the folder.
    fun restoreSelected(tree: Uri) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Restore it on the PC."
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) return
        actionBusy = true
        error = null
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val resolver = context.contentResolver
                    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    runCatching { resolver.takePersistableUriPermission(tree, flags) }
                    val root = DocumentsContract.buildDocumentUriUsingTree(
                        tree, DocumentsContract.getTreeDocumentId(tree),
                    )
                    for (target in targets) {
                        val raw = CoreBridge.nativeRestoreVideo(target.id, context.cacheDir.absolutePath)
                        val obj = JSONObject(raw)
                        if (!obj.optBoolean("ok")) {
                            return@withContext obj.optString("error").ifBlank {
                                obj.optString("detail", "Restore failed")
                            }
                        }
                        val plain = File(obj.optString("path"))
                        val mime = if (isImageName(target.name)) "image/*" else "video/*"
                        val dest = DocumentsContract.createDocument(resolver, root, mime, target.name)
                        if (dest == null) {
                            plain.delete()
                            return@withContext "Could not create ${target.name} in that folder"
                        }
                        val opened = resolver.openOutputStream(dest)
                        if (opened == null) {
                            plain.delete()
                            return@withContext "Could not write ${target.name}"
                        }
                        opened.use { out -> plain.inputStream().use { it.copyTo(out) } }
                        plain.delete()
                    }
                    null
                } catch (e: Exception) {
                    "Restore failed: ${e.message}"
                }
            }
            actionBusy = false
            if (failure != null) error = failure else selectedIds = emptySet()
            refresh()
        }
    }

    // Attaches an existing tag to every selected item. If they all already
    // have it, the same tap takes it off.
    fun toggleTagOnSelected(tag: TagEntry) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Change tags on the PC."
            showTags = false
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) return
        val allHave = targets.all { item -> item.tags.any { it.equals(tag.name, ignoreCase = true) } }
        actionBusy = true
        error = null
        scope.launch {
            var failure: String? = null
            for (target in targets) {
                val has = target.tags.any { it.equals(tag.name, ignoreCase = true) }
                if (allHave) {
                    if (!has) continue
                } else if (has) {
                    continue
                }
                val res = withContext(Dispatchers.IO) {
                    val raw = if (allHave) {
                        CoreBridge.nativeRemoveTag(target.id, tag.id)
                    } else {
                        CoreBridge.nativeAddTag(target.id, tag.name)
                    }
                    runCatching { CoreBridge.Result.parse(raw) }
                        .getOrElse { CoreBridge.Result(false, "Tag failed", it.message ?: "") }
                }
                if (!res.ok) {
                    failure = res.error.ifBlank { res.detail }
                    break
                }
            }
            actionBusy = false
            showTags = false
            if (failure != null) error = failure
            refresh()
        }
    }

    // Export: the core streams the plaintext into the app cache, which is then
    // copied to the document the user picked and deleted again.
    fun exportVideo(target: VideoEntry, destination: Uri) {
        if (RemoteVault.connected()) {
            actionMessage = "This vault is open on your PC. Export it on the PC."
            return
        }
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

    val restoreLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri != null) restoreSelected(uri)
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Mvp.window)
            // The activity draws edge to edge. Keep the toolbar under the
            // status bar and the path/count row above the navigation bar.
            .safeDrawingPadding(),
    ) {
        GalleryHeader(
            viewMode = viewMode,
            tags = tags,
            filterTagId = filterTagId,
            search = search,
            onViewMode = { mode ->
                viewMode = mode
                GalleryCache.viewMode = mode
            },
            onFilterTag = { id ->
                filterTagId = id
                GalleryCache.filterTagId = id
            },
            onSearch = { text ->
                search = text
                GalleryCache.search = text
            },
            onImport = import@{
                if (RemoteVault.connected()) {
                    error = "This vault is open on your PC. Import on the PC."
                    return@import
                }
                // Photos *and* videos: the vault stores opaque packages, and
                // the core's probe/thumbnail path handles single-frame media
                // (images) by decoding the first frame.
                galleryLauncher.launch(
                    PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo)
                )
            },
            onFiles = files@{
                if (RemoteVault.connected()) {
                    error = "This vault is open on your PC. Import on the PC."
                    return@files
                }
                fileLauncher.launch(
                    arrayOf("video/*", "image/*", "application/octet-stream")
                )
            },
            onSettings = {
                if (RemoteVault.connected()) {
                    error = "Change the password and tags on the PC."
                } else {
                    showSettings = true
                }
            },
            onLock = {
                if (RemoteVault.connected()) {
                    RemoteVault.disconnect()
                } else {
                    scope.launch { withContext(Dispatchers.Default) { CoreBridge.nativeLock() } }
                }
                GalleryCache.clear()
                onLock()
            },
        )
        Box(Modifier.fillMaxWidth().height(1.dp).background(Mvp.cardBorder))

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
        val selecting = selectedIds.isNotEmpty()
        val openActions: (VideoEntry) -> Unit = { v ->
            actionMessage = null
            actionTarget = v
        }
        val openItem: (VideoEntry) -> Unit = { v ->
            if (selecting) {
                selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
            } else if (!busy) {
                onPlay(v.id, v.name, filtered)
            }
        }
        val pressItem: (VideoEntry) -> Unit = { v ->
            selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
        }

        if (selecting) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Mvp.selection)
                    .padding(horizontal = 12.dp, vertical = 6.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text("${selectedIds.size} selected", color = Mvp.title, fontSize = 14.sp)
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    MvpButton("Remove", onClick = { confirmRemove = true }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                    MvpButton("Restore", onClick = { restoreLauncher.launch(null) }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                    MvpButton("Tag", onClick = { showTags = true }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                    MvpButton("Cancel", onClick = { selectedIds = emptySet() }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                }
            }
        }

        Box(Modifier.weight(1f).fillMaxWidth()) {
            when (viewMode) {
                0 -> DetailsView(filtered, selectedIds, openItem, pressItem, openActions)
                1 -> IconGridView(filtered, selectedIds, openItem, pressItem)
                else -> ListView(filtered, selectedIds, openItem, pressItem, openActions)
            }
        }

        // Status bar (Qt: location + count, #666666).
        Row(
            Modifier
                .fillMaxWidth()
                .background(Mvp.window)
                .padding(horizontal = 12.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(vaultLocation, color = Mvp.statusText, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Text("${filtered.size} items", color = Mvp.statusText, fontSize = 12.sp)
        }
    }

    if (showTags) {
        val chosen = videos.filter { it.id in selectedIds }
        Dialog(onDismissRequest = { if (!actionBusy) showTags = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Tag")
                MvpDescription(
                    if (tags.isEmpty()) "No tags yet. Create one in Settings."
                    else "Tap a tag to add it to the selection. A tag already on every selected item is removed.",
                )
                if (actionBusy) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                }
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 320.dp)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    tags.forEach { tag ->
                        val onAll = chosen.isNotEmpty() && chosen.all { item ->
                            item.tags.any { it.equals(tag.name, ignoreCase = true) }
                        }
                        MvpButton(
                            if (onAll) "✓  ${tag.name}" else tag.name,
                            onClick = { toggleTagOnSelected(tag) },
                            enabled = !actionBusy,
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                }
                MvpButton(
                    "Cancel",
                    onClick = { showTags = false },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }

    if (confirmRemove) {
        Dialog(onDismissRequest = { if (!actionBusy) confirmRemove = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Remove ${selectedIds.size} item(s)?")
                MvpDescription("Removal deletes the encrypted packages. This cannot be undone.")
                MvpButton(
                    "Remove",
                    onClick = { removeSelected() },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Cancel",
                    onClick = { confirmRemove = false },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
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

/** Library title, icon actions, search, and a small view switch. */
@Composable
private fun GalleryHeader(
    viewMode: Int,
    tags: List<TagEntry>,
    filterTagId: Long,
    search: String,
    onViewMode: (Int) -> Unit,
    onFilterTag: (Long) -> Unit,
    onSearch: (String) -> Unit,
    onImport: () -> Unit,
    onFiles: () -> Unit,
    onSettings: () -> Unit,
    onLock: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Mvp.headerBg)
            .padding(start = 16.dp, end = 10.dp, top = 8.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "Library",
                color = Mvp.title,
                fontSize = 22.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            HeaderIcon("Import", Icons.Filled.Add, onImport, filled = true)
            HeaderIcon("Files", FolderIcon, onFiles)
            HeaderIcon("Settings", Icons.Filled.Settings, onSettings)
            HeaderIcon("Lock", Icons.Filled.Lock, onLock)
        }
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                Modifier
                    .weight(1f)
                    .height(40.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .background(Mvp.inputBg)
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    Icons.Filled.Search,
                    contentDescription = null,
                    tint = Mvp.placeholder,
                    modifier = Modifier.size(16.dp),
                )
                Spacer(Modifier.width(8.dp))
                BasicTextField(
                    value = search,
                    onValueChange = onSearch,
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    textStyle = TextStyle(color = Mvp.title, fontSize = 15.sp),
                    cursorBrush = SolidColor(Mvp.accent),
                    decorationBox = { inner ->
                        Box {
                            if (search.isEmpty()) {
                                Text("Search", color = Mvp.placeholder, fontSize = 15.sp)
                            }
                            inner()
                        }
                    },
                )
                if (search.isNotEmpty()) {
                    Box(
                        Modifier
                            .size(24.dp)
                            .clip(CircleShape)
                            .clickable { onSearch("") },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            Icons.Filled.Close,
                            contentDescription = "Clear search",
                            tint = Mvp.description,
                            modifier = Modifier.size(14.dp),
                        )
                    }
                }
            }
            ViewModeButton(DetailsIcon, "Details", viewMode == 0) { onViewMode(0) }
            ViewModeButton(GridIcon, "Icons", viewMode == 1) { onViewMode(1) }
            ViewModeButton(Icons.Filled.List, "List", viewMode == 2) { onViewMode(2) }
            if (tags.isNotEmpty()) {
                TagFilterButton(tags, filterTagId, onFilterTag)
            }
        }
    }
}

@Composable
private fun ViewModeButton(
    icon: ImageVector,
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Box(
        Modifier
            .size(32.dp)
            .clip(CircleShape)
            .background(if (selected) Mvp.selection else Color.Transparent)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon,
            contentDescription = label,
            tint = if (selected) Color.White else Mvp.description,
            modifier = Modifier.size(16.dp),
        )
    }
}

@Composable
private fun TagFilterButton(
    tags: List<TagEntry>,
    filterTagId: Long,
    onFilterTag: (Long) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box {
        ViewModeButton(FilterIcon, "Tags", filterTagId != 0L) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(
                text = {
                    Text(
                        "All",
                        color = if (filterTagId == 0L) Mvp.accent else Mvp.text,
                        fontSize = 14.sp,
                    )
                },
                onClick = { open = false; onFilterTag(0L) },
            )
            tags.forEach { tag ->
                DropdownMenuItem(
                    text = {
                        Text(
                            tag.name,
                            color = if (filterTagId == tag.id) Mvp.accent else Mvp.text,
                            fontSize = 14.sp,
                        )
                    },
                    onClick = { open = false; onFilterTag(tag.id) },
                )
            }
        }
    }
}

@Composable
private fun HeaderIcon(
    label: String,
    icon: ImageVector,
    onClick: () -> Unit,
    filled: Boolean = false,
) {
    Box(
        Modifier
            .padding(horizontal = 2.dp)
            .size(36.dp)
            .clip(CircleShape)
            .background(if (filled) Mvp.primary else Color.Transparent)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon,
            contentDescription = label,
            tint = if (filled) Color.White else Mvp.title,
            modifier = Modifier.size(21.dp),
        )
    }
}

private fun iconPath(builder: ImageVector.Builder, path: String): ImageVector {
    builder.addPath(
        pathData = PathParser().parsePathString(path).toNodes(),
        fill = SolidColor(Color.Black),
    )
    return builder.build()
}

private val GridIcon: ImageVector by lazy {
    iconPath(
        ImageVector.Builder("Grid", 24.dp, 24.dp, 24f, 24f),
        "M4,4h6v6H4z M14,4h6v6h-6z M4,14h6v6H4z M14,14h6v6h-6z",
    )
}

private val DetailsIcon: ImageVector by lazy {
    iconPath(
        ImageVector.Builder("Details", 24.dp, 24.dp, 24f, 24f),
        "M4,5h5v5H4z M11,6h9v2h-9z M11,9h7v1.5H11z M4,12.5h5v5H4z M11,13.5h9v2h-9z M11,16.5h7v1.5H11z",
    )
}

private val FilterIcon: ImageVector by lazy {
    iconPath(
        ImageVector.Builder("Filter", 24.dp, 24.dp, 24f, 24f),
        "M4,5h16l-6,7.2V19l-4,2v-8.8z",
    )
}

private val FolderIcon: ImageVector by lazy {
    ImageVector.Builder(
        name = "Folder",
        defaultWidth = 24.dp,
        defaultHeight = 24.dp,
        viewportWidth = 24f,
        viewportHeight = 24f,
    ).apply {
        addPath(
            pathData = PathParser().parsePathString(
                "M10,4H4c-1.1,0 -1.99,0.9 -1.99,2L2,18c0,1.1 0.9,2 2,2h16c1.1,0 2,-0.9 2,-2V8c0,-1.1 -0.9,-2 -2,-2h-8l-2,-2z"
            ).toNodes(),
            fill = SolidColor(Color.Black),
        )
    }.build()
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
            val jpeg = if (RemoteVault.connected()) {
                RemoteVault.getBytes("/v1/thumbnail?id=$id&max=320")
            } else {
                CoreBridge.nativeThumbnail(id, 320)
            }
            jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
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

/** Square crop that fills an Instagram-style grid cell. */
@Composable
private fun SquareThumb(id: Long) {
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, 480)) }
    LaunchedEffect(id) {
        if (bmp != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            val jpeg = if (RemoteVault.connected()) {
                RemoteVault.getBytes("/v1/thumbnail?id=$id&max=480")
            } else {
                CoreBridge.nativeThumbnail(id, 480)
            }
            jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
        if (decoded != null) GalleryCache.putThumb(id, 480, decoded)
        bmp = decoded
    }
    val image = bmp
    if (image == null) {
        Box(Modifier.fillMaxSize().background(Mvp.card), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
        }
    } else {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.fillMaxSize(),
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
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
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
            val picked = v.id in selected
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(
                        when {
                            picked -> Mvp.selection
                            videos.indexOf(v) % 2 == 0 -> Mvp.inputBg
                            else -> Mvp.alternateRow
                        },
                    )
                    .combinedClickable(onClick = { onOpen(v) }, onLongClick = { onLongPress(v) })
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
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
) {
    // Three square columns, a hairline gap, no captions: the profile-grid look.
    LazyVerticalGrid(
        columns = GridCells.Fixed(3),
        state = GalleryCache.gridScroll,
        modifier = Modifier.fillMaxSize().background(Color.Black),
        horizontalArrangement = Arrangement.spacedBy(1.dp),
        verticalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        items(videos, key = { it.id }) { v ->
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(1f)
                    .combinedClickable(onClick = { onOpen(v) }, onLongClick = { onLongPress(v) }),
            ) {
                SquareThumb(v.id)
                if (v.id in selected) {
                    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.45f)))
                    Box(
                        Modifier
                            .align(Alignment.TopEnd)
                            .padding(6.dp)
                            .size(22.dp)
                            .clip(CircleShape)
                            .background(Mvp.primary),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("✓", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
    }
}

@Composable
private fun ListView(
    videos: List<VideoEntry>,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
) {
    LazyColumn(state = GalleryCache.listScroll, modifier = Modifier.fillMaxSize().background(Mvp.inputBg)) {
        items(videos, key = { it.id }) { v ->
            val picked = v.id in selected
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(
                        when {
                            picked -> Mvp.selection
                            videos.indexOf(v) % 2 == 0 -> Mvp.inputBg
                            else -> Mvp.alternateRow
                        },
                    )
                    .combinedClickable(onClick = { onOpen(v) }, onLongClick = { onLongPress(v) })
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
