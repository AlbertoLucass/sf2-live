package com.sf2live.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.PowerManager

/**
 * Owns Android resources that keep a live instrument session predictable.
 * It deliberately contains no UI and no synthesizer logic.
 */
class RealtimePerformanceSession(context: Context) : AutoCloseable {
    private val appContext = context.applicationContext
    private val powerManager = appContext.getSystemService(PowerManager::class.java)
    private val audioManager = appContext.getSystemService(AudioManager::class.java)

    private val wakeLock = powerManager.newWakeLock(
        PowerManager.PARTIAL_WAKE_LOCK,
        "SF2Live:RealtimeAudio"
    ).apply {
        setReferenceCounted(false)
    }

    private val audioAttributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    private val focusRequest: AudioFocusRequest? = if (Build.VERSION.SDK_INT >= 26) {
        AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(audioAttributes)
            .setAcceptsDelayedFocusGain(false)
            .setOnAudioFocusChangeListener { /* Stream lifecycle remains native. */ }
            .build()
    } else null

    private var started = false

    fun start() {
        if (started) return
        started = true
        if (!wakeLock.isHeld) wakeLock.acquire()
        focusRequest?.let(audioManager::requestAudioFocus)
    }

    override fun close() {
        if (!started) return
        started = false
        focusRequest?.let(audioManager::abandonAudioFocusRequest)
        if (wakeLock.isHeld) wakeLock.release()
    }
}
