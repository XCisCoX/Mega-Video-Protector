package org.megavideoprotect.app

import org.json.JSONObject

/**
 * JNI bridge to the C++ VideoVaultCore (libmvpcore.so). One vault is open at a
 * time (like the desktop app). All calls marshal results as JSON; errors carry
 * the user-facing code from user_message() plus a technical detail.
 */
object CoreBridge {
    init {
        System.loadLibrary("mvpcore")
    }

    external fun nativeVaultExists(root: String): Boolean
    external fun nativeCreateVault(root: String, password: String, memKib: Int, iterations: Int, parallelism: Int): String
    external fun nativeOpenVault(root: String, password: String): String
    external fun nativeIsUnlocked(): Boolean
    external fun nativeLock(): String
    external fun nativeListVideos(): String
    external fun nativeImportFile(path: String): String
    external fun nativeRemoveVideo(id: Long): String
    external fun nativeThumbnail(id: Long, maxDimension: Int): ByteArray?
    external fun nativeMediaInfo(id: Long): String
    external fun nativeRestoreVideo(id: Long, targetDir: String): String
    external fun nativeChangePassword(current: String, newPassword: String, memKib: Int, iterations: Int, parallelism: Int): String
    external fun nativeListTags(): String
    external fun nativeAddTag(videoId: Long, name: String): String
    external fun nativeRemoveTag(videoId: Long, tagId: Long): String
    external fun nativeDeleteTag(tagId: Long): String
    external fun nativeDecodeFrame(id: Long, positionMs: Long, maxDimension: Int): ByteArray?
    external fun nativeCreateTag(name: String): String
    external fun nativeVideoSize(id: Long): Long
    external fun nativeReadRange(id: Long, offset: Long, size: Int): ByteArray?
    /** Short clips only. Null means the video is longer than about 1.5s, so ExoPlayer should play it. */
    external fun nativeLoopClip(id: Long, maxDimension: Int): ByteArray?
    external fun nativeClose()

    class Result(val ok: Boolean, val error: String, val detail: String) {
        companion object {
            fun parse(raw: String): Result {
                val o = JSONObject(raw)
                return Result(
                    ok = o.optBoolean("ok", false),
                    error = o.optString("error", ""),
                    detail = o.optString("detail", ""),
                )
            }
        }
    }
}
