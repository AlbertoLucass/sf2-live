package com.sf2live.app

object NativeSynth {
    init {
        System.loadLibrary("sf2live")
    }

    external fun nativeInitialize(): Boolean
    external fun nativeShutdown()
    external fun nativeLoadSoundFont(path: String): String
    external fun nativeUnloadSoundFont(restartAudio: Boolean)
    external fun nativeGetPresetNames(): Array<String>
    external fun nativeSelectPreset(index: Int)
    external fun nativeNoteOn(channel: Int, note: Int, velocity: Int)
    external fun nativeNoteOff(channel: Int, note: Int)
    external fun nativeControlChange(channel: Int, controller: Int, value: Int)
    external fun nativePitchBend(channel: Int, value14: Int)
    external fun nativeProgramChange(channel: Int, program: Int)
    external fun nativePanic()
    external fun nativeTestTone()
    external fun nativeSetMasterVolume(value: Float)
    external fun nativeSetReleaseMilliseconds(value: Float)
    external fun nativeSetLatencyProfile(profile: Int)
    external fun nativeSetOutputDevice(deviceId: Int)
    external fun nativeConfigureOutput(deviceId: Int, profile: Int, optimized: Boolean, sampleRate: Int, dataFormat: Int)
    external fun nativeConnectMidi(device: android.media.midi.MidiDevice, portNumber: Int): Boolean
    external fun nativeDisconnectMidi()
    external fun nativeGetStatusJson(): String
}
