# Ditto — Custom Build (Vietnamese · Slash Filters · Minimalist UI)

A personalized fork of [sabrogden/Ditto](https://github.com/sabrogden/Ditto), the open-source clipboard manager for Windows. This fork ships a customized build on top of upstream `master`: full Vietnamese localization, command-style search filters, a refined white theme, and a set of daily-driver workflow fixes.

> All credit for the core product belongs to Scott Brogden (sabrogden) and upstream contributors. This repository only carries the customizations listed below.

## What's different from upstream

| Area | Change |
|---|---|
| Localization | Complete Vietnamese UI translation — 581 strings / 18 sections, set as the default language |
| Theme | New **Minimalist** theme (flat white, black text, hairline gray row separators, light-gray selection) set as default |
| Search | Slash type filters: `/image`, `/file`, `/text`, `/link`, `/star`, `/today` |
| Save | **Smart Ctrl+S** — saves the selected clip to a file, auto-picking the format: image → `.png`, text → `.txt`; `Ctrl+Shift+S` forces image export |
| Quick handoff | **Ctrl+X** in the clip list — exports one selected image as `.png` or text as UTF-8 `.txt` to `%TEMP%\Ditto-X\Handoff`, then copies the absolute path without a save dialog; the path temporarily replaces the `/image /link /today` placeholder, then restores it after 1.8 seconds |
| Public clip link | **Ctrl+Z** in the clip list — publishes the selected clip (image, or text with embedded images) as an expiring public link on `share.quoc.app/clip/` (12 hours), copies the URL, and shows it briefly in the search box; republishing identical content reuses the live link. Requires a one-time local credential via `tools/dittox-share-credential-setup.ps1` — without it, Ctrl+Z does nothing |
| Editor | `Ctrl+E` opens the clip editor (centered window, remembers position, caret at end of content); `Ctrl+Q` saves and closes: an edited clip re-selects in the list, a new clip is inserted with default metadata (no Properties dialog), and an empty new tab is discarded |
| List | ~40% taller rows, hairline separators, larger image thumbnails |
| Popup | Window remembers its last position instead of jumping to the cursor |
| Icons | New rounded app icon plus a disconnected-tray variant |
| Build | Platform toolset fallback v145 → v143 for local VS 2022 builds |

## Slash filter grammar

A filter is recognized only as the **first token** of the search box. Anything else is treated as a plain text search.

| Query | Result |
|---|---|
| `/image` | Clips containing an image format |
| `/image logo` | Image clips matching `logo` |
| `/file` | File drops (CF_HDROP) |
| `/text` | Text clips |
| `/link` | Clips starting with `http` |
| `/star` | Starred (never auto-delete) clips |
| `/today` | Clips copied today |
| `/image /today` | One type filter combined with date/star |
| `/images` | Unknown prefix → plain text search |
| `/q name` | Upstream Quick Paste filter (unchanged) |

## Download & run

Grab the latest `Ditto-custom-x64.zip` from [Releases](https://github.com/nguyendinhquocx/Ditto-X/releases), extract, and run `Ditto-custom\Ditto.exe` — no installer needed. An existing Ditto database (`%APPDATA%\Ditto\Ditto.db`) is picked up automatically.

Requirements: Windows 10/11 x64 and the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Build from source

- Visual Studio 2022 with the **MFC and ATL** components (v143 toolset)
- NuGet packages (`libpng`, `zlib`) restore automatically

```
vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"
msbuild CP_Main_10.sln /p:Configuration=Release /p:Platform=x64 /p:RestorePackagesConfig=true /m
```

Output lands in `Release64\`. Exit any running Ditto instance before rebuilding (file lock).

## Branch & upstream policy

- `master` — the customized default branch; intentionally diverges from upstream.
- `ai/*` — feature branches (for example `ai/slash-filter`), merged into `master` after review.
- **This fork is not auto-synced with upstream.** Upstream changes are pulled manually per milestone: both codebases patch `src/QPasteWnd.cpp` heavily, so an automated merge would silently resolve conflicts on our side and drop upstream fixes. When contributing back upstream (translations, type filters), clean branches are cut from `upstream/master`, not from `master`.

## License

GPL-3.0, inherited from upstream. See [LICENSE](LICENSE).
