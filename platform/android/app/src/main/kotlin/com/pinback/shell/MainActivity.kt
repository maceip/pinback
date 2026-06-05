package com.pinback.shell

import android.annotation.SuppressLint
import android.content.Context
import android.os.Bundle
import android.webkit.JavascriptInterface
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.BackEventCompat
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import androidx.core.view.MenuProvider
import androidx.lifecycle.Lifecycle
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

class MainActivity : ComponentActivity() {

    private lateinit var web: WebView
    private var inSetup = false

    private val prefs by lazy {
        getSharedPreferences("pinback", Context.MODE_PRIVATE)
    }

    @SuppressLint("SetJavaScriptEnabled", "JavascriptInterface")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        web = WebView(this).apply {
            webViewClient = object : WebViewClient() {
                override fun onReceivedError(
                    view: WebView?,
                    errorCode: Int,
                    description: String?,
                    failingUrl: String?
                ) {
                    if (!inSetup && failingUrl != null && failingUrl != "about:blank") {
                        runOnUiThread { showSetup(resolvedUrl()) }
                    }
                }
            }
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            addJavascriptInterface(Bridge(), "PinbackHost")
            addJavascriptInterface(SetupBridge(), "PinbackSetup")
        }
        setContentView(web)

        addMenuProvider(object : MenuProvider {
            override fun onCreateMenu(menu: android.view.Menu, inflater: android.view.MenuInflater) {
                menu.add(0, MENU_SERVER, 0, "Server settings")
            }
            override fun onMenuItemSelected(item: android.view.MenuItem): Boolean {
                if (item.itemId == MENU_SERVER) {
                    showSetup(resolvedUrl())
                    return true
                }
                return false
            }
        }, this, Lifecycle.State.RESUMED)

        onBackPressedDispatcher.addCallback(this, backCallback)

        val url = resolvedUrl()
        if (healthOk(url)) showCockpit(url) else showSetup(url)
    }

    private fun resolvedUrl(): String {
        System.getenv("PINBACK_URL")?.takeIf { it.isNotBlank() }?.let { return normalize(it) }
        prefs.getString(PREF_URL, null)?.takeIf { it.isNotBlank() }?.let { return normalize(it) }
        return normalize(BuildConfig.PINBACK_URL)
    }

    private fun normalize(raw: String): String {
        var u = raw.trim()
        if (!u.startsWith("http://", true) && !u.startsWith("https://", true)) u = "http://$u"
        return u.trimEnd('/')
    }

    private fun healthOk(url: String): Boolean {
        return try {
            val conn = URL("$url/healthz").openConnection() as HttpURLConnection
            conn.connectTimeout = 4000
            conn.readTimeout = 4000
            conn.requestMethod = "GET"
            conn.connect()
            val ok = conn.responseCode == 200
            conn.disconnect()
            ok
        } catch (_: Exception) {
            false
        }
    }

    private fun showCockpit(url: String) {
        inSetup = false
        prefs.edit().putString(PREF_URL, url).apply()
        web.loadUrl(url)
    }

    private fun showSetup(prefill: String) {
        inSetup = true
        web.loadUrl("file:///android_asset/setup.html")
        web.post {
            val esc = prefill.replace("\\", "\\\\").replace("'", "\\'")
            web.evaluateJavascript("document.getElementById('url').value='$esc';", null)
        }
    }

    private inner class SetupBridge {
        @JavascriptInterface
        fun getSavedUrl(): String = prefs.getString(PREF_URL, "") ?: ""

        @JavascriptInterface
        fun post(json: String) {
            val obj = try { JSONObject(json) } catch (_: Exception) { return }
            if (obj.optString("type") != "pinback-setup") return
            val action = obj.optString("action")
            val url = normalize(obj.optString("url"))
            if (url.isBlank()) return
            runOnUiThread {
                when (action) {
                    "test" -> {
                        val js = if (healthOk(url))
                            "document.getElementById('status').textContent='Server is reachable.';" +
                                "document.getElementById('status').className='ok';"
                        else
                            "document.getElementById('status').textContent='Cannot reach server.';" +
                                "document.getElementById('status').className='err';"
                        web.evaluateJavascript(js, null)
                    }
                    "connect" -> {
                        prefs.edit().putString(PREF_URL, url).apply()
                        if (healthOk(url)) showCockpit(url)
                        else web.evaluateJavascript(
                            "document.getElementById('status').textContent='Saved, but still unreachable.';" +
                                "document.getElementById('status').className='err';",
                            null
                        )
                    }
                }
            }
        }
    }

    private inner class Bridge {
        @JavascriptInterface
        fun post(json: String) {
            val canBack = try {
                JSONObject(json).optBoolean("canGoBack", false)
            } catch (_: Exception) {
                false
            }
            runOnUiThread { backCallback.isEnabled = canBack }
        }
    }

    private val backCallback = object : OnBackPressedCallback(false) {
        private var fromRight = true

        override fun handleOnBackStarted(backEvent: BackEventCompat) {
            fromRight = backEvent.swipeEdge == BackEventCompat.EDGE_RIGHT
            web.animate().cancel()
        }

        override fun handleOnBackProgressed(backEvent: BackEventCompat) {
            val p = backEvent.progress
            val dir = if (fromRight) 1f else -1f
            web.translationX = dir * p * (web.width * 0.18f)
            val s = 1f - 0.06f * p
            web.scaleX = s
            web.scaleY = s
            web.alpha = 1f - 0.25f * p
        }

        override fun handleOnBackPressed() {
            web.evaluateJavascript("window.pinback && window.pinback.back();", null)
            web.animate()
                .translationX(0f).scaleX(1f).scaleY(1f).alpha(1f)
                .setDuration(220L)
                .start()
        }

        override fun handleOnBackCancelled() {
            web.animate()
                .translationX(0f).scaleX(1f).scaleY(1f).alpha(1f)
                .setDuration(160L)
                .start()
        }
    }

    companion object {
        private const val PREF_URL = "server_url"
        private const val MENU_SERVER = 1
    }
}
