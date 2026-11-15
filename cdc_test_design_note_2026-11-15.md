# cdc_test：RP2350 Core間共有SRAMによるUSB CDC通信

**設計結果・議論の経緯・実機確認記録**  
記録日：2026-11-15  
対象：Raspberry Pi Pico 2（RP2350）、Pico SDK 2.3.1、TinyUSB、Tera Term  
リポジトリ：<https://github.com/tendai22/cdc_test>

> 本文は本スレッドでの議論・提示コード・報告された試験結果を整理したもの。最終ソースコードの完全な複製ではない。コード断片は設計を説明するための代表例であり、ビルド時には実際のリポジトリを参照すること。

## 1. 目的と位置づけ

RP2350の2つのCPUコアを分担させ、将来のZ80メモリエミュレータがUSB CDC経由でPC上のTera Termと文字入出力できることを検証する。Core0はTinyUSB CDCスタックを担当し、Core1は将来のZ80バスアクセス処理を担当する。本試作ではCore1上のC関数 `putch()`、`getch()`、`kbhit()` でZ80側の操作を模擬する。

最終的には、別途開発中の `qmi_floppy` と統合する予定。先に `qmi_floppy` のstart/poll化、変数インタフェース化、Core0/Core1分割動作確認を行う。

## 2. システム構成

```text
Tera Term (COM4)
      ⇅ USB CDC
TinyUSB / Core0
  tud_task()
  cdc_send_poll()
  cdc_receive_poll()
      ⇅
共有SRAM mem[65536]
      ⇅
Core1
  putch(), getch(), kbhit()
  （将来：PIO/DMAを介したZ80バス処理）
```

- `uint8_t mem[65536]` をZ80の64 KiBメモリ空間として用いる。
- Core0とCore1は同じオンチップSRAMを参照する。
- USB CDCのTinyUSB処理はCore0に集約する。
- UART0（GPIO0/1、115200 bps）のデバッグ出力とUSB CDCは独立した経路とする。
- バイナリは `pico_set_binary_type(cdc_test copy_to_ram)` を使用。

### TinyUSB処理方式の検討

`stdio_init_all()` による初期化を利用しながら、USB stdioのバックグラウンドタスク実行を抑止し、Core0メインループから明示的に `tud_task()` を呼ぶ方式を採用した。

```cmake
set(PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK 0 CACHE BOOL
    "Disable stdio USB background task")
pico_sdk_init()

add_executable(cdc_test cdc_test.c)
target_compile_definitions(cdc_test PRIVATE
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0)
```

`compile_commands.json` において `pico_stdio_usb/stdio_usb.c` と `stdio_usb_descriptors.c` のコンパイル引数に `-DPICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0` が含まれることを確認した。これはUSBハードウェア割り込み全体の禁止を意味しない。

Core0の基本ループ：

```c
while (true) {
    tud_task();
    cdc_send_poll();
    cdc_receive_poll();
    tight_loop_contents();
}
```

## 3. 共有メモリのレジスタ配置

```c
#define PUTCH_TRIGGER  0xe000
#define PUTCH_DATA     0xe001
#define PUTCH_TXREADY  0xe002
#define GETCH_ACK      0xe003
#define GETCH_DATA     0xe004
#define GETCH_RXREADY  0xe005

#define MEM_VOLATILE(addr) (*(volatile uint8_t *)&mem[(addr)])
```

| アドレス | 名称 | 主な書き込み側 | 意味 |
|---|---|---|---|
| E000h | `PUTCH_TRIGGER` | Core1が1、Core0が0 | 送信要求／処理完了 |
| E001h | `PUTCH_DATA` | Core1 | 送信データ1バイト |
| E002h | `PUTCH_TXREADY` | Core0 | CDC送信受付可能 |
| E003h | `GETCH_ACK` | Core1が1、Core0が0 | 受信データ取得完了 |
| E004h | `GETCH_DATA` | Core0 | 受信データ1バイト |
| E005h | `GETCH_RXREADY` | Core0 | 未読受信データあり |

`mem`全体を `volatile` にせず、通信レジスタとして使用する箇所のみvolatile修飾付きlvalueでアクセスする方針とした。初期化はCore1起動前にCore0で行う。

## 4. メモリ可視性とバリアの議論

当初、Core1が書いた値がCPUのD-cacheに残りCore0から見えない可能性が懸念された。RP2350の通常の内蔵SRAMアクセスにはCPU D-cacheが介在しないこと、RP2350のXIPキャッシュとは用途が異なることを整理した。

- `volatile`：共有フラグを毎回読み書きするようコンパイラへ指示する。単独ではコア間の完全な同期機構ではない。
- `__dmb()`：共有データと通知フラグのメモリアクセス順序を制御する。キャッシュフラッシュではない。
- 本試作では単一の送信者／受信者、割り込み等からの競合アクセスなし、通常の共有SRAMを前提とする。

参考：RP2350公式データシート <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf> のCortex-M33、SRAM、XIP cache、system busに関する節。ページ番号は版によって変わり得る。

## 5. 送信：Core1 → Core0 → USB CDC

### 5.1 ハンドシェイク

1. Core0は送信要求がないとき、CDCが送信可能かを確認して `PUTCH_TXREADY` を設定する。
2. Core1は前回の `PUTCH_TRIGGER` が0になるのを待ち、次に `PUTCH_TXREADY` が1になるのを待つ。
3. Core1は `PUTCH_DATA` を書き、`__dmb()` の後 `PUTCH_TRIGGER=1` にする。
4. Core0は `PUTCH_TRIGGER` を検出し、`__dmb()` の後データを読む。
5. `tud_cdc_n_write()` で1バイトをTinyUSBの送信FIFOへ格納し、`tud_cdc_n_write_flush()` を呼ぶ。
6. 成功後、Core0は `PUTCH_TXREADY=0`、`__dmb()`、`PUTCH_TRIGGER=0` の順で完了通知する。
7. FIFOへ格納できなければCore0は要求を残し、次回pollで再試行する。

### 5.2 Core1の確定した `putch()`

最初は今回の送信完了を待つ同期式だったが、議論の結果、**前回の要求の完了を次回呼び出しの冒頭で待つ**方式に変更した。今回の要求を発行した時点で復帰し、パイプライン的に動作する。

```c
static void putch(uint8_t c)
{
    while (MEM_VOLATILE(PUTCH_TRIGGER) != 0) {
        tight_loop_contents();
    }
    __dmb();

    while (MEM_VOLATILE(PUTCH_TXREADY) == 0) {
        tight_loop_contents();
    }

    MEM_VOLATILE(PUTCH_DATA) = c;
    __dmb();
    MEM_VOLATILE(PUTCH_TRIGGER) = 1;
}
```

復帰時点ではTinyUSB送信FIFOへの格納もPC到達も保証しない。次の `putch()` 呼び出しで前回要求の処理完了を待つため、同一実行コンテキストでの連続送信時に未処理データを上書きしない。CDC接続・ホスト受信が継続するという正常動作の前提を置く。

### 5.3 Core0の送信処理（要点）

```c
static void cdc_send_poll(void)
{
    if (MEM_VOLATILE(PUTCH_TRIGGER) == 0) {
        MEM_VOLATILE(PUTCH_TXREADY) =
            tud_cdc_n_ready(cdc_itf) &&
            tud_cdc_n_write_available(cdc_itf) > 0;
        return;
    }

    if (!tud_cdc_n_ready(cdc_itf) ||
        tud_cdc_n_write_available(cdc_itf) == 0) {
        return;
    }

    __dmb();
    uint8_t cb = MEM_VOLATILE(PUTCH_DATA);
    if (tud_cdc_n_write(cdc_itf, &cb, 1) != 1) {
        return;
    }
    tud_cdc_n_write_flush(cdc_itf);

    MEM_VOLATILE(PUTCH_TXREADY) = 0;
    __dmb();
    MEM_VOLATILE(PUTCH_TRIGGER) = 0;
}
```

## 6. 受信：USB CDC → Core0 → Core1

### 6.1 ハンドシェイク

1. Core0は `GETCH_ACK=1` を見たら `GETCH_RXREADY=0`、`__dmb()`、`GETCH_ACK=0` とし、そのpollを終了する。
2. `GETCH_RXREADY=1` の間は次のデータで `GETCH_DATA` を上書きしない。
3. 未読データがなく `tud_cdc_n_available(cdc_itf)>0` なら、`tud_cdc_n_read(cdc_itf, &cb, 1)` で1バイト読む。
4. `GETCH_DATA=cb`、`__dmb()`、`GETCH_RXREADY=1` の順で公開する。
5. Core1は `GETCH_ACK=0`、`GETCH_RXREADY=1` を待ってデータを読み、`GETCH_ACK=1` として復帰する。

TinyUSB受信APIは `tud_cdc_n_receive()` ではなく `tud_cdc_n_available()` / `tud_cdc_n_read()` を用いる。

### 6.2 Core0 `cdc_receive_poll()`

```c
static void cdc_receive_poll(void)
{
    if (MEM_VOLATILE(GETCH_ACK) != 0) {
        MEM_VOLATILE(GETCH_RXREADY) = 0;
        __dmb();
        MEM_VOLATILE(GETCH_ACK) = 0;
        return;
    }
    if (MEM_VOLATILE(GETCH_RXREADY) != 0) {
        return;
    }
    if (tud_cdc_n_available(cdc_itf) == 0) {
        return;
    }

    uint8_t cb;
    if (tud_cdc_n_read(cdc_itf, &cb, 1) != 1) {
        return;
    }
    MEM_VOLATILE(GETCH_DATA) = cb;
    __dmb();
    MEM_VOLATILE(GETCH_RXREADY) = 1;
}
```

### 6.3 Core1 `getch()`

```c
static uint8_t getch(void)
{
    while (MEM_VOLATILE(GETCH_ACK) != 0) {
        tight_loop_contents();
    }
    __dmb();

    while (MEM_VOLATILE(GETCH_RXREADY) == 0) {
        tight_loop_contents();
    }
    __dmb();

    uint8_t c = MEM_VOLATILE(GETCH_DATA);
    __dmb();
    MEM_VOLATILE(GETCH_ACK) = 1;
    return c;
}
```

`getch()`はデータ到着まで待つ。戻った時点ではCore0のACK処理完了を待たず、次回の `getch()` 冒頭で前回ACKのクリアを待つ。

### 6.4 Core1 `kbhit()`

```c
static bool kbhit(void)
{
    if (MEM_VOLATILE(GETCH_ACK) != 0) {
        return false;
    }
    return MEM_VOLATILE(GETCH_RXREADY) != 0;
}
```

未読受信データの有無のみを返し、データを消費しない。Core1側で受信データを消費する実行コンテキストは1つとする。

## 7. 試験の経緯と結果

### 7.1 送信の初期確認

Core1が `"Hello from Core1!\r\n"` をアルファベット接頭辞付きで約1秒ごとに `putch()` 送信した。COM4のTera Termに期待どおり表示された。UART0側のUSBシリアル変換器を抜いてもCOM4への出力が続き、USB CDC経路の独立性を確認した。

### 7.2 連続送信

`sleep_ms(1000)` をコメントアウトして高速連続送信した。Tera Termで表示を何度か停止して目視確認した範囲では、行長や行頭のアルファベット順序に乱れが見られなかった。これはハンドシェイクが機能している有力な実機結果。ただし受信ログの機械的な全件照合や長時間ストレス試験は未実施。

### 7.3 受信エコーバック

Core1で `uint8_t c = getch(); putch(c);` を繰り返すUSB CDCエコーバックが動作した。Tera TermのローカルエコーはOFFにする。

### 7.4 `kbhit()`確認

```c
while (true) {
    while (!kbhit()) {
        tight_loop_contents();
    }
    int c = getch();
    putch(c);
}
```

このループでもエコーバックが動作した。`kbhit()`がデータを消費せず、後続の `getch()` が同じ文字を取得できることを確認した。

### 7.5 Git管理

`cdc_test` リポジトリをGitHubに作成し、試作コードをコミット済み。`.vscode/` は `.gitignore` で除外。GitHub側に既存のLICENSEがあり履歴が独立していたため、初回の統合には `git pull origin main --allow-unrelated-histories --no-rebase` を使用した。

## 8. 今回確定した設計方針

- Core0のみがTinyUSBのpollと送受信APIを実行する。
- Core0の `cdc_send_poll()` / `cdc_receive_poll()` に待機ループを置かない。
- Core1の `putch()` は前回送信完了待ち＋今回要求発行方式。
- Core1の `getch()` は受信待ちする。`kbhit()` は待たずに状態だけ返す。
- 共有メモリ上のデータとフラグの順序は `volatile` アクセスと `__dmb()` で明示する。
- 単一のCore1側アクセス主体を前提とし、現段階ではリングバッファやロックを追加しない。
- TinyUSB FIFOに入ったこととPCで受信済みであることは区別する。

## 9. 残る確認事項・統合時の注意

- 大量送信時の完全性は目視で確認した範囲に限る。必要ならPC受信ログをプログラムで検証する。
- USB切断、再接続、ホスト停止、受信FIFO飽和などの異常系は今回の試験範囲外。
- Core1からの呼び出しを複数スレッド／IRQに拡張する場合は排他制御を再設計する。
- 将来のZ80バス経由PIO/DMAアクセスでは、今回のCPUロード／ストアと同じ可視性・順序が得られるか別途検証する。
- `qmi_floppy` と統合するときは、Core0のUSBサービスを長時間ブロックしない設計を維持する。

## 10. 次の開発予定

1. `qmi_floppy_read/write` のstart/poll化。
2. `qmi_floppy` の変数インタフェース化。
3. `qmi_floppy` のCore0/Core1分割による動作確認。
4. 本試作のUSB CDC通信機能と `qmi_floppy` を融合し、Z80メモリエミュレータを仕上げる。

---

**総括**：Core1から共有SRAMの6バイトの通信レジスタを通じてCore0のTinyUSB CDCを利用する送信・受信・受信状態確認の基本動作を実機で確認した。通信処理の責務分離とハンドシェイクの基本仕様が定まり、後続の `qmi_floppy` 統合に向けた独立した試作成果となった。
