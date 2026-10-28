package dev.omalauncher.bridge

import android.content.Context
import android.content.pm.LauncherApps
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.AdaptiveIconDrawable
import android.graphics.drawable.Drawable
import android.os.Build
import android.util.DisplayMetrics
import android.util.Log
import dev.omalauncher.AndroidApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * Module 1: Native Android Apps Bridge (No ADB needed).
 * Exports the live catalog of installed Android applications to android-apps.json
 * in the app's files directory and PRoot mounts (/run/user/0, ~/.local/share/omarchy).
 *
 * Each app's icon goes along as PNGs in ~/.local/share/omarchy/android-icons/<id>/, so Linux
 * scripts can restyle icons without adb: `icon.png` (the icon as Android draws it) and, for
 * adaptive icons, its `fg.png`, `bg.png` and `mono.png` layers, each the full 108dp layer at
 * [ICON_PX]. An icon is drawn again only when its app is installed or updated.
 */
object AppWatcher {
    private const val TAG = "AppWatcher"
    private const val FILE_NAME = "android-apps.json"
    private const val ICONS_DIR = "android-icons"
    /** Where [ICONS_DIR] is seen from Linux: context.filesDir/omarchy is ~/.local/share/omarchy. */
    private const val LINUX_ICONS = "/root/.local/share/omarchy/$ICONS_DIR"
    /** Icon and layer edge in px: 108dp at 4x. */
    private const val ICON_PX = 432

    /** Both the app list's callback and the runtime's start export; one at a time. */
    private val lock = Mutex()

    suspend fun export(context: Context, apps: List<AndroidApp>) = withContext(Dispatchers.IO) {
        lock.withLock {
            try {
                val omarchyDir = File(context.filesDir, "omarchy")
                val icons = if (omarchyDir.isDirectory) exportIcons(context, File(omarchyDir, ICONS_DIR), apps) else emptyMap()
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
                    icons[iconId(app)]?.let { files ->
                        files["icon"]?.let { obj.put("icon", "$LINUX_ICONS/${iconId(app)}/$it") }
                        val layers = JSONObject()
                        for (layer in LAYERS) files[layer]?.let { layers.put(layer, "$LINUX_ICONS/${iconId(app)}/$it") }
                        if (layers.length() > 0) obj.put("layers", layers)
                    }
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
                if (omarchyDir.exists() && omarchyDir.isDirectory) {
                    writeFile(File(omarchyDir, FILE_NAME), content)
                }
            } catch (e: Exception) {
                Log.w(TAG, "Failed to export android apps", e)
            }
        }
    }

    private val LAYERS = listOf("fg", "bg", "mono")

    /** One folder per app and profile: the package, or `package@<profile serial>` beside the owner's. */
    private fun iconId(app: AndroidApp) =
        app.component.packageName + (app.profile?.let { "@$it" } ?: "")

    /**
     * Draws the icons of [apps] into [root] where they're missing or their app changed, drops those
     * of apps that are gone, and returns each app's files by kind (icon, fg, bg, mono).
     */
    private fun exportIcons(context: Context, root: File, apps: List<AndroidApp>): Map<String, Map<String, String>> {
        val launcherApps = context.getSystemService(LauncherApps::class.java) ?: return emptyMap()
        root.mkdirs()
        val out = HashMap<String, Map<String, String>>()
        for (app in apps) {
            val id = iconId(app)
            val dir = File(root, id)
            try {
                val info = launcherApps.getActivityList(app.component.packageName, app.user)
                    .firstOrNull { it.componentName == app.component } ?: continue
                // The APK's path changes with every install and update, so it says when to draw again.
                val stamp = "${info.applicationInfo.sourceDir}|${app.component.className}"
                val stampFile = File(dir, "stamp")
                if (!(stampFile.exists() && stampFile.readText() == stamp)) {
                    dir.mkdirs()
                    dir.listFiles()?.forEach { it.delete() }
                    val drawable = info.getIcon(DisplayMetrics.DENSITY_XXXHIGH) ?: continue
                    writePng(File(dir, "icon.png"), drawable)
                    if (drawable is AdaptiveIconDrawable) {
                        drawable.foreground?.let { writePng(File(dir, "fg.png"), it) }
                        drawable.background?.let { writePng(File(dir, "bg.png"), it) }
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            drawable.monochrome?.let { writePng(File(dir, "mono.png"), it) }
                        }
                    }
                    writeFile(stampFile, stamp)
                }
                out[id] = (listOf("icon") + LAYERS).filter { File(dir, "$it.png").exists() }.associateWith { "$it.png" }
            } catch (e: Exception) {
                Log.w(TAG, "cannot export the icon of $id", e)
            }
        }
        // Apps that were uninstalled
        root.listFiles()?.forEach { if (it.isDirectory && it.name !in out) it.deleteRecursively() }
        return out
    }

    /** [drawable] drawn over the whole [ICON_PX] square: an adaptive layer's full 108dp, or a plain icon. */
    private fun writePng(dest: File, drawable: Drawable) {
        val bitmap = Bitmap.createBitmap(ICON_PX, ICON_PX, Bitmap.Config.ARGB_8888)
        val d = drawable.mutate()
        d.setBounds(0, 0, ICON_PX, ICON_PX)
        d.draw(Canvas(bitmap))
        val tmp = File(dest.parentFile, "${dest.name}.tmp")
        FileOutputStream(tmp).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
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
