package com.minima.browser

import android.util.Log

/**
 * Bridge to Minima's portable C++ core (src/core/ at the repo root) and the on-device AI
 * (vendor/llama.cpp), both built into libminima_core.so by the NDK. Ad/tracker matching,
 * address-bar resolution, and Gemma inference run the same code family as the desktop
 * builds. A Kotlin fallback keeps basic browsing working if the native library is
 * unavailable (AI requires the native library).
 */
object MinimaCore {
    var nativeLoaded = false
        private set

    init {
        nativeLoaded = try {
            System.loadLibrary("minima_core")
            true
        } catch (e: UnsatisfiedLinkError) {
            Log.w("Minima", "native core unavailable, using Kotlin fallback: $e")
            false
        }
        Log.i("Minima", "C++ core loaded: $nativeLoaded")
    }

    // Same model the desktop default uses (src/core/ai_models.h, gemma-3-1b).
    const val MODEL_FILE = "gemma-3-1b-it-Q4_K_M.gguf"
    const val MODEL_URL =
        "https://huggingface.co/unsloth/gemma-3-1b-it-GGUF/resolve/main/gemma-3-1b-it-Q4_K_M.gguf"
    const val MODEL_SIZE_MB = 770L

    private external fun nativeIsAdHost(host: String): Boolean
    private external fun nativeResolveInput(input: String, searchPrefix: String): String
    private external fun nativeAiLoad(path: String): Boolean
    private external fun nativeAiLoaded(): Boolean
    private external fun nativeAiAsk(prompt: String)
    private external fun nativeAiPoll(): String
    private external fun nativeAiBusy(): Boolean
    private external fun nativeAiStop()

    private val fallbackAdHosts = listOf(
        "doubleclick.net", "googlesyndication.com", "googleadservices.com",
        "google-analytics.com", "googletagmanager.com", "adnxs.com", "criteo.com",
        "outbrain.com", "taboola.com", "scorecardresearch.com", "pubmatic.com",
        "rubiconproject.com", "hotjar.com", "mixpanel.com", "amplitude.com")

    fun isAdHost(host: String): Boolean =
        if (nativeLoaded) nativeIsAdHost(host)
        else fallbackAdHosts.any { host == it || host.endsWith(".$it") }

    fun resolveInput(input: String, searchPrefix: String): String =
        if (nativeLoaded) nativeResolveInput(input, searchPrefix)
        else {
            val t = input.trim()
            when {
                t.startsWith("http://") || t.startsWith("https://") -> t
                t.startsWith("localhost") -> "http://$t"
                t.contains('.') && !t.contains(' ') -> "https://$t"
                else -> searchPrefix + android.net.Uri.encode(t)
            }
        }

    fun aiLoad(path: String): Boolean = nativeLoaded && nativeAiLoad(path)
    fun aiLoaded(): Boolean = nativeLoaded && nativeAiLoaded()
    fun aiAsk(prompt: String) { if (nativeLoaded) nativeAiAsk(prompt) }
    fun aiPoll(): String = if (nativeLoaded) nativeAiPoll() else ""
    fun aiBusy(): Boolean = nativeLoaded && nativeAiBusy()
    fun aiStop() { if (nativeLoaded) nativeAiStop() }
}
