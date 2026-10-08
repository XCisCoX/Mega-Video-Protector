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

    /** Short label for the picker: sandbox vs. per-volume shared storage. */
    fun label(context: Context, path: String): String = when {
        path == context.filesDir.absolutePath -> "App-private (recommended)"
        path.contains("/Android/data/") ->
            "Shared storage " + path.substringBefore("/Android/data/")
        else -> path
    }
}
