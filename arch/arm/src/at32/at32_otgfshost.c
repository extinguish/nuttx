/****************************************************************************
 * arch/arm/src/at32/at32_otgfshost.c
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

#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/signal.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/usb/usbhost_devaddr.h>
#include <nuttx/usb/usbhost_trace.h>

#include <nuttx/irq.h>

#include "chip.h"             /* Includes default GPIO settings */
#include <arch/board/board.h> /* May redefine GPIO settings */

#include "arm_internal.h"
#include "at32_gpio.h"
#include "at32_usbhost.h"

#if defined(CONFIG_AT32_USBHOST) && defined(CONFIG_AT32_OTGFS)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* AT32 USB OTG FS Host Driver Support
 *
 * Pre-requisites
 *
 *  CONFIG_AT32_USBHOST - Enable AT32 USB host support
 *  CONFIG_USBHOST       - Enable general USB host support
 *  CONFIG_AT32_OTGFS   - Enable the AT32 USB OTG FS block
 *  CONFIG_AT32_SYSCFG  - Needed
 *
 * Options:
 *
 *  CONFIG_AT32_OTGFS_RXFIFO_SIZE - Size of the RX FIFO in 32-bit words.
 *    Default 128 (512 bytes)
 *  CONFIG_AT32_OTGFS_NPTXFIFO_SIZE - Size of the non-periodic Tx FIFO
 *    in 32-bit words.  Default 96 (384 bytes)
 *  CONFIG_AT32_OTGFS_PTXFIFO_SIZE - Size of the periodic Tx FIFO in 32-bit
 *    words.  Default 96 (384 bytes)
 *  CONFIG_AT32_OTGFS_DESCSIZE - Maximum size of a descriptor.  Default: 128
 *  CONFIG_AT32_OTGFS_SOFINTR - Enable SOF interrupts.  Why would you ever
 *    want to do that?
 *  CONFIG_AT32_USBHOST_REGDEBUG - Enable very low-level register access
 *    debug.  Depends on CONFIG_DEBUG_FEATURES.
 *  CONFIG_AT32_USBHOST_PKTDUMP - Dump all incoming and outgoing USB
 *    packets. Depends on CONFIG_DEBUG_FEATURES.
 */

/* Pre-requisites (partial) */

#ifndef CONFIG_AT32_SYSCFG
#  error "CONFIG_AT32_SYSCFG is required"
#endif

/* Default RxFIFO size */

#ifndef CONFIG_AT32_OTGFS_RXFIFO_SIZE
#  define CONFIG_AT32_OTGFS_RXFIFO_SIZE 128
#endif

/* Default host non-periodic Tx FIFO size */

#ifndef CONFIG_AT32_OTGFS_NPTXFIFO_SIZE
#  define CONFIG_AT32_OTGFS_NPTXFIFO_SIZE 96
#endif

/* Default host periodic Tx fifo size register */

#ifndef CONFIG_AT32_OTGFS_PTXFIFO_SIZE
#  define CONFIG_AT32_OTGFS_PTXFIFO_SIZE 96
#endif

/* Maximum size of a descriptor */

#ifndef CONFIG_AT32_OTGFS_DESCSIZE
#  define CONFIG_AT32_OTGFS_DESCSIZE 128
#endif

/* Register/packet debug depends on CONFIG_DEBUG_FEATURES */

#ifndef CONFIG_DEBUG_USB_INFO
#  undef CONFIG_AT32_USBHOST_REGDEBUG
#  undef CONFIG_AT32_USBHOST_PKTDUMP
#endif

/* HCD Setup ****************************************************************/

/* Hardware capabilities */

#define AT32_NHOST_CHANNELS      16  /* Number of host channels */
#define AT32_MAX_PACKET_SIZE     64  /* Full speed max packet size */
#define AT32_EP0_DEF_PACKET_SIZE 8   /* EP0 default packet size */
#define AT32_EP0_MAX_PACKET_SIZE 64  /* EP0 FS max packet size */
#define AT32_MAX_TX_FIFOS        15  /* Max number of TX FIFOs */
#define AT32_MAX_PKTCOUNT        256 /* Max packet count */
#define AT32_RETRY_COUNT         3   /* Number of ctrl transfer retries */

/* Delays *******************************************************************/

#define AT32_READY_DELAY         200000      /* In loop counts */
#define AT32_FLUSH_DELAY         200000      /* In loop counts */
#define AT32_SETUP_DELAY         SEC2TICK(5) /* 5 seconds in system ticks */
#define AT32_DATANAK_DELAY       SEC2TICK(5) /* 5 seconds in system ticks */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The following enumeration represents the various states of the USB host
 * state machine (for debug purposes only)
 */

enum at32_smstate_e
{
  SMSTATE_DETACHED = 0,  /* Not attached to a device */
  SMSTATE_ATTACHED,      /* Attached to a device */
  SMSTATE_ENUM,          /* Attached, enumerating */
  SMSTATE_CLASS_BOUND,   /* Enumeration complete, class bound */
};

/* This enumeration provides the reason for the channel halt. */

enum at32_chreason_e
{
  CHREASON_IDLE = 0,     /* Inactive (initial state) */
  CHREASON_FREED,        /* Channel is no longer in use */
  CHREASON_XFRC,         /* Transfer complete */
  CHREASON_NAK,          /* NAK received */
  CHREASON_NYET,         /* NotYet received */
  CHREASON_STALL,        /* Endpoint stalled */
  CHREASON_TXERR,        /* Transfer error received */
  CHREASON_DTERR,        /* Data toggle error received */
  CHREASON_FRMOR,        /* Frame overrun */
  CHREASON_CANCELLED     /* Transfer cancelled */
};

/* This structure retains the state of one host channel.  NOTE: Since there
 * is only one channel operation active at a time, some of the fields in
 * in the structure could be moved in struct at32_ubhost_s to achieve
 * some memory savings.
 */

struct at32_chan_s
{
  sem_t             waitsem;   /* Channel wait semaphore */
  volatile uint8_t  result;    /* The result of the transfer */
  volatile uint8_t  chreason;  /* Channel halt reason. See enum at32_chreason_e */
  uint8_t           chidx;     /* Channel index */
  uint8_t           epno;      /* Device endpoint number (0-127) */
  uint8_t           eptype;    /* See OTGFS_EPTYPE_* definitions */
  uint8_t           funcaddr;  /* Device function address */
  uint8_t           speed;     /* Device speed */
  uint8_t           interval;  /* Interrupt/isochronous EP polling interval */
  uint8_t           pid;       /* Data PID */
  uint8_t           npackets;  /* Number of packets (for data toggle) */
  bool              inuse;     /* True: This channel is "in use" */
  volatile bool     indata1;   /* IN data toggle. True: DATA01 (Bulk and INTR only) */
  volatile bool     outdata1;  /* OUT data toggle.  True: DATA01 */
  bool              in;        /* True: IN endpoint */
  volatile bool     waiter;    /* True: Thread is waiting for a channel event */
  uint16_t          maxpacket; /* Max packet size */
  uint16_t          buflen;    /* Buffer length (at start of transfer) */
  volatile uint16_t xfrd;      /* Bytes transferred (at end of transfer) */
  volatile uint16_t inflight;  /* Number of Tx bytes "in-flight" */
  uint8_t          *buffer;    /* Transfer buffer pointer */
#ifdef CONFIG_USBHOST_ASYNCH
  usbhost_asynch_t  callback;  /* Transfer complete callback */
  void             *arg;       /* Argument that accompanies the callback */
#endif
};

/* A channel represents on uni-directional endpoint.  So, in the case of the
 * bi-directional, control endpoint, there must be two channels to represent
 * the endpoint.
 */

struct at32_ctrlinfo_s
{
  uint8_t           inndx;     /* EP0 IN control channel index */
  uint8_t           outndx;    /* EP0 OUT control channel index */
};

/* This structure retains the state of the USB host controller */

struct at32_usbhost_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbhost_s
   * to structat32_usbhost_s.
   */

  struct usbhost_driver_s drvr;

  /* This is the hub port description understood by class drivers */

  struct usbhost_roothubport_s rhport;

  /* Overall driver status */

  volatile uint8_t  smstate;   /* The state of the USB host state machine */
  uint8_t           chidx;     /* ID of channel waiting for space in Tx FIFO */
  volatile bool     connected; /* Connected to device */
  volatile bool     change;    /* Connection change */
  volatile bool     pscwait;   /* True: Thread is waiting for a port event */
  mutex_t           lock;      /* Support mutually exclusive access */
  sem_t             pscsem;    /* Semaphore to wait for a port event */
  struct at32_ctrlinfo_s ep0;  /* Root hub port EP0 description */

#ifdef CONFIG_USBHOST_HUB
  /* Used to pass external hub port events */

  volatile struct usbhost_hubport_s *hport;
#endif

  struct usbhost_devaddr_s devgen;  /* Address generation data */

  /* The state of each host channel */

  struct at32_chan_s chan[AT32_MAX_TX_FIFOS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register operations ******************************************************/

#ifdef CONFIG_AT32_USBHOST_REGDEBUG
static void at32_printreg(uint32_t addr, uint32_t val, bool iswrite);
static void at32_checkreg(uint32_t addr, uint32_t val, bool iswrite);
static uint32_t at32_getreg(uint32_t addr);
static void at32_putreg(uint32_t addr, uint32_t value);
#else
#  define at32_getreg(addr)     getreg32(addr)
#  define at32_putreg(addr,val) putreg32(val,addr)
#endif

static inline void at32_modifyreg(uint32_t addr, uint32_t clrbits,
                                   uint32_t setbits);

#ifdef CONFIG_AT32_USBHOST_PKTDUMP
#  define at32_pktdump(m,b,n) lib_dumpbuffer(m,b,n)
#else
#  define at32_pktdump(m,b,n)
#endif

/* Byte stream access helper functions **************************************/

static inline uint16_t at32_getle16(const uint8_t *val);

/* Channel management *******************************************************/

static int at32_chan_alloc(struct at32_usbhost_s *priv);
static inline void at32_chan_free(struct at32_usbhost_s *priv,
                                   int chidx);
static inline void at32_chan_freeall(struct at32_usbhost_s *priv);
static void at32_chan_configure(struct at32_usbhost_s *priv,
                                 int chidx);
static void at32_chan_halt(struct at32_usbhost_s *priv, int chidx,
                            enum at32_chreason_e chreason);
static int at32_chan_waitsetup(struct at32_usbhost_s *priv,
                                struct at32_chan_s *chan);
#ifdef CONFIG_USBHOST_ASYNCH
static int at32_chan_asynchsetup(struct at32_usbhost_s *priv,
                                  struct at32_chan_s *chan,
                                  usbhost_asynch_t callback, void *arg);
#endif
static int at32_chan_wait(struct at32_usbhost_s *priv,
                           struct at32_chan_s *chan);
static void at32_chan_wakeup(struct at32_usbhost_s *priv,
                              struct at32_chan_s *chan);
static int at32_ctrlchan_alloc(struct at32_usbhost_s *priv,
                                uint8_t epno, uint8_t funcaddr,
                                uint8_t speed,
                                struct at32_ctrlinfo_s *ctrlep);
static int at32_ctrlep_alloc(struct at32_usbhost_s *priv,
                              const struct usbhost_epdesc_s *epdesc,
                              usbhost_ep_t *ep);
static int at32_xfrep_alloc(struct at32_usbhost_s *priv,
                             const struct usbhost_epdesc_s *epdesc,
                             usbhost_ep_t *ep);

/* Control/data transfer logic **********************************************/

static void at32_transfer_start(struct at32_usbhost_s *priv,
                                 int chidx);
#if 0 /* Not used */
static inline uint16_t at32_getframe(void);
#endif
static int at32_ctrl_sendsetup(struct at32_usbhost_s *priv,
                                struct at32_ctrlinfo_s *ep0,
                                const struct usb_ctrlreq_s *req);
static int at32_ctrl_senddata(struct at32_usbhost_s *priv,
                               struct at32_ctrlinfo_s *ep0,
                               uint8_t *buffer, unsigned int buflen);
static int at32_ctrl_recvdata(struct at32_usbhost_s *priv,
                               struct at32_ctrlinfo_s *ep0,
                               uint8_t *buffer, unsigned int buflen);
static int at32_in_setup(struct at32_usbhost_s *priv, int chidx);
static ssize_t at32_in_transfer(struct at32_usbhost_s *priv, int chidx,
                                 uint8_t *buffer, size_t buflen);
#ifdef CONFIG_USBHOST_ASYNCH
static void at32_in_next(struct at32_usbhost_s *priv,
                          struct at32_chan_s *chan);
static int at32_in_asynch(struct at32_usbhost_s *priv, int chidx,
                           uint8_t *buffer, size_t buflen,
                           usbhost_asynch_t callback, void *arg);
#endif
static int at32_out_setup(struct at32_usbhost_s *priv, int chidx);
static ssize_t at32_out_transfer(struct at32_usbhost_s *priv,
                                  int chidx, uint8_t *buffer,
                                  size_t buflen);
#ifdef CONFIG_USBHOST_ASYNCH
static void at32_out_next(struct at32_usbhost_s *priv,
                           struct at32_chan_s *chan);
static int at32_out_asynch(struct at32_usbhost_s *priv, int chidx,
                            uint8_t *buffer, size_t buflen,
                            usbhost_asynch_t callback, void *arg);
#endif

/* Interrupt handling *******************************************************/

/* Lower level interrupt handlers */

static void at32_gint_wrpacket(struct at32_usbhost_s *priv,
                                uint8_t *buffer, int chidx, int buflen);
static inline void at32_gint_hcinisr(struct at32_usbhost_s *priv,
                                      int chidx);
static inline void at32_gint_hcoutisr(struct at32_usbhost_s *priv,
                                       int chidx);
static void at32_gint_connected(struct at32_usbhost_s *priv);
static void at32_gint_disconnected(struct at32_usbhost_s *priv);

/* Second level interrupt handlers */

#ifdef CONFIG_AT32_OTGFS_SOFINTR
static inline void at32_gint_sofisr(struct at32_usbhost_s *priv);
#endif
static inline void at32_gint_rxflvlisr(struct at32_usbhost_s *priv);
static inline void at32_gint_nptxfeisr(struct at32_usbhost_s *priv);
static inline void at32_gint_ptxfeisr(struct at32_usbhost_s *priv);
static inline void at32_gint_hcisr(struct at32_usbhost_s *priv);
static inline void at32_gint_hprtisr(struct at32_usbhost_s *priv);
static inline void at32_gint_discisr(struct at32_usbhost_s *priv);
static inline void at32_gint_ipxfrisr(struct at32_usbhost_s *priv);

/* First level, global interrupt handler */

static int at32_gint_isr(int irq, void *context, void *arg);

/* Interrupt controls */

static void at32_gint_enable(void);
static void at32_gint_disable(void);
static inline void at32_hostinit_enable(void);
static void at32_txfe_enable(struct at32_usbhost_s *priv, int chidx);

/* USB host controller operations *******************************************/

static int at32_wait(struct usbhost_connection_s *conn,
                      struct usbhost_hubport_s **hport);
static int at32_rh_enumerate(struct at32_usbhost_s *priv,
                              struct usbhost_connection_s *conn,
                              struct usbhost_hubport_s *hport);
static int at32_enumerate(struct usbhost_connection_s *conn,
                           struct usbhost_hubport_s *hport);

static int at32_ep0configure(struct usbhost_driver_s *drvr,
                              usbhost_ep_t ep0, uint8_t funcaddr,
                              uint8_t speed, uint16_t maxpacketsize);
static int at32_epalloc(struct usbhost_driver_s *drvr,
                         const struct usbhost_epdesc_s *epdesc,
                         usbhost_ep_t *ep);
static int at32_epfree(struct usbhost_driver_s *drvr, usbhost_ep_t ep);
static int at32_alloc(struct usbhost_driver_s *drvr,
                       uint8_t **buffer, size_t *maxlen);
static int at32_free(struct usbhost_driver_s *drvr,
                      uint8_t *buffer);
static int at32_ioalloc(struct usbhost_driver_s *drvr,
                         uint8_t **buffer, size_t buflen);
static int at32_iofree(struct usbhost_driver_s *drvr,
                        uint8_t *buffer);
static int at32_ctrlin(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                        const struct usb_ctrlreq_s *req,
                        uint8_t *buffer);
static int at32_ctrlout(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                         const struct usb_ctrlreq_s *req,
                         const uint8_t *buffer);
static ssize_t at32_transfer(struct usbhost_driver_s *drvr,
                              usbhost_ep_t ep, uint8_t *buffer,
                              size_t buflen);
#ifdef CONFIG_USBHOST_ASYNCH
static int at32_asynch(struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                        uint8_t *buffer, size_t buflen,
                        usbhost_asynch_t callback, void *arg);
#endif
static int at32_cancel(struct usbhost_driver_s *drvr, usbhost_ep_t ep);
#ifdef CONFIG_USBHOST_HUB
static int at32_connect(struct usbhost_driver_s *drvr,
                         struct usbhost_hubport_s *hport,
                         bool connected);
#endif
static void at32_disconnect(struct usbhost_driver_s *drvr,
                             struct usbhost_hubport_s *hport);

/* Initialization ***********************************************************/

static void at32_portreset(struct at32_usbhost_s *priv);
static void at32_flush_txfifos(uint32_t txfnum);
static void at32_flush_rxfifo(void);
static void at32_vbusdrive(struct at32_usbhost_s *priv, bool state);
static void at32_host_initialize(struct at32_usbhost_s *priv);

static inline void at32_sw_initialize(struct at32_usbhost_s *priv);
static inline int at32_hw_initialize(struct at32_usbhost_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* In this driver implementation, support is provided for only a single a
 * single USB device.  All status information can be simply retained in a
 * single global instance.
 */

static struct at32_usbhost_s g_usbhost =
{
  .lock = NXMUTEX_INITIALIZER,
  .pscsem = SEM_INITIALIZER(0),
};

/* This is the connection/enumeration interface */

static struct usbhost_connection_s g_usbconn =
{
  .wait      = at32_wait,
  .enumerate = at32_enumerate,
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: at32_printreg
 *
 * Description:
 *   Print the contents of an AT32xx register operation
 *
 ****************************************************************************/

#ifdef CONFIG_AT32_USBHOST_REGDEBUG
static void at32_printreg(uint32_t addr, uint32_t val, bool iswrite)
{
  uinfo("%08x%s%08x\n", addr, iswrite ? "<-" : "->", val);
}
#endif

/****************************************************************************
 * Name: at32_checkreg
 *
 * Description:
 *   Get the contents of an AT32 register
 *
 ****************************************************************************/

#ifdef CONFIG_AT32_USBHOST_REGDEBUG
static void at32_checkreg(uint32_t addr, uint32_t val, bool iswrite)
{
  static uint32_t prevaddr = 0;
  static uint32_t preval = 0;
  static uint32_t count = 0;
  static bool     prevwrite = false;

  /* Is this the same value that we read from/wrote to the same register
   * last time?  Are we polling the register?  If so, suppress the output.
   */

  if (addr == prevaddr && val == preval && prevwrite == iswrite)
    {
      /* Yes.. Just increment the count */

      count++;
    }
  else
    {
      /* No this is a new address or value or operation. Were there any
       * duplicate accesses before this one?
       */

      if (count > 0)
        {
          /* Yes.. Just one? */

          if (count == 1)
            {
              /* Yes.. Just one */

              at32_printreg(prevaddr, preval, prevwrite);
            }
          else
            {
              /* No.. More than one. */

              uinfo("[repeats %d more times]\n", count);
            }
        }

      /* Save the new address, value, count, and operation for next time */

      prevaddr  = addr;
      preval    = val;
      count     = 0;
      prevwrite = iswrite;

      /* Show the new regisgter access */

      at32_printreg(addr, val, iswrite);
    }
}
#endif

/****************************************************************************
 * Name: at32_getreg
 *
 * Description:
 *   Get the contents of an AT32 register
 *
 ****************************************************************************/

#ifdef CONFIG_AT32_USBHOST_REGDEBUG
static uint32_t at32_getreg(uint32_t addr)
{
  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Check if we need to print this value */

  at32_checkreg(addr, val, false);
  return val;
}
#endif

/****************************************************************************
 * Name: at32_putreg
 *
 * Description:
 *   Set the contents of an AT32 register to a value
 *
 ****************************************************************************/

#ifdef CONFIG_AT32_USBHOST_REGDEBUG
static void at32_putreg(uint32_t addr, uint32_t val)
{
  /* Check if we need to print this value */

  at32_checkreg(addr, val, true);

  /* Write the value */

  putreg32(val, addr);
}
#endif

/****************************************************************************
 * Name: at32_modifyreg
 *
 * Description:
 *   Modify selected bits of an AT32 register.
 *
 ****************************************************************************/

static inline void at32_modifyreg(uint32_t addr, uint32_t clrbits,
                                   uint32_t setbits)
{
  at32_putreg(addr, (((at32_getreg(addr)) & ~clrbits) | setbits));
}

/****************************************************************************
 * Name: at32_getle16
 *
 * Description:
 *   Get a (possibly unaligned) 16-bit little endian value.
 *
 ****************************************************************************/

static inline uint16_t at32_getle16(const uint8_t *val)
{
  return (uint16_t)val[1] << 8 | (uint16_t)val[0];
}

/****************************************************************************
 * Name: at32_chan_alloc
 *
 * Description:
 *   Allocate a channel.
 *
 ****************************************************************************/

static int at32_chan_alloc(struct at32_usbhost_s *priv)
{
  int chidx;

  /* Search the table of channels */

  for (chidx = 0; chidx < AT32_NHOST_CHANNELS; chidx++)
    {
      /* Is this channel available? */

      if (!priv->chan[chidx].inuse)
        {
          /* Yes... make it "in use" and return the index */

          priv->chan[chidx].inuse = true;
          return chidx;
        }
    }

  /* All of the channels are "in-use" */

  return -EBUSY;
}

/****************************************************************************
 * Name: at32_chan_free
 *
 * Description:
 *   Free a previoiusly allocated channel.
 *
 ****************************************************************************/

static void at32_chan_free(struct at32_usbhost_s *priv, int chidx)
{
  DEBUGASSERT((unsigned)chidx < AT32_NHOST_CHANNELS);

  /* Halt the channel */

  at32_chan_halt(priv, chidx, CHREASON_FREED);

  /* Mark the channel available */

  priv->chan[chidx].inuse = false;
}

/****************************************************************************
 * Name: at32_chan_freeall
 *
 * Description:
 *   Free all channels.
 *
 ****************************************************************************/

static inline void at32_chan_freeall(struct at32_usbhost_s *priv)
{
  uint8_t chidx;

  /* Free all host channels */

  for (chidx = 2; chidx < AT32_NHOST_CHANNELS; chidx++)
    {
      at32_chan_free(priv, chidx);
    }
}

/****************************************************************************
 * Name: at32_chan_configure
 *
 * Description:
 *   Configure or re-configure a host channel.  Host channels are configured
 *   when endpoint is allocated and EP0 (only) is re-configured with the
 *   max packet size or device address changes.
 *
 ****************************************************************************/

static void at32_chan_configure(struct at32_usbhost_s *priv, int chidx)
{
  struct at32_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;

  /* Clear any old pending interrupts for this host channel. */

  at32_putreg(AT32_OTGFS_HCINT(chidx), 0xffffffff);

  /* Enable channel interrupts required for transfers on this channel. */

  regval = 0;

  switch (chan->eptype)
    {
    case OTGFS_EPTYPE_CTRL:
    case OTGFS_EPTYPE_BULK:
      {
#ifdef HAVE_USBHOST_TRACE_VERBOSE
        uint16_t intrace;
        uint16_t outtrace;

        /* Determine the definitive trace ID to use below */

        if (chan->eptype == OTGFS_EPTYPE_CTRL)
          {
            intrace  = OTGFS_VTRACE2_CHANCONF_CTRL_IN;
            outtrace = OTGFS_VTRACE2_CHANCONF_CTRL_OUT;
          }
        else
          {
            intrace  = OTGFS_VTRACE2_CHANCONF_BULK_IN;
            outtrace = OTGFS_VTRACE2_CHANCONF_BULK_OUT;
          }
#endif

        /* Interrupts required for CTRL and BULK endpoints */

        regval |= (OTGFS_HCINT_XFRC  | OTGFS_HCINT_STALL | OTGFS_HCINT_NAK |
                   OTGFS_HCINT_TXERR | OTGFS_HCINT_DTERR);

        /* Additional setting for IN/OUT endpoints */

        if (chan->in)
          {
            usbhost_vtrace2(intrace, chidx, chan->epno);
            regval |= OTGFS_HCINT_BBERR;
          }
        else
          {
            usbhost_vtrace2(outtrace, chidx, chan->epno);
            regval |= OTGFS_HCINT_NYET;
          }
      }
      break;

    case OTGFS_EPTYPE_INTR:
      {
        /* Interrupts required for INTR endpoints */

        regval |= (OTGFS_HCINT_XFRC | OTGFS_HCINT_STALL |
                   OTGFS_HCINT_NAK   | OTGFS_HCINT_TXERR |
                   OTGFS_HCINT_FRMOR | OTGFS_HCINT_DTERR);

        /* Additional setting for IN endpoints */

        if (chan->in)
          {
            usbhost_vtrace2(OTGFS_VTRACE2_CHANCONF_INTR_IN, chidx,
                            chan->epno);
            regval |= OTGFS_HCINT_BBERR;
          }
#ifdef HAVE_USBHOST_TRACE_VERBOSE
        else
          {
            usbhost_vtrace2(OTGFS_VTRACE2_CHANCONF_INTR_OUT, chidx,
                            chan->epno);
          }
#endif
      }
      break;

    case OTGFS_EPTYPE_ISOC:
      {
        /* Interrupts required for ISOC endpoints */

        regval |= OTGFS_HCINT_XFRC | OTGFS_HCINT_ACK | OTGFS_HCINT_FRMOR;

        /* Additional setting for IN endpoints */

        if (chan->in)
          {
            usbhost_vtrace2(OTGFS_VTRACE2_CHANCONF_ISOC_IN, chidx,
                            chan->epno);
            regval |= (OTGFS_HCINT_TXERR | OTGFS_HCINT_BBERR);
          }
#ifdef HAVE_USBHOST_TRACE_VERBOSE
        else
          {
            usbhost_vtrace2(OTGFS_VTRACE2_CHANCONF_ISOC_OUT, chidx,
                            chan->epno);
          }
#endif
      }
      break;
    }

  at32_putreg(AT32_OTGFS_HCINTMSK(chidx), regval);

  /* Enable the top level host channel interrupt. */

  at32_modifyreg(AT32_OTGFS_HAINTMSK, 0, OTGFS_HAINT(chidx));

  /* Make sure host channel interrupts are enabled. */

  at32_modifyreg(AT32_OTGFS_GINTMSK, 0, OTGFS_GINT_HC);

  /* Program the HCCHAR register */

  regval = ((uint32_t)chan->maxpacket << OTGFS_HCCHAR_MPSIZ_SHIFT) |
           ((uint32_t)chan->epno      << OTGFS_HCCHAR_EPNUM_SHIFT) |
           ((uint32_t)chan->eptype    << OTGFS_HCCHAR_EPTYP_SHIFT) |
           ((uint32_t)chan->funcaddr  << OTGFS_HCCHAR_DAD_SHIFT);

  /* Special case settings for low speed devices */

  if (chan->speed == USB_SPEED_LOW)
    {
      regval |= OTGFS_HCCHAR_LSDEV;
    }

  /* Special case settings for IN endpoints */

  if (chan->in)
    {
      regval |= OTGFS_HCCHAR_EPDIR_IN;
    }

  /* Special case settings for INTR endpoints */

  if (chan->eptype == OTGFS_EPTYPE_INTR)
    {
      regval |= OTGFS_HCCHAR_ODDFRM;
    }

  /* Write the channel configuration */

  at32_putreg(AT32_OTGFS_HCCHAR(chidx), regval);
}

/****************************************************************************
 * Name: at32_chan_halt
 *
 * Description:
 *   Halt the channel associated with 'chidx' by setting the CHannel DISable
 *   (CHDIS) bit in in the HCCHAR register.
 *
 ****************************************************************************/

static void at32_chan_halt(struct at32_usbhost_s *priv, int chidx,
                            enum at32_chreason_e chreason)
{
  uint32_t hcchar;
  uint32_t intmsk;
  uint32_t eptype;
  unsigned int avail;

  /* Save the reason for the halt.  We need this in the channel halt
   * interrupt handling logic to know what to do next.
   */

  usbhost_vtrace2(OTGFS_VTRACE2_CHANHALT, chidx, chreason);

  priv->chan[chidx].chreason = (uint8_t)chreason;

  /* "The application can disable any channel by programming the
   *  OTG_FS_HCCHARx register with the CHDIS and CHENA bits set to 1.  This
   *  enables the OTG_FS host to flush the posted requests (if any) and
   *  generates a channel halted interrupt. The application must wait for
   *  the CHH interrupt in OTG_FS_HCINTx before reallocating the channel for
   *  other transactions.  The OTG_FS host does not interrupt the
   *  transaction that has already been started on the USB."
   */

  hcchar  = at32_getreg(AT32_OTGFS_HCCHAR(chidx));
  hcchar |= (OTGFS_HCCHAR_CHDIS | OTGFS_HCCHAR_CHENA);

  /* Get the endpoint type from the HCCHAR register */

  eptype = hcchar & OTGFS_HCCHAR_EPTYP_MASK;

  /* Check for space in the Tx FIFO to issue the halt.
   *
   * "Before disabling a channel, the application must ensure that there is
   *  at least one free space available in the non-periodic request queue
   *  (when disabling a non-periodic channel) or the periodic request queue
   *  (when disabling a periodic channel). The application can simply flush
   *  the posted requests when the Request queue is full (before disabling
   *  the channel), by programming the OTG_FS_HCCHARx register with the
   *  CHDIS bit set to 1, and the CHENA bit cleared to 0."
   */

  if (eptype == OTGFS_HCCHAR_EPTYP_CTRL ||
      eptype == OTGFS_HCCHAR_EPTYP_BULK)
    {
      /* Get the number of words available in the non-periodic Tx FIFO. */

      avail = at32_getreg(AT32_OTGFS_HNPTXSTS) &
              OTGFS_HNPTXSTS_NPTXFSAV_MASK;
    }
  else
    {
      /* Get the number of words available in the non-periodic Tx FIFO. */

      avail = at32_getreg(AT32_OTGFS_HPTXSTS) &
              OTGFS_HPTXSTS_PTXFSAVL_MASK;
    }

  /* Check if there is any space available in the Tx FIFO. */

  if (avail == 0)
    {
      /* The Tx FIFO is full... disable the channel to flush the requests */

      hcchar &= ~OTGFS_HCCHAR_CHENA;
    }

  /* Unmask the CHannel Halted (CHH) interrupt */

  intmsk  = at32_getreg(AT32_OTGFS_HCINTMSK(chidx));
  intmsk |= OTGFS_HCINT_CHH;
  at32_putreg(AT32_OTGFS_HCINTMSK(chidx), intmsk);

  /* Halt the channel by setting CHDIS (and maybe CHENA) in the HCCHAR */

  at32_putreg(AT32_OTGFS_HCCHAR(chidx), hcchar);
}

/****************************************************************************
 * Name: at32_chan_waitsetup
 *
 * Description:
 *   Set the request for the transfer complete event well BEFORE enabling
 *   the transfer (as soon as we are absolutely committed to the transfer).
 *   We do this to minimize race conditions.  This logic would have to be
 *   expanded if we want to have more than one packet in flight at a time!
 *
 * Assumptions:
 *   Called from a normal thread context BEFORE the transfer has been
 *   started.
 *
 ****************************************************************************/

static int at32_chan_waitsetup(struct at32_usbhost_s *priv,
                                struct at32_chan_s *chan)
{
  irqstate_t flags = enter_critical_section();
  int        ret   = -ENODEV;

  /* Is the device still connected? */

  if (priv->connected)
    {
      /* Yes.. then set waiter to indicate that we expect to be informed
       * when either (1) the device is disconnected, or (2) the transfer
       * completed.
       */

      chan->waiter   = true;
#ifdef CONFIG_USBHOST_ASYNCH
      chan->callback = NULL;
      chan->arg      = NULL;
#endif
      ret            = OK;
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: at32_chan_asynchsetup
 *
 * Description:
 *   Set the request for the transfer complete event well BEFORE enabling
 *   the transfer (as soon as we are absolutely committed to the to avoid
 *   transfer).  We do this to minimize race conditions.  This logic would
 *   have to be expanded if we want to have more than one packet in flight
 *   at a time!
 *
 * Assumptions:
 *   Might be called from the level of an interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static int at32_chan_asynchsetup(struct at32_usbhost_s *priv,
                                  struct at32_chan_s *chan,
                                  usbhost_asynch_t callback, void *arg)
{
  irqstate_t flags = enter_critical_section();
  int        ret   = -ENODEV;

  /* Is the device still connected? */

  if (priv->connected)
    {
      /* Yes.. then set waiter to indicate that we expect to be informed
       * when either (1) the device is disconnected, or (2) the transfer
       * completed.
       */

      chan->waiter   = false;
      chan->callback = callback;
      chan->arg      = arg;
      ret            = OK;
    }

  leave_critical_section(flags);
  return ret;
}
#endif

/****************************************************************************
 * Name: at32_chan_wait
 *
 * Description:
 *   Wait for a transfer on a channel to complete.
 *
 * Assumptions:
 *   Called from a normal thread context
 *
 ****************************************************************************/

static int at32_chan_wait(struct at32_usbhost_s *priv,
                           struct at32_chan_s *chan)
{
  irqstate_t flags;
  int ret;

  /* Disable interrupts so that the following operations will be atomic.  On
   * the OTG FS global interrupt needs to be disabled.  However, here we
   * disable all interrupts to exploit that fact that interrupts will be re-
   * enabled while we wait.
   */

  flags = enter_critical_section();

  /* Loop, testing for an end of transfer condition.  The channel 'result'
   * was set to EBUSY and 'waiter' was set to true before the transfer;
   * 'waiter' will be set to false and 'result' will be set appropriately
   * when the transfer is completed.
   */

  do
    {
      /* Wait for the transfer to complete.  NOTE the transfer may already
       * completed before we get here or the transfer may complete while we
       * wait here.
       */

      ret = nxsem_wait_uninterruptible(&chan->waitsem);
    }
  while (chan->waiter && ret >= 0);

  /* The transfer is complete re-enable interrupts and return the result */

  if (ret >= 0)
    {
      ret = -(int)chan->result;
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: at32_chan_wakeup
 *
 * Description:
 *   A channel transfer has completed... wakeup any threads waiting for the
 *   transfer to complete.
 *
 * Assumptions:
 *   This function is called from the transfer complete interrupt handler for
 *   the channel.  Interrupts are disabled.
 *
 ****************************************************************************/

static void at32_chan_wakeup(struct at32_usbhost_s *priv,
                              struct at32_chan_s *chan)
{
  /* Is the transfer complete? */

  if (chan->result != EBUSY)
    {
      /* Is there a thread waiting for this transfer to complete? */

      if (chan->waiter)
        {
#ifdef CONFIG_USBHOST_ASYNCH
          /* Yes.. there should not also be a callback scheduled */

          DEBUGASSERT(chan->callback == NULL);
#endif
          /* Wake'em up! */

          usbhost_vtrace2(chan->in ? OTGFS_VTRACE2_CHANWAKEUP_IN :
                                     OTGFS_VTRACE2_CHANWAKEUP_OUT,
                          chan->epno, chan->result);

          nxsem_post(&chan->waitsem);
          chan->waiter = false;
        }

#ifdef CONFIG_USBHOST_ASYNCH
      /* No.. is an asynchronous callback expected when the transfer
       * completes?
       */

      else if (chan->callback)
        {
          /* Handle continuation of IN/OUT pipes */

          if (chan->in)
            {
              at32_in_next(priv, chan);
            }
          else
            {
              at32_out_next(priv, chan);
            }
        }
#endif
    }
}

/****************************************************************************
 * Name: at32_ctrlchan_alloc
 *
 * Description:
 *   Allocate and configured channels for a control pipe.
 *
 ****************************************************************************/

static int at32_ctrlchan_alloc(struct at32_usbhost_s *priv,
                                uint8_t epno, uint8_t funcaddr,
                                uint8_t speed,
                                struct at32_ctrlinfo_s *ctrlep)
{
  struct at32_chan_s *chan;
  int inndx;
  int outndx;

  outndx = at32_chan_alloc(priv);
  if (outndx < 0)
    {
      return -ENOMEM;
    }

  ctrlep->outndx  = outndx;
  chan            = &priv->chan[outndx];
  chan->epno      = epno;
  chan->in        = false;
  chan->eptype    = OTGFS_EPTYPE_CTRL;
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->interval  = 0;
  chan->maxpacket = AT32_EP0_DEF_PACKET_SIZE;
  chan->indata1   = false;
  chan->outdata1  = false;

  /* Configure control OUT channels */

  at32_chan_configure(priv, outndx);

  /* Allocate and initialize the control IN channel */

  inndx = at32_chan_alloc(priv);
  if (inndx < 0)
    {
      at32_chan_free(priv, outndx);
      return -ENOMEM;
    }

  ctrlep->inndx   = inndx;
  chan            = &priv->chan[inndx];
  chan->epno      = epno;
  chan->in        = true;
  chan->eptype    = OTGFS_EPTYPE_CTRL;
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->interval  = 0;
  chan->maxpacket = AT32_EP0_DEF_PACKET_SIZE;
  chan->indata1   = false;
  chan->outdata1  = false;

  /* Configure control IN channels */

  at32_chan_configure(priv, inndx);
  return OK;
}

/****************************************************************************
 * Name: at32_ctrlep_alloc
 *
 * Description:
 *   Allocate a container and channels for control pipe.
 *
 * Input Parameters:
 *   priv - The private USB host driver state.
 *   epdesc - Describes the endpoint to be allocated.
 *   ep - A memory location provided by the caller in which to receive the
 *      allocated endpoint descriptor.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_ctrlep_alloc(struct at32_usbhost_s *priv,
                              const struct usbhost_epdesc_s *epdesc,
                              usbhost_ep_t *ep)
{
  struct usbhost_hubport_s *hport;
  struct at32_ctrlinfo_s *ctrlep;
  int ret;

  /* Sanity check.  NOTE that this method should only be called if a device
   * is connected (because we need a valid low speed indication).
   */

  DEBUGASSERT(epdesc->hport != NULL);
  hport = epdesc->hport;

  /* Allocate a container for the control endpoint */

  ctrlep = (struct at32_ctrlinfo_s *)
    kmm_malloc(sizeof(struct at32_ctrlinfo_s));
  if (ctrlep == NULL)
    {
      uerr("ERROR: Failed to allocate control endpoint container\n");
      return -ENOMEM;
    }

  /* Then allocate and configure the IN/OUT channels  */

  ret = at32_ctrlchan_alloc(priv, epdesc->addr & USB_EPNO_MASK,
                             hport->funcaddr, hport->speed, ctrlep);
  if (ret < 0)
    {
      uerr("ERROR: at32_ctrlchan_alloc failed: %d\n", ret);
      kmm_free(ctrlep);
      return ret;
    }

  /* Return a pointer to the control pipe container as the pipe "handle" */

  *ep = (usbhost_ep_t)ctrlep;
  return OK;
}

/****************************************************************************
 * Name: at32_xfrep_alloc
 *
 * Description:
 *   Allocate and configure one unidirectional endpoint.
 *
 * Input Parameters:
 *   priv - The private USB host driver state.
 *   epdesc - Describes the endpoint to be allocated.
 *   ep - A memory location provided by the caller in which to receive the
 *      allocated endpoint descriptor.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_xfrep_alloc(struct at32_usbhost_s *priv,
                             const struct usbhost_epdesc_s *epdesc,
                             usbhost_ep_t *ep)
{
  struct usbhost_hubport_s *hport;
  struct at32_chan_s *chan;
  int chidx;

  /* Sanity check.  NOTE that this method should only be called if a device
   * is connected (because we need a valid low speed indication).
   */

  DEBUGASSERT(epdesc->hport != NULL);
  hport = epdesc->hport;

  /* Allocate a host channel for the endpoint */

  chidx = at32_chan_alloc(priv);
  if (chidx < 0)
    {
      uerr("ERROR: Failed to allocate a host channel\n");
      return -ENOMEM;
    }

  /* Decode the endpoint descriptor to initialize the channel data
   * structures.  Note:  Here we depend on the fact that the endpoint
   * point type is encoded in the same way in the endpoint descriptor as it
   * is in the OTG HS hardware.
   */

  chan            = &priv->chan[chidx];
  chan->epno      = epdesc->addr & USB_EPNO_MASK;
  chan->in        = epdesc->in;
  chan->eptype    = epdesc->xfrtype;
  chan->funcaddr  = hport->funcaddr;
  chan->speed     = hport->speed;
  chan->interval  = epdesc->interval;
  chan->maxpacket = epdesc->mxpacketsize;
  chan->indata1   = false;
  chan->outdata1  = false;

  /* Then configure the endpoint */

  at32_chan_configure(priv, chidx);

  /* Return the index to the allocated channel as the endpoint "handle" */

  *ep = (usbhost_ep_t)chidx;
  return OK;
}

/****************************************************************************
 * Name: at32_transfer_start
 *
 * Description:
 *   Start at transfer on the select IN or OUT channel.
 *
 ****************************************************************************/

static void at32_transfer_start(struct at32_usbhost_s *priv, int chidx)
{
  struct at32_chan_s *chan;
  uint32_t regval;
  unsigned int npackets;
  unsigned int maxpacket;
  unsigned int avail;
  unsigned int wrsize;
  unsigned int minsize;

  /* Set up the initial state of the transfer */

  chan           = &priv->chan[chidx];

  usbhost_vtrace2(OTGFS_VTRACE2_STARTTRANSFER, chidx, chan->buflen);

  chan->result   = EBUSY;
  chan->inflight = 0;
  chan->xfrd     = 0;
  priv->chidx    = chidx;

  /* Compute the expected number of packets associated to the transfer.
   * If the transfer length is zero (or less than the size of one maximum
   * size packet), then one packet is expected.
   */

  /* If the transfer size is greater than one packet, then calculate the
   * number of packets that will be received/sent, including any partial
   * final packet.
   */

  maxpacket = chan->maxpacket;

  if (chan->buflen > maxpacket)
    {
      npackets = (chan->buflen + maxpacket - 1) / maxpacket;

      /* Clip if the buffer length if it exceeds the maximum number of
       * packets that can be transferred (this should not happen).
       */

      if (npackets > AT32_MAX_PKTCOUNT)
        {
          npackets = AT32_MAX_PKTCOUNT;
          chan->buflen = AT32_MAX_PKTCOUNT * maxpacket;
          usbhost_trace2(OTGFS_TRACE2_CLIP, chidx, chan->buflen);
        }
    }
  else
    {
      /* One packet will be sent/received (might be a zero length packet) */

      npackets = 1;
    }

  /* If it is an IN transfer, then adjust the size of the buffer UP to
   * a full number of packets.  Hmmm... couldn't this cause an overrun
   * into unallocated memory?
   */

#if 0 /* Think about this */
  if (chan->in)
    {
      /* Force the buffer length to an even multiple of maxpacket */

      chan->buflen = npackets * maxpacket;
    }
#endif

  /* Save the number of packets in the transfer.  We will need this in
   * order to set the next data toggle correctly when the transfer
   * completes.
   */

  chan->npackets = (uint8_t)npackets;

  /* Setup the HCTSIZn register */

  regval = ((uint32_t)chan->buflen << OTGFS_HCTSIZ_XFRSIZ_SHIFT) |
           ((uint32_t)npackets << OTGFS_HCTSIZ_PKTCNT_SHIFT) |
           ((uint32_t)chan->pid << OTGFS_HCTSIZ_DPID_SHIFT);
  at32_putreg(AT32_OTGFS_HCTSIZ(chidx), regval);

  /* Setup the HCCHAR register: Frame oddness and host channel enable */

  regval = at32_getreg(AT32_OTGFS_HCCHAR(chidx));

  /* Set/clear the Odd Frame bit.  Check for an even frame; if so set Odd
   * Frame. This field is applicable for only periodic (isochronous and
   * interrupt) channels.
   */

  if ((at32_getreg(AT32_OTGFS_HFNUM) & 1) == 0)
    {
      regval |= OTGFS_HCCHAR_ODDFRM;
    }
  else
    {
      regval &= ~OTGFS_HCCHAR_ODDFRM;
    }

  regval &= ~OTGFS_HCCHAR_CHDIS;
  regval |= OTGFS_HCCHAR_CHENA;
  at32_putreg(AT32_OTGFS_HCCHAR(chidx), regval);

  /* If this is an out transfer, then we need to do more.. we need to copy
   * the outgoing data into the correct TxFIFO.
   */

  if (!chan->in && chan->buflen > 0)
    {
      /* Handle non-periodic (CTRL and BULK) OUT transfers differently than
       * periodic (INTR and ISOC) OUT transfers.
       */

      minsize = MIN(chan->buflen, chan->maxpacket);

      switch (chan->eptype)
        {
        case OTGFS_EPTYPE_CTRL: /* Non periodic transfer */
        case OTGFS_EPTYPE_BULK:
          {
            /* Read the Non-periodic Tx FIFO status register */

            regval = at32_getreg(AT32_OTGFS_HNPTXSTS);
            avail  = ((regval & OTGFS_HNPTXSTS_NPTXFSAV_MASK) >>
                      OTGFS_HNPTXSTS_NPTXFSAV_SHIFT) << 2;
          }
          break;

        /* Periodic transfer */

        case OTGFS_EPTYPE_INTR:
        case OTGFS_EPTYPE_ISOC:
          {
            /* Read the Non-periodic Tx FIFO status register */

            regval = at32_getreg(AT32_OTGFS_HPTXSTS);
            avail  = ((regval & OTGFS_HPTXSTS_PTXFSAVL_MASK) >>
                      OTGFS_HPTXSTS_PTXFSAVL_SHIFT) << 2;
          }
          break;

        default:
          DEBUGPANIC();
          return;
        }

      /* Is there space in the TxFIFO to hold the minimum size packet? */

      if (minsize <= avail)
        {
          /* Yes.. Get the size of the biggest thing that we can put
           * in the Tx FIFO now
           */

          wrsize = chan->buflen;
          if (wrsize > avail)
            {
              /* Clip the write size to the number of full, max sized packets
               * that will fit in the Tx FIFO.
               */

              unsigned int wrpackets = avail / chan->maxpacket;
              wrsize = wrpackets * chan->maxpacket;
            }

          /* Write packet into the Tx FIFO. */

          at32_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
        }

      /* Did we put the entire buffer into the Tx FIFO? */

      if (chan->buflen > avail)
        {
          /* No, there was insufficient space to hold the entire transfer ...
           * Enable the Tx FIFO interrupt to handle the transfer when the Tx
           * FIFO becomes empty.
           */

           at32_txfe_enable(priv, chidx);
        }
    }
}

/****************************************************************************
 * Name: at32_getframe
 *
 * Description:
 *   Get the current frame number.  The frame number (FRNUM) field increments
 *   when a new SOF is transmitted on the USB, and is cleared to 0 when it
 *   reaches 0x3fff.
 *
 ****************************************************************************/

#if 0 /* Not used */
static inline uint16_t at32_getframe(void)
{
  return (uint16_t)
    (at32_getreg(AT32_OTGFS_HFNUM) & OTGFS_HFNUM_FRNUM_MASK);
}
#endif

/****************************************************************************
 * Name: at32_ctrl_sendsetup
 *
 * Description:
 *   Send an IN/OUT SETUP packet.
 *
 ****************************************************************************/

static int at32_ctrl_sendsetup(struct at32_usbhost_s *priv,
                                struct at32_ctrlinfo_s *ep0,
                                const struct usb_ctrlreq_s *req)
{
  struct at32_chan_s *chan;
  clock_t start;
  clock_t elapsed;
  int ret;

  /* Loop while the device reports NAK (and a timeout is not exceeded */

  chan  = &priv->chan[ep0->outndx];
  start = clock_systime_ticks();

  do
    {
      /* Send the  SETUP packet */

      chan->pid    = OTGFS_PID_SETUP;
      chan->buffer = (uint8_t *)req;
      chan->buflen = USB_SIZEOF_CTRLREQ;
      chan->xfrd   = 0;

      /* Set up for the wait BEFORE starting the transfer */

      ret = at32_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
          return ret;
        }

      /* Start the transfer */

      at32_transfer_start(priv, ep0->outndx);

      /* Wait for the transfer to complete */

      ret = at32_chan_wait(priv, chan);

      /* Return on success and for all failures other than EAGAIN.  EAGAIN
       * means that the device NAKed the SETUP command and that we should
       * try a few more times.
       */

      if (ret != -EAGAIN)
        {
          /* Output some debug information if the transfer failed */

          if (ret < 0)
            {
              usbhost_trace1(OTGFS_TRACE1_TRNSFRFAILED, ret);
            }

          /* Return the result in any event */

          return ret;
        }

      /* Get the elapsed time (in frames) */

      elapsed = clock_systime_ticks() - start;
    }
  while (elapsed < AT32_SETUP_DELAY);

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: at32_ctrl_senddata
 *
 * Description:
 *   Send data in the data phase of an OUT control transfer.  Or send status
 *   in the status phase of an IN control transfer
 *
 ****************************************************************************/

static int at32_ctrl_senddata(struct at32_usbhost_s *priv,
                               struct at32_ctrlinfo_s *ep0,
                               uint8_t *buffer, unsigned int buflen)
{
  struct at32_chan_s *chan = &priv->chan[ep0->outndx];
  int ret;

  /* Save buffer information */

  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  /* Set the DATA PID */

  if (buflen == 0)
    {
      /* For status OUT stage with buflen == 0, set PID DATA1 */

      chan->outdata1 = true;
    }

  /* Set the Data PID as per the outdata1 boolean */

  chan->pid = chan->outdata1 ? OTGFS_PID_DATA1 : OTGFS_PID_DATA0;

  /* Set up for the wait BEFORE starting the transfer */

  ret = at32_chan_waitsetup(priv, chan);
  if (ret < 0)
    {
      usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
      return ret;
    }

  /* Start the transfer */

  at32_transfer_start(priv, ep0->outndx);

  /* Wait for the transfer to complete and return the result */

  return at32_chan_wait(priv, chan);
}

/****************************************************************************
 * Name: at32_ctrl_recvdata
 *
 * Description:
 *   Receive data in the data phase of an IN control transfer.  Or receive
 *   status in the status phase of an OUT control transfer
 *
 ****************************************************************************/

static int at32_ctrl_recvdata(struct at32_usbhost_s *priv,
                               struct at32_ctrlinfo_s *ep0,
                               uint8_t *buffer, unsigned int buflen)
{
  struct at32_chan_s *chan = &priv->chan[ep0->inndx];
  int ret;

  /* Save buffer information */

  chan->pid    = OTGFS_PID_DATA1;
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  /* Set up for the wait BEFORE starting the transfer */

  ret = at32_chan_waitsetup(priv, chan);
  if (ret < 0)
    {
      usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
      return ret;
    }

  /* Start the transfer */

  at32_transfer_start(priv, ep0->inndx);

  /* Wait for the transfer to complete and return the result */

  return at32_chan_wait(priv, chan);
}

/****************************************************************************
 * Name: at32_in_setup
 *
 * Description:
 *   Initiate an IN transfer on an bulk, interrupt, or isochronous pipe.
 *
 ****************************************************************************/

static int at32_in_setup(struct at32_usbhost_s *priv, int chidx)
{
  struct at32_chan_s *chan;

  /* Set up for the transfer based on the direction and the endpoint type */

  chan = &priv->chan[chidx];
  switch (chan->eptype)
    {
    default:
    case OTGFS_EPTYPE_CTRL: /* Control */
      {
        /* This kind of transfer on control endpoints other than EP0 are not
         * currently supported
         */

        return -ENOSYS;
      }

    case OTGFS_EPTYPE_ISOC: /* Isochronous */
      {
        /* Set up the IN data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_ISOCIN, chidx, chan->buflen);
        chan->pid = OTGFS_PID_DATA0;
      }
      break;

    case OTGFS_EPTYPE_BULK: /* Bulk */
      {
        /* Setup the IN data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_BULKIN, chidx, chan->buflen);
        chan->pid = chan->indata1 ? OTGFS_PID_DATA1 : OTGFS_PID_DATA0;
      }
      break;

    case OTGFS_EPTYPE_INTR: /* Interrupt */
      {
        /* Setup the IN data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_INTRIN, chidx, chan->buflen);
        chan->pid = chan->indata1 ? OTGFS_PID_DATA1 : OTGFS_PID_DATA0;
      }
      break;
    }

  /* Start the transfer */

  at32_transfer_start(priv, chidx);
  return OK;
}

/****************************************************************************
 * Name: at32_in_transfer
 *
 * Description:
 *   Transfer 'buflen' bytes into 'buffer' from an IN channel.
 *
 ****************************************************************************/

static ssize_t at32_in_transfer(struct at32_usbhost_s *priv, int chidx,
                                 uint8_t *buffer, size_t buflen)
{
  struct at32_chan_s *chan;
  clock_t start;
  ssize_t xfrd;
  int ret;

  /* Loop until the transfer completes (i.e., buflen is decremented to zero)
   * or a fatal error occurs any error other than a simple NAK.  NAK would
   * simply indicate the end of the transfer (short-transfer).
   */

  chan         = &priv->chan[chidx];
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;
  xfrd         = 0;

  start = clock_systime_ticks();
  while (chan->xfrd < chan->buflen)
    {
      /* Set up for the wait BEFORE starting the transfer */

      ret = at32_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
          return (ssize_t)ret;
        }

      /* Set up for the transfer based on the direction and the endpoint */

      ret = at32_in_setup(priv, chidx);
      if (ret < 0)
        {
          uerr("ERROR: at32_in_setup failed: %d\n", ret);
          return (ssize_t)ret;
        }

      /* Wait for the transfer to complete and get the result */

      ret = at32_chan_wait(priv, chan);

      /* EAGAIN indicates that the device NAKed the transfer. */

      if (ret < 0)
        {
          /* The transfer failed.  If we received a NAK, return all data
           * buffered so far (if any).
           */

          if (ret == -EAGAIN)
            {
              /* Was data buffered prior to the NAK? */

              if (xfrd > 0)
                {
                  /* Yes, return the amount of data received.
                   *
                   * REVISIT: This behavior is clearly correct for CDC/ACM
                   * bulk transfers and HID interrupt transfers.  But I am
                   * not so certain for MSC bulk transfers which, I think,
                   * could have NAKed packets in the middle of a transfer.
                   */

                  return xfrd;
                }
              else
                {
                  useconds_t delay;

                  /* Get the elapsed time.  Has the timeout elapsed?
                   * if not then try again.
                   */

                  clock_t elapsed = clock_systime_ticks() - start;
                  if (elapsed >= AT32_DATANAK_DELAY)
                    {
                      /* Timeout out... break out returning the NAK as
                       * as a failure.
                       */

                      return (ssize_t)ret;
                    }

                  /* Wait a bit before retrying after a NAK. */

                  if (chan->eptype == OTGFS_EPTYPE_INTR)
                    {
                      /* For interrupt (and isochronous) endpoints, the
                       * polling rate is determined by the bInterval field
                       * of the endpoint descriptor (in units of frames
                       * which we treat as milliseconds here).
                       */

                      if (chan->interval > 0)
                        {
                          /* Convert the delay to units of microseconds */

                          delay = (useconds_t)chan->interval * 1000;
                        }
                      else
                        {
                          /* Out of range! For interrupt endpoints, the valid
                           * range is 1-255 frames.  Assume one frame.
                           */

                          delay = 1000;
                        }
                    }
                  else
                    {
                      /* For Isochronous endpoints, bInterval must be 1.
                       * Bulk endpoints do not have a polling interval.
                       * Rather, the should wait until data is received.
                       *
                       * REVISIT:  For bulk endpoints this 1 msec delay is
                       * only intended to give the CPU a break from the bulk
                       * EP tight polling loop.  But are there performance
                       * issues?
                       */

                      delay = 1000;
                    }

                  /* Wait for the next polling interval.  For interrupt and
                   * isochronous endpoints, this is necessary to assure the
                   * polling interval.  It is used in other cases only to
                   * prevent the polling from consuming too much CPU
                   * bandwidth.
                   *
                   * Small delays could require more resolution than is
                   * provided by the system timer.  For example, if the
                   * system timer resolution is 10MS, then
                   * nxsig_usleep(1000) will actually request a delay 20MS
                   * (due to both quantization and rounding).
                   *
                   * REVISIT: So which is better?  To ignore tiny delays and
                   * hog the system bandwidth?  Or to wait for an excessive
                   * amount and destroy system throughput?
                   */

                  if (delay > CONFIG_USEC_PER_TICK)
                    {
                      nxsig_usleep(delay - CONFIG_USEC_PER_TICK);
                    }
                }
            }
          else
            {
              /* Some unexpected, fatal error occurred. */

              usbhost_trace1(OTGFS_TRACE1_TRNSFRFAILED, ret);

              /* Break out and return the error */

              uerr("ERROR: at32_chan_wait failed: %d\n", ret);
              return (ssize_t)ret;
            }
        }
      else
        {
          /* Successfully received another chunk of data... add that to the
           * running total.  Then continue reading until we read 'buflen'
           * bytes of data or until the devices NAKs (implying a short
           * packet).
           */

          xfrd += chan->xfrd;
        }
    }

  return xfrd;
}

/****************************************************************************
 * Name: at32_in_next
 *
 * Description:
 *   Initiate the next of a sequence of asynchronous transfers.
 *
 * Assumptions:
 *   This function is always called from an interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static void at32_in_next(struct at32_usbhost_s *priv,
                          struct at32_chan_s *chan)
{
  usbhost_asynch_t callback;
  void *arg;
  ssize_t nbytes;
  int result;
  int ret;

  /* Is the full transfer complete? Did the last chunk transfer OK? */

  result = -(int)chan->result;
  if (chan->xfrd < chan->buflen && result == OK)
    {
      /* Yes.. Set up for the next transfer based on the direction and the
       * endpoint type
       */

      ret = at32_in_setup(priv, chan->chidx);
      if (ret >= 0)
        {
          return;
        }

      uerr("ERROR: at32_in_setup failed: %d\n", ret);
      result = ret;
    }

  /* The transfer is complete, with or without an error */

  uinfo("Transfer complete:  %d\n", result);

  /* Extract the callback information */

  callback       = chan->callback;
  arg            = chan->arg;
  nbytes         = chan->xfrd;

  chan->callback = NULL;
  chan->arg      = NULL;
  chan->xfrd     = 0;

  /* Then perform the callback */

  if (result < 0)
    {
      nbytes = (ssize_t)result;
    }

  callback(arg, nbytes);
}
#endif

/****************************************************************************
 * Name: at32_in_asynch
 *
 * Description:
 *   Initiate the first of a sequence of asynchronous transfers.
 *
 * Assumptions:
 *   This function is never called from an interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static int at32_in_asynch(struct at32_usbhost_s *priv, int chidx,
                           uint8_t *buffer, size_t buflen,
                           usbhost_asynch_t callback, void *arg)
{
  struct at32_chan_s *chan;
  int ret;

  /* Set up for the transfer BEFORE starting the first transfer */

  chan         = &priv->chan[chidx];
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  ret = at32_chan_asynchsetup(priv, chan, callback, arg);
  if (ret < 0)
    {
      uerr("ERROR: at32_chan_asynchsetup failed: %d\n", ret);
      return ret;
    }

  /* Set up for the transfer based on the direction and the endpoint type */

  ret = at32_in_setup(priv, chidx);
  if (ret < 0)
    {
      uerr("ERROR: at32_in_setup failed: %d\n", ret);
    }

  /* And return with the transfer pending */

  return ret;
}
#endif

/****************************************************************************
 * Name: at32_out_setup
 *
 * Description:
 *   Initiate an OUT transfer on an bulk, interrupt, or isochronous pipe.
 *
 ****************************************************************************/

static int at32_out_setup(struct at32_usbhost_s *priv, int chidx)
{
  struct at32_chan_s *chan;

  /* Set up for the transfer based on the direction and the endpoint type */

  chan = &priv->chan[chidx];
  switch (chan->eptype)
    {
    default:
    case OTGFS_EPTYPE_CTRL: /* Control */
      {
        /* This kind of transfer on control endpoints other than EP0 are not
         * currently supported
         */

        return -ENOSYS;
      }

    case OTGFS_EPTYPE_ISOC: /* Isochronous */
      {
        /* Set up the OUT data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_ISOCOUT, chidx, chan->buflen);
        chan->pid = OTGFS_PID_DATA0;
      }
      break;

    case OTGFS_EPTYPE_BULK: /* Bulk */
      {
        /* Setup the OUT data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_BULKOUT, chidx, chan->buflen);
        chan->pid = chan->outdata1 ? OTGFS_PID_DATA1 : OTGFS_PID_DATA0;
      }
      break;

    case OTGFS_EPTYPE_INTR: /* Interrupt */
      {
        /* Setup the OUT data PID */

        usbhost_vtrace2(OTGFS_VTRACE2_INTROUT, chidx, chan->buflen);
        chan->pid = chan->outdata1 ? OTGFS_PID_DATA1 : OTGFS_PID_DATA0;

        /* Toggle the OUT data PID for the next transfer */

        chan->outdata1 ^= true;
      }
      break;
    }

  /* Start the transfer */

  at32_transfer_start(priv, chidx);
  return OK;
}

/****************************************************************************
 * Name: at32_out_transfer
 *
 * Description:
 *   Transfer the 'buflen' bytes in 'buffer' through an OUT channel.
 *
 ****************************************************************************/

static ssize_t at32_out_transfer(struct at32_usbhost_s *priv,
                                  int chidx, uint8_t *buffer,
                                  size_t buflen)
{
  struct at32_chan_s *chan;
  clock_t start;
  clock_t elapsed;
  size_t xfrlen;
  ssize_t xfrd;
  int ret;
  bool zlp;

  /* Loop until the transfer completes (i.e., buflen is decremented to zero)
   * or a fatal error occurs (any error other than a simple NAK)
   */

  chan  = &priv->chan[chidx];
  start = clock_systime_ticks();
  xfrd  = 0;
  zlp   = (buflen == 0);

  while (buflen > 0 || zlp)
    {
      /* Transfer one packet at a time.  The hardware is capable of queueing
       * multiple OUT packets, but I just haven't figured out how to handle
       * the case where a single OUT packet in the group is NAKed.
       */

      xfrlen       = MIN(chan->maxpacket, buflen);
      chan->buffer = buffer;
      chan->buflen = xfrlen;
      chan->xfrd   = 0;

      /* Set up for the wait BEFORE starting the transfer */

      ret = at32_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
          return (ssize_t)ret;
        }

      /* Set up for the transfer based on the direction and the endpoint */

      ret = at32_out_setup(priv, chidx);
      if (ret < 0)
        {
          uerr("ERROR: at32_out_setup failed: %d\n", ret);
          return (ssize_t)ret;
        }

      /* Wait for the transfer to complete and get the result */

      ret = at32_chan_wait(priv, chan);

      /* Handle transfer failures */

      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_TRNSFRFAILED, ret);

          /* Check for a special case:  If (1) the transfer was NAKed and (2)
           * no Tx FIFO empty or Rx FIFO not-empty event occurred, then we
           * should be able to just flush the Rx and Tx FIFOs and try again.
           * We can detect this latter case because then the transfer buffer
           * pointer and buffer size will be unaltered.
           */

          elapsed = clock_systime_ticks() - start;
          if (ret != -EAGAIN ||                  /* Not a NAK condition OR */
              elapsed >= AT32_DATANAK_DELAY ||   /* Timeout has elapsed OR */
              chan->xfrd > 0)                    /* Data has been partially transferred */
            {
              /* Break out and return the error */

              uerr("ERROR: at32_chan_wait failed: %d\n", ret);
              return (ssize_t)ret;
            }

          /* Is this flush really necessary? What does the hardware do with
           * the data in the FIFO when the NAK occurs?  Does it discard it?
           */

          at32_flush_txfifos(OTGFS_GRSTCTL_TXFNUM_HALL);

          /* Get the device a little time to catch up.  Then retry the
           * transfer using the same buffer pointer and length.
           */

          nxsig_usleep(5 * 1000);
        }
      else
        {
          /* Successfully transferred.  Update the buffer pointer/length */

          buffer += xfrlen;
          buflen -= xfrlen;
          xfrd   += chan->xfrd;
          zlp     = false;
        }
    }

  return xfrd;
}

/****************************************************************************
 * Name: at32_out_next
 *
 * Description:
 *   Initiate the next of a sequence of asynchronous transfers.
 *
 * Assumptions:
 *   This function is always called from an interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static void at32_out_next(struct at32_usbhost_s *priv,
                           struct at32_chan_s *chan)
{
  usbhost_asynch_t callback;
  void *arg;
  ssize_t nbytes;
  int result;
  int ret;

  /* Is the full transfer complete? Did the last chunk transfer OK? */

  result = -(int)chan->result;
  if (chan->xfrd < chan->buflen && result == OK)
    {
      /* Yes.. Set up for the next transfer based on the direction and the
       * endpoint type
       */

      ret = at32_out_setup(priv, chan->chidx);
      if (ret >= 0)
        {
          return;
        }

      uerr("ERROR: at32_out_setup failed: %d\n", ret);
      result = ret;
    }

  /* The transfer is complete, with or without an error */

  uinfo("Transfer complete:  %d\n", result);

  /* Extract the callback information */

  callback       = chan->callback;
  arg            = chan->arg;
  nbytes         = chan->xfrd;

  chan->callback = NULL;
  chan->arg      = NULL;
  chan->xfrd     = 0;

  /* Then perform the callback */

  if (result < 0)
    {
      nbytes = (ssize_t)result;
    }

  callback(arg, nbytes);
}
#endif

/****************************************************************************
 * Name: at32_out_asynch
 *
 * Description:
 *   Initiate the first of a sequence of asynchronous transfers.
 *
 * Assumptions:
 *   This function is never called from an interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static int at32_out_asynch(struct at32_usbhost_s *priv, int chidx,
                            uint8_t *buffer, size_t buflen,
                            usbhost_asynch_t callback, void *arg)
{
  struct at32_chan_s *chan;
  int ret;

  /* Set up for the transfer BEFORE starting the first transfer */

  chan         = &priv->chan[chidx];
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  ret = at32_chan_asynchsetup(priv, chan, callback, arg);
  if (ret < 0)
    {
      uerr("ERROR: at32_chan_asynchsetup failed: %d\n", ret);
      return ret;
    }

  /* Set up for the transfer based on the direction and the endpoint type */

  ret = at32_out_setup(priv, chidx);
  if (ret < 0)
    {
      uerr("ERROR: at32_out_setup failed: %d\n", ret);
    }

  /* And return with the transfer pending */

  return ret;
}
#endif

/****************************************************************************
 * Name: at32_gint_wrpacket
 *
 * Description:
 *   Transfer the 'buflen' bytes in 'buffer' to the Tx FIFO associated with
 *   'chidx' (non-DMA).
 *
 ****************************************************************************/

static void at32_gint_wrpacket(struct at32_usbhost_s *priv,
                                uint8_t *buffer, int chidx, int buflen)
{
  uint32_t *src;
  uint32_t fifo;
  int buflen32;

  at32_pktdump("Sending", buffer, buflen);

  /* Get the number of 32-byte words associated with this byte size */

  buflen32 = (buflen + 3) >> 2;

  /* Get the address of the Tx FIFO associated with this channel */

  fifo = AT32_OTGFS_DFIFO_HCH(chidx);

  /* Transfer all of the data into the Tx FIFO */

  src = (uint32_t *)buffer;
  for (; buflen32 > 0; buflen32--)
    {
      uint32_t data = *src++;
      at32_putreg(fifo, data);
    }

  /* Increment the count of bytes "in-flight" in the Tx FIFO */

  priv->chan[chidx].inflight += buflen;
}

/****************************************************************************
 * Name: at32_gint_hcinisr
 *
 * Description:
 *   USB OTG FS host IN channels interrupt handler
 *
 *   One the completion of the transfer, the channel result byte may be set
 *   as follows:
 *
 *     OK     - Transfer completed successfully
 *     EAGAIN - If devices NAKs the transfer or NYET occurs
 *     EPERM  - If the endpoint stalls
 *     EIO    - On a TX or data toggle error
 *     EPIPE  - Frame overrun
 *
 *   EBUSY in the result field indicates that the transfer has not completed.
 *
 ****************************************************************************/

static inline void at32_gint_hcinisr(struct at32_usbhost_s *priv,
                                      int chidx)
{
  struct at32_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;
  uint32_t pending;

  /* Read the HCINT register to get the pending HC interrupts.  Read the
   * HCINTMSK register to get the set of enabled HC interrupts.
   */

  pending = at32_getreg(AT32_OTGFS_HCINT(chidx));
  regval  = at32_getreg(AT32_OTGFS_HCINTMSK(chidx));

  /* AND the two to get the set of enabled, pending HC interrupts */

  pending &= regval;
  uinfo("HCINTMSK%d: %08" PRIx32 " pending: %08" PRIx32 "\n",
        chidx, regval, pending);

  /* Check for a pending ACK response received/transmitted interrupt */

  if ((pending & OTGFS_HCINT_ACK) != 0)
    {
      /* Clear the pending the ACK response received/transmitted interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_ACK);
    }

  /* Check for a pending STALL response receive (STALL) interrupt */

  else if ((pending & OTGFS_HCINT_STALL) != 0)
    {
      /* Clear the NAK and STALL Conditions. */

      at32_putreg(AT32_OTGFS_HCINT(chidx),
                   OTGFS_HCINT_NAK | OTGFS_HCINT_STALL);

      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_STALL);

      /* When there is a STALL, clear any pending NAK so that it is not
       * processed below.
       */

      pending &= ~OTGFS_HCINT_NAK;
    }

  /* Check for a pending Data Toggle ERRor (DTERR) interrupt */

  else if ((pending & OTGFS_HCINT_DTERR) != 0)
    {
      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_DTERR);

      /* Clear the NAK and data toggle error conditions */

      at32_putreg(AT32_OTGFS_HCINT(chidx),
                   OTGFS_HCINT_NAK | OTGFS_HCINT_DTERR);
    }

  /* Check for a pending FRaMe OverRun (FRMOR) interrupt */

  if ((pending & OTGFS_HCINT_FRMOR) != 0)
    {
      /* Halt the channel -- the CHH interrupt is expected next */

      at32_chan_halt(priv, chidx, CHREASON_FRMOR);

      /* Clear the FRaMe OverRun (FRMOR) condition */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_FRMOR);
    }

  /* Check for a pending TransFeR Completed (XFRC) interrupt */

  else if ((pending & OTGFS_HCINT_XFRC) != 0)
    {
      /* Clear the TransFeR Completed (XFRC) condition */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_XFRC);

      /* Then handle the transfer completion event based on the endpoint */

      if (chan->eptype == OTGFS_EPTYPE_CTRL ||
          chan->eptype == OTGFS_EPTYPE_BULK)
        {
          /* Halt the channel -- the CHH interrupt is expected next */

          at32_chan_halt(priv, chidx, CHREASON_XFRC);

          /* Clear any pending NAK condition.  The 'indata1' data toggle
           * should have been appropriately updated by the RxFIFO
           * logic as each packet was received.
           */

          at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_NAK);
        }
      else if (chan->eptype == OTGFS_EPTYPE_INTR)
        {
          /* Force the next transfer on an ODD frame */

          regval = at32_getreg(AT32_OTGFS_HCCHAR(chidx));
          regval |= OTGFS_HCCHAR_ODDFRM;
          at32_putreg(AT32_OTGFS_HCCHAR(chidx), regval);

          /* Set the request done state */

          chan->result = OK;
        }
    }

  /* Check for a pending CHannel Halted (CHH) interrupt */

  else if ((pending & OTGFS_HCINT_CHH) != 0)
    {
      /* Mask the CHannel Halted (CHH) interrupt */

      regval  = at32_getreg(AT32_OTGFS_HCINTMSK(chidx));
      regval &= ~OTGFS_HCINT_CHH;
      at32_putreg(AT32_OTGFS_HCINTMSK(chidx), regval);

      /* Update the request state based on the host state machine state */

      if (chan->chreason == CHREASON_XFRC)
        {
          /* Set the request done result */

          chan->result = OK;
        }
      else if (chan->chreason == CHREASON_STALL)
        {
          /* Set the request stall result */

          chan->result = EPERM;
        }
      else if ((chan->chreason == CHREASON_TXERR) ||
               (chan->chreason == CHREASON_DTERR))
        {
          /* Set the request I/O error result */

          chan->result = EIO;
        }
      else if (chan->chreason == CHREASON_NAK)
        {
          /* Set the NAK error result */

          chan->result = EAGAIN;
        }
      else /* if (chan->chreason == CHREASON_FRMOR) */
        {
          /* Set the frame overrun error result */

          chan->result = EPIPE;
        }

      /* Clear the CHannel Halted (CHH) condition */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_CHH);
    }

  /* Check for a pending Transaction ERror (TXERR) interrupt */

  else if ((pending & OTGFS_HCINT_TXERR) != 0)
    {
      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_TXERR);

      /* Clear the Transaction ERror (TXERR) condition */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_TXERR);
    }

  /* Check for a pending NAK response received (NAK) interrupt */

  else if ((pending & OTGFS_HCINT_NAK) != 0)
    {
      /* For a BULK transfer, the hardware is capable of retrying
       * automatically on a NAK.  However, this is not always
       * what we need to do.  So we always halt the transfer and
       * return control to high level logic in the event of a NAK.
       */

#if 1
      /* Halt the interrupt channel */

      if (chan->eptype == OTGFS_EPTYPE_INTR ||
          chan->eptype == OTGFS_EPTYPE_BULK)
        {
          /* Halt the channel -- the CHH interrupt is expected next */

          at32_chan_halt(priv, chidx, CHREASON_NAK);
        }

      /* Re-activate CTRL and BULK channels.
       *
       * REVISIT:  This can cause a lot of interrupts!
       * REVISIT:  BULK endpoints are not re-activated.
       */

      else if (chan->eptype == OTGFS_EPTYPE_CTRL)
        {
          /* Re-activate the channel by clearing CHDIS and assuring that
           * CHENA is set
           *
           * TODO: set channel reason to NACK?
           */

          regval  = at32_getreg(AT32_OTGFS_HCCHAR(chidx));
          regval |= OTGFS_HCCHAR_CHENA;
          regval &= ~OTGFS_HCCHAR_CHDIS;
          at32_putreg(AT32_OTGFS_HCCHAR(chidx), regval);
        }

#else
      /* Halt all transfers on the NAK -- CHH interrupt is expected next */

      at32_chan_halt(priv, chidx, CHREASON_NAK);
#endif

      /* Clear the NAK condition */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_NAK);
    }

  /* Check for a transfer complete event */

  at32_chan_wakeup(priv, chan);
}

/****************************************************************************
 * Name: at32_gint_hcoutisr
 *
 * Description:
 *   USB OTG FS host OUT channels interrupt handler
 *
 *   One the completion of the transfer, the channel result byte may be set
 *   as follows:
 *
 *     OK     - Transfer completed successfully
 *     EAGAIN - If devices NAKs the transfer or NYET occurs
 *     EPERM  - If the endpoint stalls
 *     EIO    - On a TX or data toggle error
 *     EPIPE  - Frame overrun
 *
 *   EBUSY in the result field indicates that the transfer has not completed.
 *
 ****************************************************************************/

static inline void at32_gint_hcoutisr(struct at32_usbhost_s *priv,
                                       int chidx)
{
  struct at32_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;
  uint32_t pending;

  /* Read the HCINT register to get the pending HC interrupts.  Read the
   * HCINTMSK register to get the set of enabled HC interrupts.
   */

  pending = at32_getreg(AT32_OTGFS_HCINT(chidx));
  regval  = at32_getreg(AT32_OTGFS_HCINTMSK(chidx));

  /* AND the two to get the set of enabled, pending HC interrupts */

  pending &= regval;
  uinfo("HCINTMSK%d: %08" PRIx32 " pending: %08" PRIx32 "\n",
        chidx, regval, pending);

  /* Check for a pending ACK response received/transmitted interrupt */

  if ((pending & OTGFS_HCINT_ACK) != 0)
    {
      /* Clear the pending the ACK response received/transmitted interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_ACK);
    }

  /* Check for a pending FRaMe OverRun (FRMOR) interrupt */

  else if ((pending & OTGFS_HCINT_FRMOR) != 0)
    {
      /* Halt the channel (probably not necessary for FRMOR) */

      at32_chan_halt(priv, chidx, CHREASON_FRMOR);

      /* Clear the pending the FRaMe OverRun (FRMOR) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_FRMOR);
    }

  /* Check for a pending TransFeR Completed (XFRC) interrupt */

  else if ((pending & OTGFS_HCINT_XFRC) != 0)
    {
      /* Decrement the number of bytes remaining by the number of
       * bytes that were "in-flight".
       */

      priv->chan[chidx].buffer  += priv->chan[chidx].inflight;
      priv->chan[chidx].xfrd    += priv->chan[chidx].inflight;
      priv->chan[chidx].inflight = 0;

      /* Halt the channel -- the CHH interrupt is expected next */

      at32_chan_halt(priv, chidx, CHREASON_XFRC);

      /* Clear the pending the TransFeR Completed (XFRC) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_XFRC);
    }

  /* Check for a pending STALL response receive (STALL) interrupt */

  else if ((pending & OTGFS_HCINT_STALL) != 0)
    {
      /* Clear the pending the STALL response receive (STALL) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_STALL);

      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_STALL);
    }

  /* Check for a pending NAK response received (NAK) interrupt */

  else if ((pending & OTGFS_HCINT_NAK) != 0)
    {
      /* Halt the channel  -- the CHH interrupt is expected next */

      at32_chan_halt(priv, chidx, CHREASON_NAK);

      /* Clear the pending the NAK response received (NAK) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_NAK);
    }

  /* Check for a pending Transaction ERror (TXERR) interrupt */

  else if ((pending & OTGFS_HCINT_TXERR) != 0)
    {
      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_TXERR);

      /* Clear the pending the Transaction ERror (TXERR) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_TXERR);
    }

  /* Check for a NYET interrupt */

#if 0 /* NYET is a reserved bit in the HCINT register */
  else if ((pending & OTGFS_HCINT_NYET) != 0)
    {
      /* Halt the channel */

      at32_chan_halt(priv, chidx, CHREASON_NYET);

      /* Clear the pending the NYET interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_NYET);
    }
#endif

  /* Check for a pending Data Toggle ERRor (DTERR) interrupt */

  else if (pending & OTGFS_HCINT_DTERR)
    {
      /* Halt the channel when a STALL, TXERR, BBERR or DTERR interrupt is
       * received on the channel.
       */

      at32_chan_halt(priv, chidx, CHREASON_DTERR);

      /* Clear the pending the Data Toggle ERRor (DTERR) and NAK interrupts */

      at32_putreg(AT32_OTGFS_HCINT(chidx),
                   OTGFS_HCINT_DTERR | OTGFS_HCINT_NAK);
    }

  /* Check for a pending CHannel Halted (CHH) interrupt */

  else if ((pending & OTGFS_HCINT_CHH) != 0)
    {
      /* Mask the CHannel Halted (CHH) interrupt */

      regval  = at32_getreg(AT32_OTGFS_HCINTMSK(chidx));
      regval &= ~OTGFS_HCINT_CHH;
      at32_putreg(AT32_OTGFS_HCINTMSK(chidx), regval);

      if (chan->chreason == CHREASON_XFRC)
        {
          /* Set the request done result */

          chan->result = OK;

          /* Read the HCCHAR register to get the HCCHAR register to get
           * the endpoint type.
           */

          regval = at32_getreg(AT32_OTGFS_HCCHAR(chidx));

          /* Is it a bulk endpoint?  Were an odd number of packets
           * transferred?
           */

          if ((regval & OTGFS_HCCHAR_EPTYP_MASK) ==
               OTGFS_HCCHAR_EPTYP_BULK &&
              (chan->npackets & 1) != 0)
            {
              /* Yes to both... toggle the data out PID */

              chan->outdata1 ^= true;
            }
        }
      else if (chan->chreason == CHREASON_NAK ||
               chan->chreason == CHREASON_NYET)
        {
          /* Set the try again later result */

          chan->result = EAGAIN;
        }
      else if (chan->chreason == CHREASON_STALL)
        {
          /* Set the request stall result */

          chan->result = EPERM;
        }
      else if ((chan->chreason == CHREASON_TXERR) ||
               (chan->chreason == CHREASON_DTERR))
        {
          /* Set the I/O failure result */

          chan->result = EIO;
        }
      else /* if (chan->chreason == CHREASON_FRMOR) */
        {
          /* Set the frame error result */

          chan->result = EPIPE;
        }

      /* Clear the pending the CHannel Halted (CHH) interrupt */

      at32_putreg(AT32_OTGFS_HCINT(chidx), OTGFS_HCINT_CHH);
    }

  /* Check for a transfer complete event */

  at32_chan_wakeup(priv, chan);
}

/****************************************************************************
 * Name: at32_gint_connected
 *
 * Description:
 *   Handle a connection event.
 *
 ****************************************************************************/

static void at32_gint_connected(struct at32_usbhost_s *priv)
{
  /* We we previously disconnected? */

  if (!priv->connected)
    {
      /* Yes.. then now we are connected */

      usbhost_vtrace1(OTGFS_VTRACE1_CONNECTED, 0);
      priv->connected = true;
      priv->change    = true;
      DEBUGASSERT(priv->smstate == SMSTATE_DETACHED);

      /* Notify any waiters */

      priv->smstate = SMSTATE_ATTACHED;
      if (priv->pscwait)
        {
          nxsem_post(&priv->pscsem);
          priv->pscwait = false;
        }
    }
}

/****************************************************************************
 * Name: at32_gint_disconnected
 *
 * Description:
 *   Handle a disconnection event.
 *
 ****************************************************************************/

static void at32_gint_disconnected(struct at32_usbhost_s *priv)
{
  /* Were we previously connected? */

  if (priv->connected)
    {
      /* Yes.. then we no longer connected */

      usbhost_vtrace1(OTGFS_VTRACE1_DISCONNECTED, 0);

      /* Are we bound to a class driver? */

      if (priv->rhport.hport.devclass)
        {
          /* Yes.. Disconnect the class driver */

          CLASS_DISCONNECTED(priv->rhport.hport.devclass);
          priv->rhport.hport.devclass = NULL;
        }

      /* Re-Initialize Host for new Enumeration */

      priv->smstate   = SMSTATE_DETACHED;
      priv->connected = false;
      priv->change    = true;
      at32_chan_freeall(priv);

      priv->rhport.hport.speed = USB_SPEED_FULL;
      priv->rhport.hport.funcaddr = 0;

      /* Notify any waiters that there is a change in the connection state */

      if (priv->pscwait)
        {
          nxsem_post(&priv->pscsem);
          priv->pscwait = false;
        }
    }
}

/****************************************************************************
 * Name: at32_gint_sofisr
 *
 * Description:
 *   USB OTG FS start-of-frame interrupt handler
 *
 ****************************************************************************/

#ifdef CONFIG_AT32_OTGFS_SOFINTR
static inline void at32_gint_sofisr(struct at32_usbhost_s *priv)
{
  /* Handle SOF interrupt */

#warning "Do what?"

  /* Clear pending SOF interrupt */

  at32_putreg(AT32_OTGFS_GINTSTS, OTGFS_GINT_SOF);
}
#endif

/****************************************************************************
 * Name: at32_gint_rxflvlisr
 *
 * Description:
 *   USB OTG FS RxFIFO non-empty interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_rxflvlisr(struct at32_usbhost_s *priv)
{
  uint32_t *dest;
  uint32_t grxsts;
  uint32_t intmsk;
  uint32_t hcchar;
  uint32_t hctsiz;
  uint32_t fifo;
  int bcnt;
  int bcnt32;
  int chidx;
  int i;

  /* Disable the RxFIFO non-empty interrupt */

  intmsk  = at32_getreg(AT32_OTGFS_GINTMSK);
  intmsk &= ~OTGFS_GINT_RXFLVL;
  at32_putreg(AT32_OTGFS_GINTMSK, intmsk);

  /* Read and pop the next status from the Rx FIFO */

  grxsts = at32_getreg(AT32_OTGFS_GRXSTSP);
  uinfo("GRXSTS: %08" PRIx32 "\n", grxsts);

  /* Isolate the channel number/index in the status word */

  chidx = (grxsts & OTGFS_GRXSTSH_CHNUM_MASK) >> OTGFS_GRXSTSH_CHNUM_SHIFT;

  /* Get the host channel characteristics register (HCCHAR) */

  hcchar = at32_getreg(AT32_OTGFS_HCCHAR(chidx));

  /* Then process the interrupt according to the packet status */

  switch (grxsts & OTGFS_GRXSTSH_PKTSTS_MASK)
    {
    case OTGFS_GRXSTSH_PKTSTS_INRECVD: /* IN data packet received */
      {
        /* Read the data into the host buffer. */

        bcnt = (grxsts & OTGFS_GRXSTSH_BCNT_MASK) >>
               OTGFS_GRXSTSH_BCNT_SHIFT;
        if (bcnt > 0 && priv->chan[chidx].buffer != NULL)
          {
            /* Transfer the packet from the Rx FIFO into the user buffer */

            dest   = (uint32_t *)priv->chan[chidx].buffer;
            fifo   = AT32_OTGFS_DFIFO_HCH(0);
            bcnt32 = (bcnt + 3) >> 2;

            for (i = 0; i < bcnt32; i++)
              {
                *dest++ = at32_getreg(fifo);
              }

            at32_pktdump("Received", priv->chan[chidx].buffer, bcnt);

            /* Toggle the IN data pid (Used by Bulk and INTR only) */

            priv->chan[chidx].indata1 ^= true;

            /* Manage multiple packet transfers */

            priv->chan[chidx].buffer += bcnt;
            priv->chan[chidx].xfrd   += bcnt;

            /* Check if more packets are expected */

            hctsiz = at32_getreg(AT32_OTGFS_HCTSIZ(chidx));
            if ((hctsiz & OTGFS_HCTSIZ_PKTCNT_MASK) != 0)
              {
                /* Re-activate the channel when more packets are expected */

                hcchar |= OTGFS_HCCHAR_CHENA;
                hcchar &= ~OTGFS_HCCHAR_CHDIS;
                at32_putreg(AT32_OTGFS_HCCHAR(chidx), hcchar);
              }
          }
      }
      break;

    case OTGFS_GRXSTSH_PKTSTS_INDONE:  /* IN transfer completed */
    case OTGFS_GRXSTSH_PKTSTS_DTOGERR: /* Data toggle error */
    case OTGFS_GRXSTSH_PKTSTS_HALTED:  /* Channel halted */
    default:
      break;
    }

  /* Re-enable the RxFIFO non-empty interrupt */

  intmsk |= OTGFS_GINT_RXFLVL;
  at32_putreg(AT32_OTGFS_GINTMSK, intmsk);
}

/****************************************************************************
 * Name: at32_gint_nptxfeisr
 *
 * Description:
 *   USB OTG FS non-periodic TxFIFO empty interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_nptxfeisr(struct at32_usbhost_s *priv)
{
  struct at32_chan_s *chan;
  uint32_t     regval;
  unsigned int wrsize;
  unsigned int avail;
  unsigned int chidx;

  /* Recover the index of the channel that is waiting for space in the Tx
   * FIFO.
   */

  chidx = priv->chidx;
  chan  = &priv->chan[chidx];

  /* Reduce the buffer size by the number of bytes that were previously
   * placed in the Tx FIFO.
   */

  chan->buffer  += chan->inflight;
  chan->xfrd    += chan->inflight;
  chan->inflight = 0;

  /* If we have now transferred the entire buffer, then this transfer is
   * complete (this case really should never happen because we disable
   * the NPTXFE interrupt on the final packet).
   */

  if (chan->xfrd >= chan->buflen)
    {
      /* Disable further Tx FIFO empty interrupts and bail. */

      at32_modifyreg(AT32_OTGFS_GINTMSK, OTGFS_GINT_NPTXFE, 0);
      return;
    }

  /* Read the status from the top of the non-periodic TxFIFO */

  regval = at32_getreg(AT32_OTGFS_HNPTXSTS);

  /* Extract the number of bytes available in the non-periodic Tx FIFO. */

  avail = ((regval & OTGFS_HNPTXSTS_NPTXFSAV_MASK) >>
           OTGFS_HNPTXSTS_NPTXFSAV_SHIFT) << 2;

  /* Get the size to put in the Tx FIFO now */

  wrsize = chan->buflen - chan->xfrd;

  /* Get minimal size packet that can be sent.  Something is seriously
   * configured wrong if one packet will not fit into the empty Tx FIFO.
   */

  DEBUGASSERT(wrsize > 0 && avail >= MIN(wrsize, chan->maxpacket));
  if (wrsize > avail)
    {
      /* Clip the write size to the number of full, max sized packets
       * that will fit in the Tx FIFO.
       */

      unsigned int wrpackets = avail / chan->maxpacket;
      wrsize = wrpackets * chan->maxpacket;
    }

  /* Otherwise, this will be the last packet to be sent in this transaction.
   * We now need to disable further NPTXFE interrupts.
   */

  else
    {
      at32_modifyreg(AT32_OTGFS_GINTMSK, OTGFS_GINT_NPTXFE, 0);
    }

  /* Write the next group of packets into the Tx FIFO */

  uinfo("HNPTXSTS: %08" PRIx32 " chidx: %d avail: %d buflen: %d xfrd: %d "
        "wrsize: %d\n",
        regval, chidx, avail, chan->buflen, chan->xfrd, wrsize);

  at32_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
}

/****************************************************************************
 * Name: at32_gint_ptxfeisr
 *
 * Description:
 *   USB OTG FS periodic TxFIFO empty interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_ptxfeisr(struct at32_usbhost_s *priv)
{
  struct at32_chan_s *chan;
  uint32_t     regval;
  unsigned int wrsize;
  unsigned int avail;
  unsigned int chidx;

  /* Recover the index of the channel that is waiting for space in the Tx
   * FIFO.
   */

  chidx = priv->chidx;
  chan  = &priv->chan[chidx];

  /* Reduce the buffer size by the number of bytes that were previously
   * placed in the Tx FIFO.
   */

  chan->buffer  += chan->inflight;
  chan->xfrd    += chan->inflight;
  chan->inflight = 0;

  /* If we have now transferred the entire buffer, then this transfer is
   * complete (this case really should never happen because we disable
   * the PTXFE interrupt on the final packet).
   */

  if (chan->xfrd >= chan->buflen)
    {
      /* Disable further Tx FIFO empty interrupts and bail. */

      at32_modifyreg(AT32_OTGFS_GINTMSK, OTGFS_GINT_PTXFE, 0);
      return;
    }

  /* Read the status from the top of the periodic TxFIFO */

  regval = at32_getreg(AT32_OTGFS_HPTXSTS);

  /* Extract the number of bytes available in the periodic Tx FIFO. */

  avail = ((regval & OTGFS_HPTXSTS_PTXFSAVL_MASK) >>
           OTGFS_HPTXSTS_PTXFSAVL_SHIFT) << 2;

  /* Get the size to put in the Tx FIFO now */

  wrsize = chan->buflen - chan->xfrd;

  /* Get minimal size packet that can be sent.  Something is seriously
   * configured wrong if one packet will not fit into the empty Tx FIFO.
   */

  DEBUGASSERT(wrsize && avail >= MIN(wrsize, chan->maxpacket));
  if (wrsize > avail)
    {
      /* Clip the write size to the number of full, max sized packets
       * that will fit in the Tx FIFO.
       */

      unsigned int wrpackets = avail / chan->maxpacket;
      wrsize = wrpackets * chan->maxpacket;
    }

  /* Otherwise, this will be the last packet to be sent in this transaction.
   * We now need to disable further PTXFE interrupts.
   */

  else
    {
      at32_modifyreg(AT32_OTGFS_GINTMSK, OTGFS_GINT_PTXFE, 0);
    }

  /* Write the next group of packets into the Tx FIFO */

  uinfo("HPTXSTS: %08" PRIx32
        " chidx: %d avail: %d buflen: %d xfrd: %d wrsize: %d\n",
        regval, chidx, avail, chan->buflen, chan->xfrd, wrsize);

  at32_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
}

/****************************************************************************
 * Name: at32_gint_hcisr
 *
 * Description:
 *   USB OTG FS host channels interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_hcisr(struct at32_usbhost_s *priv)
{
  uint32_t haint;
  uint32_t hcchar;
  int i = 0;

  /* Read the Host all channels interrupt register and test each bit in the
   * register. Each bit i, i=0...(AT32_NHOST_CHANNELS-1), corresponds to
   * a pending interrupt on channel i.
   */

  haint = at32_getreg(AT32_OTGFS_HAINT);
  for (i = 0; i < AT32_NHOST_CHANNELS; i++)
    {
      /* Is an interrupt pending on this channel? */

      if ((haint & OTGFS_HAINT(i)) != 0)
        {
          /* Yes... read the HCCHAR register to get the direction bit */

          hcchar = at32_getreg(AT32_OTGFS_HCCHAR(i));

          /* Was this an interrupt on an IN or an OUT channel? */

          if ((hcchar & OTGFS_HCCHAR_EPDIR) != 0)
            {
              /* Handle the HC IN channel interrupt */

              at32_gint_hcinisr(priv, i);
            }
          else
            {
              /* Handle the HC OUT channel interrupt */

              at32_gint_hcoutisr(priv, i);
            }
        }
    }
}

/****************************************************************************
 * Name: at32_gint_hprtisr
 *
 * Description:
 *   USB OTG FS host port interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_hprtisr(struct at32_usbhost_s *priv)
{
  uint32_t hprt;
  uint32_t newhprt;
  uint32_t hcfg;

  usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT, 0);

  /* Read the port status and control register (HPRT) */

  hprt = at32_getreg(AT32_OTGFS_HPRT);

  /* Setup to clear the interrupt bits in GINTSTS by setting the
   * corresponding bits in the HPRT.  The HCINT interrupt bit is cleared
   * when the appropriate status bits in the HPRT register are cleared.
   */

  newhprt = hprt & ~(OTGFS_HPRT_PENA    | OTGFS_HPRT_PCDET  |
                     OTGFS_HPRT_PENCHNG | OTGFS_HPRT_POCCHNG);

  /* Check for Port Over-urrent CHaNGe (POCCHNG) */

  if ((hprt & OTGFS_HPRT_POCCHNG) != 0)
    {
      /* Set up to clear the POCCHNG status in the new HPRT contents. */

      usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_POCCHNG, 0);
      newhprt |= OTGFS_HPRT_POCCHNG;
    }

  /* Check for Port Connect DETected (PCDET).  The core sets this bit when a
   * device connection is detected.
   */

  if ((hprt & OTGFS_HPRT_PCDET) != 0)
    {
      /* Set up to clear the PCDET status in the new HPRT contents. Then
       * process the new connection event.
       */

      usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_PCDET, 0);
      newhprt |= OTGFS_HPRT_PCDET;
      at32_portreset(priv);
      at32_gint_connected(priv);
    }

  /* Check for Port Enable CHaNGed (PENCHNG) */

  if ((hprt & OTGFS_HPRT_PENCHNG) != 0)
    {
      /* Set up to clear the PENCHNG status in the new HPRT contents. */

      usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_PENCHNG, 0);
      newhprt |= OTGFS_HPRT_PENCHNG;

      /* Was the port enabled? */

      if ((hprt & OTGFS_HPRT_PENA) != 0)
        {
          /* Yes.. handle the new connection event */

          at32_gint_connected(priv);

          /* Check the Host ConFiGuration register (HCFG) */

          hcfg = at32_getreg(AT32_OTGFS_HCFG);

          /* Is this a low speed or full speed connection (OTG FS does not
           * support high speed)
           */

          if ((hprt & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_LS)
            {
              /* Set the Host Frame Interval Register for the 6KHz speed */

              usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_LSDEV, 0);
              at32_putreg(AT32_OTGFS_HFIR, 6000);

              /* Are we switching from FS to LS? */

              if ((hcfg & OTGFS_HCFG_FSLSPCS_MASK) !=
                  OTGFS_HCFG_FSLSPCS_LS6MHz)
                {
                  usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_FSLSSW, 0);

                  /* Yes... configure for LS */

                  hcfg &= ~OTGFS_HCFG_FSLSPCS_MASK;
                  hcfg |= OTGFS_HCFG_FSLSPCS_LS6MHz;
                  at32_putreg(AT32_OTGFS_HCFG, hcfg);

                  /* And reset the port */

                  at32_portreset(priv);
                }
            }
          else /* if ((hprt & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_FS) */
            {
              usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_FSDEV, 0);
              at32_putreg(AT32_OTGFS_HFIR, 48000);

              /* Are we switching from LS to FS? */

              if ((hcfg & OTGFS_HCFG_FSLSPCS_MASK) !=
                  OTGFS_HCFG_FSLSPCS_FS48MHz)
                {
                  usbhost_vtrace1(OTGFS_VTRACE1_GINT_HPRT_LSFSSW, 0);

                  /* Yes... configure for FS */

                  hcfg &= ~OTGFS_HCFG_FSLSPCS_MASK;
                  hcfg |= OTGFS_HCFG_FSLSPCS_FS48MHz;
                  at32_putreg(AT32_OTGFS_HCFG, hcfg);

                  /* And reset the port */

                  at32_portreset(priv);
                }
            }
        }
    }

  /* Clear port interrupts by setting bits in the HPRT */

  at32_putreg(AT32_OTGFS_HPRT, newhprt);
}

/****************************************************************************
 * Name: at32_gint_discisr
 *
 * Description:
 *   USB OTG FS disconnect detected interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_discisr(struct at32_usbhost_s *priv)
{
  /* Handle the disconnection event */

  at32_gint_disconnected(priv);

  /* Clear the dicsonnect interrupt */

  at32_putreg(AT32_OTGFS_GINTSTS, OTGFS_GINT_DISC);
}

/****************************************************************************
 * Name: at32_gint_ipxfrisr
 *
 * Description:
 *   USB OTG FS incomplete periodic interrupt handler
 *
 ****************************************************************************/

static inline void at32_gint_ipxfrisr(struct at32_usbhost_s *priv)
{
  uint32_t regval;

  /* CHENA : Set to enable the channel
   * CHDIS : Set to stop transmitting/receiving data on a channel
   */

  regval = at32_getreg(AT32_OTGFS_HCCHAR(0));
  regval |= (OTGFS_HCCHAR_CHDIS | OTGFS_HCCHAR_CHENA);
  at32_putreg(AT32_OTGFS_HCCHAR(0), regval);

  /* Clear the incomplete isochronous OUT interrupt */

  at32_putreg(AT32_OTGFS_GINTSTS, OTGFS_GINT_IPXFR);
}

/****************************************************************************
 * Name: at32_gint_isr
 *
 * Description:
 *   USB OTG FS global interrupt handler
 *
 ****************************************************************************/

static int at32_gint_isr(int irq, void *context, void *arg)
{
  /* At present, there is only support for a single OTG FS host. Hence it is
   * pre-allocated as g_usbhost.  However, in most code, the private data
   * structure will be referenced using the 'priv' pointer (rather than the
   * global data) in order to simplify any future support for multiple
   * devices.
   */

  struct at32_usbhost_s *priv = &g_usbhost;
  uint32_t pending;

  /* If OTG were supported, we would need to check if we are in host or
   * device mode when the global interrupt occurs.  Here we support only
   * host mode
   */

  /* Loop while there are pending interrupts to process.  This loop may save
   * a little interrupt handling overhead.
   */

  for (; ; )
    {
      /* Get the unmasked bits in the GINT status */

      pending  = at32_getreg(AT32_OTGFS_GINTSTS);
      pending &= at32_getreg(AT32_OTGFS_GINTMSK);

      /* Return from the interrupt when there are no further pending
       * interrupts.
       */

      if (pending == 0)
        {
          return OK;
        }

      /* Otherwise, process each pending, unmasked GINT interrupts */

      /* Handle the start of frame interrupt */

#ifdef CONFIG_AT32_OTGFS_SOFINTR
      if ((pending & OTGFS_GINT_SOF) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_SOF, 0);
          at32_gint_sofisr(priv);
        }
#endif

      /* Handle the RxFIFO non-empty interrupt */

      if ((pending & OTGFS_GINT_RXFLVL) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_RXFLVL, 0);
          at32_gint_rxflvlisr(priv);
        }

      /* Handle the non-periodic TxFIFO empty interrupt */

      if ((pending & OTGFS_GINT_NPTXFE) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_NPTXFE, 0);
          at32_gint_nptxfeisr(priv);
        }

      /* Handle the periodic TxFIFO empty interrupt */

      if ((pending & OTGFS_GINT_PTXFE) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_PTXFE, 0);
          at32_gint_ptxfeisr(priv);
        }

      /* Handle the host channels interrupt */

      if ((pending & OTGFS_GINT_HC) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_HC, 0);
          at32_gint_hcisr(priv);
        }

      /* Handle the host port interrupt */

      if ((pending & OTGFS_GINT_HPRT) != 0)
        {
          at32_gint_hprtisr(priv);
        }

      /* Handle the disconnect detected interrupt */

      if ((pending & OTGFS_GINT_DISC) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_DISC, 0);
          at32_gint_discisr(priv);
        }

      /* Handle the incomplete periodic transfer */

      if ((pending & OTGFS_GINT_IPXFR) != 0)
        {
          usbhost_vtrace1(OTGFS_VTRACE1_GINT_IPXFR, 0);
          at32_gint_ipxfrisr(priv);
        }
    }

  /* We won't get here */

  return OK;
}

/****************************************************************************
 * Name: at32_gint_enable and at32_gint_disable
 *
 * Description:
 *   Respectively enable or disable the global OTG FS interrupt.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void at32_gint_enable(void)
{
  uint32_t regval;

  /* Set the GINTMSK bit to unmask the interrupt */

  regval  = at32_getreg(AT32_OTGFS_GAHBCFG);
  regval |= OTGFS_GAHBCFG_GINTMSK;
  at32_putreg(AT32_OTGFS_GAHBCFG, regval);
}

static void at32_gint_disable(void)
{
  uint32_t regval;

  /* Clear the GINTMSK bit to mask the interrupt */

  regval  = at32_getreg(AT32_OTGFS_GAHBCFG);
  regval &= ~OTGFS_GAHBCFG_GINTMSK;
  at32_putreg(AT32_OTGFS_GAHBCFG, regval);
}

/****************************************************************************
 * Name: at32_hostinit_enable
 *
 * Description:
 *   Enable host interrupts.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void at32_hostinit_enable(void)
{
  uint32_t regval;

  /* Disable all interrupts. */

  at32_putreg(AT32_OTGFS_GINTMSK, 0);

  /* Clear any pending interrupts. */

  at32_putreg(AT32_OTGFS_GINTSTS, 0xffffffff);

  /* Clear any pending USB OTG Interrupts */

  at32_putreg(AT32_OTGFS_GOTGINT, 0xffffffff);

  /* Clear any pending USB OTG interrupts */

  at32_putreg(AT32_OTGFS_GINTSTS, 0xbfffffff);

  /* Enable the host interrupts */

  /* Common interrupts:
   *
   *   OTGFS_GINT_WKUP     : Resume/remote wakeup detected interrupt
   *   OTGFS_GINT_USBSUSP  : USB suspend
   */

  regval = (OTGFS_GINT_WKUP | OTGFS_GINT_USBSUSP);

  /* If OTG were supported, we would need to enable the following as well:
   *
   *   OTGFS_GINT_OTG      : OTG interrupt
   *   OTGFS_GINT_SRQ      : Session request/new session detected interrupt
   *   OTGFS_GINT_CIDSCHG  : Connector ID status change
   */

  /* Host-specific interrupts
   *
   *   OTGFS_GINT_SOF      : Start of frame
   *   OTGFS_GINT_RXFLVL   : RxFIFO non-empty
   *   OTGFS_GINT_IISOOXFR : Incomplete isochronous OUT transfer
   *   OTGFS_GINT_HPRT     : Host port interrupt
   *   OTGFS_GINT_HC       : Host channels interrupt
   *   OTGFS_GINT_DISC     : Disconnect detected interrupt
   */

#ifdef CONFIG_AT32_OTGFS_SOFINTR
  regval |= (OTGFS_GINT_SOF    | OTGFS_GINT_RXFLVL   | OTGFS_GINT_IISOOXFR |
             OTGFS_GINT_HPRT   | OTGFS_GINT_HC       | OTGFS_GINT_DISC);
#else
  regval |= (OTGFS_GINT_RXFLVL | OTGFS_GINT_IPXFR    | OTGFS_GINT_HPRT     |
             OTGFS_GINT_HC     | OTGFS_GINT_DISC);
#endif
  at32_putreg(AT32_OTGFS_GINTMSK, regval);
}

/****************************************************************************
 * Name: at32_txfe_enable
 *
 * Description:
 *   Enable Tx FIFO empty interrupts.  This is necessary when the entire
 *   transfer will not fit into Tx FIFO.  The transfer will then be completed
 *   when the Tx FIFO is empty.  NOTE:  The Tx FIFO interrupt is disabled
 *   the fifo empty interrupt handler when the transfer is complete.
 *
 * Input Parameters:
 *   priv - Driver state structure reference
 *   chidx - The channel that requires the Tx FIFO empty interrupt
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called from user task context.  Interrupts must be disabled to assure
 *   exclusive access to the GINTMSK register.
 *
 ****************************************************************************/

static void at32_txfe_enable(struct at32_usbhost_s *priv, int chidx)
{
  struct at32_chan_s *chan = &priv->chan[chidx];
  irqstate_t flags;
  uint32_t regval;

  /* Disable all interrupts so that we have exclusive access to the GINTMSK
   * (it would be sufficient just to disable the GINT interrupt).
   */

  flags = enter_critical_section();

  /* Should we enable the periodic or non-peridic Tx FIFO empty interrupts */

  regval = at32_getreg(AT32_OTGFS_GINTMSK);
  switch (chan->eptype)
    {
    default:
    case OTGFS_EPTYPE_CTRL: /* Non periodic transfer */
    case OTGFS_EPTYPE_BULK:
      regval |= OTGFS_GINT_NPTXFE;
      break;

    case OTGFS_EPTYPE_INTR: /* Periodic transfer */
    case OTGFS_EPTYPE_ISOC:
      regval |= OTGFS_GINT_PTXFE;
      break;
    }

  /* Enable interrupts */

  at32_putreg(AT32_OTGFS_GINTMSK, regval);
  leave_critical_section(flags);
}

/****************************************************************************
 * USB Host Controller Operations
 ****************************************************************************/

/****************************************************************************
 * Name: at32_wait
 *
 * Description:
 *   Wait for a device to be connected or disconnected to/from a hub port.
 *
 * Input Parameters:
 *   conn - The USB host connection instance obtained as a parameter from
 *     the call to the USB driver initialization logic.
 *   hport - The location to return the hub port descriptor that detected
 *     the connection related event.
 *
 * Returned Value:
 *   Zero (OK) is returned on success when a device is connected or
 *   disconnected. This function will not return until either (1) a device is
 *   connected or disconnect to/from any hub port or until (2) some failure
 *   occurs.  On a failure, a negated errno value is returned indicating the
 *   nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_wait(struct usbhost_connection_s *conn,
                      struct usbhost_hubport_s **hport)
{
  struct at32_usbhost_s *priv = &g_usbhost;
  struct usbhost_hubport_s *connport;
  irqstate_t flags;
  int ret;

  /* Loop until a change in connection state is detected */

  flags = enter_critical_section();
  for (; ; )
    {
      /* Is there a change in the connection state of the single root hub
       * port?
       */

      if (priv->change)
        {
          connport = &priv->rhport.hport;

          /* Yes. Remember the new state */

          connport->connected = priv->connected;
          priv->change = false;

          /* And return the root hub port */

          *hport = connport;
          leave_critical_section(flags);

          uinfo("RHport Connected: %s\n",
                connport->connected ? "YES" : "NO");
          return OK;
        }

#ifdef CONFIG_USBHOST_HUB
      /* Is a device connected to an external hub? */

      if (priv->hport)
        {
          /* Yes.. return the external hub port */

          connport = (struct usbhost_hubport_s *)priv->hport;
          priv->hport = NULL;

          *hport = connport;
          leave_critical_section(flags);

          uinfo("Hub port Connected: %s\n",
                connport->connected ? "YES" : "NO");
          return OK;
        }
#endif

      /* Wait for the next connection event */

      priv->pscwait = true;
      ret = nxsem_wait_uninterruptible(&priv->pscsem);
      if (ret < 0)
        {
          return ret;
        }
    }
}

/****************************************************************************
 * Name: at32_enumerate
 *
 * Description:
 *   Enumerate the connected device.  As part of this enumeration process,
 *   the driver will (1) get the device's configuration descriptor, (2)
 *   extract the class ID info from the configuration descriptor, (3) call
 *   usbhost_findclass() to find the class that supports this device, (4)
 *   call the create() method on the struct usbhost_registry_s interface
 *   to get a class instance, and finally (5) call the connect() method
 *   of the struct usbhost_class_s interface.  After that, the class is in
 *   charge of the sequence of operations.
 *
 * Input Parameters:
 *   conn - The USB host connection instance obtained as a parameter from
 *      the call to the USB driver initialization logic.
 *   hport - The descriptor of the hub port that has the newly connected
 *      device.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_rh_enumerate(struct at32_usbhost_s *priv,
                              struct usbhost_connection_s *conn,
                              struct usbhost_hubport_s *hport)
{
  uint32_t regval;
  int ret;

  DEBUGASSERT(conn != NULL && hport != NULL && hport->port == 0);

  /* Are we connected to a device?  The caller should have called the wait()
   * method first to be assured that a device is connected.
   */

  while (!priv->connected)
    {
      /* No, return an error */

      usbhost_trace1(OTGFS_TRACE1_DEVDISCONN, 0);
      return -ENODEV;
    }

  DEBUGASSERT(priv->smstate == SMSTATE_ATTACHED);

  /* USB 2.0 spec says at least 50ms delay before port reset.  We wait
   * 100ms.
   */

  nxsig_usleep(100 * 1000);

  /* Reset the host port */

  at32_portreset(priv);

  /* Get the current device speed */

  regval = at32_getreg(AT32_OTGFS_HPRT);
  if ((regval & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_LS)
    {
      priv->rhport.hport.speed = USB_SPEED_LOW;
    }
  else
    {
      priv->rhport.hport.speed = USB_SPEED_FULL;
    }

  /* Allocate and initialize the root hub port EP0 channels */

  ret = at32_ctrlchan_alloc(priv, 0, 0, priv->rhport.hport.speed,
                             &priv->ep0);
  if (ret < 0)
    {
      uerr("ERROR: Failed to allocate a control endpoint: %d\n", ret);
    }

  return ret;
}

static int at32_enumerate(struct usbhost_connection_s *conn,
                           struct usbhost_hubport_s *hport)
{
  struct at32_usbhost_s *priv = &g_usbhost;
  int ret;

  DEBUGASSERT(hport);

  /* If this is a connection on the root hub, then we need to go to
   * little more effort to get the device speed.  If it is a connection
   * on an external hub, then we already have that information.
   */

#ifdef CONFIG_USBHOST_HUB
  if (ROOTHUB(hport))
#endif
    {
      ret = at32_rh_enumerate(priv, conn, hport);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Then let the common usbhost_enumerate do the real enumeration. */

  uinfo("Enumerate the device\n");
  priv->smstate = SMSTATE_ENUM;
  ret = usbhost_enumerate(hport, &hport->devclass);

  /* The enumeration may fail either because of some HCD interfaces failure
   * or because the device class is not supported.  In either case, we just
   * need to perform the disconnection operation and make ready for a new
   * enumeration.
   */

  if (ret < 0)
    {
      /* Return to the disconnected state */

      uerr("ERROR: Enumeration failed: %d\n", ret);
      at32_gint_disconnected(priv);
    }

  return ret;
}

/****************************************************************************
 * Name: at32_ep0configure
 *
 * Description:
 *   Configure endpoint 0.  This method is normally used internally by the
 *   enumerate() method but is made available at the interface to support an
 *   external implementation of the enumeration logic.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep0 - The (opaque) EP0 endpoint instance
 *   funcaddr - The USB address of the function containing the endpoint that
 *     EP0 controls
 *   speed - The speed of the port USB_SPEED_LOW, _FULL, or _HIGH
 *   maxpacketsize - The maximum number of bytes that can be sent to or
 *    received from the endpoint in a single data packet
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_ep0configure(struct usbhost_driver_s *drvr,
                              usbhost_ep_t ep0, uint8_t funcaddr,
                              uint8_t speed, uint16_t maxpacketsize)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  struct at32_ctrlinfo_s *ep0info = (struct at32_ctrlinfo_s *)ep0;
  struct at32_chan_s *chan;
  int ret;

  DEBUGASSERT(drvr != NULL && ep0info != NULL && funcaddr < 128 &&
              maxpacketsize <= 64);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Configure the EP0 OUT channel */

  chan            = &priv->chan[ep0info->outndx];
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->maxpacket = maxpacketsize;

  at32_chan_configure(priv, ep0info->outndx);

  /* Configure the EP0 IN channel */

  chan            = &priv->chan[ep0info->inndx];
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->maxpacket = maxpacketsize;

  at32_chan_configure(priv, ep0info->inndx);

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: at32_epalloc
 *
 * Description:
 *   Allocate and configure one endpoint.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   epdesc - Describes the endpoint to be allocated.
 *   ep - A memory location provided by the caller in which to receive the
 *      allocated endpoint descriptor.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_epalloc(struct usbhost_driver_s *drvr,
                         const struct usbhost_epdesc_s *epdesc,
                         usbhost_ep_t *ep)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  int ret;

  /* Sanity check.  NOTE that this method should only be called if a device
   * is connected (because we need a valid low speed indication).
   */

  DEBUGASSERT(drvr != 0 && epdesc != NULL && ep != NULL);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Handler control pipes differently from other endpoint types.  This is
   * because the normal, "transfer" endpoints are unidirectional an require
   * only a single channel.  Control endpoints, however, are bi-diretional
   * and require two channels, one for the IN and one for the OUT direction.
   */

  if (epdesc->xfrtype == OTGFS_EPTYPE_CTRL)
    {
      ret = at32_ctrlep_alloc(priv, epdesc, ep);
    }
  else
    {
      ret = at32_xfrep_alloc(priv, epdesc, ep);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: at32_epfree
 *
 * Description:
 *   Free and endpoint previously allocated by DRVR_EPALLOC.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep - The endpoint to be freed.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_epfree(struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  int ret;

  DEBUGASSERT(priv);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);

  /* A single channel is represent by an index in the range of 0 to
   * AT32_MAX_TX_FIFOS.  Otherwise, the ep must be a pointer to an allocated
   * control endpoint structure.
   */

  if ((uintptr_t)ep < AT32_MAX_TX_FIFOS)
    {
      /* Halt the channel and mark the channel available */

      at32_chan_free(priv, (int)ep);
    }
  else
    {
      /* Halt both control channel and mark the channels available */

      struct at32_ctrlinfo_s *ctrlep =
        (struct at32_ctrlinfo_s *)ep;

      at32_chan_free(priv, ctrlep->inndx);
      at32_chan_free(priv, ctrlep->outndx);

      /* And free the control endpoint container */

      kmm_free(ctrlep);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: at32_alloc
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor
 *   data can be accessed more efficiently.  This method provides a
 *   mechanism to allocate the request/descriptor memory.  If the underlying
 *   hardware does not support such "special" memory, this functions may
 *   simply map to kmm_malloc.
 *
 *   This interface was optimized under a particular assumption.  It was
 *   assumed that the driver maintains a pool of small, pre-allocated
 *   buffers for descriptor traffic.  NOTE that size is not an input, but
 *   an output:  The size of the pre-allocated buffer is returned.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   buffer - The address of a memory location provided by the caller in
 *     which to return the allocated buffer memory address.
 *   maxlen - The address of a memory location provided by the caller in
 *     which to return the maximum size of the allocated buffer memory.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_alloc(struct usbhost_driver_s *drvr,
                       uint8_t **buffer, size_t *maxlen)
{
  uint8_t *alloc;

  DEBUGASSERT(drvr && buffer && maxlen);

  /* There is no special memory requirement for the AT32. */

  alloc = (uint8_t *)kmm_malloc(CONFIG_AT32_OTGFS_DESCSIZE);
  if (!alloc)
    {
      return -ENOMEM;
    }

  /* Return the allocated address and size of the descriptor buffer */

  *buffer = alloc;
  *maxlen = CONFIG_AT32_OTGFS_DESCSIZE;
  return OK;
}

/****************************************************************************
 * Name: at32_free
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor
 *   data can be accessed more efficiently.  This method provides a
 *   mechanism to free that request/descriptor memory.  If the underlying
 *   hardware does not support such "special" memory, this functions may
 *   simply map to kmm_free().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_free(struct usbhost_driver_s *drvr, uint8_t *buffer)
{
  /* There is no special memory requirement */

  DEBUGASSERT(drvr && buffer);
  kmm_free(buffer);
  return OK;
}

/****************************************************************************
 * Name: at32_ioalloc
 *
 * Description:
 *   Some hardware supports special memory in which larger IO buffers can
 *   be accessed more efficiently.  This method provides a mechanism to
 *   allocate the request/descriptor memory.  If the underlying hardware
 *   does not support such "special" memory, this functions may simply map
 *   to kmm_malloc.
 *
 *   This interface differs from DRVR_ALLOC in that the buffers are
 *   variable-sized.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   buffer - The address of a memory location provided by the caller in
 *     which to return the allocated buffer memory address.
 *   buflen - The size of the buffer required.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_ioalloc(struct usbhost_driver_s *drvr,
                         uint8_t **buffer, size_t buflen)
{
  uint8_t *alloc;

  DEBUGASSERT(drvr && buffer && buflen > 0);

  /* There is no special memory requirement */

  alloc = (uint8_t *)kmm_malloc(buflen);
  if (!alloc)
    {
      return -ENOMEM;
    }

  /* Return the allocated buffer */

  *buffer = alloc;
  return OK;
}

/****************************************************************************
 * Name: at32_iofree
 *
 * Description:
 *   Some hardware supports special memory in which IO data can  be accessed
 *   more efficiently.  This method provides a mechanism to free that IO
 *   buffer memory.  If the underlying hardware does not support such
 *   "special" memory, this functions may simply map to kmm_free().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_iofree(struct usbhost_driver_s *drvr,
                        uint8_t *buffer)
{
  /* There is no special memory requirement */

  DEBUGASSERT(drvr && buffer);
  kmm_free(buffer);
  return OK;
}

/****************************************************************************
 * Name: at32_ctrlin and at32_ctrlout
 *
 * Description:
 *   Process a IN or OUT request on the control endpoint.  These methods
 *   will enqueue the request and wait for it to complete.  Only one
 *   transfer may be queued; Neither these methods nor the transfer()
 *   method can be called again until the control transfer functions
 *   returns.
 *
 *   These are blocking methods; these functions will not return until the
 *   control transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep0 - The control endpoint to send/receive the control request.
 *   req - Describes the request to be sent.  This request must lie in memory
 *      created by DRVR_ALLOC.
 *   buffer - A buffer used for sending the request and for returning any
 *     responses.  This buffer must be large enough to hold the length value
 *     in the request description. buffer must have been allocated using
 *     DRVR_ALLOC.
 *
 *   NOTE: On an IN transaction, req and buffer may refer to the same
 *   allocated memory.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static int at32_ctrlin(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                        const struct usb_ctrlreq_s *req,
                        uint8_t *buffer)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  struct at32_ctrlinfo_s *ep0info = (struct at32_ctrlinfo_s *)ep0;
  uint16_t buflen;
  clock_t start;
  clock_t elapsed;
  int retries;
  int ret;

  DEBUGASSERT(priv != NULL && ep0info != NULL && req != NULL);
  usbhost_vtrace2(OTGFS_VTRACE2_CTRLIN, req->type, req->req);
  uinfo("type:%02x req:%02x value:%02x%02x index:%02x%02x len:%02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* Extract values from the request */

  buflen = at32_getle16(req->len);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Loop, retrying until the retry time expires */

  for (retries = 0; retries < AT32_RETRY_COUNT; retries++)
    {
      /* Send the SETUP request */

      ret = at32_ctrl_sendsetup(priv, ep0info, req);
      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_SENDSETUP, -ret);
          continue;
        }

      /* Handle the IN data phase (if any) */

      if (buflen > 0)
        {
          ret = at32_ctrl_recvdata(priv, ep0info, buffer, buflen);
          if (ret < 0)
            {
              usbhost_trace1(OTGFS_TRACE1_RECVDATA, -ret);
              continue;
            }
        }

      /* Get the start time.  Loop again until the timeout expires */

      start = clock_systime_ticks();
      do
        {
          /* Handle the status OUT phase */

          priv->chan[ep0info->outndx].outdata1 ^= true;
          ret = at32_ctrl_senddata(priv, ep0info, NULL, 0);
          if (ret == OK)
            {
              /* All success transactions exit here */

              nxmutex_unlock(&priv->lock);
              return OK;
            }

          usbhost_trace1(OTGFS_TRACE1_SENDDATA, ret < 0 ? -ret : ret);

          /* Get the elapsed time (in frames) */

          elapsed = clock_systime_ticks() - start;
        }
      while (elapsed < AT32_DATANAK_DELAY);
    }

  /* All failures exit here after all retries and timeouts are exhausted */

  nxmutex_unlock(&priv->lock);
  return -ETIMEDOUT;
}

static int at32_ctrlout(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                         const struct usb_ctrlreq_s *req,
                         const uint8_t *buffer)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  struct at32_ctrlinfo_s *ep0info = (struct at32_ctrlinfo_s *)ep0;
  uint16_t buflen;
  clock_t start;
  clock_t elapsed;
  int retries;
  int ret;

  DEBUGASSERT(priv != NULL && ep0info != NULL && req != NULL);
  usbhost_vtrace2(OTGFS_VTRACE2_CTRLOUT, req->type, req->req);
  uinfo("type:%02x req:%02x value:%02x%02x index:%02x%02x len:%02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* Extract values from the request */

  buflen = at32_getle16(req->len);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Loop, retrying until the retry time expires */

  for (retries = 0; retries < AT32_RETRY_COUNT; retries++)
    {
      /* Send the SETUP request */

      ret = at32_ctrl_sendsetup(priv, ep0info, req);
      if (ret < 0)
        {
          usbhost_trace1(OTGFS_TRACE1_SENDSETUP, -ret);
          continue;
        }

      /* Get the start time.  Loop again until the timeout expires */

      start = clock_systime_ticks();
      do
        {
          /* Handle the data OUT phase (if any) */

          if (buflen > 0)
            {
              /* Start DATA out transfer (only one DATA packet) */

              priv->chan[ep0info->outndx].outdata1 = true;
              ret = at32_ctrl_senddata(priv, ep0info, (uint8_t *)buffer,
                                        buflen);
              if (ret < 0)
                {
                  usbhost_trace1(OTGFS_TRACE1_SENDDATA, -ret);
                }
            }

          /* Handle the status IN phase */

          if (ret == OK)
            {
              ret = at32_ctrl_recvdata(priv, ep0info, NULL, 0);
              if (ret == OK)
                {
                  /* All success transactins exit here */

                  nxmutex_unlock(&priv->lock);
                  return OK;
                }

              usbhost_trace1(OTGFS_TRACE1_RECVDATA, ret < 0 ? -ret : ret);
            }

          /* Get the elapsed time (in frames) */

          elapsed = clock_systime_ticks() - start;
        }
      while (elapsed < AT32_DATANAK_DELAY);
    }

  /* All failures exit here after all retries and timeouts are exhausted */

  nxmutex_unlock(&priv->lock);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: at32_transfer
 *
 * Description:
 *   Process a request to handle a transfer descriptor.  This method will
 *   enqueue the transfer request, blocking until the transfer completes.
 *   Only one transfer may be  queued; Neither this method nor the ctrlin or
 *   ctrlout methods can be called again until this function returns.
 *
 *   This is a blocking method; this functions will not return until the
 *   transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep - The IN or OUT endpoint descriptor for the device endpoint on
 *     which to perform the transfer.
 *   buffer - A buffer containing the data to be sent (OUT endpoint) or
 *     received (IN endpoint).  buffer must have been allocated using
 *     DRVR_ALLOC
 *   buflen - The length of the data to be sent or received.
 *
 * Returned Value:
 *   On success, a non-negative value is returned that indicates the number
 *   of bytes successfully transferred.  On a failure, a negated errno value
 *   is returned that indicates the nature of the failure:
 *
 *     EAGAIN - If devices NAKs the transfer (or NYET or other error where
 *              it may be appropriate to restart the entire transaction).
 *     EPERM  - If the endpoint stalls
 *     EIO    - On a TX or data toggle error
 *     EPIPE  - Overrun errors
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static ssize_t at32_transfer(struct usbhost_driver_s *drvr,
                              usbhost_ep_t ep,
                              uint8_t *buffer, size_t buflen)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  unsigned int chidx = (unsigned int)ep;
  ssize_t nbytes;
  int ret;

  uinfo("chidx: %d buflen: %d\n",  (unsigned int)ep, buflen);

  DEBUGASSERT(priv && buffer && chidx < AT32_MAX_TX_FIFOS && buflen > 0);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return (ssize_t)ret;
    }

  /* Handle IN and OUT transfer slightly differently */

  if (priv->chan[chidx].in)
    {
      nbytes = at32_in_transfer(priv, chidx, buffer, buflen);
    }
  else
    {
      nbytes = at32_out_transfer(priv, chidx, buffer, buflen);
    }

  nxmutex_unlock(&priv->lock);
  return nbytes;
}

/****************************************************************************
 * Name: at32_asynch
 *
 * Description:
 *   Process a request to handle a transfer descriptor.  This method will
 *   enqueue the transfer request and return immediately.  When the transfer
 *   completes, the callback will be invoked with the provided transfer.
 *   This method is useful for receiving interrupt transfers which may come
 *   infrequently.
 *
 *   Only one transfer may be queued; Neither this method nor the ctrlin or
 *   ctrlout methods can be called again until the transfer completes.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep - The IN or OUT endpoint descriptor for the device endpoint on
 *     which to perform the transfer.
 *   buffer - A buffer containing the data to be sent (OUT endpoint) or
 *     received (IN endpoint).  buffer must have been allocated using
 *     DRVR_ALLOC
 *   buflen - The length of the data to be sent or received.
 *   callback - This function will be called when the transfer completes.
 *   arg - The arbitrary parameter that will be passed to the callback
 *     function when the transfer completes.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_ASYNCH
static int at32_asynch(struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                        uint8_t *buffer, size_t buflen,
                        usbhost_asynch_t callback, void *arg)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  unsigned int chidx = (unsigned int)ep;
  int ret;

  uinfo("chidx: %d buflen: %d\n",  (unsigned int)ep, buflen);

  DEBUGASSERT(priv && buffer && chidx < AT32_MAX_TX_FIFOS && buflen > 0);

  /* We must have exclusive access to the USB host hardware and structures */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Handle IN and OUT transfer slightly differently */

  if (priv->chan[chidx].in)
    {
      ret = at32_in_asynch(priv, chidx, buffer, buflen, callback, arg);
    }
  else
    {
      ret = at32_out_asynch(priv, chidx, buffer, buflen, callback, arg);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}
#endif /* CONFIG_USBHOST_ASYNCH */

/****************************************************************************
 * Name: at32_cancel
 *
 * Description:
 *   Cancel a pending transfer on an endpoint.  Cancelled synchronous or
 *   asynchronous transfer will complete normally with the error -ESHUTDOWN.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   ep - The IN or OUT endpoint descriptor for the device endpoint on
 *     which an asynchronous transfer should be transferred.
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 ****************************************************************************/

static int at32_cancel(struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  struct at32_chan_s *chan;
  unsigned int chidx = (unsigned int)ep;
  irqstate_t flags;

  uinfo("chidx: %u\n", chidx);

  DEBUGASSERT(priv && chidx < AT32_MAX_TX_FIFOS);
  chan = &priv->chan[chidx];

  /* We need to disable interrupts to avoid race conditions with the
   * asynchronous completion of the transfer being cancelled.
   */

  flags = enter_critical_section();

  /* Halt the channel */

  at32_chan_halt(priv, chidx, CHREASON_CANCELLED);
  chan->result = -ESHUTDOWN;

  /* Is there a thread waiting for this transfer to complete? */

  if (chan->waiter)
    {
#ifdef CONFIG_USBHOST_ASYNCH
      /* Yes.. there should not also be a callback scheduled */

      DEBUGASSERT(chan->callback == NULL);
#endif

      /* Wake'em up! */

      nxsem_post(&chan->waitsem);
      chan->waiter = false;
    }

#ifdef CONFIG_USBHOST_ASYNCH
  /* No.. is an asynchronous callback expected when the transfer
   * completes?
   */

  else if (chan->callback)
    {
      usbhost_asynch_t callback;
      void *arg;

      /* Extract the callback information */

      callback       = chan->callback;
      arg            = chan->arg;

      chan->callback = NULL;
      chan->arg      = NULL;
      chan->xfrd     = 0;

      /* Then perform the callback */

      callback(arg, -ESHUTDOWN);
    }
#endif

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: at32_connect
 *
 * Description:
 *   New connections may be detected by an attached hub.  This method is the
 *   mechanism that is used by the hub class to introduce a new connection
 *   and port description to the system.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   hport - The descriptor of the hub port that detected the connection
 *      related event
 *   connected - True: device connected; false: device disconnected
 *
 * Returned Value:
 *   On success, zero (OK) is returned. On a failure, a negated errno value
 *   is returned indicating the nature of the failure
 *
 ****************************************************************************/

#ifdef CONFIG_USBHOST_HUB
static int at32_connect(struct usbhost_driver_s *drvr,
                         struct usbhost_hubport_s *hport,
                         bool connected)
{
  struct at32_usbhost_s *priv = (struct at32_usbhost_s *)drvr;
  irqstate_t flags;

  DEBUGASSERT(priv != NULL && hport != NULL);

  /* Set the connected/disconnected flag */

  hport->connected = connected;
  uinfo("Hub port %d connected: %s\n",
        hport->port, connected ? "YES" : "NO");

  /* Report the connection event */

  flags = enter_critical_section();
  priv->hport = hport;
  if (priv->pscwait)
    {
      priv->pscwait = false;
      nxsem_post(&priv->pscsem);
    }

  leave_critical_section(flags);
  return OK;
}
#endif

/****************************************************************************
 * Name: at32_disconnect
 *
 * Description:
 *   Called by the class when an error occurs and driver has been
 *   disconnected.  The USB host driver should discard the handle to the
 *   class instance (it is stale) and not attempt any further interaction
 *   with the class driver instance (until a new instance is received from
 *   the create() method).  The driver should not call the class'
 *   disconnected() method.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the
 *     call to the class create() method.
 *   hport - The port from which the device is being disconnected.  Might be
 *      a port on a hub.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Never called from an interrupt handler.
 *
 ****************************************************************************/

static void at32_disconnect(struct usbhost_driver_s *drvr,
                             struct usbhost_hubport_s *hport)
{
  DEBUGASSERT(hport != NULL);
  hport->devclass = NULL;
}

/****************************************************************************
 * Name: at32_portreset
 *
 * Description:
 *   Reset the USB host port.
 *
 *   NOTE: "Before starting to drive a USB reset, the application waits for
 *   the OTG interrupt triggered by the debounce done bit (DBCDNE bit in
 *   OTG_FS_GOTGINT), which indicates that the bus is stable again after
 *   the electrical debounce caused by the attachment of a pull-up resistor
 *   on DP (FS) or DM (LS).
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void at32_portreset(struct at32_usbhost_s *priv)
{
  uint32_t regval;

  regval  = at32_getreg(AT32_OTGFS_HPRT);
  regval &= ~(OTGFS_HPRT_PENA | OTGFS_HPRT_PCDET | OTGFS_HPRT_PENCHNG |
              OTGFS_HPRT_POCCHNG);
  regval |= OTGFS_HPRT_PRST;
  at32_putreg(AT32_OTGFS_HPRT, regval);

  up_mdelay(20);

  regval &= ~OTGFS_HPRT_PRST;
  at32_putreg(AT32_OTGFS_HPRT, regval);

  up_mdelay(20);
}

/****************************************************************************
 * Name: at32_flush_txfifos
 *
 * Description:
 *   Flush the selected Tx FIFO.
 *
 * Input Parameters:
 *   txfnum -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void at32_flush_txfifos(uint32_t txfnum)
{
  uint32_t regval;
  uint32_t timeout;

  /* Initiate the TX FIFO flush operation */

  regval = OTGFS_GRSTCTL_TXFFLSH | txfnum;
  at32_putreg(AT32_OTGFS_GRSTCTL, regval);

  /* Wait for the FLUSH to complete */

  for (timeout = 0; timeout < AT32_FLUSH_DELAY; timeout++)
    {
      regval = at32_getreg(AT32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_TXFFLSH) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);
}

/****************************************************************************
 * Name: at32_flush_rxfifo
 *
 * Description:
 *   Flush the Rx FIFO.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void at32_flush_rxfifo(void)
{
  uint32_t regval;
  uint32_t timeout;

  /* Initiate the RX FIFO flush operation */

  at32_putreg(AT32_OTGFS_GRSTCTL, OTGFS_GRSTCTL_RXFFLSH);

  /* Wait for the FLUSH to complete */

  for (timeout = 0; timeout < AT32_FLUSH_DELAY; timeout++)
    {
      regval = at32_getreg(AT32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_RXFFLSH) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);
}

/****************************************************************************
 * Name: at32_vbusdrive
 *
 * Description:
 *   Drive the Vbus +5V.
 *
 * Input Parameters:
 *   priv  - USB host driver private data structure.
 *   state - True: Drive, False: Don't drive
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void at32_vbusdrive(struct at32_usbhost_s *priv, bool state)
{
  uint32_t regval;

#ifdef CONFIG_AT32_OTGFS_VBUS_CONTROL
  /* Enable/disable the external charge pump */

  at32_usbhost_vbusdrive(0, state);
#endif

  /* Turn on the Host port power. */

  regval = at32_getreg(AT32_OTGFS_HPRT);
  regval &= ~(OTGFS_HPRT_PENA | OTGFS_HPRT_PCDET | OTGFS_HPRT_PENCHNG |
              OTGFS_HPRT_POCCHNG);

  if (((regval & OTGFS_HPRT_PPWR) == 0) && state)
    {
      regval |= OTGFS_HPRT_PPWR;
      at32_putreg(AT32_OTGFS_HPRT, regval);
    }

  if (((regval & OTGFS_HPRT_PPWR) != 0) && !state)
    {
      regval &= ~OTGFS_HPRT_PPWR;
      at32_putreg(AT32_OTGFS_HPRT, regval);
    }

  up_mdelay(200);
}

/****************************************************************************
 * Name: at32_host_initialize
 *
 * Description:
 *   Initialize/re-initialize hardware for host mode operation.  At present,
 *   this function is called only from at32_hw_initialize().  But if OTG
 *   mode were supported, this function would also be called to switch
 *   between host and device modes on a connector ID change interrupt.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void at32_host_initialize(struct at32_usbhost_s *priv)
{
  uint32_t regval;
  uint32_t offset;
  int i;

  /* Restart the PHY Clock */

  at32_putreg(AT32_OTGFS_PCGCCTL, 0);

  /* Initialize Host Configuration (HCFG) register */

  regval  = at32_getreg(AT32_OTGFS_HCFG);
  regval &= ~OTGFS_HCFG_FSLSPCS_MASK;
  regval |= OTGFS_HCFG_FSLSPCS_FS48MHz;
  at32_putreg(AT32_OTGFS_HCFG, regval);

  /* Reset the host port */

  at32_portreset(priv);

  /* Clear the FS-/LS-only support bit in the HCFG register */

  regval = at32_getreg(AT32_OTGFS_HCFG);
  regval &= ~OTGFS_HCFG_FSLSS;
  at32_putreg(AT32_OTGFS_HCFG, regval);

  /* Carve up FIFO memory for the Rx FIFO and the periodic
   * and non-periodic Tx FIFOs
   */

  /* Configure Rx FIFO size (GRXFSIZ) */

  at32_putreg(AT32_OTGFS_GRXFSIZ, CONFIG_AT32_OTGFS_RXFIFO_SIZE);
  offset = CONFIG_AT32_OTGFS_RXFIFO_SIZE;

  /* Setup the host non-periodic Tx FIFO size (HNPTXFSIZ) */

  regval = (offset |
            (CONFIG_AT32_OTGFS_NPTXFIFO_SIZE <<
            OTGFS_HNPTXFSIZ_NPTXFD_SHIFT));
  at32_putreg(AT32_OTGFS_HNPTXFSIZ, regval);
  offset += CONFIG_AT32_OTGFS_NPTXFIFO_SIZE;

  /* Set up the host periodic Tx fifo size register (HPTXFSIZ) */

  regval = (offset |
            (CONFIG_AT32_OTGFS_PTXFIFO_SIZE <<
            OTGFS_HPTXFSIZ_PTXFD_SHIFT));
  at32_putreg(AT32_OTGFS_HPTXFSIZ, regval);

  /* If OTG were supported, we should need to clear HNP enable bit in the
   * USB_OTG control register about here.
   */

  /* Flush all FIFOs */

  at32_flush_txfifos(OTGFS_GRSTCTL_TXFNUM_HALL);
  at32_flush_rxfifo();

  /* Clear all pending HC Interrupts */

  for (i = 0; i < AT32_NHOST_CHANNELS; i++)
    {
      at32_putreg(AT32_OTGFS_HCINT(i), 0xffffffff);
      at32_putreg(AT32_OTGFS_HCINTMSK(i), 0);
    }

  /* Drive Vbus +5V (the smoke test).  Should be done elsewhere in OTG
   * mode.
   */

  at32_vbusdrive(priv, true);

  /* Enable host interrupts */

  at32_hostinit_enable();
}

/****************************************************************************
 * Name: at32_sw_initialize
 *
 * Description:
 *   One-time setup of the host driver state structure.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static inline void at32_sw_initialize(struct at32_usbhost_s *priv)
{
  struct usbhost_driver_s *drvr;
  struct usbhost_hubport_s *hport;
  int i;

  /* Initialize the device operations */

  drvr                 = &priv->drvr;
  drvr->ep0configure   = at32_ep0configure;
  drvr->epalloc        = at32_epalloc;
  drvr->epfree         = at32_epfree;
  drvr->alloc          = at32_alloc;
  drvr->free           = at32_free;
  drvr->ioalloc        = at32_ioalloc;
  drvr->iofree         = at32_iofree;
  drvr->ctrlin         = at32_ctrlin;
  drvr->ctrlout        = at32_ctrlout;
  drvr->transfer       = at32_transfer;
#ifdef CONFIG_USBHOST_ASYNCH
  drvr->asynch         = at32_asynch;
#endif
  drvr->cancel         = at32_cancel;
#ifdef CONFIG_USBHOST_HUB
  drvr->connect        = at32_connect;
#endif
  drvr->disconnect     = at32_disconnect;

  /* Initialize the public port representation */

  hport                = &priv->rhport.hport;
  hport->drvr          = drvr;
#ifdef CONFIG_USBHOST_HUB
  hport->parent        = NULL;
#endif
  hport->ep0           = (usbhost_ep_t)&priv->ep0;
  hport->speed         = USB_SPEED_FULL;

  /* Initialize function address generation logic */

  usbhost_devaddr_initialize(&priv->devgen);
  priv->rhport.pdevgen = &priv->devgen;

  /* Initialize the driver state data */

  priv->smstate   = SMSTATE_DETACHED;
  priv->connected = false;
  priv->change    = false;

  /* Put all of the channels back in their initial, allocated state */

  memset(priv->chan, 0, AT32_MAX_TX_FIFOS * sizeof(struct at32_chan_s));

  /* Initialize each channel */

  for (i = 0; i < AT32_MAX_TX_FIFOS; i++)
    {
      struct at32_chan_s *chan = &priv->chan[i];

      chan->chidx = i;
      nxsem_init(&chan->waitsem, 0, 0);
    }
}

/****************************************************************************
 * Name: at32_hw_initialize
 *
 * Description:
 *   One-time setup of the host controller hardware for normal operations.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static inline int at32_hw_initialize(struct at32_usbhost_s *priv)
{
  uint32_t regval;
  unsigned long timeout;

  /* Set the PHYSEL bit in the GUSBCFG register to select the OTG FS serial
   * transceiver: "This bit is always 1 with write-only access"
   */

  regval = at32_getreg(AT32_OTGFS_GUSBCFG);
  at32_putreg(AT32_OTGFS_GUSBCFG, regval);

  /* Reset after a PHY select and set Host mode.  First, wait for AHB master
   * IDLE state.
   */

  for (timeout = 0; timeout < AT32_READY_DELAY; timeout++)
    {
      up_udelay(3);
      regval = at32_getreg(AT32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_AHBIDL) != 0)
        {
          break;
        }
    }

  /* Then perform the core soft reset. */

  at32_putreg(AT32_OTGFS_GRSTCTL, OTGFS_GRSTCTL_CSRST);
  for (timeout = 0; timeout < AT32_READY_DELAY; timeout++)
    {
      regval = at32_getreg(AT32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_CSRST) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);

  /* Deactivate the power down */

  regval  = OTGFS_GCCFG_PWRDWN;
#if !defined(CONFIG_USBDEV_VBUSSENSING) && !defined(CONFIG_AT32_OTGFS_VBUS_CONTROL)
  regval |= OTGFS_GCCFG_NOVBUSSENS;
#endif
#ifdef CONFIG_AT32_OTGFS_SOFOUTPUT
  regval |= OTGFS_GCCFG_SOFOUTEN;
#endif
  at32_putreg(AT32_OTGFS_GCCFG, regval);
  up_mdelay(20);

  /* Initialize OTG features:  In order to support OTP, the HNPCAP and SRPCAP
   * bits would need to be set in the GUSBCFG register about here.
   */

  /* Force Host Mode */

  regval  = at32_getreg(AT32_OTGFS_GUSBCFG);
  regval &= ~OTGFS_GUSBCFG_FDMOD;
  regval |= OTGFS_GUSBCFG_FHMOD;
  at32_putreg(AT32_OTGFS_GUSBCFG, regval);
  up_mdelay(50);

  /* Initialize host mode and return success */

  at32_host_initialize(priv);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: at32_otgfshost_initialize
 *
 * Description:
 *   Initialize USB host device controller hardware.
 *
 * Input Parameters:
 *   controller -- If the device supports more than USB host controller, then
 *     this identifies which controller is being initialized.  Normally, this
 *     is just zero.
 *
 * Returned Value:
 *   And instance of the USB host interface.  The controlling task should
 *   use this interface to (1) call the wait() method to wait for a device
 *   to be connected, and (2) call the enumerate() method to bind the device
 *   to a class driver.
 *
 * Assumptions:
 * - This function should called in the initialization sequence in order
 *   to initialize the USB device functionality.
 * - Class drivers should be initialized prior to calling this function.
 *   Otherwise, there is a race condition if the device is already connected.
 *
 ****************************************************************************/

struct usbhost_connection_s *at32_otgfshost_initialize(int controller)
{
  /* At present, there is only support for a single OTG FS host. Hence it is
   * pre-allocated as g_usbhost.  However, in most code, the private data
   * structure will be referenced using the 'priv' pointer (rather than the
   * global data) in order to simplify any future support for multiple
   * devices.
   */

  struct at32_usbhost_s *priv = &g_usbhost;

  /* Sanity checks */

  DEBUGASSERT(controller == 0);

  /* Make sure that interrupts from the OTG FS core are disabled */

  at32_gint_disable();

  /* Reset the state of the host driver */

  at32_sw_initialize(priv);

  /* Alternate function pin configuration.  Here we assume that:
   *
   * 1. GPIOA, SYSCFG, and OTG FS peripheral clocking have already been\
   *    enabled as part of the boot sequence.
   * 2. Board-specific logic has already enabled other board specific GPIOs
   *    for things like soft pull-up, VBUS sensing, power controls, and over-
   *    current detection.
   */

  /* Configure OTG FS alternate function pins for DM, DP, ID, and SOF.
   *
   * PIN* SIGNAL      DIRECTION
   * ---- ----------- ----------
   * PA8  OTG_FS_SOF  SOF clock output
   * PA9  OTG_FS_VBUS VBUS input for device, Driven by external regulator by
   *                  host (not an alternate function)
   * PA10 OTG_FS_ID   OTG ID pin (only needed in Dual mode)
   * PA11 OTG_FS_DM   D- I/O
   * PA12 OTG_FS_DP   D+ I/O
   *
   * *Pins may vary from device-to-device.
   */

  at32_configgpio(GPIO_OTGFS_DM);
  at32_configgpio(GPIO_OTGFS_DP);
#ifdef CONFIG_USBDEV
  at32_configgpio(GPIO_OTGFS_ID);    /* Only needed for OTG */
#endif

  /* SOF output pin configuration is configurable */

#ifdef CONFIG_AT32_OTGFS_SOFOUTPUT
  at32_configgpio(GPIO_OTGFS_SOF);
#endif

  /* Initialize the USB OTG FS core */

  at32_hw_initialize(priv);

  /* Attach USB host controller interrupt handler */

  if (irq_attach(AT32_IRQ_OTGFS, at32_gint_isr, NULL) != 0)
    {
      usbhost_trace1(OTGFS_TRACE1_IRQATTACH, 0);
      return NULL;
    }

  /* Enable USB OTG FS global interrupts */

  at32_gint_enable();

  /* Enable interrupts at the interrupt controller */

  up_enable_irq(AT32_IRQ_OTGFS);
  return &g_usbconn;
}

#endif /* CONFIG_AT32_USBHOST && CONFIG_AT32_OTGFS */
