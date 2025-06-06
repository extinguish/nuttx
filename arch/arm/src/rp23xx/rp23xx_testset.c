/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_testset.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <nuttx/spinlock.h>

#include "hardware/rp23xx_sio.h"
#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Errata RP2350-E2 SIO SPINLOCK writes are mirrored at +0x80 offset
 * Use only safe SPINLOCKS
 * The following SIO spinlocks can be used normally as they do not alias
 * with writable registers: 5, 6, 7, 10,11, and 18 through 31.
 */

#define RP23XX_TESTSET_SPINLOCK     7   /* Spinlock used for test and set */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_testset
 *
 * Description:
 *   Perform and atomic test and set operation on the provided spinlock.
 *   This function must be provided via the architecture-specific logic.
 *
 * Input Parameters:
 *   lock  - A reference to the spinlock object.
 *
 * Returned Value:
 *   The spinlock is always locked upon return.  The previous value of the
 *   spinlock variable is returned, either SP_LOCKED if the spinlock was
 *   previously locked (meaning that the test-and-set operation failed to
 *   obtain the lock) or SP_UNLOCKED if the spinlock was previously unlocked
 *   (meaning that we successfully obtained the lock).
 *
 ****************************************************************************/

spinlock_t up_testset(volatile spinlock_t *lock)
{
  spinlock_t ret;

  /* Lock hardware spinlock */

  while (getreg32(RP23XX_SIO_SPINLOCK(RP23XX_TESTSET_SPINLOCK)) == 0)
    ;

  ret = *lock;

  if (ret == SP_UNLOCKED)
    {
      *lock = SP_LOCKED;
      UP_DMB();
    }

  /* Unlock hardware spinlock */

  putreg32(0, RP23XX_SIO_SPINLOCK(RP23XX_TESTSET_SPINLOCK));

  return ret;
}
