package com.sf2live.app

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

class AudioDeviceMonitor(
    context: Context,
    private val listener: Listener,
) {
    data class DeviceOption(
        val id: Int,
        val name: String,
        val type: Int,
        val sampleRates: IntArray,
        val channelCounts: IntArray,
    ) {
        val typeLabel: String
            get() = when (type) {
                AudioDeviceInfo.TYPE_USB_DEVICE -> "USB"
                AudioDeviceInfo.TYPE_USB_HEADSET -> "USB headset"
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Fone com fio"
                AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Headset com fio"
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Alto-falante"
                AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth A2DP"
                AudioDeviceInfo.TYPE_BLE_HEADSET -> "Bluetooth LE"
                AudioDeviceInfo.TYPE_HDMI -> "HDMI"
                else -> "Saída de áudio"
            }

        val displayLabel: String
            get() = "$name · $typeLabel"
    }

    interface Listener {
        fun onAudioDevicesChanged(devices: List<DeviceOption>)
    }

    private val audioManager = context.applicationContext.getSystemService(AudioManager::class.java)
    private val handler = Handler(Looper.getMainLooper())

    private val callback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) = refresh()
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) = refresh()
    }

    fun start() {
        audioManager?.registerAudioDeviceCallback(callback, handler)
        refresh()
    }

    fun stop() {
        try {
            audioManager?.unregisterAudioDeviceCallback(callback)
        } catch (_: Exception) {
        }
    }

    fun refresh() {
        val devices = audioManager
            ?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            ?.filter { it.isSink }
            ?.map {
                DeviceOption(
                    id = it.id,
                    name = it.productName?.toString()?.takeIf { name -> name.isNotBlank() }
                        ?: fallbackName(it.type),
                    type = it.type,
                    sampleRates = it.sampleRates,
                    channelCounts = it.channelCounts,
                )
            }
            ?.sortedWith(compareBy<DeviceOption> { priority(it.type) }.thenBy { it.name.lowercase() })
            .orEmpty()
        listener.onAudioDevicesChanged(devices)
    }

    private fun priority(type: Int): Int = when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> 0
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> 1
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> 2
        else -> 3
    }

    private fun fallbackName(type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "Interface USB"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Alto-falante do celular"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "Fone com fio"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Headset com fio"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth"
        else -> "Dispositivo de áudio"
    }
}
