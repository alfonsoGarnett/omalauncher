package dev.omalauncher.bridge

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.TrafficStats
import android.os.BatteryManager
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * Module 5: System & Network Metrics Bridge (No ADB needed).
 * Periodically exports network traffic, battery status and memory stats to
 * android-status.json in context.filesDir and PRoot mounts (/run/user/0).
 */
class MetricsWatcher(
    private val context: Context,
    private val scope: CoroutineScope,
) {
    private var job: Job? = null
    private var isScreenActive = true

    fun start() {
        if (job != null) return
        job = scope.launch(Dispatchers.IO) {
            while (isActive) {
                if (isScreenActive) {
                    try {
                        exportMetrics()
                    } catch (e: Exception) {
                        Log.w(TAG, "Error gathering metrics", e)
                    }
                }
                delay(2500)
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
    }

    fun screenStarted() {
        isScreenActive = true
    }

    fun screenStopped() {
        isScreenActive = false
    }

    private fun exportMetrics() {
        val root = JSONObject()
        root.put("timestamp", System.currentTimeMillis() / 1000)

        // 1. Network Traffic
        val net = JSONObject()
        val rxTotal = TrafficStats.getTotalRxBytes()
        val txTotal = TrafficStats.getTotalTxBytes()
        val rxMobile = TrafficStats.getMobileRxBytes()
        val txMobile = TrafficStats.getMobileTxBytes()
        net.put("rx_bytes", if (rxTotal != TrafficStats.UNSUPPORTED.toLong()) rxTotal else 0L)
        net.put("tx_bytes", if (txTotal != TrafficStats.UNSUPPORTED.toLong()) txTotal else 0L)
        net.put("rx_mobile_bytes", if (rxMobile != TrafficStats.UNSUPPORTED.toLong()) rxMobile else 0L)
        net.put("tx_mobile_bytes", if (txMobile != TrafficStats.UNSUPPORTED.toLong()) txMobile else 0L)
        root.put("network", net)

        // 2. Battery Status
        val batteryIntent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (batteryIntent != null) {
            val bat = JSONObject()
            val level = batteryIntent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = batteryIntent.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
            val pct = if (level >= 0 && scale > 0) (level * 100) / scale else level
            bat.put("level", pct)

            val status = when (batteryIntent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)) {
                BatteryManager.BATTERY_STATUS_CHARGING -> "charging"
                BatteryManager.BATTERY_STATUS_DISCHARGING -> "discharging"
                BatteryManager.BATTERY_STATUS_FULL -> "full"
                BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "not_charging"
                else -> "unknown"
            }
            bat.put("status", status)

            val plugged = when (batteryIntent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)) {
                BatteryManager.BATTERY_PLUGGED_AC -> "ac"
                BatteryManager.BATTERY_PLUGGED_USB -> "usb"
                BatteryManager.BATTERY_PLUGGED_WIRELESS -> "wireless"
                else -> "unplugged"
            }
            bat.put("plugged", plugged)

            val temp = batteryIntent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0)
            bat.put("temperature", temp / 10.0)

            val voltage = batteryIntent.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0)
            bat.put("voltage_mv", voltage)

            root.put("battery", bat)
        }

        // 3. Memory Stats
        val actMgr = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        if (actMgr != null) {
            val memInfo = ActivityManager.MemoryInfo()
            actMgr.getMemoryInfo(memInfo)
            val mem = JSONObject()
            mem.put("avail_bytes", memInfo.availMem)
            mem.put("total_bytes", memInfo.totalMem)
            mem.put("low_memory", memInfo.lowMemory)
            root.put("memory", mem)
        }

        val jsonStr = root.toString(2)

        // Write destinations
        writeFile(File(context.filesDir, FILE_NAME), jsonStr)

        val xdgDir = File(context.filesDir, "xdg")
        if (xdgDir.exists() && xdgDir.isDirectory) {
            writeFile(File(xdgDir, FILE_NAME), jsonStr)
        }

        val omarchyDir = File(context.filesDir, "omarchy")
        if (omarchyDir.exists() && omarchyDir.isDirectory) {
            writeFile(File(omarchyDir, FILE_NAME), jsonStr)
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

    companion object {
        private const val TAG = "MetricsWatcher"
        private const val FILE_NAME = "android-status.json"
    }
}
