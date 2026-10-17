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
    private var lastRxTotal = 0L
    private var lastTxTotal = 0L
    private var lastSampleTimeMs = 0L

    fun start() {
        if (job != null) return
        job = scope.launch(Dispatchers.IO) {
            while (isActive) {
                try {
                    exportMetrics()
                } catch (e: Exception) {
                    Log.w(TAG, "Error gathering metrics", e)
                }
                delay(1500)
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

    private fun formatSpeed(bytesPerSec: Long): String {
        return when {
            bytesPerSec >= 1024 * 1024 -> "%.1f MB/s".format(java.util.Locale.ROOT, bytesPerSec / (1024.0 * 1024.0))
            bytesPerSec >= 1024 -> "%.1f KB/s".format(java.util.Locale.ROOT, bytesPerSec / 1024.0)
            else -> "$bytesPerSec B/s"
        }
    }

    private fun exportMetrics() {
        val root = JSONObject()
        root.put("timestamp", System.currentTimeMillis() / 1000)

        // 1. Network Traffic & Speeds
        val net = JSONObject()
        val rxTotal = TrafficStats.getTotalRxBytes()
        val txTotal = TrafficStats.getTotalTxBytes()
        val rx = if (rxTotal != TrafficStats.UNSUPPORTED.toLong()) rxTotal else 0L
        val tx = if (txTotal != TrafficStats.UNSUPPORTED.toLong()) txTotal else 0L
        val nowMs = android.os.SystemClock.elapsedRealtime()

        var rxRate = 0L
        var txRate = 0L
        if (lastSampleTimeMs > 0 && nowMs > lastSampleTimeMs) {
            val dt = (nowMs - lastSampleTimeMs) / 1000.0
            if (dt > 0.1 && rx >= lastRxTotal && tx >= lastTxTotal) {
                rxRate = ((rx - lastRxTotal) / dt).toLong()
                txRate = ((tx - lastTxTotal) / dt).toLong()
            }
        }
        lastRxTotal = rx
        lastTxTotal = tx
        lastSampleTimeMs = nowMs

        val rxMobile = TrafficStats.getMobileRxBytes()
        val txMobile = TrafficStats.getMobileTxBytes()
        net.put("rx_bytes", rx)
        net.put("tx_bytes", tx)
        net.put("rx_rate_bytes_per_sec", rxRate)
        net.put("tx_rate_bytes_per_sec", txRate)
        net.put("rx_speed_human", formatSpeed(rxRate))
        net.put("tx_speed_human", formatSpeed(txRate))
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
