// Lucio To-do Todo
// tratar coberto: ligar programa mesmo com chuva para esses cas
// tratar desativado até dia xx
// informar questão da previsão de chuva para afinarmos o programa
// está dando erro de mismatch de tamanho de pacote. tentar colocar pacotes sem tamanho

#include <Arduino.h>
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


#define ON  HIGH
#define OFF LOW


unsigned long quandoDesligarBomba=0;
unsigned long quandoDesligarPartida=0;
unsigned long quandoLigarBomba=0;
unsigned long quandoDesligarSetor[MAXSETORESDEVICE+1];

IOTKUDP UDPTalk;

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
;Serial.print("aciona: "); Serial.print(PIN_SECTORS[i]);Serial.print("  "); Serial.println(OPEN_SECTORS[i]);        
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
    if (quandoDesligarPartida != 0)
      if (passou(quandoDesligarPartida)) { /*****Lucio: é partida */
        digitalWrite(PIN_START,OFF);
        quandoDesligarPartida = 0;
      }  
      else
        digitalWrite(PIN_START,ON);
    else
      digitalWrite(PIN_START,OFF);
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
      else
        digitalWrite(PIN_BOMBA,ON);
    else
      digitalWrite(PIN_BOMBA,OFF);
}

