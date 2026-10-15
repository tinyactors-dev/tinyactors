# tinyactors brand pack

Generated from `assets/logo.svg` (symbol) and IBM Plex Mono Medium, tracking −0.04em (wordmark, converted to outlines). All lockups are cropped close to the artwork with no built-in margin. Leave clear space when placing them (see the root readme).

## Lockups
`symbol` · `horizontal` (symbol + wordmark side by side) · `stacked` (symbol over wordmark) · `wordmark`

## Colourways (transparent backgrounds)
- `ink`: graphite `#302A26` + pine `#265E47` details. Use on vellum and light surfaces.
- `paper`: `#FFF8F0` + pine-100 `#D8E8DF` details. Use on pine `#1F5E4A`.
- `parchment`: `#F1E6D2` + pine-400 `#5FAE8F` details. Use on umber and dark surfaces.
- `pine`: pine-500 `#1F5E4A` with pine-700 `#133D30` details. A one-hue brand colour for light surfaces.
- `mono` (SVG only): `currentColor`, so it inherits the CSS text colour.

## Tiles (with clear space)
`tiles/tinyactors-{lockup}-on-{vellum|pine|umber}`, as SVG plus PNG and WebP at the raster widths. Each is the lockup on a solid background, with the clear space built in as padding: ¼ of the symbol height, or ¾ of the font size for the wordmark alone. Colours are ink on vellum `#F4EEE2`, paper on pine `#1F5E4A`, and parchment on umber `#18120D`.

## Files
- `svg/tinyactors-{lockup}-{colourway}.svg`: 20 files.
- `raster/tinyactors-{lockup}-{colourway}-{width}w.{png,webp}`
  - symbol and stacked at 512 and 1024 px wide
  - horizontal and wordmark at 1024 and 2048 px wide
- `favicon/`
  - `favicon.ico` (16, 32, 48)
  - `favicon-16.png`, `favicon-32.png`, `favicon-48.png`
  - `favicon.svg`
  - `apple-touch-icon.png` (180)
  - All favicons sit on a vellum tile, so they read on light and dark browser tabs.
- `pwa/`
  - `icon-192` and `icon-512` (transparent, purpose "any")
  - `maskable-vellum-192/512` and `maskable-pine-192/512`: full-bleed background, symbol inside the 80% safe circle
  - `site.webmanifest`
  - All icons come in PNG and WebP.

## HTML
```html
<link rel="icon" href="/favicon.ico" sizes="48x48">
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="/apple-touch-icon.png">
<link rel="manifest" href="/site.webmanifest">
<meta name="theme-color" content="#1F5E4A">
```

## Known limits
- At 16 px the ant's antennae and legs blur together. A simplified small-size symbol would be sharper.
- `favicon.svg` has no dark-mode variant. It uses the vellum tile instead.
