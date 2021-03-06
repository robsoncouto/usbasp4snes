/* Name: main.c
 * Project: Multiple NES/SNES to USB converter
 * Author: Raphael Assenat <raph@raphnet.net>
 * Copyright: (C) 2007-2009 Raphael Assenat <raph@raphnet.net>
 * License: GPLv2
 * Tabsize: 4
 * Comments: Based on HID-Test by Christian Starkjohann
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <string.h>

#include "usbdrv.h"
#include "gamepad.h"

#include "fournsnes.h"

#include "devdesc.h"

static uchar *rt_usbHidReportDescriptor=NULL;
static uchar rt_usbHidReportDescriptorSize=0;
static uchar *rt_usbDeviceDescriptor=NULL;
static uchar rt_usbDeviceDescriptorSize=0;


#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega168A__) || \
	defined(__AVR_ATmega168P__) || defined(__AVR_ATmega328__) || \
	defined(__AVR_ATmega328P__) || defined(__AVR_ATmega88__) || \
	defined(__AVR_ATmega88A__) || defined(__AVR_ATmega88P__) || \
	defined(__AVR_ATmega88PA__)
#define AT168_COMPATIBLE
#endif

/* The maximum number of independent reports that are supported. */
#define MAX_REPORTS 8

const PROGMEM int usbDescriptorStringSerialNumber[]  = {
	USB_STRING_DESCRIPTOR_HEADER(4),
	'1','0','0','0'
};

int usbDescriptorStringDevice[] = {
	USB_STRING_DESCRIPTOR_HEADER(DEVICE_STRING_LENGTH),
	DEFAULT_PROD_STRING
};

char usbDescriptorConfiguration[] = { 0 }; // dummy

uchar my_usbDescriptorConfiguration[] = {    /* USB configuration descriptor */
    9,          /* sizeof(usbDescriptorConfiguration): length of descriptor in bytes */
    USBDESCR_CONFIG,    /* descriptor type */
    18 + 7 * USB_CFG_HAVE_INTRIN_ENDPOINT + 9, 0,
                /* total length of data returned (including inlined descriptors) */
    1,          /* number of interfaces in this configuration */
    1,          /* index of this configuration */
    0,          /* configuration name string index */
#if USB_CFG_IS_SELF_POWERED
    USBATTR_SELFPOWER,  /* attributes */
#else
    USBATTR_BUSPOWER,   /* attributes */
#endif
    USB_CFG_MAX_BUS_POWER/2,            /* max USB current in 2mA units */
/* interface descriptor follows inline: */
    9,          /* sizeof(usbDescrInterface): length of descriptor in bytes */
    USBDESCR_INTERFACE, /* descriptor type */
    0,          /* index of this interface */
    0,          /* alternate setting for this interface */
    USB_CFG_HAVE_INTRIN_ENDPOINT,   /* endpoints excl 0: number of endpoint descriptors to follow */
    USB_CFG_INTERFACE_CLASS,
    USB_CFG_INTERFACE_SUBCLASS,
    USB_CFG_INTERFACE_PROTOCOL,
    0,          /* string index for interface */
//#if (USB_CFG_DESCR_PROPS_HID & 0xff)    /* HID descriptor */
    9,          /* sizeof(usbDescrHID): length of descriptor in bytes */
    USBDESCR_HID,   /* descriptor type: HID */
    0x01, 0x01, /* BCD representation of HID version */
    0x00,       /* target country code */
    0x01,       /* number of HID Report (or other HID class) Descriptor infos to follow */
    0x22,       /* descriptor type: report */
    USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH, 0,  /* total length of report descriptor */
//#endif
#if USB_CFG_HAVE_INTRIN_ENDPOINT    /* endpoint descriptor for endpoint 1 */
    7,          /* sizeof(usbDescrEndpoint) */
    USBDESCR_ENDPOINT,  /* descriptor type = endpoint */
    0x81,       /* IN endpoint number 1 */
    0x03,       /* attrib: Interrupt endpoint */
    8, 0,       /* maximum packet size */
    USB_CFG_INTR_POLL_INTERVAL, /* in ms */
#endif
};


static Gamepad *curGamepad;


/* ----------------------- hardware I/O abstraction ------------------------ */

static void hardwareInit(void)
{

	/* PORTB
	 *
	 * Bit     Description       Direction    Level/pu
	 * 0       Jumpers common    Out          0
	 * 1       JP1               In           1
	 * 2       JP2               In           1
	 * 3       MOSI              In           1
	 * 4       MISO              In           1
	 * 5       SCK               In           1
	 * 6       -
	 * 7       -
	 */
	//DDRB = 0x01;
	//PORTB = 0xFE;

	//Change to use with USBASP{
	//NOTE:PORTB is now the where the USB pins are(prev port D). I believe the library handles
	//the pins initial level and their direction itself. Also the four controllers output pin(DATA)
	//are in this port, on pins 2,3,4 and 5
	//Usb pins are init as output, low. (device reset).
	DDRB&=~0x3F ;
	PORTB|= 0x3C;//pull ups activated
	//}

	// init port C as input with pullup
	//DDRC = 0x00;
	//PORTC = 0xff;

	//Change to use with USBASP{
	//PC2 is used as the multitap pin. PC0 and PC1 are the green and red LEDs
	//on a USBASP
	DDRC = 0x07;
	PORTC = 0x00;
	//}

	/*
	 * For port D, activate pull-ups on all lines except the D+, D- and bit 1.
	 *
	 * For historical reasons (a mistake on an old PCB), bit 1
	 * is now always connected with bit 0. So bit 1 configured
	 * as an input without pullup.
	 *
	 * Usb pin are init as output, low. (device reset). They are released
	 * later when usbReset() is called.
	 */
	//PORTD = 0xf8;
	//DDRD = 0x01 | 0x04;

	///Change to use with USBASP{
	//NOTE, the UART pins (0 and 1) were used as the controllers LATCH and CLOCK (outputs)
	DDRD|= 0x03;
	PORTD&= ~(0x03);
	//}

	/* Configure timers */
#if defined(AT168_COMPATIBLE)
	TCCR2A= (1<<WGM21);
	TCCR2B=(1<<CS22)|(1<<CS21)|(1<<CS20);
	OCR2A=196;  // for 60 hz
#else
	TCCR2 = (1<<WGM21)|(1<<CS22)|(1<<CS21)|(1<<CS20);
	OCR2 = 196; // for 60 hz
#endif
}

static void usbReset(void)
{
	/* [...] a single ended zero or SE0 can be used to signify a device
	   reset if held for more than 10mS. A SE0 is generated by holding
       both th D- and D+ low (< 0.3V).

	*/
	//usb pins updated for usbasp(PORTB 0 and 1)
	PORTB &= ~(0x01 | 0x02); // Set D+ and D- to 0
	DDRB |= 0x01 | 0x02;
	_delay_ms(15);
	DDRB &= ~(0x01 | 0x02);
}

#if defined(AT168_COMPATIBLE)

#define mustPollControllers()	(TIFR2 & (1<<OCF2A))
#define clrPollControllers()	do { TIFR2 = 1<<OCF2A; } while(0)

#else

#define mustPollControllers()	(TIFR & (1<<OCF2))
#define clrPollControllers()	do { TIFR = 1<<OCF2; } while(0)

#endif


static uchar    reportBuffer[12];    /* buffer for HID reports */



/* ------------------------------------------------------------------------- */
/* ----------------------------- USB interface ----------------------------- */
/* ------------------------------------------------------------------------- */

uchar	usbFunctionDescriptor(struct usbRequest *rq)
{
	if ((rq->bmRequestType & USBRQ_TYPE_MASK) != USBRQ_TYPE_STANDARD)
		return 0;

	if (rq->bRequest == USBRQ_GET_DESCRIPTOR)
	{
		// USB spec 9.4.3, high byte is descriptor type
		switch (rq->wValue.bytes[1])
		{
			case USBDESCR_DEVICE:
				usbMsgPtr = rt_usbDeviceDescriptor;
				return rt_usbDeviceDescriptorSize;
			case USBDESCR_HID_REPORT:
				usbMsgPtr = rt_usbHidReportDescriptor;
				return rt_usbHidReportDescriptorSize;
			case USBDESCR_CONFIG:
				usbMsgPtr = my_usbDescriptorConfiguration;
				return sizeof(my_usbDescriptorConfiguration);
		}
	}

	return 0;
}


static uchar reportPos=0;

uchar	usbFunctionSetup(uchar data[8])
{
	usbRequest_t    *rq = (void *)data;

	usbMsgPtr = reportBuffer;

	/* class request type */
	if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS){
		switch (rq->bRequest)
		{
			case USBRQ_HID_GET_REPORT:
				/* wValue: ReportType (highbyte), ReportID (lowbyte) */
				reportPos=0;
				return curGamepad->buildReport(reportBuffer, rq->wValue.bytes[0]);

		}
	} else {
		/* no vendor specific requests implemented */
	}
	return 0;
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	char must_report = 0;	/* bitfield */
	int i;
	unsigned char run_mode;


	hardwareInit();

	_delay_ms(10); /* let pins settle */
	run_mode = (PINB & 0x06)>>1;

	switch(run_mode)
	{
			// Close JP1 to disable live auto-detect
		case 2:
			//disableLiveAutodetect();
			//let it autodetect, no jumpers for selection
			break;

		case 1:
		case 3:
		case 4:
			break;
	}

	curGamepad = fournsnesGetGamepad();

	// configure report descriptor according to
	// the current gamepad
	rt_usbHidReportDescriptor = curGamepad->reportDescriptor;
	rt_usbHidReportDescriptorSize = curGamepad->reportDescriptorSize;

	if (curGamepad->deviceDescriptor != 0)
	{
		rt_usbDeviceDescriptor = (void*)curGamepad->deviceDescriptor;
		rt_usbDeviceDescriptorSize = curGamepad->deviceDescriptorSize;
	}
	else
	{
		// use descriptor from devdesc.c
		//
		rt_usbDeviceDescriptor = (void*)usbDescrDevice;
		rt_usbDeviceDescriptorSize = getUsbDescrDevice_size();
	}

	// patch the config descriptor with the HID report descriptor size
	my_usbDescriptorConfiguration[25] = rt_usbHidReportDescriptorSize;

	usbReset();
	curGamepad->init();
	usbInit();

	curGamepad->update();

	sei();

	for(;;){	/* main event loop */
		wdt_reset();

		// this must be called at each 50 ms or less
		usbPoll();

		/* Read the controllers at 60hz */
		if (mustPollControllers())
		{
			clrPollControllers();

			curGamepad->update();

			/* Check what will have to be reported */
			for (i=0; i<curGamepad->num_reports; i++) {
				if (curGamepad->changed(i+1)) {
					must_report |= (1<<i);
				}
			}
		}


		if(must_report)
		{
			for (i = 0; i < curGamepad->num_reports; i++)
			{
				if ((must_report & (1<<i)) == 0)
					continue;

				if (usbInterruptIsReady())
				{
					char len;

					len = curGamepad->buildReport(reportBuffer, i+1);
					usbSetInterrupt(reportBuffer, len);

					while (!usbInterruptIsReady())
					{
						usbPoll();
						wdt_reset();
					}
				}
			}

			must_report = 0;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
