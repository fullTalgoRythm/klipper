// Hardware interface to "USB OTG (on the go) controller" on stm32
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // NULL
#include "autoconf.h" // CONFIG_MACH_STM32F446
#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/io.h" // writel
#include "board/usb_cdc.h" // usb_notify_ep0
#include "board/usb_cdc_ep.h" // USB_CDC_EP_BULK_IN
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // GPIO
#include "sched.h" // DECL_INIT


/****************************************************************
 * USB transfer memory
 ****************************************************************/

#define OTG ((USB_OTG_GlobalTypeDef*)USB_OTG_FS_PERIPH_BASE)
#define OTGD ((USB_OTG_DeviceTypeDef*)                          \
              (USB_OTG_FS_PERIPH_BASE + USB_OTG_DEVICE_BASE))
#define EPFIFO(EP) ((void*)(USB_OTG_FS_PERIPH_BASE + USB_OTG_FIFO_BASE  \
                            + ((EP) << 12)))
#define EPIN(EP) ((USB_OTG_INEndpointTypeDef*)                          \
                  (USB_OTG_FS_PERIPH_BASE + USB_OTG_IN_ENDPOINT_BASE    \
                   + ((EP) << 5)))
#define EPOUT(EP) ((USB_OTG_OUTEndpointTypeDef*)                        \
                   (USB_OTG_FS_PERIPH_BASE + USB_OTG_OUT_ENDPOINT_BASE  \
                    + ((EP) << 5)))

// Setup the USB fifos
static void
fifo_configure(void)
{
    // Reserve memory for Rx fifo
    uint32_t sz = ((4 * 1 + 6)
                   + 4 * ((USB_CDC_EP_BULK_OUT_SIZE / 4) + 1)
                   + (2 * 1));
    OTG->GRXFSIZ = sz;

    // Tx fifos
    uint32_t fpos = sz, ep_size = 0x10;
    OTG->DIEPTXF0_HNPTXFSIZ = ((fpos << USB_OTG_TX0FSA_Pos)
                               | (ep_size << USB_OTG_TX0FD_Pos));
    fpos += ep_size;

    OTG->DIEPTXF[USB_CDC_EP_ACM - 1] = (
        (fpos << USB_OTG_DIEPTXF_INEPTXSA_Pos)
        | (ep_size << USB_OTG_DIEPTXF_INEPTXFD_Pos));
    fpos += ep_size;

    OTG->DIEPTXF[USB_CDC_EP_BULK_IN - 1] = (
        (fpos << USB_OTG_DIEPTXF_INEPTXSA_Pos)
        | (ep_size << USB_OTG_DIEPTXF_INEPTXFD_Pos));
    fpos += ep_size;
}

// Write a packet to a tx fifo
static int_fast8_t
fifo_write_packet(uint32_t ep, const uint8_t *src, uint32_t len)
{
    void *fifo = EPFIFO(ep);
    USB_OTG_INEndpointTypeDef *epi = EPIN(ep);
    epi->DIEPTSIZ = len | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos);
    epi->DIEPCTL |= USB_OTG_DIEPCTL_EPENA | USB_OTG_DIEPCTL_CNAK;
    int32_t count = len;
    while (count >= 4) {
        uint32_t data;
        memcpy(&data, src, 4);
        writel(fifo, data);
        count -= 4;
        src += 4;
    }
    if (count) {
        uint32_t data = 0;
        memcpy(&data, src, count);
        writel(fifo, data);
    }
    return len;
}

// Read a packet from the rx queue
static int_fast8_t
fifo_read_packet(uint8_t *dest, uint_fast8_t max_len)
{
    // Transfer data
    void *fifo = EPFIFO(0);
    uint32_t grx = OTG->GRXSTSP;
    uint32_t bcnt = (grx & USB_OTG_GRXSTSP_BCNT) >> USB_OTG_GRXSTSP_BCNT_Pos;
    uint32_t xfer = bcnt > max_len ? max_len : bcnt, count = xfer;
    while (count >= 4) {
        uint32_t data = readl(fifo);
        memcpy(dest, &data, 4);
        count -= 4;
        dest += 4;
    }
    if (count) {
        uint32_t data = readl(fifo);
        memcpy(dest, &data, count);
    }
    uint32_t extra = DIV_ROUND_UP(bcnt, 4) - DIV_ROUND_UP(xfer, 4);
    while (extra--)
        readl(fifo);

    // Reenable packet reception if it got disabled by controller
    USB_OTG_OUTEndpointTypeDef *epo = EPOUT(grx & USB_OTG_GRXSTSP_EPNUM_Msk);
    uint32_t ctl = epo->DOEPCTL;
    if (!(ctl & USB_OTG_DOEPCTL_EPENA) || ctl & USB_OTG_DOEPCTL_NAKSTS) {
        epo->DOEPTSIZ = 64 | (1 << USB_OTG_DOEPTSIZ_PKTCNT_Pos);
        epo->DOEPCTL = ctl | USB_OTG_DOEPCTL_EPENA | USB_OTG_DOEPCTL_CNAK;
    }
    return xfer;
}

// Inspect the next packet on the rx queue
static uint32_t
peek_rx_queue(uint32_t ep)
{
    for (;;) {
        uint32_t sts = OTG->GINTSTS;
        if (!(sts & USB_OTG_GINTSTS_RXFLVL))
            // No packet ready
            return 0;
        uint32_t grx = OTG->GRXSTSR;
        uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                           >> USB_OTG_GRXSTSP_PKTSTS_Pos);
        if (pktsts != 1 && pktsts != 3 && pktsts != 4) {
            // A packet is ready
            if ((grx & USB_OTG_GRXSTSP_EPNUM_Msk) != ep)
                return 0;
            return grx;
        }
        // Discard informational entries from queue
        fifo_read_packet(NULL, 0);
    }
}


/****************************************************************
 * USB interface
 ****************************************************************/

int_fast8_t
usb_read_bulk_out(void *data, uint_fast8_t max_len)
{
    uint32_t grx = peek_rx_queue(USB_CDC_EP_BULK_OUT);
    if (!grx) {
        // Wait for packet
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        return -1;
    }
    return fifo_read_packet(data, max_len);
}

int_fast8_t
usb_send_bulk_in(void *data, uint_fast8_t len)
{
    uint32_t ctl = EPIN(USB_CDC_EP_BULK_IN)->DIEPCTL;
    if (!(ctl & USB_OTG_DIEPCTL_USBAEP))
        // Controller not enabled
        return -2;
    if (ctl & USB_OTG_DIEPCTL_EPENA) {
        // Wait for space to transmit
        OTGD->DIEPEMPMSK |= (1 << USB_CDC_EP_BULK_IN);
        return -1;
    }
    return fifo_write_packet(USB_CDC_EP_BULK_IN, data, len);
}

int_fast8_t
usb_read_ep0(void *data, uint_fast8_t max_len)
{
    uint32_t grx = peek_rx_queue(0);
    if (!grx) {
        // Wait for packet
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        return -1;
    }
    uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                       >> USB_OTG_GRXSTSP_PKTSTS_Pos);
    if (pktsts != 2)
        // Transfer interrupted
        return -2;
    return fifo_read_packet(data, max_len);
}

int_fast8_t
usb_read_ep0_setup(void *data, uint_fast8_t max_len)
{
    for (;;) {
        uint32_t grx = peek_rx_queue(0);
        if (!grx) {
            // Wait for packet
            OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
            return -1;
        }
        uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                           >> USB_OTG_GRXSTSP_PKTSTS_Pos);
        if (pktsts == 6)
            // Found a setup packet
            break;
        // Discard other packets
        fifo_read_packet(NULL, 0);
    }
    uint32_t ctl = EPIN(0)->DIEPCTL;
    if (ctl & USB_OTG_DIEPCTL_EPENA) {
        // Flush any pending tx packets
        EPIN(0)->DIEPCTL = ctl | USB_OTG_DIEPCTL_EPDIS | USB_OTG_DIEPCTL_SNAK;
        while (EPIN(0)->DIEPCTL & USB_OTG_DIEPCTL_EPENA)
            ;
        OTG->GRSTCTL = USB_OTG_GRSTCTL_TXFFLSH;
        while (OTG->GRSTCTL & USB_OTG_GRSTCTL_TXFFLSH)
            ;
    }
    return fifo_read_packet(data, max_len);
}

int_fast8_t
usb_send_ep0(const void *data, uint_fast8_t len)
{
    uint32_t grx = peek_rx_queue(0);
    if (grx) {
        // Transfer interrupted
        return -2;
    }
    if (EPIN(0)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
        // Wait for space to transmit
        OTGD->DIEPEMPMSK |= (1 << 0);
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        return -1;
    }
    return fifo_write_packet(0, data, len);
}

void
usb_stall_ep0(void)
{
    EPIN(0)->DIEPCTL |= USB_OTG_DIEPCTL_STALL;
    usb_notify_ep0(); // XXX - wake from main usb_cdc.c code?
}

void
usb_set_address(uint_fast8_t addr)
{
    OTGD->DCFG |= addr << USB_OTG_DCFG_DAD_Pos;
    usb_send_ep0(NULL, 0);
    usb_notify_ep0();
}

void
usb_set_configure(void)
{
}

void
usb_request_bootloader(void)
{
}


/****************************************************************
 * Setup and interrupts
 ****************************************************************/

// Configure interface after a USB reset event
static void
usb_reset(void)
{
    // Flush Rx queue
    OTG->GRSTCTL = USB_OTG_GRSTCTL_RXFFLSH;
    while (OTG->GRSTCTL & USB_OTG_GRSTCTL_RXFFLSH)
        ;

    // Flush Tx queues
    OTG->GRSTCTL = (16 << USB_OTG_GRSTCTL_TXFNUM_Pos) | USB_OTG_GRSTCTL_TXFFLSH;
    while (OTG->GRSTCTL & USB_OTG_GRSTCTL_TXFFLSH)
        ;

    // Configure and enable endpoints
    uint32_t mpsize_ep0 = 2;
    USB_OTG_INEndpointTypeDef *epi = EPIN(0);
    USB_OTG_OUTEndpointTypeDef *epo = EPOUT(0);
    epi->DIEPCTL = mpsize_ep0 | USB_OTG_DIEPCTL_SNAK;
    epo->DOEPTSIZ = (64 | (1 << USB_OTG_DOEPTSIZ_STUPCNT_Pos)
                     | (1 << USB_OTG_DOEPTSIZ_PKTCNT_Pos));
    epo->DOEPCTL = mpsize_ep0 | USB_OTG_DOEPCTL_EPENA | USB_OTG_DOEPCTL_CNAK;

    epi = EPIN(USB_CDC_EP_ACM);
    epi->DIEPTSIZ = (USB_CDC_EP_ACM_SIZE
                     | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos));
    epi->DIEPCTL = (
        USB_OTG_DIEPCTL_SNAK | USB_OTG_DIEPCTL_USBAEP
        | (0x03 << USB_OTG_DIEPCTL_EPTYP_Pos) | USB_OTG_DIEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_ACM << USB_OTG_DIEPCTL_TXFNUM_Pos)
        | (USB_CDC_EP_ACM_SIZE << USB_OTG_DIEPCTL_MPSIZ_Pos));

    epo = EPOUT(USB_CDC_EP_BULK_OUT);
    epo->DOEPCTL = (
        USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_USBAEP | USB_OTG_DOEPCTL_EPENA
        | (0x02 << USB_OTG_DOEPCTL_EPTYP_Pos) | USB_OTG_DOEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_BULK_OUT_SIZE << USB_OTG_DOEPCTL_MPSIZ_Pos));

    epi = EPIN(USB_CDC_EP_BULK_IN);
    epi->DIEPTSIZ = (USB_CDC_EP_BULK_IN_SIZE
                     | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos));
    epi->DIEPCTL = (
        USB_OTG_DIEPCTL_SNAK | USB_OTG_DIEPCTL_USBAEP
        | (0x02 << USB_OTG_DIEPCTL_EPTYP_Pos) | USB_OTG_DIEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_BULK_IN << USB_OTG_DIEPCTL_TXFNUM_Pos)
        | (USB_CDC_EP_BULK_IN_SIZE << USB_OTG_DIEPCTL_MPSIZ_Pos));

    // Set address to zero
    OTGD->DCFG &= ~USB_OTG_DCFG_DAD;
}

// Handle a USB disconnect
static void
usb_suspend(void)
{
    EPIN(USB_CDC_EP_BULK_IN)->DIEPCTL &= ~USB_OTG_DIEPCTL_USBAEP;
}

// Main irq handler
void
OTG_FS_IRQHandler(void)
{
    uint32_t sts = OTG->GINTSTS;
    if (sts & USB_OTG_GINTSTS_USBRST) {
        OTG->GINTSTS = USB_OTG_GINTSTS_USBRST;
        usb_reset();
    }
    if (sts & USB_OTG_GINTSTS_USBSUSP) {
        OTG->GINTSTS = USB_OTG_GINTSTS_USBSUSP;
        usb_suspend();
    }
    if (sts & USB_OTG_GINTSTS_RXFLVL) {
        // Received data - disable irq and notify endpoint
        OTG->GINTMSK &= ~USB_OTG_GINTMSK_RXFLVLM;
        uint32_t grx = OTG->GRXSTSR, ep = grx & USB_OTG_GRXSTSP_EPNUM_Msk;
        if (ep == 0)
            usb_notify_ep0();
        else
            usb_notify_bulk_out();
    }
    if (sts & USB_OTG_GINTSTS_IEPINT) {
        // Can transmit data - disable irq and notify endpoint
        uint32_t daint = OTGD->DAINT;
        OTGD->DIEPEMPMSK &= ~daint;
        if (daint & (1 << 0))
            usb_notify_ep0();
        if (daint & (1 << USB_CDC_EP_BULK_IN))
            usb_notify_bulk_in();
    }
}

DECL_CONSTANT_STR("RESERVE_PINS_USB", "PA11,PA12");

// Initialize the usb controller
void
usb_init(void)
{
    // Enable USB clock
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    while (!(OTG->GRSTCTL & USB_OTG_GRSTCTL_AHBIDL))
        ;

    // Configure USB in full-speed device mode
    OTG->GUSBCFG = (USB_OTG_GUSBCFG_FDMOD | USB_OTG_GUSBCFG_PHYSEL
                    | (6 << USB_OTG_GUSBCFG_TRDT_Pos));
    OTGD->DCFG |= (3 << USB_OTG_DCFG_DSPD_Pos);
#if CONFIG_MACH_STM32F446
    OTG->GOTGCTL = USB_OTG_GOTGCTL_BVALOEN | USB_OTG_GOTGCTL_BVALOVAL;
#else
    OTG->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
#endif

    // Route pins
    gpio_peripheral(GPIO('A', 11), GPIO_FUNCTION(10), 0);
    gpio_peripheral(GPIO('A', 12), GPIO_FUNCTION(10), 0);

    // Setup USB packet memory
    fifo_configure();

    // Enable interrupts
    OTGD->DAINTMSK = (1 << 0) | (1 << USB_CDC_EP_BULK_IN);
    OTG->GINTMSK = (USB_OTG_GINTMSK_USBRST | USB_OTG_GINTSTS_USBSUSP
                    | USB_OTG_GINTMSK_RXFLVLM | USB_OTG_GINTMSK_IEPINT);
    OTG->GAHBCFG = USB_OTG_GAHBCFG_GINT;
    armcm_enable_irq(OTG_FS_IRQHandler, OTG_FS_IRQn, 1);

    // Enable USB
    OTG->GCCFG |= USB_OTG_GCCFG_PWRDWN;
    OTGD->DCTL = 0;
}
DECL_INIT(usb_init);
