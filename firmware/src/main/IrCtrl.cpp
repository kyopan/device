/*----------------------------------------------------------------------------/
/  IR_CTRL - IR remote control module                         (C)ChaN, 2008
/-----------------------------------------------------------------------------/
/ The IR_CTRL is a generic Transmisson/Reception control module for IR remote
/ control systems. This is a free software and is opened for education,
/ research and development under license policy of following trems.
/
/  Copyright (C) 2008, ChaN, all right reserved.
/  Copyright (C) 2013-, Masakazu Ohtsuka (mash), all right reserved.
/
/ * The IR_CTRL module is a free software and there is no warranty.
/ * You can use, modify and/or redistribute it for personal, non-profit or
/   commercial use without restriction under your responsibility.
/ * Redistributions of source code must retain the above copyright notice.
/
/-----------------------------------------------------------------------------/
/ Aug 30,'08 R0.01  First release.
/ May 16,'13 R0.    mash heavily modified everything
/----------------------------------------------------------------------------*/

#include <Arduino.h>
#include "IrCtrl.h"
#include "pgmStrToRAM.h"

// we use ATmega328P-AU
// avr/iom32u4.h
#define WGM10 0
#define WGM11 1
#define WGM12 3

// avr/sfr_defs.h
#define _BV(bit) (1 << (bit))

/*----------------------------------------------------------------------------/
/ How this works (RX)
/-----------------------------------------------------------------------------/
/ When a change of the logic level occurs on Input Capture pin (ICP3)
/ and this edge conforms to Input Capture Edge Select (TCCR3B[ICES3]),
/ the 16bit value of the counter (TCNT3) is written to Input Capture Register (ICR3).
/ If Input Capture interrupt is enabled (ICIE3=1), ISR_CAPTURE() runs.
/ In ISR_CAPTURE(), we save interval of ICR3 since last edge
/ and toggle ICES3 to capture next edge
/ until the trailer (idle time without any edges) is detected.
/ see p119 of datasheet
/-----------------------------------------------------------------------------/
/ How this works (TX)
/-----------------------------------------------------------------------------/
/ We use Timer/Counter1's Fast PWM mode to generate IR carrier frequency(38kHz)
/ on Output Compare pin (OC1B).
/ 38kHz is determined by dividing our clock(16MHz) by 8 to make 2MHz, and counting 52 cycles.
/ Output Compare Register (OCR1A) is set to 52.
/ Provide an array of "Number of 2MHz cycles" in IrCtrl.buff .
/ Each entry of the array is set into Timer/Counter3's Output Compare Register (OCR3A),
/ which triggers an interrupt and run ISR_COMPARE() when compare matches.
/ In ISR_COMPARE(), we toggle ON/OFF of IR transmission (write 0/1 into TCCR1A[COM1B1])
/ and continue til the end of IrCtrl.buff .
/----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------/
/ Platform dependent definitions
/----------------------------------------------------------------------------*/
// Define interrupt service functions

// Rx: Timer input capture ISR
#define ISR_CAPTURE()   ISR(TIMER3_CAPT_vect)

// Timer compare match ISR
#define ISR_COMPARE()   ISR(TIMER3_COMPA_vect)

// --------------- RECV ---------------

// Initialize Timer3 for transmission/reception timing: Free running, clk/8
// TCCR3A : Timer/Counter3 Control Register A
//   0b00000000 = Normal port operation (don't output OC3[ABC] to any I/O pin)
// TCCR3B : Timer/Counter3 Control Register B
//   0bxxxxx001 = clk/   1
//   0bxxxxx010 = clk/   8 * 16MHz / 8 = 2MHz
//   0bxxxxx011 = clk/  64
//   0bxxxxx100 = clk/ 256
//   0bxxxxx110 = clk/1024
//   0b1xxxxxxx = Input Capture Noise Canceler: Active
//                Setting this bit (to one) activates the Input Capture Noise Canceler.
//                When the noise canceler is activated, the input from the Input Capture pin (ICP1)
//                is filtered. The filter function requires four successive equal valued samples of
//                the ICP1 pin for changing its output. The Input Capture is therefore delayed by
//                four Oscillator cycles when the noise canceler is enabled.
//   0bx0xxxxxx = Input Capture Edge Select
//                Toggle to detect rising/falling edge of ICP1 pin.
#define IR_INIT_TIMER() \
  TCCR3A = 0b00000000;  \
  TCCR3B = 0b10000010

// Check which edge generated the capture interrupt
// TCCR3B : Timer/Counter3 Control Register B
// ICES3 : Input Capture Edge Select
//   0 : falling (negative) edge is used as trigger
//   1 : rising (positive)  edge will trigger capture
// TIFR3 : Timer/Counter3 Interrupt Flag Register
// ICF3 : Timer/Counter3, Input Capture Flag
//   ICF3 can be cleared by writing a logic one to its bit location
// TOV3 : Timer/Counter3, Overflow Flag # TODO
// TIMSK3 : Timer/Counter3 Interrupt Mask Register
// ICIE3 : Timer/Counter3, Input Capture Interrupt Enable
//   When this bit is written to one,
//   and the I-flag in the Status Register is set (interrupts globally enabled),
//   the Timer/Counter3 Input Capture interrupt is enabled.
//   The corresponding Interrupt Vector (see “Interrupts” on page 57) is executed when the ICF3 Flag,
//   located in TIFR3, is set.
#define IR_CAPTURED_RISING() TCCR3B &   _BV(ICES3)
#define IR_CAPTURE_RISE()    TCCR3B |=  _BV(ICES3)
#define IR_CAPTURE_FALL()    TCCR3B &= ~_BV(ICES3)
#define IR_CAPTURE_ENABLE()  TIFR3  =   _BV(ICF3); TIMSK3 = _BV(ICIE3)
#define IR_CAPTURE_DISABLE() TIMSK3 &= ~_BV(ICIE3)   // Tx && Rx: Disable captureing interrupt

// ICR3 : Input Capture Register
#define IR_CAPTURE_REG()     ICR3

// --------------- XMIT ---------------

// Initialize Timer1 for IR subcarrier: Fast PWM, clk/8
// Countup from BOTTOM to TOP(=OCR1A), At BOTTOM set 1, At OCR1B set 0.
// "In fast PWM mode the counter is incremented until the counter value matches either one of the fixed values 0x00FF, 0x01FF, or 0x03FF (WGMn3:0 = 5, 6, or 7), the value in ICRn (WGMn3:0 = 14), or the value in OCRnA (WGMn3:0 = 15)." - we want the last behavior
// TCCR1A : Timer/Counter Control Register A
// TCCR1B : Timer/Counter Control Register B
//   0b010 : clk(T2S) / 8
// WGM10, WGM11, WGM12, WGM13 all 1 :
//   Timer/Counter Mode of Operation : Fast PWM - p148
//   TOP                             : OCRA
//   Update of OCRnX at              : TOP
//   TOV Flag Set on                 : TOP
// OCR1B : Timer/Counter1 Output Compare Register B
//   16 / 52 (OCR1A:38kHz) = 31% (PWM Duty)
#define IR_INIT_XMIT() \
  OCR1B  = 16; \
  TCCR1A = _BV(WGM11)|_BV(WGM10); \
  TCCR1B = _BV(WGM13)|_BV(WGM12)|0b010

// [atmega32u4 16MHz]
//   16MHz/8  = 2.00MHz
//   OCR1A=53 : 2.00MHz/(53+1) =  37.03KHz
//   OCR1A=52 : 2.00MHz/(52+1) =  37.73KHz * 38K
//   OCR1A=51 : 2.00MHz/(51+1) =  38.46KHz
//   OCR1A=50 : 2.00MHz/(50+1) =  39.21KHz
//   OCR1A=49 : 2.00MHz/(49+1) =  40.00KHz * 40K
#define IR_TX_38K()     OCR1A = 52; TCNT1 = 0   // Tx: Set IR burst frequency to 38kHz
#define IR_TX_40K()     OCR1A = 49; TCNT1 = 0   // Tx: Set IR burst frequency to 40kHz
#define IR_TX_ON()      TCCR1A |=  _BV(COM1B1)  // Tx: Start IR burst
#define IR_TX_OFF()     TCCR1A &= ~_BV(COM1B1)  // Tx: Stop IR burst
#define IR_TX_IS_ON()   TCCR1A &   _BV(COM1B1)  // Tx: Check if IR is being transmitted or not

// Enable compare interrupt n count after now
// OCR3A : Output Compare Register 3A
// TCNT3 : Timer/Counter3 direct access, both for read and for write operations,
//         to the Timer/Counter unit 16-bit counter.
// TIFR3 : Timer/Counter3 Interrupt Flag Register
// OCF3A : Timer/Counter3, Output Compare A Match Flag
// TIMSK3 : Timer/Counter3 Interrupt Mask Register
// OCIE3A : Timer/Counter3, Output Compare A Match Interrupt Enable
#define IR_COMPARE_ENABLE(n) \
  OCR3A   = TCNT3 + (n); \
  TIFR3   = _BV(OCF3A);  \
  TIMSK3 |= _BV(OCIE3A)

#define IR_COMPARE_DISABLE() TIMSK3 &= ~_BV(OCIE3A)  // Disable compare interrupt
#define IR_COMPARE_NEXT(n)   OCR3A += (n)            // Tx: Increase compare register by n count

// Counter clock rate and register width
// [Arduino 16MHz]
//   16MHz/8 => 1000000000ns/(16000000/8) = 500ns
#define T_CLK           500                    // Timer tick period [ns]
#define _timer_reg_t    uint16_t               // Integer type of timer register
/*---------------------------------------------------------------------------*/

// 65535 x 500[ns/tick] = 32_767_500[ns] = 32.8[ms]
// is too short,
// we're gonna wait for 4cycles of silence
// 4cycles = 131[ms] > 110[ms] (NEC protocol frame length)
#define T_TRAIL       65535
#define T_TRAIL_COUNT 4

// clear overflowed state after XXX ms
#define OVERFLOW_CLEAR_TIMEOUT 1000 // [ms]

#define RECV_TIMEOUT           1000
#define XMIT_TIMEOUT           1000

// Working area for IR communication
volatile IR_STRUCT IrCtrl;

// IR receiving interrupt on either edge of input
ISR_CAPTURE()
{
    static _timer_reg_t last_interrupt;

    _timer_reg_t counter = IR_CAPTURE_REG();

    unsigned int now = millis();

    if ((IrCtrl.overflowed > 0) &&
        ((now < IrCtrl.overflowed + OVERFLOW_CLEAR_TIMEOUT) || (now < IrCtrl.overflowed))) {
        // continue overflowed state if receiving IR data continuously
        IrCtrl.overflowed = now;
        return;
    }

    IrCtrl.overflowed = 0;
    if (IrCtrl.state == IR_RECVED_IDLE) {
        IR_state( IR_IDLE );
    }
    if ((IrCtrl.state != IR_IDLE) && (IrCtrl.state != IR_RECVING)) {
        return;
    }
    if (IrCtrl.len >= IR_BUFF_SIZE) {
        // receive buffer overflow
        // data in buffer might be valid (later data might just be "repeat" data)
        // so let's successfully finish receiving
        IR_state( IR_RECVED );
        IrCtrl.overflowed = millis();
        return;
    }

    if (IR_CAPTURED_RISING()) {

        // Rising edge: on stop of burst

        if (IrCtrl.state == IR_IDLE) {
            // can't happen
            return;
        }

        _timer_reg_t low_width = counter - last_interrupt;
        last_interrupt         = counter;

        IrCtrl.buff[ IrCtrl.len ++ ] = low_width;
        IrCtrl.trailerCount = T_TRAIL_COUNT;

        IR_CAPTURE_FALL();
        IR_COMPARE_ENABLE(T_TRAIL); // Enable trailer timer
        return;
    }

    // Falling edge: on start of burst

    _timer_reg_t high_width = counter - last_interrupt;
    last_interrupt          = counter;

    if (IrCtrl.state == IR_IDLE) {
        IR_state( IR_RECVING );
    }
    else { // is IR_RECVING
        uint8_t trailer;
        for (trailer=T_TRAIL_COUNT; trailer>IrCtrl.trailerCount; trailer--) {
            IrCtrl.buff[ IrCtrl.len ++ ] = 65535; // high
            IrCtrl.buff[ IrCtrl.len ++ ] = 0;     // low
            if (IrCtrl.len >= IR_BUFF_SIZE) {
                IR_state( IR_RECVED );
                IrCtrl.overflowed = millis();
                return;
            }
        }
        IrCtrl.buff[ IrCtrl.len ++ ] = high_width;
    }

    IR_CAPTURE_RISE();
    IR_COMPARE_DISABLE(); // Disable trailer timer
}

// Transmission timing and Trailer detection
ISR_COMPARE()
{
    if (IrCtrl.state == IR_XMITTING) {
        if ((IrCtrl.txIndex >= IrCtrl.len) ||
            (IrCtrl.txIndex >= IR_BUFF_SIZE)) {
            // tx successfully finished
            IR_state( IR_IDLE );
            return;
        }
        uint16_t next = IrCtrl.buff[ IrCtrl.txIndex ++ ];
        if (IR_TX_IS_ON()) {
            // toggle
            IR_TX_OFF();
        }
        else {
            if ( next != 0 ) {
                // toggle
                IR_TX_ON();
            }
            else {
                // continue for another uin16_t loop
                next = IrCtrl.buff[ IrCtrl.txIndex ++ ];
                IR_TX_OFF();
            }
        }

        IR_COMPARE_NEXT( next );
        return;
    }
    else if (IrCtrl.state == IR_RECVING) {
        IrCtrl.trailerCount --;
        if (IrCtrl.trailerCount == 0) {
            // Trailer detected
            IR_state( IR_RECVED );
        }
        else {
            // wait for next compare interrupt
        }
        return;
    }

    IR_state( IR_IDLE );
}

int IR_xmit ()
{
    // TODO errcode
    if ((IrCtrl.len == 0) ||
        (IrCtrl.len > IR_BUFF_SIZE)) {
        return 0;
    }
    // if ( IrCtrl.state != IR_WRITING ) {
    //     return 0; // Abort when collision detected
    // }

    IR_state( IR_XMITTING );
    if (IrCtrl.freq == 40) {
        IR_TX_40K();
    }
    else {
        IR_TX_38K();
    }
    IR_TX_ON();
    IR_COMPARE_ENABLE( IrCtrl.buff[ IrCtrl.txIndex ++ ] );

    return 1;
}

uint8_t IRDidRecvTimeout ()
{
    return (IrCtrl.state == IR_RECVING) &&  (millis() - IrCtrl.recvStart > RECV_TIMEOUT);
}

uint8_t IRDidXmitTimeout ()
{
    return (IrCtrl.state == IR_XMITTING) && (millis() - IrCtrl.xmitStart > XMIT_TIMEOUT);
}

void IR_clear (void)
{
    uint16_t i;
    IrCtrl.len        = 0;
    IrCtrl.txIndex    = 0;
    IrCtrl.freq       = IR_DEFAULT_CARRIER; // reset to 38kHz every time
    IrCtrl.overflowed = 0;
    for (i=0; i<IR_BUFF_SIZE; i++) {
        IrCtrl.buff[i] = 0;
    }
}

void IR_state (uint8_t nextState)
{
    switch (nextState) {
    case IR_IDLE:
        IR_TX_OFF();
        IR_COMPARE_DISABLE();

        // 1st interrupt when receiving ir must be falling edge
        IR_CAPTURE_FALL();
        IR_CAPTURE_ENABLE();

        IR_clear();
        break;
    case IR_RECVING:
        IR_clear();

        IrCtrl.recvStart = millis();
        break;
    case IR_RECVED:
        IR_CAPTURE_DISABLE();
        IR_COMPARE_DISABLE();
        break;
    case IR_RECVED_IDLE:
        IR_CAPTURE_FALL();
        IR_CAPTURE_ENABLE();
        break;
    case IR_READING:
        IR_CAPTURE_DISABLE();
        IR_COMPARE_DISABLE();
        break;
    case IR_WRITING:
        IR_CAPTURE_DISABLE();
        IR_COMPARE_DISABLE();
        IR_clear();
        break;
    case IR_XMITTING:
        IR_CAPTURE_DISABLE();
        IR_COMPARE_DISABLE();
        IrCtrl.txIndex = 0;
        IrCtrl.xmitStart = millis();
        break;
    }
    IrCtrl.state = nextState;
}

void IR_initialize (void)
{
    IR_INIT_TIMER();
    IR_INIT_XMIT();

    IR_state( IR_IDLE );
}

void IR_dump (void)
{
    Serial.print(P("IR .state: "));
    switch (IrCtrl.state) {
    case IR_IDLE:
        Serial.println(P("IDLE"));
        break;
    case IR_RECVING:
        Serial.println(P("RECVING"));
        break;
    case IR_RECVED:
        Serial.println(P("RECVED"));
        break;
    case IR_RECVED_IDLE:
        Serial.println(P("RECVED_IDLE"));
        break;
    case IR_READING:
        Serial.println(P("READING"));
        break;
    case IR_WRITING:
        Serial.println(P("WRITING"));
        break;
    case IR_XMITTING:
        Serial.println(P("XMITTING"));
        break;
    default:
        Serial.println(P("!!! UNEXPECTED !!!"));
        break;
    }
    Serial.print(P(" .len: "));          Serial.println(IrCtrl.len,HEX);
    Serial.print(P(" .trailerCount: ")); Serial.println(IrCtrl.trailerCount,HEX);
    Serial.print(P(" .overflowed: "));   Serial.println(IrCtrl.overflowed);
    Serial.print(P(" .txIndex: "));   Serial.println(IrCtrl.txIndex,HEX);
    Serial.print(P(" .xmitStart: "));   Serial.println(IrCtrl.xmitStart);
    for (uint16_t i=0; i<IrCtrl.len; i++) {
        if (IrCtrl.buff[i] < 0x1000) { Serial.write('0'); }
        if (IrCtrl.buff[i] < 0x0100) { Serial.write('0'); }
        if (IrCtrl.buff[i] < 0x0010) { Serial.write('0'); }
        Serial.print(IrCtrl.buff[i], HEX);
        Serial.print(P(" "));
        if (i % 16 == 15) { Serial.println(); }
    }
    Serial.println();
}