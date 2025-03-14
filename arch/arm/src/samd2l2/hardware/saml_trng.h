/****************************************************************************
 * arch/arm/src/samd2l2/hardware/saml_trng.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* References:
 *   "Atmel SAM L21E / SAM L21G / SAM L21J Smart ARM-Based Microcontroller
 *   Datasheet", Atmel-42385C-SAML21_Datasheet_Preliminary-03/20/15
 */

#ifndef __ARCH_ARM_SRC_SAMD2L2_HARDWARE_SAML_TRNG_H
#define __ARCH_ARM_SRC_SAMD2L2_HARDWARE_SAML_TRNG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "chip.h"

#ifdef CONFIG_ARCH_FAMILY_SAML21

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* TRNG register offsets ****************************************************/

#define SAM_TRNG_CTRLA_OFFSET       0x0000  /* Control A register */
#define SAM_TRNG_EVCTRL_OFFSET      0x0004  /* Event control register */
#define SAM_TRNG_INTENCLR_OFFSET    0x0008  /* Interrupt enable clear register */
#define SAM_TRNG_INTENSET_OFFSET    0x0009  /* Interrupt enable set register */
#define SAM_TRNG_INTFLAG_OFFSET     0x000a  /* Interrupt flag and status clear register */
#define SAM_TRNG_DATA_OFFSET        0x0020  /* Output data register */

/* TRNG register addresses **************************************************/

#define SAM_TRNG_CTRLA              (SAM_TRNG_BASE+SAM_TRNG_CTRLA_OFFSET)
#define SAM_TRNG_EVCTRL             (SAM_TRNG_BASE+SAM_TRNG_EVCTRL_OFFSET)
#define SAM_TRNG_INTENCLR           (SAM_TRNG_BASE+SAM_TRNG_INTENCLR_OFFSET)
#define SAM_TRNG_INTENSET           (SAM_TRNG_BASE+SAM_TRNG_INTENSET_OFFSET)
#define SAM_TRNG_INTFLAG            (SAM_TRNG_BASE+SAM_TRNG_INTFLAG_OFFSET)
#define SAM_TRNG_DATA               (SAM_TRNG_BASE+SAM_TRNG_DATA_OFFSET)

/* TRNG register bit definitions ********************************************/

/* Control register */

#define TRNG_CTRLA_ENABLE            (1 << 1)  /* Bit 1:  Enable */
#define TRNG_CTRLA_WEN               (1 << 6)  /* Bit 6:  Run in standby */

/* Event control register, Interrupt enable clear, interrupt enable set
 * register, interrupt flag status registers.
 */

#define TRNG_EVCTRL_DATARDY          (1 << 0)  /* Bit 0:  Data ready */

/* Data register (32-bit data) */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Public Functions Prototypes
 ****************************************************************************/

#endif /* CONFIG_ARCH_FAMILY_SAML21 */
#endif /* __ARCH_ARM_SRC_SAMD2L2_HARDWARE_SAML_TRNG_H */
