package org.megavideoprotect.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import java.io.File

/**
 * Picking a real folder (Download, Movies, an SD card, …) as the vault location.
 *
 * Android 11+ denies *path*-based access outside the app's own directories unless
 * the app holds "All files access" (MANAGE_EXTERNAL_STORAGE). A Storage Access
 * Framework tree URI cannot substitute: the core addresses the vault with real
 * filesystem paths — SQLCipher opens the database by path and the package reader
 * does its own file I/O — so an fd/URI-based vault would need a custom SQLite VFS
 * and a rewritten package layer.
 *
 * So: browse the real filesystem, and ask for that permission first when missing.
 */
object FolderPicker {
    /** True when path-based access to shared storage is allowed. */
    fun hasAccess(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    /** The settings page where the user grants "All files access". */
    fun settingsIntent(context: Context): Intent =
        Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
            .setData(Uri.parse("package:${context.packageName}"))

    /** Starting points, best first. */
    fun roots(): List<File> = listOfNotNull(
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
        Environment.getExternalStorageDirectory(),
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES),
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM),
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
    ).filter { it.isDirectory }.distinct()

    fun children(dir: File): List<File> =
        (dir.listFiles() ?: emptyArray())
            .filter { it.isDirectory && !it.name.startsWith(".") }
            .sortedBy { it.name.lowercase() }
}

/** Folder browser: tap to descend, ".." to go up, then "Use this folder". */
@Composable
fun FolderPickerDialog(
    onDismiss: () -> Unit,
    onPicked: (String) -> Unit,
) {
    val context = LocalContext.current
    var dir by remember { mutableStateOf(FolderPicker.roots().firstOrNull()) }
    var granted by remember { mutableStateOf(FolderPicker.hasAccess()) }

    Dialog(onDismissRequest = onDismiss) {
        MvpCard(Modifier.fillMaxWidth()) {
            MvpTitle("Choose vault folder")
            MvpDescription(dir?.absolutePath ?: "No storage is accessible yet")

            if (!granted) {
                MvpDescription(
                    "Android only lets the app use a normal folder such as Download when it " +
                        "holds \"All files access\". Grant it, then come back and pick the folder."
                )
                MvpButton(
                    "Open permission settings",
                    onClick = {
                        runCatching { context.startActivity(FolderPicker.settingsIntent(context)) }
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Granted — reload",
                    onClick = {
                        granted = FolderPicker.hasAccess()
                        dir = FolderPicker.roots().firstOrNull()
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
            } else {
                val current = dir
                Column(Modifier.fillMaxWidth().heightIn(max = 300.dp).verticalScroll(rememberScrollState())) {
                    val parent = current?.parentFile
                    if (current != null && parent != null && parent.canRead()) {
                        Row(
                            Modifier.fillMaxWidth().clickable { dir = parent }.padding(vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) { Text(".. (up)", color = Mvp.accent, fontSize = 13.sp) }
                    }
                    if (current != null) {
                        val subdirs = FolderPicker.children(current)
                        if (subdirs.isEmpty()) {
                            Text("(no sub-folders)", color = Mvp.description, fontSize = 12.sp)
                        }
                        subdirs.forEach { child ->
                            Text(
                                child.name,
                                color = Mvp.text,
                                fontSize = 13.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable { dir = child }
                                    .padding(vertical = 8.dp),
                            )
                        }
                    }
                }
                current?.let {
                    MvpButton(
                        "Use this folder",
                        onClick = { onPicked(it.absolutePath) },
                        primary = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
            MvpButton("Cancel", onClick = onDismiss, modifier = Modifier.fillMaxWidth())
        }
    }
}
