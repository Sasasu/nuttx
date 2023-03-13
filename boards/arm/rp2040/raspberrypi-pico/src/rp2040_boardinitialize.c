/****************************************************************************
 * boards/arm/rp2040/raspberrypi-pico/src/rp2040_boardinitialize.c
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

#include <debug.h>

#include <nuttx/board.h>
#include <arch/board/board.h>

#include "arm_internal.h"
#include "rp2040_gpio.h"

#ifdef CONFIG_ARCH_BOARD_COMMON
#include "rp2040_common_initialize.h"
#endif /* CONFIG_ARCH_BOARD_COMMON */

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp2040_boardearlyinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp2040_boardearlyinitialize(void)
{
  #ifdef CONFIG_ARCH_BOARD_COMMON
  rp2040_common_earlyinitialize();
  #endif

  /* --- Place any board specific early initialization here --- */

  {
    // enable the main power input
    rp2040_gpio_init(BOARD_GPIO_POWER_EN);
    rp2040_gpio_init(BOARD_GPIO_SW_1);

    rp2040_gpio_set_slew_fast(BOARD_GPIO_LED_PIN, true);
    rp2040_gpio_set_drive_strength(BOARD_GPIO_POWER_EN, RP2040_PADS_BANK0_GPIO_DRIVE_12MA);

    rp2040_gpio_setdir(BOARD_GPIO_POWER_EN, true);

    rp2040_gpio_setdir(BOARD_GPIO_SW_1, false);
    rp2040_gpio_setdir(BOARD_GPIO_SW_2, false);

    rp2040_gpio_put(BOARD_GPIO_POWER_EN, true);
  }

  /* Set board LED pin */
  rp2040_gpio_init(BOARD_GPIO_LED_PIN);
  rp2040_gpio_setdir(BOARD_GPIO_LED_PIN, true);
  rp2040_gpio_put(BOARD_GPIO_LED_PIN, true);
}

/****************************************************************************
 * Name: rp2040_boardinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp2040_boardinitialize(void)
{
  #ifdef CONFIG_ARCH_BOARD_COMMON
  rp2040_common_initialize();
  #endif

  /* --- Place any board specific initialization here --- */
}
