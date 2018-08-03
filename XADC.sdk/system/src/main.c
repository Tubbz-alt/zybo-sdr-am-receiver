/*
 * Copyright (C) 2016-2018 Tomas Dubina <t.dubina at volny dot cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Quick and dirty C code for matrix keyboard, 20x2 LCD display, audio codec and FPGA control via GPIO.
*/
#include <stdio.h>
#include <stdlib.h>
#include "math.h"
#include "xparameters.h"
#include "xil_testmem.h"
#include "xadcps.h"
#include "xgpio.h"
#include "xbasic_types.h"
#include "xllfifo.h"
#include "xscugic.h"
#include "xil_exception.h"
#include "xstreamer.h"
#include "xil_cache.h"
#include "xstatus.h"
#include "xiicps.h"
#include "wsfir.h"
#include "global.h"

// LCD module information
#define lcd_LineOne 0x00 // start of line 1
#define lcd_LineTwo 0x40 // start of line 2 //
#define lcd_LineThree 0x14 // start of line 3 (20x4) //
#define lcd_lineFour 0x54 // start of line 4 (20x4) //
#define lcd_LineThree 0x10 // start of line 3 (16x4) //
#define lcd_lineFour 0x50 // start of line 4 (16x4) // LCD instructions
#define lcd_Clear 0b00000001 // replace all characters with ASCII 'space'
#define lcd_Home 0b00000010 // return cursor to first position on first line
#define lcd_EntryMode 0b00000110 // shift cursor from left to right on read/write
#define lcd_DisplayOff 0b00001000 // turn display off
#define lcd_DisplayOn 0b00001100 // display on, cursor off, don't blink character
#define lcd_FunctionReset 0b00110000 // reset the LCD
#define lcd_FunctionSet4bit 0b00101000 // 4-bit data, 2-line display, 5 x 7 font
#define lcd_SetCursor 0b10000000 // set cursor position

// LCD interface (should agree with the diagram above)
// make sure that the LCD RW pin is connected to GND
//#define lcd_D7_port PORTD // lcd D7 connection
#define lcd_D7_bit 0x05
//#define lcd_D7_ddr DDRD
//#define lcd_D6_port PORTD // lcd D6 connection
#define lcd_D6_bit 0x04
//#define lcd_D6_ddr DDRD
//#define lcd_D5_port PORTD // lcd D5 connection
#define lcd_D5_bit 0x03
//#define lcd_D5_ddr DDRD
//#define lcd_D4_port PORTD // lcd D4 connection
#define lcd_D4_bit 0x02
//#define lcd_D4_ddr DDRD
//#define lcd_E_port PORTB // lcd Enable pin
#define lcd_E_bit 0x00
//#define lcd_E_ddr DDRB
//#define lcd_RS_port PORTB // lcd Register Select pin
#define lcd_RS_bit 0x01
//#define lcd_RS_ddr DDRB

//====================================================

// KeyCodes
/* SUM	494
 * 6	493
 * 3	491
 * 9	487
 * DEL	478
 * 5	477
 * 2	475
 * 8	471
 * 7	446
 * 1	445
 * 0	443
 * 4	439
 * CODE	382
 * RUN	381
 * CLS	379
 * ENT	375
 * TEMP	254
 * TIME	253
 * CAL	251
 * PRES	247
 *
 *
 * */

void SendToFifo(u32 *Data);
void SetFIREn(uint8_t adr, uint8_t data);
void SetFIRcoe();
int CheckKey();
void WriteCol();

// Function Prototypes
void lcd_write_4(uint8_t);
void lcd_write_instruction_4d(uint8_t);
void lcd_write_character_4d(uint8_t);
void lcd_write_string_4d(uint8_t *);
void lcd_init_4d(void);

XLlFifo Fifo;
XLlFifo_Config *Fifo_Config;
uint8_t tvalid_data = 0;
uint8_t Button_Status = 0;
uint16_t cr_data = 2048;
uint8_t fir_config = 0;
int32_t fir_data = 0;
int32_t sinus_data = 1;
int32_t pre_sinus_data = 1;
int32_t filtered = 0;
int16_t xadc = 0;
int16_t phase_mag = 0;
int16_t magnitude = 0;
int16_t mix_freq = 0;
int16_t mix_step = 5;
uint8_t enable_out = 1;
uint8_t mKeyWrite = 11;
uint8_t mKeyRead = 0;
uint8_t function = 0;
uint8_t modekey = 0;
uint16_t centerIF = 41650; // v Hz
float minMixStep = 30.51757813; // V Hz
// LSB EN RS DB7 DB6 DB5 DB4
uint8_t lcdOut = 0;
uint16_t menutimer = 0;
uint8_t showtime = 0;
uint8_t showstat = 1;
uint16_t settunefreq = 0;
uint8_t setvolume = 0;
uint8_t setposition = 0;
XGpio Button, PhaseOut, FIRConf, Sinus, TValid, FIROut;
XGpio mKeyBoard, LCD16;
int fir_coeff[256];
float dfir_coeff[2048];
float FilterOut[2048];

#define SSM2603_IIC_ADDRESS		0x1A
#define INIT_COUNT 11*2
static XIicPs iic;

uint8_t SSM2603_Init[INIT_COUNT] = {
 30 | 0x00, 0x00 ,    /* R15: RESET */
  0 | 0x01, 0x17 ,    /* R0: L_in vol : LR simul-update, unmute, 0dB */
  2 | 0x01, 0x17 ,    /* R1: R_in vol : LR simul-update, unmute, 0dB */
  4 | 0x01, 0xF9 ,    /* R2: L_HP vol : LR simul-update, zero-cross, 0dB */
  6 | 0x01, 0xF9 ,    /* R3: R_HP vol : LR simul-update, zero-cross, 0dB */
  8 | 0x00, 0x12 ,    /* R4: Analog Audio Path : No Sidetone, No bypass, DAC for Out, Line out for ADC, Mic Mute */
 10 | 0x00, 0x00 ,    /* R5: Digital Path: DAC unmute, De-emphasis 48k, ADC HPF enable */
 12 | 0x00, 0x02 ,    /* R6: Power Down : Microphone is down*/
// 14 | 0x00, 0x4E ,    /* R7: Digital Audio Format : Master, 32bit, I2S */
 14 | 0x00, 0x0E ,    /* R7: Digital Audio Format : Slave, 16bit, I2S */
 16 | 0x00, 0x11 ,    /* R8: Sampling Rate, 12(48)kHz, USB mode*/
 18 | 0x00, 0x01      /* R9: Activation : Active. */
};

int main (void)
{
		xil_printf("-- Programmstart--\r\n");
	    // GPIO konfigurieren
	    XGpio_Initialize(&Button, XPAR_GPIO_0_DEVICE_ID);
	    XGpio_Initialize(&PhaseOut, XPAR_GPIO_3_DEVICE_ID);
	    XGpio_Initialize(&Sinus, XPAR_GPIO_2_DEVICE_ID);
	    XGpio_Initialize(&FIRConf, XPAR_GPIO_4_DEVICE_ID);
	    XGpio_Initialize(&TValid, XPAR_GPIO_5_DEVICE_ID);
	    XGpio_Initialize(&FIROut, XPAR_GPIO_1_DEVICE_ID);
	    XGpio_Initialize(&mKeyBoard, XPAR_GPIO_6_DEVICE_ID);
	    XGpio_Initialize(&LCD16, XPAR_GPIO_7_DEVICE_ID);
	    /*Set the direction for output. */
	    XGpio_SetDataDirection(&PhaseOut, 1, 0x00);
	    XGpio_SetDataDirection(&FIRConf, 1, 0x00);
	    XGpio_SetDataDirection(&FIRConf, 2, 0x00);
	    XGpio_SetDataDirection(&TValid, 1, 0x00);

	    XGpio_SetDataDirection(&mKeyBoard, 1, 0x00);
	    XGpio_SetDataDirection(&LCD16, 1, 0x00);
	    XGpio_SetDataDirection(&LCD16, 2, 0x00);
	    /*Set the direction for input. */
	    XGpio_SetDataDirection(&Button, 1, 0x01);
	    XGpio_SetDataDirection(&mKeyBoard, 2, 0x01);
	    XGpio_SetDataDirection(&Sinus, 2, 0x01);
	    XGpio_SetDataDirection(&Sinus, 1, 0x01);
	    XGpio_SetDataDirection(&FIROut, 1, 0x01);

    // FIFO konfigurieren
    Fifo_Config = XLlFfio_LookupConfig(XPAR_AXI_FIFO_0_DEVICE_ID);
    XLlFifo_CfgInitialize(&Fifo, Fifo_Config, Fifo_Config->BaseAddress);

    initI2C();
    InitSSM2603();

    //SendToFifo(1);
//    	int i=0;
//    	for (i=0;i<256;i++) {
//    		fir_coeff[i]=32700;
//    	}
    	 lcd_init_4d();
    	 //lcd_write_string_4d("Demod.");
    	 //lcd_write_instruction_4d(lcd_SetCursor | lcd_LineTwo);
    	 //delay_us(80); // 40 uS delay (min)
    	 //lcd_write_string_4d("Vse");
    	 usleep(100000); // 100ms
    	 lcd_init_4d();
    	 printf("\n");
    	tvalid_data &= ~(1 << 0);
    	printf("Demo AM, true \n");
    	printf("%i\n",int_to_int(tvalid_data,2));
    	tvalid_data &= ~(1 << 1);
    	printf("Direct in, false \n");
    	printf("%i\n",int_to_int(tvalid_data,2));
    	tvalid_data &= ~(1 << 2);
    	printf("Filter BP 40kHz, 10kHz, false \n");
    	printf("%i\n",int_to_int(tvalid_data,2));
    	printf("");
    	xil_printf("-- Init done, loop--\r\n");
    	delay_us(80); // 40 uS delay (min)
    	lcd_write_string_4d("-- Init done    ");
    while(1)
    {
    	Button_Status = XGpio_DiscreteRead(&Button, 1);
    	int filterDownCL = XGpio_DiscreteRead(&Sinus, 1);
    	if(Button_Status == 1)
    	{
    		xil_printf("Button 1 pres,  Enter frequency in kHz! FC1\n\r");
    		//enable_out = 0;
    		float freq1=92;
    		//scanf("FC1 %f", &freq1);
    		//xil_printf("Button 1 pres,  Enter frequency in kHz! FC2\n\r");
    		float freq2=108;
    		//scanf("FC2 %f", &freq2);
    		freq1 = freq1/1000;
    		freq2 = freq2/1000;
    		//wsfirBP(512, 3, freq1, freq2);
    		printf("Gen Ok\n\r");
    		//SetFIRcoe();
    		printf("Set Ok\n\r");
    		sinus_data -= 1;
    		usleep(10000);
    	}
    	else if((Button_Status >> 1) == 1)
    	{
    		xil_printf("Button 2 \n\r");
    		tvalid_data = 0;
    		usleep(100000);
    		//printf("%i\n\r",cr_data);
    	}
    	else if((Button_Status >> 2) == 1)
    	{
    		xil_printf("Button 3 \n\r");
    		tvalid_data = 1;
    		usleep(100000);
    		printf("%i\n\r",cr_data);
    	}
    	else if((Button_Status >> 3) == 1)
    	{
    		printf("Button 4 -- IN: \n\r");
    		//enable_out = 1;
    		sinus_data += 1;
    		usleep(10000);
    	}
    			filtered = XGpio_DiscreteRead(&FIROut, 1);
    	    	//sinus_data = XGpio_DiscreteRead(&Sinus, 1);
    	    	phase_mag = XGpio_DiscreteRead(&Sinus, 2);
    	    	//phase_mag = phase_mag << 16;
    	    	//phase_mag = phase_mag >> 16;
    	    	magnitude = phase_mag;
    	    if (enable_out == 0) {
    	    	printf("H"); /* Unique header to identify start of message */
    	  	    printf(",");
    	   	    printf("%i", int_to_int(filterDownCL, 15));
    	   	    // 16384
    	        printf(",");
    	   	    printf("%i",sinus_data);
    	   	    printf(",");
    	        printf("%i",phase_mag);
    		    printf(",");  /* Note that a comma is sent after the last field */
    		    printf("\n");  /* send a cr/lf */
    	    }
    	    	int numeKey = CheckKey();
    	    	//printf("%i\n",numeKey);
    	    	// START KEYBOARD
    	    	if (numeKey==254)	{ // TEMP
    	    		menutimer = 0;
    	    		printf("TEMP_FCN\n");
    	    	 	function = 1;
    	    	 	lcd_write_instruction_4d(0x80);
    	    	 	delay_us(80);
    	    	 	lcd_write_string_4d("Output volume       ");
    	    	 	delay_us(80);
    	    	 	lcd_write_instruction_4d(0x80|0x40);
    	    	 	delay_us(80); // 40 uS delay (min)
    	    	 	 //PrintLCDVar(sinus_data);
    	    	 	lcd_write_string_4d("<4 ; 6> code        ");
    	    	}
    	    	else if (numeKey==253){ // CLOCK
    	    		menutimer = 0;
    	    			printf("TIME_FCN\n");
    	    	    	function = 2;
    	    	    	lcd_write_instruction_4d(0x80);
    	    	    	delay_us(80);
    	    	    	lcd_write_string_4d("Tune frequency    ");
    	    	    	delay_us(80);
    	    	    	lcd_write_instruction_4d(0x80|0x40);
    	    	    	delay_us(80); // 40 uS delay (min)
    	    	    	//PrintLCDVar(sinus_data);
    	    	    	lcd_write_string_4d("<4 ; 6> code      ");
    	    	   }
    	    	else if (numeKey==382) { // CODE
    	    		menutimer = 0;
    	    		if (function == 1) {
    	    			modekey=1;
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Enter Volume        ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			lcd_write_string_4d("                    ");
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    		}
    	    		if (function == 2) {
    	    		    modekey=2;
    	    		    delay_us(80);
    	    		    lcd_write_instruction_4d(0x80);
    	    		    delay_us(80);
    	    		    lcd_write_string_4d("Enter Frequency     ");
    	    		    delay_us(80);
    	    		    lcd_write_instruction_4d(0x80|0x40);
    	    		    lcd_write_string_4d("                    ");
    	    		    lcd_write_instruction_4d(0x80|0x40);
    	    		    delay_us(80); // 40 uS delay (min)
    	    		}

    	    	}
    	    	else if (numeKey==445) { //1
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    			if (setposition == 0) {
    	    				pre_sinus_data = 1;
    	    				setposition=1;
    	    				lcd_write_string_4d("1");
    	    			}
    	    			else if (setposition==1) {
    	    				pre_sinus_data *= 10;
    	    				pre_sinus_data +=1;
    	    				setposition=2;
    	    				lcd_write_string_4d("1");
    	    			}
    	    			else if (setposition==2) {
    	    				pre_sinus_data *=10;
    	    				pre_sinus_data +=1;
    	    				setposition=3;
    	    				lcd_write_string_4d("1 FULL");
    	    			}
    	    			usleep(700000);
    	    		}
    	    		if (modekey==2 && function==2) {
    	    		   if (setposition == 0) {
    	    		    	settunefreq = 1;
    	    		        setposition=1;
    	    		        lcd_write_string_4d("1");
    	    		    }
    	    		    else if (setposition==1) {
    	    		    	settunefreq *= 10;
    	    		    	settunefreq +=1;
    	    		    	setposition=2;
    	    		    	lcd_write_string_4d("1");
    	    		    }
    	    		   else if (setposition==2) {
    	    		    	settunefreq *=10;
    	    		    	settunefreq +=1;
    	    		    	setposition=3;
    	    		    	lcd_write_string_4d("1.");
    	    		      }
    	    		   else if (setposition==3) {
    	    		    	settunefreq *=10;
    	    		    	settunefreq +=1;
    	    		    	setposition=4;
    	    		    	lcd_write_string_4d("1 FULL");
    	    		    }
    	    		  usleep(700000);
    	    	   }

    	    	}
    	    	else if (numeKey==475) { //2
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		   if (setposition == 0) {
    	    		    	 pre_sinus_data = 2;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("2");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data += 2;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("2");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=2;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("2 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 2;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("2");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=2;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("2");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=2;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("2.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=2;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("2 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	else if (numeKey==491) { //3
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	 pre_sinus_data = 3;
    	    		    	    		    	    				setposition=1;
    	    		    	    		    	    				lcd_write_string_4d("3");
    	    		    	    		    	    			}
    	    		    	    		    	    			else if (setposition==1) {
    	    		    	    		    	    				pre_sinus_data *= 10;
    	    		    	    		    	    				pre_sinus_data += 3;
    	    		    	    		    	    				setposition=2;
    	    		    	    		    	    				lcd_write_string_4d("3");
    	    		    	    		    	    			}
    	    		    	    		    	    			else if (setposition==2) {
    	    		    	    		    	    				pre_sinus_data *=10;
    	    		    	    		    	    				pre_sinus_data +=3;
    	    		    	    		    	    				setposition=3;
    	    		    	    		    	    				lcd_write_string_4d("3 FULL");
    	    		    	    		    	    			}
    	    		    	    		    	    			usleep(700000);
    	    		    	    		    	    		}
    	    		if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 3;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("3");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=3;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("3");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=3;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("3.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=3;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("3 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	// ODECITS
    	    	else if (numeKey==439) // 4
    	    	    {
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	 pre_sinus_data = 4;
    	    		    	    		    	    				setposition=1;
    	    		    	    		    	    				lcd_write_string_4d("4");
    	    		    	    		    	    			}
    	    		    	    		    	    			else if (setposition==1) {
    	    		    	    		    	    				pre_sinus_data *= 10;
    	    		    	    		    	    				pre_sinus_data += 4;
    	    		    	    		    	    				setposition=2;
    	    		    	    		    	    				lcd_write_string_4d("4");
    	    		    	    		    	    			}
    	    		    	    		    	    			else if (setposition==2) {
    	    		    	    		    	    				pre_sinus_data *=10;
    	    		    	    		    	    				pre_sinus_data +=4;
    	    		    	    		    	    				setposition=3;
    	    		    	    		    	    				lcd_write_string_4d("4 FULL");
    	    		    	    		    	    			}
    	    		    	    		    	    			usleep(700000);
    	    		    	    		    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 4;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("4");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=4;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("4");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=4;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("4.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=4;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("4 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    		else if(function == 1) {
    	    			printf("4_LOW:VL\n");
    	    			sinus_data -= 1;
    	    			if (sinus_data < 0)
    	    				sinus_data = 0;
    	    			printf("%i\n",sinus_data);
    	    			//lcd_write_instruction_4d(0x01);
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Volume --           ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			PrintLCDVar(sinus_data);
    	    			lcd_write_string_4d("x                   ");
    	    		}
    	    		else if (function == 2) {
    	    			printf("4_LOW:LO\n");
    	    			mix_freq -= mix_step;
    	    			if (mix_freq < 0)
    	    				mix_freq = 0;
    	    			printf("%i\n",mix_freq);
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Frequency -- khz     ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			float tunefreqmix = mix_freq * minMixStep; // v Hertz
    	    			tunefreqmix += centerIF;
    	    			if (tunefreqmix<1000){
    	    				lcd_write_string_4d("0.");
    	    				int freqmix2 = tunefreqmix / 100;
    	    				//freqmix2 = roundf(freqmix2 * 100) / 100;
    	    				PrintLCDVar(freqmix2);
    	    				lcd_write_string_4d("                ");
    	    			}
    	    			if (tunefreqmix>=1000){
    	    			    float freqmix10 = tunefreqmix / 1000; // kHz
    	    			    float freqmix11 = roundf(freqmix10 * 100) / 100; // 1 mista
    	    			    int freqmix4 = freqmix11; // cele kHz
    	    			    PrintLCDVar(freqmix4);
    	    			    lcd_write_string_4d(".");
    	    			    float freqmixfrac = freqmix11 - freqmix4;
    	    			    int freqmix5 = freqmixfrac * 100;
    	    			    if (freqmixfrac < 0.1) {
    	    			    	lcd_write_string_4d("0");
    	    			    }
    	    			    printf("deset: %i", freqmix5);
    	    			    PrintLCDVar(freqmix5);
    	    			    lcd_write_string_4d("                ");
    	    			}

    	    		}
    	    		else if (function == 3) {
    	    		  printf("4_LOW:TNST\n");
    	    		  mix_step -= 1;
    	    		  if (mix_step == 0) {
    	    		      mix_step = 1;
    	    		  }
    	    		  printf("%i\n",mix_step);
    	    		  lcd_write_instruction_4d(0x80);
    	    		  delay_us(80);
    	    		  lcd_write_string_4d("Tune step --         ");
    	    		  delay_us(80);
    	    		  lcd_write_instruction_4d(0x80|0x40);
    	    		  delay_us(80); // 40 uS delay (min)
    	    		  int mixer_step_int = mix_step * minMixStep;
    	    		  PrintLCDVar(mixer_step_int);
    	    		  lcd_write_string_4d(" Hz                  ");
    	    		  usleep(5000);
    	    		}
    	    	}
    	    	else if (numeKey==477) { //5
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 5;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("5");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=5;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("5");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=5;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("5 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 5;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("5");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=5;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("5");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=5;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("5.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=5;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("5 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	// pRICIST
    	    	else if (numeKey==493) // 6
    	    	    {
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 6;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("6");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=6;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("6");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=6;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("6 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    			    	    		   if (setposition == 0) {
    	    		    	    			    	    		    	settunefreq = 6;
    	    		    	    			    	    		        setposition=1;
    	    		    	    			    	    		        lcd_write_string_4d("6");
    	    		    	    			    	    		    }
    	    		    	    			    	    		    else if (setposition==1) {
    	    		    	    			    	    		    	settunefreq *= 10;
    	    		    	    			    	    		    	settunefreq +=6;
    	    		    	    			    	    		    	setposition=2;
    	    		    	    			    	    		    	lcd_write_string_4d("6");
    	    		    	    			    	    		    }
    	    		    	    			    	    		   else if (setposition==2) {
    	    		    	    			    	    		    	settunefreq *=10;
    	    		    	    			    	    		    	settunefreq +=6;
    	    		    	    			    	    		    	setposition=3;
    	    		    	    			    	    		    	lcd_write_string_4d("6.");
    	    		    	    			    	    		      }
    	    		    	    			    	    		   else if (setposition==3) {
    	    		    	    			    	    		    	settunefreq *=10;
    	    		    	    			    	    		    	settunefreq +=6;
    	    		    	    			    	    		    	setposition=4;
    	    		    	    			    	    		    	lcd_write_string_4d("6 FULL");
    	    		    	    			    	    		    }
    	    		    	    			    	    		  usleep(700000);
    	    		    	    			}


    	    		else if (function == 1) {
    	    			printf("4_LOW:VL\n");
    	    			sinus_data += 1;
    	    			printf("%i\n",sinus_data);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Volume ++           ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			PrintLCDVar(sinus_data);
    	    			lcd_write_string_4d("x                   ");
    	    		}
    	    		else if (function == 2) {
    	    			printf("4_LOW:LO\n");
    	    			mix_freq += mix_step;
    	    			printf("%i\n",mix_freq);
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Frequency ++ khz    ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			float tunefreqmix = mix_freq * minMixStep; // v Hertz
    	    			tunefreqmix += centerIF;
    	    			if (tunefreqmix<1000){
    	    			    	   lcd_write_string_4d("0.");
    	    			    	   int freqmix2 = tunefreqmix / 100;
    	    			    	   //freqmix2 = roundf(freqmix2 * 100) / 100;
    	    			    	   PrintLCDVar(freqmix2);
    	    			    	   lcd_write_string_4d("                ");
    	    			}
    	    			if (tunefreqmix>=1000){
    	    			    	   float freqmix10 = tunefreqmix / 1000; // kHz
    	    			    	   float freqmix11 = roundf(freqmix10 * 100) / 100; // 1 mista
    	    			    	   int freqmix4 = freqmix11; // cele kHz
    	    			    	   PrintLCDVar(freqmix4);
    	    			    	   lcd_write_string_4d(".");
    	    			    	   float freqmixfrac = freqmix11 - freqmix4; // jen des
    	    			    	   int freqmix5 = freqmixfrac * 100;
    	    			    	   if (freqmixfrac < 0.1) {
    	    			    	      lcd_write_string_4d("0");
    	    			    	   }
    	    			    	   printf("deset: %i", freqmix5);
    	    			    	   PrintLCDVar(freqmix5);
    	    			    	   lcd_write_string_4d("                ");
    	    			}
    	    		}
    	    		else if (function == 3) {
    	    			printf("4_LOW:TNST\n");
    	    			mix_step += 1;
    	    			if (mix_step > 32) {
    	    				mix_step = 32;
    	    			}
    	    			printf("%i\n",mix_step);
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Tune step ++         ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			int mixer_step_int = mix_step * minMixStep;
    	    			PrintLCDVar(mixer_step_int);
    	    			lcd_write_string_4d(" Hz                  ");
    	    			usleep(5000);
    	    		}
    }
    	    	else if (numeKey==446) { //7
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 7;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("7");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=7;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("7");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=7;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("7 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 7;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("7");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=7;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("7");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=7;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("7.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=7;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("7 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	else if (numeKey==471) { //8
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 8;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("8");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=8;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("8");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=8;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("8 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 8;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("8");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=8;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("8");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=8;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("8.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=8;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("8 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	else if (numeKey==487) { //9
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 9;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("9");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=9;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("9");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=9;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("9 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		else if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 9;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("9");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=9;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("9");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=9;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("9.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=9;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("9 FULL");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }
    	    	}
    	    	else if (numeKey==443) { //0
    	    		menutimer = 0;
    	    		if (modekey==1 && function==1) {
    	    		    	    			if (setposition == 0) {
    	    		    	    				pre_sinus_data = 0;
    	    		    	    				setposition=1;
    	    		    	    				lcd_write_string_4d("0");
    	    		    	    			}
    	    		    	    			else if (setposition==1) {
    	    		    	    				pre_sinus_data *= 10;
    	    		    	    				pre_sinus_data +=0;
    	    		    	    				setposition=2;
    	    		    	    				lcd_write_string_4d("0");
    	    		    	    			}
    	    		    	    			else if (setposition==2) {
    	    		    	    				pre_sinus_data *=10;
    	    		    	    				pre_sinus_data +=0;
    	    		    	    				setposition=3;
    	    		    	    				lcd_write_string_4d("0 FULL");
    	    		    	    			}
    	    		    	    			usleep(700000);
    	    		    	    		}
    	    		if (modekey==2 && function==2) {
    	    		    	    		   if (setposition == 0) {
    	    		    	    		    	settunefreq = 0;
    	    		    	    		        setposition=1;
    	    		    	    		        lcd_write_string_4d("0");
    	    		    	    		    }
    	    		    	    		    else if (setposition==1) {
    	    		    	    		    	settunefreq *= 10;
    	    		    	    		    	settunefreq +=0;
    	    		    	    		    	setposition=2;
    	    		    	    		    	lcd_write_string_4d("0");
    	    		    	    		    }
    	    		    	    		   else if (setposition==2) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=0;
    	    		    	    		    	setposition=3;
    	    		    	    		    	lcd_write_string_4d("0.");
    	    		    	    		      }
    	    		    	    		   else if (setposition==3) {
    	    		    	    		    	settunefreq *=10;
    	    		    	    		    	settunefreq +=0;
    	    		    	    		    	setposition=4;
    	    		    	    		    	lcd_write_string_4d("0 FULL    ");
    	    		    	    		    }
    	    		    	    		  usleep(700000);
    	    		    	    	   }

    	    	}
    	    	else if (numeKey==375) { //ENTER
    	    		if (modekey==1 && function==1) {
    	    			sinus_data = pre_sinus_data;
    	    			function=0;
    	    			modekey=0;
    	    			setposition=0;
    	    		}
    	    		if (modekey==2 && function==2) {
    	    			if (setposition==2) {
    	    			    	settunefreq *= 10;
    	    			    	setposition==4;
    	    			}
    	    			else if (setposition==3) {
    	    				settunefreq *= 10;
    	    			    setposition==4;
    	    			}
    	    			float settunefreqFL = settunefreq * 100; // Hz
    	    			if (settunefreqFL < 40000) {
    	    				settunefreqFL = 40000;
    	    			}

    	    			float settunefreqNIF = settunefreqFL - centerIF;
    	    			settunefreqNIF /= minMixStep;
    	    			mix_freq = settunefreqNIF;
    	    		    function=0;
    	    		    modekey=0;
    	    		    setposition=0;
    	    		}


    	      }
    	    	else if (numeKey==247)  // Pressure key
    	    	{
    	    		menutimer = 0;
    	    		printf("PRESS_FCN\n");
    	    		function = 3;
    	    		lcd_write_instruction_4d(0x80);
    	    		delay_us(80);
    	    		lcd_write_string_4d("Tune step ");
    	    		delay_us(80);
    	    		int mixer_step_int = mix_step * minMixStep;
    	    		PrintLCDVar(mixer_step_int);
    	    		lcd_write_string_4d(" Hz      ");
    	    		lcd_write_instruction_4d(0x80|0x40);
    	    		delay_us(80); // 40 uS delay (min)
    	    		//PrintLCDVar(sinus_data);
    	    		lcd_write_string_4d("<4 ; 6>           ");

    	    	}
    	    	// KeyCodes
    	    	/* SUM	494
    	    	 * 6	493
    	    	 * 3	491
    	    	 * 9	487
    	    	 * DEL	478
    	    	 * 5	477
    	    	 * 2	475
    	    	 * 8	471
    	    	 * 7	446
    	    	 * 1	445
    	    	 * 0	443
    	    	 * 4	439
    	    	 * CODE	382
    	    	 * RUN	381
    	    	 * CLS	379
    	    	 * ENT	375
    	    	 * TEMP	254
    	    	 * TIME	253
    	    	 * CAL	251
    	    	 * PRES	247
    	    	 *
    	    	 *
    	    	 * */
    	    	else if (numeKey==251) //CALL LIST
    	    	{
    	    		printf("SWT %i", showtime);
    	    		menutimer = 0;
    	    		function = 10;
    	    		if (showstat == 0 && showtime == 41) {
    	    			lcd_write_instruction_4d(0x80);
    	    			delay_us(80);
    	    			lcd_write_string_4d("Mode                ");
    	    			delay_us(80);
    	    			lcd_write_instruction_4d(0x80|0x40);
    	    			delay_us(80); // 40 uS delay (min)
    	    			lcd_write_string_4d("Audio NF             ");
    	    			printf("CAL_SET_DIR_IN_TRUE\n");
    	    		    printf("CAL_SET_FILTER_USE_FALSE\n");
    	    			tvalid_data |= 1 << 1;
    	    			tvalid_data &= ~(1 << 2);
    	    			printf("%i\n",int_to_int(tvalid_data,3));
    	    			showtime = 0;
    	    			showstat= 1;
    	    		}
    	    		if (showstat == 1 && showtime == 41) {
    	    		  lcd_write_instruction_4d(0x80);
    	    		  delay_us(80);
    	    		  lcd_write_string_4d("Mode                ");
    	    		  delay_us(80);
    	    		lcd_write_instruction_4d(0x80|0x40);
    	    		delay_us(80); // 40 uS delay (min)
    	    		lcd_write_string_4d("Demodulate all      ");
    	    		printf("SUM_SET_DEM_ALL_TRUE\n");
    	    		printf("SUM_SET_DIR_IN_FALSE\n");
    	    		printf("SUM_SET_FILTER_IN_FALSE\n");
    	    		tvalid_data &= ~(1 << 1);
    	    		tvalid_data &= ~(1 << 2);
    	    		printf("%i\n",int_to_int(tvalid_data,3));
    	    		showstat= 2;
    	    		showtime=0;
    	    		}
    	    		if (showstat == 2 && showtime == 41) {
    	    		    lcd_write_instruction_4d(0x80);
    	    		    delay_us(80);
    	    		    lcd_write_string_4d("Mode                ");
    	    		    delay_us(80);
    	    		    lcd_write_instruction_4d(0x80|0x40);
    	    		    delay_us(80); // 40 uS delay (min)
    	    		    lcd_write_string_4d("Tune frequency      ");
    	    		    printf("PRESS_SET_FILTER_USE_TRUE\n");
    	    		    printf("PRESS_SET_DIR_IN_FALSE\n");
    	    		    tvalid_data &= ~(1 << 1);
    	    		     tvalid_data |= 1 << 2;
    	    		    printf("%i\n",int_to_int(tvalid_data,3));
    	    		    showstat= 0;
    	    		    showtime=0;
    	    		 }

    	    	 }
    	    	// END KEYBOARD
    	    	if (function == 0 && modekey == 0 ) {
    	    		lcd_write_instruction_4d(0x80);
    	    		delay_us(80);
    	    		lcd_write_string_4d("AM  ");
    	    		if (showstat == 1) {
    	    			lcd_write_string_4d("NF              ");
    	    		}
    	    		else if (showstat == 2) {
    	    			lcd_write_string_4d("Dem. all        ");
    	    		}
    	    		else if (showstat == 0) {
    	    			lcd_write_string_4d("Tune            ");
    	    		}
    	    		delay_us(80);
    	    		lcd_write_instruction_4d(0x80|0x40);
    	    		delay_us(80); // 40 uS delay (min)
    	    		float tunefreqmix = mix_freq * minMixStep; // v Hertz
    	    		tunefreqmix += centerIF;
    	    	if (tunefreqmix<1000){
    	    		     lcd_write_string_4d("0.");
    	    		     int freqmix2 = tunefreqmix / 100;
    	    		     //freqmix2 = roundf(freqmix2 * 100) / 100;
    	    		    PrintLCDVar(freqmix2);
    	    		  	lcd_write_string_4d(" kHz ");
    	    		}
    	    	if (tunefreqmix>=1000){
    	    		float freqmix10 = tunefreqmix / 1000; // kHz
    	    		float freqmix11 = roundf(freqmix10 * 100) / 100; // 1 mista
    	    		int freqmix4 = freqmix11; // cele kHz
    	            PrintLCDVar(freqmix4);
    	    		lcd_write_string_4d(".");
    	    		float freqmixfrac = freqmix11 - freqmix4; // jen des
    	    		int freqmix5 = freqmixfrac * 100;
    	    		if (freqmixfrac < 0.1) {
    	    		   lcd_write_string_4d("0");
    	    		}
    	    		PrintLCDVar(freqmix5);
    	    	    lcd_write_string_4d(" kHz ");
    	    	}
    	    		delay_us(80); // 40 uS delay (min)
    	    		PrintLCDVar(sinus_data);
    	    		lcd_write_string_4d("x     ");
    	    	}
    	    	fflush(stdout);
    	    	WriteCol();
    		    XGpio_DiscreteWrite(&PhaseOut, 1, cr_data);
    	    	XGpio_DiscreteWrite(&TValid, 1, tvalid_data);
    	    	XGpio_DiscreteWrite(&FIRConf, 2, sinus_data);
    	    	XGpio_DiscreteWrite(&FIRConf, 1, mix_freq);
    	    	int mixFIR = 4 << 8;
    	    	mixFIR += 10;
    	    	//tvalid_data |= 1 << 3;
    	    	XGpio_DiscreteWrite(&LCD16, 1, mixFIR);
    	    	if (menutimer >= 250) {
    	    		function = 0;
    	    		modekey = 0;
    	    		setposition=0;
    	    	}
    	    	else {
    	    		menutimer++;
    	    	}
    	    	if (showtime > 40) {
    	    	  	//showtime++;

    	    	    }
    	    	else {
    	    	   showtime++;

    	    	}
    	    	usleep(20000);
    }
}


void SendToFifo(u32 *Data)
{
	u32 Datas;
	Datas = XLlFifo_iTxVacancy(&Fifo);
	printf("Data %lu\n\r", Datas);

	XLlFifo_TxPutWord(&Fifo, 1);
	XLlFifo_iTxSetLen(&Fifo, 4);

	while(!(XLlFifo_IsTxDone(&Fifo)))
	{

	}

	Datas = XLlFifo_iTxVacancy(&Fifo);
	printf("Data %lu\n\r", Datas);
}

void SetFIREn(uint8_t adr, uint8_t data) {
	// TValid 3 bits adr, data 0 or 1
	// 0 s_axis_config_tvalid
	// 1 s_axis_reload_tvalid
	// 2 s_axis_reload_tlast
	if (data == 0) {
		tvalid_data &= ~(1 << adr);
	}
	if (data == 1) {
		tvalid_data |= 1 << adr;
	}
	XGpio_DiscreteWrite(&TValid, 1, tvalid_data);
	//int_to_int(tvalid_data, 4);
}

void SetFIRcoe() {
	uint8_t ia = 0;
	// max 101 coefficients, set in Vivado
	uint16_t ik = 0;
	for ( ik = 0; ik < 255; ik++ ) {
	   //printf( "*(p + %i) : %f\n", ika, FilterOut[ika]);
		FilterOut[ik]=FilterOut[ik]*65536;
		fir_coeff[ik]=(int)FilterOut[ik];
		//fir_coeff[ik]=(int)dfir_coeff[ik];
	   //printf( "%i: %i\n", ika, FirOT[ika]);
	}
	for (ia=0;ia<255;ia++) {
		SetFIREn(0,1);
		XGpio_DiscreteWrite(&FIRConf, 1, ia);
		usleep(50);
		SetFIREn(0,0);
		SetFIREn(1,1);
		if(ia==255) {
			SetFIREn(2,1);
		}
		XGpio_DiscreteWrite(&FIRConf, 2, fir_coeff[ia]);
		usleep(50);
		SetFIREn(1,0);
		SetFIREn(2,0);
	}
}

int CheckKey(){
	mKeyRead = XGpio_DiscreteRead(&mKeyBoard, 2);
	return  (mKeyRead << 4)+mKeyWrite;
	//from LSB 14, 13, 11, 7
}

void WriteCol() {
	if (mKeyWrite==14) {
		mKeyWrite=13;
	}
	else if (mKeyWrite==13) {
		mKeyWrite=11;
	}
	else if (mKeyWrite==11) {
		mKeyWrite=7;
	}
	else if (mKeyWrite==7) {
		mKeyWrite=14;
	}
	XGpio_DiscreteWrite(&mKeyBoard, 1, mKeyWrite);
}

void PrintLCDVar(int number) {
		int n;
	if (number == 0) {
	    n = 1;
		}
	else {
		n = log10(number) + 1;
	}
	   int i;
	   int nmar = calloc(n, sizeof(char)) + 1;
	   int npos = nmar - 1;
	   char numberArray[nmar];
	   for ( i = 0; i < n; ++i, number /= 10 ) {
	    	     numberArray[i] = number % 10;
	    	     //printf("ps %i, val %i\n",i, numberArray[i]);
       }

      while ( i-- ) {
		  if(numberArray[i]==0)
			  lcd_write_string_4d("0");
		  //   printf("%i",numberArray[i]);
		  if(numberArray[i]==1)
		  			  lcd_write_string_4d("1");
		  //  printf("%i",numberArray[i]);
		  if(numberArray[i]==2)
		  			  lcd_write_string_4d("2");
		  //   printf("%i",numberArray[i]);
		  if(numberArray[i]==3)
		  			  lcd_write_string_4d("3");
		  //   printf("%i",numberArray[i]);
		  if(numberArray[i]==4)
		  			  lcd_write_string_4d("4");
		  //  printf("%i",numberArray[i]);
		  if(numberArray[i]==5)
		      lcd_write_string_4d("5");
		  //  printf("%i",numberArray[i]);
		  if(numberArray[i]==6)
		  			  lcd_write_string_4d("6");
		  //   printf("%i",numberArray[i]);
		  if(numberArray[i]==7)
		  			  lcd_write_string_4d("7");
		  //   printf("%i",numberArray[i]);
		  if(numberArray[i]==8)
		  			  lcd_write_string_4d("8");
		  //  printf("%i",numberArray[i]);
		  if(numberArray[i]==9)
		  			  lcd_write_string_4d("9");
		 // printf("%i",numberArray[i]);
	   }
}

uint8_t I2C_Read1Byte(uint8_t address)
{
	uint8_t SendBuffer[1];
	uint8_t RecvBuffer[1] = {0x00};

	SendBuffer[0] = address;
	XIicPs_MasterSendPolled(&iic, SendBuffer, 1, SSM2603_IIC_ADDRESS);
	while (XIicPs_BusIsBusy(&iic)) {}
	XIicPs_MasterRecvPolled(&iic, RecvBuffer, 1, SSM2603_IIC_ADDRESS);
	while (XIicPs_BusIsBusy(&iic)) {}
	return (RecvBuffer[0]);
}

void I2C_Write1Byte(uint8_t address, uint8_t data)
{
	uint8_t SendBuffer[2];

	SendBuffer[0] = address;
	SendBuffer[1] = data;
	XIicPs_MasterSendPolled(&iic, SendBuffer, 2, SSM2603_IIC_ADDRESS);
	while (XIicPs_BusIsBusy(&iic)) {}
}

int initI2C(void)
{
	XIicPs_Config *Config;
	int Status = XST_SUCCESS;

	Config = XIicPs_LookupConfig(XPAR_XIICPS_0_DEVICE_ID);
	XIicPs_CfgInitialize(&iic, Config, Config->BaseAddress);
	XIicPs_SetSClk(&iic, 400000);

	return Status;
}

void InitSSM2603(void)
{
	int i;
	for (i = 0; i < 11; i++)
		I2C_Write1Byte(SSM2603_Init[i * 2], SSM2603_Init[i * 2 + 1]);
}

int int_to_int(int n, int pk) {
	int c, k;
	for (c = pk; c >= 0; c--)
	  {
	    k = n >> c;

	    if (k & 1)
	      printf("1");
	    else
	      printf("0");
	  }
}

// Write to GPIO outputs
void LCDwrite()
{
  XGpio_DiscreteWrite(&LCD16, 2, lcdOut);
}

// Delay function (microseconds)
void delay_us(Xuint32 time)
{
  usleep(time);
}

// Delay function (milliseconds)
void delay_ms(Xuint32 time)
{
  usleep(time*1000);
}

void lcd_init_4d(void) { // Power-up delay
	delay_ms(100); // initial 40 mSec delay
	 // Set up the RS and E lines for the 'lcd_write_4' subroutine.
	lcdOut &= ~(1<<lcd_RS_bit); // select the Instruction Register (RS low)
	//LCDwrite();
	lcdOut &= ~(1<<lcd_E_bit); // make sure E is initially low
	LCDwrite();

	// Reset the LCD controller
	lcd_write_4(lcd_FunctionReset); // first part of reset sequence
	delay_ms(10); // 4.1 mS delay (min)
	lcd_write_4(lcd_FunctionReset); // second part of reset sequence
	delay_us(200); // 100uS delay (min)
	lcd_write_4(lcd_FunctionReset); // third part of reset sequence
	delay_us(200); // this delay is omitted in the data sheet

	// Preliminary Function Set instruction - used only to set the 4-bit mode.
	// The number of lines or the font cannot be set at this time since the controller is still in the
	// 8-bit mode, but the data transfer mode can be changed since this parameter is determined by one
	// of the upper four bits of the instruction.
	lcd_write_4(lcd_FunctionSet4bit); // set 4-bit mode HD4488
	delay_us(80); // 40uS delay (min)
	// Function Set instruction
	lcd_write_instruction_4d(lcd_FunctionSet4bit);  // set mode, lines, and font

	// The next three instructions are specified in the data sheet as part of the initialization routine,
	// so it is a good idea (but probably not necessary) to do them just as specified and then redo them
	// later if the application requires a different configuration.

	// Display On/Off Control instruction
	lcd_write_instruction_4d(lcd_DisplayOff); // turn display OFF
	delay_us(80); // 40uS delay (min)
	// Clear Display instruction
	lcd_write_instruction_4d(lcd_Clear); // clear display RAM
	delay_ms(4); // 1.64 mS delay (min)

	// ; Entry Mode Set instruction
	lcd_write_instruction_4d(lcd_EntryMode); // set desired shift characteristics
	delay_us(80); // 40uS delay (min)
	// This is the end of the LCD controller initialization as specified in the data sheet, but the display
	// has been left in the OFF condition. This is a good time to turn the display back ON.
	// Display On/Off Control instruction
	lcd_write_instruction_4d(lcd_DisplayOn); // turn the display ON
	delay_us(80); // 40uS delay (min)
}

void lcd_write_string_4d(uint8_t theString[]) {
	volatile int i = 0; // character counter*/
	while (theString[i] != 0) {
		lcd_write_character_4d(theString[i]);
		i++;
		delay_us(80); // 40 uS delay (min)
	}
}

void lcd_write_character_4d(uint8_t theData) {
	lcdOut |= (1<<lcd_RS_bit); // select the Data Register (RS high)
	//LCDwrite();
	lcdOut &= ~(1<<lcd_E_bit); // make sure E is initially low
	LCDwrite();
	//printf("WCH %i\n",theData);
	lcd_write_4(theData); // write the upper 4-bits of the data
	lcd_write_4(theData << 4); // write the lower 4-bits of the data }
}

void lcd_write_instruction_4d(uint8_t theInstruction) {
	lcdOut &= ~(1<<lcd_RS_bit); // select the Instruction Register (RS low)
	//LCDwrite();
	lcdOut &= ~(1<<lcd_E_bit); // make sure E is initially low
	LCDwrite();
	lcd_write_4(theInstruction); // write the upper 4-bits of the data
	lcd_write_4(theInstruction << 4); // write the lower 4-bits of the data }
}

void lcd_write_4(uint8_t theByte) {
	lcdOut &= ~(1<<lcd_D7_bit); // assume that data is '0'
	//LCDwrite();
	if (theByte & 1<<7) {
		lcdOut |= (1<<lcd_D7_bit); // make data = '1' if necessary
		//LCDwrite();
	}
	lcdOut &= ~(1<<lcd_D6_bit); // repeat for each data bit
	//LCDwrite();
	if (theByte & 1<<6) {
		lcdOut |= (1<<lcd_D6_bit);
		//LCDwrite();
	}
	lcdOut &= ~(1<<lcd_D5_bit);
	//LCDwrite();
	if (theByte & 1<<5) {
		lcdOut |= (1<<lcd_D5_bit);
	   // LCDwrite();
	}
	lcdOut &= ~(1<<lcd_D4_bit);
	//LCDwrite();
	if (theByte & 1<<4) {
		lcdOut |= (1<<lcd_D4_bit);
		//LCDwrite();
	}
	LCDwrite();
	// write the data // 'Address set-up time' (40 nS)
	lcdOut |= (1<<lcd_E_bit); // Enable pin high
	LCDwrite();
	delay_us(1); // implement 'Data set-up time' (80 nS) and 'Enable pulse width' (230 nS)
	lcdOut &= ~(1<<lcd_E_bit); // Enable pin low
	LCDwrite();
	delay_us(1); // implement 'Data hold time' (10 nS) and 'Enable cycle time' (500 nS)
}