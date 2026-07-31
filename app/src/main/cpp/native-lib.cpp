#include "AudioEngine.h"

#include <jni.h>
#include <string>

using sf2live::AudioCommand;
using sf2live::AudioEngine;
using sf2live::CommandType;

namespace {
std::string fromJString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sf2live_app_NativeSynth_nativeInitialize(JNIEnv*, jobject) {
    return AudioEngine::instance().initialize() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeShutdown(JNIEnv*, jobject) {
    AudioEngine::instance().shutdown();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_sf2live_app_NativeSynth_nativeLoadSoundFont(JNIEnv* env, jobject, jstring path) {
    std::string error;
    const int result = AudioEngine::instance().loadSoundFont(fromJString(env, path), error);
    if (result < 0) {
        return toJString(env, std::string("ERR|") + (error.empty() ? "Falha desconhecida ao carregar o SF2." : error));
    }
    return toJString(env, std::string("OK|") + std::to_string(result));
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeUnloadSoundFont(JNIEnv*, jobject, jboolean restartAudio) {
    AudioEngine::instance().unloadSoundFont(restartAudio == JNI_TRUE);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_sf2live_app_NativeSynth_nativeGetPresetNames(JNIEnv* env, jobject) {
    const auto presets = AudioEngine::instance().presetInfos();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(presets.size()), stringClass, nullptr);
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto& preset = presets[i];
        const std::string label = "B" + std::to_string(preset.bank) + " P" +
            std::to_string(preset.program) + " · " + preset.name;
        jstring value = env->NewStringUTF(label.c_str());
        env->SetObjectArrayElement(array, static_cast<jsize>(i), value);
        env->DeleteLocalRef(value);
    }
    return array;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeSelectPreset(JNIEnv*, jobject, jint index) {
    AudioEngine::instance().selectPreset(index);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeNoteOn(JNIEnv*, jobject, jint channel, jint note, jint velocity) {
    AudioEngine::instance().send({CommandType::NoteOn, channel, note, velocity});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeNoteOff(JNIEnv*, jobject, jint channel, jint note) {
    AudioEngine::instance().send({CommandType::NoteOff, channel, note, 0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeControlChange(JNIEnv*, jobject, jint channel, jint controller, jint value) {
    AudioEngine::instance().send({CommandType::ControlChange, channel, controller, value});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativePitchBend(JNIEnv*, jobject, jint channel, jint value14) {
    AudioEngine::instance().send({CommandType::PitchBend, channel, value14, 0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeProgramChange(JNIEnv*, jobject, jint channel, jint program) {
    AudioEngine::instance().send({CommandType::ProgramChange, channel, program, 0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativePanic(JNIEnv*, jobject) {
    AudioEngine::instance().send({CommandType::Panic, 0, 0, 0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeTestTone(JNIEnv*, jobject) {
    AudioEngine::instance().send({CommandType::TestTone, 0, 0, 0});
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeSetMasterVolume(JNIEnv*, jobject, jfloat value) {
    AudioEngine::instance().setMasterVolume(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeSetReleaseMilliseconds(JNIEnv*, jobject, jfloat value) {
    AudioEngine::instance().setReleaseMilliseconds(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeSetOutputDevice(JNIEnv*, jobject, jint deviceId) {
    AudioEngine::instance().setOutputDevice(deviceId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeSetLatencyProfile(JNIEnv*, jobject, jint profile) {
    AudioEngine::instance().setLatencyProfile(profile);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeConfigureOutput(
    JNIEnv*, jobject, jint deviceId, jint profile, jboolean optimized, jint sampleRate, jint dataFormat) {
    AudioEngine::instance().configureOutput(
        deviceId,
        profile,
        optimized == JNI_TRUE,
        sampleRate,
        dataFormat);
}


extern "C" JNIEXPORT jboolean JNICALL
Java_com_sf2live_app_NativeSynth_nativeConnectMidi(JNIEnv* env, jobject, jobject midiDevice, jint portNumber) {
    return AudioEngine::instance().connectNativeMidi(env, midiDevice, portNumber) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sf2live_app_NativeSynth_nativeDisconnectMidi(JNIEnv*, jobject) {
    AudioEngine::instance().disconnectNativeMidi();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_sf2live_app_NativeSynth_nativeGetStatusJson(JNIEnv* env, jobject) {
    return toJString(env, AudioEngine::instance().statusJson());
}
