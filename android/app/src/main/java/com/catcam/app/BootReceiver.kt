package com.catcam.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat

// READY at boot: the tablet is reachable by the PC with nobody touching it.
// BOOT_COMPLETED is one of the allowed background starts for a foreground
// service (connectedDevice type; the camera types come later, from the
// foreground, when the PC or the user actually asks for the camera).
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        // MY_PACKAGE_REPLACED: an app update kills the process and a sticky
        // service does not come back on its own; re-arm so an update never
        // leaves the tablet unreachable.
        if (intent.action != Intent.ACTION_BOOT_COMPLETED
            && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        ContextCompat.startForegroundService(context,
            Intent(context, StreamerService::class.java).setAction(StreamerService.ACTION_ARM))
    }
}
