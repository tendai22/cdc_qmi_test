# RP2350 `cdc_test`：TinyUSB CDC送信設計・検討記録

- **記録日**：2026年11月15日（日本時間）
- **用途**：開発再開時の引き継ぎ／2026年末に制作予定の技術同人誌の素材
- **対象**：Raspberry Pi Pico 2（RP2350）、Pico SDK 2.3.1、TinyUSB、Tera Term、Z80メモリエミュレータ
- **状態**：コア分担とUSBイベント処理方式の方針決定。CDC送信の単純動作を実機で確認済み。共有メモリを介したZ80送信処理は今後の実装対象。

## 1. 背景と目的

RP2350のPIO/DMAでZ80から見える64 KiBメモリ空間をエミュレートし、そのメモリ内に配置したレジスタを通じてZ80とUSB CDC（PC側はTera Term）を接続する。Z80側からは、メモリマップドI/Oとして1バイトずつ文字を送信できるようにする。

当面の`cdc_test`は、Z80実機やPIO/DMAバス処理を組み込む前に、RP2350の2コアとTinyUSB CDCの基本動作・責務分担を検証するためのテストプログラムである。

## 2. CDC送信のメモリマップとハンドシェイク

| アドレス | シンボル | 用途 |
|---|---|---|
| `0xE000` | `PUTCH_TRIGGER` | Z80が送信要求を発行（`1`）／RP2350が受理後にクリア（`0`） |
| `0xE001` | `PUTCH_DATA` | Z80が送信したい1バイトを格納 |
| `0xE002` | `PUTCH_TXREADY` | RP2350が次の送信を受け付けられることを通知 |

想定する送信シーケンス：

1. Z80は`TXREADY`で送信可能か調べる。`PUTCH_TRIGGER != 0`または`PUTCH_TXREADY == 0`なら送信不可（Zフラグ）。
2. 送信可能なら、Z80は`PUTCH_DATA`に文字を書き、続けて`PUTCH_TRIGGER = 1`を書き込む。
3. RP2350のCDC処理はトリガを検出し、CDC接続状態と送信FIFO空きを確認する。
4. 1バイトをTinyUSBの送信FIFOへ受け付けさせたら、`PUTCH_TXREADY = 0`、`PUTCH_TRIGGER = 0`として要求を完了する。
5. トリガが立っていない間は、CDCの送信余力に応じて`PUTCH_TXREADY`を更新する。

**重要な設計判断**：送信完了とは「TinyUSBの送信FIFOへの格納成功」を意味する。PCのTera Term画面への表示やホスト側受信完了の確認までは要求しない。

Z80の`PUTCH`ルーチンでは、直前の`TXREADY`確認後にCDCの空き状況が変化し得るため、二重にTXREADYを検査して文字を落とす構造は避ける。`PUTCH`側では未処理トリガの有無を確認し、DATA→TRIGGERの順で書き込む。Z80側は単一実行コンテキストで、割り込みから同じ送信処理を再入させない前提。

## 3. 実験の経過

### 3.1 最初のコア割り当て

- **Core0**：UART0の`printf()`（約1秒ごとの`Hello, world!`）。
- **Core1**：`tud_task()`とTinyUSB CDCへの`'A'`の1バイト送信（約1秒ごと）。

実機で、UART0接続のTera TermとUSB CDC接続のTera Termに、それぞれ想定どおりの文字が出力されることを確認した。これは基本的な2コア同時動作の確認であり、高負荷時の安全性やTinyUSB内部の排他性まで保証するものではない。

当初のコードには、`tud_cdc_n_write_available()`の空きを待つループ中に`tud_task()`を実行しないと、USBイベント処理が進まず待ち続ける可能性がある、という指摘があった。その後、待機ループ内でも`tud_task()`を呼ぶように変更した。

### 3.2 コア割り当てを変更

将来のZ80バスアクセスを優先して、次の構成を採用する方針にした。

| 担当 | Core0 | Core1 |
|---|---|---|
| 主処理 | TinyUSB CDC、UART0デバッグ、共有メモリのCDC送受信処理 | Z80バスアクセス、PIO/DMAの設定・制御 |
| テスト段階 | CDCへ`'A'`を定期送信 | UART0へ`Hello, world!`を定期出力 |

Core0で`stdio_init_all()`を実行し、TinyUSB初期化とUSBイベント処理を同じコアに集約する。Core1からはTinyUSB APIを呼ばない設計とする。`stdio_filter_driver(&stdio_uart)`によって`printf()`の出力先をUART0に限定する。ただし、これはUSB処理を停止する設定ではない。

PIO/DMAはCPUと独立して動作する部分があり、Core1が必ず常時ポーリングするとは限らない。両コアはSRAMやバスなどを共有するため、時間的な完全独立が保証されるわけでもない。

## 4. TinyUSBのバックグラウンド処理を無効化する検討

### 4.1 問題意識

`pico_enable_stdio_usb(cdc_test 1)`を使用すると、SDKの`pico_stdio_usb`がTinyUSB初期化やバックグラウンドでの`tud_task()`実行に関与する場合がある。この状態でアプリケーションのCore0メインループからも`tud_task()`を呼ぶと、処理の二重実行・競合が懸念される。

そこで、**USBハードウェア割り込みそのものではなく、`pico_stdio_usb`によるバックグラウンドの`tud_task()`呼び出しを無効化**し、Core0のメインループから明示的に`tud_task()`を呼ぶ方針とした。

### 4.2 最終的に採用したCMake記述

```cmake
# pico_stdio_usbのバックグラウンドtud_task()を無効化する。
# USBハードウェア割り込み自体は無効化しない。
set(PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK 0 CACHE BOOL
    "Disable stdio USB background task")

pico_sdk_init()

add_executable(cdc_test cdc_test.c)

# アプリケーションおよびターゲットに組み込まれるSDKソースへ定義を渡す。
target_compile_definitions(cdc_test PRIVATE
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
)

pico_enable_stdio_uart(cdc_test 1)
pico_enable_stdio_usb(cdc_test 1)
```

`set(... CACHE BOOL ...)`はCMake変数の設定であり、それ自体がCコンパイラの`-D`を発行するわけではない。`target_compile_definitions()`を併記してプリプロセッサマクロを明示した。

### 4.3 ビルドログで確認できたこと

提供された`compile_commands.json`抜粋では、Pico SDK 2.3.1の以下のソースに対するコンパイルコマンドに、**`-DPICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0`**が含まれていた。

- `src/rp2_common/pico_stdio_usb/stdio_usb.c`
- `src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c`

よって、少なくとも**該当ソースのコンパイル時にマクロ定義が渡されていること**は確認済み。実行時の全経路や初期化設定の完全な検証を行った、という意味ではない。

なお、`PICO_STDIO_USB_ENABLE_TINYUSB_INIT`は別の設定であり、バックグラウンド処理の無効化とTinyUSBの初期化責務は分けて考える。

## 5. 現在のCore0 CDC送信テストの要点

Core0では`stdio_init_all()`、UART0の初期化、`stdio_filter_driver(&stdio_uart)`、Core1起動を行った後、USB CDC送信ループに入る。

```c
uint8_t cb = 'A';
uint32_t time0 = time_ms_32();

while (true) {
    tud_task();

    if ((uint32_t)(time_ms_32() - time0) < 1000) {
        tight_loop_contents();
        continue;
    }
    time0 = time_ms_32();

    if (tud_cdc_n_write_available(cdc_itf) > 0) {
        if (tud_cdc_n_write(cdc_itf, &cb, 1) == 1) {
            tud_cdc_n_write_flush(cdc_itf);
        }
    }
}
```

これは議論中に提示された**非ブロッキング版の参考コード**であり、ユーザーが最後に提示した実機コードそのものではない。最後に提示されたコードは、送信空き待ちループ内でも`tud_task()`を呼ぶ方式だった。`tud_cdc_n_write_flush()`は、実験時に送信の進行に必要と観察されたため使用している。

`time_ms_32()`はPico SDKの`to_ms_since_boot(get_absolute_time())`を使って自作したヘルパーである。

## 6. ここまでの結論と未完了事項

**決定した方針**：TinyUSBのイベント処理はCore0に集約し、`pico_stdio_usb`のバックグラウンドタスクを無効化する。Z80バス処理はCore1へ配置する。USB CDCへの1バイト送信は、TinyUSB送信FIFOに受け付けられた時点でZ80側要求を完了とみなす。

**確認済み**：旧構成でCore0のUART出力とCore1のCDC出力が実機で並行動作したこと。`compile_commands.json`上で、`stdio_usb.c`にバックグラウンドタスク無効化マクロが渡されていること。

**今後の検証・実装**：

1. 新しいCore0=CDC／Core1=バス担当の配置で実機動作を確認する。
2. Core0での`tud_task()`の単一実行、TinyUSB初期化の一元化を確認する。
3. `PUTCH_DATA`→`PUTCH_TRIGGER`の共有メモリハンドシェイクをCDC送信ループへ接続する。
4. CDC切断・送信FIFO満杯・連続送信時の挙動をテストする。
5. PIO/DMAによるZ80メモリ書き込みとCore0からの観測について、順序性・可視性を別途レビューする。
6. UARTデバッグとUSB処理が共存した場合の負荷・遅延を測定する。

## 7. 技術同人誌向けの題材メモ

- **「2コアを入れ替えただけでは終わらない」**：TinyUSBの初期化コア、IRQ、`tud_task()`の実行主体を整理する過程。
- **「`stdio_filter_driver()`はUSBを止めない」**：標準出力のルーティングとUSBスタックのイベント処理は別物。
- **「CMake変数とコンパイラの`-D`は違う」**：`compile_commands.json`を根拠にSDK内部への設定反映を確認する方法。
- **「1バイト送信でもプロトコル設計が必要」**：DATA→TRIGGER、TXREADY、受理完了の定義、FIFO満杯への対処。
- **「実機で動くことと競合がないことは別」**：低負荷の成功から、排他性・割り込み・高負荷検証へ進む手順。

---

### 編集時の注意

本稿は**2026年11月15日時点の設計議論の記録**であり、SDK内部の全実装を独立検証した技術仕様書ではない。技術同人誌に掲載する際は、使用したPico SDK 2.3.1の`stdio_usb.c`／`stdio_usb.h`の該当箇所、CMakeビルドログ、実機試験結果を再確認し、「検討した方針」と「実機で確認した事実」を区別して記述すること。
