package com.pinback.shell

import android.annotation.SuppressLint
import android.os.Bundle
import android.webkit.JavascriptInterface
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.BackEventCompat
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import org.json.JSONObject

// The entire Android shell: one Activity that fills the window with the system
// WebView (Chromium) and points it at the cockpit UI. No layout XML, no fragment.
//
// Sugar (2026): predictive back. The cockpit is a single-page view whose only
// navigation is *switching workspaces*, so "back" maps to "return to the
// previously-active workspace". The page maintains that MRU stack and tells us,
// over a JS bridge, whether a previous workspace exists (`canGoBack`). We only
// intercept back while it does; otherwise the system plays its back-to-home
// animation and the activity finishes. During the gesture we drive a custom
// peek (slide + scale + fade toward the swipe edge) so releasing reads as the
// previous workspace sliding back in — the in-app analogue of the system's
// cross-activity predictive preview. Needs no new dependencies: the predictive
// callbacks ship in androidx.activity, and the bridge uses framework JSON.
class MainActivity : ComponentActivity() {

    private lateinit var web: WebView

    @SuppressLint("SetJavaScriptEnabled", "JavascriptInterface")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val url = System.getenv("PINBACK_URL") ?: BuildConfig.PINBACK_URL

        web = WebView(this).apply {
            webViewClient = WebViewClient() // keep navigation inside the shell
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            addJavascriptInterface(Bridge(), "PinbackHost") // web -> native
            loadUrl(url)
        }
        setContentView(web)

        onBackPressedDispatcher.addCallback(this, backCallback)
    }

    /**
     * web -> native: the cockpit posts its workspace state on every change.
     * We only care about `canGoBack` here (whether an MRU previous workspace
     * exists) to gate our predictive-back interception.
     */
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

        // handleOnBack{Started,Progressed,Cancelled} are invoked by the system
        // only on Android 14+ (API 34); on older devices just handleOnBackPressed
        // fires, so this degrades to a plain (un-animated) workspace switch.
        override fun handleOnBackStarted(backEvent: BackEventCompat) {
            fromRight = backEvent.swipeEdge == BackEventCompat.EDGE_RIGHT
            web.animate().cancel()
        }

        override fun handleOnBackProgressed(backEvent: BackEventCompat) {
            val p = backEvent.progress            // 0f..1f, eased by the system
            val dir = if (fromRight) 1f else -1f
            web.translationX = dir * p * (web.width * 0.18f)
            val s = 1f - 0.06f * p
            web.scaleX = s
            web.scaleY = s
            web.alpha = 1f - 0.25f * p
        }

        override fun handleOnBackPressed() {
            // Commit: switch the cockpit to the previous workspace, then settle
            // the WebView from its peeked offset back to rest so the newly
            // activated workspace reads as having slid in.
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
}
