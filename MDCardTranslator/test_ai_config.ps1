param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot "ai_config.json"),
    [string]$Text = ""
)

[Console]::InputEncoding  = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding           = [System.Text.UTF8Encoding]::new($false)

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  MDCardTranslator AI Config Tester (No Game Launch Needed)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if (-not (Test-Path $ConfigPath)) {
    Write-Host "[ERROR] Config file not found: $ConfigPath" -ForegroundColor Red
    Write-Host "Copy ai_config.example.json to ai_config.json and fill in your api_key." -ForegroundColor Yellow
    exit 1
}

try {
    $cfg = Get-Content -Raw -Encoding UTF8 $ConfigPath | ConvertFrom-Json
} catch {
    Write-Host "[LOI] File JSON khong hop le ($ConfigPath): $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$apiUrl = if ($cfg.api_url) { $cfg.api_url.Trim() } else { "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions" }
$apiKey = if ($cfg.api_key) { $cfg.api_key.Trim() } else { "" }
$model  = if ($cfg.model)   { $cfg.model.Trim() }   else { "gemini-3.8-flash" }
$targetLang = if ($cfg.target_language -and -not [string]::IsNullOrWhiteSpace($cfg.target_language)) { $cfg.target_language.Trim() } else { "Vietnamese" }
$timeoutSec = if ($cfg.timeout_ms -and [int]$cfg.timeout_ms -ge 1000) { [math]::Ceiling([int]$cfg.timeout_ms / 1000) } else { 30 }
$reasoningEffort = if ($cfg.reasoning_effort) { $cfg.reasoning_effort.Trim() } else { "" }

if ([string]::IsNullOrEmpty($reasoningEffort) -and $apiUrl -match "generativelanguage\.googleapis\.com" -and $model -match "gemini-3") {
    $reasoningEffort = "low"
}

$isVi = ($targetLang -match '^(vi|vietnamese)$' -or $targetLang -match 'viet|việt')
if ($isVi) {
    $defaultPrompt = @"
Bạn là chuyên gia dịch thuật thẻ bài Yu-Gi-Oh! sang Tiếng Việt theo văn phong tự nhiên, ngắn gọn, chính xác của cộng đồng Yu-Gi-Oh! Việt Nam.
Quy tắc bắt buộc:
1. CHỈ sử dụng Tiếng Việt chuẩn và các thuật ngữ tiếng Anh trong mục 2 & 3. TUYỆT ĐỐI KHÔNG trộn lẫn ký tự ngôn ngữ khác (như tiếng Thái, Trung, Nhật, Hàn...). Luôn đặt đúng dấu cách giữa từ Tiếng Việt và thuật ngữ tiếng Anh (không viết dính chữ).
2. GIỮ NGUYÊN tiếng Anh đối với mọi tên lá bài và tên archetype nằm trong dấu ngoặc kép "...".
3. Chỉ giữ nguyên tiếng Anh đối với các thuật ngữ cốt lõi của Yu-Gi-Oh!: Special Summon, Normal Summon, Fusion/Synchro/Xyz/Link/Pendulum Summon, GY, Banish, Quick Effect, ATK, DEF, LP, Deck, Extra Deck, Field, Main Phase, Battle Phase, End Phase, Spell, Trap, Tuner, Token, Counter, Material. BẮT BUỘC dịch sang Tiếng Việt các động từ và danh từ thông thường (ví dụ: discard -> bỏ, negate -> vô hiệu hóa, destroy -> phá hủy, target -> chọn mục tiêu, draw -> rút, send -> gửi, add -> thêm, turn -> lượt, hand -> tay, once per turn -> một lần mỗi lượt).
4. GIỮ NGUYÊN cấu trúc câu và dấu câu (dấu chấm '.', dấu hai chấm ':', dấu chấm phẩy ';', ký hiệu '●') khớp 1-1 với bản tiếng Anh: không gộp 2 câu thành 1 và không tách 1 câu thành 2 để tương thích với hệ thống highlight hiệu ứng trong game.
5. Nếu đầu vào có phần [ Pendulum Effect ] và [ Monster Effect ] ngăn cách bởi ----------------------------------------, hãy giữ nguyên chính xác các dòng tiêu đề và dấu ngăn cách đó.
6. Chỉ trả về đúng phần văn bản mô tả đã dịch, không kèm lời giải thích, không kèm tên lá bài và không bọc trong markdown code block.
"@
} else {
    $defaultPrompt = @"
You are an expert Yu-Gi-Oh! card text translator. Translate the English card effect text into natural, concise, accurate $targetLang.
Strict rules:
1. Write ONLY in $targetLang (plus the English terms allowed in rules 2 & 3). NEVER leak characters from any unrelated language, and always keep proper spacing around English terms.
2. Keep all card names and archetype names inside double quotes "..." strictly in English.
3. Translate all common verbs and nouns (such as discard, negate, destroy, target, draw, send, add, turn, hand, once per turn) naturally into $targetLang. Only keep core Yu-Gi-Oh! game terms in English where standard for the $targetLang community (e.g., ATK, DEF, LP, GY, Quick Effect, Special Summon, Extra Deck).
4. Preserve exact 1-to-1 sentence structure and punctuation ('.', ':', ';', '●') matching the English text: do NOT merge two sentences into one and do NOT split one sentence into two.
5. If the input contains [ Pendulum Effect ] and [ Monster Effect ] separated by ----------------------------------------, keep those exact headers and separator line unchanged.
6. Output ONLY the translated description text, with no explanations, no card name header, and no markdown code fences.
"@
}

$systemPrompt = if ($cfg.system_prompt -and -not [string]::IsNullOrWhiteSpace($cfg.system_prompt)) {
    $cfg.system_prompt
} else {
    $defaultPrompt
}

$maskedKey = "(trong / local)"
if (-not [string]::IsNullOrEmpty($apiKey)) {
    if ($apiKey -eq "YOUR_API_KEY_HERE") {
        $maskedKey = "YOUR_API_KEY_HERE (CHUA DIEN!)"
    } elseif ($apiKey.Length -gt 10) {
        $maskedKey = $apiKey.Substring(0, 6) + "..." + $apiKey.Substring($apiKey.Length - 4)
    } else {
        $maskedKey = "***"
    }
}

Write-Host "File config : $ConfigPath"
Write-Host "API URL     : $apiUrl"
Write-Host "Model       : $model"
Write-Host "Target Lang : $targetLang"
Write-Host "API Key     : $maskedKey"
Write-Host "Timeout     : ${timeoutSec}s"
if ($reasoningEffort) {
    Write-Host "Reasoning   : $reasoningEffort"
}
Write-Host ""

$isLocal = ($apiUrl -match "localhost" -or $apiUrl -match "127\.0\.0\.1")
if (-not $isLocal -and ([string]::IsNullOrEmpty($apiKey) -or $apiKey -eq "YOUR_API_KEY_HERE")) {
    Write-Host "[LOI] Ban chua dien api_key trong ai_config.json!" -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Text)) {
    $Text = 'When a card or effect is activated that includes any of these effects (Quick Effect): You can discard this card; negate that effect. ● Add a card from the Deck to the hand. ● Special Summon from the Deck. ● Send a card from the Deck to the GY. You can only use this effect of "Ash Blossom & Joyous Spring" once per turn.'
}

Write-Host "--- Mo ta tieng Anh mau ---" -ForegroundColor DarkGray
Write-Host $Text -ForegroundColor Gray
Write-Host ""
Write-Host "Dang gui yeu cau dich toi API..." -ForegroundColor Yellow

$payloadObj = [ordered]@{
    model       = $model
    temperature = 0.1
    messages    = @(
        @{ role = "system"; content = $systemPrompt },
        @{ role = "user";   content = $Text }
    )
}
if (-not [string]::IsNullOrEmpty($reasoningEffort)) {
    $payloadObj["reasoning_effort"] = $reasoningEffort
}

$bodyJson  = $payloadObj | ConvertTo-Json -Depth 5 -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

$headers = @{
    "Content-Type" = "application/json; charset=utf-8"
}
if (-not [string]::IsNullOrEmpty($apiKey) -and $apiKey -ne "YOUR_API_KEY_HERE") {
    $headers["Authorization"] = "Bearer $apiKey"
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    # Dung Invoke-WebRequest + doc RawContentStream bang UTF-8 de tranh loi PowerShell 5.1 mac dinh giai ma Latin-1
    $webResp = Invoke-WebRequest -Uri $apiUrl -Method Post -Headers $headers -Body $bodyBytes -TimeoutSec $timeoutSec -UseBasicParsing -ErrorAction Stop
    $sw.Stop()

    $rawJson = ""
    if ($webResp.RawContentStream) {
        $webResp.RawContentStream.Position = 0
        $reader = New-Object System.IO.StreamReader($webResp.RawContentStream, [System.Text.Encoding]::UTF8)
        $rawJson = $reader.ReadToEnd()
        $reader.Close()
    } else {
        $rawJson = [System.Text.Encoding]::UTF8.GetString([System.Text.Encoding]::GetEncoding("ISO-8859-1").GetBytes($webResp.Content))
    }

    $resp = $rawJson | ConvertFrom-Json
    $translated = $resp.choices[0].message.content
    if ([string]::IsNullOrWhiteSpace($translated)) {
        Write-Host "[THAT BAI] API tra ve 200 nhung noi dung rong! (Thoi gian: $($sw.ElapsedMilliseconds) ms)" -ForegroundColor Red
        exit 1
    }

    Write-Host "[THANH CONG] HTTP 200 | Thoi gian phan hoi: $($sw.ElapsedMilliseconds) ms" -ForegroundColor Green
    Write-Host ""
    Write-Host "--- Ket qua dich tieng Viet ---" -ForegroundColor Cyan
    Write-Host $translated.Trim() -ForegroundColor White
    Write-Host "-------------------------------" -ForegroundColor Cyan
    exit 0
} catch {
    $sw.Stop()
    $statusCode = $null
    $respBody   = ""

    if ($_.Exception.Response) {
        try { $statusCode = [int]$_.Exception.Response.StatusCode } catch {}
        try {
            $stream = $_.Exception.Response.GetResponseStream()
            if ($stream) {
                $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
                $respBody = $reader.ReadToEnd()
                $reader.Close()
            }
        } catch {}
    }

    Write-Host "[THAT BAI] Thoi gian: $($sw.ElapsedMilliseconds) ms" -ForegroundColor Red
    if ($statusCode) {
        Write-Host "HTTP Status : $statusCode" -ForegroundColor Red
    }
    Write-Host "Loi chi tiet: $($_.Exception.Message)" -ForegroundColor Red

    if (-not [string]::IsNullOrWhiteSpace($respBody)) {
        Write-Host ""
        Write-Host "--- Phan hoi tu may chu ---" -ForegroundColor DarkYellow
        Write-Host $respBody -ForegroundColor Yellow
    }

    Write-Host ""
    if ($statusCode -eq 401 -or $statusCode -eq 403) {
        Write-Host "Goi y: Kiem tra lai api_key trong ai_config.json." -ForegroundColor Cyan
    } elseif ($statusCode -eq 404) {
        Write-Host "Goi y: Ten model '$model' khong ton tai hoac da ngung ho tro. Hay doi sang model khac (vd: gemini-2.5-flash-lite, gemini-3.8-flash)." -ForegroundColor Cyan
    } elseif ($statusCode -eq 429) {
        Write-Host "Goi y: Vuot gioi han toc do/quota. Hay cho vai giay hoac tang delay_ms trong ai_config.json." -ForegroundColor Cyan
    } elseif ($statusCode -eq 503) {
        Write-Host "Goi y: Model '$model' dang bi qua tai phia may chu. Hay thu lai sau hoac doi model nhe hon (vd: gemini-2.5-flash-lite hoac Groq)." -ForegroundColor Cyan
    }
    exit 1
}
