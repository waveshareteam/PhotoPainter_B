# 專案介紹: PhotoPainter_B

## 專案概述
PhotoPainter_B 是一個基於 Raspberry Pi Pico (或 Pico 2) 與 Waveshare 7.3吋電子紙 (e-Paper) 的電子相框開源專案。該專案的主要功能是從 SD 卡中讀取經過處理的圖像（如 Floyd-Steinberg 抖動演算法處理後的 6 色圖片），並將其顯示在低功耗的電子紙螢幕上。專案內建電池管理、RTC (即時時鐘) 定時喚醒、按鍵觸發更新以及基於 SPI 的 SD 卡文件系統支援，特別適合用作長時間運作的無電源線裝飾相框。

---

## 系統工作流程 (Workflow)

系統的主要執行流程位於 `main.c`，核心邏輯圍繞在「極致省電」的原則下運行。步驟如下：

1. **硬體與模組初始化**: 
   - 初始化周邊硬體 (GPIO, SPI, I2C 等介面)。
   - 啟動看門狗計時器 (Watchdog) 防止系統死機。
   - 初始化 RTC (PCF85063) 模組並註冊中斷。
   - 註冊充電狀態中斷回呼函數。

2. **電池電量檢測**:
   - 系統啟動後，立刻透過 ADC 讀取電池電壓 (`measureVBAT()`)。
   - 若電壓過低 (< 3.1V)，則進入低電量保護模式：LED 閃爍提示、關閉 RTC 喚醒功能，並自動切斷電源 (`powerOff()`) 避免電池過放。

3. **SD 卡掃描與文件讀取**:
   - 檢測是否插入 SD 卡 (`sdTest()`)。
   - 根據原始碼中定義的 `Mode` (0, 1, 2) 掃描 SD 卡內的資料夾與圖片文件：
     - **Mode 0**: 自動獲取圖片資料夾並自動排序。
     - **Mode 1**: 自動獲取圖片資料夾但不排序。
     - **Mode 2**: 使用者需自行建立 `fileList.txt` 指定圖片順序。

4. **顯示與電源狀態管理**:
   - **未充電狀態 (使用電池)**: 
     讀取並顯示圖片 (`run_display()`)，接著設定下一次 RTC 喚醒的時間，最後**直接切斷系統總電源** (`powerOff()`)。系統將完全斷電直到下一次 RTC 定時器中斷或使用者按下按鍵將其喚醒。
   - **充電狀態 (連接 USB VBUS)**: 
     進入持續運行循環，監聽充電狀態 (切換 LED 指示燈)。在此狀態下，若 RTC 時間到達觸發中斷，或使用者按下實體按鍵 (`BAT_STATE`)，則會刷新顯示下一張圖片。

---

## 各部件功能說明 (Component Functions)

專案目錄結構主要由 `lib` (底層硬體與模組驅動) 與 `examples` (應用層代碼) 等資料夾組成，分別負責不同的功能：

### 1. 核心控制與硬體設定
- **`main.c`**: 程式主入口，負責總體流程控制、工作模式判定、電源管理與事件觸發。
- **`lib/Config/`**: 硬體設定層 (如 `DEV_Config`)，定義了 Pico 腳位分配 (GPIO mapping)、硬體介面初始化以及基本的暫存器操作巨集。

### 2. 電子紙與顯示驅動 (Display)
- **`lib/e-Paper/`**: 電子紙底層驅動。包含與 Waveshare 7.3吋電子紙 (如 `EPD_7in3e`) 通訊、發送初始化參數、刷新指令與傳送圖片數據的驅動程式碼。
- **`lib/GUI/` 及 `lib/Fonts/`**: 點陣圖形繪製庫 (畫點、線、圓、矩形等) 以及字庫，供需要顯示文字、電量資訊或簡單介面時使用。

### 3. 文件系統與存儲 (Storage)
- **`lib/FatFs_SPI/`**: 基於 SPI 介面的 SD 卡底層驅動及開源的 FatFs 文件系統。用於掛載 SD 卡、讀取目錄結構與讀取檔案。
- **`examples/run_File.c`**: 應用層的文件處理邏輯，負責在 SD 卡中遍歷尋找圖片檔、管理路徑清單，並讀取 BMP 圖片資料流送往電子紙顯示函數。

### 4. 電源與時間管理 (Power & Time)
- **`lib/RTC/`**: 外部即時時鐘模組 (`PCF85063`) 驅動程式。負責時間的設定與讀取，最重要的是提供獨立的定時喚醒 (Alarm) 中斷訊號，實現電子相框在斷電下的定時更新。
- **`lib/led/`**: LED 狀態指示燈控制，用於在系統低電量、充電中或充電完成時顯示不同的燈號。

### 5. 圖片轉換支援 (`convert.py`)
為了能在多色電子紙上呈現最佳的視覺效果，使用者放入 SD 卡的圖片需事先經過影像處理，調整為適合的解析度，並透過 Floyd-Steinberg 抖動演算法轉為適合電子紙的調色盤格式。專案中包含了 [`convert.py`](file:///home/felix/projects/PhotoPainter_B/convert.py) 這個 Python 腳本來自動化處理圖片。

**環境需求**:
執行此腳本需要安裝 Python 及 Pillow (PIL) 影像處理套件：
```bash
pip install Pillow
```

**腳本功能與使用方式**:
腳本會自動讀取圖片，根據參數進行縮放 (`scale`) 或裁切 (`cut`) 以符合電子紙的解析度 (例如橫向 800x480)，再執行色彩量化與抖動，最終輸出 `.bmp` 檔案供相框讀取。

**常用指令**:
- **基本轉換** (預設使用 scale 縮放模式及 Floyd-Steinberg 抖動):
  ```bash
  python convert.py input.jpg
  ```
- **指定顯示方向**:
  可以使用 `--dir` 指定 `landscape` (橫向 800x480) 或 `portrait` (直向 480x800)：
  ```bash
  python convert.py input.jpg --dir landscape
  ```
- **指定畫面處理模式**:
  可以使用 `--mode` 指定 `scale` (等比例縮放並留白) 或 `cut` (裁切填滿畫面)：
  ```bash
  python convert.py input.jpg --mode cut
  ```
- **關閉色彩抖動** (純色塊顯示):
  ```bash
  python convert.py input.jpg --dither 0
  ```
轉換完成後，腳本會自動在同目錄下生成帶有後綴的 `.bmp` 檔案 (例如 `input_scale_output.bmp`)，將其複製到 SD 卡中即可讓相框讀取顯示。
