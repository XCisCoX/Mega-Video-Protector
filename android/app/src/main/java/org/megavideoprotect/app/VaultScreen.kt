package org.megavideoprotect.app

import android.app.Activity
import android.content.ClipData
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.view.DragAndDropPermissions
import android.view.View
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.draganddrop.dragAndDropSource
import androidx.compose.foundation.draganddrop.dragAndDropTarget
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
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
import androidx.compose.ui.draganddrop.DragAndDropEvent
import androidx.compose.ui.draganddrop.DragAndDropTarget
import androidx.compose.ui.draganddrop.DragAndDropTransferData
import androidx.compose.ui.draganddrop.toAndroidDragEvent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerEventTimeoutCancellationException
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import dev.chrisbanes.haze.HazeStyle
import dev.chrisbanes.haze.HazeTint
import dev.chrisbanes.haze.hazeEffect
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
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
    var folders: List<FolderEntry> = emptyList()
    var currentFolderId = 0L
    var loaded = false
    // Toolbar state — deliberately *not* reset by clear(): which view you were
    // using is a preference, not vault data.
    var viewMode = 0
    var filterTagId = 0L
    var search = ""
    var sortKey by mutableStateOf(SortKey.Name)
    var sortAscending by mutableStateOf(true)
    var mediaVersion by mutableStateOf(0)
    val media = HashMap<Long, MediaInfo>()
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

    fun noteMedia(id: Long, info: MediaInfo) {
        if (media[id] == info) return
        media[id] = info
        mediaVersion++
    }

    fun clear() {
        vaultLocation = null
        videos = emptyList()
        tags = emptyList()
        folders = emptyList()
        currentFolderId = 0L
        loaded = false
        thumbs.clear()
        media.clear()
        mediaVersion++
    }
}

enum class SortKey(val label: String) {
    Name("Name"),
    Size("Size"),
    Duration("Duration"),
    Resolution("Resolution"),
    Codec("Codec"),
    Tags("Tags"),
    Imported("Imported"),
}

private fun applySort(key: SortKey) {
    if (GalleryCache.sortKey == key) {
        GalleryCache.sortAscending = !GalleryCache.sortAscending
    } else {
        GalleryCache.sortKey = key
        GalleryCache.sortAscending = true
    }
}

/** Per-row container metadata. Probing opens the package and decrypts container
 *  bytes, so it is done lazily for visible rows only — the desktop fills these
 *  columns the same way. */
data class MediaInfo(
    val durationMs: Long,
    val width: Int,
    val height: Int,
    val codec: String,
    val artist: String = "",
    val title: String = "",
) {
    val resolution: String get() = if (width > 0 && height > 0) "${width}×${height}" else "…"
    val durationText: String
        get() {
            if (durationMs <= 0) return "…"
            val sec = durationMs / 1000
            return "%d:%02d".format(sec / 60, sec % 60)
        }
    val codecText: String get() = codec.ifBlank { "…" }
}

/** "Artist - Title" from a file name, with a leading track number stripped. */
private fun songLabel(name: String): Pair<String, String> {
    var base = name.substringBeforeLast('.', name).trim()
    base = base.replace(Regex("^\\d{1,3}[.\\s\\-_]+"), "").trim().ifBlank {
        name.substringBeforeLast('.', name).trim()
    }
    val parts = base.split(Regex("\\s+[—–-]\\s+"), limit = 2)
    return if (parts.size == 2 && parts[0].isNotBlank() && parts[1].isNotBlank()) {
        parts[0].trim() to parts[1].trim()
    } else {
        "" to base
    }
}

private fun loadMedia(id: Long): MediaInfo? = runCatching {
    val raw = if (RemoteVault.connected()) {
        RemoteVault.getText("/v1/media?id=$id") ?: "{}"
    } else if (GalleryCache.videos.any { it.id == id && isImageName(it.name) }) {
        // A still has no movie duration. Probing it crashes the native reader
        // on some gallery JPEGs and takes the whole app down.
        return MediaInfo(0L, 0, 0, "image")
    } else {
        CoreBridge.nativeMediaInfo(id)
    }
    val o = JSONObject(raw)
    if (!o.has("width") && !o.has("durationMs")) return null
    MediaInfo(
        durationMs = o.optLong("durationMs", 0),
        width = o.optInt("width", 0),
        height = o.optInt("height", 0),
        codec = o.optString("codec", ""),
        artist = o.optString("artist", ""),
        title = o.optString("title", ""),
    )
}.getOrNull()

@Composable
private fun rememberMediaInfo(id: Long): MediaInfo? {
    var info by remember(id) { mutableStateOf(GalleryCache.media[id]) }
    LaunchedEffect(id) {
        if (info != null) return@LaunchedEffect
        val loaded = withContext(Dispatchers.Default) { loadMedia(id) }
        info = loaded
        if (loaded != null) GalleryCache.noteMedia(id, loaded)
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
    onOpenMusic: () -> Unit = {},
    shareUris: List<Uri> = emptyList(),
    onShareHandled: () -> Unit = {},
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var videos by remember { mutableStateOf(GalleryCache.videos) }
    var tags by remember { mutableStateOf(GalleryCache.tags) }
    var folders by remember { mutableStateOf(GalleryCache.folders) }
    var currentFolderId by remember { mutableStateOf(GalleryCache.currentFolderId) }
    var folderMenu by remember { mutableStateOf<FolderEntry?>(null) }
    var moveTarget by remember { mutableStateOf<MoveRequest?>(null) }
    var namePrompt by remember { mutableStateOf<NamePrompt?>(null) }
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
            val (v, t, f) = withContext(Dispatchers.Default) {
                val list = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/videos") else CoreBridge.nativeListVideos()
                }.getOrNull()
                val tagList = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/tags") else CoreBridge.nativeListTags()
                }.getOrNull()
                val folderList = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/folders") else CoreBridge.nativeListFolders()
                }.getOrNull()
                Triple(Json.videos(list ?: "[]"), Json.tags(tagList ?: "[]"), Json.folders(folderList ?: "[]"))
            }
            videos = v
            tags = t
            folders = f
            if (currentFolderId != 0L && f.none { it.id == currentFolderId }) {
                currentFolderId = 0L
                GalleryCache.currentFolderId = 0L
            }
            GalleryCache.vaultLocation = vaultLocation
            GalleryCache.videos = v
            GalleryCache.tags = t
            GalleryCache.folders = f
            GalleryCache.loaded = true
        }
    }

    // Cached rows render instantly when the same vault is re-entered (returning
    // from the player); the list is refetched only when the vault really changed.
    LaunchedEffect(vaultLocation) {
        if (GalleryCache.loaded && GalleryCache.vaultLocation == vaultLocation) {
            videos = GalleryCache.videos
            tags = GalleryCache.tags
            folders = GalleryCache.folders
            currentFolderId = GalleryCache.currentFolderId
        } else {
            GalleryCache.clear()
            refresh()
        }
    }

    // Import: multi-select, streamed into the vault one file at a time with a
    // byte-level progress bar for the whole batch (no per-file jumps).
    fun startImport(
        uris: List<Uri>,
        folderId: Long = currentFolderId,
        permissions: DragAndDropPermissions? = null,
    ) {
        if (RemoteVault.connected()) {
            permissions?.release()
            error = "This vault is open on your PC. Import on the PC."
            return
        }
        if (uris.isEmpty()) {
            permissions?.release()
            return
        }
        error = null
        importProgress = 0f
        importLabel = "Adding to vault"
        scope.launch {
            try {
                val results = withContext(Dispatchers.IO) {
                    importBatch(context, uris, folderId) { done, total, label ->
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
            } finally {
                permissions?.release()
            }
        }
    }

    LaunchedEffect(shareUris) {
        if (shareUris.isEmpty()) return@LaunchedEffect
        startImport(shareUris)
        onShareHandled()
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

    var showAdd by remember { mutableStateOf(false) }

    // The system photo picker caps a selection. Ask for its own maximum
    // instead of a small fixed count.
    val galleryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.PickMultipleVisualMedia()
    ) { uris: List<Uri> -> startImport(uris) }

    val musicLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris: List<Uri> -> startImport(uris) }

    // Documents cover containers the gallery does not index (Matroska, files
    // from another app or a cloud provider).
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

    val haze = rememberHazeState()
    val density = LocalDensity.current
    var topInset by remember { mutableStateOf(0) }
    var bottomInset by remember { mutableStateOf(0) }
    val contentPad = PaddingValues(
        top = with(density) { topInset.toDp() },
        bottom = with(density) { bottomInset.toDp() },
    )
    val glass = HazeStyle(
        backgroundColor = Color(0xC4101014),
        tints = listOf(HazeTint(Color.Black.copy(alpha = 0.28f))),
        blurRadius = 32.dp,
        noiseFactor = 0.03f,
    )

    fun openFolder(id: Long) {
        currentFolderId = id
        GalleryCache.currentFolderId = id
        selectedIds = emptySet()
    }

    fun moveDragged(drag: VaultDrag, destination: Long) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Move it on the PC."
            return
        }
        val movingFolders = drag.folders.filter { id ->
            (folders.firstOrNull { it.id == id }?.parentId ?: 0L) != destination
        }
        val movingVideos = drag.videos.filter { id ->
            videos.firstOrNull { it.id == id }?.folderId != destination
        }
        if (movingFolders.isEmpty() && movingVideos.isEmpty()) return
        scope.launch {
            val problem = withContext(Dispatchers.Default) {
                var message = ""
                for (folderId in movingFolders) {
                    if (folderContains(folders, folderId, destination)) {
                        message = "A folder can't be moved into itself."
                        continue
                    }
                    val result = CoreBridge.Result.parse(CoreBridge.nativeMoveFolder(folderId, destination))
                    if (!result.ok) message = result.detail.ifBlank { result.error }
                }
                for (videoId in movingVideos) {
                    val result = CoreBridge.Result.parse(CoreBridge.nativeMoveVideo(videoId, destination))
                    if (!result.ok) message = result.detail.ifBlank { result.error }
                }
                message
            }
            if (problem.isNotEmpty()) error = problem
            selectedIds = emptySet()
            refresh()
        }
    }

    fun handleDrop(event: DragAndDropEvent, destination: Long): Boolean {
        val dragEvent = event.toAndroidDragEvent()
        val local = dragEvent.localState as? VaultDrag
        if (local != null) {
            moveDragged(local, destination)
            return true
        }
        val clip = dragEvent.clipData
        val uris = ArrayList<Uri>()
        if (clip != null) {
            for (index in 0 until clip.itemCount) {
                clip.getItemAt(index).uri?.let { uris.add(it) }
            }
        }
        if (uris.isNotEmpty()) {
            val permissions = activityOf(context)?.requestDragAndDropPermissions(dragEvent)
            startImport(uris, destination, permissions)
            return true
        }
        val parsed = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.text?.toString()?.let { parseVaultDrag(it) }
        if (parsed != null) {
            moveDragged(parsed, destination)
            return true
        }
        return false
    }

    val dragVideo: (VideoEntry) -> DragAndDropTransferData = { video ->
        val ids = if (video.id in selectedIds) selectedIds.toList() else listOf(video.id)
        vaultDragData(VaultDrag(ids, emptyList()))
    }
    val dropHere: (DragAndDropEvent) -> Boolean = { handleDrop(it, currentFolderId) }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black),
    ) {
        if (showSettings) {
            SettingsDialog(
                currentTags = tags,
                remote = RemoteVault.connected(),
                onDismiss = { showSettings = false },
                onChanged = { refresh() },
            )
        }

        val filtered = videos.filter { v ->
            v.folderId == currentFolderId &&
                (filterTagId == 0L || v.tags.any { t -> tags.firstOrNull { it.id == filterTagId }?.name?.equals(t, true) == true }) &&
                (search.isBlank() || v.name.contains(search, ignoreCase = true))
        }
        val childFolders = folders.filter { folder ->
            folder.parentId == currentFolderId &&
                (search.isBlank() || folder.name.contains(search, ignoreCase = true))
        }
        val sortKey = GalleryCache.sortKey
        val sortAscending = GalleryCache.sortAscending
        val mediaVersion = GalleryCache.mediaVersion
        val shown = remember(filtered, sortKey, sortAscending, mediaVersion) {
            val cmp = Comparator<VideoEntry> { a, b ->
                val primary = when (sortKey) {
                    SortKey.Name -> a.name.compareTo(b.name, ignoreCase = true)
                    SortKey.Size -> a.size.compareTo(b.size)
                    SortKey.Duration ->
                        (GalleryCache.media[a.id]?.durationMs ?: 0L)
                            .compareTo(GalleryCache.media[b.id]?.durationMs ?: 0L)
                    SortKey.Resolution -> {
                        fun pixels(v: VideoEntry) =
                            GalleryCache.media[v.id]?.let { it.width.toLong() * it.height.toLong() } ?: 0L
                        pixels(a).compareTo(pixels(b))
                    }
                    SortKey.Codec ->
                        (GalleryCache.media[a.id]?.codec ?: "").compareTo(
                            GalleryCache.media[b.id]?.codec ?: "",
                            ignoreCase = true,
                        )
                    SortKey.Tags ->
                        a.tags.joinToString(",").compareTo(b.tags.joinToString(","), ignoreCase = true)
                    SortKey.Imported -> a.importedAt.compareTo(b.importedAt)
                }
                if (primary != 0) primary else a.name.compareTo(b.name, ignoreCase = true)
            }
            if (sortAscending) filtered.sortedWith(cmp) else filtered.sortedWith(cmp.reversed())
        }
        LaunchedEffect(sortKey, videos) {
            if (sortKey != SortKey.Duration && sortKey != SortKey.Resolution && sortKey != SortKey.Codec) return@LaunchedEffect
            for (v in videos) {
                if (GalleryCache.media.containsKey(v.id)) continue
                val loaded = withContext(Dispatchers.Default) { loadMedia(v.id) } ?: continue
                GalleryCache.noteMedia(v.id, loaded)
            }
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
                onPlay(v.id, v.name, shown)
            }
        }
        val pressItem: (VideoEntry) -> Unit = { v ->
            selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
        }

        val upParentId = if (currentFolderId == 0L) {
            null
        } else {
            folders.firstOrNull { it.id == currentFolderId }?.parentId ?: 0L
        }
        val orderedFolders = remember(childFolders, sortKey, sortAscending) {
            val byName = childFolders.sortedBy { it.name.lowercase() }
            if (sortKey == SortKey.Name && !sortAscending) byName.asReversed() else byName
        }
        Box(Modifier.fillMaxSize().hazeSource(state = haze)) {
            when (viewMode) {
                0 -> DetailsView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                )
                1 -> IconGridView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                )
                else -> ListView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                )
            }
        }

        Column(
            Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .onSizeChanged { topInset = it.height }
                .hazeEffect(state = haze, style = glass)
                .statusBarsPadding(),
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
                        error = "This vault is open on your PC. Add it on the PC."
                        return@import
                    }
                    showAdd = true
                },
                onSettings = { showSettings = true },
                onLock = onLock,
            )
            TelegramMusicBar(onOpen = onOpenMusic)
            FolderBar(
                folders = folders,
                currentFolderId = currentFolderId,
                onOpen = { openFolder(it) },
                onDrop = { event, folderId -> handleDrop(event, folderId) },
            )
            if (selecting) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Text("${selectedIds.size} selected", color = Mvp.title, fontSize = 14.sp)
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        MvpButton("Move", onClick = {
                            if (RemoteVault.connected()) {
                                error = "This vault is open on your PC. Move it on the PC."
                            } else {
                                moveTarget = MoveRequest.Videos(selectedIds.toList())
                            }
                        }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                        MvpButton("Remove", onClick = { confirmRemove = true }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                        MvpButton("Restore", onClick = { restoreLauncher.launch(null) }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                        MvpButton("Tag", onClick = { showTags = true }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                        MvpButton("Cancel", onClick = { selectedIds = emptySet() }, enabled = !actionBusy, modifier = Modifier.weight(1f))
                    }
                }
            }
            importProgress?.let { fraction ->
                Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)) {
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
        }

        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .onSizeChanged { bottomInset = it.height }
                .hazeEffect(state = haze, style = glass)
                .navigationBarsPadding()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(vaultLocation, color = Color.White.copy(alpha = 0.86f), fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Text("${childFolders.size + filtered.size} items", color = Color.White.copy(alpha = 0.86f), fontSize = 12.sp)
        }

        error?.let { message ->
            LaunchedEffect(message) {
                delay(4200)
                if (error == message) error = null
            }
            Text(
                message,
                color = Color.White,
                fontSize = 13.sp,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = with(density) { bottomInset.toDp() } + 10.dp)
                    .padding(horizontal = 28.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .hazeEffect(state = haze, style = glass)
                    .border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(16.dp))
                    .clickable { error = null }
                    .padding(horizontal = 16.dp, vertical = 10.dp),
            )
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
                        .heightIn(max = 240.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                FlowRow(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    tags.forEach { tag ->
                        val onAll = chosen.isNotEmpty() && chosen.all { item ->
                            item.tags.any { it.equals(tag.name, ignoreCase = true) }
                        }
                        GlassChip(
                            text = if (onAll) "✓  ${tag.name}" else tag.name,
                            selected = onAll,
                            onClick = { toggleTagOnSelected(tag) },
                            enabled = !actionBusy,
                        )
                    }
                }
                }
                Text(
                    "Done",
                    color = Mvp.accent,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .align(Alignment.CenterHorizontally)
                        .clickable(enabled = !actionBusy) { showTags = false }
                        .padding(vertical = 4.dp),
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
                    "Rename",
                    onClick = {
                        if (RemoteVault.connected()) {
                            actionMessage = "This vault is open on your PC. Rename it on the PC."
                        } else {
                            val video = target
                            actionTarget = null
                            namePrompt = NamePrompt.RenameVideo(video.id, video.name)
                        }
                    },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Move to folder…",
                    onClick = {
                        if (RemoteVault.connected()) {
                            actionMessage = "This vault is open on your PC. Move it on the PC."
                        } else {
                            moveTarget = MoveRequest.Videos(listOf(target.id))
                            actionTarget = null
                        }
                    },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
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

    folderMenu?.let { folder ->
        Dialog(onDismissRequest = { folderMenu = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(folder.name)
                MvpDescription("Videos inside stay in the vault. Removing a folder moves them up one level.")
                MvpButton("Open", onClick = { folderMenu = null; openFolder(folder.id) }, modifier = Modifier.fillMaxWidth())
                MvpButton("Rename", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected()) error = "This vault is open on your PC. Rename the folder on the PC."
                    else namePrompt = NamePrompt.RenameFolder(folder.id, folder.name)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Move to…", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected()) error = "This vault is open on your PC. Move the folder on the PC."
                    else moveTarget = MoveRequest.Folder(folder.id)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Remove folder", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected()) {
                        error = "This vault is open on your PC. Remove the folder on the PC."
                    } else scope.launch {
                        val result = withContext(Dispatchers.Default) {
                            CoreBridge.Result.parse(CoreBridge.nativeRemoveFolder(folder.id))
                        }
                        if (!result.ok) {
                            error = result.detail.ifBlank { result.error }
                        } else {
                            if (folderContains(folders, folder.id, currentFolderId)) {
                                openFolder(folder.parentId)
                            }
                            refresh()
                        }
                    }
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { folderMenu = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    if (showAdd) {
        Dialog(onDismissRequest = { showAdd = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Add")
                MvpButton("Gallery", onClick = {
                    showAdd = false
                    galleryLauncher.launch(
                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo)
                    )
                }, primary = true, modifier = Modifier.fillMaxWidth())
                MvpButton("Music", onClick = {
                    showAdd = false
                    musicLauncher.launch(arrayOf("audio/*"))
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Files", onClick = {
                    showAdd = false
                    fileLauncher.launch(arrayOf("video/*", "audio/*", "image/*", "application/octet-stream"))
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("New folder", onClick = {
                    showAdd = false
                    namePrompt = NamePrompt.NewFolder(currentFolderId)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { showAdd = false }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    namePrompt?.let { prompt ->
        var draft by remember(prompt) { mutableStateOf(prompt.initial) }
        Dialog(onDismissRequest = { namePrompt = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(prompt.title)
                MvpInput(draft, { draft = it }, "Name")
                MvpButton("Save", onClick = {
                    val typed = draft.trim()
                    scope.launch {
                        val result = withContext(Dispatchers.Default) {
                            CoreBridge.Result.parse(
                                when (prompt) {
                                    is NamePrompt.NewFolder -> CoreBridge.nativeCreateFolder(prompt.parentId, typed)
                                    is NamePrompt.RenameFolder -> CoreBridge.nativeRenameFolder(prompt.id, typed)
                                    is NamePrompt.RenameVideo -> CoreBridge.nativeRenameVideo(prompt.id, typed)
                                }
                            )
                        }
                        if (!result.ok) {
                            error = result.detail.ifBlank { result.error }
                        } else {
                            namePrompt = null
                            refresh()
                        }
                    }
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { namePrompt = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    moveTarget?.let { request ->
        val blocked = when (request) {
            is MoveRequest.Folder -> folderSubtree(folders, request.id)
            is MoveRequest.Videos -> emptySet()
        }
        val choices = ArrayList<Pair<Long, String>>()
        if (0L !in blocked) choices.add(0L to "Library")
        folders.filter { it.id !in blocked }.sortedBy { it.name.lowercase() }.forEach { folder ->
            choices.add(folder.id to folderPath(folders, folder.id))
        }
        Dialog(onDismissRequest = { moveTarget = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Move to")
                Column(
                    Modifier.fillMaxWidth().heightIn(max = 320.dp).verticalScroll(rememberScrollState()),
                ) {
                    choices.forEach { (id, label) ->
                        Text(
                            label,
                            color = Mvp.text,
                            fontSize = 15.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    val chosen = id
                                    val moving = request
                                    moveTarget = null
                                    scope.launch {
                                        val result = withContext(Dispatchers.Default) {
                                            when (moving) {
                                                is MoveRequest.Folder ->
                                                    CoreBridge.Result.parse(CoreBridge.nativeMoveFolder(moving.id, chosen))
                                                is MoveRequest.Videos -> {
                                                    var last = CoreBridge.Result(true, "", "")
                                                    for (videoId in moving.ids) {
                                                        last = CoreBridge.Result.parse(CoreBridge.nativeMoveVideo(videoId, chosen))
                                                        if (!last.ok) break
                                                    }
                                                    last
                                                }
                                            }
                                        }
                                        if (!result.ok) error = result.detail.ifBlank { result.error }
                                        else refresh()
                                    }
                                }
                                .padding(vertical = 10.dp),
                        )
                    }
                }
                MvpButton("Cancel", onClick = { moveTarget = null }, modifier = Modifier.fillMaxWidth())
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
    onSettings: () -> Unit,
    onLock: () -> Unit,
) {
        Column(
        Modifier
            .fillMaxWidth()
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
            HeaderIcon("Add", Icons.Filled.Add, onImport, filled = true)
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
                    .background(Mvp.glass)
                    .border(1.dp, Mvp.glassStroke, RoundedCornerShape(20.dp))
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
            Row(
                Modifier
                    .clip(RoundedCornerShape(20.dp))
                    .background(Mvp.glass)
                    .border(1.dp, Mvp.glassStroke, RoundedCornerShape(20.dp))
                    .padding(2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ViewModeButton(DetailsIcon, "Details", viewMode == 0) { onViewMode(0) }
                ViewModeButton(GridIcon, "Icons", viewMode == 1) { onViewMode(1) }
                ViewModeButton(Icons.Filled.List, "List", viewMode == 2) { onViewMode(2) }
            }
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
private sealed class NamePrompt(val title: String, val initial: String) {
    class NewFolder(val parentId: Long) : NamePrompt("New folder", "")
    class RenameFolder(val id: Long, name: String) : NamePrompt("Rename folder", name)
    class RenameVideo(val id: Long, name: String) : NamePrompt("Rename", name)
}

private sealed class MoveRequest {
    class Videos(val ids: List<Long>) : MoveRequest()
    class Folder(val id: Long) : MoveRequest()
}

private fun folderContains(folders: List<FolderEntry>, ancestor: Long, node: Long): Boolean {
    var cursor = node
    repeat(64) {
        if (cursor == 0L) return false
        if (cursor == ancestor) return true
        cursor = folders.firstOrNull { it.id == cursor }?.parentId ?: return false
    }
    return false
}

private fun folderSubtree(folders: List<FolderEntry>, root: Long): Set<Long> {
    val ids = ArrayList<Long>()
    ids.add(root)
    var index = 0
    while (index < ids.size) {
        val parent = ids[index++]
        folders.filter { it.parentId == parent }.forEach { ids.add(it.id) }
    }
    return ids.toSet()
}

private fun folderPath(folders: List<FolderEntry>, folderId: Long): String {
    val parts = ArrayList<String>()
    var cursor = folderId
    for (guard in 0 until 64) {
        if (cursor == 0L) break
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        parts.add(0, folder.name)
        cursor = folder.parentId
    }
    return if (parts.isEmpty()) "Library" else parts.joinToString(" / ")
}

private data class VaultDrag(val videos: List<Long>, val folders: List<Long>)

private fun vaultDragData(drag: VaultDrag): DragAndDropTransferData {
    val text = "v:" + drag.videos.joinToString(",") + ";f:" + drag.folders.joinToString(",")
    return DragAndDropTransferData(
        ClipData.newPlainText("mvp-vault", text),
        drag,
        View.DRAG_FLAG_GLOBAL,
    )
}

private fun parseVaultDrag(text: String): VaultDrag? {
    if (!text.startsWith("v:") || !text.contains(";f:")) return null
    val parts = text.split(";f:", limit = 2)
    if (parts.size != 2) return null
    val videos = parts[0].removePrefix("v:").split(',').mapNotNull { it.toLongOrNull() }
    val folders = parts[1].split(',').mapNotNull { it.toLongOrNull() }
    if (videos.isEmpty() && folders.isEmpty()) return null
    return VaultDrag(videos, folders)
}

private fun activityOf(context: Context): Activity? {
    var current = context
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

private fun acceptsVaultDrop(event: DragAndDropEvent): Boolean {
    val drag = event.toAndroidDragEvent()
    if (drag.localState is VaultDrag) return true
    val clip = drag.clipData ?: return false
    for (index in 0 until clip.itemCount) {
        val item = clip.getItemAt(index)
        if (item.uri != null) return true
        val text = item.text?.toString() ?: continue
        if (parseVaultDrag(text) != null) return true
    }
    return false
}

@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.vaultDropTarget(
    onActive: (Boolean) -> Unit,
    onDrop: (DragAndDropEvent) -> Boolean,
): Modifier = this.dragAndDropTarget(
    shouldStartDragAndDrop = ::acceptsVaultDrop,
    target = object : DragAndDropTarget {
        override fun onEntered(event: DragAndDropEvent) { onActive(true) }
        override fun onExited(event: DragAndDropEvent) { onActive(false) }
        override fun onEnded(event: DragAndDropEvent) { onActive(false) }
        override fun onDrop(event: DragAndDropEvent): Boolean {
            onActive(false)
            return onDrop(event)
        }
    },
)

/**
 * Tap opens. A long press that lifts without moving is the item menu or
 * selection. A long press that then moves starts a drag, the same split a
 * file manager uses so the list can still scroll before the long press.
 */
@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.explorerItem(
    onClick: () -> Unit,
    onLongPress: () -> Unit,
    transfer: () -> DragAndDropTransferData,
): Modifier = this.dragAndDropSource(
    drawDragDecoration = {
        drawRoundRect(color = Color(0xCC3390EC), cornerRadius = CornerRadius(24f, 24f))
    },
    block = {
    val source = this
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        var timedOut = false
        val up = try {
            withTimeout(viewConfiguration.longPressTimeoutMillis) {
                waitForUpOrCancellation()
            }
        } catch (_: PointerEventTimeoutCancellationException) {
            timedOut = true
            null
        }
        if (!timedOut) {
            if (up != null) {
                up.consume()
                onClick()
            }
            return@awaitEachGesture
        }
        // A long press that lifts is the menu. The first move hands the pointer
        // to the system drag so a drop on a folder is delivered.
        val origin = down.position
        val slop = viewConfiguration.touchSlop
        while (true) {
            val event = awaitPointerEvent(PointerEventPass.Initial)
            val change = event.changes.firstOrNull { it.id == down.id } ?: return@awaitEachGesture
            if (!change.pressed) {
                onLongPress()
                return@awaitEachGesture
            }
            if ((change.position - origin).getDistance() >= slop) {
                source.startTransfer(transfer())
                return@awaitEachGesture
            }
        }
    }
    },
)

@Composable
private fun FolderBar(
    folders: List<FolderEntry>,
    currentFolderId: Long,
    onOpen: (Long) -> Unit,
    onDrop: (DragAndDropEvent, Long) -> Boolean,
) {
    val crumb = ArrayList<FolderEntry>()
    var cursor = currentFolderId
    var guard = 0
    while (cursor != 0L && guard++ < 64) {
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        crumb.add(0, folder)
        cursor = folder.parentId
    }
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Crumb("Library", current = currentFolderId == 0L, onClick = { onOpen(0L) }, onDrop = { onDrop(it, 0L) })
        crumb.forEach { folder ->
            Text("/", color = Mvp.description, fontSize = 13.sp)
            Crumb(
                folder.name,
                current = folder.id == currentFolderId,
                onClick = { onOpen(folder.id) },
                onDrop = { onDrop(it, folder.id) },
            )
        }
    }
}

@Composable
private fun Crumb(
    text: String,
    current: Boolean,
    onClick: () -> Unit,
    onDrop: (DragAndDropEvent) -> Boolean,
) {
    var hot by remember { mutableStateOf(false) }
    Text(
        text,
        color = if (current || hot) Color.White else Mvp.accent,
        fontSize = 13.sp,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(if (hot) Mvp.accent.copy(alpha = 0.45f) else Color.Transparent)
            .vaultDropTarget(onActive = { hot = it }, onDrop = onDrop)
            .clickable(onClick = onClick)
            .padding(horizontal = 4.dp, vertical = 2.dp),
    )
}

@Composable
private fun FolderGlyph(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val w = size.width
        val h = size.height
        drawRoundRect(
            color = Color(0xFF8CC4FF),
            topLeft = Offset(w * 0.08f, h * 0.14f),
            size = Size(w * 0.42f, h * 0.22f),
            cornerRadius = CornerRadius(w * 0.08f, w * 0.08f),
        )
        drawRoundRect(
            color = Color(0xFF408CE6),
            topLeft = Offset(w * 0.06f, h * 0.30f),
            size = Size(w * 0.88f, h * 0.56f),
            cornerRadius = CornerRadius(w * 0.1f, w * 0.1f),
        )
    }
}

private fun folderModifier(
    onClick: () -> Unit,
    onLongPress: (() -> Unit)?,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onHot: (Boolean) -> Unit,
): Modifier {
    val base = Modifier.vaultDropTarget(onActive = onHot, onDrop = onDrop)
    return if (transfer != null && onLongPress != null) {
        base.explorerItem(onClick = onClick, onLongPress = onLongPress, transfer = transfer)
    } else {
        base.clickable(onClick = onClick)
    }
}

@Composable
private fun FolderDetailRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
) {
    var hot by remember { mutableStateOf(false) }
    Row(
        Modifier
            .fillMaxWidth()
            .background(if (hot) Mvp.accent.copy(alpha = 0.35f) else Mvp.inputBg)
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it })
            .padding(horizontal = 10.dp, vertical = 7.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(44.dp))
        Text(name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(2.2f))
        Text(meta, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
        Spacer(Modifier.weight(5f))
        Spacer(Modifier.width(40.dp))
    }
}

@Composable
private fun FolderListRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
) {
    var hot by remember { mutableStateOf(false) }
    Row(
        Modifier
            .fillMaxWidth()
            .background(if (hot) Mvp.accent.copy(alpha = 0.35f) else Mvp.inputBg)
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it })
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(36.dp))
        Column(Modifier.weight(1f).padding(start = 10.dp)) {
            Text(name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(meta, color = Mvp.description, fontSize = 11.sp, maxLines = 1)
        }
    }
}

@Composable
private fun FolderTile(
    name: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
) {
    var hot by remember { mutableStateOf(false) }
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(if (hot) Mvp.accent.copy(alpha = 0.35f) else Color(0xFF161618))
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it }),
    ) {
        FolderGlyph(Modifier.align(Alignment.Center).fillMaxSize().padding(18.dp))
        Text(
            name,
            color = Color.White,
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.align(Alignment.BottomStart).padding(start = 6.dp, end = 6.dp, bottom = 6.dp),
        )
        if (hot) {
            Box(Modifier.fillMaxSize().background(Mvp.accent.copy(alpha = 0.35f)))
        }
    }
}

private suspend fun importBatch(
    context: android.content.Context,
    uris: List<Uri>,
    folderId: Long,
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
            CoreBridge.Result.parse(CoreBridge.nativeImportInto(tmp.absolutePath, folderId))
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
private fun Thumb(id: Long, size: Int, audio: Boolean = false, image: Boolean = false) {
    // Thumbnails are cached across screens too — returning from the player must
    // not re-decode every visible frame.
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, size)) }
    LaunchedEffect(id) {
        if (bmp != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            if (image && !RemoteVault.connected()) {
                decodeVaultImage(id, 320)
            } else {
                val jpeg = if (RemoteVault.connected()) {
                    RemoteVault.getBytes("/v1/thumbnail?id=$id&max=320")
                } else {
                    CoreBridge.nativeThumbnail(id, 320)
                }
                jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
            }
        }
        if (decoded != null) {
            GalleryCache.putThumb(id, size, decoded)
            bmp = decoded
        }
    }
    val image = bmp
    if (image != null) {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)),
            contentScale = ContentScale.Crop,
        )
    } else if (audio) {
        MusicNote(Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)))
    } else {
        Box(
            Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)).background(Mvp.card),
            contentAlignment = Alignment.Center,
        ) { CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp) }
    }
}

@Composable
private fun MusicNote(modifier: Modifier) {
    Box(modifier.background(Color(0xFF1C1C1E)), contentAlignment = Alignment.Center) {
        Text("♪", color = Mvp.accent, fontSize = 16.sp)
    }
}

/** Square crop that fills an Instagram-style grid cell. */
@Composable
private fun SquareThumb(id: Long, audio: Boolean = false, image: Boolean = false) {
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, 480)) }
    LaunchedEffect(id) {
        if (bmp != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            if (image && !RemoteVault.connected()) {
                decodeVaultImage(id, 480)
            } else {
                val jpeg = if (RemoteVault.connected()) {
                    RemoteVault.getBytes("/v1/thumbnail?id=$id&max=480")
                } else {
                    CoreBridge.nativeThumbnail(id, 480)
                }
                jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
            }
        }
        if (decoded != null) {
            GalleryCache.putThumb(id, 480, decoded)
            bmp = decoded
        }
    }
    val image = bmp
    if (image != null) {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.fillMaxSize(),
            contentScale = ContentScale.Crop,
        )
    } else if (audio) {
        MusicNote(Modifier.fillMaxSize())
    } else {
        Box(Modifier.fillMaxSize().background(Mvp.card), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
        }
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
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
) {
    LazyColumn(
        state = GalleryCache.detailsScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        item {
            Row(
                Modifier.fillMaxWidth().background(Mvp.headerBg).padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                listOf(
                    SortKey.Name to 2.2f,
                    SortKey.Size to 1f,
                    SortKey.Duration to 1f,
                    SortKey.Resolution to 1f,
                    SortKey.Codec to 1f,
                    SortKey.Tags to 1f,
                    SortKey.Imported to 1f,
                ).forEach { (key, weight) ->
                    val active = GalleryCache.sortKey == key
                    val mark = if (!active) "" else if (GalleryCache.sortAscending) " ↑" else " ↓"
                    Text(
                        key.label + mark,
                        color = if (active) Mvp.accent else Mvp.headerText,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(weight).clickable { applySort(key) },
                    )
                }
                Spacer(Modifier.width(40.dp))
            }
        }
        if (upParentId != null) {
            item(key = "up") {
                FolderDetailRow("..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            FolderDetailRow(
                folder.name,
                "Folder",
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
            )
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
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    )
                    .padding(horizontal = 10.dp, vertical = 7.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Thumb(v.id, 44, audio = isAudioName(v.name), image = isImageName(v.name))
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
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
) {
    LazyVerticalGrid(
        columns = GridCells.Fixed(3),
        state = GalleryCache.gridScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
        horizontalArrangement = Arrangement.spacedBy(1.dp),
        verticalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        item(span = { GridItemSpan(3) }) { ListSortBar() }
        if (upParentId != null) {
            item(key = "up") {
                FolderTile("..", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            FolderTile(
                folder.name,
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
            )
        }
        items(videos, key = { it.id }) { v ->
            val media = rememberMediaInfo(v.id)
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(1f)
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    ),
            ) {
                SquareThumb(v.id, audio = isAudioName(v.name), image = isImageName(v.name))
                if (isAudioName(v.name)) {
                    val parsed = songLabel(v.name)
                    val artist = media?.artist?.takeIf { it.isNotBlank() } ?: parsed.first
                    val title = media?.title?.takeIf { it.isNotBlank() } ?: parsed.second
                    Box(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .height(48.dp)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Transparent, Color.Black.copy(alpha = 0.78f)),
                                ),
                            ),
                    )
                    Column(
                        Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth()
                            .padding(start = 6.dp, end = 6.dp, bottom = 5.dp),
                    ) {
                        Text(
                            title,
                            color = Color.White,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        if (artist.isNotBlank()) {
                            Text(
                                artist,
                                color = Color(0xFFDCDCE0),
                                fontSize = 11.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
                val duration = media?.durationText
                if (!isAudioName(v.name) && duration != null && duration != "…" && media.durationMs > 0L) {
                    Box(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .height(36.dp)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Transparent, Color.Black.copy(alpha = 0.72f)),
                                ),
                            ),
                    )
                    Text(
                        duration,
                        color = Color.White,
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold,
                        style = TextStyle(
                            shadow = Shadow(Color.Black, Offset(0f, 1f), 3f),
                        ),
                        modifier = Modifier
                            .align(Alignment.BottomStart)
                            .padding(start = 6.dp, bottom = 5.dp),
                    )
                }
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
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
) {
    LazyColumn(
        state = GalleryCache.listScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        item { ListSortBar() }
        if (upParentId != null) {
            item(key = "up") {
                FolderListRow("..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            FolderListRow(
                folder.name,
                "Folder",
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
            )
        }
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
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    )
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Thumb(v.id, 36, audio = isAudioName(v.name), image = isImageName(v.name))
                Column(Modifier.weight(1f).padding(start = 10.dp)) {
                    Text(v.name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${v.sizeText} · ${v.tags.joinToString(", ")}", color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                RowActions { onAction(v) }
            }
        }
    }
}

@Composable
private fun ListSortBar() {
    var open by remember { mutableStateOf(false) }
    val key = GalleryCache.sortKey
    val arrow = if (GalleryCache.sortAscending) "↑" else "↓"
    Box(Modifier.fillMaxWidth()) {
        Row(
            Modifier
                .fillMaxWidth()
                .clickable { open = true }
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Sort", color = Mvp.description, fontSize = 12.sp)
            Text("$arrow  ${key.label}", color = Mvp.accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            SortKey.entries.forEach { item ->
                val mark = if (item != key) "" else if (GalleryCache.sortAscending) "  ↑" else "  ↓"
                DropdownMenuItem(
                    text = {
                        Text(
                            item.label + mark,
                            color = if (item == key) Mvp.accent else Mvp.text,
                            fontSize = 14.sp,
                        )
                    },
                    onClick = {
                        open = false
                        applySort(item)
                    },
                )
            }
        }
    }
}
