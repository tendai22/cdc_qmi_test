#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include <stdio.h>

#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"
#include "tusb.h"

// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart0
#define BAUD_RATE 115200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 0
#define UART_RX_PIN 1

//
// RAM emulator memory for Z80
//

uint8_t mem[65536];

//
// cdc putch interface
//
#define PUTCH_TRIGGER 0xe000
#define PUTCH_DATA 0xe001
#define PUTCH_TXREADY 0xe002

#define MEM_VOLATILE(addr) \
    (*(volatile uint8_t *)&mem[(addr)])

#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <stdint.h>

// 共有メモリ
extern uint8_t mem[65536];

// interface variables on mem[]
#define PUTCH_TRIGGER 0xe000
#define PUTCH_DATA 0xe001
#define PUTCH_TXREADY 0xe002

#define GETCH_ACK 0xe003
#define GETCH_DATA 0xe004
#define GETCH_RXREADY 0xe005

// 指定アドレスをvolatileアクセスする
#define MEM_VOLATILE(addr) \
    (*(volatile uint8_t *)&mem[(addr)])

// function declaration
static uint8_t getch(void);
static void putch(uint8_t c);

//
// bool kbhit(void) ... CDC データ受信可能かどうかを調べる。
//
static bool kbhit(void) {
    // 前回の受信完了通知が未処理なら、
    // 新しい受信データはまだ利用できない
    if (MEM_VOLATILE(GETCH_ACK) != 0) {
        return false;
    }

    // 受信データが存在するか確認
    return MEM_VOLATILE(GETCH_RXREADY) != 0;
}

static uint8_t getch(void) {
    // 前回の受信完了通知が処理されるまで待つ
    while (MEM_VOLATILE(GETCH_ACK) != 0) {
        tight_loop_contents();
    }

    __dmb();

    // 受信データが到着するまで待つ
    while (MEM_VOLATILE(GETCH_RXREADY) == 0) {
        tight_loop_contents();
    }

    // Core0が書き込んだDATAを読み出す
    __dmb();

    uint8_t c = MEM_VOLATILE(GETCH_DATA);

    // DATAの読み出しをACKの通知より先行させる
    __dmb();

    // Core0へ受信完了を通知
    MEM_VOLATILE(GETCH_ACK) = 1;

    return c;
}

//
// putch() - 同期式1文字出力
//
// Core1から呼び出す。
// Core0のcdc_send_poll()が送信要求を処理するまで待つ。
//
static void putch(uint8_t c) {
    // 以前の送信完了が残っている場合、Core0が送信要求を処理するまで待つ
    while (MEM_VOLATILE(PUTCH_TRIGGER) != 0) {
        tight_loop_contents();
    }
    __dmb();
    // CDC送信可能になるまで待つ
    while (MEM_VOLATILE(PUTCH_TXREADY) == 0) {
        tight_loop_contents();
    }

    // 送信データをセット
    MEM_VOLATILE(PUTCH_DATA) = c;

    // データの書き込みを先行させる
    __dmb();

    // Core0へ送信要求を通知
    MEM_VOLATILE(PUTCH_TRIGGER) = 1;

    // Core0による送信処理完了
    __dmb();
}

static uint32_t time_ms_32(void) {
    return to_ms_since_boot(get_absolute_time());
}

static int cdc_itf = 0;

//
// core1 のメインループ
// この関数からリターンしない。
__attribute__((noinline)) void __time_critical_func(cdc_test_core1_entry)(void) {
    //
    // core1 (bus read/write loop)
    //
    const char *message = "Hello from Core1!\r\n";
    const char alphabet[26] = "abcdefghijklmnopqrstuvwxyz";

    int count = 0;
    while (true) {
#if 0
        const char *p = message;

        putch(alphabet[count++ % 26]);
        putch(' ');
        while (*p) {
            putch((uint8_t)*p++);
        }

        // sleep_ms(1000);
#endif
        while (!kbhit()) {
            tight_loop_contents();
        }

        int c = getch();
        putch(c);
    }
}

//
// core0 and main function
//
//
// CDC send polling
//
// Z80がPUTCH_DATAにデータを書き込み、PUTCH_TRIGGER=1にすると、
// Core0がTinyUSB CDCの送信FIFOに1バイト書き込む。
//
// 送信FIFOへの書き込み成功後、
// PUTCH_TXREADY=0, PUTCH_TRIGGER=0 としてZ80へ通知する。
//
// PUTCH_TRIGGER=0の場合、CDC送信FIFOの空き状態を
// PUTCH_TXREADYに反映する。
//
// この関数は待ちループを持たず、必ず短時間で戻る。
//
static void cdc_send_poll(void) {
    uint8_t cb;

    // Z80からの送信要求がない場合
    if (MEM_VOLATILE(PUTCH_TRIGGER) == 0) {

        // CDC送信可能状態をZ80に通知
        MEM_VOLATILE(PUTCH_TXREADY) =
            (tud_cdc_n_ready(cdc_itf) &&
             tud_cdc_n_write_available(cdc_itf) > 0)
                ? 1
                : 0;

        return;
    }

    // Z80からの送信要求がある場合

    // CDCが送信可能になるまで次回のpollで再試行
    if (!tud_cdc_n_ready(cdc_itf) ||
        tud_cdc_n_write_available(cdc_itf) == 0) {
        return;
    }

    __dmb();
    // 送信データを取得
    cb = MEM_VOLATILE(PUTCH_DATA);

    // TinyUSB CDCの送信FIFOに1バイト書き込む
    if (tud_cdc_n_write(cdc_itf, &cb, 1) != 1) {
        return;
    }

    // 送信FIFOの内容をUSB転送へ回す
    tud_cdc_n_write_flush(cdc_itf);

    // Z80へ送信要求の処理完了を通知
    MEM_VOLATILE(PUTCH_TXREADY) = 0;
    __dmb();
    MEM_VOLATILE(PUTCH_TRIGGER) = 0;
}

static void cdc_receive_poll(void) {
    // Core1からの受信完了通知を処理
    if (MEM_VOLATILE(GETCH_ACK) != 0) {
        // RXREADYのクリアをACKのクリアより先行させる
        MEM_VOLATILE(GETCH_RXREADY) = 0;
        __dmb();
        MEM_VOLATILE(GETCH_ACK) = 0;
        return;
    }

    // 前回の受信データが未取得なら何もしない
    if (MEM_VOLATILE(GETCH_RXREADY) != 0) {
        return;
    }

    // TinyUSB CDCに受信データがなければ終了
    if (tud_cdc_n_available(cdc_itf) == 0) {
        return;
    }

    // TinyUSB CDCから1バイト読み出す
    uint8_t cb;

    if (tud_cdc_n_read(cdc_itf, &cb, 1) != 1) {
        return;
    }

    // 受信データを共有SRAMに格納
    MEM_VOLATILE(GETCH_DATA) = cb;

    // DATAの書き込みをRXREADYの通知より先行させる
    __dmb();

    // Core1へ受信データがあることを通知
    MEM_VOLATILE(GETCH_RXREADY) = 1;
}

__attribute__((noinline)) int __time_critical_func(main)(void) {
    stdio_init_all();

    // Set up our UART
    uart_init(UART_ID, BAUD_RATE);
    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uart_puts(UART_ID, "\r\n Hello, UART!\r\n");

    // For more examples of UART use see https://github.com/raspberrypi/pico-examples/tree/master/uart
    // UART0だけを使用

    // initialze interface variables
    MEM_VOLATILE(GETCH_ACK) = 0;
    MEM_VOLATILE(GETCH_DATA) = 0;
    MEM_VOLATILE(GETCH_RXREADY) = 0;
    // TinyUSB CDC が立ち上がるまでの時間を稼ぐ
    sleep_ms(1000);

    // stdio 対照を UARTのみとする。
    stdio_filter_driver(&stdio_uart);

    printf("UART0 output\n");

    multicore_launch_core1(cdc_test_core1_entry);
    // usb CDC handling
    int c;
    uint32_t time0 = time_ms_32();
    // infinite loop for CDC send/receive polling functions
    while (1) {
        tud_task();

        cdc_send_poll();
        cdc_receive_poll();
        tight_loop_contents();
    }
}
