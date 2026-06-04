package com.pinback.shell

import android.annotation.SuppressLint
import android.os.Bundle
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge

// The entire Android shell: one Activity that fills the window with the system
// WebView (Chromium) and points it at the cockpit UI. No layout XML, no fragment.
class MainActivity : ComponentActivity() {

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val url = System.getenv("PINBACK_URL") ?: BuildConfig.PINBACK_URL

        val web = WebView(this).apply {
            webViewClient = WebViewClient() // keep navigation inside the shell
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            loadUrl(url)
        }
        setContentView(web)
    }
}
