/*!\file pcsed.c
 *
 *  Link-layer Driver Routines.
 *
 *  The TCP code uses Ethernet constants for protocol numbers and 48 bits
 *  for address. Also, 0xFF:FF:FF:FF:FF:FF is assumed to be a broadcast.
 *  Except for ARCNET where broadcast is 0x00.
 *
 *  If you need to write a new driver, implement it at this level and use
 *  the above mentioned constants as this program's constants, not device
 *  dependent constants.
 *
 *  The packet driver or WinPcap interface code lies below this module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "copyrigh.h"
#include "wattcp.h"
#include "wdpmi.h"
#include "language.h"
#include "sock_ini.h"
#include "loopback.h"
#include "cpumodel.h"
#include "misc.h"
#include "misc_str.h"
#include "run.h"
#include "timer.h"
#include "profile.h"
#include "split.h"
#include "ip4_in.h"
#include "ip6_in.h"
#include "bsddbug.h"
#include "pcigmp.h"
#include "pcqueue.h"
#include "pcconfig.h"
#include "pcdbug.h"
#include "pctcp.h"
#include "pcstat.h"
#include "pcpkt.h"
#include "pcsed.h"
#include "pppoe.h"

/* !!fix-me: Should allow the largest possible MAC-address to be used;
 *           An AX-25 address is 7 bytes.
 */
mac_address _eth_addr;               /**< Local link-layer source address */
mac_address _eth_brdcast;            /**< Link-layer broadcast address */
mac_address _eth_loop_addr;          /**< Link-layer loopback address */
mac_address _eth_real_addr;          /**< Our real MAC address */
BYTE        _eth_mac_len;            /**< Size of a MAC address */
BOOL        _eth_is_init   = FALSE;  /**< we are initialised */
BOOL        _ip_recursion  = FALSE;  /**< avoid recursion in arp_resolve() */
BOOL        _eth_ndis3pkt  = FALSE;  /**< for DOS-programs only */
BOOL        _eth_SwsVpkt   = FALSE;  /**< for DOS *and* Win32 programs */
BOOL        _eth_wanpacket = FALSE;  /**< for Win32 using an WanPacket adapter */
BOOL        _eth_npcap     = FALSE;  /**< for Win32 using an NPcap adapter */
BOOL        _eth_win10pcap = FALSE;  /**< for Win32 using an Win10Pcap adapter */
BOOL        _eth_winpcap   = FALSE;  /**< for Win32 using an WinPcap adapter (default) */

const char *_eth_not_init = "Packet driver not initialised";

/**
 * Sizes and timestamps of last packet recv/sent.
 */
struct _eth_last_info _eth_last;

/**
 * Pointer to functions that when set changes the behaviour of polling
 * and send functions _eth_arrived() and _eth_send().
 *
 * _eth_recv_hook:
 *   If set, must point to a function that supplies frames ready to be
 *   received. Return NULL if no frame is ready. Otherwise points to
 *   a raw MAC frame. If not a serial-driver, must also set '*type'.
 *
 * _eth_recv_peek:
 *   If set, must point to a function that gets a chance to peek at the
 *   raw received frame. It can modify it or even consume it by returning
 *   zero. To prevent recursion, this function cannot call tcp_tick(). It
 *   should return as fast as possible in order not to block reception of
 *   other frames waiting in the receive buffer.
 *
 * _eth_xmit_hook:
 *   If set, must point to a function that should transmit all frames
 *   generated by Watt-32. Must return length of frame sent or <= 0 if
 *   it failed.
 */
void *(W32_CALL *_eth_recv_hook) (WORD *type)                        = NULL;
int   (W32_CALL *_eth_recv_peek) (void *mac_buf)                     = NULL;
int   (W32_CALL *_eth_xmit_hook) (const void *mac_buf, unsigned len) = NULL;

/**
 * Pointer to functions that does the filling of correct MAC-header
 * and sends the link-layer packet. We store 'proto' between calls.
 */
static void *(*mac_tx_format)(void *mac_buf, const void *mac_dest, WORD type);
static int   (*mac_transmit) (const void *mac_buf, WORD len);

static WORD  proto;   /* protocol set in _eth_formatpacket() */
static void *nw_pkt;  /* where network protocol packet starts */

static void W32_CALL __eth_release (void);

/**
 * Output Tx-buffer.
 * We maintain only a single output buffer, and it gets used quickly
 * then released.  The benefits of non-blocking systems are immense.
 *
 * \note For DOS4GW/X32VM targets (and djgpp with near-pointers), we
 *       use the allocated Tx-buffer in low RAM. Hence no need to copy
 *       from `&outbuf' to low RAM.
 */
#if (DOSX & (DOS4GW|X32VM))
  #define TX_BUF()  ((union link_Packet*) pkt_tx_buf())

#elif (DOSX & DJGPP)
  static union link_Packet outbuf;

  #define TX_BUF() (_pkt_inf && _pkt_inf->use_near_ptr ? \
                     (union link_Packet*)pkt_tx_buf()  : \
                     (&outbuf))
#else
  static union link_Packet outbuf;
  #define TX_BUF()  (&outbuf)
#endif

/**
 * _eth_format_packet() places the next packet to be transmitted into
 * the above link-layer output packet.
 * \return address of higher-level protocol (IP/RARP/RARP) header.
 */
void * W32_CALL _eth_formatpacket (const void *mac_dest, WORD eth_type)
{
  nw_pkt = (*mac_tx_format) (TX_BUF(), mac_dest, eth_type);
  return (nw_pkt);
}


#if defined(USE_LOOPBACK)
/**
 * Enqueue a link-layer frame (IPv4/v6 only) to the internal loopback device.
 *
 * \note This function uses call-by-value. Thus `pkt' buffer can
 *       be modified by loopback_device() and loopback handler may
 *       send using _eth_send().
 *
 * \note Loopback device cannot send to itself (potential recursion).
 */
static int send_loopback (link_Packet pkt, BOOL is_ip6, unsigned *err_line)
{
  struct pkt_ringbuf *q;
  const  in_Header   *ip;
  int    ip_len;

  if (!_pkt_inf)
  {
    *err_line = __LINE__;
    goto drop_it;
  }

  /* Call loopback handler with IP-packet
   */
  ip     = (in_Header*) ((BYTE*)&pkt + _pkt_ip_ofs);
  ip_len = loopback_device ((in_Header*)ip);

  q = &_pkt_inf->pkt_queue;

  if (!q || ip_len > (int)_mtu)
  {
    *err_line = __LINE__;
    goto drop_it;
  }

  if (ip_len > 0)
  {
#if defined(USE_FAST_PKT)
    /*
     * Don't let pkt_receiver() modify the queue while testing/copying.
     */
    if (pkt_buffers_used() >= _pkt_inf->pkt_queue.num_buf - 1)
    {
      *err_line = __LINE__;
      goto drop_it;
    }
    {
      char     tx_buf [ETH_MAX];
      unsigned len = ip_len;

      /* Enqueue packet to head of input IP-queue.
       */
      if (!_pktserial)
      {
        void *data = (*mac_tx_format) (tx_buf, _eth_addr,
                                       is_ip6 ? IP6_TYPE : IP4_TYPE);
        memcpy (MAC_SRC(data), &_eth_loop_addr, sizeof(mac_address));
        memcpy (data, ip, ip_len);
        len += _pkt_ip_ofs;
      }
      else
        memcpy (tx_buf, ip, ip_len);

      if (!pkt_append_recv(tx_buf, len))
      {
        *err_line = __LINE__;
        goto drop_it;
      }
    }
#elif defined(WIN32)
    struct pkt_rx_element *head;

    ENTER_CRIT();
    if (pktq_in_index(q) == q->out_index)  /* queue is full, drop it */
    {
      q->num_drop++;
      LEAVE_CRIT();
      *err_line = __LINE__;
      goto drop_it;
    }

    head = (struct pkt_rx_element*) pktq_in_buf (q);
    head->rx_length = _pkt_ip_ofs + ip_len;
    head->tstamp_put = win_get_perf_count();

    /* Enqueue packet to head of input IP-queue.
     */
    if (!_pktserial)
    {
      void *data = (*mac_tx_format) (&head->rx_buf, _eth_addr,
                                     is_ip6 ? IP6_TYPE : IP4_TYPE);
      memcpy (MAC_SRC(data), &_eth_loop_addr, sizeof(mac_address));
      memcpy (data, ip, ip_len);
    }
    else
      memcpy (head, ip, ip_len);

    /* Update queue head index
     */
    q->in_index = pktq_in_index (q);

    LEAVE_CRIT();

#else
    union link_Packet *head;

    DISABLE();
    if (pktq_in_index(q) == q->out_index)  /* queue is full, drop it */
    {
      q->num_drop++;
      ENABLE();
      *err_line = __LINE__;
      goto drop_it;
    }

    head = (union link_Packet*) pktq_in_buf (q);

    /* Enqueue packet to head of input IP-queue.
     */
    if (!_pktserial)
    {
      void *data = (*mac_tx_format) (head, _eth_addr,
                                     is_ip6 ? IP6_TYPE : IP4_TYPE);
      memcpy (MAC_SRC(data), &_eth_loop_addr, sizeof(mac_address));
      memcpy (data, ip, ip_len);
    }
    else
      memcpy (head, ip, ip_len);

    /* Update queue head index
     */
    q->in_index = pktq_in_index (q);

    ENABLE();
#endif
  }
  *err_line = 0;
  return (ip_len + _pkt_ip_ofs);

drop_it:
  /*
   * Maybe this should be an input counter
   */
  if (is_ip6)
       STAT (ip6stats.ip6s_odropped++);
  else STAT (ip4stats.ips_odropped++);
  return (0);
}
#endif  /* USE_LOOPBACK */


/**
 * _eth_send() does the actual transmission once we are complete with
 * filling the buffer.  Do any last minute patches here, like fix the
 * size. Send to "loopback" device if it's IP and destination matches
 * loopback network (127.x.x.x.).
 *
 * Return length of network-layer packet (not length of link-layer
 * packet).
 */
int W32_CALL _eth_send (WORD len, const void *sock, const char *file, unsigned line)
{
#if defined(USE_DEBUG) || defined(USE_LOOPBACK)
  unsigned errline = 0;
#endif
  BOOL send_loopback_to_driver = FALSE;

  if (!_eth_is_init)  /* GvB 2002-09, Lets us run without a working eth */
  {
    SOCK_ERRNO (ENETDOWN);
    return (0);
  }

#if defined(WIN32)
  /*
   * Just a test for now; send it to the driver and look what happens....
   * They go on the wire and not to the Winsock loopback provider.
   * No surprise here yet.
   */
  if (loopback_mode & LBACK_MODE_WINSOCK)
     send_loopback_to_driver = TRUE;
#endif

  if (proto == IP4_TYPE)
  {
    /* Sending to loopback device if IPv4.
     */
    const in_Header *ip = (const in_Header*) nw_pkt;

    if (!send_loopback_to_driver &&
        _ip4_is_loopback_addr(intel(ip->destination)))
    {
#if defined(USE_LOOPBACK)
      len = send_loopback (*TX_BUF(), FALSE, &errline);
#else
      STAT (ip4stats.ips_odropped++);    /* packet dropped (null-device) */
#endif
      goto debug_tx;
    }
  }

#if defined(USE_IPV6)
  else if (proto == IP6_TYPE)
  {
    const in6_Header *ip = (const in6_Header*) nw_pkt;

    if (!send_loopback_to_driver &&
        IN6_IS_ADDR_LOOPBACK(&ip->destination))
    {
#if defined(USE_LOOPBACK)
      len = send_loopback (*TX_BUF(), TRUE, &errline);
#else
      STAT (ip6stats.ip6s_odropped++);
#endif
      goto debug_tx;
    }
  }
#endif  /* USE_IPV6 */

#if defined(USE_PPPOE)
  else if (proto == PPPOE_SESS_TYPE)
  {
    pppoe_Packet *pppoe = (pppoe_Packet*) TX_BUF()->eth.data;

    pppoe->length = intel16 (len+2);
    len += PPPOE_HDR_SIZE + 2;      /* add 2 for protocol */
  }
#endif

  /* Store the last Tx CPU timestamp (for debugging).
   */
#if (DOSX)
  if (debug_xmit && has_rdtsc)
  {
  #if defined(WIN32)
     uint64 cnt = win_get_perf_count();
     memcpy (&_eth_last.tx.tstamp, &cnt, sizeof(_eth_last.tx.tstamp));
  #else
     get_rdtsc2 (&_eth_last.tx.tstamp);
  #endif
  }
#endif

  /* Do the MAC-dependent transmit. `len' on return is total length
   * of link-layer packet sent. `len' is 0 on failure. The xmit-hook
   * is used by e.g. libpcap/libnet.
   */
  if (_eth_xmit_hook)
       len = (*_eth_xmit_hook) (TX_BUF(), len + _pkt_ip_ofs);
  else len = (*mac_transmit) (TX_BUF(), len + _pkt_ip_ofs);

  if (len > _pkt_ip_ofs)
  {
    _eth_last.tx.size = len;
    len -= _pkt_ip_ofs;
  }
  else
  {
    if (debug_on)
       outs ("Tx failed. ");
    len = 0;
    _eth_last.tx.size = 0;
  }

debug_tx:

#if defined(NEED_PKT_SPLIT)
  pkt_split_mac_out (TX_BUF());
#endif

#if defined(USE_STATISTICS)
  if (len > 0)
     update_out_stat();
#endif

#if defined(USE_DEBUG)
  if (debug_xmit)
    (*debug_xmit) (sock, (const in_Header*)nw_pkt, file, line);

  if (len == 0)
  {
    if (errline && !send_loopback_to_driver)
       dbug_printf ("** Error in loopback handler, line %u\n", errline);
    else
    {
      const char err[] = "** Transmit fault **\n";

      TRACE_CONSOLE (1, err);
      dbug_printf (err);
    }
  }
#else
  ARGSUSED (sock);
  ARGSUSED (file);
  ARGSUSED (line);
#endif

  /* Undo hack done in pppoe_mac_format()
   */
  if (proto == PPPOE_SESS_TYPE || _pktdevclass == PDCLASS_ETHER)
     _pkt_ip_ofs = sizeof(eth_Header);
  return (len);
}

/**
 * Format the MAC-header for Ethernet.
 */
static void *eth_mac_format (void *mac_buf, const void *mac_dest, WORD type)
{
  union link_Packet *buf = (union link_Packet*) mac_buf;

  proto = type;      /* remember protocol for _eth_send() */

  /* Clear any remains of an old small packet.
   */
  memset (&buf->eth.data[0], 0, ETH_MIN-sizeof(eth_Header));

#if defined(USE_PPPOE)
  if (type == IP4_TYPE && pppoe_is_up(mac_dest))
  {
    proto = PPPOE_SESS_TYPE;
    return pppoe_mac_format (buf);
  }
#endif

  if (mac_dest)
     memcpy (&buf->eth.head.destination, mac_dest, sizeof(mac_address));
  memcpy (&buf->eth.head.source, &_eth_addr, sizeof(mac_address));

  buf->eth.head.type = type;
  return (&buf->eth.data);
}

/**
 * Format the MAC-header for Token-Ring.
 */
static void *tok_mac_format (void *mac_buf, const void *mac_dest, WORD type)
{
  union link_Packet *buf = (union link_Packet*) mac_buf;

  /* No need to clear data behind header.
   */
  if (mac_dest)
     memcpy (&buf->tok.head.destination, mac_dest, sizeof(mac_address));
  memcpy (&buf->tok.head.source, &_eth_addr, sizeof(mac_address));

#if 0
  /* !!fix me: need to expand the RIF
   */
  if (_pktdevclass == PDCLASSS_TOKEN_RIF)
     ((void)0);
#endif

  buf->tok.head.accessCtrl = TR_AC;
  buf->tok.head.frameCtrl  = TR_FC;
  buf->tok.head.DSAP       = TR_DSAP;
  buf->tok.head.SSAP       = TR_SSAP;
  buf->tok.head.ctrl       = TR_CTRL;
  buf->tok.head.org[0]     = TR_ORG;
  buf->tok.head.org[1]     = TR_ORG;
  buf->tok.head.org[2]     = TR_ORG;
  buf->tok.head.type       = type;
  proto = type;
  return (&buf->tok.data);
}

/**
 * Format the MAC-header for FDDI.
 */
static void *fddi_mac_format (void *mac_buf, const void *mac_dest, WORD type)
{
  union link_Packet *buf = (union link_Packet*) mac_buf;

  memset (&buf->fddi.data[0], 0, FDDI_MIN-sizeof(fddi_Header));
  if (mac_dest)
     memcpy (&buf->fddi.head.destination, mac_dest, sizeof(mac_address));
  memcpy (&buf->fddi.head.source, &_eth_addr, sizeof(mac_address));

  buf->fddi.head.frameCtrl = FDDI_FC;
  buf->fddi.head.DSAP      = FDDI_DSAP;
  buf->fddi.head.SSAP      = FDDI_SSAP;
  buf->fddi.head.ctrl      = FDDI_CTRL;
  buf->fddi.head.org[0]    = FDDI_ORG;
  buf->fddi.head.org[1]    = FDDI_ORG;
  buf->fddi.head.org[2]    = FDDI_ORG;
  buf->fddi.head.type      = type;
  proto = type;
  return (&buf->fddi.data);
}

/**
 * Format the MAC-header for ARCNET.
 */
static void *arcnet_mac_format (void *mac_buf, const void *mac_dest, WORD type)
{
  union link_Packet *buf = (union link_Packet*) mac_buf;
  BYTE  dest;

  if (type == IP4_TYPE)     /* Map to DataPoint proto types */
      type = ARCNET_IP_1201;
  else if (type == IP6_TYPE)
      type = ARCNET_IP6;
  else if (type == ARP_TYPE)
      type = ARCNET_ARP_1201;
  else if (type == RARP_TYPE)
      type = ARCNET_RARP_1201;

  if (!mac_dest || !memcmp(mac_dest, &_eth_brdcast, sizeof(_eth_brdcast)))
       dest = 0x00;              /* map to ARCNET broadcast */
  else dest = *(BYTE*)mac_dest;  /* Use MSB as destination */

  buf->arc.head.source      = _eth_addr[0];
  buf->arc.head.destination = dest;
  buf->arc.head.type        = (BYTE) type;
  buf->arc.head.flags       = 0;     /* !! */
  buf->arc.head.sequence    = 0;     /* !! */
#if 0 /* should never be transmitted */
  buf->arc.head.type2       = type;
  buf->arc.head.flags2      = 0;     /* !! */
  buf->arc.head.sequence2   = 0;     /* !! */
#endif

  proto = (BYTE) type;
  return (void*) ((BYTE*)&buf->arc + ARC_HDRLEN);
}

/**
 * Format MAC-header for protocols without MAC-headers.
 * Nothing done here.
 */
static void *null_mac_format (void *mac_buf, const void *mac_dest, WORD type)
{
  union link_Packet *buf = (union link_Packet*) mac_buf;

  memset (&buf->ip.head, 0, sizeof(buf->ip.head));
  ARGSUSED (mac_dest);
  ARGSUSED (type);
  proto = IP4_TYPE;
  return (void*) (&buf->ip.head);
}

/**
 * Functions called via function pointer `mac_transmit' to
 * actually send the data.
 */
static int eth_mac_xmit (const void *buf, WORD len)
{
  if (len < ETH_MIN)
       len = ETH_MIN;  /* zero padding already done in eth_mac_format() */
  else if (len > ETH_MAX)
       len = ETH_MAX;

  return pkt_send (buf, len);
}

static int fddi_mac_xmit (const void *buf, WORD len)
{
  if (len < FDDI_MIN)
       len = FDDI_MIN;
  else if (len > FDDI_MAX)
       len = FDDI_MAX;

  return pkt_send (buf, len);
}

static int arcnet_mac_xmit (const void *buf, WORD len)
{
  if (len < ARCNET_MIN)
       len = ARCNET_MIN;
  else if (len > ARCNET_MAX)
       len = ARCNET_MAX;

  return pkt_send (buf, len);
}

static int tok_mac_xmit (const void *buf, WORD len)
{
  if (len > TOK_MAX)  /* Token-Ring has no min. length */
      len = TOK_MAX;
  return pkt_send (buf, len);
}

static int null_mac_xmit (const void *buf, WORD len)
{
  return pkt_send (buf, len);
}


/**
 * Initialize the network driver interface.
 * \return 0 okay.
 * \return error-code otherwise.
 */
int W32_CALL _eth_init (void)
{
  int rc;

  if (_eth_is_init)
     return (0);

  rc = pkt_eth_init (&_eth_addr);
  if (rc)
  {
    if (rc == WERR_NO_DRIVER)
    {
      /* Initialize to some sane default.  */
      mac_tx_format = null_mac_format;
      mac_transmit  = null_mac_xmit;
    }
    return (rc);  /* error message already printed */
  }

  /* Save our MAC-address incase we change it. Change back at exit.
   */
  memcpy (_eth_real_addr, _eth_addr, sizeof(_eth_real_addr));

  switch (_pktdevclass)
  {
    case PDCLASS_ETHER:
         mac_tx_format = eth_mac_format;
         mac_transmit  = eth_mac_xmit;
         break;
    case PDCLASS_TOKEN:
    case PDCLASS_TOKEN_RIF:
         mac_tx_format = tok_mac_format;
         mac_transmit  = tok_mac_xmit;
         break;
    case PDCLASS_FDDI:
         mac_tx_format = fddi_mac_format;
         mac_transmit  = fddi_mac_xmit;
         break;
    case PDCLASS_ARCNET:
         mac_tx_format = arcnet_mac_format;
         mac_transmit  = arcnet_mac_xmit;
         break;
    case PDCLASS_SLIP:
    case PDCLASS_PPP:
    case PDCLASS_AX25:  /* !! for now */
         mac_tx_format = null_mac_format;
         mac_transmit  = null_mac_xmit;
         break;
    default:
         outsnl (_LANG("No supported driver class found"));
         return (WERR_NO_DRIVER);
  }

  memset (TX_BUF(), 0, sizeof(union link_Packet));
  memset (&_eth_brdcast, 0xFF, sizeof(_eth_brdcast));
  _eth_loop_addr[0] = 0xCF;
  pkt_buf_wipe();

  if (!_eth_get_hwtype(NULL, &_eth_mac_len))
     _eth_mac_len = sizeof(eth_address);

#if defined(__MSDOS__)
  if (!strcmp(_pktdrvrname,"NDIS3PKT"))
     _eth_ndis3pkt = TRUE;

  else if (!strcmp(_pktdrvrname,"SwsVpkt"))
     _eth_SwsVpkt = TRUE;
#endif

  _eth_is_init = TRUE;
  RUNDOWN_ADD (__eth_release, 10);

  return (0);  /* everything okay */
}

/**
 * Sets a new MAC address for our interface.
 */
int W32_CALL _eth_set_addr (const void *addr)
{
  if (_pktserial || _pktdevclass == PDCLASS_ARCNET)
     return (1);

  if (pkt_set_addr(addr))
  {
    memcpy (_eth_addr, addr, sizeof(_eth_addr));
    return (1);
  }
  return (0);
}


/**
 * Fill in hardware address type/length for BOOTP/DHCP packets.
 * Also used for ARP/RARP packets. Should never be called for PPP/SLIP.
 */
BYTE W32_CALL _eth_get_hwtype (BYTE *hwtype, BYTE *hwlen)
{
  if (_pktdevclass == PDCLASS_ETHER)
  {
    if (hwlen)  *hwlen  = sizeof (eth_address);
    if (hwtype) *hwtype = HW_TYPE_ETHER;
    return (HW_TYPE_ETHER);
  }
  if (_pktdevclass == PDCLASS_FDDI)
  {
    if (hwlen)  *hwlen  = sizeof (fddi_address);
    if (hwtype) *hwtype = HW_TYPE_FDDI;
    return (HW_TYPE_FDDI);
  }
  if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
  {
    if (hwlen)  *hwlen  = sizeof (tok_address);
    if (hwtype) *hwtype = HW_TYPE_TOKEN;
    return (HW_TYPE_TOKEN);
  }
  if (_pktdevclass == PDCLASS_AX25)
  {
    if (hwlen)  *hwlen  = sizeof (ax25_address);
    if (hwtype) *hwtype = HW_TYPE_AX25;
    return (HW_TYPE_AX25);
  }
  if (_pktdevclass == PDCLASS_ARCNET)
  {
    if (hwlen)  *hwlen  = sizeof (arcnet_address);  /* 1 byte */
    if (hwtype) *hwtype = HW_TYPE_ARCNET;
    return (HW_TYPE_ARCNET);
  }
  return (0);
}


/**
 * Free an input buffer once it is no longer needed.
 */
void W32_CALL _eth_free (const void *pkt)
{
  if (_eth_recv_hook) /* hook-function should free it's own packet */
     return;

  if (!pkt)
       pkt_buf_wipe();   /* restart the queue */
  else pkt_free_pkt (pkt);
}


/**
 * Check a Token-Ring raw packet for RIF/RCF. Remove RCF if
 * found. '*trp' on output is corrected for dropped RCF.
 * These macros are taken from `tcpdump'.
 */
#define RCF DSAP   /* Routing Control where DSAP is when no routing */

#define TR_IS_SROUTED(th)   ((th)->source[0] & 0x80)
#define TR_IS_BROADCAST(th) (((intel16((th)->RCF) & 0xE000) >> 13) >= 4)
#define TR_RIF_LENGTH(th)   ((intel16((th)->RCF) & 0x1F00) >> 8)

#define TR_MAC_SIZE         (2+2+2*sizeof(mac_address)) /* AC,FC,src/dst */

static void fix_tok_head (tok_Header **trp)
{
  tok_Header *tr = *trp;

#if defined(USE_DEBUG)
  const BYTE *raw = (const BYTE*)tr;
  int   i;

  dbug_write ("TR raw: ");
  for (i = 0; i < 50; i++)
      dbug_printf ("%02X ", raw[i]);
  dbug_write ("\n");
#endif

  if (TR_IS_SROUTED(tr))     /* Source routed */
  {
    int rlen = TR_RIF_LENGTH (tr);

    tr->source[0] &= 0x7F;   /* clear RII bit */

    /* Set our notion of link-layer broadcast
     */
    if (TR_IS_BROADCAST(tr))
       tr->destination[0] |= 1;

    /* Copy MAC-header rlen bytes upwards
     */
    memmove ((BYTE*)tr + rlen, tr, TR_MAC_SIZE);
    *trp = (tok_Header*) ((BYTE*)tr + rlen);
  }
}

/*
 * Check (and fix?) the received ARCNET header.
 * All frames must have a common layout with headlen ARC_HDRLEN.
 */
static BOOL fix_arc_head (const arcnet_Header *head, WORD *type)
{
#if defined(USE_DEBUG)
  const BYTE *raw = (const BYTE*)head;
  int   i;

  dbug_write ("ARC raw: ");
  for (i = 0; i < 50; i++)
      dbug_printf ("%02X ", raw[i]);
  dbug_write ("\n");
#endif

  *type = head->type;

  /* Map to IANA numbers
   */
  if (*type == ARCNET_IP_1051 || *type == ARCNET_IP_1201)
     *type = IP4_TYPE;
  if (*type == ARCNET_ARP_1051 || *type == ARCNET_ARP_1201)
     *type = ARP_TYPE;

  if (head->flags == 0xFF ||   /* Exception packet */
      (*type != IP4_TYPE &&    /* Neither IP nor ARP */
       *type != ARP_TYPE))
     return (FALSE);
  return (TRUE);
}

/**
 * Poll the packet queue. Return first packet in queue.
 * Optionally do receiver profiling.
 *
 *   \retval pointer to start of MAC-header.
 *   \retval NULL    no packets are queued.
 *
 * \note Not used when e.g. libpcap has set the `_eth_recv_hook' to
 *       do it's own packet-polling.
 * \note 'type' is always set.
 */
static union link_Packet *poll_recv_queue (WORD *type)
{
#if defined(USE_FAST_PKT) || defined(WIN32)
  struct pkt_rx_element *buf;
#endif
  struct pkt_ringbuf *q = &_pkt_inf->pkt_queue;
  union  link_Packet *pkt;
  struct in_Header   *ip;

  ASSERT_PKT_INF (NULL);

#if defined(USE_DEBUG)
#if defined(USE_FAST_PKT)
  if (!pktq_far_check(q))  /* MSDOS only */
#else
  if (!pktq_check(q))
#endif
  {
    fprintf (stderr, "%s: pkt-queue destroyed!\n", __FILE__);
    exit (-1);
  }
#endif  /* USE_DEBUG */

#if defined(WIN32)
  buf = pkt_poll_recv();
  if (!buf)
     return (NULL);

  PROFILE_RECV (buf->tstamp_put, buf->tstamp_get);
  _eth_last.rx.size = buf->rx_length;
  memcpy (&_eth_last.rx.tstamp, &buf->tstamp_put, sizeof(_eth_last.rx.tstamp));
  pkt = (link_Packet*) &buf->rx_buf[0];

#elif defined(USE_FAST_PKT)
  buf = pkt_poll_recv();
  if (!buf)
     return (NULL);

  PROFILE_RECV (*(uint64*)&buf->tstamp_put, *(uint64*)&buf->tstamp_get);
  pkt = (link_Packet*) buf->rx_buf;
  _eth_last.rx.size      = buf->rx_length_1;    /* length on 1st upcall */
  _eth_last.rx.tstamp.lo = buf->tstamp_put[0];  /* TSC set in asmpkt */
  _eth_last.rx.tstamp.hi = buf->tstamp_put[1];

#else
  if (!pktq_queued(q))
     return (NULL);

  pkt = (link_Packet*) pktq_out_buf (q);
  _eth_last.rx.size      = ETH_MAX;  /* !! wrong, but doesn't matter for pcap */
  _eth_last.rx.tstamp.lo = 0UL;      /* pcdbug.c writes rx-time == dbg-time */
  _eth_last.rx.tstamp.hi = 0UL;
#endif


  if (_pktserial)
  {
    ip = &pkt->ip.head;       /* SLIP/PPP/AX25 */
    *type = (ip->ver == 4) ? IP4_TYPE : IP6_TYPE;
    return (pkt);
  }

  if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
  {
    tok_Header *tr = &pkt->tok.head;

    fix_tok_head (&tr);
    *type = tr->type;
    return (union link_Packet*) tr;
  }

  if (_pktdevclass == PDCLASS_ARCNET)
  {
    arcnet_Packet *arc = &pkt->arc;

    if (!fix_arc_head(&arc->head, type))
    {
      DEBUG_RX (NULL, &arc->data);
      pkt_free_pkt (pkt);
      pkt = NULL;
    }
    return (pkt);
  }

  if (_pktdevclass == PDCLASS_FDDI)
  {
    fddi_Packet *fddi = &pkt->fddi;

    *type = fddi->head.type;
    return (pkt);
  }

  /* must be PDCLASS_ETHER */

  *type = pkt->eth.head.type;
  ARGSUSED (q);
  return (pkt);
}

/*
 * Fix the LLC header to look like ordinary Ethernet II
 * !! not yet.
 */
static void fix_llc_head (void **mac)
{
#if 1
  DEBUG_RX (NULL, *mac);
  _eth_free (*mac);
  *mac = NULL;
#else
  /** \todo handle IEEE 802.3 encapsulation also.
   */
#endif
}


/**
 * Poll for arrival of new packets (IP/ARP/RARP/PPPoE protocol).
 * Sets protocol-type of packet received in 'type'.
 *
 * For Ethernet/TokenRing-type drivers: \n
 *  \retval Pointer past the MAC-header to the IP/ARP/RARP
 *          protocol header. Also check for link-layer broadcast.
 *
 * For PPP/SLIP-type drivers (no MAC-headers): \n
 *  \retval Pointer to the IP-packet itself.
 *   IP-protocol is assumed. Can never be link-layer broadcast.
 */
void * W32_CALL _eth_arrived (WORD *type_ptr, BOOL *broadcast)
{
  union link_Packet *pkt;
  mac_address       *dst;
  void              *ret = NULL;
  BOOL               is_bcast = FALSE;
  WORD               type = 0;

  if (!_eth_is_init)  /* GvB 2002-09, Lets us run without a working driver */
     return (NULL);

  if (_eth_recv_hook)
       pkt = (union link_Packet*) (*_eth_recv_hook) (&type);
  else pkt = poll_recv_queue (&type);

  if (!pkt)
     return (NULL);

  if (_eth_recv_peek && !(*_eth_recv_peek)(pkt))
  {
    _eth_free (pkt);
    return (NULL);
  }

  /* Hack: If _ip?_handler() can't be reentered, only accept
   *       non-IP packets. Assume PPPoE session packets carries only IP.
   */
  if (_ip_recursion &&
      (type == IP4_TYPE || type == IP6_TYPE || type == PPPOE_SESS_TYPE))
  {
    /**< \todo push back packet, else it's lost */
    STAT (macstats.num_ip_recurse++);
    _eth_free (pkt);
    return (NULL);
  }

  if (_pktserial)
  {
    dst = NULL;
    ret = (void*) &pkt->ip;
  }
  else if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
  {
    dst = &pkt->tok.head.destination;
    ret = (void*) &pkt->tok.data;
  }
  else if (_pktdevclass == PDCLASS_FDDI)
  {
    dst = &pkt->fddi.head.destination;
    ret = (void*) &pkt->fddi.data;
  }
  else if (_pktdevclass == PDCLASS_ARCNET)
  {
    dst = (mac_address*) &pkt->arc.head.destination;
    ret = (void*) ((BYTE*)pkt + ARC_HDRLEN);
  }
  else   /* must be ether */
  {
    dst = &pkt->eth.head.destination;
    ret = (void*) &pkt->eth.data;
  }

#if defined(NEED_PKT_SPLIT)
  pkt_split_mac_in (pkt);
#endif

#if defined(USE_STATISTICS)
  update_in_stat();
#endif

#if defined(NEED_PKT_SPLIT) && defined(USE_DEBUG) && 0  /* test */
  pkt_print_split_in();
#endif

  /* ARCnet should never have LLC fields. So don't test for it
   */
  if (_pktdevclass != PDCLASS_ARCNET)
  {
    if (dst && !memcmp(dst, &_eth_brdcast, sizeof(_eth_brdcast)))
       is_bcast = TRUE;

    if (intel16(type) < 0x600)       /* LLC length field */
       fix_llc_head (&ret);
  }
  else if (dst && *(BYTE*)dst == 0)  /* ARCnet broadcast */
  {
    is_bcast = TRUE;
  }

  if (type_ptr)
     *type_ptr = type;

  if (broadcast)
     *broadcast = is_bcast;

  return (ret);
}


#if defined(NOT_USED)
/**
 * Return pointer to MAC header start address of an IP packet.
 */
void *_eth_mac_hdr (const in_Header *ip)
{
  if (!_pktserial)
     return (void*) ((BYTE*)ip - _pkt_ip_ofs);

  (*_printf) ("Illegal use of `_eth_mac_hdr()' for class %d\n",
              _pktdevclass);
  exit (-1);
  return (NULL);
}

/**
 * Return pointer to MAC destination address of an IP packet.
 */
void *_eth_mac_dst (const in_Header *ip)
{
  const union link_Packet *pkt;

  pkt = (const union link_Packet*) ((BYTE*)ip - _pkt_ip_ofs);

  if (_pktdevclass == PDCLASS_ETHER)
     return (void*) &pkt->eth.head.destination;

  if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
     return (void*) &pkt->tok.head.destination;

  if (_pktdevclass == PDCLASS_FDDI)
     return (void*) &pkt->fddi.head.destination;

  if (_pktdevclass == PDCLASS_ARCNET)
     return (void*) &pkt->arc.head.destination;

  (*_printf) ("Illegal use of `_eth_mac_dst()' for class %d\n",
              _pktdevclass);
  exit (-1);
  return (NULL);
}

/**
 * Return pointer to MAC source address of an IP packet.
 */
void *_eth_mac_src (const in_Header *ip)
{
  const union link_Packet *pkt;

  pkt = (const union link_Packet*) ((BYTE*)ip - _pkt_ip_ofs);

  if (_pktdevclass == PDCLASS_ETHER)
     return (void*) &pkt->eth.head.source;

  if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
     return (void*) &pkt->tok.head.source;

  if (_pktdevclass == PDCLASS_FDDI)
     return (void*) &pkt->fddi.head.source;

  if (_pktdevclass == PDCLASS_ARCNET)
     return (void*) &pkt->arc.head.source;

  (*_printf) ("Illegal use of `_eth_mac_src()' for class %d\n",
              _pktdevclass);
  exit (-1);
  return (NULL);
}

/**
 * Return value of protocol-type given an IP packet.
 */
WORD _eth_mac_typ (const in_Header *ip)
{
  const union link_Packet *pkt;

  pkt = (const union link_Packet*) ((BYTE*)ip - _pkt_ip_ofs);

  if (_pktdevclass == PDCLASS_ETHER)
     return (pkt->eth.head.type);

  if (_pktdevclass == PDCLASS_TOKEN || _pktdevclass == PDCLASS_TOKEN_RIF)
     return (pkt->tok.head.type);

  if (_pktdevclass == PDCLASS_FDDI)
     return (pkt->fddi.head.type);

  if (_pktdevclass == PDCLASS_ARCNET)
     return (pkt->arc.head.type);

  (*_printf) ("Illegal use of `_eth_mac_typ()' for class %d\n",
              _pktdevclass);
  exit (-1);
  return (0);
}
#endif /* NOT_USED */


/*
 * Jim Martin's Multicast extensions
 */

#if defined(USE_MULTICAST)
/**
 * Joins a multicast group (at the physical layer).
 *
 * \retval 1 The group was joined successfully.
 * \retval 0 Attempt failed.
 */
BOOL _eth_join_mcast_group (const struct MultiCast *mc)
{
  eth_address list [IPMULTI_SIZE];
  int         i, len, nextentry;
  BOOL        is_mcast1, is_mcast2, is_promis;

  /* Return if we're already receiving all multicasts or is in
   * promiscous mode.
   */
#if defined(WIN32)
  is_mcast1 = (_pkt_rxmode & RXMODE_MULTICAST1);
  is_mcast2 = (_pkt_rxmode & RXMODE_MULTICAST2);
  is_promis = (_pkt_rxmode & RXMODE_PROMISCOUS);
#else
  is_mcast1 = (_pkt_rxmode >= RXMODE_MULTICAST1);
  is_mcast2 = (_pkt_rxmode >= RXMODE_MULTICAST2);
  is_promis = (_pkt_rxmode >= RXMODE_PROMISCOUS);
#endif

  if (is_mcast2 || is_promis)
     return (TRUE);

  if (!is_mcast1)
  {
    is_mcast1 = pkt_set_rcv_mode(RXMODE_MULTICAST1);
    if (!is_mcast1)
       is_promis = pkt_set_rcv_mode(RXMODE_PROMISCOUS);
    if (is_promis)
       return (TRUE);
    if (!is_mcast1 && !is_promis)
       return (FALSE);     /* hopeless, give up */
  }

  len = sizeof(list);
  if (!pkt_get_multicast_list (list, &len))
  {
    /* If no MC support, switch to MC2 mode
     */
    if (_pkt_errno == PDERR_NO_MULTICAST)
       return pkt_set_rcv_mode (RXMODE_MULTICAST2);
    return (FALSE);
  }

  /* check to see if the address is already in the list
   */
  for (i = 0; i < len / SIZEOF(list[0]); i++)
      if (!memcmp(list[i], &mc->ethaddr, sizeof(list[0])))
         return (TRUE);

  if (len > 0)
       nextentry = len / sizeof(list[0]);  /* append entry */
  else nextentry = 0;

  memcpy (&list[nextentry], &mc->ethaddr, sizeof(eth_address));
  len += sizeof (list[0]);

  if (!pkt_set_multicast_list((const eth_address*)&list,len))
  {
    /* If no space or no MC support, switch to MC2 mode
     */
    if (_pkt_errno == PDERR_NO_SPACE ||
        _pkt_errno == PDERR_NO_MULTICAST)
       return pkt_set_rcv_mode (RXMODE_MULTICAST2);
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Leaves a multicast group (at the physical layer)
 *
 * \retval 1 The group was left successfully.
 * \retval 0 Attempt failed.
 */
BOOL _eth_leave_mcast_group (const struct MultiCast *mc)
{
  eth_address list[IPMULTI_SIZE];
  int i, len, idx;

  /* \note This should be expanded to include switching back to
   *       RXMODE_MULTCAST1 if the list of multicast addresses has
   *       shrunk sufficiently.
   *
   * First check to see if we're in RXMODE_MULTICAST2. if so return
   */
#if defined(WIN32)
  if (_pkt_rxmode & RXMODE_MULTICAST2)
     return (TRUE);
#else
  if (_pkt_rxmode >= RXMODE_MULTICAST2)
     return (TRUE);
#endif

  /* get the list of current multicast addresses
   */
  len = sizeof(list);
  if (!pkt_get_multicast_list (list, &len))
     return (FALSE);

  /* find the apropriate entry
   */
  for (i = 0, idx = -1; i < len/SIZEOF(list[0]); i++)
      if (!memcmp(list[i],&mc->ethaddr,sizeof(list[0])))
         idx = i;

  /* if it's not in the list, just return
   */
  if (idx == -1)
     return (TRUE);

  /* ahh, but it _is_ in the list. So shorten the list
   * and send it back to the PKTDRVR
   */
  if (idx+1 < len/SIZEOF(list[0]))
     memmove (&list[idx], &list[idx+1], len-(idx+1)*sizeof(list[0]));

  len -= sizeof(list[0]);

  if (!pkt_set_multicast_list((const eth_address*)&list, len))
  {
    /* If no space or no MC support, switch mode
     */
    if (_pkt_errno == PDERR_NO_SPACE ||
        _pkt_errno == PDERR_NO_MULTICAST)
       return pkt_set_rcv_mode (RXMODE_MULTICAST2);
    return (FALSE);
  }
  return (TRUE);
}
#endif /* USE_MULTICAST */

/**
 * Turn off stack-checking because _eth_release() might be called from
 * an exception handler.
 */
#include "nochkstk.h"

static void W32_CALL __eth_release (void)
{
  _eth_release();
}

/**
 * Release the hardware driver.
 */
void W32_CALL _eth_release (void)
{
  if (!_eth_is_init)
     return;

  /* Set original MAC address (if not fatal-error or serial-driver)
   */
  if (!_watt_fatal_error)
  {
    if (!_pktserial)
    {
      if (memcmp(&_eth_addr, &_eth_real_addr, sizeof(_eth_addr)))
         pkt_set_addr (&_eth_real_addr);

#if defined(USE_MULTICAST) || defined(USE_IPV6) /* restore startup Rx mode */
      if (_pkt_rxmode0 > -1 && _pkt_rxmode0 != _pkt_rxmode)
         pkt_set_rcv_mode (_pkt_rxmode0);
#endif
    }

#if defined(USE_DEBUG)
    if (debug_on > 0)
    {
      u_long drops = pkt_dropped();
      if (drops)
        (*_printf) ("%lu packets dropped\n", drops);
    }
#endif
  }
  _eth_is_init = FALSE;  /* in case we crash in pkt_release() */
  pkt_release();
}
