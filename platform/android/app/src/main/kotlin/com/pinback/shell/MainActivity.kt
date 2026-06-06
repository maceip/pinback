package com.pinback.shell

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.webkit.JavascriptInterface
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
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
    private var cockpitUrl = ""

    private val prefs by lazy {
        getSharedPreferences("pinback", Context.MODE_PRIVATE)
    }

    @SuppressLint("SetJavaScriptEnabled", "JavascriptInterface")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        web = WebView(this).apply {
            webViewClient = object : WebViewClient() {
                @Deprecated("Deprecated in API 23")
                override fun onReceivedError(
                    view: WebView?,
                    errorCode: Int,
                    description: String?,
                    failingUrl: String?
                ) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) return
                    maybeShowSetupForLoadFailure(failingUrl)
                }

                override fun onReceivedError(
                    view: WebView?,
                    request: WebResourceRequest?,
                    error: WebResourceError?
                ) {
                    if (request == null || !request.isForMainFrame) return
                    maybeShowSetupForLoadFailure(request.url?.toString())
                }

                override fun onReceivedHttpError(
                    view: WebView?,
                    request: WebResourceRequest?,
                    errorResponse: WebResourceResponse?
                ) {
                    if (request == null || !request.isForMainFrame) return
                    val code = errorResponse?.statusCode ?: return
                    if (code >= 500) maybeShowSetupForLoadFailure(request.url?.toString())
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
        when {
            url.isBlank() -> showSetup("")
            healthOk(url) -> showCockpit(url)
            else -> showSetup(url)
        }
    }

    private fun maybeShowSetupForLoadFailure(failingUrl: String?) {
        if (inSetup || failingUrl == null || failingUrl == "about:blank") return
        if (failingUrl.startsWith("file:///android_asset/")) return
        runOnUiThread { showSetup(cockpitUrl.ifBlank { resolvedUrl() }) }
    }

    private fun defaultUrl(): String {
        if (isProbablyEmulator()) return "http://10.0.2.2:8088"
        return ""
    }

    private fun isProbablyEmulator(): Boolean {
        return (Build.FINGERPRINT.startsWith("generic")
            || Build.FINGERPRINT.startsWith("unknown")
            || Build.MODEL.contains("google_sdk", ignoreCase = true)
            || Build.MODEL.contains("Emulator", ignoreCase = true)
            || Build.MODEL.contains("Android SDK built for x86", ignoreCase = true)
            || Build.MANUFACTURER.contains("Genymotion", ignoreCase = true)
            || Build.HARDWARE.contains("goldfish", ignoreCase = true)
            || Build.HARDWARE.contains("ranchu", ignoreCase = true)
            || Build.PRODUCT.contains("sdk_google", ignoreCase = true)
            || Build.PRODUCT.contains("google_sdk", ignoreCase = true)
            || Build.PRODUCT.contains("emulator", ignoreCase = true)
            || Build.PRODUCT.contains("simulator", ignoreCase = true))
    }

    private fun resolvedUrl(): String {
        System.getenv("PINBACK_URL")?.takeIf { it.isNotBlank() }?.let { return normalize(it) }
        prefs.getString(PREF_URL, null)?.takeIf { it.isNotBlank() }?.let { return normalize(it) }
        val fromBuild = BuildConfig.PINBACK_URL.trim()
        if (fromBuild.isNotBlank()) return normalize(fromBuild)
        return defaultUrl()
    }

    private fun normalize(raw: String): String {
        var u = raw.trim()
        if (u.isBlank()) return ""
        if (!u.startsWith("http://", true) && !u.startsWith("https://", true)) u = "http://$u"
        return u.trimEnd('/')
    }

    private fun healthOk(url: String): Boolean {
        if (url.isBlank()) return false
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
        cockpitUrl = url
        prefs.edit().putString(PREF_URL, url).apply()
        web.loadUrl(url)
    }

    private fun showSetup(prefill: String) {
        inSetup = true
        web.loadUrl("file:///android_asset/setup.html")
        val seed = when {
            prefill.isNotBlank() -> prefill
            prefs.getString(PREF_URL, null)?.isNotBlank() == true -> prefs.getString(PREF_URL, "")!!
            else -> defaultUrl()
        }
        web.post {
            val esc = seed.replace("\\", "\\\\").replace("'", "\\'")
            web.evaluateJavascript("document.getElementById('url').value='$esc';", null)
        }
    }

    private inner class SetupBridge {
        @JavascriptInterface
        fun getSavedUrl(): String = prefs.getString(PREF_URL, "") ?: ""

        @JavascriptInterface
        fun getDefaultUrl(): String = defaultUrl()

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
            val obj = try { JSONObject(json) } catch (_: Exception) { return }
            if (obj.optString("type") == "pinback-host" &&
                obj.optString("action") == "openSetup"
            ) {
                runOnUiThread { showSetup(cockpitUrl.ifBlank { resolvedUrl() }) }
                return
            }
            val canBack = obj.optBoolean("canGoBack", false)
            runOnUiThread { backCallback.isEnabled = canBack }
        }

        @JavascriptInterface
        fun openServerSettings() {
            runOnUiThread { showSetup(cockpitUrl.ifBlank { resolvedUrl() }) }
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
            if (inSetup) {
                finish()
                return
            }
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
