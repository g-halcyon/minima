// minima_jni.cpp — the JNI bridge that gives the Android shell the REAL portable core
// (src/core/) and Minima's on-device AI (llama.cpp, vendored at vendor/llama.cpp,
// compiled by the NDK). Same Gemma models and behavior as the desktop builds.
//
// AI surface is poll-based streaming (simple + robust across JNI):
//   nativeAiLoad(modelPath)  — load the GGUF once (background thread on the Kotlin side)
//   nativeAiAsk(prompt)      — start generating on a worker thread
//   nativeAiPoll()           — snapshot of the text generated so far
//   nativeAiBusy()           — still generating?
//   nativeAiStop()           — cancel generation
#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../../../src/core/adblock.h"
#include "../../../../../src/core/urls.h"
#include "../../../../../src/core/utf.h"
#include "llama.h"

using namespace minima;

static std::wstring JStringToWide(JNIEnv* env, jstring s) {
    if (!s) return L"";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::wstring w = Utf8ToWideStr(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return w;
}

static std::string JStringToUtf8(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_minima_browser_MinimaCore_nativeIsAdHost(JNIEnv* env, jobject, jstring host) {
    return IsAdHost(JStringToWide(env, host)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_minima_browser_MinimaCore_nativeResolveInput(JNIEnv* env, jobject, jstring input,
                                                      jstring searchPrefix) {
    std::string url = WideToUtf8(
        ResolveInputToUrl(JStringToWide(env, input), JStringToWide(env, searchPrefix)));
    return env->NewStringUTF(url.c_str());
}

// ---------------------------------------------------------------------------
// On-device AI (llama.cpp).
// ---------------------------------------------------------------------------
static llama_model* g_model = nullptr;
static llama_context* g_ctx = nullptr;
static const llama_vocab* g_vocab = nullptr;
static llama_sampler* g_smpl = nullptr;
static std::mutex g_mx;
static std::string g_out;
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_stop{false};

extern "C" JNIEXPORT jboolean JNICALL
Java_com_minima_browser_MinimaCore_nativeAiLoad(JNIEnv* env, jobject, jstring path) {
    static std::mutex loadMx; // warm-load at startup and ask-triggered load may race
    std::lock_guard<std::mutex> lk(loadMx);
    if (g_model) return JNI_TRUE;
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    g_model = llama_model_load_from_file(JStringToUtf8(env, path).c_str(), mp);
    if (!g_model) return JNI_FALSE;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 1024; // inline answers are short; smaller KV cache = less memory pressure
    unsigned hw = std::thread::hardware_concurrency();
    cp.n_threads = cp.n_threads_batch = hw > 2 ? static_cast<int>(hw) - 1 : 2;
    g_ctx = llama_init_from_model(g_model, cp);
    if (!g_ctx) {
        llama_model_free(g_model);
        g_model = nullptr;
        return JNI_FALSE;
    }
    g_vocab = llama_model_get_vocab(g_model);
    g_smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(g_smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(g_smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_minima_browser_MinimaCore_nativeAiLoaded(JNIEnv*, jobject) {
    return g_model ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_minima_browser_MinimaCore_nativeAiAsk(JNIEnv* env, jobject, jstring prompt) {
    if (!g_ctx || g_busy.exchange(true)) return;
    g_stop = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_out.clear();
    }
    std::string userText = JStringToUtf8(env, prompt);
    std::thread([userText] {
        // Gemma instruction format; each ask is a fresh single-turn exchange.
        llama_memory_clear(llama_get_memory(g_ctx), true);
        std::string full = "<start_of_turn>user\nAnswer concisely, in a few sentences. " + userText +
                           "<end_of_turn>\n<start_of_turn>model\n";
        std::vector<llama_token> toks(full.size() + 16);
        int n = llama_tokenize(g_vocab, full.c_str(), static_cast<int32_t>(full.size()),
                               toks.data(), static_cast<int32_t>(toks.size()), true, true);
        if (n <= 0) {
            g_busy = false;
            return;
        }
        toks.resize(n);
        llama_batch batch = llama_batch_get_one(toks.data(), n);
        if (llama_decode(g_ctx, batch) != 0) {
            g_busy = false;
            return;
        }
        for (int i = 0; i < 200 && !g_stop.load(); i++) {
            llama_token tok = llama_sampler_sample(g_smpl, g_ctx, -1);
            if (llama_vocab_is_eog(g_vocab, tok)) break;
            char buf[256];
            int len = llama_token_to_piece(g_vocab, tok, buf, sizeof(buf), 0, true);
            if (len > 0) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_out.append(buf, static_cast<size_t>(len));
            }
            llama_batch next = llama_batch_get_one(&tok, 1);
            if (llama_decode(g_ctx, next) != 0) break;
        }
        g_busy = false;
    }).detach();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_minima_browser_MinimaCore_nativeAiPoll(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lk(g_mx);
    return env->NewStringUTF(g_out.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_minima_browser_MinimaCore_nativeAiBusy(JNIEnv*, jobject) {
    return g_busy.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_minima_browser_MinimaCore_nativeAiStop(JNIEnv*, jobject) {
    g_stop = true;
}
