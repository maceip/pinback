# Shiki bundle toolchain

Infrequent dev-only project used to rebuild `ui/app/vendor/shiki/` (Shiki + Oniguruma WASM).

```sh
cd ui/shiki-bundle
npm install
# rebuild shiki.mjs + onig.wasm into ../app/vendor/shiki/ (see package scripts or manual esbuild invocation)
```

Then from repo root:

```sh
make embed
make
```
