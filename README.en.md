# MDCardTranslator — AI Card Description Translator Mod for Yu-Gi-Oh! Master Duel

[English](README.en.md) | [Tiếng Việt](README.md)

A lightweight native DLL proxy (`version.dll`) that translates **card effect descriptions** in **Yu-Gi-Oh! Master Duel** on the fly using any **OpenAI-compatible AI API**, while keeping card names in English.

## Features

- **Translate Descriptions Only (Keep Card Names in English):** Card names and archetype names inside double quotes (`"..."`) always remain in English for easy lookup and consistent gameplay.
- **Any OpenAI-Compatible AI API:** Reads the original English card text directly from Master Duel (supporting both **Pendulum Effects** and **Monster Effects**, including brand-new releases and Tokens) and translates asynchronously on a background worker thread with zero gameplay stutter.
- **Configurable Target Language (`target_language`):** Translate into any language (`Vietnamese`, `Indonesian`, `Thai`, `Spanish`, `French`, etc.) with automatic per-language cache files (`cards_vietnamese.json` for Vietnamese, `cards_<language>.json` for other languages).
- **Persistent Local Cache:** Each card is only translated **once** and saved locally so subsequent views are instant and free of API calls.
- **Automatic Newlines Per Effect:** Splits each separate effect, summoning condition, bullet point (`●`), and once-per-turn restriction onto its own line (`\n`) for much better readability.
- **Accurate Effect Highlight Alignment:** Maps active effects by sentence and punctuation so Master Duel's blue active-effect highlight and effect-number icons always align with the translated text.
- **Unicode / Diacritic Font Fallback:** Automatically attaches a system fallback font (`Arial` / `Segoe UI` / `Tahoma`) to TextMeshPro to prevent missing-glyph boxes (`□`).
- **Live UI Refresh & Non-Spam Error Notice:** Updates open card panels immediately once translation finishes. On API errors, displays a single concise red notice inside the card description instead of intrusive popups, and pauses automatically on fatal config errors (`HTTP 401/403/404`).
- **In-Game Hotkeys (`F9` / `F10`):**
  - **`F9`:** Re-translate the currently displayed card immediately (clears its old cache entry, shows `[CardAI] Re-translating with AI...`, prioritizes it at the front of the queue, and updates the UI + cache once done).
  - **`F10`:** Hot-reload `ai_config.json` (switch model, API URL, `target_language`, or `system_prompt`, and clear error state) **without restarting the game**.

---

## Installation & Configuration

### 1. Place Files in the Game Directory
Place the following in your `masterduel.exe` folder (typically `...\Steam\steamapps\common\Yu-Gi-Oh!  Master Duel\`):
- `version.dll` *(in the game root, next to `masterduel.exe`)*
- `MDCardTranslator\` folder containing `ai_config.json` (copy from `ai_config.example.json`) and helper scripts.

### 2. Configure AI API (`MDCardTranslator/ai_config.json`)
Open `MDCardTranslator\ai_config.json` and fill in your `api_key`. The default template is pre-configured for **Google Gemini**:

```json
{
  "api_url": "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
  "api_key": "YOUR_API_KEY_HERE",
  "model": "gemini-3.8-flash",
  "target_language": "Vietnamese",
  "timeout_ms": 30000,
  "delay_ms": 1500,
  "system_prompt": ""
}
```

#### Preset Endpoints for Popular Providers

| Provider | `api_url` | Suggested `model` | Notes |
| :--- | :--- | :--- | :--- |
| **Google Gemini** *(default)* | `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` | `gemini-3.8-flash` / `gemini-2.5-flash-lite` | Free tier available via Google AI Studio |
| **Groq** | `https://api.groq.com/openai/v1/chat/completions` | `llama-3.3-70b-versatile` | Free tier available, ultra-fast (~1s) |
| **OpenRouter** | `https://openrouter.ai/api/v1/chat/completions` | `google/gemini-2.5-flash` | One key for many models (some `:free`) |
| **OpenAI** | `https://api.openai.com/v1/chat/completions` | `gpt-4o-mini` | Paid, fast and reliable |
| **DeepSeek** | `https://api.deepseek.com/chat/completions` | `deepseek-chat` | Paid, low cost |
| **Ollama (Local)** | `http://localhost:11434/v1/chat/completions` | `qwen2.5:7b` / `llama3.1` | 100% free & offline, leave `api_key` empty |
| **LM Studio (Local)** | `http://localhost:1234/v1/chat/completions` | *(loaded model name)* | 100% free & offline, leave `api_key` empty |

#### Configuration Fields
- `api_url`: Full OpenAI-compatible `/chat/completions` endpoint URL.
- `api_key`: Provider API key (leave `""` for local `localhost` / `127.0.0.1` servers).
- `model`: Model identifier to use.
- `target_language`: Target translation language (default `"Vietnamese"` -> `cards_vietnamese.json`; any other language automatically uses `cards_<language>.json`).
- `reasoning_effort` *(optional)*: Override reasoning effort (e.g. `"low"`, `"medium"`, `"high"`). Defaults to `"low"` automatically for Gemini 3.x models to avoid thinking timeouts.
- `timeout_ms`: HTTP request timeout in milliseconds (default `30000`).
- `delay_ms`: Delay between consecutive API requests in milliseconds (default `1500`).
- `system_prompt`: Leave `""` to use the built-in Yu-Gi-Oh! translation prompt (optimized for 1-to-1 sentence alignment with Master Duel highlights), or enter your own custom prompt.

### 3. Test Your Config Without Launching the Game
Double-click **`test_ai_config.bat`** or run via PowerShell:

```powershell
.\test_ai_config.ps1
```

You can also pass custom card text:

```powershell
.\test_ai_config.ps1 -Text "Target 1 monster on the field; destroy it."
```

---

## Building from Source

Requirements: **Visual Studio 2022** or **Visual Studio 2022 Build Tools** (Desktop development with C++).

Run the build script:

```cmd
MDCardTranslator\src\build.bat
```

The compiled DLL will be output to `MDCardTranslator\src\out\version.dll`. Copy it to the game root directory next to `masterduel.exe`.

---

## Project Structure

```text
.
├── README.md                         # Vietnamese documentation
├── README.en.md                      # English documentation
├── version.dll                       # Pre-built DLL proxy (placed next to masterduel.exe)
└── MDCardTranslator/
    ├── ai_config.example.json        # Sample AI configuration template
    ├── ai_config.json                # AI configuration (copy from ai_config.example.json)
    ├── cards_<language>.json         # Per-language cached card descriptions (auto-created)
    ├── card_translator.log           # English log file
    ├── test_ai_config.bat            # Double-click launcher for testing AI config
    ├── test_ai_config.ps1            # Standalone PowerShell AI config tester
    └── src/
        ├── build.bat                 # MSVC x64 build script
        ├── dllmain.cpp               # Main source (IL2CPP hooks, AI client, highlight alignment, font fallback, hotkeys)
        └── minhook/                  # MinHook library
```

## Troubleshooting

- **Translation failed / API errors:** Check `MDCardTranslator\card_translator.log` (all log entries are in English) or read the red `[CardAI]` error line shown at the bottom of untranslated cards. After editing `MDCardTranslator\ai_config.json`, press **`F10`** in-game to hot-reload without restarting.
- **Manual edits:** You can edit any cached translation directly inside `MDCardTranslator\cards_<language>.json` (or press **`F9`** in-game to ask AI to re-translate the currently viewed card).
- **Uninstall:** Delete or rename `version.dll` in the game directory.
