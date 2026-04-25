// Lucio To-do Todo
// tratar coberto: ligar programa mesmo com chuva para esses cas
// tratar desativado até dia xx
// informar questão da previsão de chuva para afinarmos o programa
// está dando erro de mismatch de tamanho de pacote. tentar colocar pacotes sem tamanho

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>

char ssids[]     = "secretoca~goiaba";
char passwords[] = "Goiaba5090~heptA2019";
#define DEVMODE false



long int tempoBombams;
const int tempoBomba   = 5; // min


#define NUMPROGS 5
#define NUMSETORES 12
#define NUMSETORESMASTER 5
#define MAXSETORESDEVICE 8
#define NUMSETORESSLAVE (NUMSETORES-NUMSETORESMASTER)
#define TCP_CMD_PORT 5272
#define TCP_STATUS_PORT 5273
#define PIN_BTN_DISPLAY 27
#define OLED_TIMEOUT_MS (30UL * 1000UL)


#define ON  HIGH
#define OFF LOW


unsigned long quandoDesligarBomba=0;
unsigned long quandoDesligarPartida=0;
unsigned long quandoLigarBomba=0;
unsigned long quandoDesligarSetor[MAXSETORESDEVICE+1];

unsigned long setorAtualResta;


#define ON HIGH
#define OFF LOW

void ativarBomba(unsigned long tempoms=0, long delayBombams=0) {
  static unsigned long temp_tempoms = 0;
  
  if (delayBombams >0) {
    quandoLigarBomba = millis() + delayBombams;
    quandoDesligarBomba = 0;
    quandoDesligarPartida = 0;
    temp_tempoms=tempoms;
  }
  else if (delayBombams==-1) {
    quandoLigarBomba = 0;
    ativarBomba(temp_tempoms);
  }
  else {
    #ifdef MASTER
    if (tempoBombams<tempoPartidams)
      quandoDesligarPartida = millis() + tempoBombams;
    else
      quandoDesligarPartida = millis() + tempoPartidams;
    #endif
   
    if (tempoms==0)
      quandoDesligarBomba = millis() + tempoBombams;
    else
      quandoDesligarBomba = millis() + tempoms;
  }
}

void desativarBomba() {
  quandoDesligarPartida = 0;
  quandoDesligarBomba = 0;
  quandoLigarBomba = 0;
}

void acionarSetor(byte x) {
  static byte last = 255;
  if (x!=0) {
    for (byte i=0; i<NUMLOCALSECTORS; i++)
      if (x == i+1+FIRSTLOCALSECTOR) {
        digitalWrite(PIN_SECTORS[i],OPEN_SECTORS[i]);
//;Serial.print("aciona: "); Serial.print(PIN_SECTORS[i]);Serial.print("  "); Serial.println(OPEN_SECTORS[i]);        
        if (last != x) 
          delay(2000);
      }
  }
  for (byte i=0; i<NUMLOCALSECTORS; i++)
    if (x != i+1+FIRSTLOCALSECTOR)
      digitalWrite(PIN_SECTORS[i],CLOSED_SECTORS[i]);

  last = x;
}

void resetTemposSetores() {
  memset(quandoDesligarSetor,0,sizeof(quandoDesligarSetor));
}

void setores(byte delay=0,byte *tempoSetores=NULL) {
  if (tempoSetores) {
    unsigned long x;
    quandoDesligarSetor[0] = x = millis()+delay*30L*1000L;
        
    for (byte i=0;i<MAXSETORESDEVICE; i++) {
      if (tempoSetores[i]<=100)
        x += tempoSetores[i]*1000L*30L; // uso normal
      else if (tempoSetores[i]<=200)
        x += ((tempoSetores[i]-100)*30L + 5)*1000L; // para último setor - acrescenta 5 segundos
      else
        x += (tempoSetores[i]-200)*1000L;
      quandoDesligarSetor[i+1]=x;
    }
  }
  else {
    acionarSetor(0); // desliga todos
    memset(quandoDesligarSetor,0,sizeof(quandoDesligarSetor));
  }
}

bool passou(unsigned long x) { //************** Retirar? **************
  return (millis()>x || 
          millis()+ 10L*24L*60L*60L*1000L < x);
}


void processaSetores() {
  activeSector = 0;
  for (byte i=1;i<=MAXSETORESDEVICE;i++)
    if (quandoDesligarSetor[i] != 0 &&
        quandoDesligarSetor[i-1] < quandoDesligarSetor[i] &&
        passou(quandoDesligarSetor[i-1]) &&
        !passou(quandoDesligarSetor[i])) {
      acionarSetor(FIRSTLOCALSECTOR + i);
      quandoDesligarPartida = millis()+5000;
      #ifdef MASTER
        activeSector = i;
      #else
        activeSector = i+NUMSETORESMASTER;
      #endif
      setorAtualResta = quandoDesligarSetor[i] - millis();
    }
  if (activeSector==0)
    acionarSetor(0);
}

void processaBomba() {
  #ifdef MASTER
    if (quandoDesligarPartida != 0 && passou(quandoDesligarPartida))
      quandoDesligarPartida = 0;
  #endif
  
  if (quandoLigarBomba != 0) {
    if (passou(quandoLigarBomba))
      ativarBomba(0,-1); // força início
    else
      digitalWrite(PIN_BOMBA,OFF);
  }
  else 
    if (quandoDesligarBomba != 0)
      if (passou(quandoDesligarBomba)) {
        digitalWrite(PIN_BOMBA,OFF);
        desativarBomba();
      }  
      else {
        #ifdef MASTER
        // após a partida, mantém bomba somente se houver fluxo de água
        if (quandoDesligarPartida == 0 && !waterFlow) {
          digitalWrite(PIN_BOMBA, OFF);
          desativarBomba();
          sendSimpleMail("[Irrigação secretoca] Alerta de falta de água", "A bomba foi desligada devido à falta de fluxo de água.");
        } else
          digitalWrite(PIN_BOMBA, ON);
        #else
          digitalWrite(PIN_BOMBA, ON);
        #endif
          
      }
    else
      digitalWrite(PIN_BOMBA,OFF);
}

// ---------- OLED / screensaver ----------
extern Adafruit_SSD1306 oled;
unsigned long oledLastActivity = 0;

void oledWake() {
  oledLastActivity = millis();
}

void oledInitDisplay(const char *deviceName) {
  pinMode(PIN_BTN_DISPLAY, INPUT_PULLUP);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println(F("SSD1306 allocation failed"));
  else
    Serial.println("SSD1306 allocation success");
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setTextColor(WHITE);
  oled.setCursor(0, 10);
  oled.println("IRRIGACAO");
  oled.println(deviceName);
  oled.display();
  oledWake();
}

void oledHandle() {
  // botao acende display
  static bool lastBtn = HIGH;
  bool btn = digitalRead(PIN_BTN_DISPLAY);
  if (lastBtn == HIGH && btn == LOW)
    oledWake();
  lastBtn = btn;

  // programa ativo acende display
  if (quandoDesligarBomba != 0 || activeSector != 0)
    oledWake();

  bool screenOn = (millis() - oledLastActivity < OLED_TIMEOUT_MS);

  static bool screenWasOn = true;
  if (!screenOn) {
    if (screenWasOn) {
      oled.clearDisplay();
      oled.display();
      oled.ssd1306_command(SSD1306_DISPLAYOFF);
      screenWasOn = false;
    }
    return;
  }
  if (!screenWasOn) {
    oled.ssd1306_command(SSD1306_DISPLAYON);
  }
  screenWasOn = true;

  static unsigned long lastDraw = 0;
  if (!MILLISDIFF(lastDraw, 200))
    return;
  lastDraw = millis();

  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.print("Bomba: ");
  oled.setTextSize(2);
  if (quandoDesligarBomba != 0)
    oled.println((quandoDesligarBomba - millis()) / 1000);
  else
    oled.println("deslig");
  oled.setTextSize(1);
  oled.print("Setor: ");
  oled.setTextSize(2);
  if (activeSector != 0) {
    oled.print(activeSector);
    oled.print(" ");
    oled.println(setorAtualResta / 1000);
  } else
    oled.println("nenhum");
  oled.setTextSize(1);
  oled.println();
  if (WiFi.status() == WL_CONNECTED) {
    oled.print("signal: "); oled.print(WiFi.RSSI());
    oled.print(" ("); oled.print(WiFi.localIP()[3]); oled.println(")");
  } else
    oled.print("conectando...");
  oled.display();
}

