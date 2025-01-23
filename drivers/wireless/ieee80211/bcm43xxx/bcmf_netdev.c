/****************************************************************************
 * drivers/wireless/ieee80211/bcm43xxx/bcmf_netdev.c
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
#if defined(CONFIG_NET) && defined(CONFIG_IEEE80211_BROADCOM_FULLMAC)

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/wqueue.h>
#include <nuttx/net/netdev.h>
#include <nuttx/wireless/wireless.h>
#include <nuttx/wireless/ieee80211/bcmf_board.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

#include "bcmf_driver.h"
#include "bcmf_cdc.h"
#include "bcmf_bdc.h"
#include "bcmf_ioctl.h"
#include "bcmf_netdev.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* If processing is not done at the interrupt level, then work queue support
 * is required.
 */

#if !defined(CONFIG_SCHED_WORKQUEUE)
#  error Work queue support is required in this configuration (CONFIG_SCHED_WORKQUEUE)
#endif

/* The low priority work queue is preferred.  If it is not enabled, LPWORK
 * will be the same as HPWORK.
 *
 * Use high priority queue if the bcmf daemon task has a higher priority
 * than HPWORK, which will bring better performance especially on devices
 * that focus on real-time of network.
 */

#if defined(CONFIG_SCHED_HPWORK) && \
    (CONFIG_IEEE80211_BROADCOM_SCHED_PRIORITY >= CONFIG_SCHED_HPWORKPRIORITY)
#  define BCMFWORK HPWORK
#else
#  define BCMFWORK LPWORK
#endif

/* CONFIG_IEEE80211_BROADCOM_NINTERFACES determines the number of physical
 * interfaces that will be supported.
 */

#ifndef CONFIG_IEEE80211_BROADCOM_NINTERFACES
#  define CONFIG_IEEE80211_BROADCOM_NINTERFACES 1
#endif

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
#  define LP_IFDOWN_TIMEOUT CONFIG_IEEE80211_BROADCOM_LP_IFDOWN_TIMEOUT
#  define LP_DTIM_TIMEOUT   CONFIG_IEEE80211_BROADCOM_LP_DTIM_TIMEOUT
#  define LP_DTIM_INTERVAL  CONFIG_IEEE80211_BROADCOM_LP_DTIM_INTERVAL
#endif

/* This is a helper pointer for accessing the contents of Ethernet header */

#define BUF(iface) ((FAR struct eth_hdr_s *)priv->bc_dev[iface].d_buf)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Common TX logic */

static int  bcmf_transmit(FAR struct bcmf_dev_s *priv,
                          FAR struct bcmf_frame_s *frame, uint8_t iface);
static void bcmf_receive(FAR struct bcmf_dev_s *priv);
static int  bcmf_txpoll(FAR struct net_driver_s *dev);
static void bcmf_rxpoll_work(FAR void *arg);

/* NuttX callback functions */

static int  bcmf_ifup(FAR struct net_driver_s *dev);
static int  bcmf_ifdown(FAR struct net_driver_s *dev);

static int  bcmf_txavail(FAR struct net_driver_s *dev);

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int  bcmf_addmac(FAR struct net_driver_s *dev,
                        FAR const uint8_t *mac);
#ifdef CONFIG_NET_MCASTGROUP
static int  bcmf_rmmac(FAR struct net_driver_s *dev,
                       FAR const uint8_t *mac);
#endif
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int  bcmf_ioctl(FAR struct net_driver_s *dev, int cmd,
                       unsigned long arg);
#endif

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
static void bcmf_lowpower_poll(FAR struct bcmf_dev_s *priv);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

int bcmf_netdev_alloc_tx_frame(FAR struct bcmf_dev_s *priv, uint8_t iface)
{
  if (priv->tx_frame[iface] != NULL)
    {
      /* Frame available */

      return OK;
    }

  /* Allocate frame for TX */

  priv->tx_frame[iface] = bcmf_bdc_allocate_frame(priv,
                                                  MAX_NETDEV_PKTSIZE, false);
  if (!priv->tx_frame[iface])
    {
      wlinfo("INFO: Cannot allocate TX frame\n");
      return -ENOMEM;
    }

  priv->bc_dev[iface].d_buf = priv->tx_frame[iface]->data;
  priv->bc_dev[iface].d_len = 0;

  return OK;
}

/****************************************************************************
 * Name: bcmf_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Input Parameters:
 *   priv  - Reference to the driver state structure
 *   frame - Reference to the buffer to be send
 *   iface   - Interface number
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   the network is locked.
 *
 ****************************************************************************/

static int bcmf_transmit(FAR struct bcmf_dev_s *priv,
                         struct bcmf_frame_s *frame, uint8_t iface)
{
  int ret;

  frame->len = priv->bc_dev[iface].d_len +
      (unsigned int)(frame->data - frame->base);

  ret = bcmf_bdc_transmit_frame(priv, frame, iface);

  if (ret)
    {
      wlerr("ERROR: Failed to transmit frame\n");
      return -EIO;
    }

  NETDEV_TXPACKETS(&priv->bc_dev[iface]);

  return OK;
}

/****************************************************************************
 * Name: bcmf_receive
 *
 * Description:
 *   An interrupt was received indicating the availability of a new RX packet
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void bcmf_receive(FAR struct bcmf_dev_s *priv)
{
  struct bcmf_frame_s *frame;
  uint8_t iface;

  do
    {
      /* Request frame buffer from bus interface */

      frame = bcmf_bdc_rx_frame(priv, &iface);

      if (frame == NULL)
        {
          /* No more frame to process */

          bcmf_netdev_notify_tx(priv, (1 << CHIP_STA_INTERFACE) | \
                                      (1 << CHIP_AP_INTERFACE));
          break;
        }

      if (!priv->bc_bifup[iface])
        {
          /* Interface down, drop frame */

          priv->bus->free_frame(priv, frame);
          continue;
        }

      priv->bc_dev[iface].d_buf = frame->data;
      priv->bc_dev[iface].d_len = frame->len - (frame->data - frame->base);

#ifdef CONFIG_NET_PKT
      /* When packet sockets are enabled, feed the frame into the tap */

       pkt_input(&priv->bc_dev[iface]);
#endif

      /* Check if this is an 802.1Q VLAN tagged packet */

      if (BUF(iface)->type == HTONS(TPID_8021QVLAN))
        {
          /* Need to remove the 4 octet VLAN Tag, by moving src and dest
           * addresses 4 octets to the right, and then read the actual
           * ethertype. The VLAN ID and priority fields are currently
           * ignored.
           */

          /* ### TODO ### Implement VLAN support */

          uint8_t temp_buffer[12];
          memcpy(temp_buffer, frame->data, 12);
          memcpy(frame->data + 4, temp_buffer, 12);

          priv->bc_dev[iface].d_buf = frame->data = frame->data + 4;
          priv->bc_dev[iface].d_len -= 4;
        }

      /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
      if (BUF(iface)->type == HTONS(ETHTYPE_IP))
        {
          ninfo("IPv4 frame\n");
          NETDEV_RXIPV4(&priv->bc_dev[iface]);

          /* Receive an IPv4 packet from the network device */

          ipv4_input(&priv->bc_dev[iface]);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, d_len field will set to a value > 0.
           */

          if (priv->bc_dev[iface].d_len > 0)
            {
              /* And send the packet */

              bcmf_transmit(priv, frame, iface);
            }
          else
            {
              /* Release RX frame buffer */

              priv->bus->free_frame(priv, frame);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (BUF(iface)->type == HTONS(ETHTYPE_IP6))
        {
          ninfo("IPv6 frame\n");
          NETDEV_RXIPV6(&priv->bc_dev[iface]);

          /* Give the IPv6 packet to the network layer */

          ipv6_input(&priv->bc_dev[iface]);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, d_len field will set to a value > 0.
           */

          if (priv->bc_dev[iface].d_len > 0)
            {
              /* And send the packet */

              bcmf_transmit(priv, frame, iface);
            }
          else
            {
              /* Release RX frame buffer */

              priv->bus->free_frame(priv, frame);
            }
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (BUF(iface)->type == HTONS(ETHTYPE_ARP))
        {
          arp_input(&priv->bc_dev[iface]);
          NETDEV_RXARP(&priv->bc_dev[iface]);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, d_len field will set to a value > 0.
           */

          if (priv->bc_dev[iface].d_len > 0)
            {
              bcmf_transmit(priv, frame, iface);
            }
          else
            {
              /* Release RX frame buffer */

              priv->bus->free_frame(priv, frame);
            }
        }
      else
#endif
        {
          /* On some routers, it may constantly receive mysterious packet...
           * https://www.wireshark.org/docs/wsar_html/epan/etypes_8h.html
           * for more etypes definitions.
           */

          NETDEV_RXDROPPED(&priv->bc_dev[iface]);
          priv->bus->free_frame(priv, frame);
        }
    }
  while (1); /* While there are more packets to be processed */
}

/****************************************************************************
 * Name: bcmf_txpoll
 *
 * Description:
 *   The transmitter is available, check if the network has any outgoing
 *   packets ready to send.  This is a callback from devif_poll().
 *   devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send times out and the interface is
 *      reset
 *   3. During normal TX polling
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   the network is locked.
 *
 ****************************************************************************/

static int bcmf_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)dev->d_private;
  uint8_t iface = (dev->d_ifindex - 1) % CHIP_MAX_INTERFACE;

  /* Send the packet */

  if (priv->tx_frame[iface] != NULL)
    {
      bcmf_transmit(priv, priv->tx_frame[iface], iface);
    }

  /* TODO: Check if there is room in the device to hold another
   * packet. If not, return a non-zero value to terminate the poll.
   */

  priv->tx_frame[iface] = NULL;
  return 1;
}

/****************************************************************************
 * Function: bcmf_tx_poll_work
 *
 * Description:
 *   The function is called in order to perform an out-of-sequence TX poll.
 *   This is done:
 *
 *   1. After completion of a transmission (bcmf_netdev_notify_tx), and
 *   2. When new TX data is available (bcmf_txavail).
 *
 * Input Parameters:
 *   arg - context of device to use
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static void bcmf_tx_poll_work(FAR void *arg)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)arg;

  net_lock();

  for (uint8_t iface = 0; iface < CHIP_MAX_INTERFACE; iface++)
    {
      /* Ignore the notification if the interface is not yet up */

      if ((priv->tx_imask & (1 << iface)) && priv->bc_bifup[iface])
        {
          /* Check if there is room in the hardware to hold another packet. */

          while (bcmf_netdev_alloc_tx_frame(priv, iface) == OK)
            {
              /* If so, then poll the network for new XMIT data */

              devif_poll(&priv->bc_dev[iface], bcmf_txpoll);

              /* Break out the continuous send if IP stack has
               * no data to send.
               */

              if (priv->tx_frame[iface] != NULL)
                {
                  break;
                }
            }
        }
    }

  priv->tx_imask = 0;

  net_unlock();
}

/****************************************************************************
 * Name: bcmf_rxpoll_work
 *
 * Description:
 *   Process RX frames
 *
 * Input Parameters:
 *   arg - context of device to use
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *
 ****************************************************************************/

static void bcmf_rxpoll_work(FAR void *arg)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)arg;
  FAR void *oldbuf[CHIP_MAX_INTERFACE];

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Tx work will hold the d_buf until there is data to send,
   * replace and cache the d_buf temporarily
   */

  /* Save the driver buffers */

  for (uint8_t iface = 0; iface < CHIP_MAX_INTERFACE; iface++)
    {
      oldbuf[iface] = priv->bc_dev[iface].d_buf;
    }

  /* Process the received frame */

  bcmf_receive(priv);

  /* Restore the driver buffers */

  for (uint8_t iface = 0; iface < CHIP_MAX_INTERFACE; iface++)
    {
      priv->bc_dev[iface].d_buf = oldbuf[iface];
    }

  /* Check if a packet transmission just completed.  If so, call bcmf_txdone.
   * This may disable further Tx interrupts if there are no pending
   * transmissions.
   */

#if 0
  bcmf_txdone(priv);
#endif
  net_unlock();
}

/****************************************************************************
 * Name: bcmf_netdev_notify_tx
 *
 * Description:
 *   Notify callback called when TX frame is avail or sent.
 *
 * Assumptions:
 *
 ****************************************************************************/

void bcmf_netdev_notify_tx(FAR struct bcmf_dev_s *priv, uint8_t tx_imask)
{
  /* Schedule to perform a poll for new Tx data the worker thread. */

  if (work_available(&priv->bc_pollwork))
    {
      priv->tx_imask = tx_imask;
      work_queue(BCMFWORK, &priv->bc_pollwork,
                 bcmf_tx_poll_work, priv, 0);
    }
}

/****************************************************************************
 * Name: bcmf_netdev_notify_rx
 *
 * Description:
 *   Notify callback called when RX frame is available
 *
 * Assumptions:
 *
 ****************************************************************************/

void bcmf_netdev_notify_rx(FAR struct bcmf_dev_s *priv)
{
  /* Queue a job to process RX frames */

  if (work_available(&priv->bc_rxwork))
    {
      work_queue(BCMFWORK, &priv->bc_rxwork, bcmf_rxpoll_work, priv, 0);
    }
}

/****************************************************************************
 * Name: bcmf_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int bcmf_ifup(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)dev->d_private;
  uint8_t iface = (dev->d_ifindex - 1) % CHIP_MAX_INTERFACE;
  struct ether_addr zmac;
  irqstate_t flags;
  uint32_t out_len;
  int ret = OK;

  /* Disable the hardware interrupt */

  flags = enter_critical_section();

  if (priv->bc_bifup[iface])
    {
      goto errout_in_critical_section;
    }

  if (iface == CHIP_STA_INTERFACE)
    {
      ret = bcmf_wl_active(priv, true);
      if (ret != OK)
        {
          goto errout_in_critical_section;
        }

      /* Enable chip */

      ret = bcmf_wl_enable(priv, true, iface);
      if (ret != OK)
        {
          goto errout_in_wl_active;
        }

      /* Set customized MAC address */

      memset(&zmac, 0, sizeof(zmac));

      if (memcmp(&priv->bc_dev[iface].d_mac.ether, &zmac, sizeof(zmac)) != 0)
        {
          out_len = ETHER_ADDR_LEN;
          bcmf_cdc_iovar_request(priv, CHIP_STA_INTERFACE, true,
                                 IOVAR_STR_CUR_ETHERADDR,
                                 priv->bc_dev[iface].d_mac.ether.ether_addr_octet,
                                 &out_len);
        }

      /* Query MAC address */

      out_len = ETHER_ADDR_LEN;
      ret = bcmf_cdc_iovar_request(priv, CHIP_STA_INTERFACE, false,
                                   IOVAR_STR_CUR_ETHERADDR,
                                   priv->bc_dev[iface].d_mac.ether.ether_addr_octet,
                                   &out_len);
      if (ret != OK)
        {
          goto errout_in_wl_active;
        }

      if (CONFIG_IEEE80211_BROADCOM_DEFAULT_COUNTRY[0])
        {
          bcmf_wl_set_country_code(priv, CHIP_STA_INTERFACE,
                                  CONFIG_IEEE80211_BROADCOM_DEFAULT_COUNTRY);
        }

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
      bcmf_lowpower_poll(priv);
#endif
      bcmf_wl_set_pta_priority(priv, IW_PTA_PRIORITY_COEX_HIGH);

      /* Enable the hardware interrupt */

      priv->bc_bifup[iface] = true;
    }
  else
    {
      /* Enable the hardware interrupt */

      priv->bc_bifup[iface] = true;

      /* Setup is comleted ? */

      if (priv->softap.setup)
        {
          /* Bring up the SoftAP */

          ret = bcmf_wl_ap_set_up(priv, true, WL_AP_UP_TIMEOUT);
          if (ret != OK)
            {
              goto errout_in_wl_active;
            }
        }
      else
        {
          /* Query MAC address */

          out_len = ETHER_ADDR_LEN;
          ret = bcmf_cdc_iovar_request(priv, CHIP_STA_INTERFACE, false,
                                      IOVAR_STR_CUR_ETHERADDR,
                                      priv->bc_dev[iface].d_mac.ether.ether_addr_octet,
                                      &out_len);
          if (ret != OK)
            {
              goto errout_in_wl_active;
            }
          priv->bc_dev[iface].d_mac.ether.ether_addr_octet[5]++;
        }
    }

  goto errout_in_critical_section;

errout_in_wl_active:
  bcmf_wl_active(priv, false);

errout_in_critical_section:
  leave_critical_section(flags);

  wlinfo("bcmf_ifup done: %d\n", ret);

  return ret;
}

/****************************************************************************
 * Name: bcmf_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int bcmf_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)dev->d_private;
  uint8_t iface = (dev->d_ifindex - 1) % CHIP_MAX_INTERFACE;
  irqstate_t flags;

  /* Disable the hardware interrupt */

  flags = enter_critical_section();

  if (iface == CHIP_STA_INTERFACE)
    {
      if (priv->bc_bifup[iface])
        {
          /* Mark the device "down" */

          priv->bc_bifup[iface] = false;

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
          if (!work_available(&priv->lp_work_dtim))
            {
              work_cancel(LPWORK, &priv->lp_work_dtim);
            }

          if (!work_available(&priv->lp_work_ifdown))
            {
              work_cancel(LPWORK, &priv->lp_work_ifdown);
            }
#endif
          bcmf_wl_set_pta_priority(priv, IW_PTA_PRIORITY_COEX_MAXIMIZED);

          bcmf_wl_enable(priv, false, iface);
          bcmf_wl_active(priv, false);
        }
    }
  else
    {
      if (priv->bc_bifup[iface])
        {
          /* Bring down the SoftAP */

          bcmf_wl_ap_set_up(priv, false, WL_AP_DOWN_TIMEOUT);

          /* Mark the device "down" */

          priv->bc_bifup[iface] = false;
        }
    }

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bcmf_lowpower_work
 *
 * Description:
 *   Process low power saving dueto work timer expiration
 *
 * Input Parameters:
 *   arg - context of device to use
 *
 ****************************************************************************/

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
static bool bcmf_lowpower_expiration(FAR struct bcmf_dev_s *priv,
                                     FAR struct work_s *work,
                                     worker_t worker, clock_t timeout)
{
  clock_t ticks;

  if (priv->bc_bifup[iface])
    {
      /* Disable the hardware interrupt */

      ticks = clock_systime_ticks() - priv->lp_ticks;

      if (ticks >= timeout)
        {
          return true;
        }
      else
        {
          work_queue(LPWORK, work, worker, priv, timeout - ticks);
        }
    }

  return false;
}

static void bcmf_lowpower_work(FAR void *arg)
{
  FAR struct bcmf_dev_s *priv = arg;

  if (bcmf_lowpower_expiration(arg, &priv->lp_work_dtim, bcmf_lowpower_work,
                               SEC2TICK(LP_DTIM_TIMEOUT)))
    {
      if (priv->bc_bifup[iface])
        {
          bcmf_wl_set_dtim(priv, LP_DTIM_INTERVAL);
        }
    }
}

static void bcmf_lowpower_ifdown_work(FAR void *arg)
{
  FAR struct bcmf_dev_s *priv = arg;

  if (bcmf_lowpower_expiration(arg, &priv->lp_work_ifdown,
                               bcmf_lowpower_ifdown_work,
                               SEC2TICK(LP_IFDOWN_TIMEOUT)))
    {
      if (priv->bc_bifup[iface])
        {
          netdev_ifdown(&priv->bc_dev[iface]);
        }
    }
}

/****************************************************************************
 * Name: bcmf_lowpower_poll
 *
 * Description:
 *   Polling low power
 *
 * Input Parameters:
 *   arg - context of device to use
 *
 ****************************************************************************/

static void bcmf_lowpower_poll(FAR struct bcmf_dev_s *priv)
{
  if (priv->bc_bifup[iface])
    {
      bcmf_wl_set_dtim(priv, 100); /* Listen-iterval to 100 ms */

      /* Disable the hardware interrupt */

      priv->lp_ticks = clock_systime_ticks();
      if (work_available(&priv->lp_work_dtim) &&
          priv->lp_dtim != LP_DTIM_INTERVAL)
        {
          work_queue(LPWORK, &priv->lp_work_dtim, bcmf_lowpower_work, priv,
                     SEC2TICK(LP_DTIM_TIMEOUT));
        }

      if (work_available(&priv->lp_work_ifdown))
        {
          work_queue(LPWORK, &priv->lp_work_ifdown,
                     bcmf_lowpower_ifdown_work, priv,
                     SEC2TICK(LP_IFDOWN_TIMEOUT));
        }
    }
}

#endif

/****************************************************************************
 * Name: bcmf_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called in normal user mode
 *
 ****************************************************************************/

static int bcmf_txavail(FAR struct net_driver_s *dev)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)dev->d_private;
  uint8_t iface = (dev->d_ifindex - 1) % CHIP_MAX_INTERFACE;

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
  bcmf_lowpower_poll(priv);
#endif
  bcmf_netdev_notify_tx(priv, 1 << iface);
  return OK;
}

/****************************************************************************
 * Name: bcmf_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int bcmf_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  return OK;
}
#endif

/****************************************************************************
 * Name: bcmf_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware
 *   multicast address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int bcmf_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  return OK;
}
#endif

/****************************************************************************
 * Name: bcmf_ioctl
 *
 * Description:
 *   Handle network IOCTL commands directed to this device.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *   cmd - The IOCTL command
 *   arg - The argument for the IOCTL command
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_IOCTL
static int bcmf_ioctl(FAR struct net_driver_s *dev, int cmd,
                      unsigned long arg)
{
  FAR struct bcmf_dev_s *priv = (FAR struct bcmf_dev_s *)dev->d_private;
  uint8_t iface = (dev->d_ifindex - 1) % CHIP_MAX_INTERFACE;
  int ret;

  if (!priv->bc_bifup[iface])
    {
      wlerr("ERROR: invalid state (unable to execute command: %x)\n", cmd);
      return -EPERM;
    }

  if ((ret = nxmutex_lock(&priv->ioctl_mutex)) < 0)
    {
      return ret;
    }

#ifdef CONFIG_IEEE80211_BROADCOM_LOWPOWER
  bcmf_lowpower_poll(priv);
#endif

  /* Decode and dispatch the driver-specific IOCTL command */

  switch (cmd)
    {
      case SIOCSIWSCAN:
        bcmf_wl_set_pta_priority(priv, IW_PTA_PRIORITY_WLAN_MAXIMIZED);
        ret = bcmf_wl_start_scan(priv, (struct iwreq *)arg);
        if (ret != OK)
          {
            bcmf_wl_set_pta_priority(priv, IFF_IS_RUNNING(dev->d_flags) ?
                                     IW_PTA_PRIORITY_BALANCED :
                                     IW_PTA_PRIORITY_COEX_HIGH);
          }
        break;

      case SIOCGIWSCAN:
        ret = bcmf_wl_get_scan_results(priv, (struct iwreq *)arg);
        if (ret != -EAGAIN)
          {
            bcmf_wl_set_pta_priority(priv, IFF_IS_RUNNING(dev->d_flags) ?
                                     IW_PTA_PRIORITY_BALANCED :
                                     IW_PTA_PRIORITY_COEX_HIGH);
          }
        break;

      case SIOCSIFHWADDR:    /* Set device MAC address */
        ret = bcmf_wl_set_mac_address(priv, (struct ifreq *)arg);
        break;

      case SIOCSIWAUTH:
        ret = bcmf_wl_set_auth_param(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWENCODEEXT:
        ret = bcmf_wl_set_encode_ext(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWFREQ:     /* Set channel/frequency (Hz) */
        ret = bcmf_wl_set_channel(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWFREQ:     /* Get channel/frequency (Hz) */
        ret = bcmf_wl_get_frequency(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWMODE:     /* Set operation mode */
        ret = bcmf_wl_set_mode(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWMODE:     /* Get operation mode */
        ret = bcmf_wl_get_mode(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWAP:       /* Set access point MAC addresses */
        ret = bcmf_wl_set_bssid(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWAP:       /* Get access point MAC addresses */
        ret = bcmf_wl_get_bssid(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWESSID:    /* Set ESSID (network name) */
        bcmf_wl_set_pta_priority(priv, IW_PTA_PRIORITY_WLAN_MAXIMIZED);
        ret = bcmf_wl_set_ssid(priv, (struct iwreq *)arg);
        if (ret != OK)
          {
            bcmf_wl_set_pta_priority(priv, IW_PTA_PRIORITY_COEX_HIGH);
          }
        else
          {
            bcmf_wl_set_pta_priority(priv, (bcmf_wl_get_channel(priv, CHIP_STA_INTERFACE) > 14)
                                            ? IW_PTA_PRIORITY_COEX_MAXIMIZED
                                            : IW_PTA_PRIORITY_BALANCED);
          }
        break;

      case SIOCGIWESSID:    /* Get ESSID */
        ret = bcmf_wl_get_ssid(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWRATE:     /* Set default bit rate (bps) */
        wlwarn("WARNING: SIOCSIWRATE not implemented\n");
        ret = -ENOSYS;
        break;

      case SIOCGIWRATE:     /* Get default bit rate (bps) */
        ret = bcmf_wl_get_rate(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWTXPOW:    /* Set transmit power (dBm) */
        wlwarn("WARNING: SIOCSIWTXPOW not implemented\n");
        ret = -ENOSYS;
        break;

      case SIOCGIWTXPOW:    /* Get transmit power (dBm) */
        ret = bcmf_wl_get_txpower(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWSENS:     /* Get transmit power (dBm) */
        ret = bcmf_wl_get_rssi(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWRANGE:    /* Get range of parameters */
        ret = bcmf_wl_get_iwrange(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWCOUNTRY:  /* Set country code */
        ret = bcmf_wl_set_country(priv, (struct iwreq *)arg);
        break;

      case SIOCGIWCOUNTRY:  /* Get country code */
        ret = bcmf_wl_get_country(priv, (struct iwreq *)arg);
        break;

#ifdef CONFIG_IEEE80211_BROADCOM_PTA_PRIORITY
      case SIOCGIWPTAPRIO:  /* Get Packet Traffic Arbitration */
        ret = bcmf_wl_get_pta(priv, (struct iwreq *)arg);
        break;

      case SIOCSIWPTAPRIO:  /* Set Packet Traffic Arbitration */
        ret = bcmf_wl_set_pta(priv, (struct iwreq *)arg);
        break;
#endif

      case SIOCSIWAPASSOC:
        wlwarn("WARNING: SIOCSIWAPASSOC not implemented\n");
        ret = -ENOSYS;
        break;

      case SIOCGIWAPASSOC:
        ret = bcmf_wl_ap_get_stas(priv, (struct iwreq *)arg);
        break;

      default:
        nerr("ERROR: Unrecognized IOCTL command: %x\n", cmd);
        ret = -ENOTTY;  /* Special return value for this case */
        break;
    }

  nxmutex_unlock(&priv->ioctl_mutex);

  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bcmf_netdev_register
 *
 * Description:
 *   Register a network driver and set Broadcom chip in a proper state
 *
 * Input Parameters:
 *   priv - Broadcom driver device
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

int bcmf_netdev_register(FAR struct bcmf_dev_s *priv)
{
  /* Initialize network driver structures */

  for (int i = 0; i < CHIP_MAX_INTERFACE; i++)
    {
      memset(&priv->bc_dev[i], 0, sizeof(priv->bc_dev[i]));
      priv->bc_dev[i].d_ifup    = bcmf_ifup;     /* I/F up (new IP address) callback */
      priv->bc_dev[i].d_ifdown  = bcmf_ifdown;   /* I/F down callback */
      priv->bc_dev[i].d_txavail = bcmf_txavail;  /* New TX data callback */
    #ifdef CONFIG_NET_MCASTGROUP
      priv->bc_dev[i].d_addmac  = bcmf_addmac;   /* Add multicast MAC address */
      priv->bc_dev[i].d_rmmac   = bcmf_rmmac;    /* Remove multicast MAC address */
    #endif
    #ifdef CONFIG_NETDEV_IOCTL
      priv->bc_dev[i].d_ioctl   = bcmf_ioctl;    /* Handle network IOCTL commands */
    #endif
      priv->bc_dev[i].d_private = priv;          /* Used to recover private state from dev */

      /* Initialize network stack interface buffer */

      priv->bc_dev[i].d_buf     = NULL;
      priv->tx_frame[i]         = NULL;

      /* Initialize MAC address */

      bcmf_board_etheraddr(&priv->bc_dev[i].d_mac.ether, i);

      /* Register the device with the OS so that socket IOCTLs can be performed */

      netdev_register(&priv->bc_dev[i], NET_LL_IEEE80211);
    }

  return OK;
}

#endif /* CONFIG_NET && CONFIG_IEEE80211_BROADCOM_FULLMAC */
