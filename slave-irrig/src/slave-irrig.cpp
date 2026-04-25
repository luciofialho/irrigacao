#include <IOTK.h>
#include <IOTK_ESP32.h>
#include <IOTK_Dallas.h>
#include <IOTK_CurrentMonitor.h>
#include <IOTK_NTP.h>
#include <IOTK_UDP.h>
#include <IOTK_WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define PIN_BOMBA 32
#define NUMLOCALSECTORS 8
#define FIRSTLOCALSECTOR 5 // aparentemente subtrai 1
                                       //  6    7     8     9     10   11   12    
byte PIN_SECTORS[NUMLOCALSECTORS]    = {  17,   18,   19,   21,   22,   23,  25}; // slave proximo é 26
byte OPEN_SECTORS[NUMLOCALSECTORS]   = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
byte CLOSED_SECTORS[NUMLOCALSECTORS] = { LOW,  LOW,  LOW,  LOW,  LOW,  LOW, LOW};






int activeSector;
bool slavePump;

#define SLAVE
#include "../../Irrigacao_common.h"

Adafruit_SSD1306 oled(128, 64, &Wire, -1);

String getStatus() {
  String status;
  status = "<br>uptime: " + formatedUptime() + "<p><p>";

  if (quandoDesligarBomba!=0)
    status += "Bomba ligada por mais " + String ((quandoDesligarBomba-millis())/1000) + " seg";

  else
    status += "Bomba desligada";
  status += "<p>";

  if (activeSector != 0)
    status += "setor " + String(activeSector) + " ativo por mais " +String(setorAtualResta/1000)+ " seg";
  else
    status += "nenhum setor ativo";

  status += "</br>";
  
  return status;
}

void oledStatus() {
  static unsigned long last = 0;

  if (MILLISDIFF(last,200)) {
    last = millis();

    oled.clearDisplay();

    oled.setCursor(0,0);
    oled.setTextSize(1);
    oled.print("Bomba: ");
    oled.setTextColor(WHITE);
    oled.setTextSize(2);
    if (quandoDesligarBomba != 0) 
      oled.println((quandoDesligarBomba-millis())/1000);
    else
      oled.println("deslig");
    oled.setTextSize(1);
    oled.print("Setor: ");
    oled.setTextSize(2);
    if (activeSector != 0) {
      oled.print(activeSector);
      oled.print(" ");
      oled.println(setorAtualResta/1000);
    }
    else
      oled.println("nenhum");

    oled.setTextSize(1);
    oled.println();
    if (wifiIsConnected()) {
      oled.print("signal: "); oled.print(WiFi.RSSI()); oled.print(" ("); oled.print(WiFi.localIP()[3]); oled.println(")");
    }
    else
      oled.print("conectando...");
    oled.display();               // show on OLED  
  }
}

float slavecmd(char *cmd) {
Serial.print("Comando: "); Serial.println(cmd); //*************
  char buf[10];
  byte buflen=0;
  byte fld=0;
  byte pos=0;
  bool ok = true;
  long pumpTime = 0;
  long pumpDelay = 0;
  int firstSectorDelay = 0;
  byte cmdSetor[MAXSETORESDEVICE];

  memset(cmdSetor,0,MAXSETORESDEVICE);
  while (1) {
    if(cmd[pos]=='~' || cmd[pos]==0) {
      buf[buflen] = 0;
      switch (fld) {
        case 0: pumpTime          = atol(buf); break;
        case 1: pumpDelay         = atol(buf); break;
        case 2: firstSectorDelay  = atoi(buf); break;
        default:
          cmdSetor[fld-3] = atoi(buf);
          break;
      }
      if (cmd[pos]) {
        pos++;
        fld++;
        buflen = 0;
        if (fld>3+NUMLOCALSECTORS) {
          Serial.print("fld = ");Serial.println(fld);
          ok = false;
          break;
        }
      }
      else
        break;
    }
    else if (cmd[pos]>='0' && cmd[pos]<='9')
      buf[buflen++] = cmd[pos++];
    else {
      ok = false;
      break;
    }
  }

  if (fld != NUMSETORESSLAVE+2) { // +2 e não +3, pois, no último, não incrementa
    Serial.print("fld=");Serial.println(fld);
    ok = false;
  }

  if (ok) {
    if (pumpTime>0)
      ativarBomba(pumpTime*30L*1000L,pumpDelay*30L*1000L);
    else
      desativarBomba();
    setores(firstSectorDelay,cmdSetor);
    Serial.println("slavecmd ok");
    //*********************
Serial.println(pumpTime);
Serial.println(pumpDelay);
Serial.println(firstSectorDelay);    
    return 0;
  }
  else {
    Serial.println("slavecmd fail");
    return 1;
  }
}

void setup() {
  pinMode(PIN_BOMBA,OUTPUT);     digitalWrite(PIN_BOMBA, OFF);
  for (byte i=0; i<NUMLOCALSECTORS; i++) {
    pinMode(PIN_SECTORS[i], OUTPUT); 
    digitalWrite(PIN_SECTORS[i],CLOSED_SECTORS[i]);
  }

  Serial.begin(115200);
  delay(10);
  Serial.println("Iniciando irrigação - slave");
 // ESP8266_AppName="Irrigation - slave";  

  setStatusSource(getStatus);
  selectWifiAndInit(ssids,passwords);
  UDPTalk.begin();
  UDPTalk.onPacket("IRG_CMD",slavecmd);
  




  delay(10);  
  
  resetTemposSetores();

  Wire.begin(5,4);

  // initialize OLED display with address 0x3C for 128x64
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println(F("SSD1306 allocation failed"));
  else 
    Serial.println("SSD1306 allocation success");  
  
  oled.clearDisplay();

  oled.setTextSize(2);
  oled.setTextColor(WHITE);
  oled.setCursor(0, 10);
  oled.println("IRRIGACAO");
  oled.println("SLAVE");
  oled.display();               

  setBuiltinLedPin(33);
}

void loop() {
  handle_IOTK();
  UDPTalk.handle();
  server.handleClient();
  oledStatus();

  static unsigned long int lastStatusSend = 0;
  static bool lastStatusWasActive = false;
  unsigned statusInterval;
  if (quandoDesligarBomba != 0 || activeSector !=0 || lastStatusWasActive) {
    lastStatusWasActive = quandoDesligarBomba != 0 || activeSector !=0;
    statusInterval = 2000L;
  }
  else
    statusInterval = 2000L;

  if (MILLISDIFF(lastStatusSend,statusInterval)) {
    lastStatusSend = millis();
    char dig[2]="0";
    dig[0] = '0'+activeSector;
    String resp = String(dig) + String(int(quandoDesligarBomba != 0)) + getStatus();
    UDPTalk.send("IRG_SLAVE_STATUS",resp.c_str());
  }

  processaBomba();
  processaSetores();

            
  //--------------------------------------------- Controle led
  ledAsWifiStatus();
}
