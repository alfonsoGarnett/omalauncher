package dev.omalauncher.bridge

import android.content.Context
import android.util.Log
import dev.omalauncher.AndroidApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * Module 1: Native Android Apps Bridge (No ADB needed).
 * Exports the live catalog of installed Android applications to android-apps.json
 * in the app's files directory and PRoot mounts (/run/user/0, ~/.local/share/omarchy).
 */
object AppWatcher {
    private const val TAG = "AppWatcher"
    private const val FILE_NAME = "android-apps.json"

    suspend fun export(context: Context, apps: List<AndroidApp>) = withContext(Dispatchers.IO) {
        try {
            val jsonArray = JSONArray()
            for (app in apps) {
                val obj = JSONObject()
                obj.put("name", app.label)
                obj.put("package", app.component.packageName)
                obj.put("activity", app.component.className)
                if (app.profile != null) {
                    obj.put("profile", app.profile)
                }
                obj.put("system", app.system)
                jsonArray.put(obj)
            }
            val content = jsonArray.toString(2)

            // 1. Write to context.filesDir/android-apps.json
            writeFile(File(context.filesDir, FILE_NAME), content)

            // 2. Write to context.filesDir/xdg/android-apps.json (/run/user/0 in PRoot)
            val xdgDir = File(context.filesDir, "xdg")
            if (xdgDir.exists() && xdgDir.isDirectory) {
                writeFile(File(xdgDir, FILE_NAME), content)
            }

            // 3. Write to context.filesDir/omarchy/android-apps.json (/root/.local/share/omarchy)
            val omarchyDir = File(context.filesDir, "omarchy")
            if (omarchyDir.exists() && omarchyDir.isDirectory) {
                writeFile(File(omarchyDir, FILE_NAME), content)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to export android apps", e)
        }
    }

    private fun writeFile(dest: File, content: String) {
        dest.parentFile?.mkdirs()
        val tmp = File(dest.parentFile, "${dest.name}.tmp")
        FileOutputStream(tmp).use { it.write(content.toByteArray(Charsets.UTF_8)) }
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
        }
    }
}
