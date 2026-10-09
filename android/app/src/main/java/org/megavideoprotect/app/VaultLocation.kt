package org.megavideoprotect.app

import android.content.Context

/**
 * Where a vault may live.
 *
 * The core works on real filesystem paths, so a Storage-Access-Framework tree
 * URI is not usable directly. These are the real, app-writable roots Android
 * offers without any storage permission: the private sandbox (the default) and
 * the app's per-volume external directories — internal shared storage plus, on
 * phones that have one, the SD card. Picking an external root makes the vault
 * visible over USB (`Android/data/<pkg>/files`) for backups, while the private
 * sandbox stays the recommended default.
 *
 * The choice is remembered so the unlock screen opens the same vault next time.
 */
object VaultLocation {
    private const val PREFS = "mvp_prefs"
    private const val KEY_PATH = "vault_path"

    fun options(context: Context): List<String> {
        val paths = ArrayList<String>()
        paths.add(context.filesDir.absolutePath)
        // getExternalFilesDirs returns one entry per mounted volume; entries can
        // be null when a volume is unavailable (e.g. an ejected SD card).
        context.getExternalFilesDirs(null).forEach { dir ->
            if (dir != null) paths.add(dir.absolutePath)
        }
        return paths.distinct()
    }

    fun saved(context: Context): String? =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_PATH, null)

    fun save(context: Context, path: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY_PATH, path).apply()
    }

    private const val KEY_IDLE = "idle_lock_ms"
    /** Leaving the app locks it only after this long. Zero means never. */
    const val DEFAULT_IDLE_MS = 60L * 60L * 1000L
    val idleChoices: List<Pair<Long, String>> = listOf(
        0L to "Never",
        15L * 60L * 1000L to "15 minutes",
        DEFAULT_IDLE_MS to "1 hour",
        4L * 60L * 60L * 1000L to "4 hours",
    )

    fun idleLockMs(context: Context): Long =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getLong(KEY_IDLE, DEFAULT_IDLE_MS)

    fun idleLockLabel(context: Context): String {
        val ms = idleLockMs(context)
        return idleChoices.firstOrNull { it.first == ms }?.second ?: "1 hour"
    }

    fun saveIdleLock(context: Context, ms: Long) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putLong(KEY_IDLE, ms).apply()
    }

    /** A plain name for the unlock screen. The raw path stays off that page. */
    fun shortName(context: Context, path: String): String = when {
        path == context.filesDir.absolutePath -> "App-private (recommended)"
        path.contains("/Android/data/") && path.contains("emulated/0") -> "Shared storage"
        path.contains("/Android/data/") -> "SD card"
        else -> java.io.File(path).name.ifBlank { "Chosen folder" }
    }
    fun label(context: Context, path: String): String = when {
        path == context.filesDir.absolutePath -> "App-private (recommended)"
        path.contains("/Android/data/") ->
            "Shared storage " + path.substringBefore("/Android/data/")
        else -> path
    }
}
