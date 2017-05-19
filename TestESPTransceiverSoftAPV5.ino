/**
 * This version adds sd card
 */
#include "debugmacros.h"
#include "Arduino.h"
#include "ESP8266WiFi.h"
#include "WiFiClient.h"
#include "ESP8266WebServer.h"
#include "WiFiUdp.h"
#include "SocketData.h"
#include <FS.h>
#include "Properties.h"
#include "SD.h"
#include "SPI.h"
#include "ClientManager.h"


//tcp/ip comm attributes
#define ICACHE_RAM_ATTR     __attribute__((section(".iram.text")))//required for timer
#define TIMER_DELAY 45  //19.25khz //46 for 22050 (1,000,000/22050)
//#define TIMER_DELAY 500000// 1 sec
#define INBUFSIZE 2920
#define OUTBUFSIZE 2920
#define DEFAULT_SOFT_AP_SSID "ESPAP"
//#define MAX_REC_LENGTH 500000
#define CONFIG_FILE "/config.txt"
#define NOBODY_RESPONDING "nobody_responding.raw"
#define THANK_YOU "thank_you.raw"
#define NOT_AT_HOME "not_at_home.raw"
#define PLEASE_WAIT "please_wait.raw"
#define REQUEST_SOURCE_PHONE "phone"
#define REQUEST_SOURCE_NOTIFY "notify"
#define REQUEST_SOURCE_NOTIFY_STOP "stop_notify"



#define NOTIFY_GPIO_PIN D3//D4 of nodemcu
#define PAIR_GPIO_PIN D4//D3 of nodemcu

#define NOTIFY_ARDUINO_PIN D1//D6 of nodemcu
#define OPEN_DOOR_PIN D5//D7 of nodemcu
#define CLOSE_DOOR_PIN D0//D8 of nodemcu
#define SD_CARD_CHIP_SELECT D2


//const char* ssid = "debojit-dlink";
//const char* password = "India@123";

char responseBuffer[256];       // a string to send back
char *longResponseBuffer=NULL;       // a string to send back

const char* host="192.168.0.104";
const char * delim=".:";

uint32_t maxRecLen=500000;
int availableForWrite=0;
uint32_t recordLength=0;


boolean notifyRequested=false;
boolean notifyFlowInProgress=false;

boolean pairingRequested=false;
boolean pairingFlowInProgress=false;

IPAddress apIP(192, 168, 4, 1);//way to set gateway ip manually

boolean sdCardPresent=false;
boolean playingFromSD=false;

boolean recordInProgress=false;
boolean playbackInProgress=false;
boolean restoreInProgress=false;
boolean recordFromMicInProgress=false;
boolean someHeavyProcessingInProgress=false;
boolean initiateMessageRecordingfromMic=false;//part of workflow

char playRequestSource[10];//values can be notify, phone
char recordRequestSource[10];//values can be notify, phone
char fileNameBuffer[30], fileNameBuffer1[30];

SDFileWrapper recordingFile;
SDFileWrapper recordingFileFromMic;
SDFileWrapper playingFileFromSD;
SDFileWrapper msgsDir;

File playingFileFromFlash;

Properties properties(10);

uint32_t currentTimeStamp=0, previousTimeStamp=0;
ClientManager clientManager;
WSClientWrapper *heavyProcessResponseClient;
WSClientWrapper *currentWSClient;


uint8_t outBuffer[OUTBUFSIZE];
uint8_t inBuffer[INBUFSIZE];
boolean tcpIpSocketConnected=false;
boolean tcpIpCommStarted=false;

char commands[][30]={
		"UDP_PAIR_BROADCAST",
		"UDP_CONNECT_BC_REQUEST",
		"UDP_CONNECT_BC_RESPONSE",
		"UDP_CONNECT_BC_RETRY",
		"UDP_CONNECT_BC_STARTHB",
		"START_COMM",
		"STOP_COMM",
		"START_RECORD",
		"STOP_RECORD",
		"MIC_RECORD_START",
		"MIC_RECORD_STOP",
		"START_PLAY",
		"STOP_PLAY",
		"RESET",
		"NOTIFY",
		"HELLO",
		"OPEN_DOOR",
		"CLOSE_DOOR",
		"SAVE_CONFIG",
		"LOAD_CONFIG",
		"DELETE_FILE",
		"GET_MESSAGES",
		"FORMAT",
		"FREE_SPACE",
		"SD_WRITE_TEST",
		"TEST_NOTIFY",
		"RESTORE"
};


void setup(){
	Serial.begin(1000000);//20usec/byte
	//Serial.begin(500000);
	INFO_PRINTLN("\nStarting TestESPTransceiverSoftAPV5...");
	INFO_PRINTLN("Configuring SPI flash");

	SPIFFS.begin();
	setupSDCard();
	readConfigFileAndConfigureWifi();
	//starting up clientManager
	clientManager.setup(processIncomingUDPCommands);
	//setupNotifyGPIO();
	setupPairingGPIO();
	setupDoorControl();
	setupNotifyArduino();
	setupTimer1();
	currentTimeStamp=previousTimeStamp=millis();
}



void setupSDCard(){
	DEBUG_PRINTLN("Initializing SD card...");

	if (!SD.begin(SD_CARD_CHIP_SELECT)) {
		DEBUG_PRINTLN("SD card initialization failed!");
		return;
	}
	sdCardPresent=true;
	DEBUG_PRINTLN("sd card initialization done.");

	recordingFileFromMic=SD.open("testsd.txt", FILE_WRITE_NO_APPEND);
	if(recordingFileFromMic){
		DEBUG_PRINTF("File Open Success: %s\n",recordingFileFromMic.name());
		recordingFileFromMic.println("testing file write");
		recordingFileFromMic.close();
		SD.remove("testsd.txt");
	}else{
		DEBUG_PRINTLN("File open failed, restarting...");
		delay(1000);
		ESP.restart();
	}
}

void readConfigFileAndConfigureWifi(){
	properties.load(CONFIG_FILE);
	properties.print();
	if(properties.getCurrentSize()>0){
		String apMode=properties.get("mode");
		String softapSsid=properties.get("softap_ssid");
		INFO_PRINTLN("AP mode is: "+apMode);
		INFO_PRINTLN("softapSsid: "+softapSsid);
		//String maxRecLenStr=properties.get("max_rec_len");
		//maxRecLen=maxRecLenStr.toInt();
		//PRINTLN("maxRecLen: "+maxRecLen);
		if(softapSsid.equals("")){
			softapSsid=DEFAULT_SOFT_AP_SSID;
		}
		if(apMode.equals("softap")){
			setupEspRadioAsSoftAP(softapSsid.c_str());
		}else{
			String ssid=properties.get("station_ssid");
			String password=properties.get("station_pwd");
			setupEspRadioAsStation(softapSsid.c_str(),ssid.c_str(), password.c_str());
		}
		String deviceId=properties.get("device_id");
		if(deviceId.length()>0){
			clientManager.setDeviceId((char *)deviceId.c_str());
		}
	}else{
		//properties.put("mode","softap");
		properties.put("mode","station");
		properties.put("softap_ssid","ESPAP");
		properties.put("station_ssid","debojit-dlink");
		properties.put("station_pwd","India@123");
		properties.put("not_at_home","false");
		properties.put("max_rec_len","200000");
		properties.put("device_id",DEFAULT_DEVICE_ID);

		properties.store(CONFIG_FILE);
		INFO_PRINTLN("Properties file not found, creating default, restarting");
		delay(1000);
		ESP.restart();
	}
}


void setupEspRadioAsSoftAP(const char *ssid){
	yield();
	INFO_PRINTLN("Configuring ESP radio access point as SOFT_AP mode...");
	WiFi.mode(WIFI_AP);//Access point mode only
	/* You can remove the password parameter if you want the AP to be open. */
	WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));//use to set custom IP
	WiFi.softAP(ssid);//starting the soft ap

	IPAddress myIP = WiFi.softAPIP();//get the soft AP IP
	INFO_PRINT("AP IP address: ");INFO_PRINTLN(myIP);
	//!!VVI!!
	WiFi.disconnect();//disconnect the wifi so that it does not search for wifi host to connect to!
	//if it is not disconnected it would scan for wifi hosts and reduce performance.
#ifdef INFO
	WiFi.printDiag(Serial);//printing wifi details
	INFO_PRINTLN("#################");
#endif
}

void setupEspRadioAsStation(const char *softAPSsid, const char *ssid, const char * password){
	INFO_PRINTF("Connecting to %s-%s\nConfiguring access point...", ssid, password);
	/* You can remove the password parameter if you want the AP to be open. */
	WiFi.softAP(softAPSsid);
	IPAddress myIP = WiFi.softAPIP();
	INFO_PRINT("AP IP address: ");DEBUG_PRINTLN(myIP);
	WiFi.begin(ssid, password);
	uint32_t delayMs=10000+millis();
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
#ifdef INFO
		INFO_PRINT(".");
#endif
		if(delayMs<millis()){
			INFO_PRINTF("Could not connect to ssid: %s", ssid);
			WiFi.disconnect();
			break;
		}
	}
#ifdef INFO
	INFO_PRINT("WiFi ip is: ");INFO_PRINTLN(WiFi.localIP());
	INFO_PRINTLN("#################");
	WiFi.printDiag(Serial);
	INFO_PRINTLN("setup done");
#endif
}


boolean hasIncomingNotificationRequest(){
	if(notifyRequested ){
		notifyRequested=false;
		if(notifyFlowInProgress || pairingFlowInProgress || someHeavyProcessingInProgress){
			return false;
		}
		notifyFlowInProgress=true;
		return true;
	}else{
		return false;
	}
}

boolean hasIncomingPairingRequest(){
	if(pairingRequested ){
		pairingRequested=false;
		if(notifyFlowInProgress || pairingFlowInProgress || someHeavyProcessingInProgress){
			return false;
		}
		pairingFlowInProgress=true;
		return true;
	}else{
		return false;
	}
}


void processNotify(){
	if(hasIncomingNotificationRequest()){
		DEBUG_PRINTLN("Notify requested, sending notify");
		//if out of home active then immediately play out of home message
		if(properties.get("not_at_home").equals("true")){
			startPlayback(NOT_AT_HOME,REQUEST_SOURCE_NOTIFY);
			notifyFlowInProgress=false;
		}else{
			//startPlayback(PLEASE_WAIT,REQUEST_SOURCE_NOTIFY);
			char *notifyResponse=clientManager.notify(20000);
			if(strlen(notifyResponse)>0){
				//server notified, await tcp/ip communication
				DEBUG_PRINTLN("server notified, await tcp/ip communication");
				if(strstr(notifyResponse, "NOTIFY:ACK")!=NULL){
					//notify accepted, do nothing
					notifyFlowInProgress=false;
					DEBUG_PRINTLN("notify accepted, await tcp/ip communication");
					//reenable notification
					setupNotifyGPIO();
				}else if(strstr(notifyResponse, "NOTIFY:NACK")!=NULL){
					//notification not accepted
					DEBUG_PRINTLN("notify not accepted, play NOBODY_RESPONDING");
					startPlayback(NOBODY_RESPONDING,REQUEST_SOURCE_NOTIFY);
				}
			}else{
				//recipient device is not found so start fallback plan
				//plan to provide a message and recording options on device.
				//1. startPlayback(NOBODY_RESPONDING)
				//2. bufferedPlaybackToSpeaker(), on completion of file read do next
				//3. startRecordFromMic(messageFileName),
				//4. bufferedRecordFromMic() does the recording, on MAX_RECORD_LEN condition do next
				//5. startPlayback(THANK_YOU,REQUEST_SOURCE_NOTIFY);
				DEBUG_PRINTLN("notification response not received, playing NOBODY_RESPONDING");
				startPlayback(NOBODY_RESPONDING,REQUEST_SOURCE_NOTIFY);
			}
		}
	}

}


void processPairing(){
	if(hasIncomingPairingRequest()){
		clientManager.pair();
		pairingFlowInProgress=false;
		DEBUG_PRINTLN("Reenabling pairing gpio");
		setupPairingGPIO();
	}
}

void loop(void){
	//process notify
	processNotify();
	//process pairing
	processPairing();

	if(!notifyFlowInProgress && !pairingFlowInProgress && clientManager.initializeUDPConnection()){
		//this call checks if the device has any data from any client via websocket call
		clientManager.update();
	}
	if(someHeavyProcessingInProgress){
		processTcpIpComm();
		recordMessageFromPhone();
		bufferedPlaybackToSpeaker();
		bufferedRecordFromMic();
		bufferedRestoreFromFlashToSD();
	}
}

void gpioNotifyInterrupt() {
	detachInterrupt(NOTIFY_GPIO_PIN);
	DEBUG_PRINTLN("Notified externally");
	notifyRequested=true;
}

void gpioPairingInterrupt() {
	detachInterrupt(PAIR_GPIO_PIN);
	DEBUG_PRINTLN("Pairing Initiated");
	pairingRequested=true;
}


void setupDoorControl(){
	INFO_PRINTLN("setupDoorControl called");
	pinMode(OPEN_DOOR_PIN, OUTPUT);
	pinMode(CLOSE_DOOR_PIN, OUTPUT);
	digitalWrite(OPEN_DOOR_PIN, LOW);
	digitalWrite(CLOSE_DOOR_PIN, LOW);
}
void setupNotifyGPIO(){
	INFO_PRINTLN("setupNotifyGPIO called");
	pinMode(BUILTIN_LED, OUTPUT);
	pinMode(NOTIFY_GPIO_PIN,INPUT);
	attachInterrupt(NOTIFY_GPIO_PIN, gpioNotifyInterrupt, CHANGE);
}

void setupPairingGPIO(){
	INFO_PRINTLN("setupPairingGPIO called");
	pinMode(PAIR_GPIO_PIN,INPUT);
	attachInterrupt(PAIR_GPIO_PIN, gpioPairingInterrupt, CHANGE);
}


static int arduinoNotified=0;
void setupNotifyArduino(){
	pinMode(NOTIFY_ARDUINO_PIN, OUTPUT);
	digitalWrite(BUILTIN_LED, arduinoNotified);
	digitalWrite(NOTIFY_ARDUINO_PIN, arduinoNotified);
}

void notifyArduino(int state){
	digitalWrite(NOTIFY_ARDUINO_PIN, state);
	digitalWrite(BUILTIN_LED, state);
}


void processIncomingUDPCommands(CommandData commandData, WSClientWrapper *wsClient){
	currentWSClient=wsClient;
#ifdef DEBUG
	getFreeHeap();
#endif
	if(strcmp(commands[START_COMM], commandData.command)==0){
		if(!tcpIpCommStarted){
			//preparing response
			strcpy(responseBuffer,commands[START_COMM]);
			if(openAudioChannel()){
				tcpIpCommStarted=true;
				enableTimer1();
				notifyArduino(HIGH);
				someHeavyProcessingInProgress=true;
				heavyProcessResponseClient=currentWSClient;
				strcat(responseBuffer,":ACK");
			}else{
				strcat(responseBuffer,":NACK:TCP/IP connection failed");
			}

		}else{
			strcat(responseBuffer,":NACK:TCP/IP communication already started");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commands[STOP_COMM], commandData.command)==0){
		strcpy(responseBuffer,commands[STOP_COMM]);
		if(tcpIpCommStarted){
			tcpIpCommStarted=false;
			closeAudioChannel();
			notifyArduino(LOW);
			disableTimer1();
			someHeavyProcessingInProgress=false;
			strcat(responseBuffer,":ACK");
		}else{
			strcat(responseBuffer,"NACK:TCP/IP communication already stopped");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commands[HELLO], commandData.command)==0){
		testFileWriteSD();
		strcpy(responseBuffer,commands[HELLO]);
		strcat(responseBuffer,":ACK:TestESPTransceiverSoftAPV5");
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commands[RESET], commandData.command)==0){
		strcpy(responseBuffer,commands[RESET]);
		strcat(responseBuffer,":ACK");
		clientManager.sendWSCommand(responseBuffer, wsClient);
		delay(1000);
		ESP.restart();
	}
	if(strcmp(commands[OPEN_DOOR], commandData.command)==0){
		digitalWrite(OPEN_DOOR_PIN, HIGH);
		strcpy(responseBuffer,commands[OPEN_DOOR]);
		strcat(responseBuffer,":ACK");
		clientManager.sendWSCommand(responseBuffer, wsClient);
		delay(500);
		digitalWrite(OPEN_DOOR_PIN,LOW);
	}
	if(strcmp(commands[CLOSE_DOOR], commandData.command)==0){
		digitalWrite(CLOSE_DOOR_PIN, HIGH);
		strcpy(responseBuffer,commands[CLOSE_DOOR]);
		strcat(responseBuffer,":ACK");
		clientManager.sendWSCommand(responseBuffer, wsClient);
		delay(500);
		digitalWrite(CLOSE_DOOR_PIN, LOW);
	}
	if(strcmp(commandData.command,commands[START_RECORD])==0){
		char * fileName=commandData.data;
		strcpy(responseBuffer,commands[START_RECORD]);
		if(sdCardPresent){
			if(!recordInProgress &&!someHeavyProcessingInProgress){
				if(openAudioChannel()){
					//recordingFile=initiateFileRecording(fileName);
					trimPath(fileName, fileNameBuffer, true);
					recordingFile=SD.open(fileNameBuffer, FILE_WRITE_NO_APPEND);
#ifdef DEBUG
					if(recordingFile){
						DEBUG_PRINT("File opened success: ");DEBUG_PRINTLN(fileNameBuffer);
					}else{
						DEBUG_PRINT("File opened failed: ");DEBUG_PRINTLN(fileNameBuffer);
					}
#endif
					if(recordingFile){
						strcat(responseBuffer,":ACK");
						recordInProgress=true;
						someHeavyProcessingInProgress=true;
						heavyProcessResponseClient=currentWSClient;
					}else{
						strcat(responseBuffer,":NACK:File open failed");
					}
				}
			}else{
				strcat(responseBuffer,":NACK:Communication going on");
			}
		}else{
			strcat(responseBuffer,":NACK:SD card not present");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[STOP_RECORD])==0){
		strcpy(responseBuffer,commands[STOP_RECORD]);
		if(sdCardPresent){
			if(recordInProgress){
				recordInProgress=false;
				someHeavyProcessingInProgress=false;
				closeAudioChannel();
				//stopFileProcessing(recordingFile);
				recordingFile.close();
				recordLength=0;
				strcat(responseBuffer,":ACK");
			}else{
				strcat(responseBuffer,":NACK:TCP/IP recording already stopped");
			}
		}else{
			strcat(responseBuffer,":NACK:SD card not present");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[START_PLAY])==0){
		const char * fileName=commandData.data;
		strcpy(responseBuffer,commands[START_PLAY]);
		if(startPlayback(fileName, REQUEST_SOURCE_PHONE)){
			strcat(responseBuffer,":ACK");
		}else{
			strcat(responseBuffer,":NACK:Playback already started");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[STOP_PLAY])==0){
		strcpy(responseBuffer,commands[STOP_PLAY]);
		if(stopPlayback()){
			strcat(responseBuffer,":ACK");
		}else{
			strcat(responseBuffer,":NACK:playback already stopped");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[MIC_RECORD_START])==0){
		const char * fileName=commandData.data;
		strcpy(responseBuffer,commands[MIC_RECORD_START]);
		if(sdCardPresent){
			if(startRecordFromMic(fileName, REQUEST_SOURCE_PHONE)){
				strcat(responseBuffer,":ACK");
			}else{
				strcat(responseBuffer,":NACK:Record from mic already going on");
			}
		}else{
			strcat(responseBuffer,":NACK:SD card not present");

		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[MIC_RECORD_STOP])==0){
		strcpy(responseBuffer,commands[MIC_RECORD_STOP]);
		if(sdCardPresent){
			if(stopRecordFromMic()){
				strcat(responseBuffer,":ACK");
			}else{
				strcat(responseBuffer,":NACK:Record from mic already stopped");
			}
		}else{
			strcat(responseBuffer,":NACK:SD card not present");
		}

		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[SAVE_CONFIG])==0){
		const char * config=commandData.data;
		String str(config);
		properties.parsePropertiesAndPut(str);
		properties.print();
		properties.store(CONFIG_FILE);
		strcpy(responseBuffer,commands[SAVE_CONFIG]);
		strcat(responseBuffer,":ACK");
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[LOAD_CONFIG])==0){
		properties.print();
		properties.load(CONFIG_FILE);
		strcpy(responseBuffer,commands[LOAD_CONFIG]);
		strcat(responseBuffer,":ACK:");
		strcat(responseBuffer,properties.serialize().c_str());
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[DELETE_FILE])==0){
		char * fileName=commandData.data;
		strcpy(responseBuffer,commands[DELETE_FILE]);
		trimPath(fileName, fileNameBuffer, false);
		if(SD.remove(fileNameBuffer)){
			strcat(responseBuffer,":ACK");
		}else{
			strcat(responseBuffer,":NACK:Cant delete file");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[FORMAT])==0){
		strcpy(responseBuffer,commands[FORMAT]);

		if(SPIFFS.format()){
			strcat(responseBuffer,":ACK");
			delay(1000);
			DEBUG_PRINTLN("restarting nodemcu");
			ESP.restart();
		}else{
			strcat(responseBuffer,":NACK:Cant format");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[FREE_SPACE])==0){
		strcpy(responseBuffer,commands[FREE_SPACE]);
#ifdef DEBUG
		FSInfo fs_info;
		SPIFFS.info(fs_info);
		DEBUG_PRINT("TotalBytes: ");DEBUG_PRINTLN(fs_info.totalBytes);
		DEBUG_PRINT("UsedBytes: ");DEBUG_PRINTLN(fs_info.usedBytes);
		DEBUG_PRINT("BlockSize: ");DEBUG_PRINTLN(fs_info.blockSize);
		DEBUG_PRINT("PageSize: ");DEBUG_PRINTLN(fs_info.pageSize);
		DEBUG_PRINT("maxOpenFiles: ");DEBUG_PRINTLN(fs_info.maxOpenFiles);
		DEBUG_PRINT("maxPathLength: ");DEBUG_PRINTLN(fs_info.maxPathLength);
#endif
		strcat(responseBuffer,":ACK");
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}

	if(strcmp(commandData.command,commands[GET_MESSAGES])==0){
		longResponseBuffer=new char[1024];
		strcpy(longResponseBuffer,commands[GET_MESSAGES]);
		strcat(longResponseBuffer,":ACK:");
		msgsDir=SD.open("messages");
		if(msgsDir){
			getMessagesList(msgsDir,longResponseBuffer);
		}else{
			strcat(longResponseBuffer, "messages folder not found");
		}
		clientManager.sendWSCommand(longResponseBuffer, wsClient);
		delete []longResponseBuffer;
	}
	if(strcmp(commandData.command,commands[SD_WRITE_TEST])==0){
		strcpy(responseBuffer,commands[SD_WRITE_TEST]);
		generateRandomFileName(fileNameBuffer, NULL);
		//trimFileName(fileNameBuffer, fileNameBuffer);
		recordingFileFromMic=SD.open(fileNameBuffer, FILE_WRITE_NO_APPEND);
		if(recordingFileFromMic){
			DEBUG_PRINTF("File Open Success: %s\n",recordingFileFromMic.name());
			recordingFileFromMic.println("testing file ");
			recordingFileFromMic.println(fileNameBuffer);
			recordingFileFromMic.close();
			strcat(responseBuffer,":ACK:File Open Success");
		}else{
			DEBUG_PRINTLN("File open failed");
			strcat(responseBuffer,":NACK:File open failed");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[TEST_NOTIFY])==0){
		strcpy(responseBuffer,commands[TEST_NOTIFY]);
		strcat(responseBuffer,":ACK:");
		notifyRequested=true;
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
	if(strcmp(commandData.command,commands[RESTORE])==0){
		char * fileName=commandData.data;
		strcpy(responseBuffer,commands[RESTORE]);
		if(sdCardPresent){
			if(!someHeavyProcessingInProgress){
				if(startRestoreFromFlashToSD(fileName)){
					strcat(responseBuffer,":ACK:Restore in progress");
				}else{
					strcat(responseBuffer,":NACK:Restore not possible");
				}
			}else{
				strcat(responseBuffer,":NACK:Other tasks going on");
			}
		}else{
			strcat(responseBuffer,":NACK:SD card not present, restore not possible");
		}
		clientManager.sendWSCommand(responseBuffer, wsClient);
	}
}

void getFreeHeap(){
	DEBUG_PRINT("Free heap is: ");DEBUG_PRINTLN(ESP.getFreeHeap());
}

//tcp ip tcpIpSocketConnected comm methods



void setupTimer1(){
	timer1_isr_init();
	disableTimer1();
}

void enableTimer1(){
	if(!timer1_enabled()){
		DEBUG_PRINTLN("Enabling timer1");
#ifdef DEBUG
		timer1_attachInterrupt(mockPcmSamplingISR);
#else
		timer1_attachInterrupt(pcmSamplingISR);
#endif
		timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
		uint32_t t1=clockCyclesPerMicrosecond() / 16 * TIMER_DELAY;
		DEBUG_PRINT("Counter: ");DEBUG_PRINTLN(t1);
		timer1_write(t1); //32us = 31.250kHz sampling freq
	}else{
		DEBUG_PRINTLN("Timer1 already enabled");
	}
}

void disableTimer1(){
	if(timer1_enabled()){
		DEBUG_PRINTLN("Disabling timer1");
		timer1_disable();
		timer1_detachInterrupt();
	}else{
		DEBUG_PRINTLN("Timer1 already disabled");
	}
}

#pragma GCC push_options
#pragma GCC optimize("O3")

//handles tcp/ip communication
boolean openAudioChannel(){
	if(clientManager.openAudioChannel()){
		tcpIpSocketConnected=false;
	}else{
		tcpIpSocketConnected=true;
	}
	return tcpIpSocketConnected;
}

void closeAudioChannel(){
	clientManager.closeAudioChannel();
	tcpIpSocketConnected=false;
}

void processTcpIpComm(){
	if(tcpIpSocketConnected){
		ESP.wdtDisable();
		processSocketBufferedRead();
		processSocketBufferedWrite();
		ESP.wdtEnable(0);
	}
}

uint8_t rbuffer[2][INBUFSIZE];
uint8_t rbufferState[2]={0,0};
int readBufferStart[2]={0,0};
int readBufferLast[2]={0,0};
uint8_t readBufferNextToRead=0;//0/1
uint8_t readBufferNextToFill=0;//0/1

uint8_t wbuffer[2][OUTBUFSIZE];
uint8_t wbufferState[2]={0,0};
int writeBufferStart[2]={0,0};
int writeBufferLast[2]={OUTBUFSIZE,OUTBUFSIZE};
uint8_t writeBufferNextToRead=0;//0/1
uint8_t writeBufferNextToFill=0;//0/1
uint8_t sample=0, prevSample=0;


void ICACHE_RAM_ATTR pcmSamplingISR(void){
	//NOTE: In ISR never use yield() or any function that calls yield, this results in resets.
	//thats why client.available is not used in this routine as it has a call to optimistic_yield()
	if(rbufferState[readBufferNextToRead]==1){
		if(readBufferStart[readBufferNextToRead]<readBufferLast[readBufferNextToRead]){
			//last position not reached
			Serial.write(rbuffer[readBufferNextToRead][(readBufferStart[readBufferNextToRead])]);//data at bufPos sent
			readBufferStart[readBufferNextToRead]++;//pointing to next pos
		}else{
			rbufferState[readBufferNextToRead]=0;
			readBufferNextToRead=(readBufferNextToRead==0)?1:0;
		}
	}
	if(wbufferState[writeBufferNextToFill]==0){
		if(writeBufferLast[writeBufferNextToFill]>writeBufferStart[writeBufferNextToFill]){
			sample=Serial.read();
			wbuffer[writeBufferNextToFill][(writeBufferStart[writeBufferNextToFill])]=((sample>=250 || sample<=5)?prevSample:sample);
			writeBufferStart[writeBufferNextToFill]++;
			prevSample=sample;
		}else{
			int temp=writeBufferNextToFill;
			writeBufferNextToFill=(writeBufferNextToFill==0)?1:0;
			wbufferState[temp]=1;
		}
	}

	//buffer writer code
}

//TODO clientManager.getAudioClient().client should be accessed via pointer locally instead of calling via member every time
void processSocketBufferedRead(){
	if(tcpIpSocketConnected && rbufferState[readBufferNextToFill]==0){
		int temp=readBufferNextToFill;
		readBufferLast[readBufferNextToFill]=clientManager.audioSocket.read(rbuffer[readBufferNextToFill],INBUFSIZE);
		readBufferStart[readBufferNextToFill]=0;
		readBufferNextToFill=(readBufferNextToFill==0)?1:0;
		rbufferState[temp]=1;
	}
}

void processSocketBufferedWrite(){//it is the reader of the buffer
	if(tcpIpSocketConnected && wbufferState[writeBufferNextToRead]==1){//ready for read
		clientManager.audioSocket.write((const uint8_t *)wbuffer[writeBufferNextToRead],writeBufferLast[writeBufferNextToRead]);
		writeBufferStart[writeBufferNextToRead]=0;
		wbufferState[writeBufferNextToRead]=0;//set state=0 indicating ready for write
		writeBufferNextToRead=(writeBufferNextToRead==0)?1:0;//switching to next buffer
	}
}

int mockWriteByteCounter=0;
int mockReadByteCounter=0;

#ifdef DEBUG
void ICACHE_RAM_ATTR mockPcmSamplingISR(void){
	if(rbufferState[readBufferNextToRead]==1){
		if(readBufferStart[readBufferNextToRead]<readBufferLast[readBufferNextToRead]){
			//last position not reached
			Serial.write(rbuffer[readBufferNextToRead][(readBufferStart[readBufferNextToRead])]);//data at bufPos sent
			mockWriteByteCounter++;
			if(mockWriteByteCounter>=22050){
				Serial.write("W");
				mockWriteByteCounter=0;
			}
			readBufferStart[readBufferNextToRead]++;//pointing to next pos
		}else{
			rbufferState[readBufferNextToRead]=0;
			readBufferNextToRead=(readBufferNextToRead==0)?1:0;
		}
	}
	if(wbufferState[writeBufferNextToFill]==0){
		if(writeBufferLast[writeBufferNextToFill]>writeBufferStart[writeBufferNextToFill]){
			mockReadByteCounter++;
			if(mockReadByteCounter>=22050){
				Serial.write("R");
				mockReadByteCounter=0;
			}
			//sample=65;//A
			sample=Serial.read();
			//Serial.write(sample);
			wbuffer[writeBufferNextToFill][(writeBufferStart[writeBufferNextToFill])]=((sample>=250 || sample<=5)?prevSample:sample);
			//wbuffer[writeBufferNextToFill][(writeBufferStart[writeBufferNextToFill])]=sample;
			writeBufferStart[writeBufferNextToFill]++;
			prevSample=sample;
		}else{
			int temp=writeBufferNextToFill;
			writeBufferNextToFill=(writeBufferNextToFill==0)?1:0;
			wbufferState[temp]=1;
		}
	}
}
#endif

//record message to file from phone
void recordMessageFromPhone(){
	if(recordInProgress && someHeavyProcessingInProgress){
		if(clientManager.audioSocket.available()){
			int readBytes=clientManager.audioSocket.read(rbuffer[0],INBUFSIZE);
			recordLength+=readBytes;
			DEBUG_PRINT("R-");DEBUG_PRINTLN(recordLength);
			//max file size is MAX_REC_LENGTH
			if(recordLength>maxRecLen){
				recordLength=0;
				recordInProgress=false;
				someHeavyProcessingInProgress=false;
				recordingFile.close();
				//stopFileProcessing(recordingFile);
				strcpy(responseBuffer,commands[STOP_RECORD]);
				strcat(responseBuffer,":ACK:MAX_REC_LENGTH:max file size reached");
				clientManager.sendWSCommand(responseBuffer, heavyProcessResponseClient);
				DEBUG_PRINT("recordMessageFromPhone stopped, max size reached, bytes: ");DEBUG_PRINTLN(recordLength);
				return;
			}
			size_t size=recordingFile.write(rbuffer[0],readBytes);
			DEBUG_PRINT("written bytes: ");DEBUG_PRINTLN(size);
		}
	}
}

//playMessage to speaker from file
void bufferedPlaybackToSpeaker(){
	if(playbackInProgress && rbufferState[readBufferNextToFill]==0){
		DEBUG_PRINT("-P-");
		//if no data available then stop playback
		boolean dataAvailable=false;
		if(playingFromSD){
			dataAvailable=playingFileFromSD.available();
		}else{
			dataAvailable=playingFileFromFlash.available();
		}
		if(!dataAvailable){
			DEBUG_PRINTLN("playingFile.available() is false");
			if(strcmp(playRequestSource,REQUEST_SOURCE_PHONE)==0){
				DEBUG_PRINTLN("request source is phone");
				strcpy(responseBuffer,commands[STOP_PLAY]);
				if(stopPlayback()){
					strcat(responseBuffer,":ACK:FILE_END:Playback completed");
				}else{
					strcat(responseBuffer,":ACK:FILE_END:File end reached and playback stopped");
				}
				clientManager.sendWSCommand(responseBuffer, heavyProcessResponseClient);
				DEBUG_PRINT("sent message is: ");DEBUG_PRINT(responseBuffer);

			}else if(strcmp(playRequestSource,REQUEST_SOURCE_NOTIFY)==0){
				DEBUG_PRINTLN("request source is notify");
				stopPlayback();
				if(sdCardPresent){
					//record message
					generateRandomFileName(fileNameBuffer, "messages");

					if(startRecordFromMic(fileNameBuffer,REQUEST_SOURCE_NOTIFY)){
						//recording started
						DEBUG_PRINTLN("recording started");
					}
				}else{
					//play thank you and close notification process
					DEBUG_PRINT("playing THANK_YOU: ");DEBUG_PRINTLN(recordRequestSource);
					startPlayback(THANK_YOU, REQUEST_SOURCE_NOTIFY_STOP);
				}
			}else if(strcmp(playRequestSource,REQUEST_SOURCE_NOTIFY_STOP)==0){
				//do nothing
				DEBUG_PRINTLN("request source is notify");
				stopPlayback();
				notifyFlowInProgress=false;
			}
			return;
		}
		ESP.wdtDisable();
		int temp=readBufferNextToFill;
		if(playingFromSD){
			readBufferLast[readBufferNextToFill]=playingFileFromSD.read(rbuffer[readBufferNextToFill],INBUFSIZE);
		}else{
			readBufferLast[readBufferNextToFill]=playingFileFromFlash.read(rbuffer[readBufferNextToFill],INBUFSIZE);
		}
		readBufferStart[readBufferNextToFill]=0;
		readBufferNextToFill=(readBufferNextToFill==0)?1:0;
		rbufferState[temp]=1;
		ESP.wdtEnable(0);
	}
}

//Records message in file from mic
void bufferedRecordFromMic(){//it is the reader of the buffer
	if(recordFromMicInProgress && wbufferState[writeBufferNextToRead]==1){//ready for read
		DEBUG_PRINT("-R-");
		ESP.wdtDisable();
		recordLength+=writeBufferLast[writeBufferNextToRead];
		if(recordLength>maxRecLen){
			DEBUG_PRINT("bufferedRecordFromMic max file size reached, bytes: ");DEBUG_PRINTLN(recordLength);
			recordLength=0;
			stopRecordFromMic();
			DEBUG_PRINT("bufferedRecordFromMic recordRequestSource: ");DEBUG_PRINTLN(recordRequestSource);
			if(strcmp(recordRequestSource,REQUEST_SOURCE_NOTIFY)==0){
				DEBUG_PRINT("playing THANK_YOU: ");DEBUG_PRINTLN(recordRequestSource);
				startPlayback(THANK_YOU,REQUEST_SOURCE_NOTIFY_STOP);
			}
			return;
		}

		recordingFileFromMic.write((const uint8_t *)wbuffer[writeBufferNextToRead],writeBufferLast[writeBufferNextToRead]);
		writeBufferStart[writeBufferNextToRead]=0;
		wbufferState[writeBufferNextToRead]=0;//set state=0 indicating ready for write
		writeBufferNextToRead=(writeBufferNextToRead==0)?1:0;//switching to next buffer
		ESP.wdtEnable(0);
	}
}

boolean startPlayback(const char * fileName, const char * requestSource){
	boolean retVal=false;
	strcpy(playRequestSource,requestSource);
#ifdef DEBUG
	DEBUG_PRINT("startPlayback file: ");DEBUG_PRINTLN(fileName);
	DEBUG_PRINT("requestSource: ");DEBUG_PRINTLN(requestSource);
	DEBUG_PRINT("playbackInProgress: ");DEBUG_PRINTLN(playbackInProgress);
	DEBUG_PRINT("someHeavyProcessingInProgress: ");DEBUG_PRINTLN(someHeavyProcessingInProgress);
	DEBUG_PRINTLN("****************************************************************");
	Serial.flush();
	delay(1000);

#endif
	if(!playbackInProgress && !someHeavyProcessingInProgress){
		//playingFile=initiateFilePlayback(fileName);
		//if sd card is present then try playing from sd card, else from spi flash
		boolean filePresent=false;
		playingFromSD=false;
		if(sdCardPresent){
			trimPath((char *)fileName, fileNameBuffer, false);
			DEBUG_PRINT("Opening file: ");DEBUG_PRINTLN(fileNameBuffer);
			playingFileFromSD=SD.open(fileNameBuffer);
			if(playingFileFromSD){
				filePresent=true;
				playingFromSD=true;
			}
		}else{
			char * spiFileName=strcat("/",fileName);
			playingFileFromFlash=SPIFFS.open(spiFileName, "r");
			if(playingFileFromFlash){
				filePresent=true;
				playingFromSD=false;
			}
		}
		//if file exists then attempt playback
		if(filePresent){
#ifdef DEBUG
			DEBUG_PRINT("startPlayback - playback started ");
			if(sdCardPresent){
				DEBUG_PRINTLN(playingFileFromSD.name());
			}else{
				DEBUG_PRINTLN(playingFileFromFlash.name());
			}
#endif
			enableTimer1();
			notifyArduino(HIGH);
			playbackInProgress=true;
			someHeavyProcessingInProgress=true;
			heavyProcessResponseClient=currentWSClient;
			retVal=true;
		}else{
			DEBUG_PRINTLN("startPlayback - file read error");
			retVal=false;
		}
	}else{
		retVal=false;
	}
	return retVal;
}


boolean stopPlayback(){
	boolean retVal=false;
#ifdef DEBUG
	DEBUG_PRINTLN("stopPlaybackFromFlash called");
	DEBUG_PRINT("playbackInProgress: ");DEBUG_PRINTLN(playbackInProgress);
	DEBUG_PRINT("someHeavyProcessingInProgress: ");DEBUG_PRINTLN(someHeavyProcessingInProgress);
#endif
	if(playbackInProgress){
		disableTimer1();
		notifyArduino(LOW);
		if(playingFromSD){
			playingFileFromSD.close();
		}else{
			playingFileFromFlash.close();
		}
		playbackInProgress=false;
		someHeavyProcessingInProgress=false;
#ifdef DEBUG
		delay(1000);
		DEBUG_PRINTLN("<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
		Serial.flush();
		DEBUG_PRINTLN("stopPlaybackFromFlash done success");
#endif
		retVal=true;
	}else{
		DEBUG_PRINTLN("stopPlaybackFromFlash done fault, playbackInProgress is false");
		retVal=false;
	}
	return retVal;
}


boolean startRecordFromMic(const char * fileName, const char * requestSource){
	boolean retVal=false;
	strcpy(recordRequestSource, requestSource);
#ifdef DEBUG
	DEBUG_PRINT("startRecordFromMic file: ");DEBUG_PRINTLN(fileName);
	DEBUG_PRINT("requestSource: ");DEBUG_PRINTLN(requestSource);
	DEBUG_PRINT("recordFromMicInProgress: ");DEBUG_PRINTLN(recordFromMicInProgress);
	DEBUG_PRINT("someHeavyProcessingInProgress: ");DEBUG_PRINTLN(someHeavyProcessingInProgress);
#endif
	if(!recordFromMicInProgress && !someHeavyProcessingInProgress){
		DEBUG_PRINTLN("startRecordFromMic in progress: ");
		enableTimer1();
		notifyArduino(HIGH);
		//recordingFile=initiateFileRecording(fileName);
		trimPath((char *)fileName, fileNameBuffer1, true);
		recordingFileFromMic=SD.open(fileNameBuffer1, FILE_WRITE_NO_APPEND);
		recordFromMicInProgress=true;
		someHeavyProcessingInProgress=true;
		heavyProcessResponseClient=currentWSClient;
		recordLength=0;
		retVal=true;
	}else{
		retVal=false;
	}

	return retVal;
}

boolean stopRecordFromMic(){
	boolean retVal=false;
	DEBUG_PRINT("stopRecordFromMic called: ");
	if(recordFromMicInProgress){
		DEBUG_PRINT("recordFromMicInProgress: ");DEBUG_PRINTLN(recordFromMicInProgress);
		DEBUG_PRINT("someHeavyProcessingInProgress: ");DEBUG_PRINTLN(someHeavyProcessingInProgress);
		disableTimer1();
		notifyArduino(LOW);
		recordingFileFromMic.close();
		//stopFileProcessing(recordingFile);
		recordFromMicInProgress=false;
		someHeavyProcessingInProgress=false;
		retVal=true;
	}else{
		retVal=false;
	}
	return retVal;
}

String formatBytes(size_t bytes){
	if (bytes < 1024){
		return String(bytes)+"B";
	} else if(bytes < (1024 * 1024)){
		return String(bytes/1024.0)+"KB";
	} else if(bytes < (1024 * 1024 * 1024)){
		return String(bytes/1024.0/1024.0)+"MB";
	} else {
		return String(bytes/1024.0/1024.0/1024.0)+"GB";
	}
}


void generateRandomFileName(char * shortName, const char * folder){
	if(folder!=NULL){
		strcpy(shortName, folder);
		strcat(shortName,"/");
		strcat(shortName,"R_");
	}else{
		strcpy(shortName,"R_");
	}
	char randBuf[10];
	char * fileNumber=itoa((unsigned short)millis(), randBuf, 10);
	strcat(shortName,fileNumber);
	strcat(shortName,".raw");
}


char *getMessagesList(SDFileWrapper dir, char *fileList){
	if(dir){
		dir.rewindDirectory();
		DEBUG_PRINTLN(dir.name());
		//fileList[0]='\0';
		while(true) {

			SDFileWrapper entry =  dir.openNextFile();
			if (! entry) {
				// no more files
				break;
			}
			if (!entry.isDirectory()) {
				strcat(fileList,entry.name());
				strcat(fileList,",");
			}
			entry.close();
		}
	}

	return fileList;
}


char dateStr[11];
void printDirectory(SDFileWrapper dir, int numTabs) {
	int nextFileCount=0;
	while(true) {

		SDFileWrapper entry =  dir.openNextFile();
		nextFileCount++;
		//PRINT(" next count: ");PRINT(nextFileCount);PRINT("---");
		if (! entry) {
			//PRINTLN("no more files");
			break;
		}
		for (uint8_t i=0; i<numTabs; i++) {
			DEBUG_PRINT('\t');
		}
		//PRINT(entry.name());
		if (entry.isDirectory()) {
			DEBUG_PRINTLN("/");
			printDirectory(entry, numTabs+1);
		} else {
			// files have sizes, directories do not
			DEBUG_PRINT("\t\t");
			DEBUG_PRINTDEC(entry.size());
			DEBUG_PRINT(" ");
			DEBUG_PRINT(entry.getStringDate(dateStr));
			DEBUG_PRINT(" ");
			DEBUG_PRINTLN(entry.getStringTime(dateStr));
		}
		//PRINT("closing entry: ");PRINTLN(entry.name());
		entry.close();
	}
}


void testFileWriteSD(){
	if(sdCardPresent){
		generateRandomFileName(fileNameBuffer, NULL);
		recordingFileFromMic=SD.open(fileNameBuffer, FILE_WRITE_NO_APPEND);
		if(recordingFileFromMic){
			DEBUG_PRINT("File Open Success: ");DEBUG_PRINTLN(recordingFileFromMic.name());
			recordingFileFromMic.println("testing file ");
			recordingFileFromMic.println(fileNameBuffer);
			recordingFileFromMic.close();
		}else{
			DEBUG_PRINT("File open failed: ");DEBUG_PRINTLN(fileNameBuffer);
		}
	}else{
		DEBUG_PRINTLN("SD Card not found");
	}
}


void trimPath(char * sourceStr, char *retstr, bool create){
	char *source=sourceStr;
	const char *s = "\\/";

	char *token;
	char part[20]="";
	/* get the first token */
	token = strtok(source, s);

	/* walk through other tokens */
	int tokenCount=0;
	while( token != NULL )
	{
		part[0]='\0';
		trim(token,part);
		if(tokenCount==0){
			strcpy(retstr, part);
		}else{
			strcat(retstr,"/");
			strcat(retstr, part);

		}
		tokenCount++;
		token = strtok(NULL, s);
	}
	if(create){
		int lastIndex=lastIndexOf(retstr,'/');
		if(lastIndex>0){
			char *dest=(char *)malloc(lastIndex+2);
			substr(retstr,dest,0,lastIndex);
			DEBUG_PRINT("dir name: "); DEBUG_PRINTLN(dest);
			if(!SD.exists(dest)){
				SD.mkdir(dest);
			}
			free(dest);
		}
	}
}

int lastIndexOf(char * source, char character){
	int lastPos=-1;
	for(int i=0;source[i]!='\0';i++){
		if(source[i]==character){
			lastPos=i;
		}
	}
	return lastPos;
}

void substr(char * source, char *dest, int startIndex, int endIndex){
	dest[0]='\0';
	if(startIndex>endIndex)return;
	int j=0;
	for(int i=0;source[i]!='\0';i++){
		if(i>=startIndex && i<endIndex){
			dest[j++]=source[i];
		}
	}
	dest[j]='\0';

}
void trim(char * name, char *sname){
	char *copy=name;
	int dotCount=0;
	int lastDotPos=-1;
	int i=0;
	for(;*copy!='\0';i++){
		if(*copy=='.'){
			dotCount++;
			lastDotPos=i;
		}
		copy++;
	}
	int strLength=i;
	copy=name;
	int namePartLength=0;
	if(lastDotPos>0){
		for(int j=0;j<lastDotPos;j++){
			if(name[j]!='.' && name[j]!=' ' && name[j]!='\t'){
				if(name[j]==',' ||name[j]==';' ||name[j]=='[' || name[j]==']' || name[j]=='+'){
					sname[namePartLength]='_';
				}else{
					sname[namePartLength]=toupper(name[j]);
				}
				if(namePartLength==8){
					sname[6]='~';
					sname[7]='1';
					break;
				}
				namePartLength++;
			}
		}
		if(namePartLength==0){
			//invalid file name
			sname[0]='\0';
			return;
		}
		sname[namePartLength++]='.';
		int extLength=0;
		for(int j=lastDotPos+1;j<strLength;j++){
			if(name[j]!='\t' || name[j]!=' '){
				sname[namePartLength++]=toupper(name[j]);
				extLength++;
				if(extLength>=3){
					break;
				}
			}
		}
		if(extLength==0){
			sname[namePartLength-1]='\0';
		}else{
			sname[namePartLength]='\0';
		}
	}else{
		for(int j=0;j<strLength;j++){
			if(name[j]!='.' && name[j]!=' ' && name[j]!='\t'){
				if(name[j]==',' ||name[j]==';' ||name[j]=='[' || name[j]==']' || name[j]=='+'){
					sname[namePartLength]='_';
				}else{
					sname[namePartLength]=toupper(name[j]);
				}
				if(namePartLength==8){
					sname[6]='~';
					sname[7]='1';
					break;
				}
				namePartLength++;
			}
		}
		if(namePartLength==0){
			//invalid file name
			sname[0]='\0';
			return;
		}else{
			sname[namePartLength]='\0';
		}

	}

}
boolean startRestoreFromFlashToSD(char *fileName){
	boolean retVal=false;
	if(sdCardPresent){
		if(!restoreInProgress && !someHeavyProcessingInProgress){
			//opening source file in flash
			DEBUG_PRINT("Restoring file: ");DEBUG_PRINTLN(fileName);
			char * spiFileName=strcat("/",(char *)fileName);
			playingFileFromFlash=SPIFFS.open(spiFileName, "r");
			if(!playingFileFromFlash){
				return false;
			}
#ifdef DEBUG
			DEBUG_PRINT("open file from flash success: ");DEBUG_PRINT(playingFileFromFlash.name());DEBUG_PRINTLN(playingFileFromFlash.size());
			int rb=playingFileFromFlash.read(inBuffer,2920);
			DEBUG_PRINT("readbytes: "+rb);
			playingFileFromFlash.close();
#endif
			//opening the destination file

			trimPath((char *)fileName, fileNameBuffer, false);
			playingFileFromSD=SD.open(fileNameBuffer, FILE_WRITE_NO_APPEND);
			if(!playingFileFromSD){
				//dest file could not be opened, so close the source file as well
				playingFileFromFlash.close();
				return false;
			}
			//both files opened
			DEBUG_PRINT("open file from sd success: ");DEBUG_PRINT(playingFileFromSD.name());DEBUG_PRINTLN(playingFileFromSD.size());
			restoreInProgress=true;
			someHeavyProcessingInProgress=true;
			heavyProcessResponseClient=currentWSClient;
			retVal = true;
		}
	}
	return retVal;
}

void bufferedRestoreFromFlashToSD(){
	if(restoreInProgress){
		boolean dataAvailable=false;
		dataAvailable=playingFileFromFlash.available();
		if(!dataAvailable){
			DEBUG_PRINTLN("playingFileFromFlash.available() is false");
			strcpy(responseBuffer,commands[RESTORE]);
			strcat(responseBuffer,":ACK:FILE_END:Restore completed");
			clientManager.sendWSCommand(responseBuffer, heavyProcessResponseClient);
			DEBUG_PRINT("sent message is: ");DEBUG_PRINT(responseBuffer);
			restoreInProgress=false;
			someHeavyProcessingInProgress=false;
			playingFileFromFlash.close();
			playingFileFromSD.close();
		}else{
			ESP.wdtDisable();
			size_t readBytes=playingFileFromFlash.read(rbuffer[0],INBUFSIZE);
			DEBUG_PRINT("Bytes read: ");DEBUG_PRINTLN(readBytes);
			playingFileFromSD.write(rbuffer[0],readBytes);
			ESP.wdtEnable(0);
		}
	}
}