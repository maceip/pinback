// Curated Shiki bundle for pinback's chat code-block highlighter.
// Languages chosen to match what DS4 typically emits in coding tasks.
// Themes: one dark + one light, to follow the page's color-scheme.
//
// We use the Oniguruma WASM engine (the same one Jules / VS Code use) so
// the tokenization is byte-for-byte identical to VS Code's TextMate output.
// onig.wasm is shipped precompiled by upstream (vscode-oniguruma); pinback
// just bundles it as a static asset.

import { createHighlighterCore } from 'shiki/core'
import { createOnigurumaEngine } from 'shiki/engine/oniguruma'

import bash       from '@shikijs/langs/bash'
import c          from '@shikijs/langs/c'
import cpp        from '@shikijs/langs/cpp'
import css        from '@shikijs/langs/css'
import diff       from '@shikijs/langs/diff'
import dockerfile from '@shikijs/langs/docker'
import go         from '@shikijs/langs/go'
import html       from '@shikijs/langs/html'
import java       from '@shikijs/langs/java'
import javascript from '@shikijs/langs/javascript'
import json       from '@shikijs/langs/json'
import jsx        from '@shikijs/langs/jsx'
import lua        from '@shikijs/langs/lua'
import makefile   from '@shikijs/langs/makefile'
import markdown   from '@shikijs/langs/markdown'
import nginx      from '@shikijs/langs/nginx'
import php        from '@shikijs/langs/php'
import python     from '@shikijs/langs/python'
import ruby       from '@shikijs/langs/ruby'
import rust       from '@shikijs/langs/rust'
import sql        from '@shikijs/langs/sql'
import swift      from '@shikijs/langs/swift'
import toml       from '@shikijs/langs/toml'
import tsx        from '@shikijs/langs/tsx'
import typescript from '@shikijs/langs/typescript'
import yaml       from '@shikijs/langs/yaml'

import vitesseDark  from '@shikijs/themes/vitesse-dark'
import vitesseLight from '@shikijs/themes/vitesse-light'

let highlighterPromise = null

// onigInput may be a URL string (browser hot path), an ArrayBuffer /
// Uint8Array (node test path), or an already-instantiated WebAssembly
// module / instance. The same bundle thus serves both pinback in the
// browser AND the node-side fidelity tests, which is what lets us catch
// long-markdown regressions without spinning up a real browser.
export async function getPinbackHighlighter(onigInput) {
  if (highlighterPromise) return highlighterPromise
  highlighterPromise = (async () => {
    let wasmBytes = onigInput
    if (typeof onigInput === 'string') {
      const wasmResponse = await fetch(onigInput)
      if (!wasmResponse.ok) {
        throw new Error('failed to fetch onig.wasm: ' + wasmResponse.status)
      }
      wasmBytes = await wasmResponse.arrayBuffer()
    }
    const engine = await createOnigurumaEngine(wasmBytes)
    return createHighlighterCore({
      engine,
      themes: [vitesseDark, vitesseLight],
      langs: [
        bash, c, cpp, css, diff, dockerfile, go, html, java, javascript,
        json, jsx, lua, makefile, markdown, nginx, php, python, ruby,
        rust, sql, swift, toml, tsx, typescript, yaml,
      ],
    })
  })()
  return highlighterPromise
}

// Convenience: highlight a single block synchronously after init.
export function highlightToHtml(highlighter, code, lang, theme) {
  // Shiki normalizes unknown langs to "txt"; check ourselves so the UI can
  // fall back to a plain <pre> if we don't ship a grammar for `lang`.
  const known = highlighter.getLoadedLanguages()
  const useLang = known.includes(lang) ? lang : 'txt'
  return highlighter.codeToHtml(code, {
    lang: useLang,
    theme: theme || 'vitesse-dark',
  })
}
