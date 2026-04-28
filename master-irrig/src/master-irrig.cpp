#include <IOTK.h>
#include <IOTK_Dallas.h>
#include <IOTK_CurrentMonitor.h>
#include <IOTK_NTP.h>
#include <IOTK_UDP.h>
#include <IOTK_ESPAsyncServer.h>
#include <HTTPClient.h>
#include <__Timer.h>
#include <IOTK_SimpleMail.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NTPClient.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

Adafruit_SSD1306 oled(128, 64, &Wire, -1);

#define PIN_BOMBA 17
#define PIN_WATER_FLOW     16
#define PIN_WATER_FLOW_LED 18
#define PIN_STATUS_LED    19

byte PIN_SECTORS[]    = {33,  32,  26,  25,  23};     // master
byte OPEN_SECTORS[]   = {HIGH, HIGH, HIGH, HIGH, HIGH};
byte CLOSED_SECTORS[] = {LOW,  LOW,  LOW,  LOW,  LOW};

String callStr = 
      "http://api.openweathermap.org/data/2.5/forecast?lat=-22.367&lon=-43.183&appid=b847ba98415936c72703f5873c89bcdb&units=metric&mode=json"; // secretario;
      //"http://api.openweathermap.org/data/2.5/forecast?lat=-1.455&lon=-48.502&appid=b847ba98415936c72703f5873c89bcdb&units=metric&mode=json"; // belem
      // "http://api.openweathermap.org/data/2.5/forecast?lat=-25.548&lon=-54.588&appid=b847ba98415936c72703f5873c89bcdb&units=metric&mode=json"; // foz
 

#define NUMLOCALSECTORS 5
#define FIRSTLOCALSECTOR 0
#define LASTLOCALSECTOR 4
#define PIN_START 2

#define INTERVALOPREVISAO (60L * 60L * 1000L)

#define thingspeakInterval 30000L
#define thingpeakID 1757100
#define thingspeakKey "FT1AY0G9WHN7KLHE"

byte tempoPartida = 40; //seg
unsigned long int tempoPartidams;

int activeSector;
bool masterPump;
float rainForecast;
bool skipIrrigation = false;

// Remote data
unsigned long int lastSlaveRead=0;
byte setorSlave = 0;
bool bombaSlave = false;

unsigned long int irrigTimeToday = 0;

bool waterFlow = false; // true = há fluxo de água no pino 16
bool waterAlertPending = false; // flag: bomba desligada por falta de fluxo
byte waterAlertSector = 0;      // setor ativo no momento da falta de fluxo

unsigned long bombaPumpOffSince = 0;    // millis() quando bomba desligou; 0=desconhecido/nunca ligou
bool fluxoSempreCheckAtivo = false;     // checagem "água sempre" em andamento (janela de 2s)
unsigned long fluxoSempreCheckAte = 0; // quando os 2s da checagem expiram
bool waterFluxoSemprePending = false;   // flag para envio do e-mail de erro no sensor

#define MASTER
#include "../../Irrigacao_common.h"
WiFiServer tcpStatusServer(TCP_STATUS_PORT);

// program parameters
byte progInicioMem=0;  
byte lastRunDay;
byte progHora[NUMPROGS],progMin[NUMPROGS];
byte progTempoSetor[NUMPROGS][NUMSETORES];
bool progDiaSemana[NUMPROGS][7];
byte progCoberto[NUMSETORES];
byte progModoBomba;
byte progPrevisao; // 0=automático, 1=alta (cancela), 2=baixa (não cancela)
byte progFuncionamento;
unsigned long bloqueioAte = 0; // millis() alvo para auto-desbloqueio; 0 = não armado
byte progFimMem=0;
char slaveIP[16] = "192.168.29.191";
char caixaIP[16]  = "192.168.29.192";

unsigned long int Epoch;
unsigned long int EpochMillis;
unsigned long int nowEpoch=0;

char slaveStatus[128];

enum HttpCommandType {
  HTTP_CMD_NONE,
  HTTP_CMD_LIGA_POCO,
  HTTP_CMD_LIGA_CAIXA,
  HTTP_CMD_DESLIGA,
  HTTP_CMD_PROGRAMA
};

struct PendingHttpCommand {
  HttpCommandType type;
  long overrideTempo;
  int progNum;
};

PendingHttpCommand pendingHttpCommand = {HTTP_CMD_NONE, 0, 0};
volatile bool httpCommandPending = false;
volatile bool httpCommandBusy = false;
char lastHttpCommandStatus[128] = "nenhum comando HTTP executado";
unsigned long lastHttpCommandMillis = 0;
portMUX_TYPE httpCommandMux = portMUX_INITIALIZER_UNLOCKED;

void setLastHttpCommandStatus(const char *status) {
  portENTER_CRITICAL(&httpCommandMux);
  strncpy(lastHttpCommandStatus, status, sizeof(lastHttpCommandStatus) - 1);
  lastHttpCommandStatus[sizeof(lastHttpCommandStatus) - 1] = 0;
  lastHttpCommandMillis = millis();
  portEXIT_CRITICAL(&httpCommandMux);
}

void setLastHttpCommandStatus(const String &status) {
  char statusBuf[sizeof(lastHttpCommandStatus)];
  status.toCharArray(statusBuf, sizeof(statusBuf));
  setLastHttpCommandStatus(statusBuf);
}

bool queueHttpCommand(HttpCommandType type, long overrideTempo = 0, int progNum = 0) {
  bool queued = false;
  portENTER_CRITICAL(&httpCommandMux);
  if (!httpCommandPending && !httpCommandBusy) {
    pendingHttpCommand.type = type;
    pendingHttpCommand.overrideTempo = overrideTempo;
    pendingHttpCommand.progNum = progNum;
    httpCommandPending = true;
    queued = true;
  }
  portEXIT_CRITICAL(&httpCommandMux);
  if (queued)
    setLastHttpCommandStatus("comando aceito, aguardando execucao");
  return queued;
}

void queueHttpCommandOrRespond(AsyncWebServerRequest *request, HttpCommandType type, long overrideTempo, int progNum) {
  if (queueHttpCommand(type, overrideTempo, progNum))
    responseConfirmation(request, "Comando aceito. Consulte /cmdstatus para o resultado.");
  else
    responseConfirmation(request, "Ja existe um comando HTTP em processamento. Consulte /cmdstatus.");
}

void handleCmdStatus(AsyncWebServerRequest *request) {
  char statusBuf[sizeof(lastHttpCommandStatus)];
  unsigned long statusMillis;

  portENTER_CRITICAL(&httpCommandMux);
  strncpy(statusBuf, lastHttpCommandStatus, sizeof(statusBuf) - 1);
  statusBuf[sizeof(statusBuf) - 1] = 0;
  statusMillis = lastHttpCommandMillis;
  portEXIT_CRITICAL(&httpCommandMux);

  String resp = String(statusBuf);
  if (statusMillis != 0)
    resp += " (" + String((millis() - statusMillis) / 1000UL) + " s)";
  request->send(200, "text/plain; charset=utf-8", resp);
}

void processPendingHttpCommand();

void serverHandleRoot(AsyncWebServerRequest *request) {
  File f=LittleFS.open("/irrigacao.html","r");
  if (!f) {
    Serial.println("File open failed");
    request->send(404, "text/html; charset=utf-8", "File open failed");
  }
  else {
    String s = f.readString();
    f.close();
    request->send(200, "text/html; charset=utf-8", s);
  }
}

float readSlaveStatus(char *a) {
  setorSlave = a[0] - '0';
  bombaSlave = a[1] == '1';
  strncpy(slaveStatus,a+2,127);
  lastSlaveRead = millis();
  /*;Serial.print("Slave transmitiu===>");
  ;Serial.println(a);
  ;Serial.print("processei ==> ");
  ;Serial.println(slaveStatus);*/
  return 0;
}

String fnsz(int n, byte sz) {
  String s = String(n);
  while (s.length()<sz)
    s = "0" + s;
  return s;
}  


String getStatus() {
  String status;

  status  =  "<br>Current time: " + fnsz(NTPHour(),2) + ":" + fnsz(NTPMinute(),2) + ":" + fnsz(NTPSecond(),2); //***** Implemnetar na bibioteca **
  switch (NTPDayOfWeek()) {
    case 0: status += " - sunday"; break;
    case 1: status += " - monday"; break;
    case 2: status += " - tuesday"; break;
    case 3: status += " - wednesday"; break;
    case 4: status += " - thursday"; break;
    case 5: status += " - friday"; break;
    case 6: status += " - saturday"; break;
  }
  status += "</br>";
  status += "<br> rain forcast: " + String(rainForecast) + "</br>";

  status += "<br>Last slave read: " + String ((millis()-lastSlaveRead)/1000L) + " seg   ---   sector: " + String(setorSlave) + "  -  pump: ";
  if (bombaSlave) status += "ON"; else status += "OFF";
  status += "</br>";

  status += "<br>M A S T E R :<p><p>";

  if (quandoDesligarBomba!=0) {
    status += "Bomba ligada por mais " + String ((quandoDesligarBomba-millis())/1000) + " seg";
    if (quandoDesligarPartida!=0)
      status += " (em partida) " + String(quandoDesligarPartida) + " - " + String(millis());
  }
  else
    status += "Bomba desligada";
  status += "<p>";

  if (activeSector != 0)
    status += "setor " + String(activeSector) + " ativo por mais " +String(setorAtualResta/1000)+ " seg";
  else
    status += "nenhum setor ativo";

  status += "</br>";

  status += "<br><p><p>S L A V E:<p>";
  
  if (MILLISDIFF(lastSlaveRead,60000L))
    status += "OFFLINE";
  else
    status += String(slaveStatus);

  char cmdStatus[sizeof(lastHttpCommandStatus)];
  portENTER_CRITICAL(&httpCommandMux);
  strncpy(cmdStatus, lastHttpCommandStatus, sizeof(cmdStatus) - 1);
  cmdStatus[sizeof(cmdStatus) - 1] = 0;
  portEXIT_CRITICAL(&httpCommandMux);

  status += "<br><p><p>CMD HTTP:<p>" + String(cmdStatus);
 
  return status;
}

bool slaveCmd(int tempoBomba, 
              int delayBomba=0,
              int delaySetores=0,
              byte *temposSetores=NULL){

  bool ok = false;
  

  Serial.print("tempoBomba = "); Serial.println(tempoBomba); //*************
  
  String callStr = String(tempoBomba) + "~" + String(delayBomba) + "~" + String(delaySetores);
  for (byte i = NUMSETORESMASTER; i<NUMSETORES; i++)
    if (temposSetores)
      callStr += "~" + String(temposSetores[i-NUMSETORESMASTER]);
    else
      callStr += "~0";
  for (byte attempt = 1; attempt <= 5 && !ok; attempt++) {
    if (attempt > 1) {
      Serial.printf("slaveCmd: aguardando 0.5s antes da tentativa %d/5...\n", attempt);
      yield();
      delay(2000);
      yield();
    }
    { // para forçar destruição das variáveis http e tcpClient antes da próxima tentativa
      WiFiClient tcpClient;
      delay(1); // reseta TWDT antes de bloquear no connect
      if (tcpClient.connect(slaveIP, TCP_CMD_PORT, 1500)) {
        tcpClient.println(callStr);
        unsigned long t = millis();
        while (!tcpClient.available() && millis() < t + 3000) delay(1);
        String response = tcpClient.readStringUntil('\n');
        response.trim();
        tcpClient.stop();
        delay(1);
        ok = response.startsWith("OK");
        if (ok)
          Serial.printf("slaveCmd TCP sucesso (tentativa %d): ", attempt);
        else
          Serial.printf("slaveCmd TCP resposta inesperada (tentativa %d): ", attempt);
        Serial.println(callStr);
      } else {
        Serial.printf("slaveCmd TCP: falha na conexão (tentativa %d/5)\n", attempt);
      }
    }
  }
  return ok;
}


bool lePrevisao() {
  static unsigned long lastPrevisao = 999999999; //************** Passar para controle do IOTK 
  bool ok = false;
  
  if (passou(lastPrevisao + INTERVALOPREVISAO)) {
    lastPrevisao = millis();
    
    HTTPClient http;
    

    String payLoad;

    if (http.begin(client,callStr)) {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        Serial.println("request de previsao de tempo enviado com sucesso");
        payLoad = http.getString();
        ok = true;
      }
      else {
        Serial.printf("[HTTP] GET... failed, error: %d %s\n", httpCode, http.errorToString(httpCode).c_str());
      }
    }
    else
      Serial.println("Falha ao iniciar http para leitura da previsao");
    http.end();
    
    int pSearch = 0;
    float previsao = 0;
    String stProb   = "\"pop\":";
    String stRain   = "\"rain\":{\"3h\":";
    for (int i = 0; i < 8; i++) {
      int pPop = payLoad.indexOf(stProb, pSearch);
      if (pPop == -1) break;

      // limite da janela = início do próximo pop (ou fim do payload)
      int pNext = payLoad.indexOf(stProb, pPop + stProb.length());
      int windowEnd = (pNext == -1) ? payLoad.length() : pNext;

      float prob = payLoad.substring(pPop + stProb.length(), pPop + stProb.length() + 5).toFloat();

      float vol = 0;
      int pRain = payLoad.indexOf(stRain, pPop);
      if (pRain != -1 && pRain < windowEnd) {
        int vStart = pRain + stRain.length();
        vol = payLoad.substring(vStart, vStart + 6).toFloat();
      }

      previsao += prob * prob * vol;
      Serial.printf("%d: pop=%.2f  rain3h=%.2f  contrib=%.3f  total=%.3f\n",
                    i, prob, vol, prob * prob * vol, previsao);

      pSearch = pPop + stProb.length();
    }
    rainForecast = previsao;
  }
  else
    ok = true;
    
  // *******okPrevisao = ok; está com bug nisso ***
  
  return ok;
}

void ligaPoco(AsyncWebServerRequest *request) {
  queueHttpCommandOrRespond(request, HTTP_CMD_LIGA_POCO, 0, 0);
}

void ligaCaixa(AsyncWebServerRequest *request) {
  queueHttpCommandOrRespond(request, HTTP_CMD_LIGA_CAIXA, 0, 0);
}

void desliga(AsyncWebServerRequest *request) {
  queueHttpCommandOrRespond(request, HTTP_CMD_DESLIGA, 0, 0);
}

String programa_(AsyncWebServerRequest *request, long overrideTempo,int progNum) {
  bool bombaMaster = progModoBomba==0 || progModoBomba==2;
  int j;
  if (overrideTempo==0)
    j = progNum;
  else
    j = 1;
    
  if (j>0) j--;
  byte tempoSetorMaster[MAXSETORESDEVICE];
  byte tempoSetorSlave [MAXSETORESDEVICE];
  int delaySlave = 0;
  int tempoBombaPgm=0;
  
  memset(tempoSetorMaster,0,sizeof(tempoSetorMaster));
  memset(tempoSetorSlave ,0,sizeof(tempoSetorSlave));
  byte lastMaster=99, lastSlave=99;
  byte tempoSet;
  
  for (byte k=0; k<NUMSETORESMASTER; k++) {
    if (j>=0) // se for programa
      if (overrideTempo)
        if (progTempoSetor[j][k]>0)
          tempoSet = overrideTempo;
        else
          tempoSet = 0;
      else
        tempoSet= progTempoSetor[j][k];
    else // se for único setor
      tempoSet = k+1 != -j ? 0 : overrideTempo ? overrideTempo : tempoBomba;
        
    if (tempoSet)
      lastMaster = k;

    delaySlave += tempoSet;
    tempoBombaPgm += tempoSet;
      
    tempoSetorMaster[k] = tempoSet;
  }
  
  for (byte k=0; k<NUMSETORESSLAVE; k++) {
    if (j>=0) // se for programa
      if (overrideTempo)
        if (progTempoSetor[j][k+NUMSETORESMASTER]>0)
          tempoSet = overrideTempo;
        else
          tempoSet = 0;
      else
        tempoSet = progTempoSetor[j][k+NUMSETORESMASTER];
    else // se for acionamento de único setor
      tempoSet = k+1+NUMSETORESMASTER != -j ? 0 : overrideTempo ? overrideTempo : tempoBomba;
        
    if (tempoSet) 
      lastSlave = k;

    tempoBombaPgm += tempoSet;
    tempoSetorSlave[k] = tempoSet;
  }
      
  if (lastSlave!=99)
    tempoSetorSlave[lastSlave]+=100;
  else if (lastMaster!=99)
    tempoSetorMaster[lastMaster]+=100;
  else {
    if (request) responseConfirmation(request, "Nenhum setor na programação - programa abortado");
    return "ABORTADO - Nenhum setor na programação";
  }
      
  Serial.println("Iniciando programa");
  Serial.printf("Bomba: %s por %i seg\n\r", bombaMaster ? "Poco" : "Chuva", tempoBombaPgm*30);
  Serial.printf("Setores master:\n\r");
  for (byte k=0; k<NUMSETORESMASTER; k++) 
    Serial.printf("   %i --> %i seg\n\r",(int)k+1,(int)tempoSetorMaster[k]*30);
  Serial.printf("Setores Slave (delay =%i):\n\r",delaySlave);
  for (byte k=0; k<NUMSETORESSLAVE; k++) 
    Serial.printf("   %i --> %i seg\n\r",(int)k+NUMSETORESMASTER,(int)tempoSetorSlave[k]*30);
    
  if (bombaMaster) {
    if (slaveCmd(0,0,delaySlave,tempoSetorSlave)) {
      setores(0,tempoSetorMaster);
      if (tempoBombaPgm>0)
        ativarBomba(tempoBombaPgm*30L*1000L);
      if (request) responseConfirmation(request, "Executando programa com bomba do poço");
      return "iniciando";
    }
    else {
      if (request) responseConfirmation(request, "Falha de comunicação com slave - programa abortado");
      return "ABORTADO - falha de comunicação com slave";
    }
  }
  else
    if (slaveCmd(tempoBombaPgm,0,delaySlave,tempoSetorSlave)) {
      setores(0,tempoSetorMaster);
      desativarBomba();
      if (request) responseConfirmation(request, "Executando programa com bomba da chuva");
      return "iniciando";
    }
    else {
      if (request) responseConfirmation(request, "Falha de comunicação com slave - programa abortado");
      return "ABORTADA - falha de comuicação com slave";
    }
}

void comandoSetor(AsyncWebServerRequest *request, byte x) {
  queueHttpCommandOrRespond(request, HTTP_CMD_PROGRAMA, 0, -x);
}

void setor(AsyncWebServerRequest *request) { 
  if (request->argName(0) != "setor") {
    Serial.print(request->argName(0)); 
    Serial.println(" nao e um parametro valido");
    responseConfirmation(request, "Parametro invalido na execuçao de setor");
  }
  else {
    int x = request->arg((size_t)0).toInt();
    if (x<1 || x>NUMSETORES) {
      Serial.print(x); 
      Serial.println(" nao e um setor valido");
      responseConfirmation(request, "Numero de setor invalido na execuçao do comando setor");      
    }
    else 
      comandoSetor(request, x); 
  }
}

void programa(AsyncWebServerRequest *request) {
  if(request->argName(0) != "prog") {
    Serial.print(request->argName(0)); 
    Serial.println(" nao e um parametro valido");
    responseConfirmation(request, "Chamada inválida");
  }
  else {    
    int x = request->arg((size_t)0).toInt();
    if (x>=1 && x<=NUMPROGS)
      queueHttpCommandOrRespond(request, HTTP_CMD_PROGRAMA, 0, x);
    else {
      Serial.print(request->arg((size_t)0));
      Serial.println(" não é um programa válido");
      responseConfirmation(request, "Programa inválido");
    }
  }
}

void progRapido(AsyncWebServerRequest *request) {
  queueHttpCommandOrRespond(request, HTTP_CMD_PROGRAMA, 1, 1);
}

void processPendingHttpCommand() {
  PendingHttpCommand cmd;

  portENTER_CRITICAL(&httpCommandMux);
  if (!httpCommandPending || httpCommandBusy) {
    portEXIT_CRITICAL(&httpCommandMux);
    return;
  }
  cmd = pendingHttpCommand;
  httpCommandPending = false;
  httpCommandBusy = true;
  portEXIT_CRITICAL(&httpCommandMux);

  setLastHttpCommandStatus("processando comando HTTP");

  String result;
  switch (cmd.type) {
    case HTTP_CMD_LIGA_POCO:
      ativarBomba();
      slaveCmd(0);
      result = "Ligando bomba do master - sem setor";
      break;

    case HTTP_CMD_LIGA_CAIXA:
      if (slaveCmd(tempoBomba)) {
        desativarBomba();
        result = "Ligando bomba do slave - sem setor";
      } else
        result = "Falha ao comunicar com o slave";
      break;

    case HTTP_CMD_DESLIGA: {
      byte alivio[MAXSETORESDEVICE]={205,0,0,0,0,0};
      desativarBomba();
      setores(0,alivio);
      if (slaveCmd(0))
        result = "Desligando master e slave";
      else
        result = "Desligando master - falha ao comunicar com o slave";
      break;
    }

    case HTTP_CMD_PROGRAMA:
      result = programa_(NULL, cmd.overrideTempo, cmd.progNum);
      break;

    default:
      result = "Nenhum comando HTTP pendente";
      break;
  }

  portENTER_CRITICAL(&httpCommandMux);
  httpCommandBusy = false;
  portEXIT_CRITICAL(&httpCommandMux);

  setLastHttpCommandStatus(result);
  Serial.print("Comando HTTP concluido: ");
  Serial.println(result);
}

uint16_t computeConfigChecksum() {
  uint16_t sum = 0;
  for (byte i = 0; i < NUMPROGS; i++) {
    sum += progHora[i] + progMin[i];
    for (byte j = 0; j < NUMSETORES; j++) sum += progTempoSetor[i][j];
    for (byte j = 0; j < 7; j++)         sum += (byte)progDiaSemana[i][j];
  }
  for (byte i = 0; i < NUMSETORES; i++) sum += progCoberto[i];
  sum += progModoBomba + progPrevisao + progFuncionamento + lastRunDay + tempoPartida;
  for (int i = 0; slaveIP[i]; i++) sum += (byte)slaveIP[i];
  for (int i = 0; caixaIP[i]; i++) sum += (byte)caixaIP[i];
  return sum;
}

void gravaConfig() {
  Preferences prefs;
  prefs.begin("irrig", false);
  prefs.putBytes("hora",      progHora,        sizeof(progHora));
  prefs.putBytes("min",       progMin,         sizeof(progMin));
  prefs.putBytes("tempo",     progTempoSetor,  sizeof(progTempoSetor));
  prefs.putBytes("dias",      progDiaSemana,   sizeof(progDiaSemana));
  prefs.putBytes("coberto",   progCoberto,     sizeof(progCoberto));
  prefs.putUChar("modoBomba", progModoBomba);
  prefs.putUChar("previsao",  progPrevisao);
  prefs.putUChar("funcmnt",   progFuncionamento);
  prefs.putUChar("lastDay",   lastRunDay);
  prefs.putString("slaveIP",  slaveIP);
  prefs.putString("caixaIP",  caixaIP);
  prefs.putUChar("tPartida",  tempoPartida);
  prefs.putUShort("csum",     computeConfigChecksum());
  prefs.end();
}

void leConfig() {
  // Inicializa defaults
  memset(progHora,        0, sizeof(progHora));
  memset(progMin,         0, sizeof(progMin));
  memset(progTempoSetor,  0, sizeof(progTempoSetor));
  memset(progDiaSemana,   0, sizeof(progDiaSemana));
  memset(progCoberto,     0, sizeof(progCoberto));
  progModoBomba = 0;  progPrevisao = 0;
  progFuncionamento = 0;  lastRunDay = 0;
  strncpy(slaveIP, "192.168.1.100", sizeof(slaveIP));
  strncpy(caixaIP, "192.168.1.101", sizeof(caixaIP));

  Preferences prefs;
  prefs.begin("irrig", true);
  uint16_t storedCsum = prefs.getUShort("csum", 0xFFFF);
  prefs.getBytes("hora",    progHora,       sizeof(progHora));
  prefs.getBytes("min",     progMin,        sizeof(progMin));
  prefs.getBytes("tempo",   progTempoSetor, sizeof(progTempoSetor));
  prefs.getBytes("dias",    progDiaSemana,  sizeof(progDiaSemana));
  prefs.getBytes("coberto", progCoberto,    sizeof(progCoberto));
  progModoBomba     = prefs.getUChar("modoBomba", 0);
  progPrevisao      = prefs.getUChar("previsao",  0);
  progFuncionamento = prefs.getUChar("funcmnt",   0);
  lastRunDay        = prefs.getUChar("lastDay",   0);
  prefs.getString("slaveIP", slaveIP, sizeof(slaveIP));
  prefs.getString("caixaIP", caixaIP, sizeof(caixaIP));
  tempoPartida = prefs.getUChar("tPartida", 40);
  prefs.end();

  if (storedCsum != computeConfigChecksum()) {
    Serial.println("Config NVS inválida - resetando para defaults");
    memset(progHora,        0, sizeof(progHora));
    memset(progMin,         0, sizeof(progMin));
    memset(progTempoSetor,  0, sizeof(progTempoSetor));
    memset(progDiaSemana,   0, sizeof(progDiaSemana));
    memset(progCoberto,     0, sizeof(progCoberto));
    progModoBomba = 0;  progPrevisao = 0;
    progFuncionamento = 0;  lastRunDay = 0;
    strncpy(slaveIP, "192.168.1.100", sizeof(slaveIP));
    strncpy(caixaIP, "192.168.1.101", sizeof(caixaIP));
    tempoPartida = 40;
  } else {
    Serial.println("Config carregada do NVS com sucesso");
  }
}

void handleConfig(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_POST) {
    if (request->hasArg("slaveIP"))
      request->arg("slaveIP").toCharArray(slaveIP, sizeof(slaveIP));
    if (request->hasArg("caixaIP"))
      request->arg("caixaIP").toCharArray(caixaIP, sizeof(caixaIP));
    if (request->hasArg("tempoPartida")) {
      int v = request->arg("tempoPartida").toInt();
      if (v > 0 && v <= 255) tempoPartida = (byte)v;
    }
    gravaConfig();
    tempoPartidams = (tempoPartida * 1000L) - LACKOFFLOW_TIMEOUT_MS;
    request->send(200, "text/html; charset=utf-8",
      "<html><body><p>Configuração salva!</p><a href='/config'>Voltar</a></body></html>");
  } else {
    String html = "<html><body><h2>Configuração de IPs</h2>"
      "<form method='POST' action='/config'>"
      "<p><label>IP do Slave: <input type='text' name='slaveIP' value='" + String(slaveIP) + "'></label></p>"
      "<p><label>IP da Caixa: <input type='text' name='caixaIP' value='" + String(caixaIP) + "'></label></p>"
      "<p><label>Tempo de partida (seg): <input type='number' name='tempoPartida' value='" + String(tempoPartida) + "' min='1' max='255'></label></p>"
      "<p><input type='submit' value='Salvar'></p>"
      "</form></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
  }
}


void gravaProg(AsyncWebServerRequest *request) {
  Serial.println("Gravando programação");
  bool ok=false;
  String nome;

  for (byte k=0;k<7;k++) {
    for (byte j=0;j<NUMPROGS;j++)
      progDiaSemana[j][k] = false;
    progCoberto[k] = false;
  }
      
  for (int i = 0; i<request->args(); i++) {
    for (byte j=0;j<NUMPROGS;j++) {
      nome = "inicioP"+String(j+1);
      if (request->argName(i)==nome) {
        progHora[j] = request->arg(i).substring(0,2).toInt();
        progMin[j]  = request->arg(i).substring(3,5).toInt();
      }
      for (byte k=0;k<NUMSETORES;k++) {
        nome = "tempoP"+String(j+1)+"S"+String(k+1);
        if (request->argName(i)==nome) {
          progTempoSetor[j][k] = request->arg(i).toInt();
          if (request->arg(i) != "")
            ok = true;
        }
      }
      for (byte k=0;k<7;k++) {
        nome = "diaP"+String(j+1)+"WD"+String(k+1);
        if (request->argName(i)==nome && request->arg(i)=="on") 
          progDiaSemana[j][k] = true;
      }
    }        
    for (byte k=0;k<NUMSETORES;k++) {
      nome = "cobertoS"+String(k+1);
      if (request->argName(i)==nome && request->arg(i)=="on")
        progCoberto[k] = true;
    }

    if (request->argName(i)=="modoBomba")
      progModoBomba = request->arg(i).toInt();
    if (request->argName(i)=="previsao")
      progPrevisao = request->arg(i).toInt();
    if (request->argName(i)=="funcionamento") {
      progFuncionamento = request->arg(i).toInt();
      bloqueioAte = (progFuncionamento == 2) ? millis() + 24UL*3600UL*1000UL : 0;
    }
  }
  if (ok) {
    gravaConfig();
    responseConfirmation(request, "Configuração gravada");
  }
  else {
    responseConfirmation(request, "Descartado comando - refazer programação");
    leConfig();
  }
}

String leProgStr() {
  String s;
  String pref="document.getElementById('";
  String suf = "').value=";
  String sufCheckTrue = "').checked=true";
  String sufCheckFalse = "').checked=false";
  String EoL = ";\r\n";
  
  s = pref+"modoBomba"+suf + String(progModoBomba) + EoL;
  s += pref+"previsao"+suf + String(progPrevisao) + EoL;
  s += pref+"funcionamento"+suf+String(progFuncionamento) + EoL;
  
  
  for (byte j=0;j<NUMPROGS;j++) {
    s += pref+"inicioP"+String(j+1)+suf + +"'"+ fnsz(progHora[j],2)+":"+fnsz(progMin[j],2) + "'" + EoL;
    for (byte k=0;k<NUMSETORES;k++) 
      s += pref+"tempoP"+String(j+1)+"S"+String(k+1)+suf + String(progTempoSetor[j][k]) + EoL;
    for (byte k=0; k<7; k++)
      if (progDiaSemana[j][k])
        s += pref+"diaP"+String(j+1)+"WD"+String(k+1)+sufCheckTrue + EoL;
      else
        s += pref+"diaP"+String(j+1)+"WD"+String(k+1)+sufCheckFalse + EoL;
  }
  for (byte k=0;k<NUMSETORES;k++) 
    if (progCoberto[k])
      s += pref+"cobertoS"+String(k+1)+sufCheckTrue + EoL;
    else
      s += pref+"cobertoS"+String(k+1)+sufCheckFalse + EoL;

  return s;
}

void leProg(AsyncWebServerRequest *request)  {
  request->send(200, "text/plain; charset=utf-8", leProgStr());    
}

struct EmailTaskParams {
  char subject[160];
  char body[512];
};

void emailTaskFn(void *param) {
  EmailTaskParams *e = (EmailTaskParams*)param;
  sendSimpleMail(String(e->subject), String(e->body), true);
  delete e;
  vTaskDelete(NULL);
}

void enviarEmail(String subject_, String message_) {
  EmailTaskParams *e = new EmailTaskParams();
  ("[Irrigação secretoca] " + subject_).toCharArray(e->subject, sizeof(e->subject));
  message_.toCharArray(e->body, sizeof(e->body));
  xTaskCreate(emailTaskFn, "sendMail", 8192, e, 1, NULL);
}

void testaEmail(AsyncWebServerRequest *request) {
  enviarEmail("E-mail de teste", "Este é um e-mail de teste enviado pela irrigação da Secretoca");
  responseConfirmation(request, "E-mail enviado em background");
}


void writeToThingspeak() {
  static unsigned long int lastTS = 0;
  byte status = 0;
  if (MILLISDIFF(lastTS,thingspeakInterval)) {
    if (masterPump || bombaSlave) {
      if (activeSector !=0 || setorSlave != 0)
        status = 1;
      else
        status = 2;
    }
    else
        status = 0;
        
    ThingSpeak.setField(1,0 /*waterTankLevel*/);
    ThingSpeak.setField(2,0 /*float(waterInTank)*/);
    ThingSpeak.setField(3,rainForecast);
    ThingSpeak.setField(4,0 /*rainSinceNoon*/);
    ThingSpeak.setField(5,progModoBomba);
    //humidity
    ThingSpeak.setField(7,float(irrigTimeToday));
    ThingSpeak.setField(8,status);

    ThingSpeak.writeFields(thingpeakID,thingspeakKey);
    lastTS = millis();
  }
}

void setup() {

  pinMode(PIN_BOMBA,OUTPUT);     digitalWrite(PIN_BOMBA, OFF);

  pinMode(PIN_WATER_FLOW,     INPUT);
  pinMode(PIN_WATER_FLOW_LED, OUTPUT);
  for (byte i=0; i<NUMLOCALSECTORS; i++) {
    pinMode(PIN_SECTORS[i], OUTPUT); 
    digitalWrite(PIN_SECTORS[i],CLOSED_SECTORS[i]);
  }
  
  Serial.begin(115200);
  strcpy(ESP_AppName, "Irrigation - master");
  Wire.begin(5,4);
  oledInitDisplay("MASTER", 2);

  setStatusSource(getStatus);
  const char* ssid_arr[]     = {"secretoca", "goiaba"};
  const char* pass_arr[]     = {"Goiaba5090", "heptA2019"};
  ESPSetup(2, ssid_arr, pass_arr);
  ThingSpeak.begin(client);


  resetTemposSetores();

  server.on("/", HTTP_ANY, serverHandleRoot);
  server.on("/ligapoco",ligaPoco);
  server.on("/ligacaixa",ligaCaixa);
  server.on("/desliga", desliga);
  server.on("/setor",setor);
  server.on("/programa",programa);
  server.on("/prograpido",progRapido);
  server.on("/gravaProg",gravaProg);
  server.on("/leProg",leProg);
  server.on("/testaemail",testaEmail);
  server.on("/config", HTTP_ANY, handleConfig);
  server.on("/cmdstatus", HTTP_GET, handleCmdStatus);

  leConfig();

  tempoBombams = tempoBomba * 1000L *30L;
  tempoPartidams = (tempoPartida * 1000L) - LACKOFFLOW_TIMEOUT_MS;

  NTPBegin(-3);  
  tcpStatusServer.begin();
  

  if(LittleFS.begin())
    Serial.println("LittleFS Initialize....ok");
  else 
    Serial.println("LittleFS Initialization...failed");  

  setBuiltinLedPin(PIN_STATUS_LED);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
}

void loop() {
  handle_IOTK();
  processPendingHttpCommand();

  // Recebe status do slave via TCP
  WiFiClient statusClient = tcpStatusServer.available();
  if (statusClient) {
    unsigned long t = millis();
    while (!statusClient.available() && millis() < t + 2000) yield();
    String line = statusClient.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
      readSlaveStatus((char*)line.c_str());
    statusClient.stop();
  }

  //server.handleClient();
  lePrevisao();

  processaBomba();
  processaSetores();

  if (waterAlertPending) {
    waterAlertPending = false;
    String msg = "A bomba foi desligada após o tempo de partida pois não foi detectado fluxo de água.";
    if (waterAlertSector != 0)
      msg += " Setor aberto: " + String(waterAlertSector) + ".";
    enviarEmail("Alerta de FALTA DE ÁGUA", msg);
  }

  masterPump = quandoDesligarBomba != 0;

  // Checagem de sensor de fluxo preso em "água sempre":
  // Se a bomba ficou desligada >5 min e, ao ligar, detectar água por 2 s contínuos,
  // considera falha do sensor (indicando água sempre) e envia e-mail de erro.
  {
    static bool lastPump = false;
    if (masterPump && !lastPump) {
      // bomba acabou de ligar
      if (bombaPumpOffSince == 0 || millis() - bombaPumpOffSince >= 5UL * 60UL * 1000UL) {
        fluxoSempreCheckAtivo = true;
        fluxoSempreCheckAte = millis() + 2000UL; // observa os 2 primeiros segundos
      }
      bombaPumpOffSince = 0;
    }
    if (!masterPump && lastPump) {
      // bomba acabou de desligar
      bombaPumpOffSince = millis();
      fluxoSempreCheckAtivo = false;
    }
    lastPump = masterPump;
  }

  if (fluxoSempreCheckAtivo) {
    if (!waterFlow) {
      // sensor sem fluxo em algum momento — está OK, cancela checagem
      fluxoSempreCheckAtivo = false;
    } else if (millis() >= fluxoSempreCheckAte) {
      // 2s com fluxo ininterrupto antes da bomba poder causar isso — sensor travado
      fluxoSempreCheckAtivo = false;
      waterFluxoSemprePending = true;
    }
  }

  if (waterFluxoSemprePending) {
    waterFluxoSemprePending = false;
    enviarEmail("ERRO NO SENSOR DE FLUXO",
      "Sensor de fluxo indicando água sempre. "
      "Ao iniciar a partida (após mais de 5 min desligado), "
      "fluxo detectado por 2 segundos sem que a bomba pudesse causá-lo. "
      "Verificar sensor de fluxo - possível falha permanente.");
    Serial.println("Erro no sensor de fluxo: indicando água sempre");
  }

  static unsigned lastCheckTime = 0;
  if (MILLISDIFF(lastCheckTime,5000L)) {
    if ((activeSector || setorSlave) && (masterPump || bombaSlave))
      irrigTimeToday += (millis() - lastCheckTime)/1000;
    lastCheckTime = millis();

    if (NTPHour()==4 && irrigTimeToday!=0)
      irrigTimeToday = 0;

    writeToThingspeak();
  }



  //--------------------- auto-desbloqueio de 24 horas
  if (progFuncionamento == 2 && millis() >= bloqueioAte) {
    progFuncionamento = 0;
    bloqueioAte = 0;
    gravaConfig();
  }

  //--------------------- dispara programa
  static byte lastCheck = 0;

  if (lastCheck != NTPMinute()) {
    lastCheck = NTPMinute();
    for (byte i=0; i<NUMPROGS; i++) {
      bool temSetor = false;
      for (byte j = 0;j<NUMSETORES;j++)
        if (progTempoSetor[i][j]>0)
          temSetor = true;
          
      String subject;
      String body;

      if (temSetor && progDiaSemana[i][NTPDayOfWeek()]) {
        if (progHora[i] == NTPHour() &&
            progMin[i]  == NTPMinute()) {
          subject = "programa " + String(i+1);
          // 0=automático, 1=alta(cancela sempre), 2=baixa(nunca cancela)
          bool skipByForecast;
          switch (progPrevisao) {
            case 1:  skipByForecast = true; break;
            case 2:  skipByForecast = false; break;
            default: skipByForecast = (rainForecast > 5.0f); break;
          }

          if (!skipByForecast && progFuncionamento==0) {
            // programa completo
            Serial.print("Iniciando programa ");
            Serial.println(i+1);
            subject += " - " + programa_(NULL, 0,i+1);
            body = subject + "  (rainForecast=" + String(rainForecast,1) + ")<br>";
            if (progModoBomba==0 || progModoBomba==2)
              body += "Bomba utilizada: poço";
            else
              body += "Bomba utilizada: caixa";
            for (byte j=0;j<NUMSETORES;j++) {
              if (progTempoSetor[i][j]) {
                body += "<br>Setor " + String(j+1)+": ";
                float tempo = progTempoSetor[i][j]/2.;
                body += String(tempo);
                body += " min";
              }
            }
          }
          else if (skipByForecast && progFuncionamento==0) {
            // dispensado por previsão - verifica setores cobertos
            bool temCoberto = false;
            for (byte k=0;k<NUMSETORES;k++)
              if (progTempoSetor[i][k]>0 && progCoberto[k])
                temCoberto = true;
            if (temCoberto) {
              // executa apenas setores cobertos
              byte backup[NUMSETORES];
              for (byte k=0;k<NUMSETORES;k++) {
                backup[k] = progTempoSetor[i][k];
                if (!progCoberto[k]) progTempoSetor[i][k] = 0;
              }
              Serial.print("Iniciando programa (só cobertos) ");
              Serial.println(i+1);
              subject += " - cobertos - " + programa_(NULL, 0,i+1);
              for (byte k=0;k<NUMSETORES;k++) progTempoSetor[i][k] = backup[k];
              body = subject + "  (apenas cobertos, rainForecast=" + String(rainForecast,1) + ")<br>";
              for (byte j=0;j<NUMSETORES;j++) {
                if (progTempoSetor[i][j] && progCoberto[j]) {
                  body += "<br>Setor " + String(j+1)+": ";
                  body += String(progTempoSetor[i][j]/2.);
                  body += " min (coberto)";
                }
              }
            } else {
              subject += " dispensado";
              const char* motivo = progPrevisao==1 ? "previsão: alta (fixo)" : "previsão automática";
              body = subject + "  (" + motivo + ", rainForecast=" + String(rainForecast,1) + ")";
            }
          }
          else {
            // bloqueado por progFuncionamento
            subject += " dispensado";
            body = subject + "  (funcionamento bloqueado)";
          }
          enviarEmail(subject, body);
          break;
        }
      }
    }
  }


            
  //--------------------------------------------- Controle led do sensor de fluxo e atualização do OLED
  waterFlow = digitalRead(PIN_WATER_FLOW);
  digitalWrite(PIN_WATER_FLOW_LED, waterFlow);
  ledAsWifiStatus();
  oledHandle();

  ledAsWifiStatus();
 
}
