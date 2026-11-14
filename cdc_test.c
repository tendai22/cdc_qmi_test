#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "pico/time.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"

// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart0
#define BAUD_RATE 115200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 0
#define UART_RX_PIN 1



static uint32_t time_ms_32(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static int cdc_itf = 0;

//
// core1 のメインループ
// この関数からリターンしない。
__attribute__((noinline)) void __time_critical_func(cdc_test_core1_entry)(void)
{
    //
    // core1 (bus read/write loop)
    //
    int count = 0;
    while (true)
    {
        printf("Hello, world! %4d\n", count++);
        sleep_ms(1000);
    }
}

__attribute__((noinline)) int __time_critical_func(main)(void)
{
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

    // TinyUSB CDC が立ち上がるまでの時間を稼ぐ
    sleep_ms(1000);

    // stdio 対照を UARTのみとする。
    stdio_filter_driver(&stdio_uart);

    printf("UART0 output\n");

    multicore_launch_core1(cdc_test_core1_entry);
    // usb CDC handling
    int c;
    uint8_t cb = 'A';
    uint32_t data;
    uint32_t cmd;
    uint32_t time0 = time_ms_32();
    while (1)
    {
        tud_task();
        //
        if (time_ms_32() - time0 < 1000) {
            tight_loop_contents();
            continue;
        }
        time0 = time_ms_32();
        while (tud_cdc_n_write_available(cdc_itf) <= 0) {
            tud_task();
            tight_loop_contents();
        }
        tud_cdc_n_write(cdc_itf, &cb, 1);
        tud_cdc_n_write_flush(cdc_itf);
#if 0
        if (rx_rdy == 0 && tud_cdc_n_available(cdc_itf) > 0) {
            tud_cdc_n_read(cdc_itf, &cb, 1);
            rx_data = cb;
            rx_rdy = 1;
        }
        if (tx_rdy == 0 && tud_cdc_n_write_available(cdc_itf) > 0) {
            tx_rdy = 2;
        }
        if (tx_rdy && txbuf_full) {
            cb = tx_data;
            tud_cdc_n_write(cdc_itf, &cb, 1);
            tud_cdc_n_write_flush(cdc_itf);     // flush seems to be needed
            txbuf_full = 0;
            tx_rdy = 0;
        }
#endif
    }
}
