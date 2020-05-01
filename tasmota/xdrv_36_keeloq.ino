/*
  xdrv_36_keeloq.ino - Jarolift Keeloq shutter support for Tasmota

  Copyright (C) 2020  he-so

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_KEELOQ
/*********************************************************************************************\
 * Keeloq shutter support
 *
 * Uses hardware 
 * either :SPI and two user configurable GPIO's (CC1101 GDO0 and CC1101 GDO2)
 * * Considering the implementation these two user GPIO's are fake.
 * Only CC1101 GDO0 is used and must always be GPIO05 dictated by the used CC1101 library.
 * 
 * or send vie sonoff RF bridge via serial port
 * 
 *
 *
\*********************************************************************************************/

#define XDRV_36 36


#include <KeeloqLib.h>

// keeloq output device CC1101 or Sonoff RF_bridge
#define CC1101 0
#define RFBRIDGE 1
#define KeeLoqSend  RFBRIDGE

#if KeeLoqSend == CC1101
  #include "cc1101.h"
#endif

#define SYNC_WORD 199

#define Lowpulse         400
#define Highpulse        800
#define RFRawLen         0x61

void sendKeeloqPacket();

const char kJaroliftCommands[] PROGMEM = D_PRFX_KEELOQ "|" // keeloq prefix
 D_CMND_KEELOQ_SET_KEYS "|" D_CMND_KEELOQ_SET_REMOTE "|" D_CMND_KEELOQ_SENDBUTTON  "|"  D_CMND_KEELOQ_SENDRAW ;

void (* const jaroliftCommand[])(void) PROGMEM = {&CmdSetKeys, &CmdSetRemote, &CmdSendButton,&CmdSendRaw};

#if KeeLoqSend == CC1101
CC1101 cc1101;
#endif

struct JAROLIFT_DEVICE {
  int device_key_msb       = 0x0; // stores cryptkey MSB
  int device_key_lsb       = 0x0; // stores cryptkey LSB
  uint8_t button          = 0x0; // 1000=0x8 up, 0100=0x4 stop, 0010=0x2 down, 0001=0x1 learning
  uint8_t group            =0x0; //8 bits for grouping
  int disc                 = 0x0100; // 0x0100 for single channel remote
  uint32_t enc             = 0x0;   // stores the 32Bit encrypted code
  uint64_t pack            = 0;   // Contains data to send.
  int count                = 0;
  uint32_t serial          = 0x0;
  uint8_t port_tx;
  uint8_t port_rx;
} jaroliftDevice;

void CmdSetKeys(void)
{
  if (XdrvMailbox.data_len > 0) {
      char *p;
      uint32_t i = 0;
      uint32_t param[2] = { 0 };
      DEBUG_DRIVER_LOG( PSTR("KeeloqSetKeys->params: %s,len:%d"), XdrvMailbox.data,XdrvMailbox.data_len);
      for (char *str = strtok_r(XdrvMailbox.data, ", ", &p); str && i < 2; str = strtok_r(nullptr, ", ", &p)) {
        param[i] = strtoul(str, nullptr, 0);
        i++;
      }
      for (uint32_t i = 0; i < 2; i++) {
        if (param[i] < 0) { 
          DEBUG_DRIVER_LOG( PSTR("invalid keys"));
          return;
         }  
      }
      DEBUG_DRIVER_LOG( PSTR("KeeloqSetKeys:params: msb:%08x lsb:%08x"), param[0], param[1]);
      Settings.keeloq_master_msb = param[0];
      Settings.keeloq_master_lsb = param[1];
      
      ResponseCmndDone();
  } else {
    DEBUG_DRIVER_LOG( PSTR("KeeloqSetKeys: no param"));
  }
}

void CmdSetRemote(void)
{
  if (XdrvMailbox.data_len > 0) {
      char *p;
      uint32_t i = 0;
      uint32_t param[3] = { 0 };
      DEBUG_DRIVER_LOG( PSTR("KeeloqSetremote->params: %s,len:%d"), XdrvMailbox.data,XdrvMailbox.data_len);
      for (char *str = strtok_r(XdrvMailbox.data, ", ", &p); str && i < 3; str = strtok_r(nullptr, ", ", &p)) {
        param[i] = strtoul(str, nullptr, 0);
        i++;
      }
      
      DEBUG_DRIVER_LOG( PSTR("KeeloqSetremote->params:serial:%08x count:%08x slotNb:%08x"), param[0], param[1], param[2]);
      if (param[2]<1 || param[2] > MAX_KEELOQ){
        DEBUG_DRIVER_LOG( PSTR("KeeloqSetremote->slot nb out of range"));
        return;
      }

      Settings.keeloq_serial[param[2]-1] = param[0];  // commands key nb start at 1: param[0]-> serial nb
      Settings.keeloq_count[param[2]-1] = param[1];   // param [1]-->counter
      
      ResponseCmndDone();
  } else {
    DEBUG_DRIVER_LOG( PSTR("no param"));
  }
}

void GenerateDeviceCryptKey()
{
  Keeloq k(Settings.keeloq_master_msb, Settings.keeloq_master_lsb);
  jaroliftDevice.device_key_msb = k.decrypt(jaroliftDevice.serial | 0x60000000L);
  jaroliftDevice.device_key_lsb = k.decrypt(jaroliftDevice.serial | 0x20000000L);

  AddLog_P2(LOG_LEVEL_DEBUG, PSTR("generated device keys: %08x %08x"), jaroliftDevice.device_key_msb, jaroliftDevice.device_key_lsb);
}

void CmdSendButton(void)
{
  

  if (XdrvMailbox.data_len > 0)
  {
    if (XdrvMailbox.payload > 0)
    {
      char *p;
      uint32_t i = 0;
      uint32_t param[2] = { 0 };
      uint32_t remoteIndex;

      for (char *str = strtok_r(XdrvMailbox.data, ", ", &p); str && i < 2; str = strtok_r(nullptr, ", ", &p)) {
        param[i] = strtoul(str, nullptr, 0);
        i++;
      }
      if (param[0] <0 || param[0] >15){
        DEBUG_DRIVER_LOG( PSTR("button nb out of range"));
        return;
      }
      if(param[1]> MAX_KEELOQ){  DEBUG_DRIVER_LOG( PSTR("index out of range")); }
      if(param[1]< 0){  param[1]=1; }
      DEBUG_DRIVER_LOG( PSTR("KeeloqSendButton->params:button:%08x slotNb:%08x"), param[0], param[1]);

      jaroliftDevice.button = param[0]; 
      remoteIndex=param[1]-1 ;// slot nb  start at 1
      KeeloqSetDevice(remoteIndex); 
    
      DEBUG_DRIVER_LOG( PSTR("msb: 0X%X"), jaroliftDevice.device_key_msb);
      DEBUG_DRIVER_LOG( PSTR("lsb: 0X%X"), jaroliftDevice.device_key_lsb);
      DEBUG_DRIVER_LOG( PSTR("serial: 0X%X"), jaroliftDevice.serial);
      DEBUG_DRIVER_LOG( PSTR("disc: 0X%X"), jaroliftDevice.disc);
      DEBUG_DRIVER_LOG( PSTR("button: 0X%X"), jaroliftDevice.button);
    
      AddLog_P2(LOG_LEVEL_DEBUG, PSTR("KLQ: count: %08x-->%d"), jaroliftDevice.count,jaroliftDevice.count);

      CreateKeeloqPacket();
      jaroliftDevice.count++;
      Settings.keeloq_count[remoteIndex] = jaroliftDevice.count;
      SendKeeloqPacket();
      
    }
  }

  ResponseCmndDone();
}

void CmdSendRaw(void)
  {
    DEBUG_DRIVER_LOG( PSTR("cmd send called at %d"), micros());
    #if KeeLoqSend == CC1101
      sendRawCC1101();
    #elif KeeLoqSend == RFBRIDGE
      SerialSendRaw(RemoveSpace(XdrvMailbox.data));
      SnfBridge.receive_raw_flag = 1;
    #endif
    ResponseCmndDone();
  }
 


#if KeeLoqSend == RFBRIDGE // specifc functions for RF_bridge output
  void createRfRawB0Packet(char * dataBuf){
    // write preamble and description of the 5 bukets: 
    //1150us-->0x047E high before sync sequence
    //400us->0x0190 short bit
    //800us->0x0320 long bit
    //16000us->0x3E80 low after bit sequence
    // seq len is fixed: (Length of filled databuf without spaces)/2-4  ->0x61
    strcat(dataBuf,"AA B0 61 05 06 0690 017C 0FB4 0348 3DD6");
    // add sync sequence
    strcat(dataBuf,"481919191919191919191919192");
    // add the 72 bits of  message: 8 bits from the group and 64 bits from the pack
    uint64_t bits1to64 = jaroliftDevice.pack;
    uint8_t  bits65to72 = jaroliftDevice.group;
    // first send the packed message with button nb, serial and cipered part
    for(int i=0; i<64; i++)
    {
      strcat(dataBuf, ((bits1to64 >> i) & 0x0000000000000001)?"93":"B1");
    }
    // then send the group bits (bits 8-16)
    for(int i=0; i<8; i++)
    {
      strcat(dataBuf, ((bits65to72 >> i) & 0x1)?"93":"B1");
    }
    
    // send  post data Bucket and 55 to end B0 sequence
    strcat(dataBuf, "4 55");
    DEBUG_DRIVER_LOG(PSTR("computed RFRAw B0 data string %s"), dataBuf);
  }

#elif KeeLoqSend == CC1101
  void SendBit(byte bitToSend)
  {
    if (bitToSend==1)
    {
      digitalWrite(jaroliftDevice.port_tx, LOW);  // Simple encoding of bit state 1
      delayMicroseconds(Lowpulse);
      digitalWrite(jaroliftDevice.port_tx, HIGH);
      delayMicroseconds(Highpulse);
    }
    else
    {
      digitalWrite(jaroliftDevice.port_tx, LOW);  // Simple encoding of bit state 0
      delayMicroseconds(Highpulse);
      digitalWrite(jaroliftDevice.port_tx, HIGH);
      delayMicroseconds(Lowpulse);
    }
  }

  void enterrx() {
    unsigned char marcState = 0;
    cc1101.setRxState();
    delay(2);
    unsigned long rx_time = micros();
    while (((marcState = cc1101.readStatusReg(CC1101_MARCSTATE)) & 0x1F) != 0x0D )
    {
      if (micros() - rx_time > 50000) break; // Quit when marcState does not change...
    }
  }

  void entertx() {
    unsigned char marcState = 0;
    cc1101.setTxState();
    delay(2);
    unsigned long rx_time = micros();
    while (((marcState = cc1101.readStatusReg(CC1101_MARCSTATE)) & 0x1F) != 0x13 && 0x14 && 0x15)
    {
      if (micros() - rx_time > 50000) break; // Quit when marcState does not change...
    }
  }

  void sendRawCC1101()
  {
    noInterrupts();
    entertx();
    for(int repeat = 0; repeat <= 1; repeat++)
    {
      if (XdrvMailbox.data_len > 0)
      {
        digitalWrite(jaroliftDevice.port_tx, LOW);
        delayMicroseconds(1150);
        SendSyncPreamble(13);
        delayMicroseconds(3500);

        for(int i=XdrvMailbox.data_len-1; i>=0; i--)
        {
          SendBit(XdrvMailbox.data[i] == '1');
        }
        DEBUG_DRIVER_LOG( PSTR("finished sending bits at %d"), micros());

        delay(16);                       // delay in loop context is save for wdt
      }
      interrupts();
    }
    enterrx();
  }
  void SendSyncPreamble(int l)
  {
    for (int i = 0; i < l; ++i)
    {
      digitalWrite(jaroliftDevice.port_tx, LOW);
      delayMicroseconds(400);
      digitalWrite(jaroliftDevice.port_tx, HIGH);
      delayMicroseconds(380);
    }
  }
#endif

void SendKeeloqPacket()
{
#if KeeLoqSend == RFBRIDGE
  // send command using RF bridge send raw command
  DEBUG_DRIVER_LOG(PSTR("sending RF command"));
  static char topicBuf[] = "RfRaw";
  static char dataBuf[256];
  dataBuf[0]=0;
  createRfRawB0Packet(dataBuf);
  uint32 data_len=strlen(dataBuf);
  //AddLog_P2(LOG_LEVEL_DEBUG, PSTR("CmndRfRaw: sending raw %s"), dataBuf);
  SerialSendRaw(RemoveSpace(dataBuf));
  SnfBridge.receive_raw_flag = 1;

#elif KeeLoqSend == CC1101
  // send command via CC1101
  noInterrupts();
  entertx();

  for(int repeat = 0; repeat <= 1; repeat++)
  {
    uint64_t bitsToSend = jaroliftDevice.pack;
    digitalWrite(jaroliftDevice.port_tx, LOW);
    delayMicroseconds(1150);
    SendSyncPreamble(13);
    delayMicroseconds(3500);
    for(int i=72; i>0; i--)
    {
      SendBit(bitsToSend & 0x0000000000000001);
      bitsToSend >>= 1;
    }
    DEBUG_DRIVER_LOG(PSTR("finished sending bits at %d"), micros());

    delay(16); // delay in loop context is save for wdt
  }
  interrupts();
  enterrx();
#endif
}

void CreateKeeloqPacket()
{
  Keeloq k(jaroliftDevice.device_key_msb, jaroliftDevice.device_key_lsb);
  unsigned int result = (jaroliftDevice.disc << 16) | jaroliftDevice.count;
  jaroliftDevice.pack = (uint64_t)0;
	jaroliftDevice.pack |= jaroliftDevice.serial & 0xfffffffL;
	jaroliftDevice.pack |= (jaroliftDevice.button & 0xfL) << 28;

  jaroliftDevice.pack <<= 32;
  jaroliftDevice.enc = k.encrypt(result);
  jaroliftDevice.pack |= jaroliftDevice.enc;
  AddLog_P2(LOG_LEVEL_DEBUG_MORE, PSTR("pack high: %08x"), jaroliftDevice.pack>>32);
  AddLog_P2(LOG_LEVEL_DEBUG_MORE, PSTR("pack low:  %08x"), jaroliftDevice.pack);
  AddLog_P2(LOG_LEVEL_DEBUG, PSTR("pack cyphered: 0X%X"), jaroliftDevice.pack &0xffffffffL);
  AddLog_P2(LOG_LEVEL_DEBUG, PSTR("serial nb:  0X%X"), jaroliftDevice.pack>>32 & 0xfffffffL);
  AddLog_P2(LOG_LEVEL_DEBUG, PSTR("button:  0X%X"), jaroliftDevice.pack>>60 & 0xfL);
  
}
void KeeloqSetDevice(uint32_t index){
  jaroliftDevice.serial = Settings.keeloq_serial[index];
  jaroliftDevice.count = Settings.keeloq_count[index];
  GenerateDeviceCryptKey();
}

void KeeloqInit()
{
  jaroliftDevice.port_tx = pin[GPIO_CC1101_GDO2];              // Output port for transmission
  jaroliftDevice.port_rx = pin[GPIO_CC1101_GDO0];              // Input port for reception

  DEBUG_DRIVER_LOG( PSTR("cc1101.init()"));
  delay(100);
  #if KeeLoqSend == CC1101
    cc1101.init();
    AddLog_P(LOG_LEVEL_DEBUG_MORE, PSTR("CC1101 done."));
    cc1101.setSyncWord(SYNC_WORD, false);
    cc1101.setCarrierFreq(CFREQ_433);
    cc1101.disableAddressCheck();
  #endif  

  pinMode(jaroliftDevice.port_tx, OUTPUT);
  pinMode(jaroliftDevice.port_rx, INPUT_PULLUP);
  
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/
bool Xdrv36(uint8_t function)
{
  

  #if KeeLoqSend == CC1101
    if ((99 == pin[GPIO_CC1101_GDO0]) || (99 == pin[GPIO_CC1101_GDO2])) { return false; }
  #endif
  
  bool result = false;

  switch (function) {
    case FUNC_COMMAND:
      AddLog_P2(LOG_LEVEL_DEBUG, PSTR("Xdrv36:calling command"));
      result = DecodeCommand(kJaroliftCommands, jaroliftCommand);
      break;
    case FUNC_INIT:
      KeeloqInit();
      DEBUG_DRIVER_LOG( PSTR("init done."));
      break;
  }
  //AddLog_P2(LOG_LEVEL_DEBUG, PSTR("exit  Xdrv36 ;result=%d"),result);
  return result;
}

#endif // USE_KEELOQ
