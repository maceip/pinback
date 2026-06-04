# Cockpit UI (`ui/app/`)

Shipped static UI for `pinback-server`: `index.html`, `app.js`, and vendored Shiki/Oniguruma under `vendor/shiki/`.

## Edit loop

```sh
./build/pinback-server --dev --agent-bin ./build/fake-ds4-agent --workspace "$(pwd)"
```

## Embed into the server binary

```sh
make embed
make
```

See `ui/shiki-bundle/README.md` to regenerate the Shiki bundle.
