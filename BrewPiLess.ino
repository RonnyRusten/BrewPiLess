#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <Hash.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "espconfig.h"
//{ brewpi
#include <OneWire.h>

#include "Ticks.h"
#include "Display.h"
#include "TempControl.h"
#include "PiLink.h"
#include "Menu.h"
#include "Pins.h"
#include "RotaryEncoder.h"
#include "Buzzer.h"
#include "TempSensor.h"
#include "TempSensorMock.h"
#include "TempSensorExternal.h"
#include "Ticks.h"
#include "Sensor.h"
#include "SettingsManager.h"
#include "EepromFormat.h"

#if BREWPI_SIMULATE
#include "Simulator.h"
#endif

//}brewpi

#include "espconfig.h"

#include <Ticker.h>
#include "SNTPTime.h"
#include "SNTPClock.h"
#include "BrewKeeper.h"
#ifdef GSLOGGING
#include "GSLogger.h"
#endif

extern "C" {
#include <sntp.h>
}


#include "ESPUpdateServer.h"
#include "WifiSetup.h"
#include "AsyncServerSideEvent.h"
#include "BrewPiProxy.h"



/**************************************************************************************/
/* Start of Configuration 															  */
/**************************************************************************************/
static const char DefaultConfiguration[] PROGMEM =
R"END(
{"name":"brewpi",
"user":"brewpi",
"pass":"brewpi",
"protect":0
}
)END";

static const char* configFormat =
R"END(
{"name":"%s",
"user":"%s",
"pass":"%s",
"protect":%d
}
)END";


#define MAX_CONFIG_LEN 1024
#define JSON_BUFFER_SIZE 1024



#define PROFILE_FILENAME "/brewing.json"
#define CONFIG_FILENAME "/brewpi.cfg"

#define SSE_PATH 		"/getline"
#define POLLING_PATH 	"/getline_p"
#define PUTLINE_PATH	"/putline"


#ifdef GSLOGGING
#define LOGGING_PATH	"/log"
#endif

#define CONFIG_PATH		"/config"
#define TIME_PATH       "/time"

#define FPUTS_PATH       "/fputs"
#define FLIST_PATH       "/list"
#define DELETE_PATH       "/rm"

#define DEFAULT_INDEX_FILE     "index.htm"

const char *public_list[]={
"/bwf.js"
};
//*******************************************

bool passwordLcd;
char username[32];
char password[32];
char hostnetworkname[32];
AsyncWebServer server(80);
BrewPiProxy brewPi;
BrewKeeper brewKeeper([](const char* str){ brewPi.putLine(str);});
#ifdef GSLOGGING
GSLogger gslogger;
#endif

AsyncServerSideEventServer sse(SSE_PATH);

const char *confightml=R"END(
<html><head><title>Configuration</title></head><body>
<form action="" method="post">
<table>
<tr><td>Host/Network Name</td><td><input name="name" type="text" size="12" maxlength="16" value="%s"></td></tr>
<tr><td>User Name</td><td><input name="user" type="text" size="12" maxlength="16" value="%s"></td></tr>
<tr><td>Password</td><td><input name="pass" type="password" size="12" maxlength="16" value="%s"></td></tr>
<tr><td>Always need password</td><td><input type="checkbox" name="protect" value="yes" %s></td></tr>
<tr><td>Save Change</td><td><input type="submit" name="submit"></input></td></tr>
</table></form></body></html>)END";

const char *saveconfightml=R"END(
<html><head><title>Configuration Saved</title>
function r(){setTimeout(function(){window.location.reload();},15000)}
</head><body onload=r()>
Configuration Saved. Wait for restart...
</body></html>)END";


void requestRestart(void);
//Externals in SNTPClock.cpp
extern SNTPClock Clock;

void initTime(void)
{
	sntp_init();
  	sntp_setservername(0, (char*)"time.nist.gov");
  	sntp_setservername(1, (char*)"time.windows.com");
  	sntp_setservername(2, (char*)"de.pool.ntp.org");

  	sntp_set_timezone(0);
  	Clock.begin("time.nist.gov", 3600, 1);
}

class BrewPiWebHandler: public AsyncWebHandler 
{
	void handleFileList(AsyncWebServerRequest *request) {
		if(request->hasParam("dir",true)){
        	String path = request->getParam("dir",true)->value();
          	Dir dir = SPIFFS.openDir(path);
          	path = String();
          	String output = "[";
          	while(dir.next()){
            	File entry = dir.openFile("r");
            	if (output != "[") output += ',';
            		bool isDir = false;
            		output += "{\"type\":\"";
            		output += (isDir)?"dir":"file";
            		output += "\",\"name\":\"";
            		output += String(entry.name()).substring(1);
            		output += "\"}";
            		entry.close();
          	}
          	output += "]";
          	request->send(200, "text/json", output);
          	output = String();
        }
        else
          request->send(400);
	}
	
	void handleFileDelete(AsyncWebServerRequest *request){
		if(request->hasParam("path", true)){
        	ESP.wdtDisable(); SPIFFS.remove(request->getParam("path", true)->value()); ESP.wdtEnable(10);
            request->send(200, "", "DELETE: "+request->getParam("path", true)->value());
        } else
          request->send(404);
    }

	void handleFilePuts(AsyncWebServerRequest *request){
		if(request->hasParam("path", true)
			&& request->hasParam("content", true)){
        	ESP.wdtDisable(); 
    		String file=request->getParam("path", true)->value();
    		File fh= SPIFFS.open(file, "w");
    		if(!fh){
    			request->send(500);
    			return;
    		}
    		String c=request->getParam("content", true)->value();
      		fh.print(c.c_str());
      		fh.close();
        	ESP.wdtEnable(10);
            request->send(200);
            DBG_PRINTF("fputs path=%s\n",file.c_str());
            if(file == PROFILE_FILENAME){
	            DBG_PRINTF("reload file\n");
            	brewKeeper.reloadProfile();
            }
        } else
          request->send(404);
    }

public:
	BrewPiWebHandler(void){}

	void handleRequest(AsyncWebServerRequest *request){
	 	if(request->method() == HTTP_GET && request->url() == POLLING_PATH) {
	 		char *line=brewPi.getLastLine();
	 		if(line[0]!=0) request->send(200, "text/plain", line);
	 		else request->send(200, "text/plain;", "");
	 	}else if(request->method() == HTTP_POST && request->url() == PUTLINE_PATH){
	 		String data=request->getParam("data", true, false)->value();
	 		DBG_PRINTF("putline:%s\n",data.c_str());

	 		brewPi.putLine(data.c_str());
	 		request->send(200);
	 	}else if(request->method() == HTTP_GET && request->url() == CONFIG_PATH){
	 	    if(!request->authenticate(username, password))
	        return request->requestAuthentication();

			AsyncResponseStream *response = request->beginResponseStream("text/html");
			response->printf(confightml,hostnetworkname,username,password,(passwordLcd? "checked=\"checked\"":" "));
			request->send(response);
	 	}else if(request->method() == HTTP_POST && request->url() == CONFIG_PATH){
	 	    if(!request->authenticate(username, password))
	        return request->requestAuthentication();
			
			if(request->hasParam("name", true) 
					&& request->hasParam("user", true)
  					&& request->hasParam("pass", true)){
  				AsyncWebParameter* name = request->getParam("name", true);
  				AsyncWebParameter* user = request->getParam("user", true);
  				AsyncWebParameter* pass = request->getParam("pass", true);
  				
  				File config=SPIFFS.open(CONFIG_FILENAME,"w+");
  				
  				if(!config){
  					request->send(500);
  					return;
  				}
  				
  				int protect = 0;
  				if(request->hasParam("protect", true)) protect=1;
  				
  				config.printf(configFormat,name->value().c_str(),user->value().c_str(),pass->value().c_str(),protect);
  				config.close();
				request->send(200,"text/html",saveconfightml);
				requestRestart();
  			}else{
	  			request->send(400);
  			}
	 	}else if(request->method() == HTTP_GET &&  request->url() == TIME_PATH){

			AsyncResponseStream *response = request->beginResponseStream("application/json");
			response->printf("{\"t\":\"%s\"}",Clock.getDateTimeStr());
			request->send(response);
	 	
	 	}else if(request->method() == HTTP_POST &&  request->url() == FLIST_PATH){
	 	    if(!request->authenticate(username, password))
	        return request->requestAuthentication();
	 	
			handleFileList(request);
	 	}else if(request->method() == HTTP_DELETE &&  request->url() == DELETE_PATH){
	 	    if(!request->authenticate(username, password))
	        return request->requestAuthentication();
	 	
			handleFileDelete(request);
	 	}else if(request->method() == HTTP_POST &&  request->url() == FPUTS_PATH){
	 	    if(!request->authenticate(username, password))
	        return request->requestAuthentication();
	 	
			handleFilePuts(request);
			
	 	#ifdef GSLOGGING
	 	}else if (request->url() == LOGGING_PATH){
	 		if(request->method() == HTTP_POST){
		 		gslogger.updateSetting(request);
	 		}else{
	 			gslogger.getSettings(request);
	 		}	 		
	 	#endif	
	 	}else if(request->method() == HTTP_GET){
	 	
			String path=request->url();
	 		if(path.endsWith("/")) path +=DEFAULT_INDEX_FILE;

	 		if(request->url().equals("/")){
		 		if(!passwordLcd){
		 			request->send(SPIFFS, path);	 		
		 			return;
		 		}
		 	}
			bool auth=true;
			
			for(byte i=0;i< sizeof(public_list)/sizeof(const char*);i++){
				if(path.equals(public_list[i])){
						auth=false;
						break;
					}
			}
			
	 	    if(auth && !request->authenticate(username, password))
	        return request->requestAuthentication();
	        
	 		request->send(SPIFFS, path);
		}
	 }
	 
	bool canHandle(AsyncWebServerRequest *request){
	 	if(request->method() == HTTP_GET){
	 		if(request->url() == POLLING_PATH || request->url() == CONFIG_PATH || request->url() == TIME_PATH 
	 		#ifdef GSLOGGING
	 		|| request->url() == LOGGING_PATH
	 		#endif
	 		){
	 			return true;
			}else{
				// get file
				String path=request->url();
	 			if(path.endsWith("/")) path +=DEFAULT_INDEX_FILE;
				if(SPIFFS.exists(path)) return true;
			}
	 	}else if(request->method() == HTTP_DELETE && request->url() == DELETE_PATH){
				return true;
	 	}else if(request->method() == HTTP_POST){
	 		if(request->url() == PUTLINE_PATH || request->url() == CONFIG_PATH 
	 			|| request->url() ==  FPUTS_PATH || request->url() == FLIST_PATH 
	 			#ifdef GSLOGGING
	 			|| request->url() == LOGGING_PATH
	 			#endif
	 			)
	 			return true;	 	
		}
		return false;
	 }	 
};

BrewPiWebHandler brewPiWebHandler;

uint8_t clientCount;
void sseEventHandler(AsyncServerSideEventServer * server, AsyncServerSideEventClient * client, SseEventType type)
{
//	DBG_PRINTF("eventHandler type:\n");
//	DBG_PRINTF(type);
	
	if(type ==SseEventConnected){

		clientCount ++;
		DBG_PRINTF("***New connection, current client number:%d\n",clientCount);

		char *line=brewPi.getLastLine();
		
		if(line[0]!='\0') client->sendData(line);
		else client->sendData("");
	}else if (type ==SseEventDisconnected){
		clientCount --;
		DBG_PRINTF("***Disconnected, current client number:%d\n",clientCount);
	}
}

void stringAvailable(const char *str)
{
	sse.broadcastData(str);
}

//{brewpi


// global class objects static and defined in class cpp and h files

// instantiate and configure the sensors, actuators and controllers we want to use


/* Configure the counter and delay timer. The actual type of these will vary depending upon the environment.
* They are non-virtual to keep code size minimal, so typedefs and preprocessing are used to select the actual compile-time type used. */
TicksImpl ticks = TicksImpl(TICKS_IMPL_CONFIG);
DelayImpl wait = DelayImpl(DELAY_IMPL_CONFIG);

DisplayType realDisplay;
DisplayType DISPLAY_REF display = realDisplay;

ValueActuator alarm;

#ifdef ESP8266_WiFi


WiFiServer server(23);
WiFiClient serverClient;
#endif
void handleReset()
{
#if defined(ESP8266)
	// The asm volatile method doesn't work on ESP8266. Instead, use ESP.restart
	ESP.restart();
#else
	// resetting using the watchdog timer (which is a full reset of all registers) 
	// might not be compatible with old Arduino bootloaders. jumping to 0 is safer.
	asm volatile ("  jmp 0");
#endif
}


void brewpi_setup()
{

#if defined(ESP8266)
	// We need to initialize the EEPROM on ESP8266
	EEPROM.begin(MAX_EEPROM_SIZE_LIMIT);
	eepromAccess.set_manual_commit(false); // TODO - Move this where it should actually belong (a class constructor)
#endif

#if BREWPI_BUZZER	
	buzzer.init();
	buzzer.beep(2, 500);
#endif	

	piLink.init();

	logDebug("started");
	tempControl.init();
	settingsManager.loadSettings();

#if BREWPI_SIMULATE
	simulator.step();
	// initialize the filters with the assigned initial temp value
	tempControl.beerSensor->init();
	tempControl.fridgeSensor->init();
#endif	

	display.init();
	display.printStationaryText();
	display.printState();

	rotaryEncoder.init();

	logDebug("init complete");
}

void brewpiLoop(void)
{
	static unsigned long lastUpdate = 0;
	uint8_t oldState;

	if (ticks.millis() - lastUpdate >= (1000)) { //update settings every second
		lastUpdate = ticks.millis();

#if BREWPI_BUZZER
		buzzer.setActive(alarm.isActive() && !buzzer.isActive());
#endif			

		tempControl.updateTemperatures();
		tempControl.detectPeaks();
		tempControl.updatePID();
		oldState = tempControl.getState();
		tempControl.updateState();
		if (oldState != tempControl.getState()) {
			piLink.printTemperatures(); // add a data point at every state transition
		}
		tempControl.updateOutputs();

#if BREWPI_MENU
		if (rotaryEncoder.pushed()) {
			rotaryEncoder.resetPushed();
			menu.pickSettingToChange();
		}
#endif

		// update the lcd for the chamber being displayed
		display.printState();
		display.printAllTemperatures();
		display.printMode();
		display.updateBacklight();
	}

	//listen for incoming serial connections while waiting to update
#ifdef ESP8266_WiFi
	yield();
	connectClients();
	yield();
#endif
	piLink.receive();

}

//}brewpi

unsigned long _time;
byte _wifiState;
#define WiFiStateConnected 0
#define WiFiStateWaitToConnect 1
#define WiFiStateConnecting 2

#define SystemStateOperating 0
#define SystemStateRestartPending 1
#define SystemStateWaitRestart 2

byte _systemState=SystemStateOperating;

void requestRestart(void)
{
	_systemState =SystemStateRestartPending;
}

#define IS_RESTARTING (_systemState!=SystemStateOperating)

#define TIME_WAIT_TO_CONNECT 10000
#define TIME_RECONNECT_TIMEOUT 10000
#define TIME_RESTART_TIMEOUT 3000

void setup(void){
	_wifiState=WiFiStateConnected;
	
	#if SerialDebug == true
  	DebugPort.begin(115200);
  	DBG_PRINTF("\n");
  	DebugPort.setDebugOutput(true);
  	#endif
 
	//0.Initialize file system
	//start SPI Filesystem
  	if(!SPIFFS.begin()){
  		// TO DO: what to do?
  		DBG_PRINTF("SPIFFS.being() failed");
  	}else{
  		DBG_PRINTF("SPIFFS.being() Success");
  	}
	// try open configuration
	char configBuf[MAX_CONFIG_LEN];
	File config=SPIFFS.open(CONFIG_FILENAME,"r+");

	DynamicJsonBuffer jsonBuffer(JSON_BUFFER_SIZE);

	if(config){
		size_t len=config.readBytes(configBuf,MAX_CONFIG_LEN);
		configBuf[len]='\0';
	}

	JsonObject& root = jsonBuffer.parseObject(configBuf);
	const char* host;
	
	if(!config
			|| !root.success()
			|| !root.containsKey("name")
			|| !root.containsKey("user")
			|| !root.containsKey("pass")){
		
		strcpy_P(configBuf,DefaultConfiguration);
		JsonObject& root = jsonBuffer.parseObject(configBuf);
  		strcpy(hostnetworkname,root["name"]);
  		strcpy(username,root["user"]);
  		strcpy(password,root["pass"]);
		passwordLcd=(bool)root["protect"];
		
	}else{
		config.close();
		  	
  		strcpy(hostnetworkname,root["name"]);
  		strcpy(username,root["user"]);
  		strcpy(password,root["pass"]);
  		passwordLcd=(root.containsKey("protect"))? (bool)(root["protect"]):false;
  	}
	#ifdef GSLOGGING
  	gslogger.loadConfig();
  	#endif
	//1. Start WiFi 
	WiFiSetup::begin(hostnetworkname);

	// get time
	initTime();

  	DBG_PRINTF("Connected!");

	if (!MDNS.begin(hostnetworkname)) {
		DBG_PRINTF("Error setting mDNS responder");
	}	
	MDNS.addService("http", "tcp", 80);

	// TODO: SSDP responder

	
	//3. setup Web Server

	//3.1 Normal serving pages 
	//3.1.1 status report through SSE
	sse.onEvent(sseEventHandler);
	server.addHandler(&sse);

	server.addHandler(&brewPiWebHandler);
	//3.1.2 SPIFFS is part of the serving pages
	//server.serveStatic("/", SPIFFS, "/","public, max-age=259200"); // 3 days

    
	server.on("/fs",[](AsyncWebServerRequest *request){
		FSInfo fs_info;
		SPIFFS.info(fs_info);
		request->send(200,"","totalBytes:" +String(fs_info.totalBytes) +
		" usedBytes:" + String(fs_info.usedBytes)+" blockSize:" + String(fs_info.blockSize)
		+" pageSize:" + String(fs_info.pageSize));
		//testSPIFFS();
	});
	
	// 404 NOT found.
  	//called when the url is not defined here
	server.onNotFound([](AsyncWebServerRequest *request){
		request->send(404);
	});
	
	//4. start Web server
	server.begin();
	DBG_PRINTF("HTTP server started\n");

	
	// 5. try to connnect Arduino
  	brewPi.begin(stringAvailable);

	brewKeeper.setFile(PROFILE_FILENAME);
	
	// 6. start WEB update pages.	
#if (DEVELOPMENT_OTA == true) || (DEVELOPMENT_FILEMANAGER == true)
	ESPUpdateServer_setup(username,password);
#endif

	brewpi_setup();
}



void loop(void){
//{brewpi
#if BREWPI_SIMULATE
	simulateLoop();
#else
	brewpiLoop();
#endif
//}brewpi
#if (DEVELOPMENT_OTA == true) || (DEVELOPMENT_FILEMANAGER == true)
	ESPUpdateServer_loop();
#endif
	time_t now=Clock.getTimeSeconds();
	
	char unit, mode;
	float beerSet,fridgeSet;
	brewPi.getControlParameter(&unit,&mode,&beerSet,&fridgeSet);
  	brewKeeper.keep(now,unit,mode,beerSet);
  	
  	brewPi.loop();
 	
 	#ifdef GSLOGGING

 	gslogger.loop(now,[](float *pBeerTemp,float *pBeerSet,float *pFridgeTemp, float *pFridgeSet){
 			brewPi.getTemperature(pBeerTemp,pBeerSet,pFridgeTemp,pFridgeSet);
 		});
 	#endif
 	if(WiFi.status() != WL_CONNECTED && !IS_RESTARTING)
 	{
 		if(_wifiState==WiFiStateConnected)
 		{
			_time=millis();
			_wifiState = WiFiStateWaitToConnect;
		}
		else if(_wifiState==WiFiStateWaitToConnect)
		{
			if((millis() - _time) > TIME_WAIT_TO_CONNECT)
			{
				WiFi.begin();
				_time=millis();
				_wifiState = WiFiStateConnecting;
			}
		}
		else if(_wifiState==WiFiStateConnecting)
		{
			if((millis() - _time) > TIME_RECONNECT_TIMEOUT){
				_time=millis();
				_wifiState = WiFiStateWaitToConnect;
			}
		}
 	}
 	else
 	{
 		_wifiState=WiFiStateConnected;
  	}
  	
  	if(_systemState ==SystemStateRestartPending){
	  	_time=millis();
	  	_systemState =SystemStateWaitRestart;
  	}else if(_systemState ==SystemStateWaitRestart){
  		if((millis() - _time) > TIME_RESTART_TIMEOUT){
  			ESP.restart();
  		}
  	}
}







