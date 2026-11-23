// MDCardTranslator (Yu-Gi-Oh! Master Duel AI Edition) - Native version.dll proxy mod
// that translates card descriptions in real time via any OpenAI-compatible AI API,
// caches translations per language in cards_<language>.json (keeping card names in English),
// auto-formats newlines per effect, aligns active-effect highlights, and loads a Unicode fallback font.

#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include "minhook/include/MinHook.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")

// Chuyển tiếp toàn bộ 17 export của version.dll về System32\version.dll
#define FWD(n) __pragma(comment(linker, "/export:" #n "=C:\\Windows\\System32\\version." #n))
FWD(GetFileVersionInfoA) FWD(GetFileVersionInfoByHandle) FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW) FWD(GetFileVersionInfoSizeA) FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW) FWD(GetFileVersionInfoSizeW) FWD(GetFileVersionInfoW)
FWD(VerFindFileA) FWD(VerFindFileW) FWD(VerInstallFileA) FWD(VerInstallFileW)
FWD(VerLanguageNameA) FWD(VerLanguageNameW) FWD(VerQueryValueA) FWD(VerQueryValueW)

// -------------------- Đường dẫn & Ghi log --------------------
static char g_baseDir[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_cachePath[MAX_PATH];
static char g_configPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;

static void Log(const char* fmt, ...)
{
    EnterCriticalSection(&g_logCs);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// -------------------- Chuẩn hóa chuỗi & HTML --------------------
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

static std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Xóa thẻ <ruby>...</ruby> hoặc các thẻ HTML/TMP khỏi tên bài nếu có
static std::string StripTags(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '<') {
            size_t close = raw.find('>', i + 1);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

// Chuẩn hóa tên bài để tra cứu & gọi URL (đổi en-dash/em-dash và fullwidth sang ASCII)
static std::string NormalizeCardName(const std::string& raw)
{
    std::string s = StripTags(raw);
    ReplaceAll(s, "\xE2\x80\x93", "-"); // –
    ReplaceAll(s, "\xE2\x80\x94", "-"); // —
    ReplaceAll(s, "\xE2\x88\x92", "-"); // −
    ReplaceAll(s, "\xE2\x80\x99", "'"); // ’
    ReplaceAll(s, "\xE2\x80\x98", "'"); // ‘
    ReplaceAll(s, "\xE2\x80\x9C", "\""); // “
    ReplaceAll(s, "\xE2\x80\x9D", "\""); // ”
    return Trim(s);
}

static void AppendUtf8Codepoint(std::string& out, unsigned int cp)
{
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring out((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], wlen);
    return out;
}

// Tách phần Pendulum Effect và Monster/Card Effect từ chuỗi viText trong cache
static void SplitPendulumAndMonsterText(const std::string& viText, std::string& outPendulum, std::string& outMonster)
{
    outPendulum.clear();
    outMonster = viText;

    const char* monsterMarkers[] = {
        "\n----------------------------------------\n[ Monster Effect ]\n",
        "----------------------------------------\n[ Monster Effect ]",
        "[ Monster Effect ]",
        "[ Hi\xE1\xBB\x87u \xE1\xBB\xA9ng qu\xC3\xA1i th\xC3\xBA ]",
        "[ Hi\xE1\xBB\x87u \xE1\xBB\xA9ng Qu\xC3\xA1i th\xC3\xBA ]",
        "[ Flavor Text ]"
    };

    size_t splitPos = std::string::npos;
    size_t markerLen = 0;
    for (const char* m : monsterMarkers) {
        size_t p = viText.find(m);
        if (p != std::string::npos) {
            splitPos = p;
            markerLen = strlen(m);
            break;
        }
    }

    if (splitPos != std::string::npos) {
        std::string pendPart = viText.substr(0, splitPos);
        std::string monPart  = viText.substr(splitPos + markerLen);

        ReplaceAll(pendPart, "[ Pendulum Effect ]", "");
        ReplaceAll(pendPart, "[ Hi\xE1\xBB\x87u \xE1\xBB\xA9ng Pendulum ]", "");
        ReplaceAll(pendPart, "----------------------------------------", "");

        outPendulum = Trim(pendPart);
        outMonster  = Trim(monPart);
    } else if (viText.find("[ Pendulum Effect ]") != std::string::npos) {
        std::string tmp = viText;
        ReplaceAll(tmp, "[ Pendulum Effect ]", "");
        outMonster = Trim(tmp);
    }
}

// -------------------- Đọc / Ghi file JSON cache --------------------
static std::mutex g_cacheMutex;
static std::unordered_map<std::string, std::string> g_cache;       // cardName -> viText
static std::unordered_set<std::string> g_notFoundOrPending;        // tránh gửi dịch lặp lại

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8]; sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

static bool ParseJsonString(const std::string& src, size_t& i, std::string& out)
{
    out.clear();
    if (i >= src.size() || src[i] != '"') return false;
    ++i;
    while (i < src.size()) {
        char c = src[i++];
        if (c == '"') return true;
        if (c == '\\' && i < src.size()) {
            char e = src[i++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':
                    if (i + 4 <= src.size()) {
                        unsigned int cp = (unsigned int)strtoul(src.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        // Hỗ trợ UTF-16 surrogate pair (\uD800..\uDBFF theo sau bởi \uDC00..\uDFFF)
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= src.size() && src[i] == '\\' && src[i + 1] == 'u') {
                            unsigned int low = (unsigned int)strtoul(src.substr(i + 2, 4).c_str(), nullptr, 16);
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                i += 6;
                            }
                        }
                        AppendUtf8Codepoint(out, cp);
                    }
                    break;
                default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

static void LoadCacheFile(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    size_t i = 0;
    int count = 0;
    while (i < content.size()) {
        size_t q = content.find('"', i);
        if (q == std::string::npos) break;
        i = q;
        std::string key;
        if (!ParseJsonString(content, i, key)) break;
        while (i < content.size() && (unsigned char)content[i] <= ' ') ++i;
        if (i >= content.size() || content[i] != ':') continue;
        ++i;
        while (i < content.size() && (unsigned char)content[i] <= ' ') ++i;
        std::string val;
        if (!ParseJsonString(content, i, val)) break;
        if (!key.empty() && !val.empty()) {
            g_cache[NormalizeCardName(key)] = val;
            ++count;
        }
    }
    Log("Loaded %d cards from cache: %s", count, path);
}

static void SaveCacheFileLocked()
{
    FILE* f = nullptr;
    if (fopen_s(&f, g_cachePath, "wb") != 0 || !f) return;
    fprintf(f, "{\n");
    size_t idx = 0, total = g_cache.size();
    for (const auto& kv : g_cache) {
        ++idx;
        std::string k = JsonEscape(kv.first);
        std::string v = JsonEscape(kv.second);
        fprintf(f, "  \"%s\": \"%s\"%s\n", k.c_str(), v.c_str(), (idx < total ? "," : ""));
    }
    fprintf(f, "}\n");
    fclose(f);
}

// -------------------- Cấu hình & Dịch bằng AI API (OpenAI-compatible) --------------------
struct AiConfig {
    std::string apiUrl;
    std::string apiKey;
    std::string model;
    std::string targetLanguage;
    std::string reasoningEffort;
    std::string systemPrompt;
    int timeoutMs = 30000;
    int delayMs = 1500;
    bool loaded = false;
};

static std::mutex g_aiConfigMutex;
static AiConfig g_aiConfig;
static bool g_warnedMissingApiKey = false;

static bool IsVietnameseLang(const std::string& lang)
{
    std::string low = lang;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return low.empty() || low == "vi" || low == "vietnamese" ||
           low.find("viet") != std::string::npos ||
           low.find("vi\xE1\xBB\x87t") != std::string::npos;
}

static std::string BuildDefaultSystemPrompt(const std::string& targetLang)
{
    std::string lang = targetLang.empty() ? "Vietnamese" : targetLang;
    if (IsVietnameseLang(lang)) {
        return
            "Bạn là chuyên gia dịch thuật thẻ bài Yu-Gi-Oh! sang Tiếng Việt theo văn phong tự nhiên, ngắn gọn, chính xác của cộng đồng Yu-Gi-Oh! Việt Nam.\n"
            "Quy tắc bắt buộc:\n"
            "1. CHỈ sử dụng Tiếng Việt chuẩn và các thuật ngữ tiếng Anh trong mục 2 & 3. TUYỆT ĐỐI KHÔNG trộn lẫn ký tự ngôn ngữ khác (như tiếng Thái, Trung, Nhật, Hàn...). Luôn đặt đúng dấu cách giữa từ Tiếng Việt và thuật ngữ tiếng Anh (không viết dính chữ).\n"
            "2. GIỮ NGUYÊN tiếng Anh đối với mọi tên lá bài và tên archetype nằm trong dấu ngoặc kép \"...\".\n"
            "3. Chỉ giữ nguyên tiếng Anh đối với các thuật ngữ cốt lõi của Yu-Gi-Oh!: Special Summon, Normal Summon, Fusion/Synchro/Xyz/Link/Pendulum Summon, GY, Banish, Quick Effect, ATK, DEF, LP, Deck, Extra Deck, Field, Main Phase, Battle Phase, End Phase, Spell, Trap, Tuner, Token, Counter, Material. BẮT BUỘC dịch sang Tiếng Việt các động từ và danh từ thông thường (ví dụ: discard -> bỏ, negate -> vô hiệu hóa, destroy -> phá hủy, target -> chọn mục tiêu, draw -> rút, send -> gửi, add -> thêm, turn -> lượt, hand -> tay, once per turn -> một lần mỗi lượt).\n"
            "4. GIỮ NGUYÊN cấu trúc câu và dấu câu (dấu chấm '.', dấu hai chấm ':', dấu chấm phẩy ';', ký hiệu '●') khớp 1-1 với bản tiếng Anh: không gộp 2 câu thành 1 và không tách 1 câu thành 2 để tương thích với hệ thống highlight hiệu ứng trong game.\n"
            "5. Nếu đầu vào có phần [ Pendulum Effect ] và [ Monster Effect ] ngăn cách bởi ----------------------------------------, hãy giữ nguyên chính xác các dòng tiêu đề và dấu ngăn cách đó.\n"
            "6. Chỉ trả về đúng phần văn bản mô tả đã dịch, không kèm lời giải thích, không kèm tên lá bài và không bọc trong markdown code block.";
    }

    return
        "You are an expert Yu-Gi-Oh! card text translator. Translate the English card effect text into natural, concise, accurate " + lang + ".\n"
        "Strict rules:\n"
        "1. Write ONLY in " + lang + " (plus the English terms allowed in rules 2 & 3). NEVER leak characters from any unrelated language, and always keep proper spacing around English terms.\n"
        "2. Keep all card names and archetype names inside double quotes \"...\" strictly in English.\n"
        "3. Translate all common verbs and nouns (such as discard, negate, destroy, target, draw, send, add, turn, hand, once per turn) naturally into " + lang + ". Only keep core Yu-Gi-Oh! game terms in English where standard for the " + lang + " community (e.g., ATK, DEF, LP, GY, Quick Effect, Special Summon, Extra Deck).\n"
        "4. Preserve exact 1-to-1 sentence structure and punctuation ('.', ':', ';', '●') matching the English text: do NOT merge two sentences into one and do NOT split one sentence into two.\n"
        "5. If the input contains [ Pendulum Effect ] and [ Monster Effect ] separated by ----------------------------------------, keep those exact headers and separator line unchanged.\n"
        "6. Output ONLY the translated description text, with no explanations, no card name header, and no markdown code fences.";
}

static void UpdateCachePathForLanguage(const std::string& targetLang)
{
    if (IsVietnameseLang(targetLang)) {
        sprintf_s(g_cachePath, "%scards_vietnamese.json", g_baseDir);
        return;
    }
    std::string slug;
    for (unsigned char c : targetLang) {
        if ((c >= 'A' && c <= 'Z')) slug.push_back((char)(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug.push_back((char)c);
        else if (c == ' ' || c == '-' || c == '_') slug.push_back('_');
    }
    if (slug.empty()) slug = "custom";
    sprintf_s(g_cachePath, "%scards_%s.json", g_baseDir, slug.c_str());
}

static void LoadOrCreateAiConfig(const char* path)
{
    std::lock_guard<std::mutex> lk(g_aiConfigMutex);
    // Giá trị mặc định: Google Gemini OpenAI-compatible endpoint
    g_aiConfig.apiUrl = "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions";
    g_aiConfig.apiKey = "";
    g_aiConfig.model  = "gemini-3.8-flash";
    g_aiConfig.targetLanguage = "Vietnamese";
    g_aiConfig.reasoningEffort = "";
    g_aiConfig.systemPrompt = "";
    g_aiConfig.timeoutMs = 30000;
    g_aiConfig.delayMs = 1500;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        // Tự động tạo file cấu hình mẫu để người dùng chỉ cần dán api_key
        FILE* wf = nullptr;
        if (fopen_s(&wf, path, "wb") == 0 && wf) {
            fprintf(wf,
                "{\n"
                "  \"api_url\": \"https://generativelanguage.googleapis.com/v1beta/openai/chat/completions\",\n"
                "  \"api_key\": \"YOUR_API_KEY_HERE\",\n"
                "  \"model\": \"gemini-3.8-flash\",\n"
                "  \"target_language\": \"Vietnamese\",\n"
                "  \"timeout_ms\": 30000,\n"
                "  \"delay_ms\": 1500,\n"
                "  \"system_prompt\": \"\"\n"
                "}\n");
            fclose(wf);
        }
        g_aiConfig.systemPrompt = BuildDefaultSystemPrompt(g_aiConfig.targetLanguage);
        UpdateCachePathForLanguage(g_aiConfig.targetLanguage);
        Log("[AI] Config not found at %s -> created starter config. Please open it and fill in your api_key!", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        g_aiConfig.systemPrompt = BuildDefaultSystemPrompt(g_aiConfig.targetLanguage);
        UpdateCachePathForLanguage(g_aiConfig.targetLanguage);
        return;
    }
    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    size_t i = 0;
    while (i < content.size()) {
        size_t q = content.find('"', i);
        if (q == std::string::npos) break;
        i = q;
        std::string key;
        if (!ParseJsonString(content, i, key)) break;
        while (i < content.size() && (unsigned char)content[i] <= ' ') ++i;
        if (i >= content.size() || content[i] != ':') continue;
        ++i;
        while (i < content.size() && (unsigned char)content[i] <= ' ') ++i;
        if (i >= content.size()) break;

        if (content[i] == '"') {
            std::string val;
            if (!ParseJsonString(content, i, val)) break;
            if (key == "api_url" && !val.empty()) g_aiConfig.apiUrl = Trim(val);
            else if (key == "api_key") g_aiConfig.apiKey = Trim(val);
            else if (key == "model" && !val.empty()) g_aiConfig.model = Trim(val);
            else if (key == "target_language" && !Trim(val).empty()) g_aiConfig.targetLanguage = Trim(val);
            else if (key == "reasoning_effort") g_aiConfig.reasoningEffort = Trim(val);
            else if (key == "system_prompt" && !Trim(val).empty()) g_aiConfig.systemPrompt = val;
        } else {
            size_t numStart = i;
            while (i < content.size() && ((content[i] >= '0' && content[i] <= '9') || content[i] == '-')) ++i;
            int numVal = atoi(content.substr(numStart, i - numStart).c_str());
            if (key == "timeout_ms" && numVal >= 1000) g_aiConfig.timeoutMs = numVal;
            else if (key == "delay_ms" && numVal >= 0) g_aiConfig.delayMs = numVal;
        }
    }

    if (g_aiConfig.systemPrompt.empty()) {
        g_aiConfig.systemPrompt = BuildDefaultSystemPrompt(g_aiConfig.targetLanguage);
    }
    UpdateCachePathForLanguage(g_aiConfig.targetLanguage);

    g_aiConfig.loaded = true;
    Log("[AI] Loaded config from %s | url=%s | model=%s | lang=%s | cache=%s | key=%s",
        path,
        g_aiConfig.apiUrl.c_str(),
        g_aiConfig.model.c_str(),
        g_aiConfig.targetLanguage.c_str(),
        g_cachePath,
        (g_aiConfig.apiKey.empty() || g_aiConfig.apiKey == "YOUR_API_KEY_HERE") ? "(not set / local)" : "(configured)");
}

// -------------------- Quản lý thông báo lỗi AI (chống spam) --------------------
enum AiHttpResult {
    AI_OK = 0,
    AI_ERR_TRANSIENT,   // Lỗi tạm thời (mất mạng, timeout, 5xx) -> thử lại được
    AI_ERR_RATE_LIMIT,  // HTTP 429 -> tạm nghỉ dài hơn rồi thử lại
    AI_ERR_FATAL        // Lỗi cấu hình (401/403/404, sai URL, thiếu key) -> tạm dừng gửi để không spam
};

static std::mutex g_aiErrorMutex;
static std::string g_lastAiError;
static std::string g_lastLoggedError;
static DWORD g_lastErrorLogTick = 0;
static volatile bool g_aiFatalPaused = false;
static volatile bool g_pendingUIRefresh = false;

static std::string TruncateUtf8Safe(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// Trích xuất trường "message" ngắn gọn từ JSON lỗi của API (OpenAI / Gemini / DeepSeek / Groq...)
static std::string ExtractApiErrorMessage(const std::string& resp)
{
    size_t pos = resp.find("\"message\"");
    if (pos != std::string::npos) {
        size_t i = pos + 9;
        while (i < resp.size() && (unsigned char)resp[i] <= ' ') ++i;
        if (i < resp.size() && resp[i] == ':') {
            ++i;
            while (i < resp.size() && (unsigned char)resp[i] <= ' ') ++i;
            if (i < resp.size() && resp[i] == '"') {
                std::string msg;
                if (ParseJsonString(resp, i, msg) && !msg.empty()) {
                    ReplaceAll(msg, "\r", " ");
                    ReplaceAll(msg, "\n", " ");
                    return TruncateUtf8Safe(Trim(msg), 180);
                }
            }
        }
    }
    std::string fallback = resp;
    ReplaceAll(fallback, "\r", " ");
    ReplaceAll(fallback, "\n", " ");
    return TruncateUtf8Safe(Trim(fallback), 160);
}

static void SetAiError(const std::string& errText, bool isFatal)
{
    bool shouldRefresh = false;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lk(g_aiErrorMutex);
        if (g_lastAiError != errText) {
            g_lastAiError = errText;
            shouldRefresh = true;
        }
        DWORD now = GetTickCount();
        if (g_lastLoggedError != errText || (now - g_lastErrorLogTick) >= 30000) {
            g_lastLoggedError = errText;
            g_lastErrorLogTick = now;
            shouldLog = true;
        }
        if (isFatal) g_aiFatalPaused = true;
    }
    if (shouldLog) {
        Log("[AI ERROR%s] %s", isFatal ? " - PAUSED" : "", errText.c_str());
    }
    if (shouldRefresh) {
        g_pendingUIRefresh = true;
    }
}

static void ClearAiError()
{
    std::lock_guard<std::mutex> lk(g_aiErrorMutex);
    g_lastAiError.clear();
    g_lastLoggedError.clear();
    g_aiFatalPaused = false;
}

static std::string g_retranslatingCardName;

static std::string GetLastAiError()
{
    std::lock_guard<std::mutex> lk(g_aiErrorMutex);
    return g_lastAiError;
}

static void SetRetranslatingCard(const std::string& cardName)
{
    std::lock_guard<std::mutex> lk(g_aiErrorMutex);
    g_retranslatingCardName = cardName;
}

static void ClearRetranslatingCardIfMatches(const std::string& cardName)
{
    std::lock_guard<std::mutex> lk(g_aiErrorMutex);
    if (g_retranslatingCardName == cardName) {
        g_retranslatingCardName.clear();
    }
}

static bool IsCardRetranslating(const std::string& cardName)
{
    if (cardName.empty()) return false;
    std::lock_guard<std::mutex> lk(g_aiErrorMutex);
    return !g_retranslatingCardName.empty() && g_retranslatingCardName == cardName;
}

static AiConfig GetAiConfigSnapshot()
{
    std::lock_guard<std::mutex> lk(g_aiConfigMutex);
    return g_aiConfig;
}

// Bóc tách choices[0].message.content từ JSON phản hồi chuẩn OpenAI Chat Completions
static bool ExtractAssistantContentFromJson(const std::string& json, std::string& outContent)
{
    size_t choicesPos = json.find("\"choices\"");
    size_t searchStart = (choicesPos != std::string::npos) ? choicesPos : 0;

    size_t msgKeyPos = json.find("\"message\"", searchStart);
    if (msgKeyPos == std::string::npos) return false;

    size_t bracePos = json.find('{', msgKeyPos + 9);
    if (bracePos == std::string::npos) return false;

    int depth = 0;
    size_t i = bracePos;
    while (i < json.size()) {
        char c = json[i];
        if (c == '{') {
            depth++;
            i++;
        } else if (c == '}') {
            depth--;
            i++;
            if (depth <= 0) break;
        } else if (c == '"') {
            std::string str;
            if (!ParseJsonString(json, i, str)) break;
            if (depth == 1 && str == "content") {
                while (i < json.size() && (unsigned char)json[i] <= ' ') ++i;
                if (i < json.size() && json[i] == ':') {
                    ++i;
                    while (i < json.size() && (unsigned char)json[i] <= ' ') ++i;
                    if (i < json.size() && json[i] == '"') {
                        return ParseJsonString(json, i, outContent);
                    }
                }
            }
        } else {
            i++;
        }
    }
    return false;
}

static std::string CleanAiTranslationOutput(std::string text)
{
    ReplaceAll(text, "\r\n", "\n");
    ReplaceAll(text, "\r", "\n");
    text = Trim(text);

    // Gỡ bỏ markdown code fence nếu AI vô tình bọc ```...```
    if (text.size() >= 6 && text.rfind("```", 0) == 0) {
        size_t nl = text.find('\n');
        if (nl != std::string::npos) {
            text = text.substr(nl + 1);
            size_t endFence = text.rfind("```");
            if (endFence != std::string::npos) {
                text = text.substr(0, endFence);
            }
            text = Trim(text);
        }
    }
    return text;
}

static AiHttpResult HttpPostOpenAICompatible(const std::string& cardName, const std::string& engDesc, std::string& outViText)
{
    AiConfig cfg = GetAiConfigSnapshot();

    if (cfg.apiUrl.empty() || cfg.model.empty()) {
        SetAiError("Missing api_url or model in ai_config.json", true);
        return AI_ERR_FATAL;
    }

    bool isLocalUrl = (cfg.apiUrl.find("localhost") != std::string::npos ||
                       cfg.apiUrl.find("127.0.0.1") != std::string::npos);
    if (!isLocalUrl && (cfg.apiKey.empty() || cfg.apiKey == "YOUR_API_KEY_HERE")) {
        SetAiError("Missing api_key in ai_config.json", true);
        return AI_ERR_FATAL;
    }

    std::wstring wUrl = Utf8ToWide(cfg.apiUrl);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[1024] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.size(), 0, &uc)) {
        SetAiError("Invalid api_url in ai_config.json", true);
        return AI_ERR_FATAL;
    }

    std::wstring wHost(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring wPath(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (wPath.empty()) wPath = L"/";
    bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    // Xây dựng JSON payload chuẩn OpenAI Chat Completions
    std::string body = "{";
    body += "\"model\":\"" + JsonEscape(cfg.model) + "\",";
    body += "\"temperature\":0.1,";
    std::string effort = cfg.reasoningEffort;
    if (effort.empty() &&
        cfg.apiUrl.find("generativelanguage.googleapis.com") != std::string::npos &&
        cfg.model.find("gemini-3") != std::string::npos)
    {
        // Gemini 3.x mặc định thinking MEDIUM gây chậm/timeout 30s -> tự động hạ xuống low để dịch nhanh hơn
        effort = "low";
    }
    if (!effort.empty()) {
        body += "\"reasoning_effort\":\"" + JsonEscape(effort) + "\",";
    }
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"" + JsonEscape(cfg.systemPrompt) + "\"},";
    body += "{\"role\":\"user\",\"content\":\"" + JsonEscape(engDesc) + "\"}";
    body += "]}";

    HINTERNET hSession = WinHttpOpen(L"MasterDuel-CardTranslator-AI/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return AI_ERR_TRANSIENT;
    WinHttpSetTimeouts(hSession, 8000, 10000, cfg.timeoutMs, cfg.timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return AI_ERR_TRANSIENT; }

    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return AI_ERR_TRANSIENT; }

    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
    if (!cfg.apiKey.empty() && cfg.apiKey != "YOUR_API_KEY_HERE") {
        headers += L"Authorization: Bearer " + Utf8ToWide(cfg.apiKey) + L"\r\n";
    }

    AiHttpResult result = AI_ERR_TRANSIENT;
    if (WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD statusCode = 0, sz = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);

        std::string resp;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            size_t cur = resp.size();
            resp.resize(cur + avail);
            DWORD read = 0;
            WinHttpReadData(hRequest, &resp[cur], avail, &read);
            resp.resize(cur + read);
        }

        if (statusCode == 200) {
            std::string rawContent;
            if (ExtractAssistantContentFromJson(resp, rawContent)) {
                outViText = CleanAiTranslationOutput(rawContent);
                if (!outViText.empty()) {
                    ClearAiError();
                    result = AI_OK;
                } else {
                    SetAiError("AI returned empty content for '" + cardName + "'", false);
                }
            } else {
                SetAiError("Failed to parse JSON response from AI (model: " + cfg.model + ")", false);
            }
        } else {
            std::string errMsg = ExtractApiErrorMessage(resp);
            char codeBuf[64];
            sprintf_s(codeBuf, "HTTP %lu", statusCode);
            std::string fullErr = std::string(codeBuf) + (errMsg.empty() ? "" : (": " + errMsg));

            if (statusCode == 401 || statusCode == 403 || statusCode == 404) {
                fullErr += " (Fix ai_config.json and press F10 to reload)";
                SetAiError(fullErr, true);
                result = AI_ERR_FATAL;
            } else if (statusCode == 429) {
                SetAiError("Rate limit / quota exceeded (" + fullErr + ")", false);
                result = AI_ERR_RATE_LIMIT;
            } else {
                SetAiError(fullErr, false);
                result = AI_ERR_TRANSIENT;
            }
        }
    } else {
        char errBuf[128];
        sprintf_s(errBuf, "Network error connecting to AI API (WinHTTP err=%lu)", GetLastError());
        SetAiError(errBuf, false);
        result = AI_ERR_TRANSIENT;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// -------------------- Hàng đợi Dịch AI nền --------------------
struct TranslateTask {
    std::string cardName;
    std::string engDesc;
};

static std::mutex g_queueMutex;
static std::condition_variable g_queueCv;
static std::deque<TranslateTask> g_translateQueue;
static const size_t k_MaxTranslateQueueSize = 10;

static void EnqueueTranslate(const std::string& cardName, const std::string& engDesc)
{
    if (cardName.empty() || engDesc.empty()) return;
    if (g_aiFatalPaused) return; // Đang tạm dừng do lỗi cấu hình -> không xếp hàng spam
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        if (g_cache.find(cardName) != g_cache.end()) return;
        if (g_notFoundOrPending.find(cardName) != g_notFoundOrPending.end()) return;
        g_notFoundOrPending.insert(cardName);
    }

    std::string evictedCard;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        // Ưu tiên lá bài người dùng vừa bấm xem (LIFO - đẩy lên đầu hàng đợi)
        g_translateQueue.push_front({ cardName, engDesc });
        if (g_translateQueue.size() > k_MaxTranslateQueueSize) {
            evictedCard = std::move(g_translateQueue.back().cardName);
            g_translateQueue.pop_back();
        }
    }
    if (!evictedCard.empty()) {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_notFoundOrPending.erase(evictedCard);
    }
    g_queueCv.notify_one();
}

static DWORD WINAPI TranslateWorkerThread(LPVOID)
{
    while (true) {
        TranslateTask task;
        {
            std::unique_lock<std::mutex> lk(g_queueMutex);
            g_queueCv.wait(lk, [] { return !g_translateQueue.empty(); });
            task = std::move(g_translateQueue.front());
            g_translateQueue.pop_front();
        }

        if (g_aiFatalPaused) {
            // Đã gặp lỗi cấu hình (401/403/404/thiếu key) -> bỏ qua các task còn lại để không spam API
            ClearRetranslatingCardIfMatches(task.cardName);
            continue;
        }

        AiConfig cfg = GetAiConfigSnapshot();
        Log("[AI] Translating '%s' to %s with model '%s' (%zu bytes)...",
            task.cardName.c_str(), cfg.targetLanguage.c_str(), cfg.model.c_str(), task.engDesc.size());
        std::string viText;
        AiHttpResult res = HttpPostOpenAICompatible(task.cardName, task.engDesc, viText);
        ClearRetranslatingCardIfMatches(task.cardName);
        if (res == AI_OK) {
            {
                std::lock_guard<std::mutex> lk(g_cacheMutex);
                g_cache[task.cardName] = viText;
                g_notFoundOrPending.erase(task.cardName);
                SaveCacheFileLocked();
            }
            Log("[AI] Translated & cached '%s' (%zu bytes)", task.cardName.c_str(), viText.size());
            g_pendingUIRefresh = true;
            if (cfg.delayMs > 0) Sleep((DWORD)cfg.delayMs);
        } else if (res == AI_ERR_FATAL) {
            // Dọn hàng đợi để không spam thêm bất kỳ request nào
            std::lock_guard<std::mutex> lk(g_queueMutex);
            g_translateQueue.clear();
        } else {
            // Lỗi tạm thời (429 / mạng / 5xx): cho phép thử lại khi người dùng xem lại lá bài sau
            {
                std::lock_guard<std::mutex> lk(g_cacheMutex);
                g_notFoundOrPending.erase(task.cardName);
            }
            Sleep(res == AI_ERR_RATE_LIMIT ? 15000 : 4000);
        }
    }
    return 0;
}

// -------------------- IL2CPP API --------------------
typedef void* (*t_domain_get)();
typedef void* (*t_thread_attach)(void*);
typedef void** (*t_domain_get_assemblies)(void*, size_t*);
typedef void* (*t_assembly_get_image)(void*);
typedef const char* (*t_image_get_name)(void*);
typedef void* (*t_class_from_name)(void*, const char*, const char*);
typedef void* (*t_class_get_method_from_name)(void*, const char*, int);
typedef void* (*t_class_get_field_from_name)(void*, const char*);
typedef void (*t_field_get_value)(void*, void*, void*);
typedef void (*t_field_set_value_object)(void*, void*, void*);
typedef void* (*t_runtime_invoke)(void*, void*, void**, void**);
typedef const wchar_t* (*t_string_chars)(void*);
typedef int (*t_string_length)(void*);
typedef void* (*t_string_new)(const char*);
typedef void* (*t_object_get_class)(void*);
typedef void* (*t_object_new)(void*);

static t_domain_get il2cpp_domain_get;
static t_thread_attach il2cpp_thread_attach;
static t_domain_get_assemblies il2cpp_domain_get_assemblies;
static t_assembly_get_image il2cpp_assembly_get_image;
static t_image_get_name il2cpp_image_get_name;
static t_class_from_name il2cpp_class_from_name;
static t_class_get_method_from_name il2cpp_class_get_method_from_name;
static t_class_get_field_from_name il2cpp_class_get_field_from_name;
static t_field_get_value il2cpp_field_get_value;
static t_field_set_value_object il2cpp_field_set_value_object;
static t_runtime_invoke il2cpp_runtime_invoke;
static t_string_chars il2cpp_string_chars;
static t_string_length il2cpp_string_length;
static t_string_new il2cpp_string_new;
static t_object_get_class il2cpp_object_get_class;
static t_object_new il2cpp_object_new;

static bool ResolveApi(HMODULE h)
{
#define R(n) il2cpp_##n = (t_##n)GetProcAddress(h, "il2cpp_" #n); if (!il2cpp_##n) { Log("Missing export il2cpp_" #n); return false; }
    R(domain_get) R(thread_attach) R(domain_get_assemblies) R(assembly_get_image) R(image_get_name)
    R(class_from_name) R(class_get_method_from_name) R(class_get_field_from_name)
    R(field_get_value) R(field_set_value_object)
    R(runtime_invoke) R(string_chars) R(string_length) R(string_new) R(object_get_class) R(object_new)
#undef R
    return true;
}

static std::string Il2CppStrToStdString(void* il2str)
{
    if (!il2str) return "";
    int len = il2cpp_string_length(il2str);
    const wchar_t* w = il2cpp_string_chars(il2str);
    if (len <= 0 || !w) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &out[0], needed, nullptr, nullptr);
    return out;
}

static bool IsUnityObjectAlive(void* obj)
{
    if (!obj) return false;
    void* cachedPtr = *(void**)((char*)obj + 0x10);
    return cachedPtr != nullptr;
}

// -------------------- Hệ thống Fallback Font Tiếng Việt cho TextMeshPro --------------------
static void* g_clsTMPFontAsset = nullptr;
static void* g_clsTMPFontAssetUtilities = nullptr;
static void* g_clsTMPSettings = nullptr;
static void* g_clsUnityFont = nullptr;

// Static methods trên TMP_FontAsset:
// 1. CreateFontAsset(string fontFilePath, int faceIndex, int samplingPointSize, int atlasPadding, GlyphRenderMode renderMode, int atlasWidth, int atlasHeight) -> 7 params
typedef void* (*t_CreateFontAsset7)(void* fontFilePath, int faceIndex, int samplingPointSize, int atlasPadding, int renderMode, int atlasWidth, int atlasHeight, void* methodInfo);
// 2. CreateFontAsset(string familyName, string styleName, int pointSize) -> 3 params
typedef void* (*t_CreateFontAsset3)(void* familyName, void* styleName, int pointSize, void* methodInfo);
// 3. CreateFontAsset(Font font) -> 1 param
typedef void* (*t_CreateFontAsset1)(void* font, void* methodInfo);
// 4. TryAddCharacters(string characters, bool includeFontFeatures) -> 2 params (instance)
typedef bool (*t_TryAddCharactersStr)(void* self, void* charactersStr, bool includeFontFeatures, void* methodInfo);
// 5. UnityEngine.Font.Internal_CreateFont(Font self, string name) -> 2 params (static)
typedef void (*t_InternalCreateFont)(void* fontSelf, void* nameStr, void* methodInfo);
// 6. UnityEngine.Object.DontDestroyOnLoad(Object target) -> 1 param (static)
typedef void (*t_DontDestroyOnLoad)(void* obj, void* methodInfo);
// 7. TMP_FontAsset.get_fallbackFontAssetTable(self) -> 0 params (instance)
typedef void* (*t_GetFallbackTable)(void* self, void* methodInfo);
// 8. TMP_Settings.get_fallbackFontAssets() -> 0 params (static)
typedef void* (*t_GetGlobalFallbacks)(void* methodInfo);

// Hook TMP_FontAssetUtilities.GetCharacterFromFontAsset_Internal(uint unicode, TMP_FontAsset sourceFontAsset, bool includeFallbacks, FontStyles fontStyle, FontWeight fontWeight, out bool isAlternativeTypeface)
typedef void* (*t_GetCharFromFontAssetInternal)(unsigned int unicode, void* sourceFontAsset, bool includeFallbacks, int fontStyle, int fontWeight, bool* isAlternativeTypeface, void* methodInfo);

static t_CreateFontAsset7 g_fnCreateFontAsset7 = nullptr;
static t_CreateFontAsset3 g_fnCreateFontAsset3 = nullptr;
static t_CreateFontAsset1 g_fnCreateFontAsset1 = nullptr;
static t_TryAddCharactersStr g_fnTryAddCharactersStr = nullptr;
static t_InternalCreateFont g_fnInternalCreateFont = nullptr;
static t_DontDestroyOnLoad g_fnDontDestroyOnLoad = nullptr;
static t_GetFallbackTable g_fnGetFallbackTable = nullptr;
static t_GetGlobalFallbacks g_fnGetGlobalFallbacks = nullptr;
static t_GetCharFromFontAssetInternal g_origGetCharInternal = nullptr;

static void* g_viFallbackFontAsset = nullptr;
static bool g_viFontInitTried = false;

// Bảng toàn bộ ký tự tiếng Việt (hoa + thường) để nạp sẵn vào Atlas của TMP_FontAsset
static const char* k_VietnameseCharsUtf8 =
    "a\xC3\xA0\xC3\xA1\xE1\xBA\xA3\xC3\xA3\xE1\xBA\xA1"
    "\xC4\x83\xE1\xBA\xB1\xE1\xBA\xAF\xE1\xBA\xB3\xE1\xBA\xB5\xE1\xBA\xB7"
    "\xC3\xA2\xE1\xBA\xA7\xE1\xBA\xA5\xE1\xBA\xA9\xE1\xBA\xAB\xE1\xBA\xAD"
    "e\xC3\xA8\xC3\xA9\xE1\xBA\xBB\xE1\xBA\xBD\xE1\xBA\xB9"
    "\xC3\xAA\xE1\xBB\x81\xE1\xBA\xBF\xE1\xBB\x83\xE1\xBB\x85\xE1\xBB\x87"
    "i\xC3\xAC\xC3\xAD\xE1\xBB\x89\xC4\xA9\xE1\xBB\x8B"
    "o\xC3\xB2\xC3\xB3\xE1\xBB\x8F\xC3\xB5\xE1\xBB\x8D"
    "\xC3\xB4\xE1\xBB\x93\xE1\xBB\x91\xE1\xBB\x95\xE1\xBB\x97\xE1\xBB\x99"
    "\xC6\xA1\xE1\xBB\x9D\xE1\xBB\x9B\xE1\xBB\x9F\xE1\xBB\xA1\xE1\xBB\xA3"
    "u\xC3\xB9\xC3\xBA\xE1\xBB\xA7\xC5\xA9\xE1\xBB\xA5"
    "\xC6\xB0\xE1\xBB\xAB\xE1\xBB\xA9\xE1\xBB\xAD\xE1\xBB\xAF\xE1\xBB\xB1"
    "y\xE1\xBB\xB3\xC3\xBD\xE1\xBB\xB7\xE1\xBB\xB9\xE1\xBB\xB5"
    "\xC4\x91"
    "A\xC3\x80\xC3\x81\xE1\xBA\xA2\xC3\x83\xE1\xBA\xA0"
    "\xC4\x82\xE1\xBA\xB0\xE1\xBA\xAE\xE1\xBA\xB2\xE1\xBA\xB4\xE1\xBA\xB6"
    "\xC3\x82\xE1\xBA\xA6\xE1\xBA\xA4\xE1\xBA\xA8\xE1\xBA\xAA\xE1\xBA\xAC"
    "E\xC3\x88\xC3\x89\xE1\xBA\xBA\xE1\xBA\xBC\xE1\xBA\xB8"
    "\xC3\x8A\xE1\xBB\x80\xE1\xBA\xBE\xE1\xBB\x82\xE1\xBB\x84\xE1\xBB\x86"
    "I\xC3\x8C\xC3\x8D\xE1\xBB\x88\xC4\xA8\xE1\xBB\x8A"
    "O\xC3\x92\xC3\x93\xE1\xBB\x8E\xC3\x95\xE1\xBB\x8C"
    "\xC3\x94\xE1\xBB\x92\xE1\xBB\x90\xE1\xBB\x94\xE1\xBB\x96\xE1\xBB\x98"
    "\xC6\xA0\xE1\xBB\x9C\xE1\xBB\x9A\xE1\xBB\x9E\xE1\xBB\xA0\xE1\xBB\xA2"
    "U\xC3\x99\xC3\x9A\xE1\xBB\xA6\xC5\xA8\xE1\xBB\xA4"
    "\xC6\xAF\xE1\xBB\xAA\xE1\xBB\xA8\xE1\xBB\xAC\xE1\xBB\xAE\xE1\xBB\xB0"
    "Y\xE1\xBB\xB2\xC3\x9D\xE1\xBB\xB6\xE1\xBB\xB8\xE1\xBB\xB4"
    "\xC4\x90"
    "0123456789.,;:!?()-[]/\"' \xE2\x80\x93\xE2\x80\x94\xE2\x8F\xBE";

static void EnsureVietnameseFallbackFont()
{
    if (g_viFallbackFontAsset || g_viFontInitTried) return;
    g_viFontInitTried = true;

    __try {
        // SDFAA = 4165 (0x1045) trong UnityEngine.TextCore.LowLevel.GlyphRenderMode
        const int k_GlyphRenderMode_SDFAA = 4165;

        const char* fontPaths[] = {
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf"
        };

        // Cách 1: Gọi TMP_FontAsset.CreateFontAsset(string fontFilePath, 0, 36, 4, SDFAA, 1024, 1024)
        if (g_fnCreateFontAsset7) {
            for (const char* fp : fontPaths) {
                if (GetFileAttributesA(fp) == INVALID_FILE_ATTRIBUTES) continue;
                void* pathStr = il2cpp_string_new(fp);
                void* fa = g_fnCreateFontAsset7(pathStr, 0, 36, 4, k_GlyphRenderMode_SDFAA, 1024, 1024, nullptr);
                if (fa && IsUnityObjectAlive(fa)) {
                    g_viFallbackFontAsset = fa;
                    Log("[Font] Created TMP_FontAsset from file '%s' @ %p", fp, fa);
                    break;
                }
            }
        }

        // Cách 2: Gọi TMP_FontAsset.CreateFontAsset(familyName, styleName, pointSize)
        if (!g_viFallbackFontAsset && g_fnCreateFontAsset3) {
            void* fam = il2cpp_string_new("Arial");
            void* sty = il2cpp_string_new("Regular");
            void* fa = g_fnCreateFontAsset3(fam, sty, 36, nullptr);
            if (fa && IsUnityObjectAlive(fa)) {
                g_viFallbackFontAsset = fa;
                Log("[Font] Created TMP_FontAsset(Arial, Regular, 36) @ %p", fa);
            }
        }

        // Cách 3: Tạo UnityEngine.Font rồi gọi TMP_FontAsset.CreateFontAsset(Font)
        if (!g_viFallbackFontAsset && g_clsUnityFont && g_fnInternalCreateFont && g_fnCreateFontAsset1) {
            for (const char* fp : fontPaths) {
                if (GetFileAttributesA(fp) == INVALID_FILE_ATTRIBUTES) continue;
                void* uFont = il2cpp_object_new(g_clsUnityFont);
                if (uFont) {
                    g_fnInternalCreateFont(uFont, il2cpp_string_new(fp), nullptr);
                    if (IsUnityObjectAlive(uFont)) {
                        if (g_fnDontDestroyOnLoad) g_fnDontDestroyOnLoad(uFont, nullptr);
                        void* fa = g_fnCreateFontAsset1(uFont, nullptr);
                        if (fa && IsUnityObjectAlive(fa)) {
                            g_viFallbackFontAsset = fa;
                            Log("[Font] Created TMP_FontAsset via UnityEngine.Font('%s') @ %p", fp, fa);
                            break;
                        }
                    }
                }
            }
        }

        if (g_viFallbackFontAsset) {
            if (g_fnDontDestroyOnLoad) {
                g_fnDontDestroyOnLoad(g_viFallbackFontAsset, nullptr);
            }
        } else {
            Log("[Font] WARNING: Failed to create fallback TMP_FontAsset!");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Font] Exception while creating fallback TMP_FontAsset (0x%08lX)", GetExceptionCode());
    }
}

// Hook TMP_FontAssetUtilities.GetCharacterFromFontAsset_Internal:
// Mỗi khi font gốc của game (RoGSanSrfStd-Bd / Fot-RodinPro, ...) không có ký tự tiếng Việt (trả về nullptr),
// ta tự động lấy ký tự đó từ g_viFallbackFontAsset!
static void* Hook_GetCharFromFontAssetInternal(
    unsigned int unicode,
    void* sourceFontAsset,
    bool includeFallbacks,
    int fontStyle,
    int fontWeight,
    bool* isAlternativeTypeface,
    void* methodInfo)
{
    void* ch = g_origGetCharInternal(unicode, sourceFontAsset, includeFallbacks, fontStyle, fontWeight, isAlternativeTypeface, methodInfo);
    if (!ch && unicode > 0x7F) {
        EnsureVietnameseFallbackFont();
        if (g_viFallbackFontAsset && sourceFontAsset != g_viFallbackFontAsset) {
            ch = g_origGetCharInternal(unicode, g_viFallbackFontAsset, false, fontStyle, fontWeight, isAlternativeTypeface, methodInfo);
        }
    }
    return ch;
}

// -------------------- Con trỏ Class & Hàm gốc trong Master Duel --------------------
static void* g_clsContent = nullptr;
static void* g_clsCardInfo = nullptr;
static void* g_clsCardInfoBase = nullptr;
static void* g_clsCardInfoDetail = nullptr;
static void* g_clsCardDetailView = nullptr;
static void* g_clsCardActionMenu = nullptr;
static void* g_clsCardUtilWidget = nullptr;
static void* g_clsCardDecoderActionMenu = nullptr;
static void* g_clsLotteryActionMenu = nullptr;
static void* g_clsLotteryDetailView = nullptr;
static void* g_clsEnqueteDetailWidget = nullptr;
static void* g_clsCardInfoForViewer = nullptr;
static void* g_clsCardInfoGenericEx = nullptr;
static void* g_clsSelectorManager = nullptr;

// YgomGame.Card.Content methods (instance methods trên singleton Content.Instance)
typedef void* (*t_Content_GetName)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_GetDesc)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_GetRawDesc)(void* self, int cardId, void* methodInfo);
typedef void* (*t_Content_GetDescWithoutPendulum)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_GetDescOfPendulum)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_GetDescWithoutPendulumForCardInfo)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_GetDescOfPendulumForCardInfo)(void* self, int cardId, bool replaceAlnum, void* methodInfo);
typedef void* (*t_Content_getDescActiveEffect)(void* self, int cardId, int numOfEfx, void* methodInfo);

// Các hàm UI refresh
typedef void (*t_InstanceVoid0)(void* self, void* methodInfo);
typedef void (*t_InstanceVoid1)(void* self, void* arg0, void* methodInfo);

static t_Content_GetName g_fnContentGetName = nullptr;
static t_Content_GetDesc g_origGetDesc = nullptr;
static t_Content_GetRawDesc g_origGetRawDesc = nullptr;
static t_Content_GetDescWithoutPendulum g_origGetDescWithoutPendulum = nullptr;
static t_Content_GetDescOfPendulum g_origGetDescOfPendulum = nullptr;
static t_Content_GetDescWithoutPendulumForCardInfo g_origGetDescWithoutPendulumForCardInfo = nullptr;
static t_Content_GetDescOfPendulumForCardInfo g_origGetDescOfPendulumForCardInfo = nullptr;
static t_Content_getDescActiveEffect g_origGetDescActiveEffect = nullptr;

// Cờ thread-local để gọi hàm gốc tiếng Anh mà không bị các hook mô tả chặn ngang
static thread_local bool g_bypassDescHooks = false;

struct ScopedBypassDescHooks {
    bool prev;
    ScopedBypassDescHooks() : prev(g_bypassDescHooks) { g_bypassDescHooks = true; }
    ~ScopedBypassDescHooks() { g_bypassDescHooks = prev; }
};

// -------------------- Căn chỉnh Highlight hiệu ứng theo dấu chấm câu --------------------
struct SentenceSpan {
    size_t start;           // Vị trí byte bắt đầu câu trong chuỗi
    size_t end;             // Vị trí byte kết thúc câu (bao gồm dấu chấm câu)
    bool precededByNewline; // Có xuống dòng trước câu này không
    bool isBullet;          // Câu bắt đầu bằng "●" (\xE2\x97\x8F)
    bool isInlineParen;     // Câu chú thích "(...)" nằm trên cùng dòng ngay sau câu trước
    bool hasColonOrSemi;    // Chứa ':' hoặc ';' (đặc trưng của hiệu ứng kích hoạt PSCT)
};

static bool IsBulletAt(const std::string& s, size_t i)
{
    return (i + 2 < s.size() &&
            (unsigned char)s[i]     == 0xE2 &&
            (unsigned char)s[i + 1] == 0x97 &&
            (unsigned char)s[i + 2] == 0x8F);
}

static std::vector<SentenceSpan> SplitIntoSentences(const std::string& text, bool mergeInlineParens)
{
    std::vector<SentenceSpan> spans;
    size_t n = text.size();
    size_t i = 0;
    bool sawNewline = true;

    while (i < n) {
        while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            if (text[i] == '\n') sawNewline = true;
            i++;
        }
        if (i >= n) break;

        size_t sentStart = i;
        bool isBullet = IsBulletAt(text, sentStart);
        bool hasColonOrSemi = false;

        if (isBullet) {
            i += 3; // qua "●"
            while (i < n && text[i] != '\r' && text[i] != '\n') {
                if (IsBulletAt(text, i)) break;
                if (text[i] == ':' || text[i] == ';') hasColonOrSemi = true;
                i++;
            }
        } else {
            bool inQuotes = false;
            int parenDepth = 0;
            while (i < n) {
                char c = text[i];
                if (c == '\r' || c == '\n') break;
                if (i > sentStart && IsBulletAt(text, i)) break;

                if (c == '"') {
                    inQuotes = !inQuotes;
                    i++;
                    continue;
                }
                if (c == ':' || c == ';') {
                    hasColonOrSemi = true;
                    inQuotes = false; // Tên lá bài trong "..." không bao giờ chứa ':' hay ';'
                }
                if (!inQuotes) {
                    if (c == '(') {
                        parenDepth++;
                        i++;
                        continue;
                    }
                    if (c == ')') {
                        if (parenDepth > 0) parenDepth--;
                        // Trường hợp câu trong ngoặc kết thúc bằng ".)" như "(This is treated as an Xyz Summon.)"
                        if (parenDepth == 0 && i > sentStart && text[i - 1] == '.') {
                            if (i + 1 >= n || text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '\r' || text[i + 1] == '\n') {
                                i++;
                                break;
                            }
                        }
                        i++;
                        continue;
                    }
                    if (c == '.' && parenDepth == 0) {
                        if (i + 1 >= n || text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '\r' || text[i + 1] == '\n') {
                            i++;
                            break;
                        }
                    }
                }
                i++;
            }
        }

        size_t sentEnd = i;
        while (sentEnd > sentStart && (text[sentEnd - 1] == ' ' || text[sentEnd - 1] == '\t')) {
            sentEnd--;
        }
        if (sentEnd <= sentStart) continue;

        bool isInlineParen = (!sawNewline && !isBullet && text[sentStart] == '(' && text[sentEnd - 1] == ')');
        if (mergeInlineParens && isInlineParen && !spans.empty() && !spans.back().isBullet) {
            spans.back().end = sentEnd;
            spans.back().hasColonOrSemi = spans.back().hasColonOrSemi || hasColonOrSemi;
        } else {
            SentenceSpan sp;
            sp.start = sentStart;
            sp.end = sentEnd;
            sp.precededByNewline = sawNewline;
            sp.isBullet = isBullet;
            sp.isInlineParen = isInlineParen;
            sp.hasColonOrSemi = hasColonOrSemi;
            spans.push_back(sp);
        }
        sawNewline = false;
    }
    return spans;
}

// Tìm vị trí [outStart, outEnd) của engEffect trong engSection (bỏ qua khác biệt \r\n vs \n)
static bool FindEffectRangeInSection(const std::string& section, const std::string& effect, size_t& outStart, size_t& outEnd)
{
    std::string trimmed = Trim(effect);
    if (trimmed.empty() || section.empty()) return false;

    size_t pos = section.find(trimmed);
    if (pos != std::string::npos) {
        outStart = pos;
        outEnd = pos + trimmed.size();
        return true;
    }

    // Fallback: so khớp bỏ qua khoảng trắng (' ', '\t', '\r', '\n')
    std::string normEffect;
    for (char c : trimmed) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') normEffect.push_back(c);
    }
    if (normEffect.empty()) return false;

    std::string normSec;
    std::vector<size_t> normToOrig;
    normSec.reserve(section.size());
    normToOrig.reserve(section.size());
    for (size_t i = 0; i < section.size(); ++i) {
        char c = section[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            normSec.push_back(c);
            normToOrig.push_back(i);
        }
    }

    size_t npos = normSec.find(normEffect);
    if (npos != std::string::npos) {
        outStart = normToOrig[npos];
        outEnd = normToOrig[npos + normEffect.size() - 1] + 1;
        return true;
    }
    return false;
}

static bool FindOverlappingSentenceRange(
    const std::string& text,
    const std::vector<SentenceSpan>& spans,
    size_t effStart,
    size_t effEnd,
    size_t& outFirst,
    size_t& outLast)
{
    bool found = false;
    outFirst = 0;
    outLast = 0;
    for (size_t k = 0; k < spans.size(); ++k) {
        size_t ovStart = (spans[k].start > effStart) ? spans[k].start : effStart;
        size_t ovEnd   = (spans[k].end   < effEnd)   ? spans[k].end   : effEnd;
        if (ovEnd > ovStart) {
            size_t nonWs = 0;
            for (size_t p = ovStart; p < ovEnd; ++p) {
                char c = text[p];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') nonWs++;
            }
            if (nonWs >= 2) {
                if (!found) {
                    outFirst = k;
                    found = true;
                }
                outLast = k;
            }
        }
    }
    return found;
}

static bool MapEffectSpanIndices(
    const std::string& engSection,
    const std::vector<SentenceSpan>& engSpans,
    const std::vector<SentenceSpan>& viSpans,
    size_t firstSeg,
    size_t lastSeg,
    size_t& outViFirst,
    size_t& outViLast)
{
    if (engSpans.empty() || viSpans.empty() || firstSeg >= engSpans.size() || lastSeg >= engSpans.size()) {
        return false;
    }

    size_t N = engSpans.size();
    size_t M = viSpans.size();
    size_t viFirst = 0, viLast = 0;

    if (N == M) {
        // Khớp 1-1 theo câu hoàn hảo
        viFirst = firstSeg;
        viLast  = lastSeg;
    } else {
        // Nếu số lượng câu lệch nhau nhưng câu đang kích hoạt có dấu ':' hoặc ';' (PSCT activated effect),
        // đối chiếu thứ tự câu có ':' hoặc ';' giữa bản Anh và bản Việt.
        bool matchedByColon = false;
        if (engSpans[firstSeg].hasColonOrSemi) {
            std::vector<size_t> engColonIdx, viColonIdx;
            int ordinal = -1;
            for (size_t k = 0; k < N; ++k) {
                if (engSpans[k].hasColonOrSemi) {
                    if (k == firstSeg) ordinal = (int)engColonIdx.size();
                    engColonIdx.push_back(k);
                }
            }
            for (size_t k = 0; k < M; ++k) {
                if (viSpans[k].hasColonOrSemi) {
                    viColonIdx.push_back(k);
                }
            }
            if (ordinal >= 0 && engColonIdx.size() == viColonIdx.size()) {
                viFirst = viColonIdx[ordinal];
                size_t spanCount = lastSeg - firstSeg;
                viLast = (viFirst + spanCount < M) ? (viFirst + spanCount) : (M - 1);
                matchedByColon = true;
            }
        }

        if (!matchedByColon) {
            if (firstSeg == 0) {
                viFirst = 0;
            } else if (firstSeg == N - 1) {
                viFirst = M - 1;
            } else {
                viFirst = (firstSeg * M) / N;
            }

            if (lastSeg == N - 1) {
                viLast = M - 1;
            } else if (lastSeg == firstSeg) {
                viLast = viFirst;
            } else {
                viLast = ((lastSeg + 1) * M - 1) / N;
            }
            if (viFirst >= M) viFirst = M - 1;
            if (viLast >= M)  viLast  = M - 1;
            if (viLast < viFirst) viLast = viFirst;
        }
    }

    outViFirst = viFirst;
    outViLast  = viLast;
    return true;
}

static std::string MapEffectByPunctuation(
    const std::string& engSection,
    const std::string& engEffect,
    const std::string& viSection)
{
    size_t engStart = 0, engEnd = 0;
    if (!FindEffectRangeInSection(engSection, engEffect, engStart, engEnd)) {
        return "";
    }

    // Bước 1: Thử tách câu nguyên bản (giữ riêng câu chú thích "(...)" nếu cả 2 ngôn ngữ đều tách giống nhau)
    std::vector<SentenceSpan> engSpans = SplitIntoSentences(engSection, false);
    std::vector<SentenceSpan> viSpans  = SplitIntoSentences(viSection, false);
    if (engSpans.empty() || viSpans.empty()) return "";

    size_t firstSeg = 0, lastSeg = 0;
    if (!FindOverlappingSentenceRange(engSection, engSpans, engStart, engEnd, firstSeg, lastSeg)) {
        return "";
    }

    // Nếu số câu chưa khớp (ví dụ 1 ngôn ngữ gộp "(This is treated as an Xyz Summon.)" vào câu trước),
    // thử gộp các câu chú thích "(...)" trên cùng dòng vào câu liền trước ở cả 2 bên.
    if (engSpans.size() != viSpans.size()) {
        std::vector<SentenceSpan> engMerged = SplitIntoSentences(engSection, true);
        std::vector<SentenceSpan> viMerged  = SplitIntoSentences(viSection, true);
        size_t fM = 0, lM = 0;
        if (!engMerged.empty() && !viMerged.empty() &&
            FindOverlappingSentenceRange(engSection, engMerged, engStart, engEnd, fM, lM))
        {
            if (engMerged.size() == viMerged.size() ||
                std::abs((int)engMerged.size() - (int)viMerged.size()) < std::abs((int)engSpans.size() - (int)viSpans.size()))
            {
                engSpans = std::move(engMerged);
                viSpans  = std::move(viMerged);
                firstSeg = fM;
                lastSeg  = lM;
            }
        }
    }

    size_t viFirst = 0, viLast = 0;
    if (!MapEffectSpanIndices(engSection, engSpans, viSpans, firstSeg, lastSeg, viFirst, viLast)) {
        return "";
    }

    size_t outByteStart = viSpans[viFirst].start;
    size_t outByteEnd   = viSpans[viLast].end;
    if (outByteEnd <= outByteStart || outByteEnd > viSection.size()) return "";
    return viSection.substr(outByteStart, outByteEnd - outByteStart);
}

// Định dạng lại đoạn mô tả tiếng Việt sao cho mỗi hiệu ứng / câu độc lập luôn nằm trên một dòng riêng ('\n'),
// đồng thời giữ các câu thuộc cùng một hiệu ứng đa câu trên cùng một dòng (' ') để tương thích hoàn toàn với Highlight.
static std::string FormatSectionWithEffectNewlines(
    void* self,
    int cardId,
    const std::string& viSection,
    bool isPendulum)
{
    if (viSection.empty()) return viSection;

    // Tách câu với mergeInlineParens = true để các câu chú thích "(...)" luôn đi kèm câu hiệu ứng chính trên cùng dòng
    std::vector<SentenceSpan> viSpans = SplitIntoSentences(viSection, true);
    if (viSpans.empty()) return viSection;
    if (viSpans.size() == 1) {
        return viSection.substr(viSpans[0].start, viSpans[0].end - viSpans[0].start);
    }

    // Mặc định xuống dòng ('\n') sau mỗi câu / hiệu ứng riêng biệt.
    // Nếu một hiệu ứng đánh số trong game bao gồm nhiều câu liên tiếp (viFirst .. viLast),
    // giữ các câu thuộc cùng hiệu ứng đó trên cùng dòng (' ').
    std::vector<bool> keepSameLineAfter(viSpans.size(), false);

    if (self && cardId > 0 && g_origGetDescActiveEffect) {
        ScopedBypassDescHooks bypass;
        std::string engSection;
        if (isPendulum && g_origGetDescOfPendulum) {
            void* engPendObj = g_origGetDescOfPendulum(self, cardId, true, nullptr);
            engSection = Il2CppStrToStdString(engPendObj);
        } else if (!isPendulum && g_origGetDescWithoutPendulum) {
            void* engMonObj = g_origGetDescWithoutPendulum(self, cardId, true, nullptr);
            engSection = Il2CppStrToStdString(engMonObj);
        }

        if (!engSection.empty()) {
            std::vector<SentenceSpan> engSpans = SplitIntoSentences(engSection, true);
            if (!engSpans.empty()) {
                int baseNum = isPendulum ? 20 : 10;
                for (int idx = 0; idx < 10; ++idx) {
                    void* effObj = g_origGetDescActiveEffect(self, cardId, baseNum + idx, nullptr);
                    std::string engEff = Il2CppStrToStdString(effObj);
                    if (engEff.empty()) break;

                    size_t engStart = 0, engEnd = 0;
                    if (!FindEffectRangeInSection(engSection, engEff, engStart, engEnd)) continue;

                    size_t firstSeg = 0, lastSeg = 0;
                    if (!FindOverlappingSentenceRange(engSection, engSpans, engStart, engEnd, firstSeg, lastSeg)) continue;

                    size_t viFirst = 0, viLast = 0;
                    if (MapEffectSpanIndices(engSection, engSpans, viSpans, firstSeg, lastSeg, viFirst, viLast)) {
                        for (size_t k = viFirst; k < viLast && k + 1 < viSpans.size(); ++k) {
                            if (!viSpans[k + 1].precededByNewline && !viSpans[k + 1].isBullet && !viSpans[k].isBullet) {
                                keepSameLineAfter[k] = true;
                            }
                        }
                    }
                }
            }
        }
    }

    std::string out;
    out.reserve(viSection.size() + viSpans.size());
    for (size_t k = 0; k < viSpans.size(); ++k) {
        out.append(viSection, viSpans[k].start, viSpans[k].end - viSpans[k].start);
        if (k + 1 < viSpans.size()) {
            out.push_back(keepSameLineAfter[k] ? ' ' : '\n');
        }
    }
    return out;
}

static std::string FormatFullViDescWithNewlines(void* self, int cardId, const std::string& viText)
{
    std::string pendText, monText;
    SplitPendulumAndMonsterText(viText, pendText, monText);
    std::string monFormatted = FormatSectionWithEffectNewlines(self, cardId, monText, false);
    if (!pendText.empty()) {
        std::string pendFormatted = FormatSectionWithEffectNewlines(self, cardId, pendText, true);
        return "[ Hiệu ứng Pendulum ]\n" + pendFormatted + "\n----------------------------------------\n[ Hiệu ứng Quái thú ]\n" + monFormatted;
    }
    return monFormatted;
}

static t_InstanceVoid0 g_origCardInfo_SetDescriptionArea = nullptr;
static t_InstanceVoid1 g_origCardInfoDetail_SetDescriptionArea = nullptr;
static t_InstanceVoid0 g_origCardDetailView_setCardText = nullptr;
static t_InstanceVoid0 g_origCardActionMenu_setCardText = nullptr;
static t_InstanceVoid0 g_origCardUtilWidget_setCardText = nullptr;
static t_InstanceVoid0 g_origCardDecoderActionMenu_setCardText = nullptr;
static t_InstanceVoid0 g_origLotteryActionMenu_setCardText = nullptr;
static t_InstanceVoid0 g_origLotteryDetailView_setCardText = nullptr;
static t_InstanceVoid0 g_origEnqueteDetailWidget_setCardText = nullptr;
static t_InstanceVoid0 g_origCardInfoForViewer_SetDescriptionArea = nullptr;
static t_InstanceVoid0 g_origCardInfoGenericEx_SetDescriptionArea = nullptr;
static t_InstanceVoid0 g_origSelectorManager_Update = nullptr;

// Lưu instance UI gần nhất để LiveRefresh khi dịch AI xong
static void* g_lastCardInfo = nullptr;
static void* g_lastCardInfoDetail = nullptr;
static void* g_lastCardInfoDetailArg = nullptr;
static void* g_lastCardDetailView = nullptr;
static void* g_lastCardActionMenu = nullptr;
static void* g_lastCardUtilWidget = nullptr;
static void* g_lastCardDecoderActionMenu = nullptr;
static void* g_lastLotteryActionMenu = nullptr;
static void* g_lastLotteryDetailView = nullptr;
static void* g_lastEnqueteDetailWidget = nullptr;
static void* g_lastCardInfoForViewer = nullptr;
static void* g_lastCardInfoGenericEx = nullptr;

// Lưu ánh xạ cardId -> Tên tiếng Anh đã chuẩn hóa để không cần gọi GetName nhiều lần
static std::mutex g_cardMapMutex;
static std::unordered_map<int, std::string> g_cardIdToName;

static std::string GetCardEnglishName(void* contentSelf, int cardId)
{
    if (cardId <= 0) return "";
    {
        std::lock_guard<std::mutex> lk(g_cardMapMutex);
        auto it = g_cardIdToName.find(cardId);
        if (it != g_cardIdToName.end()) return it->second;
    }
    if (!contentSelf || !g_fnContentGetName) return "";
    void* nameObj = g_fnContentGetName(contentSelf, cardId, true, nullptr);
    std::string rawName = Il2CppStrToStdString(nameObj);
    std::string normName = NormalizeCardName(rawName);
    if (!normName.empty()) {
        std::lock_guard<std::mutex> lk(g_cardMapMutex);
        g_cardIdToName[cardId] = normName;
    }
    return normName;
}

static std::string GetCardEnglishDescForAI(void* contentSelf, int cardId)
{
    if (!contentSelf || cardId <= 0) return "";
    ScopedBypassDescHooks bypass;
    std::string engPend, engMon;
    if (g_origGetDescOfPendulum) {
        void* pendObj = g_origGetDescOfPendulum(contentSelf, cardId, true, nullptr);
        engPend = Trim(Il2CppStrToStdString(pendObj));
    }
    if (g_origGetDescWithoutPendulum) {
        void* monObj = g_origGetDescWithoutPendulum(contentSelf, cardId, true, nullptr);
        engMon = Trim(Il2CppStrToStdString(monObj));
    }
    if (!engPend.empty()) {
        return "[ Pendulum Effect ]\n" + engPend + "\n----------------------------------------\n[ Monster Effect ]\n" + engMon;
    }
    if (!engMon.empty()) {
        return engMon;
    }
    if (g_origGetDesc) {
        void* descObj = g_origGetDesc(contentSelf, cardId, true, nullptr);
        return Trim(Il2CppStrToStdString(descObj));
    }
    return "";
}

// Theo dõi chính xác lá bài đang được hiển thị trong khung Card Info để phục vụ phím tắt F9 (dịch lại)
static thread_local bool g_inActiveCardPanel = false;
struct ScopedActiveCardPanel {
    bool prev;
    ScopedActiveCardPanel() : prev(g_inActiveCardPanel) { g_inActiveCardPanel = true; }
    ~ScopedActiveCardPanel() { g_inActiveCardPanel = prev; }
};

static std::mutex g_lastViewedMutex;
static void* g_lastViewedContentSelf = nullptr;
static int g_lastViewedCardId = 0;
static std::string g_lastViewedCardName;

static void RecordViewedCardIfActive(void* contentSelf, int cardId, const std::string& cardName, const char* callerTag)
{
    if (!contentSelf || cardId <= 0 || cardName.empty()) return;
    // Không ghi đè khi gọi từ getDescActiveEffect (chỉ lấy câu highlight)
    if (callerTag && strstr(callerTag, "getDescActiveEffect") != nullptr) return;

    std::lock_guard<std::mutex> lk(g_lastViewedMutex);
    // Ưu tiên tuyệt đối các lần gọi phát sinh từ khung UI hiển thị bài đang mở (hoặc nếu chưa có lá nào)
    if (g_inActiveCardPanel || g_lastViewedCardId == 0 ||
        (callerTag && strstr(callerTag, "ForCardInfo") != nullptr))
    {
        g_lastViewedContentSelf = contentSelf;
        g_lastViewedCardId      = cardId;
        g_lastViewedCardName    = cardName;
    }
}

static bool LookupOrTranslateCard(void* contentSelf, int cardId, std::string& outViText, const char* callerTag)
{
    std::string cardName = GetCardEnglishName(contentSelf, cardId);
    if (cardName.empty()) return false;

    RecordViewedCardIfActive(contentSelf, cardId, cardName, callerTag);

    bool found = false;
    bool alreadyPending = false;
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        auto it = g_cache.find(cardName);
        if (it != g_cache.end()) {
            outViText = it->second;
            found = true;
        } else if (g_notFoundOrPending.find(cardName) != g_notFoundOrPending.end()) {
            alreadyPending = true;
        }
    }

    if (found) return true;
    if (alreadyPending || g_aiFatalPaused) return false;

    std::string engDesc = GetCardEnglishDescForAI(contentSelf, cardId);
    if (!engDesc.empty()) {
        Log("[%s] Card: '%s' (mrk=%d) -> cache miss -> queued for AI translation",
            callerTag, cardName.c_str(), cardId);
        EnqueueTranslate(cardName, engDesc);
    }
    return false;
}

// Khi đang dịch lại (F9) hoặc AI gặp lỗi dịch, gắn thêm 1 dòng trạng thái gọn gàng ở cuối mô tả tiếng Anh
static void* AppendAiErrorNoticeIfAny(void* origIl2CppStr, void* contentSelf, int cardId)
{
    if (!origIl2CppStr) return origIl2CppStr;
    std::string cardName = GetCardEnglishName(contentSelf, cardId);
    std::string suffix;

    if (IsCardRetranslating(cardName)) {
        suffix = "\n\n<color=#FFD166>[CardAI] Re-translating with AI...</color>";
    } else {
        std::string err = GetLastAiError();
        if (err.empty()) return origIl2CppStr;
        suffix = "\n\n<color=#FF6B6B>[CardAI] Translation error: " + err + "</color>";
    }

    std::string base = Il2CppStrToStdString(origIl2CppStr);
    if (base.empty()) return origIl2CppStr;

    std::string combined = base + suffix;
    void* s = il2cpp_string_new(combined.c_str());
    return s ? s : origIl2CppStr;
}

// -------------------- Các hàm Hook trên YgomGame.Card.Content --------------------

// 1. GetDesc(int cardId, bool replaceAlnum)
static void* Hook_GetDesc(void* self, int cardId, bool replaceAlnum, void* methodInfo)
{
    if (g_bypassDescHooks) {
        return g_origGetDesc(self, cardId, replaceAlnum, methodInfo);
    }
    std::string viText;
    if (LookupOrTranslateCard(self, cardId, viText, "Content.GetDesc")) {
        std::string formatted = FormatFullViDescWithNewlines(self, cardId, viText);
        void* s = il2cpp_string_new(formatted.c_str());
        if (s) return s;
    }
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDesc(self, cardId, replaceAlnum, methodInfo);
    }
    return AppendAiErrorNoticeIfAny(origRes, self, cardId);
}

// 2. GetRawDesc(int cardId)
// Lưu ý: GetRawDesc được game dùng nội bộ trong getDescActiveEffect để cắt chuỗi theo byte offset tiếng Anh.
// Khi g_bypassDescHooks bật (hoặc gọi từ getDescActiveEffect), trả về nguyên bản tiếng Anh để không cắt lệch UTF-8.
static void* Hook_GetRawDesc(void* self, int cardId, void* methodInfo)
{
    return g_origGetRawDesc(self, cardId, methodInfo);
}

// 3. GetDescWithoutPendulum(int cardId, bool replaceAlnum)
static void* Hook_GetDescWithoutPendulum(void* self, int cardId, bool replaceAlnum, void* methodInfo)
{
    if (g_bypassDescHooks) {
        return g_origGetDescWithoutPendulum(self, cardId, replaceAlnum, methodInfo);
    }
    std::string viText;
    if (LookupOrTranslateCard(self, cardId, viText, "Content.GetDescWithoutPendulum")) {
        std::string pendText, monText;
        SplitPendulumAndMonsterText(viText, pendText, monText);
        if (!monText.empty()) {
            std::string formatted = FormatSectionWithEffectNewlines(self, cardId, monText, false);
            void* s = il2cpp_string_new(formatted.c_str());
            if (s) return s;
        }
    }
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDescWithoutPendulum(self, cardId, replaceAlnum, methodInfo);
    }
    return AppendAiErrorNoticeIfAny(origRes, self, cardId);
}

// 4. GetDescWithoutPendulumForCardInfo(int cardId, bool replaceAlnum)
static void* Hook_GetDescWithoutPendulumForCardInfo(void* self, int cardId, bool replaceAlnum, void* methodInfo)
{
    if (g_bypassDescHooks) {
        return g_origGetDescWithoutPendulumForCardInfo(self, cardId, replaceAlnum, methodInfo);
    }
    std::string viText;
    if (LookupOrTranslateCard(self, cardId, viText, "Content.GetDescWithoutPendulumForCardInfo")) {
        std::string pendText, monText;
        SplitPendulumAndMonsterText(viText, pendText, monText);
        if (!monText.empty()) {
            std::string formatted = FormatSectionWithEffectNewlines(self, cardId, monText, false);
            void* s = il2cpp_string_new(formatted.c_str());
            if (s) return s;
        }
    }
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDescWithoutPendulumForCardInfo(self, cardId, replaceAlnum, methodInfo);
    }
    return AppendAiErrorNoticeIfAny(origRes, self, cardId);
}

// 5. GetDescOfPendulum(int cardId, bool replaceAlnum)
static void* Hook_GetDescOfPendulum(void* self, int cardId, bool replaceAlnum, void* methodInfo)
{
    if (g_bypassDescHooks) {
        return g_origGetDescOfPendulum(self, cardId, replaceAlnum, methodInfo);
    }
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDescOfPendulum(self, cardId, replaceAlnum, methodInfo);
    }
    std::string origStr = Il2CppStrToStdString(origRes);
    if (origStr.empty()) return origRes; // Không phải bài Pendulum

    std::string viText;
    if (LookupOrTranslateCard(self, cardId, viText, "Content.GetDescOfPendulum")) {
        std::string pendText, monText;
        SplitPendulumAndMonsterText(viText, pendText, monText);
        if (!pendText.empty()) {
            std::string formatted = FormatSectionWithEffectNewlines(self, cardId, pendText, true);
            void* s = il2cpp_string_new(formatted.c_str());
            if (s) return s;
        }
    }
    return origRes;
}

// 6. GetDescOfPendulumForCardInfo(int cardId, bool replaceAlnum)
static void* Hook_GetDescOfPendulumForCardInfo(void* self, int cardId, bool replaceAlnum, void* methodInfo)
{
    if (g_bypassDescHooks) {
        return g_origGetDescOfPendulumForCardInfo(self, cardId, replaceAlnum, methodInfo);
    }
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDescOfPendulumForCardInfo(self, cardId, replaceAlnum, methodInfo);
    }
    std::string origStr = Il2CppStrToStdString(origRes);
    if (origStr.empty()) return origRes; // Không phải bài Pendulum

    std::string viText;
    if (LookupOrTranslateCard(self, cardId, viText, "Content.GetDescOfPendulumForCardInfo")) {
        std::string pendText, monText;
        SplitPendulumAndMonsterText(viText, pendText, monText);
        if (!pendText.empty()) {
            std::string formatted = FormatSectionWithEffectNewlines(self, cardId, pendText, true);
            void* s = il2cpp_string_new(formatted.c_str());
            if (s) return s;
        }
    }
    return origRes;
}

// 7. getDescActiveEffect(int cardId, int numOfEfx)
// Ánh xạ câu hiệu ứng tiếng Anh đang kích hoạt sang đúng câu tương ứng trong bản dịch tiếng Việt dựa theo dấu chấm câu.
static void* Hook_getDescActiveEffect(void* self, int cardId, int numOfEfx, void* methodInfo)
{
    void* origRes = nullptr;
    {
        ScopedBypassDescHooks bypass;
        origRes = g_origGetDescActiveEffect(self, cardId, numOfEfx, methodInfo);
    }
    if (g_bypassDescHooks) return origRes;

    std::string engEffect = Il2CppStrToStdString(origRes);
    if (engEffect.empty()) return origRes;

    std::string viText;
    if (!LookupOrTranslateCard(self, cardId, viText, "Content.getDescActiveEffect")) {
        return origRes;
    }

    std::string pendText, monText;
    SplitPendulumAndMonsterText(viText, pendText, monText);

    bool isPendulum = (numOfEfx >= 20 && !pendText.empty());
    std::string engSection;
    std::string rawViSection = isPendulum ? pendText : monText;
    {
        ScopedBypassDescHooks bypass;
        if (isPendulum && g_origGetDescOfPendulum) {
            void* engPendObj = g_origGetDescOfPendulum(self, cardId, true, nullptr);
            engSection = Il2CppStrToStdString(engPendObj);
        } else if (!isPendulum && g_origGetDescWithoutPendulum) {
            void* engMonObj = g_origGetDescWithoutPendulum(self, cardId, true, nullptr);
            engSection = Il2CppStrToStdString(engMonObj);
        }
    }

    if (!engSection.empty() && !rawViSection.empty()) {
        // Định dạng xuống dòng y hệt như chuỗi đã trả về cho UI để chuỗi con trả về khớp 100% khi Highlight
        std::string formattedViSection = FormatSectionWithEffectNewlines(self, cardId, rawViSection, isPendulum);
        std::string viEffect = MapEffectByPunctuation(engSection, engEffect, formattedViSection);
        if (!viEffect.empty()) {
            void* s = il2cpp_string_new(viEffect.c_str());
            if (s) return s;
        }
    }
    return origRes;
}

// -------------------- Các hàm Hook theo dõi UI đang mở để LiveRefresh --------------------

static void Hook_CardInfo_SetDescriptionArea(void* self, void* methodInfo)
{
    g_lastCardInfo = self;
    ScopedActiveCardPanel active;
    g_origCardInfo_SetDescriptionArea(self, methodInfo);
}

static void Hook_CardInfoDetail_SetDescriptionArea(void* self, void* cardinfodata, void* methodInfo)
{
    g_lastCardInfoDetail = self;
    g_lastCardInfoDetailArg = cardinfodata;
    ScopedActiveCardPanel active;
    g_origCardInfoDetail_SetDescriptionArea(self, cardinfodata, methodInfo);
}

static void Hook_CardDetailView_setCardText(void* self, void* methodInfo)
{
    g_lastCardDetailView = self;
    ScopedActiveCardPanel active;
    g_origCardDetailView_setCardText(self, methodInfo);
}

static void Hook_CardActionMenu_setCardText(void* self, void* methodInfo)
{
    g_lastCardActionMenu = self;
    ScopedActiveCardPanel active;
    g_origCardActionMenu_setCardText(self, methodInfo);
}

static void Hook_CardUtilWidget_setCardText(void* self, void* methodInfo)
{
    g_lastCardUtilWidget = self;
    ScopedActiveCardPanel active;
    g_origCardUtilWidget_setCardText(self, methodInfo);
}

static void Hook_CardDecoderActionMenu_setCardText(void* self, void* methodInfo)
{
    g_lastCardDecoderActionMenu = self;
    ScopedActiveCardPanel active;
    g_origCardDecoderActionMenu_setCardText(self, methodInfo);
}

static void Hook_LotteryActionMenu_setCardText(void* self, void* methodInfo)
{
    g_lastLotteryActionMenu = self;
    ScopedActiveCardPanel active;
    g_origLotteryActionMenu_setCardText(self, methodInfo);
}

static void Hook_LotteryDetailView_setCardText(void* self, void* methodInfo)
{
    g_lastLotteryDetailView = self;
    ScopedActiveCardPanel active;
    g_origLotteryDetailView_setCardText(self, methodInfo);
}

static void Hook_EnqueteDetailWidget_setCardText(void* self, void* methodInfo)
{
    g_lastEnqueteDetailWidget = self;
    ScopedActiveCardPanel active;
    g_origEnqueteDetailWidget_setCardText(self, methodInfo);
}

static void Hook_CardInfoForViewer_SetDescriptionArea(void* self, void* methodInfo)
{
    g_lastCardInfoForViewer = self;
    ScopedActiveCardPanel active;
    g_origCardInfoForViewer_SetDescriptionArea(self, methodInfo);
}

static void Hook_CardInfoGenericEx_SetDescriptionArea(void* self, void* methodInfo)
{
    g_lastCardInfoGenericEx = self;
    ScopedActiveCardPanel active;
    g_origCardInfoGenericEx_SetDescriptionArea(self, methodInfo);
}

static void TryInvokeRefresh(void* instance, t_InstanceVoid0 origFn, const char* name)
{
    if (!instance || !origFn) return;
    __try {
        if (IsUnityObjectAlive(instance)) {
            origFn(instance, nullptr);
            Log("[LiveRefresh] Refreshed %s on screen", name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LiveRefresh] Caught exception while refreshing %s (0x%08lX)", name, GetExceptionCode());
    }
}

static void SafeRefreshActivePanels()
{
    EnsureVietnameseFallbackFont();
    TryInvokeRefresh(g_lastCardInfo,              g_origCardInfo_SetDescriptionArea,       "Duel.CardInfo");
    if (g_lastCardInfoDetail && g_origCardInfoDetail_SetDescriptionArea && g_lastCardInfoDetailArg) {
        __try {
            if (IsUnityObjectAlive(g_lastCardInfoDetail)) {
                g_origCardInfoDetail_SetDescriptionArea(g_lastCardInfoDetail, g_lastCardInfoDetailArg, nullptr);
                Log("[LiveRefresh] Refreshed Duel.CardInfoDetail on screen");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[LiveRefresh] Caught exception while refreshing Duel.CardInfoDetail (0x%08lX)", GetExceptionCode());
        }
    }
    TryInvokeRefresh(g_lastCardDetailView,        g_origCardDetailView_setCardText,        "Deck.CardDetailView");
    TryInvokeRefresh(g_lastCardActionMenu,        g_origCardActionMenu_setCardText,        "Deck.CardActionMenu");
    TryInvokeRefresh(g_lastCardUtilWidget,        g_origCardUtilWidget_setCardText,        "Deck.CardUtilWidget");
    TryInvokeRefresh(g_lastCardDecoderActionMenu, g_origCardDecoderActionMenu_setCardText, "CardDecoderCardActionMenu");
    TryInvokeRefresh(g_lastLotteryActionMenu,     g_origLotteryActionMenu_setCardText,     "LotteryCardSelectCardActionMenu");
    TryInvokeRefresh(g_lastLotteryDetailView,     g_origLotteryDetailView_setCardText,     "LotteryCardSelectCardDetailView");
    TryInvokeRefresh(g_lastEnqueteDetailWidget,   g_origEnqueteDetailWidget_setCardText,   "EnqueteCardDetailWidget");
    TryInvokeRefresh(g_lastCardInfoForViewer,     g_origCardInfoForViewer_SetDescriptionArea, "CardInfoForViewer");
    TryInvokeRefresh(g_lastCardInfoGenericEx,     g_origCardInfoGenericEx_SetDescriptionArea, "CardInfoForGenericCardListEx");
}

// -------------------- Phím tắt trong game: F9 (Dịch lại lá đang xem) & F10 (Nạp lại config) --------------------
static bool IsMasterDuelForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void TriggerRetranslateLastViewedCard()
{
    void* contentSelf = nullptr;
    int cardId = 0;
    std::string cardName;
    {
        std::lock_guard<std::mutex> lk(g_lastViewedMutex);
        contentSelf = g_lastViewedContentSelf;
        cardId      = g_lastViewedCardId;
        cardName    = g_lastViewedCardName;
    }

    if (!contentSelf || cardId <= 0 || cardName.empty()) {
        Log("[Hotkey F9] No active card panel to re-translate.");
        return;
    }

    // Cho phép chạy lại kể cả khi trước đó đang tạm dừng do lỗi
    ClearAiError();

    std::string engDesc = GetCardEnglishDescForAI(contentSelf, cardId);
    if (engDesc.empty()) {
        Log("[Hotkey F9] Failed to fetch English description for '%s' (mrk=%d).", cardName.c_str(), cardId);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_cache.erase(cardName);
        g_notFoundOrPending.erase(cardName);
    }

    SetRetranslatingCard(cardName);
    Log("[Hotkey F9] Re-translate requested for card: '%s' (mrk=%d)", cardName.c_str(), cardId);
    EnqueueTranslate(cardName, engDesc);
    SafeRefreshActivePanels();
}

static void HotReloadAiConfig()
{
    std::string oldCachePath(g_cachePath);
    LoadOrCreateAiConfig(g_configPath);
    ClearAiError();

    if (oldCachePath != g_cachePath) {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_cache.clear();
        g_notFoundOrPending.clear();
        LoadCacheFile(g_cachePath);
    }

    AiConfig cfg = GetAiConfigSnapshot();
    Log("[Hotkey F10] Hot-reloaded config: model=%s | lang=%s | cache=%s",
        cfg.model.c_str(), cfg.targetLanguage.c_str(), g_cachePath);
    SafeRefreshActivePanels();
}

static void Hook_SelectorManager_Update(void* self, void* methodInfo)
{
    g_origSelectorManager_Update(self, methodInfo);
    if (!g_viFontInitTried) {
        EnsureVietnameseFallbackFont();
    }

    // Bắt phím tắt F9 / F10 (chỉ kích hoạt khi cửa sổ Master Duel đang ở foreground)
    static bool s_prevF9 = false;
    static bool s_prevF10 = false;
    if (IsMasterDuelForeground()) {
        bool f9Down  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
        bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f9Down && !s_prevF9) {
            TriggerRetranslateLastViewedCard();
        }
        if (f10Down && !s_prevF10) {
            HotReloadAiConfig();
        }
        s_prevF9  = f9Down;
        s_prevF10 = f10Down;
    } else {
        s_prevF9  = false;
        s_prevF10 = false;
    }

    if (g_pendingUIRefresh) {
        g_pendingUIRefresh = false;
        SafeRefreshActivePanels();
    }
}

// -------------------- Tiện ích gắn Hook --------------------
static bool InstallMethodHook(void* klass, const char* methodName, int paramCount, void* detour, void** origOut)
{
    if (!klass) return false;
    void* mi = il2cpp_class_get_method_from_name(klass, methodName, paramCount);
    if (!mi) {
        Log("Method not found: %s(%d)", methodName, paramCount);
        return false;
    }
    void* target = *(void**)mi;
    if (!target) {
        Log("Null methodPointer for %s(%d)", methodName, paramCount);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, origOut);
    if (s != MH_OK) {
        Log("MH_CreateHook failed for %s: %s", methodName, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    Log("Hooked %s(%d) @ %p -> %s", methodName, paramCount, target, MH_StatusToString(s));
    return s == MH_OK;
}

// -------------------- Khởi tạo --------------------
static DWORD WINAPI InitThread(LPVOID)
{
    Log("=== MDCardTranslator (AI Translate Edition) started in masterduel.exe ===");

    // Load AI API config from ai_config.json (or auto-create starter config if missing)
    LoadOrCreateAiConfig(g_configPath);

    // Load existing card translation cache
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        LoadCacheFile(g_cachePath);
    }

    // Khởi chạy worker thread phục vụ dịch AI bất đồng bộ
    CreateThread(nullptr, 0, TranslateWorkerThread, nullptr, 0, nullptr);

    HMODULE ga = nullptr;
    for (int i = 0; i < 240 && !(ga = GetModuleHandleA("GameAssembly.dll")); ++i) Sleep(500);
    if (!ga) { Log("GameAssembly.dll not found"); return 0; }
    if (!ResolveApi(ga)) return 0;

    void* imageCSharp = nullptr;
    void* imageTMP = nullptr;
    void* imageTextRendering = nullptr;
    void* imageCoreModule = nullptr;

    for (int i = 0; i < 480 && !imageCSharp; ++i) {
        Sleep(500);
        void* dom = il2cpp_domain_get();
        if (!dom) continue;
        il2cpp_thread_attach(dom);
        size_t n = 0;
        void** asms = il2cpp_domain_get_assemblies(dom, &n);
        for (size_t k = 0; asms && k < n; ++k) {
            void* img = il2cpp_assembly_get_image(asms[k]);
            const char* name = img ? il2cpp_image_get_name(img) : nullptr;
            if (!name) continue;
            if (strcmp(name, "Assembly-CSharp.dll") == 0) imageCSharp = img;
            else if (strcmp(name, "Unity.TextMeshPro.dll") == 0) imageTMP = img;
            else if (strcmp(name, "UnityEngine.TextRenderingModule.dll") == 0) imageTextRendering = img;
            else if (strcmp(name, "UnityEngine.CoreModule.dll") == 0) imageCoreModule = img;
        }
    }
    if (!imageCSharp) { Log("Assembly-CSharp.dll not found"); return 0; }
    Log("Found Assembly-CSharp.dll (%p), Unity.TextMeshPro.dll (%p)", imageCSharp, imageTMP);

    // 0. Tìm các class & method của TextMeshPro để xử lý Fallback Font tiếng Việt
    if (imageTMP) {
        g_clsTMPFontAsset = il2cpp_class_from_name(imageTMP, "TMPro", "TMP_FontAsset");
        g_clsTMPFontAssetUtilities = il2cpp_class_from_name(imageTMP, "TMPro", "TMP_FontAssetUtilities");
        g_clsTMPSettings = il2cpp_class_from_name(imageTMP, "TMPro", "TMP_Settings");

        if (g_clsTMPFontAsset) {
            void* mi7 = il2cpp_class_get_method_from_name(g_clsTMPFontAsset, "CreateFontAsset", 7);
            if (mi7) g_fnCreateFontAsset7 = (t_CreateFontAsset7)(*(void**)mi7);

            void* mi3 = il2cpp_class_get_method_from_name(g_clsTMPFontAsset, "CreateFontAsset", 3);
            if (mi3) g_fnCreateFontAsset3 = (t_CreateFontAsset3)(*(void**)mi3);

            void* mi1 = il2cpp_class_get_method_from_name(g_clsTMPFontAsset, "CreateFontAsset", 1);
            if (mi1) g_fnCreateFontAsset1 = (t_CreateFontAsset1)(*(void**)mi1);

            void* miAdd = il2cpp_class_get_method_from_name(g_clsTMPFontAsset, "TryAddCharacters", 2);
            if (miAdd) g_fnTryAddCharactersStr = (t_TryAddCharactersStr)(*(void**)miAdd);

            void* miFb = il2cpp_class_get_method_from_name(g_clsTMPFontAsset, "get_fallbackFontAssetTable", 0);
            if (miFb) g_fnGetFallbackTable = (t_GetFallbackTable)(*(void**)miFb);
        }
    }
    if (imageTextRendering) {
        g_clsUnityFont = il2cpp_class_from_name(imageTextRendering, "UnityEngine", "Font");
        if (g_clsUnityFont) {
            void* miCreate = il2cpp_class_get_method_from_name(g_clsUnityFont, "Internal_CreateFont", 2);
            if (miCreate) g_fnInternalCreateFont = (t_InternalCreateFont)(*(void**)miCreate);
        }
    }
    if (imageCoreModule) {
        void* clsObj = il2cpp_class_from_name(imageCoreModule, "UnityEngine", "Object");
        if (clsObj) {
            void* miDdol = il2cpp_class_get_method_from_name(clsObj, "DontDestroyOnLoad", 1);
            if (miDdol) g_fnDontDestroyOnLoad = (t_DontDestroyOnLoad)(*(void**)miDdol);
        }
    }

    // 1. Tìm YgomGame.Card.Content và hàm lấy tên bài
    g_clsContent = il2cpp_class_from_name(imageCSharp, "YgomGame.Card", "Content");
    if (!g_clsContent) {
        Log("Class YgomGame.Card.Content not found!");
        return 0;
    }
    void* miGetName = il2cpp_class_get_method_from_name(g_clsContent, "GetName", 2);
    if (miGetName) g_fnContentGetName = (t_Content_GetName)(*(void**)miGetName);
    Log("YgomGame.Card.Content=%p, GetName=%p", g_clsContent, g_fnContentGetName);

    // 2. Tìm các class UI hiển thị mô tả bài
    g_clsCardInfo              = il2cpp_class_from_name(imageCSharp, "YgomGame.Duel", "CardInfo");
    g_clsCardInfoBase          = il2cpp_class_from_name(imageCSharp, "YgomGame.Duel", "CardInfoBase");
    g_clsCardInfoDetail        = il2cpp_class_from_name(imageCSharp, "YgomGame.Duel", "CardInfoDetail");
    g_clsCardDetailView        = il2cpp_class_from_name(imageCSharp, "YgomGame.Deck", "CardDetailView");
    g_clsCardActionMenu        = il2cpp_class_from_name(imageCSharp, "YgomGame.Deck", "CardActionMenu");
    g_clsCardUtilWidget        = il2cpp_class_from_name(imageCSharp, "YgomGame.Deck", "CardUtilWidget");
    g_clsCardDecoderActionMenu = il2cpp_class_from_name(imageCSharp, "YgomGame.CardDecoder", "CardDecoderCardActionMenu");
    g_clsLotteryActionMenu     = il2cpp_class_from_name(imageCSharp, "YgomGame.Lottery", "LotteryCardSelectCardActionMenu");
    g_clsLotteryDetailView     = il2cpp_class_from_name(imageCSharp, "YgomGame.Lottery", "LotteryCardSelectCardDetailView");
    g_clsEnqueteDetailWidget   = il2cpp_class_from_name(imageCSharp, "YgomGame.Enquete", "EnqueteCardDetailWidget");
    g_clsCardInfoForViewer     = il2cpp_class_from_name(imageCSharp, "YgomGame.PhysicalCardViewer", "CardInfoForViewer");
    g_clsCardInfoGenericEx     = il2cpp_class_from_name(imageCSharp, "YgomGame.Duel", "CardInfoForGenericCardListEx");
    g_clsSelectorManager       = il2cpp_class_from_name(imageCSharp, "YgomSystem.UI", "SelectorManager");

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }

    // Hook TMP_FontAssetUtilities.GetCharacterFromFontAsset_Internal(6) để hiển thị đầy đủ tiếng Việt
    if (g_clsTMPFontAssetUtilities) {
        InstallMethodHook(g_clsTMPFontAssetUtilities, "GetCharacterFromFontAsset_Internal", 6,
            (void*)Hook_GetCharFromFontAssetInternal, (void**)&g_origGetCharInternal);
    }

    // 3. Hook các hàm lấy mô tả bài trong YgomGame.Card.Content
    InstallMethodHook(g_clsContent, "GetDesc",                           2, (void*)Hook_GetDesc,                           (void**)&g_origGetDesc);
    InstallMethodHook(g_clsContent, "GetRawDesc",                        1, (void*)Hook_GetRawDesc,                        (void**)&g_origGetRawDesc);
    InstallMethodHook(g_clsContent, "GetDescWithoutPendulum",            2, (void*)Hook_GetDescWithoutPendulum,            (void**)&g_origGetDescWithoutPendulum);
    InstallMethodHook(g_clsContent, "GetDescOfPendulum",                 2, (void*)Hook_GetDescOfPendulum,                 (void**)&g_origGetDescOfPendulum);
    InstallMethodHook(g_clsContent, "GetDescWithoutPendulumForCardInfo", 2, (void*)Hook_GetDescWithoutPendulumForCardInfo, (void**)&g_origGetDescWithoutPendulumForCardInfo);
    InstallMethodHook(g_clsContent, "GetDescOfPendulumForCardInfo",      2, (void*)Hook_GetDescOfPendulumForCardInfo,      (void**)&g_origGetDescOfPendulumForCardInfo);
    InstallMethodHook(g_clsContent, "getDescActiveEffect",               2, (void*)Hook_getDescActiveEffect,               (void**)&g_origGetDescActiveEffect);

    // 4. Hook các hàm hiển thị UI để tự động cập nhật ngay khi dịch AI xong
    InstallMethodHook(g_clsCardInfo,              "SetDescriptionArea", 0, (void*)Hook_CardInfo_SetDescriptionArea,       (void**)&g_origCardInfo_SetDescriptionArea);
    InstallMethodHook(g_clsCardInfoDetail,        "SetDescriptionArea", 1, (void*)Hook_CardInfoDetail_SetDescriptionArea, (void**)&g_origCardInfoDetail_SetDescriptionArea);
    InstallMethodHook(g_clsCardDetailView,        "setCardText",        0, (void*)Hook_CardDetailView_setCardText,        (void**)&g_origCardDetailView_setCardText);
    InstallMethodHook(g_clsCardActionMenu,        "setCardText",        0, (void*)Hook_CardActionMenu_setCardText,        (void**)&g_origCardActionMenu_setCardText);
    InstallMethodHook(g_clsCardUtilWidget,        "setCardText",        0, (void*)Hook_CardUtilWidget_setCardText,        (void**)&g_origCardUtilWidget_setCardText);
    InstallMethodHook(g_clsCardDecoderActionMenu, "setCardText",        0, (void*)Hook_CardDecoderActionMenu_setCardText, (void**)&g_origCardDecoderActionMenu_setCardText);
    InstallMethodHook(g_clsLotteryActionMenu,     "setCardText",        0, (void*)Hook_LotteryActionMenu_setCardText,     (void**)&g_origLotteryActionMenu_setCardText);
    InstallMethodHook(g_clsLotteryDetailView,     "setCardText",        0, (void*)Hook_LotteryDetailView_setCardText,     (void**)&g_origLotteryDetailView_setCardText);
    InstallMethodHook(g_clsEnqueteDetailWidget,   "setCardText",        0, (void*)Hook_EnqueteDetailWidget_setCardText,   (void**)&g_origEnqueteDetailWidget_setCardText);
    InstallMethodHook(g_clsCardInfoForViewer,     "SetDescriptionArea", 0, (void*)Hook_CardInfoForViewer_SetDescriptionArea, (void**)&g_origCardInfoForViewer_SetDescriptionArea);
    InstallMethodHook(g_clsCardInfoGenericEx,     "SetDescriptionArea", 0, (void*)Hook_CardInfoGenericEx_SetDescriptionArea, (void**)&g_origCardInfoGenericEx_SetDescriptionArea);

    // 5. Hook SelectorManager.Update (chạy mỗi frame trên main thread của Unity)
    InstallMethodHook(g_clsSelectorManager,       "Update",             0, (void*)Hook_SelectorManager_Update,            (void**)&g_origSelectorManager_Update);

    Log("=== All hooks for Yu-Gi-Oh! Master Duel (with fallback font & F9/F10 hotkeys) are ready! ===");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);

        // Chỉ kích hoạt trong tiến trình masterduel.exe (bỏ qua UnityCrashHandler64.exe...)
        const char* baseName = strrchr(exePath, '\\');
        baseName = baseName ? (baseName + 1) : exePath;
        if (_stricmp(baseName, "masterduel.exe") != 0) return TRUE;

        InitializeCriticalSection(&g_logCs);
        char gameDir[MAX_PATH] = {0};
        strcpy_s(gameDir, exePath);
        char* slash = strrchr(gameDir, '\\');
        if (slash) *(slash + 1) = '\0';

        sprintf_s(g_baseDir, "%sMDCardTranslator\\", gameDir);
        CreateDirectoryA(g_baseDir, nullptr);

        sprintf_s(g_logPath, "%scard_translator.log", g_baseDir);
        sprintf_s(g_cachePath, "%scards_vietnamese.json", g_baseDir);
        sprintf_s(g_configPath, "%sai_config.json", g_baseDir);

        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
