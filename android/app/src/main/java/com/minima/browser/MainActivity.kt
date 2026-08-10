package com.minima.browser

import android.annotation.SuppressLint
import android.app.DownloadManager
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.webkit.CookieManager
import android.webkit.URLUtil
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.File

/**
 * Minima for Android — the Android shell of the Minima browser (see ../../../../PORTING.md).
 *
 * Runs the portable C++ core (src/core/) AND the on-device AI (vendor/llama.cpp) over
 * JNI — the same Gemma model family as desktop, fully local. UI follows Minima's desktop
 * design language, including the signature inline AI answer under the search bar.
 */
class MainActivity : AppCompatActivity() {

    // ---- Minima palette (mirrors the desktop chrome CSS variables) ----
    private var dark = false
    private val cBg get() = if (dark) 0xFF1F2023.toInt() else 0xFFECEEF2.toInt()
    private val cSurface get() = if (dark) 0xFF37393E.toInt() else 0xFFFFFFFF.toInt()
    private val cField get() = if (dark) 0xFF2A2B2F.toInt() else 0xFFE2E4E9.toInt()
    private val cFg get() = if (dark) 0xFFE8EAED.toInt() else 0xFF1C1C21.toInt()
    private val cMuted get() = if (dark) 0xFF9AA0A6.toInt() else 0xFF5F6368.toInt()
    private val cAccent = 0xFF3B82F6.toInt()

    private class Tab(val view: WebView) {
        var title: String = "New Tab"
        var url: String = ""
        var blocked: Int = 0
    }

    /** The desktop toolbar's shield, drawn from the same SVG path. */
    private inner class ShieldView(ctx: Context) : View(ctx) {
        var on = true
            set(v) { field = v; invalidate() }
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = dp(2).toFloat()
            strokeJoin = Paint.Join.ROUND
            strokeCap = Paint.Cap.ROUND
        }
        private val path = Path()
        override fun onDraw(c: Canvas) {
            // viewBox 24: M12 3l7 3v5.5c0 4.5-3 8-7 9.5-4-1.5-7-5-7-9.5V6z
            val s = minOf(width, height) / 24f
            path.reset()
            path.moveTo(12 * s, 3 * s)
            path.lineTo(19 * s, 6 * s)
            path.lineTo(19 * s, 11.5f * s)
            path.cubicTo(19 * s, 16 * s, 16 * s, 19.5f * s, 12 * s, 21 * s)
            path.cubicTo(8 * s, 19.5f * s, 5 * s, 16 * s, 5 * s, 11.5f * s)
            path.lineTo(5 * s, 6 * s)
            path.close()
            paint.color = if (on) cAccent else cMuted
            paint.alpha = if (on) 255 else 110
            c.drawPath(path, paint)
        }
    }

    private val tabs = mutableListOf<Tab>()
    private var active = -1
    private lateinit var root: FrameLayout
    private lateinit var addr: EditText
    private lateinit var shield: ShieldView
    private lateinit var shieldCount: TextView
    private lateinit var tabCountBtn: TextView
    private lateinit var container: FrameLayout
    private lateinit var progress: ProgressBar
    private lateinit var findBar: LinearLayout
    private lateinit var findInput: EditText
    private lateinit var findCount: TextView
    private lateinit var suggestPanel: LinearLayout
    private lateinit var answerCard: LinearLayout
    private lateinit var answerQuery: TextView
    private lateinit var answerBody: TextView
    private lateinit var answerAction: TextView
    private var overlay: View? = null
    private lateinit var prefs: android.content.SharedPreferences
    private var adblock = true
    private var searchPrefix = "https://www.mojeek.com/search?q="
    private val ui = Handler(Looper.getMainLooper())

    // Mirrors src/core/models.h kSearchEngines.
    private val searchEngines = listOf(
        "Mojeek (private, no tracking)" to "https://www.mojeek.com/search?q=",
        "DuckDuckGo" to "https://duckduckgo.com/?q=",
        "Google" to "https://www.google.com/search?q=",
        "Bing" to "https://www.bing.com/search?q=",
        "Brave Search" to "https://search.brave.com/search?q=",
        "Startpage" to "https://www.startpage.com/sp/search?query=")
    // Internal files dir is preferred (also reachable via `adb run-as` for development);
    // older builds downloaded to the external files dir, so accept the model from either.
    private val modelFile: File
        get() {
            val internal = File(filesDir, MinimaCore.MODEL_FILE)
            if (internal.length() > 0) return internal
            return File(getExternalFilesDir(null), MinimaCore.MODEL_FILE)
        }

    /** A file is only a usable model if it's complete — partial downloads don't count. */
    private fun modelReady(): Boolean = modelFile.length() >= 700_000_000L
    private var aiPending: String? = null // query to answer once the model is ready

    // ------------------------------------------------------------------ lifecycle
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        dark = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        prefs = getSharedPreferences("minima", Context.MODE_PRIVATE)
        adblock = prefs.getBoolean("adblock", true)
        searchPrefix = prefs.getString("searchPrefix", searchPrefix)!!

        window.statusBarColor = cBg
        window.navigationBarColor = cBg
        if (!dark) window.decorView.systemUiVisibility =
            window.decorView.systemUiVisibility or View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR

        root = FrameLayout(this)
        val main = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(cBg)
        }
        main.addView(buildToolbar())
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progressDrawable.setTint(cAccent)
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(3))
            visibility = View.INVISIBLE
        }
        main.addView(progress)
        main.addView(buildFindBar())
        main.addView(buildSuggestPanel())
        main.addView(buildAnswerCard())
        container = FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            isFocusableInTouchMode = true
        }
        main.addView(container)
        root.addView(main, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        container.requestFocus()

        val launchUrl = intent?.dataString
        if (launchUrl != null) {
            newTab(launchUrl)
        } else {
            val session = JSONArray(prefs.getString("session", "[]")!!)
            if (session.length() == 0) {
                newTab(null)
            } else {
                var cur = 0
                for (i in 0 until session.length()) {
                    val o = session.getJSONObject(i)
                    newTab(o.getString("url"))
                    if (o.optBoolean("cur")) cur = i
                }
                selectTab(cur)
            }
        }
        // Warm the AI in the background if the model is already downloaded (desktop parity).
        if (modelReady() && MinimaCore.nativeLoaded)
            Thread { MinimaCore.aiLoad(modelFile.absolutePath) }.start()
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        intent?.dataString?.let { newTab(it) }
    }

    override fun onPause() {
        super.onPause()
        saveSession()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (overlay != null) { closeOverlay(); return true }
            if (answerCard.visibility == View.VISIBLE) { hideAnswer(); return true }
            if (findBar.visibility == View.VISIBLE) { hideFind(); return true }
            val v = tabs.getOrNull(active)?.view
            if (v != null && v.canGoBack()) { v.goBack(); return true }
            if (tabs.size > 1) { closeTab(active); return true }
        }
        return super.onKeyDown(keyCode, event)
    }

    // ------------------------------------------------------------------ toolbar
    private fun pill(radiusDp: Int, color: Int): GradientDrawable =
        GradientDrawable().apply { cornerRadius = dp(radiusDp).toFloat(); setColor(color) }

    private fun buildToolbar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), dp(8), dp(6), dp(8))
            setBackgroundColor(cBg)
        }
        val pillBox = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = pill(21, cField)
            layoutParams = LinearLayout.LayoutParams(0, dp(42), 1f)
        }
        addr = EditText(this).apply {
            hint = "Search or enter address"
            setHintTextColor(cMuted)
            setTextColor(cFg)
            textSize = 14.5f
            setSingleLine()
            background = null
            inputType = android.text.InputType.TYPE_TEXT_VARIATION_URI
            imeOptions = EditorInfo.IME_ACTION_GO
            setPadding(dp(16), 0, dp(6), 0)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
            setOnEditorActionListener { _, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_GO) { navigate(text.toString()); true } else false
            }
            setOnFocusChangeListener { _, has ->
                if (has) post { selectAll() } else hideSuggest()
            }
            addTextChangedListener(object : android.text.TextWatcher {
                override fun afterTextChanged(s: android.text.Editable?) {
                    if (hasFocus()) updateSuggest(s?.toString()?.trim() ?: "")
                }
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            })
        }
        val shieldBox = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4), 0, dp(12), 0)
            setOnClickListener {
                adblock = !adblock
                prefs.edit().putBoolean("adblock", adblock).apply()
                updateShield()
                toast(if (adblock) "Ad & tracker blocking on" else "Ad & tracker blocking off")
            }
        }
        shield = ShieldView(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(22), dp(22))
        }
        shieldCount = TextView(this).apply {
            textSize = 10.5f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(cAccent)
            setPadding(dp(3), 0, 0, 0)
        }
        shieldBox.addView(shield)
        shieldBox.addView(shieldCount)
        pillBox.addView(addr)
        pillBox.addView(shieldBox)

        tabCountBtn = TextView(this).apply {
            textSize = 12.5f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(cFg)
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                cornerRadius = dp(8).toFloat()
                setStroke(dp(2), cMuted)
                setColor(Color.TRANSPARENT)
            }
            layoutParams = LinearLayout.LayoutParams(dp(34), dp(34)).apply { marginStart = dp(10) }
            setOnClickListener { showTabSwitcher() }
        }
        val menuBtn = TextView(this).apply {
            text = "⋮"
            textSize = 20f
            setTextColor(cMuted)
            gravity = Gravity.CENTER
            setPadding(dp(10), 0, dp(10), 0)
            setOnClickListener { showMenuPanel() }
        }
        bar.addView(pillBox)
        bar.addView(tabCountBtn)
        bar.addView(menuBtn)
        updateShield()
        return bar
    }

    private fun updateShield() {
        shield.on = adblock
        val blocked = tabs.getOrNull(active)?.blocked ?: 0
        shieldCount.text = if (adblock && blocked > 0) blocked.toString() else ""
    }

    // ------------------------------------------------------------------ suggestions dropdown
    private fun buildSuggestPanel(): View {
        suggestPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), 0, dp(10), dp(8))
            setBackgroundColor(cBg)
            visibility = View.GONE
        }
        return suggestPanel
    }

    private fun suggestRow(icon: String, iconColor: Int, label: String, sub: String?,
                           onTap: () -> Unit): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = pill(11, cSurface)
            setPadding(dp(14), dp(11), dp(14), dp(11))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(6) }
            setOnClickListener { onTap() }
        }
        row.addView(TextView(this).apply {
            text = icon
            textSize = 14f
            setTextColor(iconColor)
            setPadding(0, 0, dp(12), 0)
        })
        val texts = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        texts.addView(TextView(this).apply {
            text = label
            textSize = 13.5f
            setTextColor(cFg)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        })
        if (sub != null) texts.addView(TextView(this).apply {
            text = sub
            textSize = 11.5f
            setTextColor(cMuted)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        })
        row.addView(texts)
        return row
    }

    private fun updateSuggest(q: String) {
        suggestPanel.removeAllViews()
        if (q.isEmpty()) { suggestPanel.visibility = View.GONE; return }
        // Signature feature: ask the on-device AI straight from the search bar.
        suggestPanel.addView(suggestRow("✦", cAccent, "Ask Minima AI: $q", null) { askAi(q) })
        var count = 0
        fun matches(s: String) = s.contains(q, ignoreCase = true)
        val seen = mutableSetOf<String>()
        for (key in listOf("bookmarks", "history")) {
            val arr = readArray(key)
            for (i in 0 until arr.length()) {
                if (count >= 5) break
                val o = arr.getJSONObject(i)
                val url = o.getString("url")
                val title = o.optString("title")
                if (!seen.add(url)) continue
                if (matches(title) || matches(url)) {
                    suggestPanel.addView(
                        suggestRow(if (key == "bookmarks") "★" else "◷",
                                   cMuted, title.ifEmpty { url }, url) {
                            hideSuggest(); addr.clearFocus(); hideKeyboard(addr)
                            tabs.getOrNull(active)?.view?.loadUrl(url)
                        })
                    count++
                }
            }
        }
        suggestPanel.visibility = View.VISIBLE
    }

    private fun hideSuggest() { suggestPanel.visibility = View.GONE }

    // ------------------------------------------------------------------ inline AI answer (signature)
    private fun buildAnswerCard(): View {
        answerCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = pill(14, cSurface)
            setPadding(dp(16), dp(12), dp(16), dp(12))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(dp(10), 0, dp(10), dp(8)) }
            visibility = View.GONE
        }
        val head = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        head.addView(TextView(this).apply {
            text = "✦"
            textSize = 15f
            setTextColor(cAccent)
            setPadding(0, 0, dp(10), 0)
        })
        answerQuery = TextView(this).apply {
            textSize = 13.5f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(cFg)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        head.addView(answerQuery)
        head.addView(TextView(this).apply {
            text = "✕"
            textSize = 14f
            setTextColor(cMuted)
            setPadding(dp(12), dp(2), dp(2), dp(2))
            setOnClickListener { hideAnswer() }
        })
        answerCard.addView(head)
        answerBody = TextView(this).apply {
            textSize = 13.5f
            setTextColor(cFg)
            setLineSpacing(0f, 1.25f)
            setPadding(0, dp(8), 0, dp(4))
            maxHeight = dp(260)
            movementMethod = android.text.method.ScrollingMovementMethod()
        }
        answerCard.addView(answerBody)
        answerAction = TextView(this).apply {
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            background = pill(18, cAccent)
            setPadding(0, dp(9), 0, dp(9))
            visibility = View.GONE
        }
        answerCard.addView(answerAction)
        answerCard.addView(TextView(this).apply {
            text = "Gemma 3 1B · runs fully on this device"
            textSize = 10.5f
            setTextColor(cMuted)
            setPadding(0, dp(6), 0, 0)
        })
        return answerCard
    }

    private fun hideAnswer() {
        MinimaCore.aiStop()
        aiPending = null
        answerCard.visibility = View.GONE
    }

    /** The desktop's "fast answer under the search bar", on-device via llama.cpp. */
    private fun askAi(q: String) {
        hideSuggest()
        addr.clearFocus()
        hideKeyboard(addr)
        answerCard.visibility = View.VISIBLE
        answerQuery.text = q
        answerAction.visibility = View.GONE
        when {
            MinimaCore.aiLoaded() -> startGeneration(q)
            !MinimaCore.nativeLoaded ->
                answerBody.text = "On-device AI needs the native core, which failed to load."
            modelReady() -> {
                answerBody.text = "Starting the on-device AI…"
                aiPending = q
                Thread {
                    val ok = MinimaCore.aiLoad(modelFile.absolutePath)
                    ui.post {
                        val p = aiPending ?: return@post
                        if (ok) startGeneration(p)
                        else answerBody.text = "Could not load the AI model."
                    }
                }.start()
            }
            else -> {
                answerBody.text = "Answers run fully on this device — nothing leaves your phone. " +
                    "First use downloads the Gemma model (~${MinimaCore.MODEL_SIZE_MB} MB)."
                answerAction.text = "Download & answer"
                answerAction.visibility = View.VISIBLE
                answerAction.setOnClickListener { startModelDownload(q) }
            }
        }
    }

    private fun startGeneration(q: String, retry: Boolean = true) {
        MinimaCore.aiAsk(q)
        answerBody.text = "…"
        val poller = object : Runnable {
            override fun run() {
                if (answerCard.visibility != View.VISIBLE) return
                val text = MinimaCore.aiPoll().replace("**", "")
                if (text.isNotEmpty()) answerBody.text = text
                if (MinimaCore.aiBusy()) {
                    ui.postDelayed(this, 150)
                } else if (text.isEmpty()) {
                    // A decode can fail transiently right after the model loads — retry once.
                    if (retry) ui.postDelayed({ startGeneration(q, retry = false) }, 500)
                    else answerBody.text = "(no answer — try again)"
                }
            }
        }
        ui.postDelayed(poller, 150)
    }

    // Minima downloads the model itself (HttpURLConnection, resumable via Range) straight
    // into internal storage — DownloadManager proved unreliable with the HF CDN redirect.
    @Volatile private var dlCancel = false
    private var dlActive = false

    private fun startModelDownload(q: String?, onProgress: (String) -> Unit = { answerBody.text = it }) {
        if (dlActive) return
        dlActive = true
        dlCancel = false
        answerAction.visibility = View.GONE
        aiPending = q
        onProgress("Connecting…")
        Thread {
            val part = File(filesDir, MinimaCore.MODEL_FILE + ".part")
            val dest = File(filesDir, MinimaCore.MODEL_FILE)
            var err: String? = null
            try {
                var url = java.net.URL(MinimaCore.MODEL_URL)
                var resume = part.length()
                // Follow redirects manually so the Range header survives them.
                var conn: java.net.HttpURLConnection
                var hops = 0
                while (true) {
                    conn = url.openConnection() as java.net.HttpURLConnection
                    conn.instanceFollowRedirects = false
                    conn.connectTimeout = 20000
                    conn.readTimeout = 30000
                    conn.setRequestProperty("User-Agent", "Minima/1.0")
                    if (resume > 0) conn.setRequestProperty("Range", "bytes=$resume-")
                    val code = conn.responseCode
                    if (code in 301..308 && hops++ < 6) {
                        val loc = conn.getHeaderField("Location") ?: throw Exception("bad redirect")
                        conn.disconnect()
                        url = java.net.URL(url, loc)
                        continue
                    }
                    if (code == 200) { resume = 0; part.delete() } // server ignored Range
                    else if (code != 206 && code != 200) throw Exception("HTTP $code")
                    break
                }
                val total = resume + conn.contentLengthLong.coerceAtLeast(0)
                conn.inputStream.use { input ->
                    java.io.FileOutputStream(part, resume > 0).use { out ->
                        val buf = ByteArray(256 * 1024)
                        var done = resume
                        var lastUi = 0L
                        while (true) {
                            if (dlCancel) throw Exception("cancelled")
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            done += n
                            val now = System.currentTimeMillis()
                            if (now - lastUi > 500) {
                                lastUi = now
                                val msg = if (total > 0)
                                    "Downloading the AI model… ${done * 100 / total}%  (${done / 1048576} MB)"
                                else "Downloading the AI model… ${done / 1048576} MB"
                                ui.post { onProgress(msg) }
                            }
                        }
                    }
                }
                if (!part.renameTo(dest)) throw Exception("could not save model")
            } catch (e: Exception) {
                err = e.message ?: "download error"
            }
            dlActive = false
            ui.post {
                if (err == null) {
                    onProgress("Download complete.")
                    val p = aiPending
                    if (p != null) askAi(p) // model now exists → load + answer
                } else if (err != "cancelled") {
                    onProgress("Model download failed ($err). Tap to retry — it resumes where it left off.")
                    answerAction.text = "Retry download"
                    answerAction.visibility = View.VISIBLE
                    answerAction.setOnClickListener { q?.let { startModelDownload(it) } }
                }
            }
        }.start()
    }

    // ------------------------------------------------------------------ find bar
    private fun buildFindBar(): View {
        findBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), 0, dp(6), dp(8))
            setBackgroundColor(cBg)
            visibility = View.GONE
        }
        findInput = EditText(this).apply {
            hint = "Find in page"
            setHintTextColor(cMuted)
            setTextColor(cFg)
            textSize = 14f
            setSingleLine()
            background = pill(18, cField)
            setPadding(dp(16), 0, dp(16), 0)
            layoutParams = LinearLayout.LayoutParams(0, dp(38), 1f)
            addTextChangedListener(object : android.text.TextWatcher {
                override fun afterTextChanged(s: android.text.Editable?) {
                    tabs.getOrNull(active)?.view?.findAllAsync(s?.toString() ?: "")
                }
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            })
            setOnEditorActionListener { _, id, _ ->
                if (id == EditorInfo.IME_ACTION_GO || id == EditorInfo.IME_ACTION_SEARCH ||
                    id == EditorInfo.IME_ACTION_DONE) {
                    tabs.getOrNull(active)?.view?.findNext(true); true
                } else false
            }
        }
        findCount = TextView(this).apply {
            textSize = 12f
            setTextColor(cMuted)
            setPadding(dp(10), 0, dp(4), 0)
        }
        fun navBtn(label: String, onClick: () -> Unit) = TextView(this).apply {
            text = label
            textSize = 16f
            setTextColor(cMuted)
            gravity = Gravity.CENTER
            setPadding(dp(10), 0, dp(10), 0)
            setOnClickListener { onClick() }
        }
        findBar.addView(findInput)
        findBar.addView(findCount)
        findBar.addView(navBtn("▲") { tabs.getOrNull(active)?.view?.findNext(false) })
        findBar.addView(navBtn("▼") { tabs.getOrNull(active)?.view?.findNext(true) })
        findBar.addView(navBtn("✕") { hideFind() })
        return findBar
    }

    private fun showFind() {
        findBar.visibility = View.VISIBLE
        findInput.setText("")
        findInput.requestFocus()
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
            .showSoftInput(findInput, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun hideFind() {
        findBar.visibility = View.GONE
        tabs.getOrNull(active)?.view?.clearMatches()
        hideKeyboard(findInput)
    }

    // ------------------------------------------------------------------ tabs
    @SuppressLint("SetJavaScriptEnabled")
    private fun newTab(url: String?): Tab {
        val web = WebView(this)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true
        web.settings.databaseEnabled = true
        web.settings.setSupportZoom(true)
        web.settings.builtInZoomControls = true
        web.settings.displayZoomControls = false
        web.setBackgroundColor(cBg)
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, false)
        val tab = Tab(web)
        web.webViewClient = object : WebViewClient() {
            override fun onPageStarted(v: WebView?, u: String?, favicon: android.graphics.Bitmap?) {
                if (u != null && !u.startsWith("data:")) tab.url = u
                tab.blocked = 0
                if (isActive(tab)) {
                    if (!addr.hasFocus()) addr.setText(displayUrl(tab.url))
                    progress.visibility = View.VISIBLE
                    updateShield()
                }
            }
            override fun onPageFinished(v: WebView?, u: String?) {
                if (isActive(tab)) progress.visibility = View.INVISIBLE
                tab.title = v?.title ?: tab.url
                if (u != null && !u.startsWith("data:")) tab.url = u
                addHistory(tab.url, tab.title)
                saveSession()
            }
            override fun shouldInterceptRequest(v: WebView?, req: WebResourceRequest?): WebResourceResponse? {
                if (!adblock || req == null) return null
                val host = req.url.host?.lowercase() ?: return null
                if (MinimaCore.isAdHost(host)) {
                    tab.blocked++
                    if (isActive(tab)) runOnUiThread { updateShield() }
                    return WebResourceResponse("text/plain", "utf-8", ByteArrayInputStream(ByteArray(0)))
                }
                return null
            }
        }
        web.webChromeClient = object : WebChromeClient() {
            override fun onProgressChanged(v: WebView?, p: Int) {
                if (isActive(tab)) progress.progress = p
            }
            override fun onReceivedTitle(v: WebView?, title: String?) {
                if (title != null) tab.title = title
            }
        }
        web.setFindListener { activeIdx, total, done ->
            if (done) findCount.text = if (total == 0) "0/0" else "${activeIdx + 1}/$total"
        }
        web.setDownloadListener { dlUrl, userAgent, contentDisposition, mimeType, _ ->
            try {
                val name = URLUtil.guessFileName(dlUrl, contentDisposition, mimeType)
                val req = DownloadManager.Request(Uri.parse(dlUrl)).apply {
                    setMimeType(mimeType)
                    addRequestHeader("User-Agent", userAgent)
                    setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                    setDestinationInExternalPublicDir(android.os.Environment.DIRECTORY_DOWNLOADS, name)
                }
                (getSystemService(DOWNLOAD_SERVICE) as DownloadManager).enqueue(req)
                toast("Downloading $name")
            } catch (e: Exception) {
                toast("Download failed to start")
            }
        }
        tabs.add(tab)
        container.addView(web, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        if (url != null) web.loadUrl(url) else loadStartPage(web)
        selectTab(tabs.size - 1)
        return tab
    }

    private fun isActive(t: Tab) = active >= 0 && active < tabs.size && tabs[active] === t

    private fun displayUrl(u: String) =
        if (u.startsWith("data:") || u.startsWith("https://start.minima.local")) "" else u

    private fun selectTab(i: Int) {
        if (i < 0 || i >= tabs.size) return
        active = i
        for ((k, t) in tabs.withIndex()) t.view.visibility = if (k == i) View.VISIBLE else View.GONE
        addr.clearFocus()
        addr.setText(displayUrl(tabs[i].url))
        tabCountBtn.text = tabs.size.toString()
        updateShield()
    }

    private fun closeTab(i: Int) {
        if (i < 0 || i >= tabs.size) return
        val t = tabs.removeAt(i)
        container.removeView(t.view)
        t.view.destroy()
        if (tabs.isEmpty()) { newTab(null); return }
        selectTab(minOf(i, tabs.size - 1))
        saveSession()
    }

    private fun navigate(input: String) {
        val t = input.trim()
        if (t.isEmpty()) return
        hideSuggest()
        tabs.getOrNull(active)?.view?.loadUrl(MinimaCore.resolveInput(t, searchPrefix))
        addr.clearFocus()
        hideKeyboard(addr)
    }

    // ------------------------------------------------------------------ start page
    private fun loadStartPage(web: WebView) {
        val tiles = StringBuilder()
        val arr = readArray("bookmarks")
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            val label = o.optString("title").ifEmpty { o.getString("url") }
            val initial = label.firstOrNull()?.uppercaseChar() ?: '?'
            tiles.append("<a class='tile' href='").append(o.getString("url"))
                .append("'><div class='fav'>").append(initial).append("</div><span>")
                .append(android.text.Html.escapeHtml(label.take(22))).append("</span></a>")
        }
        val empty = if (arr.length() == 0)
            "<p class='hint'>Pages you bookmark appear here.</p>" else ""
        val html = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root { color-scheme: light dark; }
* { margin:0; padding:0; box-sizing:border-box; font-family:system-ui,Roboto,sans-serif; }
body { background:#eceef2; color:#1c1c21; min-height:100vh; display:flex;
       flex-direction:column; align-items:center; padding:64px 20px 40px; }
@media (prefers-color-scheme: dark) { body { background:#1f2023; color:#e8eaed; } }
.logo { width:76px; height:76px; border-radius:22px; display:flex; align-items:center;
        justify-content:center; font-size:40px; font-weight:800; color:#fff;
        background:linear-gradient(135deg,#4a80f5,#7c5cff);
        box-shadow:0 10px 30px rgba(74,128,245,.35); margin-bottom:14px; }
h1 { font-size:22px; font-weight:700; margin-bottom:34px; letter-spacing:.01em; }
.tiles { display:grid; grid-template-columns:repeat(auto-fill,minmax(96px,1fr));
         gap:14px; width:100%; max-width:420px; }
.tile { display:flex; flex-direction:column; align-items:center; gap:8px;
        text-decoration:none; color:inherit; padding:12px 4px; border-radius:14px; }
.tile:active { background:rgba(59,130,246,.12); }
.fav { width:52px; height:52px; border-radius:16px; background:rgba(59,130,246,.14);
       color:#3b82f6; font-size:22px; font-weight:700; display:flex;
       align-items:center; justify-content:center; }
.tile span { font-size:12px; text-align:center; overflow:hidden; max-width:96px;
             white-space:nowrap; text-overflow:ellipsis; }
.hint { color:#888; font-size:13.5px; }
</style></head><body>
<div class="logo">M</div><h1>Minima</h1>
<div class="tiles">$tiles</div>$empty
</body></html>"""
        web.loadDataWithBaseURL("https://start.minima.local/", html, "text/html", "utf-8", null)
    }

    // ------------------------------------------------------------------ data
    private fun readArray(key: String) = JSONArray(prefs.getString(key, "[]")!!)
    private fun writeArray(key: String, arr: JSONArray) =
        prefs.edit().putString(key, arr.toString()).apply()

    private fun addHistory(url: String, title: String) {
        if (url.isEmpty() || url.startsWith("about:") || url.startsWith("data:") ||
            url.startsWith("https://start.minima.local")) return
        val arr = readArray("history")
        val out = JSONArray()
        out.put(JSONObject().put("url", url).put("title", title).put("time", System.currentTimeMillis()))
        for (i in 0 until minOf(arr.length(), 499)) {
            val o = arr.getJSONObject(i)
            if (o.getString("url") != url) out.put(o)
        }
        writeArray("history", out)
    }

    private fun toggleBookmark() {
        val t = tabs.getOrNull(active) ?: return
        if (t.url.isEmpty() || t.url.startsWith("https://start.minima.local")) return
        val arr = readArray("bookmarks")
        val out = JSONArray()
        var removed = false
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            if (o.getString("url") == t.url) removed = true else out.put(o)
        }
        if (!removed) out.put(JSONObject().put("url", t.url).put("title", t.title))
        writeArray("bookmarks", out)
        toast(if (removed) "Bookmark removed" else "Bookmarked")
    }

    private fun saveSession() {
        val arr = JSONArray()
        for ((i, t) in tabs.withIndex()) {
            if (t.url.isEmpty() || t.url.startsWith("about:") || t.url.startsWith("data:") ||
                t.url.startsWith("https://start.minima.local")) continue
            arr.put(JSONObject().put("url", t.url).put("cur", i == active))
        }
        writeArray("session", arr)
    }

    // ------------------------------------------------------------------ overlays (Minima-styled)
    private fun showOverlay(build: (LinearLayout) -> Unit, topGravity: Boolean = false) {
        closeOverlay()
        val ov = FrameLayout(this).apply {
            setBackgroundColor(if (dark) 0xCC101114.toInt() else 0xCC30343B.toInt())
            setOnClickListener { closeOverlay() }
        }
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(if (topGravity) 60 else 26), dp(18), dp(26))
        }
        build(col)
        ov.addView(ScrollView(this).apply {
            addView(col)
            isVerticalScrollBarEnabled = false
        }, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            if (topGravity) Gravity.TOP else Gravity.CENTER_VERTICAL))
        root.addView(ov, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        overlay = ov
    }

    private fun closeOverlay() {
        overlay?.let { root.removeView(it) }
        overlay = null
    }

    private fun heading(text: String) = TextView(this).apply {
        this.text = text
        textSize = 20f
        typeface = Typeface.DEFAULT_BOLD
        setTextColor(Color.WHITE)
        setPadding(dp(6), 0, 0, dp(14))
    }

    private fun card(strokeAccent: Boolean = false): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        background = pill(14, cSurface).also {
            if (strokeAccent) it.setStroke(dp(2), cAccent)
        }
        setPadding(dp(16), dp(13), dp(10), dp(13))
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dp(9) }
    }

    // ------------------------------------------------------------------ tab switcher
    private fun showTabSwitcher() {
        showOverlay({ col ->
            col.addView(heading("Tabs"))
            for ((i, t) in tabs.withIndex()) {
                val row = card(strokeAccent = i == active).apply {
                    setOnClickListener { selectTab(i); closeOverlay() }
                }
                val texts = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }
                texts.addView(TextView(this).apply {
                    text = t.title.ifEmpty { "New Tab" }
                    textSize = 15f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(cFg)
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.END
                })
                texts.addView(TextView(this).apply {
                    text = displayUrl(t.url).ifEmpty { "Start page" }
                    textSize = 12f
                    setTextColor(cMuted)
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.END
                })
                row.addView(texts)
                row.addView(TextView(this).apply {
                    text = "✕"
                    textSize = 15f
                    setTextColor(cMuted)
                    setPadding(dp(12), dp(4), dp(12), dp(4))
                    setOnClickListener { closeTab(i); closeOverlay(); showTabSwitcher() }
                })
                col.addView(row)
            }
            col.addView(TextView(this).apply {
                text = "+  New tab"
                textSize = 15f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(Color.WHITE)
                gravity = Gravity.CENTER
                background = pill(22, cAccent)
                setPadding(0, dp(13), 0, dp(13))
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { topMargin = dp(8) }
                setOnClickListener { newTab(null); closeOverlay() }
            })
        })
    }

    // ------------------------------------------------------------------ menu (Minima-styled)
    private fun showMenuPanel() {
        showOverlay({ col ->
            fun item(icon: String, label: String, accent: Boolean = false, action: () -> Unit) {
                val row = card().apply { setOnClickListener { closeOverlay(); action() } }
                row.addView(TextView(this).apply {
                    text = icon
                    textSize = 15f
                    setTextColor(if (accent) cAccent else cMuted)
                    setPadding(0, 0, dp(14), 0)
                })
                row.addView(TextView(this).apply {
                    text = label
                    textSize = 14.5f
                    setTextColor(cFg)
                })
                col.addView(row)
            }
            col.addView(heading("Minima"))
            item("✦", "Ask Minima AI", accent = true) {
                addr.setText("")
                addr.requestFocus()
                (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
                    .showSoftInput(addr, InputMethodManager.SHOW_IMPLICIT)
                toast("Type a question, then tap the ✦ row")
            }
            item("+", "New tab") { newTab(null) }
            item("⌕", "Find in page") { showFind() }
            item("★", "Bookmark this page") { toggleBookmark() }
            item("★", "Bookmarks") { showLinkList("Bookmarks", readArray("bookmarks")) }
            item("◷", "History") { showLinkList("History", readArray("history")) }
            item("↓", "Downloads") { startActivity(Intent(DownloadManager.ACTION_VIEW_DOWNLOADS)) }
            item("↗", "Share page") {
                tabs.getOrNull(active)?.let { t ->
                    startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"; putExtra(Intent.EXTRA_TEXT, t.url)
                    }, "Share page"))
                }
            }
            item("⟳", "Reload") { tabs.getOrNull(active)?.view?.reload() }
            item("⛨", if (adblock) "Ad blocking: on" else "Ad blocking: off") {
                adblock = !adblock
                prefs.edit().putBoolean("adblock", adblock).apply()
                updateShield()
                toast(if (adblock) "Ad & tracker blocking on" else "Ad & tracker blocking off")
            }
            item("⚙", "Settings") { showSettingsPanel() }
        }, topGravity = true)
    }

    // ------------------------------------------------------------------ settings (Minima-styled)
    private fun showSettingsPanel() {
        showOverlay({ col ->
            fun section(label: String) = col.addView(TextView(this).apply {
                text = label.uppercase()
                textSize = 11.5f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xB3FFFFFF.toInt())
                letterSpacing = 0.06f
                setPadding(dp(8), dp(14), 0, dp(8))
            })
            col.addView(heading("Settings"))

            section("Search engine")
            for ((label, prefix) in searchEngines) {
                val selected = searchPrefix == prefix
                val row = card(strokeAccent = selected).apply {
                    setOnClickListener {
                        searchPrefix = prefix
                        prefs.edit().putString("searchPrefix", prefix).apply()
                        closeOverlay(); showSettingsPanel()
                    }
                }
                row.addView(TextView(this).apply {
                    text = if (selected) "●" else "○"
                    textSize = 13f
                    setTextColor(if (selected) cAccent else cMuted)
                    setPadding(0, 0, dp(14), 0)
                })
                row.addView(TextView(this).apply {
                    text = label
                    textSize = 14f
                    setTextColor(cFg)
                })
                col.addView(row)
            }

            section("Privacy")
            val ab = card().apply {
                setOnClickListener {
                    adblock = !adblock
                    prefs.edit().putBoolean("adblock", adblock).apply()
                    updateShield()
                    closeOverlay(); showSettingsPanel()
                }
            }
            ab.addView(TextView(this).apply {
                text = "⛨"
                textSize = 15f
                setTextColor(if (adblock) cAccent else cMuted)
                setPadding(0, 0, dp(14), 0)
            })
            ab.addView(TextView(this).apply {
                text = "Block ads & trackers"
                textSize = 14f
                setTextColor(cFg)
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            })
            ab.addView(TextView(this).apply {
                text = if (adblock) "On" else "Off"
                textSize = 13f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(if (adblock) cAccent else cMuted)
                setPadding(0, 0, dp(6), 0)
            })
            col.addView(ab)

            section("On-device AI")
            val status = when {
                MinimaCore.aiLoaded() -> "Gemma 3 1B — ready"
                dlActive -> "Gemma 3 1B — downloading…"
                modelReady() -> "Gemma 3 1B — downloaded"
                else -> "Gemma 3 1B — not downloaded (~${MinimaCore.MODEL_SIZE_MB} MB)"
            }
            val aiRow = card()
            val aiTexts = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            }
            aiTexts.addView(TextView(this).apply {
                text = status
                textSize = 14f
                setTextColor(cFg)
            })
            aiTexts.addView(TextView(this).apply {
                text = "Answers run fully on this device — nothing leaves your phone."
                textSize = 11.5f
                setTextColor(cMuted)
            })
            aiRow.addView(TextView(this).apply {
                text = "✦"
                textSize = 15f
                setTextColor(cAccent)
                setPadding(0, 0, dp(14), 0)
            })
            aiRow.addView(aiTexts)
            col.addView(aiRow)
            if (!modelReady() && !dlActive) {
                col.addView(TextView(this).apply {
                    text = "Download AI model"
                    textSize = 14f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(Color.WHITE)
                    gravity = Gravity.CENTER
                    background = pill(22, cAccent)
                    setPadding(0, dp(12), 0, dp(12))
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                    ).apply { bottomMargin = dp(9) }
                    setOnClickListener {
                        closeOverlay()
                        answerCard.visibility = View.VISIBLE
                        answerQuery.text = "Setting up on-device AI"
                        startModelDownload(null)
                    }
                })
            } else if (modelReady() && !MinimaCore.aiLoaded() && !dlActive) {
                col.addView(TextView(this).apply {
                    text = "Delete AI model"
                    textSize = 14f
                    setTextColor(0xFFE0555F.toInt())
                    gravity = Gravity.CENTER
                    background = pill(22, cSurface)
                    setPadding(0, dp(12), 0, dp(12))
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                    ).apply { bottomMargin = dp(9) }
                    setOnClickListener {
                        File(filesDir, MinimaCore.MODEL_FILE).delete()
                        File(getExternalFilesDir(null), MinimaCore.MODEL_FILE).delete()
                        toast("AI model deleted")
                        closeOverlay(); showSettingsPanel()
                    }
                })
            }

            section("Data")
            fun dangerRow(label: String, action: () -> Unit) {
                val row = card().apply { setOnClickListener { action(); closeOverlay() } }
                row.addView(TextView(this).apply {
                    text = label
                    textSize = 14f
                    setTextColor(0xFFE0555F.toInt())
                })
                col.addView(row)
            }
            dangerRow("Clear browsing history") { writeArray("history", JSONArray()); toast("History cleared") }
            dangerRow("Clear bookmarks") { writeArray("bookmarks", JSONArray()); toast("Bookmarks cleared") }

            col.addView(TextView(this).apply {
                text = "Minima 0.9 · engine: system WebView · C++ core: " +
                    if (MinimaCore.nativeLoaded) "loaded" else "unavailable"
                textSize = 11f
                setTextColor(0x80FFFFFF.toInt())
                setPadding(dp(8), dp(12), 0, 0)
            })
        }, topGravity = true)
    }

    private fun showLinkList(title: String, arr: JSONArray) {
        if (arr.length() == 0) { toast("Nothing here yet"); return }
        showOverlay({ col ->
            col.addView(heading(title))
            for (i in 0 until minOf(arr.length(), 40)) {
                val o = arr.getJSONObject(i)
                val url = o.getString("url")
                val row = card().apply {
                    setOnClickListener {
                        closeOverlay()
                        tabs.getOrNull(active)?.view?.loadUrl(url) ?: newTab(url)
                    }
                }
                val texts = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }
                texts.addView(TextView(this).apply {
                    text = o.optString("title").ifEmpty { url }
                    textSize = 14.5f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(cFg)
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.END
                })
                texts.addView(TextView(this).apply {
                    text = url
                    textSize = 11.5f
                    setTextColor(cMuted)
                    maxLines = 1
                    ellipsize = android.text.TextUtils.TruncateAt.END
                })
                row.addView(texts)
                col.addView(row)
            }
        })
    }

    // ------------------------------------------------------------------ misc
    private fun hideKeyboard(v: View) =
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
            .hideSoftInputFromWindow(v.windowToken, 0)

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
}
