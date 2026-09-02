/******************************************************************************
 *
 * enet_io_ccs.cmd - CCS linker configuration file for enet_io.
 *
 * Copyright (c) 2013-2020 Texas Instruments Incorporated.  All rights reserved.
 * Software License Agreement
 * 
 * Texas Instruments (TI) is supplying this software for use solely and
 * exclusively on TI's microcontroller products. The software is owned by
 * TI and/or its suppliers, and is protected under applicable copyright
 * laws. You may not combine this software with "viral" open-source
 * software in order to form a larger program.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND WITH ALL FAULTS.
 * NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT
 * NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. TI SHALL NOT, UNDER ANY
 * CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
 * DAMAGES, FOR ANY REASON WHATSOEVER.
 * 
 * This is part of revision 2.2.0.295 of the EK-TM4C1294XL Firmware Package.
 *
 *****************************************************************************/

--retain=g_pfnVectors

/* The following command line options are set as part of the CCS project.    */
/* If you are building using the command line, or for some reason want to    */
/* define them here, you can uncomment and modify these lines as needed.     */
/* If you are using CCS for building, it is probably better to make any such */
/* modifications in your CCS project and leave this file alone.              */
/*                                                                           */
/* --heap_size=0                                                             */
/* --stack_size=256                                                          */
/* --library=rtsv7M3_T_le_eabi.lib                                           */

/* The starting address of the application.  Normally the interrupt vectors  */
/* must be located at the beginning of the application.                      */
#define APP_BASE 0x00000000
#define RAM_BASE 0x20000000

/* System memory map
 *
 * Flash is split into two halves:
 *   FLASH  (0x00000 - 0x7FFFF)  512 KB  application code
 *   OTA    (0x80000 - 0xFFFFF)  512 KB  OTA staging (header at 0x80000, image at 0x84000)
 *
 * Limiting FLASH to 512 KB keeps the linker from placing sections into the OTA
 * region and provides a compile-time guard against firmware size creep.
 */

MEMORY
{
    FLASH   (RX)  : origin = APP_BASE,    length = 0x00080000  /* 512 KB app slot  */
    OTA     (R)   : origin = 0x00080000,  length = 0x00080000  /* 512 KB OTA area  */
    SRAM    (RWX) : origin = 0x20000000,  length = 0x00040000  /* 256 KB SRAM      */
}

/* Section allocation in memory */

SECTIONS
{
    .intvecs:   > APP_BASE
    .text   :   > FLASH
    .const  :   > FLASH
    .rodata :   > FLASH
    .cinit  :   > FLASH
    .pinit  :   > FLASH
    .init_array : > FLASH
    .TI.ramfunc : {} load=FLASH, run=SRAM, table(BINIT)
    .binit      :    > FLASH

    .vtable :   > RAM_BASE
    .data   :   > SRAM
    .bss    :   > SRAM
    .sysmem :   > SRAM
    .stack  :   > SRAM
}

__STACK_TOP = __stack + 1024;
