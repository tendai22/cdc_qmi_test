#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
//#include <stddef.h>


//#include "pico/stdlib.h"
//#include "hardware/clocks.h"
//#include "hardware/uart.h"
//#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
//#include "pico/stdio_usb.h"
//#include "pico/time.h"
#include "tusb.h"

#include "cdc.h"
#include "conf.h"
#include "cmd.h"
#include "qmi_flash.h"
#include "memprt.h"

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
// core1 のメインループ
// この関数からリターンしない。
__attribute__((noinline)) void __time_critical_func(cdc_test_core1_entry)(void) {
    //
    // core1 (bus read/write loop)
    //
    const char *message = "Hello from Core1!\r\n";
    const char alphabet[26] = "abcdefghijklmnopqrstuvwxyz";

    int count = 0;
    qmi_init();
    command_loop();

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
#if 0
        while (!kbhit()) {
            tight_loop_contents();
        }

        int c = getch();
        putch(c);
#endif
#if 0
        int c;
        while ((c = getchar()) != EOF) {
            printf("%c", c);
        }
#endif
    }
}

//
// core0 and main function
//
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
