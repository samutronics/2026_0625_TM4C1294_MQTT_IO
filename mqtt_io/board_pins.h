//*****************************************************************************
//
// board_pins.h - TM4C1294 <-> control board (EV300E) pin map for the field I/O.
//
// Every assignment below was traced from the EV300E connector in
// ControlBoardSch.pdf; see HARDWARE.md section 2.5 for the full trace.  The
// two daisy chains use separate hardware SSI modules:
//
//   Input chain  (SN65HVS882 x2 per board, 16 inputs)  -> SSI0, read-only
//   Output chain (DRV8860     x2 per board, 16 relays) -> SSI1, full-duplex
//
// LATCH / CE / ENABLE / nFAULT are plain GPIO because their timing does not
// match SSI FSS auto-pulsing.  RS-485 (UART0) is deferred - see the block at
// the end of this file.
//
//*****************************************************************************

#ifndef __BOARD_PINS_H__
#define __BOARD_PINS_H__

#include "inc/hw_memmap.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"

//*****************************************************************************
//
// Per-board channel counts (two 8-channel devices per board).
//
//*****************************************************************************
#define INPUTS_PER_BOARD        16      // 2x SN65HVS882
#define RELAYS_PER_BOARD        16      // 2x DRV8860

//*****************************************************************************
//
// Input chain - SN65HVS882 serializers, clocked out over SSI0 (RX only).
// PA4 (SSI0XDAT0/MOSI) is intentionally unused: the chain is read-only.
//
//   Input_SPI_clk   PA2  SSI0CLK       -> CLK
//   Input_SPI_CS    PA3  SSI0FSS       -> CE  (clock enable, active-low)
//   Input_SPI_MISO  PA5  SSI0XDAT1(RX) <- SOP (serial data from the chain)
//   Input_Latch     PH0  GPIO          -> LD  (load/latch, active-low)
//
//*****************************************************************************
#define INPUT_SSI_BASE          SSI0_BASE
#define INPUT_SSI_PERIPH        SYSCTL_PERIPH_SSI0

#define INPUT_SSI_CLK_PORT      GPIO_PORTA_BASE
#define INPUT_SSI_CLK_PIN       GPIO_PIN_2
#define INPUT_SSI_CLK_CFG       GPIO_PA2_SSI0CLK

#define INPUT_SSI_FSS_PORT      GPIO_PORTA_BASE     // used as CE
#define INPUT_SSI_FSS_PIN       GPIO_PIN_3
#define INPUT_SSI_FSS_CFG       GPIO_PA3_SSI0FSS

#define INPUT_SSI_RX_PORT       GPIO_PORTA_BASE     // SOP / MISO
#define INPUT_SSI_RX_PIN        GPIO_PIN_5
#define INPUT_SSI_RX_CFG        GPIO_PA5_SSI0XDAT1

#define INPUT_SSI_GPIO_PERIPH   SYSCTL_PERIPH_GPIOA

#define INPUT_LATCH_PORT        GPIO_PORTH_BASE     // LD (active-low)
#define INPUT_LATCH_PIN         GPIO_PIN_0
#define INPUT_LATCH_PERIPH      SYSCTL_PERIPH_GPIOH

//*****************************************************************************
//
// Output chain - DRV8860 relay drivers, written over SSI1 (full-duplex so the
// fault/echo bits shift back in on MISO while relay data shifts out on MOSI).
// LATCH is a GPIO pulse issued once per full 16*N-bit frame.
//
//   OUTPUT_SPI_CLK   PB5  SSI1CLK        -> CLK
//   OUTPUT_SPI_MOSI  PE4  SSI1XDAT0(TX)  -> DIN   (relay data)
//   OUTPUT_SPI_MISO  PE5  SSI1XDAT1(RX)  <- DOUT  (fault/echo readback)
//   OUTPUT_SPI_CS    PB4  GPIO           -> LATCH (pulse after full frame)
//   REL_EN           PH2  GPIO           -> ENABLE
//   Relay_Fault      PH1  GPIO in (pull-up) <- nFAULT (open-drain, wired-OR)
//
//*****************************************************************************
#define OUTPUT_SSI_BASE         SSI1_BASE
#define OUTPUT_SSI_PERIPH       SYSCTL_PERIPH_SSI1

#define OUTPUT_SSI_CLK_PORT     GPIO_PORTB_BASE
#define OUTPUT_SSI_CLK_PIN      GPIO_PIN_5
#define OUTPUT_SSI_CLK_CFG      GPIO_PB5_SSI1CLK
#define OUTPUT_SSI_CLK_PERIPH   SYSCTL_PERIPH_GPIOB

#define OUTPUT_SSI_TX_PORT      GPIO_PORTE_BASE     // DIN
#define OUTPUT_SSI_TX_PIN       GPIO_PIN_4
#define OUTPUT_SSI_TX_CFG       GPIO_PE4_SSI1XDAT0

#define OUTPUT_SSI_RX_PORT      GPIO_PORTE_BASE     // DOUT
#define OUTPUT_SSI_RX_PIN       GPIO_PIN_5
#define OUTPUT_SSI_RX_CFG       GPIO_PE5_SSI1XDAT1
#define OUTPUT_SSI_E_PERIPH     SYSCTL_PERIPH_GPIOE

#define OUTPUT_LATCH_PORT       GPIO_PORTB_BASE     // driven as GPIO
#define OUTPUT_LATCH_PIN        GPIO_PIN_4
#define OUTPUT_LATCH_PERIPH     SYSCTL_PERIPH_GPIOB

#define OUTPUT_ENABLE_PORT      GPIO_PORTH_BASE
#define OUTPUT_ENABLE_PIN       GPIO_PIN_2
#define OUTPUT_ENABLE_PERIPH    SYSCTL_PERIPH_GPIOH

#define OUTPUT_NFAULT_PORT      GPIO_PORTH_BASE     // input, needs pull-up
#define OUTPUT_NFAULT_PIN       GPIO_PIN_1
#define OUTPUT_NFAULT_PERIPH    SYSCTL_PERIPH_GPIOH

//*****************************************************************************
//
// RS-485 - DEFERRED (see HARDWARE.md section 2.5).  On UART0 (PA0/PA1), which
// currently carries the ICDI backchannel console; enabling RS-485 means moving
// the console off UART0 first.  Kept here for reference only.
//
//   RS485_Rx  PA0  U0RX
//   RS485_Tx  PA1  U0TX
//   RS485_DE  PK7  GPIO (driver enable,  active-high)
//   RS485_RE  PK6  GPIO (receiver enable, active-low)
//
//*****************************************************************************
#if 0
#define RS485_UART_BASE         UART0_BASE
#define RS485_DE_PORT           GPIO_PORTK_BASE
#define RS485_DE_PIN            GPIO_PIN_7
#define RS485_RE_PORT           GPIO_PORTK_BASE
#define RS485_RE_PIN            GPIO_PIN_6
#endif

#endif // __BOARD_PINS_H__
