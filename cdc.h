#pragma once
//
// cdc.h
//

//
// cdc putch interface
//

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
extern uint8_t getch(void);
extern void putch(uint8_t c);

void cdc_receive_poll(void);
void cdc_send_poll(void);
uint32_t time_ms_32(void);
