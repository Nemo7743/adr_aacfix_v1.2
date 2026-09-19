# adr_aacfix 1.2

修正 Adrenaline 上「有影片的 PSP 自製遊戲」閃退（錯誤碼 C2-12828-1）

## 這是什麼

有些 PSP 自製遊戲（例如用 AVG MAKER PORTABLE 引擎做的「初音」系列 AVG）開場有 logo 影片或片頭動畫，影片的音軌是 AAC。在 PS Vita 的 Adrenaline 上，這類遊戲會在進入標題畫面之前**閃退**，Vita 顯示錯誤碼 **C2-12828-1**，而且之後要重開機才能再啟動任何遊戲。

這個外掛修正了 Vita 官方 PSP 模擬器（ScePspemu）處理 AAC 音訊時的一個錯誤，讓影片能**正常出畫面、出聲音**，不需要修改遊戲檔案或 ISO。

- 只影響 Adrenaline 裡執行的 PSP 遊戲。**不影響** Vita 原生遊戲、LiveArea、VitaShell。
- 其他 PSP 遊戲預期不受影響：外掛只改變「位址轉換本來會失敗、且位址落在 ME 專用記憶體範圍」的情況，並攔截 AAC 專用的處理函式。測試版（1.1）已用 Outrun Coast 2 Coast 確認正常；正式版（1.2）只是移除了多餘的記錄項目，尚未有實機回歸紀錄。

## 需求

| 項目 | 需求 |
|---|---|
| Vita 韌體 | **3.65 – 3.70**（3.65、3.67、3.68、3.69、3.70）<br>只在 **3.65** 實際測試過；3.71–3.74 目前**不支援**（外掛會自動停用，不會造成傷害） |
| 破解環境 | 已安裝 taiHEN（h-encore／HENkaku） |
| Adrenaline | 6.61 Adrenaline（v7 或 v8），測試版本為 v8.0.1 |
| 需要修改的檔案 | 只有 `ur0:tai/config.txt` |

## 安裝步驟

1. 用 VitaShell（或 FTP）把 **`adr_aacfix.suprx`** 複製到 Vita 的 **`ur0:tai/`** 資料夾。

2. 用 VitaShell 開啟 **`ur0:tai/config.txt`**，在**檔案最後面**另起一行加入下面兩行（區段名稱必須完全一樣，不要有多餘空格）：

   ```
   *PSPEMUCFW
   ur0:tai/adr_aacfix.suprx
   ```

   範例（你原本的內容不用動，只要多這兩行）：

   ```
   *KERNEL
   ur0:tai/gamesd.skprx
   ux0:app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx
   *PSPEMUCFW
   ur0:tai/adr_aacfix.suprx
   *ALL
   ```

   > [!IMPORTANT]
   > `*PSPEMUCFW` 是 Adrenaline 的程式代號。**不要**把這個外掛放在 `*KERNEL` 或 `*ALL` 裡。

3. 儲存後，**重新載入 taiHEN 設定**：設定 → HENkaku 設定 → 重新載入 taiHEN config.txt。（若你是用 h-encore 且沒安裝 Ensō，重新執行一次 h-encore 也可以。）

4. 啟動 Adrenaline，開啟原本會閃退的遊戲。

## 怎麼確認有生效

遊戲的開場影片會**有畫面、有聲音**，並且不再閃退。

也可以看記錄檔：用 VitaShell 開啟 **`ux0:data/adr_aacfix.log`**，應該有一行：

```
adr_aacfix 1.2 active: ScePspemu text=…, shadow=on
```

| 記錄內容 | 意義 |
|---|---|
| `active … shadow=on` | 完全正常 |
| `shadow=OFF(guard only)` | 外掛仍會避免閃退，但影片的聲音會被略過（沒有畫面卡死） |
| `… not supported … disabled` | 你的韌體不是 3.65–3.70，外掛沒有動作 |
| 沒有這個檔案 | 外掛沒有載入，請檢查 `config.txt` 的區段名稱，並確認已重新載入設定 |
| 出現 `GUARD …` 行 | 外掛攔截了一個不正常的請求以避免閃退（代表遭遇了沒預期的情況），歡迎回報 |

## 解除安裝

用 VitaShell 開啟 `ur0:tai/config.txt`，把 `*PSPEMUCFW` 與其下面那一行刪掉（或前面加 `#` 註解），再重新載入 taiHEN 設定。也可以直接刪除 `ur0:tai/adr_aacfix.suprx`。

> [!TIP]
> 如果安裝後 Adrenaline 無法啟動：外掛只會被載入到 Adrenaline，你可以先用 VitaShell 照上面方式移除，其他功能不受影響。

## 常見問題

<details>
<summary><b>以前測試版（adr_diag）的頭兩秒聲音會破音，這一版呢？</b></summary>

那是測試版每個音訊 frame 都寫記錄檔，拖慢解碼造成的。這一版正常情況下**不寫逐筆記錄**，所以沒有這個問題。

此結論由測試版的時間戳推得：記錄期間每個 frame 96–106 ms，不記錄時恰為 23.2 ms 的即時速率；正式版尚未有實機回報。

</details>

<details>
<summary><b>我之前裝過 <code>adr_diag.suprx</code>（測試版）。</b></summary>

請先從 `config.txt` 移除它的那一行並刪除檔案，只保留 `adr_aacfix.suprx`。兩個一起載入會重複攔截同一組位置。

</details>

<details>
<summary><b>仍然閃退怎麼辦？</b></summary>

請保留 `ux0:data/adr_aacfix.log`，以及 `ux0:data/` 裡的 `psp2core-PSPEMUCFW.…` 傾印檔（如果有）；並告訴我遊戲名稱、韌體版本、Adrenaline 版本。

若需要更詳細的資訊，在 `ux0:data/` 建立一個空檔案 **`adr_aacfix_debug`**，重新啟動 Adrenaline，log 會多記錄前幾筆 AAC 請求。

</details>

<details>
<summary><b>3.71–3.74 可以用嗎？</b></summary>

目前不行。要支援其他韌體，需要那個版本的 `ScePspemu`（`vs0:app/NPXS10028/eboot.bin`）解密後的檔案，才能重新計算 3 個 hook 位置。

</details>

<details>
<summary><b>遊戲無法存檔／讀檔是這個外掛的問題嗎？</b></summary>

不是。已在 PPSSPP 對照過，同一款遊戲在 PPSSPP 上也一樣。這是遊戲本身的行為，與 Vita 或 Adrenaline 無關。

</details>

## 檔案

| 檔案 | 說明 |
|---|---|
| `adr_aacfix.suprx` | 外掛本體（7,280 bytes） |
| [`src/`](src) | 原始碼（vitasdk 編譯） |
| [`技術說明.md`](技術說明.md) | 原因分析與實作細節 |

SHA-256：`89a3c16788121e4650bf30267cfbb58ba5803c2e5608387eed8a36d1b48f0d2b`
