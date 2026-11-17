// Android glue (see android_bridge.h). JNI entry points are called by the Java classes in
// android/app/src/main/java/com/xamphy/pes13; the launcher ones run before the game starts.
#include "android_bridge.h"
#include "launcher/launcher_core.h"
#include "debug/process_memory.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex g_touchMutex;
    Video::HostPad g_touch;

    std::mutex g_promptMutex;
    std::function<void(bool, std::u16string)> g_promptDone;

    std::u16string FromJava(JNIEnv* env, jstring s)
    {
        if (!s) return {};
        const jsize n = env->GetStringLength(s);
        std::u16string out(size_t(n), u'\0');
        env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(out.data()));
        return out;
    }

    jstring ToJava(JNIEnv* env, const std::u16string& s)
    {
        return env->NewString(reinterpret_cast<const jchar*>(s.data()), jsize(s.size()));
    }

    jstring ToJavaUtf8(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
}

namespace Video::Android
{
    HostPad GetTouchPad()
    {
        std::lock_guard lock(g_touchMutex);
        return g_touch;
    }

    bool ShowTextPrompt(const std::u16string& title, const std::u16string& message, const std::u16string& text,
        uint32_t maxChars, std::function<void(bool, std::u16string)> done)
    {
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (!env || !activity) return false;
        jclass cls = env->GetObjectClass(activity);
        jmethodID m = env->GetStaticMethodID(cls, "showTextPrompt", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Z");
        if (!m)
        {
            env->ExceptionClear();
            env->DeleteLocalRef(cls);
            env->DeleteLocalRef(activity);
            return false;
        }
        {
            std::lock_guard lock(g_promptMutex);
            g_promptDone = std::move(done);
        }
        jstring t = ToJava(env, title), msg = ToJava(env, message), def = ToJava(env, text);
        const bool shown = env->CallStaticBooleanMethod(cls, m, t, msg, def, jint(maxChars)) == JNI_TRUE;
        env->DeleteLocalRef(t);
        env->DeleteLocalRef(msg);
        env->DeleteLocalRef(def);
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
        if (!shown)
        {
            std::lock_guard lock(g_promptMutex);
            g_promptDone = nullptr;
        }
        return shown;
    }
}

extern "C"
{
    // ---------------------------------------------------------------- PesActivity (in game)
    JNIEXPORT void JNICALL Java_com_xamphy_pes13_PesActivity_nativeSetTouchPad(JNIEnv*, jclass,
        jint buttons, jint lt, jint rt, jint lx, jint ly)
    {
        Video::HostPad p;
        p.buttons = uint16_t(buttons);
        p.lt = uint8_t(lt);
        p.rt = uint8_t(rt);
        p.lx = int16_t(lx);
        p.ly = int16_t(ly);
        std::lock_guard lock(g_touchMutex);
        g_touch = p;
    }

    // The overlay's numbers: fps, CP avg/max ms, slow frames, draws, footprint MB, frame w/h.
    JNIEXPORT jfloatArray JNICALL Java_com_xamphy_pes13_PesActivity_nativePerfStats(JNIEnv* env, jclass)
    {
        const Video::PerfStats p = Video::GetPerfStats();
        const ProcessMemory::Info m = ProcessMemory::Get();
        const jfloat v[] = { jfloat(p.fps), p.cpAvgMs, p.cpMaxMs, jfloat(p.slowFrames), jfloat(p.drawsPerFrame),
            jfloat(m.footprint / 1048576.0), jfloat(p.frameWidth), jfloat(p.frameHeight) };
        jfloatArray out = env->NewFloatArray(jsize(std::size(v)));
        env->SetFloatArrayRegion(out, 0, jsize(std::size(v)), v);
        return out;
    }

    // Java events worth a line in the session log (thermal changes, controllers).
    JNIEXPORT void JNICALL Java_com_xamphy_pes13_PesActivity_nativeLog(JNIEnv* env, jclass, jstring line)
    {
        const char* s = env->GetStringUTFChars(line, nullptr);
        std::printf("%s\n", s);
        env->ReleaseStringUTFChars(line, s);
    }

    JNIEXPORT void JNICALL Java_com_xamphy_pes13_PesActivity_nativePromptDone(JNIEnv* env, jclass, jboolean ok, jstring text)
    {
        std::function<void(bool, std::u16string)> done;
        {
            std::lock_guard lock(g_promptMutex);
            done = std::move(g_promptDone);
            g_promptDone = nullptr;
        }
        if (done) done(ok == JNI_TRUE, FromJava(env, text));
    }

    // ---------------------------------------------------------------- LauncherActivity (before the game)
    // The launcher works in the same folder as the game (Android/data/<app>/files), so the settings
    // file and the game-file check use the same relative paths as everywhere else.
    JNIEXPORT void JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeSetWorkDir(JNIEnv* env, jclass, jstring dir)
    {
        const char* s = env->GetStringUTFChars(dir, nullptr);
        chdir(s);
        env->ReleaseStringUTFChars(dir, s);
    }

    JNIEXPORT jintArray JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeRenderBackground(JNIEnv* env, jclass, jint w, jint h)
    {
        const std::vector<uint32_t> px = Launcher::RenderBackground(uint32_t(w), uint32_t(h));
        // RGBA8 (little-endian R first) -> Android ARGB ints.
        std::vector<jint> argb(px.size());
        for (size_t i = 0; i < px.size(); ++i)
        {
            const uint32_t c = px[i];
            argb[i] = jint(0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
        }
        jintArray out = env->NewIntArray(jsize(argb.size()));
        env->SetIntArrayRegion(out, 0, jsize(argb.size()), argb.data());
        return out;
    }

    // Settings as {upscaler, upscaleHeight, perfOverlay, touchControls, blurEffects}.
    JNIEXPORT jintArray JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeLoadSettings(JNIEnv* env, jclass)
    {
        const Launcher::Settings s = Launcher::LoadSettings();
        const jint v[] = { s.upscaler, s.upscaleHeight, s.perfOverlay, s.touchControls, s.blurEffects };
        jintArray out = env->NewIntArray(5);
        env->SetIntArrayRegion(out, 0, 5, v);
        return out;
    }

    JNIEXPORT void JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeSaveSettings(JNIEnv* env, jclass, jintArray values)
    {
        jint v[5] = {};
        env->GetIntArrayRegion(values, 0, 5, v);
        Launcher::Settings s;
        s.upscaler = v[0];
        s.upscaleHeight = v[1];
        s.perfOverlay = v[2] != 0;
        s.touchControls = v[3] != 0;
        s.blurEffects = v[4] != 0;
        Launcher::SaveSettings(s);
    }

    // About page as lines: "#Section", "label\tvalue", or plain text paragraphs.
    JNIEXPORT jobjectArray JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeAbout(JNIEnv* env, jclass, jstring device)
    {
        const char* d = env->GetStringUTFChars(device, nullptr);
        const Launcher::About a = Launcher::GetAbout(std::string("Android - ") + d, "");
        env->ReleaseStringUTFChars(device, d);
        std::vector<std::string> lines;
        lines.push_back("#Build");
        for (const auto& l : a.build) lines.push_back(l.label + "\t" + l.value);
        lines.push_back("Game files\t" + a.gameFiles);
        lines.push_back("#Credits");
        for (const auto& l : a.credits) lines.push_back(l.label + "\t" + l.value);
        lines.push_back(a.disclaimer);
        jobjectArray out = env->NewObjectArray(jsize(lines.size()), env->FindClass("java/lang/String"), nullptr);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            jstring s = ToJavaUtf8(env, lines[i]);
            env->SetObjectArrayElement(out, jsize(i), s);
            env->DeleteLocalRef(s);
        }
        return out;
    }

    JNIEXPORT jboolean JNICALL Java_com_xamphy_pes13_LauncherActivity_nativeGameFilesPresent(JNIEnv*, jclass)
    {
        return Launcher::GameFilesPresent() ? JNI_TRUE : JNI_FALSE;
    }
}
