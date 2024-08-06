//---------------------------------------------------------------------------
//
//	ADBuino ADB keyboard and mouse adapter
//
//     Copyright 2011 Jun WAKO <wakojun@gmail.com>
//     Copyright 2013 Shay Green <gblargg@gmail.com>
//	   Copyright (C) 2017 bbraun
//	   Copyright (C) 2020 difegue
//	   Copyright (C) 2021-2022 akuker
//
//  This file is part of ADBuino.
//
//  ADBuino is free software: you can redistribute it and/or modify it under 
//  the terms of the GNU General Public License as published by the Free 
//  Software Foundation, either version 3 of the License, or (at your option) 
// any later version.
//
//  ADBuino is distributed in the hope that it will be useful, but WITHOUT ANY 
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
//  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more 
//  details.
//
//  You should have received a copy of the GNU General Public License along 
//  with ADBuino. If not, see <https://www.gnu.org/licenses/>.
//
//  Portions of this code were originally released under a Modified BSD 
//  License. See LICENSE in the root of this repository for more info.
//
//----------------------------------------------------------------------------
#include "Arduino.h"
#include <util/delay.h>
#define kModCmd 1
#define kModOpt 2
#define kModShift 4
#define kModControl 8
#define kModReset 16
#define kModCaps 32
#define kModDelete 64
uint8_t mousepending = 0;
uint8_t kbdpending = 0;
uint8_t kbdskip = 0;
uint16_t kbdprev0 = 0;
uint16_t mousereg0 = 0;
uint16_t kbdreg0 = 0;
uint8_t kbdsrq = 0;
uint8_t mousesrq = 0;
uint8_t modifierkeys = 0xFF;
uint32_t kbskiptimer = 0;
#define ADB_PORT PORTD
#define ADB_PIN PIND
#define ADB_DDR DDRD
#define ADB_DATA_BIT 3

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

enum e_command_code_t : uint8_t
{
  // The SendReset command causes all devices on the network to reset to their
  // power-on states
  SendReset,
  // The action of the Flush command is defined for each device. Normally, it
  // is used to clear any internal registers in the device
  Flush,
  // The Listen command is a request for the device to receive data transmitted
  // from the computer and store it in a specific internal register (0 through 3).
  Listen,
  // The Talk command initiates a data transfer to the computer from a specific
  // register (0 through 3) of an ADB input devic
  Talk,
  // Not used
  Reserved,
};

// The original data_lo code would just set the bit as an output
// That works for a host, since the host is doing the pullup on the ADB line,
// but for a device, it won't reliably pull the line low.  We need to actually
// set it.
#define data_lo()                       \
  {                                     \
    (ADB_DDR |= (1 << ADB_DATA_BIT));   \
    (ADB_PORT &= ~(1 << ADB_DATA_BIT)); \
  }
#define data_hi() (ADB_DDR &= ~(1 << ADB_DATA_BIT))
#define data_in() (ADB_PIN & (1 << ADB_DATA_BIT))
static inline uint16_t wait_data_lo(uint16_t us)
{
  do
  {
    if (!data_in())
      break;
    _delay_us(1 - (6 * 1000000.0 / F_CPU));
  } while (--us);
  return us;
}
static inline uint16_t wait_data_hi(uint16_t us)
{
  do
  {
    if (data_in())
      break;
    _delay_us(1 - (6 * 1000000.0 / F_CPU));
  } while (--us);
  return us;
}

static inline void place_bit0(void)
{
  data_lo();
  _delay_us(65);
  data_hi();
  _delay_us(35);
}
static inline void place_bit1(void)
{
  data_lo();
  _delay_us(35);
  data_hi();
  _delay_us(65);
}
static inline void send_byte(uint8_t data)
{
  for (int i = 0; i < 8; i++)
  {
    if (data & (0x80 >> i))
      place_bit1();
    else
      place_bit0();
  }
}
static uint8_t inline adb_recv_cmd(uint8_t srq)
{
  uint8_t bits;
  uint16_t data = 0;

  // find attention & start bit
  if (!wait_data_lo(5000))
    return 0;
  uint16_t lowtime = wait_data_hi(1000);
  if (!lowtime || lowtime > 500)
  {
    return 0;
  }
  wait_data_lo(100);

  for (bits = 0; bits < 8; bits++)
  {
    uint8_t lo = wait_data_hi(130);
    if (!lo)
    {
      goto out;
    }
    uint8_t hi = wait_data_lo(lo);
    if (!hi)
    {
      goto out;
    }
    hi = lo - hi;
    lo = 130 - lo;

    data <<= 1;
    if (lo < hi)
    {
      data |= 1;
    }
  }

  if (srq)
  {
    data_lo();
    _delay_us(250);
    data_hi();
  }
  else
  {
    // Stop bit normal low time is 70uS + can have an SRQ time of 300uS
    wait_data_hi(400);
  }

  return data;
out:
  return 0;
}

void Handle_Listen()
{
  // When the computer sends a Listen command to a device, the device
  // receives the next data packet from the computer and places it in
  // the appropriate register. After the stop bit following the data is
  // received, the transaction is complete and the computer releases the bus
}
int Receive_ADB_Data(uint8_t *data_buf, int buf_size)
{
  // When the computer sends a Talk command to a device, the device must
  // respond with data within 260 µs. The selected device performs its
  // data transaction and releases the bus, leaving it high
  uint8_t bits;
  uint16_t data = 0;
  int word_count = 0;

  uint8_t x = wait_data_lo(260);
  if(!x){
    return word_count;
  }

  for (int i = 0; i < 8; i++)
  {
    if (word_count > buf_size)
    {
      Serial.println("Warning! Buffer overflow in Receive_ADB_Data. Data may be lost");
      return word_count;
    }

    for (bits = 0; bits < 8; bits++)
    {
      uint8_t lo = wait_data_hi(130);
      if (!lo)
      {
        return word_count;
      }
      uint8_t hi = wait_data_lo(lo);
      if (!hi)
      {
        return word_count;
      }
      hi = lo - hi;
      lo = 130 - lo;

      data <<= 1;
      if (lo < hi)
      {
        data |= 1;
      }
      data_buf[word_count] = data;
    }
    word_count++;
  }
  return word_count;
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)

    // Set ADB line as input
    ADB_DDR &= ~(1 << ADB_DATA_BIT);

  Serial.println("setup complete");
}

void loop()
{
  uint8_t cmd = 0;
  e_command_code_t cmd_code = Reserved;
  uint8_t cmd_register = 0;
  uint8_t cmd_address = 0;
  int blk_size = 0;
  uint8_t data_buffer[8]; // Max size of a ADB transaction is 8 bytes
  memset(data_buffer, 0, sizeof(data_buffer));
  char cmd_str[64];
  // uint16_t us;

  cmd = adb_recv_cmd(mousesrq | kbdsrq);
  if (cmd != 0)
  {
    cmd_register = (cmd & 0x03);
    cmd_address = ((cmd & 0xF0) >> 4);

    switch ((cmd & 0x0C) >> 2)
    {
    case 0x0:
      switch (cmd & 0x01)
      {
      case 0x0:
        cmd_code = SendReset;
        strcpy(cmd_str, "Reset");
        break;
      case 0x1:
        cmd_code = Flush;
        strcpy(cmd_str, "Flush");
      }
      break;
    case 0x2:
      cmd_code = Listen;
      strcpy(cmd_str, "Listen");
      break;
    case 0x3:
      cmd_code=Talk;
      strcpy(cmd_str, "Talk");
      break;
    default:
      strcpy(cmd_str, "Invalid");
    }

    switch (cmd_code)
    {
    case Talk:
    case Listen:
      blk_size = Receive_ADB_Data(data_buffer, ARRAY_SIZE(data_buffer));

      if (blk_size > 0)
      {
        Serial.print("ADB Command:");
        Serial.print(cmd, HEX);
        Serial.print(" (");
        Serial.print(cmd_str);
        Serial.print(") addr:");
        Serial.print(cmd_address, HEX);
        Serial.print(" reg:");
        Serial.print(cmd_register);
        Serial.print(" Data[");
        Serial.print(blk_size);
        Serial.print("]: ");
        for (int i = 0; i < blk_size; i++)
        {
          Serial.print(data_buffer[i], HEX);
        }
              Serial.println("");

      }
      break;
    case SendReset:
    case Flush:
    default:
      break;
    }
    
  }
}