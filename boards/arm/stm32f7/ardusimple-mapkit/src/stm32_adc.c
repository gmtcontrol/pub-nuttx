/****************************************************************************
 * boards/arm/stm32f7/ardusimple-mapkit/src/stm32_adc.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/board.h>
#include <nuttx/analog/adc.h>
#include <arch/board/board.h>
#include <nuttx/sensors/analog.h>

#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include "stm32_gpio.h"
#include "stm32_adc.h"

#ifdef CONFIG_ADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* Up to 3 ADC interfaces are supported */

#if STM32F7_NADC < 3
#  undef CONFIG_STM32F7_ADC3
#endif

#if STM32F7_NADC < 2
#  undef CONFIG_STM32F7_ADC2
#endif

#if STM32F7_NADC < 1
#  undef CONFIG_STM32F7_ADC1
#endif

#if defined(CONFIG_STM32F7_ADC1) || defined(CONFIG_STM32F7_ADC2) || defined(CONFIG_STM32F7_ADC3)
#ifndef CONFIG_STM32F7_ADC1
#  warning "Channel information only available for ADC1"
#endif

/* The number of ADC channels in the conversion list */

#define ADC1_NCHANNELS  2

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Identifying number of each ADC channel (even if NCHANNELS is less ) */

static const uint8_t g_chanlist[ADC1_NCHANNELS] =
{
  0, 1
};

/* Configurations of pins used by each ADC channel */

static const uint32_t g_pinlist[ADC1_NCHANNELS] =
{
  GPIO_ADC1_IN0,                /* PA0: VLIPO   */
  GPIO_ADC1_IN1,                /* PA1: HW_REV  */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_adc_setup
 *
 * Description:
 *   Initialize ADC and register the ADC driver.
 *
 ****************************************************************************/

int stm32_adc_setup(void)
{
#ifdef CONFIG_STM32F7_ADC1
  static bool initialized = false;
  struct adc_dev_s *adc;
  int ret;
  int i;

  /* Check if we have already initialized */

  if (!initialized)
    {
      /* Configure the pins as analog inputs for the selected channels */

      for (i = 0; i < ADC1_NCHANNELS; i++)
        {
          if (g_pinlist[i] != 0)
            {
              stm32_configgpio(g_pinlist[i]);
            }
        }

      /* Call stm32_adcinitialize() to get an instance of the ADC interface */

      adc = stm32_adc_initialize(1, g_chanlist, ADC1_NCHANNELS);
      if (adc == NULL)
        {
          aerr("ERROR: Failed to get ADC interface\n");
          return -ENODEV;
        }

      /* Register the ADC driver at "/dev/adc0" */

      ret = adc_register("/dev/adc0", adc);
      if (ret < 0)
        {
          aerr("ERROR: adc_register failed: %d\n", ret);
          return ret;
        }

#ifdef CONFIG_SENSORS_ANALOG
      ret = analog_uorb_register(0, 1);
      if (ret < 0)
        {
          aerr("ERROR: analog_uorb_register failed: %d\n", ret);
          return ret;
        }
#endif /* CONFIG_SENSORS_ANALOG */

      /* Now we are initialized */

      initialized = true;
    }

  return OK;
#else
  return -ENOSYS;
#endif
}

#endif /* CONFIG_STM32F7_ADC1 || CONFIG_STM32F7_ADC2 || CONFIG_STM32F7_ADC3 */
#endif /* CONFIG_ADC */
