package org.megavideoprotect.app

import org.json.JSONArray
import org.json.JSONObject

/** One gallery row, mirroring the desktop's VideoInfo + MediaInfo. */
data class VideoEntry(
    val id: Long,
    val name: String,
    val size: Long,
    val durationMs: Long,
    val width: Int,
    val height: Int,
    val codec: String,
    val tags: List<String>,
    val importedAt: Long,
) {
    val resolution: String get() = if (width > 0 && height > 0) "${width}×${height}" else "—"
    val durationText: String get() {
        if (durationMs <= 0) return "—"
        val totalSec = durationMs / 1000
        return "%d:%02d".format(totalSec / 60, totalSec % 60)
    }
    val sizeText: String get() {
        if (size <= 0) return "—"
        return when {
            size >= 1_000_000_000 -> "%.2f GB".format(size / 1e9)
            size >= 1_000_000 -> "%.1f MB".format(size / 1e6)
            size >= 1_000 -> "%.0f KB".format(size / 1e3)
            else -> "$size B"
        }
    }
    val importedText: String get() {
        if (importedAt <= 0) return "—"
        // The vault stores imported_at as seconds since the epoch; Date wants ms.
        val d = java.util.Date(importedAt * 1000L)
        val fmt = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", java.util.Locale.US)
        return fmt.format(d)
    }
}

data class TagEntry(val id: Long, val name: String, val videoCount: Long)

object Json {
    fun videos(raw: String): List<VideoEntry> {
        val out = ArrayList<VideoEntry>()
        val arr = JSONArray(raw)
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            val tags = ArrayList<String>()
            val t = o.optJSONArray("tags") ?: JSONArray()
            for (j in 0 until t.length()) tags.add(t.getString(j))
            out.add(
                VideoEntry(
                    id = o.getLong("id"),
                    name = o.getString("name"),
                    size = o.optLong("size", 0),
                    durationMs = o.optLong("durationMs", 0),
                    width = o.optInt("width", 0),
                    height = o.optInt("height", 0),
                    codec = o.optString("codec", ""),
                    tags = tags,
                    importedAt = o.optLong("importedAt", 0),
                )
            )
        }
        return out
    }

    fun tags(raw: String): List<TagEntry> {
        val out = ArrayList<TagEntry>()
        val arr = JSONArray(raw)
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            out.add(TagEntry(o.getLong("id"), o.getString("name"), o.optLong("count", 0)))
        }
        return out
    }
}

sealed interface Screen {
    data object Setup : Screen
    data object Login : Screen
    data object Vault : Screen
    data class Player(val videoId: Long, val name: String) : Screen
    /** A still image: shown in the image viewer, not the media player. */
    data class Image(val videoId: Long, val name: String) : Screen
}
