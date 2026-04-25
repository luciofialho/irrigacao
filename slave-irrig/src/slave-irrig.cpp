#include <IOTK.h>
#include <IOTK_ESPAsyncServer.h>
#include <IOTK_Dallas.h>
#include <IOTK_CurrentMonitor.h>
#include <IOTK_NTP.h>
#include <Wire.h>

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

char masterIP[16] = "";
WiFiServer tcpCmdServer(TCP_CMD_PORT);

Adafruit_SSD1306 oled(128, 64, &Wire, -1); // definição local referenciada pelo common

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
  strcpy(ESP_AppName, "Irrigation - slave");

  setStatusSource(getStatus);
  const char* ssid_arr[] = {"secretoca", "goiaba"};
  const char* pass_arr[] = {"Goiaba5090", "heptA2019"};
  ESPSetup(2, ssid_arr, pass_arr);
  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->redirect("/getstatus");
  });
  tcpCmdServer.begin();
  




  delay(10);  
  
  resetTemposSetores();

  Wire.begin(5, 4);
  oledInitDisplay("SLAVE");

  setBuiltinLedPin(33);
}

void loop() {
  handle_IOTK();
  oledHandle();

  // Recebe comandos via TCP
  WiFiClient tcpClient = tcpCmdServer.available();
  if (tcpClient) {
    if (masterIP[0] == '\0') {
      tcpClient.remoteIP().toString().toCharArray(masterIP, sizeof(masterIP));
      Serial.print("Master IP detectado: "); Serial.println(masterIP);
    }
    String line = tcpClient.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      float result = slavecmd((char*)line.c_str());
      tcpClient.println(result == 0 ? "OK" : "FAIL");
    }
    tcpClient.stop();
  }

  static unsigned long int lastStatusSend = 0;
  static bool lastStatusWasActive = false;
  unsigned statusInterval;
  if (quandoDesligarBomba != 0 || activeSector !=0 || lastStatusWasActive) {
    lastStatusWasActive = quandoDesligarBomba != 0 || activeSector !=0;
    statusInterval = 2000L;
  }
  else
    statusInterval = 2000L;

  if (masterIP[0] != '\0' && MILLISDIFF(lastStatusSend,statusInterval)) {
    lastStatusSend = millis();
    char dig[2]="0";
    dig[0] = '0'+activeSector;
    String resp = String(dig) + String(int(quandoDesligarBomba != 0)) + getStatus();
    WiFiClient tcpStatus;
    if (tcpStatus.connect(masterIP, TCP_STATUS_PORT)) {
      unsigned long t = millis();
      while (!tcpStatus.connected() && millis() < t + 1000) yield();
      tcpStatus.println(resp);
      tcpStatus.flush();
      tcpStatus.stop();
    }
  }

  processaBomba();
  processaSetores();

            
  //--------------------------------------------- Controle led
  ledAsWifiStatus();
}
