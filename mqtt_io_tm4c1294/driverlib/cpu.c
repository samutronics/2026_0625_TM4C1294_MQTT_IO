//*****************************************************************************
//
// cpu.c - Instruction wrappers for special CPU instructions needed by the
//         drivers.
//
// Copyright (c) 2006-2020 Texas Instruments Incorporated.  All rights reserved.
// Software License Agreement
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions
//   are met:
//
//   Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
//   Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the
//   distribution.
//
//   Neither the name of Texas Instruments Incorporated nor the names of
//   its contributors may be used to endorse or promote products derived
//   from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// This is part of revision 2.2.0.295 of the Tiva Peripheral Driver Library.
//
//*****************************************************************************
//
// NOTE (mqtt_io project): The upstream TivaWare cpu.c implements these wrappers
// as GCC "naked" functions whose bodies contain a trailing C return statement.
// The TI Arm Clang (tiarmclang) compiler rejects non-asm statements inside a
// naked function ("non-ASM statement in naked function is not supported").
// This file re-implements the same wrappers using ordinary (non-naked)
// functions with extended inline assembly, which is accepted by both Clang and
// GCC and produces identical behaviour.
//
//*****************************************************************************

#include <stdint.h>
#include "driverlib/cpu.h"

//*****************************************************************************
//
// Wrapper function for the CPSID instruction.  Returns the state of PRIMASK
// on entry.
//
//*****************************************************************************
uint32_t
CPUcpsid(void)
{
    uint32_t ui32Ret;

    //
    // Read PRIMASK and disable interrupts.
    //
    __asm volatile("mrs   %0, PRIMASK\n"
                   "cpsid i\n"
                   : "=r" (ui32Ret));

    return(ui32Ret);
}

//*****************************************************************************
//
// Wrapper function returning the state of PRIMASK (indicating whether
// interrupts are enabled or disabled).
//
//*****************************************************************************
uint32_t
CPUprimask(void)
{
    uint32_t ui32Ret;

    //
    // Read PRIMASK.
    //
    __asm volatile("mrs   %0, PRIMASK\n"
                   : "=r" (ui32Ret));

    return(ui32Ret);
}

//*****************************************************************************
//
// Wrapper function for the CPSIE instruction.  Returns the state of PRIMASK
// on entry.
//
//*****************************************************************************
uint32_t
CPUcpsie(void)
{
    uint32_t ui32Ret;

    //
    // Read PRIMASK and enable interrupts.
    //
    __asm volatile("mrs   %0, PRIMASK\n"
                   "cpsie i\n"
                   : "=r" (ui32Ret));

    return(ui32Ret);
}

//*****************************************************************************
//
// Wrapper function for the WFI instruction.
//
//*****************************************************************************
void
CPUwfi(void)
{
    //
    // Wait for the next interrupt.
    //
    __asm volatile("wfi\n");
}

//*****************************************************************************
//
// Wrapper function for writing the BASEPRI register.
//
//*****************************************************************************
void
CPUbasepriSet(uint32_t ui32NewBasepri)
{
    //
    // Set the BASEPRI register.
    //
    __asm volatile("msr   BASEPRI, %0\n"
                   :
                   : "r" (ui32NewBasepri));
}

//*****************************************************************************
//
// Wrapper function for reading the BASEPRI register.
//
//*****************************************************************************
uint32_t
CPUbasepriGet(void)
{
    uint32_t ui32Ret;

    //
    // Read BASEPRI.
    //
    __asm volatile("mrs   %0, BASEPRI\n"
                   : "=r" (ui32Ret));

    return(ui32Ret);
}
