#include <IOTK.h>
#include <IOTK_ESP8266.h>
#include <IOTK_WiFi.h>
#include <IOTK_Dallas.h>
#include <IOTK_CurrentMonitor.h>
#include <IOTK_UDP.h>

#include <__NumFilters.h>
#include <Adafruit_INA219.h>

//const char* ssid     = "secretoca";
//const char* password = "Goiaba5090";
char ssids[]     = "secretoca~goiaba";
char passwords[] = "Goiaba5090~heptA2019";

#define PIN_BOIA D5

#define TIMEOUT 2000

Adafruit_INA219 ina219;

int  nivel;
bool boia;


IOTKUDP UDPTalk;

averageIntVector avgNivel(180);
int nivelmaMedio;
int nivelmaPontual;
float nivelmaPontualFloat;
#define MINNIVEL 420
#define MAXNIVEL 1790


int nivelPercent() {
  if (nivelmaMedio<MINNIVEL)
    return 0;
  else if (nivelmaMedio>MAXNIVEL)
    return 100;
  else
    return int((nivelmaMedio - MINNIVEL)*100L/(MAXNIVEL-MINNIVEL));
}

void serverHandleLeBoia() {
  server.send (200,"text/plain", boia+"~"+String(nivelPercent())+"~"+String(nivelmaPontual));
}

String getStatus() {
  String st;
  
  st =  "<br>Nivel: " + String(nivel) + "%    -- " + String(nivelmaMedio/100.) + "ma (pontual = " +String(nivelmaPontualFloat) + " ma)";
  if (boia)
    st += "<br>Boia: flutuando";
  else
    st += "<br>Boia: afundada";
     
  return st;
}

void setup() {
  Serial.begin(115200);
    pinMode(PIN_BOIA,INPUT_PULLUP);
  delay(30);
  ESP8266_AppName="Irrigation - tank";

  Serial.println();
  Serial.println("Iniciando controle da irrigacao CAIXA");
  
  ina219.begin();
  ina219.setCalibration_16V_40mA();

  setStatusSource(getStatus);

  selectWifiAndInit(ssids,passwords);

  UDPTalk.begin();  
}

void loop() {
  handle_IOTK();
  //server.handleClient(); - precisa mesmo? Turei e não testei

  static unsigned long ultNivel = 0;
  if (MILLISDIFF(ultNivel,500L)) {
    ultNivel = millis();
    nivelmaPontualFloat = ina219.getCurrent_mA();
    nivelmaPontual = int(nivelmaPontualFloat *100);
    avgNivel.add(nivelmaPontual);
    nivelmaMedio = avgNivel.value();
    nivel = nivelPercent();
    boia = digitalRead(PIN_BOIA) == LOW;
  }

  static unsigned long int ultUDPBroadcast = 0;
  if (MILLISDIFF(ultUDPBroadcast,15000L)) {
    ultUDPBroadcast = millis();
    char buf[60];
    sprintf(buf,"%d~%d",nivel,int(boia));
    UDPTalk.send("IRG_TANK",buf);
  }
  
  ledAsWifiStatus();  
}

