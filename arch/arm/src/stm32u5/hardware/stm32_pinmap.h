/****************************************************************************
 * arch/arm/src/stm32u5/hardware/stm32_pinmap.h
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

#ifndef __ARCH_ARM_SRC_STM32U5_HARDWARE_STM32_PINMAP_H
#define __ARCH_ARM_SRC_STM32U5_HARDWARE_STM32_PINMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include "chip.h"

#if defined(CONFIG_STM32U5_STM32U585XX) || defined(CONFIG_STM32U5_STM32U5A5XX)
#  include "hardware/stm32u5xx_pinmap.h"
#else
#  error "Unsupported STM32U5 pin map"
#endif

#endif /* __ARCH_ARM_SRC_STM32U5_HARDWARE_STM32_PINMAP_H */
