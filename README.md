# MDCardTranslator — Mod Dịch Mô Tả Bài Bằng AI Cho Yu-Gi-Oh! Master Duel

[English](README.en.md) | [Tiếng Việt](README.md)

Mod DLL proxy (`version.dll`) giúp tự động dịch **mô tả hiệu ứng lá bài** sang tiếng Việt trực tiếp trong **Yu-Gi-Oh! Master Duel** bằng API AI chuẩn OpenAI-compatible, đồng thời giữ nguyên tên lá bài tiếng Anh.

## Tính năng chính

- **Chỉ dịch mô tả, giữ nguyên tên bài:** Tên lá bài và tên archetype nằm trong dấu ngoặc kép (`"..."`) luôn được giữ nguyên bằng tiếng Anh để dễ tra cứu và đúng chuẩn cộng đồng Yu-Gi-Oh! Việt Nam.
- **Dịch tự động bằng AI API (chuẩn OpenAI-compatible):** Lấy trực tiếp nguyên văn mô tả tiếng Anh trong game (hỗ trợ cả **Pendulum Effect** và **Monster Effect**, bao gồm cả bài mới ra mắt hoặc Token) và gửi dịch bất đồng bộ ở luồng nền, không gây giật lag game.
- **Cache vĩnh viễn (`cards_vietnamese.json`):** Mỗi lá bài chỉ cần gọi API dịch **đúng 1 lần**, sau đó được lưu vào `cards_vietnamese.json` để hiển thị tức thì cho mọi lần sau và tiết kiệm chi phí API.
- **Tự động xuống dòng theo từng hiệu ứng:** Phân tách rõ ràng từng hiệu ứng/điều kiện riêng biệt xuống dòng mới (`\n`), giúp đọc mô tả dài dễ dàng hơn.
- **Căn chỉnh Highlight hiệu ứng chính xác:** Tự động ánh xạ hiệu ứng đang kích hoạt theo câu và dấu chấm câu để phần bôi xanh (highlight) và biểu tượng số thứ tự hiệu ứng trong Duel luôn khớp chính xác với văn bản tiếng Việt.
- **Sửa lỗi font tiếng Việt (ô vuông):** Tự động gắn Fallback Font hệ thống (`Arial` / `Segoe UI` / `Tahoma`) vào TextMeshPro để hiển thị đầy đủ dấu tiếng Việt.
- **Cập nhật trực tiếp (Live Refresh):** Khi một lá bài mới dịch xong ở nền, giao diện mô tả đang mở sẽ tự động làm mới ngay lập tức.
- **Phím tắt ngay trong game (`F9` / `F10`):**
  - **`F9`:** Dịch lại ngay lá bài bạn đang xem (xóa bản dịch cũ khỏi cache, hiện trạng thái *"Đang dịch lại bằng AI..."*, tự động cập nhật bản dịch mới và lưu đè vào file cache).
  - **`F10`:** Nạp lại cấu hình `ai_config.json` nóng (đổi model, đổi `target_language`, sửa prompt, xóa trạng thái tạm dừng lỗi) mà **không cần tắt mở lại game**.

---

## Hướng dẫn cài đặt & cấu hình

### 1. Đặt file vào thư mục game
Đặt các thành phần sau vào thư mục chứa `masterduel.exe` (thường là `...\Steam\steamapps\common\Yu-Gi-Oh!  Master Duel\`):
- `version.dll` *(đặt ở thư mục gốc, cùng cấp với `masterduel.exe`)*
- Thư mục `MDCardTranslator\` chứa `ai_config.json` (sao chép từ `ai_config.example.json`) và các công cụ đi kèm.

### 2. Cấu hình API AI (`MDCardTranslator/ai_config.json`)
Mở file `MDCardTranslator\ai_config.json` và điền `api_key` của bạn. File mặc định đang cấu hình sẵn cho **Google Gemini**:

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

#### Cấu hình gợi ý cho các nhà cung cấp phổ biến

| Nhà cung cấp | `api_url` | `model` gợi ý | Ghi chú |
| :--- | :--- | :--- | :--- |
| **Google Gemini** *(mặc định)* | `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` | `gemini-3.8-flash` | Nhanh, chất lượng dịch tiếng Việt tốt |
| **OpenAI** | `https://api.openai.com/v1/chat/completions` | `gpt-4o-mini` | Ổn định, chi phí thấp |
| **DeepSeek** | `https://api.deepseek.com/chat/completions` | `deepseek-chat` | Giá rẻ, dịch thuật ngữ tốt |
| **Groq** | `https://api.groq.com/openai/v1/chat/completions` | `llama-3.3-70b-versatile` | Tốc độ phản hồi rất nhanh |
| **OpenRouter** | `https://openrouter.ai/api/v1/chat/completions` | `google/gemini-2.5-flash` | Dùng 1 key cho nhiều model |
| **Ollama (Local)** | `http://localhost:11434/v1/chat/completions` | `qwen2.5:7b` / `llama3.1` | Chạy offline, để trống `api_key` |
| **LM Studio (Local)** | `http://localhost:1234/v1/chat/completions` | *(tên model đang load)* | Chạy offline, để trống `api_key` |

#### Ý nghĩa các trường cấu hình
- `api_url`: Đường dẫn đầy đủ tới endpoint `/chat/completions` chuẩn OpenAI.
- `api_key`: API key của nhà cung cấp (để `""` nếu dùng model chạy local qua `localhost` / `127.0.0.1`).
- `model`: Tên model dùng để dịch.
- `target_language`: Ngôn ngữ đích cần dịch (mặc định `"Vietnamese"` lưu cache tại `cards_vietnamese.json`; có thể đổi sang bất kỳ ngôn ngữ nào như `"Indonesian"`, `"Thai"`, `"Spanish"`, `"French"`... và mod sẽ tự tách file cache riêng `cards_<ngôn_ngữ>.json`).
- `timeout_ms`: Thời gian chờ tối đa cho mỗi yêu cầu API (mili-giây, mặc định `30000`).
- `delay_ms`: Khoảng nghỉ giữa các lần gọi API liên tiếp để tránh vượt giới hạn tốc độ (mili-giây, mặc định `1500`).
- `system_prompt`: Để trống `""` để sử dụng prompt mặc định đã tối ưu cho Yu-Gi-Oh! Master Duel (giữ thuật ngữ, giữ cấu trúc câu 1-1 cho highlight), hoặc tự nhập prompt riêng của bạn.

### 3. Kiểm tra nhanh cấu hình (không cần mở game)
Sau khi chỉnh sửa `ai_config.json`, bạn có thể chạy trực tiếp **`test_ai_config.bat`** (nháy đúp trong Windows Explorer) hoặc chạy qua PowerShell:

```powershell
.\test_ai_config.ps1
```

Công cụ sẽ gửi thử một lá bài mẫu (`Ash Blossom & Joyous Spring`) tới API AI với đúng cấu hình và prompt của mod, hiển thị tốc độ phản hồi (ms), bản dịch tiếng Việt hoặc mã lỗi chi tiết ngay lập tức.

---

## Tự biên dịch từ mã nguồn

Yêu cầu: **Visual Studio 2022** hoặc **Visual Studio 2022 Build Tools** (Desktop development with C++).

Chạy script biên dịch:

```cmd
MDCardTranslator\src\build.bat
```

File `version.dll` sau khi biên dịch sẽ nằm tại `MDCardTranslator\src\out\version.dll`. Sao chép file này ra thư mục gốc của game (cùng cấp với `masterduel.exe`).

---

## Cấu trúc dự án

```text
.
├── README.md                         # Tài liệu hướng dẫn (Tiếng Việt)
├── README.en.md                      # Tài liệu hướng dẫn (English)
├── version.dll                       # DLL proxy đã biên dịch sẵn (đặt cùng cấp masterduel.exe)
└── MDCardTranslator/
    ├── ai_config.example.json        # File cấu hình AI mẫu
    ├── ai_config.json                # File cấu hình AI (sao chép từ ai_config.example.json)
    ├── cards_<language>.json         # Cache mô tả bài theo từng ngôn ngữ (tự tạo khi dịch)
    ├── card_translator.log           # File log tiếng Anh
    ├── test_ai_config.bat            # Chạy nhanh kiểm tra cấu hình AI (nháy đúp)
    ├── test_ai_config.ps1            # Script PowerShell kiểm tra cấu hình AI
    └── src/
        ├── build.bat                 # Script biên dịch MSVC x64
        ├── dllmain.cpp               # Mã nguồn chính (IL2CPP hook, AI client, highlight, font fallback, phím tắt)
        └── minhook/                  # Thư viện MinHook
```

## Xử lý sự cố

- **Bài chưa được dịch / báo lỗi API:** Mở file `MDCardTranslator\card_translator.log` để kiểm tra mã HTTP và thông báo lỗi chi tiết (ví dụ: sai `api_key`, hết quota `HTTP 429`, sai tên `model`...).
- **Cập nhật bản dịch thủ công:** Bạn có thể mở trực tiếp `MDCardTranslator\cards_<language>.json` để chỉnh sửa câu chữ của bất kỳ lá bài nào, hoặc nhấn **`F9`** ngay trong game để AI dịch lại lá bài đang xem.
- **Gỡ cài đặt mod:** Chỉ cần xóa (hoặc đổi tên) file `version.dll` trong thư mục game.
