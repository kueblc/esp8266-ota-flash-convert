#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
extern "C" uint8_t system_upgrade_userbin_check();
extern "C" void system_upgrade_flag_set(uint8 flag);
extern "C" void system_upgrade_reboot (void);

#define MAGIC_V1 0xE9
#define MAGIC_V2 0xEA
#define UPGRADE_FLAG_START 0x01
#define UPGRADE_FLAG_FINISH 0x02
#define SECTOR_SIZE 4096
#define BUFFER_SIZE 1024
#define TIMEOUT 5000
#define SPI_FLASH_ADDR 0x40200000

#define VERSION "VTRUST-FLASH 1.0\n(c) VTRUST GMBH https://www.vtrust.de/35c3/"
#define WIFI_SSID "vtrust-flash"
#define WIFI_PASSWORD "flashmeifyoucan"
#define WIFI_APSSID "vtrust-recovery"

IPAddress ip(10, 42, 42, 42);
IPAddress gateway(10, 42, 42, 42);   
IPAddress subnet(255,255,255,0);   

#define RETRY 3         //number of times to retry

#define URL_ROM_2 "http://10.42.42.1/files/user2.bin"
#define URL_ROM_3 "http://10.42.42.1/files/thirdparty.bin"

ESP8266WebServer server(80);
char responseBuffer[282]; // total + 1 (for null terminal)

uint8_t userspace = system_upgrade_userbin_check();

void handleRoot() {
  // print WiFi diagonistics to Serial
  WiFi.printDiag(Serial);

  // get flash info; size, mode, and speed
  uint32_t flashInfo;
  ESP.flashRead(0, (uint32_t *)&flashInfo, 4);

  const char * FlashSize = "";
  switch((flashInfo >> 28) & 0xF){
    case 0x0:  FlashSize = "512K";  break;
    case 0x1:  FlashSize = "256K";  break;
    case 0x2:  FlashSize = "1M";  break;
    case 0x3:  FlashSize = "2M";  break;
    case 0x4:  FlashSize = "4M";  break;
    case 0x8:  FlashSize = "8M";  break;
    case 0x9:  FlashSize = "16M";  break;
  }

  const char * FlashMode = "";
  switch((flashInfo >> 16) & 0xF){
    case 0:  FlashMode = "QIO";  break;
    case 1:  FlashMode = "QOUT";  break;
    case 2:  FlashMode = "DIO";  break;
    case 3:  FlashMode = "DOUT";  break;
  }

  const char * FlashSpeed = "";
  switch((flashInfo >> 24) & 0xF){
    case 0x0:  FlashSpeed = "40";  break;
    case 0x1:  FlashSpeed = "26";  break;
    case 0x2:  FlashSpeed = "20";  break;
    case 0xF:  FlashSpeed = "80";  break;
  }

  sprintf(responseBuffer,
    VERSION "\n"                               // 61
    "READ FLASH: http://%s/backup" "\n"        // 27
    "ChipID: %x" "\n"                          // 9
    "MAC: %s" "\n"                             // 6
    "BootVersion: %d" "\n"                     // 14
    "BootMode: %s" "\n"                        // 11
    "FlashMode: %s %s @ %sMHz" "\n"            // 19
    "FlashChipId: %x" "\n"                     // 14
    "FlashChipRealSize: %dK" "\n"              // 21
    "Active Userspace: user%s000" "\n",        // 26
    WiFi.localIP().toString().c_str(),         // max 15
    ESP.getChipId(),                           // 6
    WiFi.macAddress().c_str(),                 // 17
    ESP.getBootVersion(),                      // 1
    ESP.getBootMode() ? "normal" : "enhanced", // max 8
    FlashSize, FlashMode, FlashSpeed,          // max 10
    ESP.getFlashChipId(),                      // 6
    ESP.getFlashChipRealSize() / 1024,         // max 4
    userspace ? "2 0x81" : "1 0x01");          // 6

  server.send(200, "text/plain", responseBuffer);
}

void handleNotFound(){
  server.send(404, "text/plain", "File Not Found\n");
}

void handleFlash2(){
  if (userspace)
  {
    server.send(200, "text/plain", "Device is already booting from userspace 2 (0x81000)\n");
    return;
  }
  else
  {
    const char * url = URL_ROM_2;
    String customUrl;
    if (server.hasArg("url")) {
      customUrl = server.arg("url");
      url = customUrl.c_str();
    }
    sprintf(responseBuffer, "Device should flash %s to userspace 0x81000 and restart\n", url);
    server.send(200, "text/plain", responseBuffer);
    flashRom2(url);
  }
}

void handleUndo(){
  sprintf(responseBuffer, "Rebooting into userspace %d\n", 2 - userspace);
  server.send(200, "text/plain", responseBuffer);

  system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
  system_upgrade_reboot();
}

void handleFlash3(){
  if (userspace)
  {
    const char * url = URL_ROM_3;
    String customUrl;
    if (server.hasArg("url")) {
      customUrl = server.arg("url");
      url = customUrl.c_str();
    }
    sprintf(responseBuffer, "Device should flash %s and restart\n", url);
    server.send(200, "text/plain", responseBuffer);
    flashRom1(url);
  }
  else
  {
    server.send(200, "text/plain", "Device is booting from userspace 1 (0x01000) Please flash it to boot from userspace 2 first!\n");
    return;
  }
}


void handleRead(){
  sprintf(responseBuffer, "attachment; filename=\"firmware-%x.bin\"", ESP.getChipId());
  server.sendHeader("Content-Disposition", responseBuffer);
  server.send_P(200, "application/octet-stream", (PGM_P) SPI_FLASH_ADDR, ESP.getFlashChipSize());
}

void setup_webserver(void){
  server.on("/", handleRoot);
  server.on("/flash2", handleFlash2);
  server.on("/flash3", handleFlash3);
  server.on("/undo", handleUndo);
  server.on("/backup", handleRead);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}




byte buffer[BUFFER_SIZE] __attribute__((aligned(4))) = {0};
byte bootrom[SECTOR_SIZE] __attribute__((aligned(4))) = {0};

void setup()
{
  // using esp boot loader default baud rate so we don't have to keep switching baud rates to get all the messages
  Serial.begin(74880);
  Serial.println();
  Serial.println(VERSION);
  connectToWiFiBlocking();
  setup_webserver();
}


void softAPsetup()
{
  Serial.println("Setting up SoftAP...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ip, gateway , subnet);   // subnet FF FF FF 00
  WiFi.softAP(WIFI_APSSID);
  delay(1000); 
}

void connectToWiFiBlocking()
{
  int x = 0;

  Serial.print("Connecting to Wifi");
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.config(ip, gateway , subnet);   // subnet FF FF FF 00

  while (WiFi.status() != WL_CONNECTED)
  {
    x++;
    delay(200);
    Serial.print("."); yield(); // reset watchdog
    if(x>100)
    {
      Serial.println("\nFailed to connect");
      softAPsetup();
      return;
    }
  }
  Serial.printf("\nConnected to %s as %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
}



void flashRom1(const char * url)
{
  bool result = downloadRomToFlash(
    1,                //Rom 1
    true,             //Bootloader is being updated
    0xE9,             //Standard Arduino Magic
    0x00000,          //Write to 0x0 since we are replacing the bootloader
    0x80000,          //Stop before 0x80000
    0,                //Erase Sector from 0 to
    128,              //Sector 128 (not inclusive)
    url,              //URL
    RETRY             //Retry Count
  );

  ESP.restart(); //restart regardless of success
}

//Download special rom.
void flashRom2(const char * url)
{
  system_upgrade_flag_set(UPGRADE_FLAG_START);
  bool result = downloadRomToFlash(
    2,                //Rom 2
    false,            //Bootloader is not being updated
    0xEA,             //V2 Espressif Magic
    0x081000,         //Not replacing bootloader
    0x100000,         //Stop before end of ram
    128,              //From middle of flash
    256,              //End of flash
    url,              //URL
    RETRY             //Retry Count
  );
  
  if(result)
  {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
    system_upgrade_reboot();
  }
  else
  {
    ESP.restart();
  }
}

//Assumes bootloader must be in first SECTOR_SIZE bytes.
bool downloadRomToFlash(byte rom, byte bootloader, byte magic, uint32_t start_address, uint32_t end_address, uint16_t erase_sectior_start, uint16_t erase_sector_end, const char * url, uint8_t retry_limit)
{
  uint8_t retry_counter = 0;
  while(retry_counter < retry_limit)
  {
    uint16_t erase_start = erase_sectior_start;
    uint32_t write_address = start_address;
    uint8_t header[4] = { 0 };
    bootrom[SECTOR_SIZE] = { 0 };
    buffer[BUFFER_SIZE] = { 0 };

    Serial.printf("Flashing rom %d (retry:%d): %s\n", rom, retry_counter, url);
    HTTPClient http;
    http.begin(url);
    http.useHTTP10(true);
    http.setTimeout(TIMEOUT);

    //Response Code Check
    uint16_t httpCode = http.GET();
    Serial.printf("HTTP response Code: %d\n", httpCode);
    if(httpCode != HTTP_CODE_OK)
    {
      Serial.println("Invalid response Code - retry");
      retry_counter++;
      continue;
    }

    //Length Check (at least one sector)
    uint32_t len = http.getSize();
    Serial.printf("HTTP response length: %d\n", len);
    if(len < SECTOR_SIZE)
    {
      Serial.println("Length too short - retry");
      retry_counter++;
      continue;
    }

    if(len > (end_address-start_address))
    {
      Serial.println("Length exceeds flash size - retry");
      retry_counter++;
      continue;
    }

    //Confirm magic byte
    WiFiClient* stream = http.getStreamPtr();
    stream->peekBytes(&header[0],4);
    Serial.printf("Magic byte from stream: 0x%02X\n", header[0]);
    if(header[0] != magic)
    {
      Serial.println("Invalid magic byte - retry");
      retry_counter++;
      continue;
    }

    if(bootloader)
    { 
      Serial.printf("Downloading %d byte bootloader", sizeof(bootrom));
      size_t size = stream->available();
      while(size < sizeof(bootrom))
      {
        Serial.print("."); yield(); // reset watchdog
        size = stream->available();
      }
      int c = stream->readBytes(bootrom, sizeof(bootrom));

      //Skip the bootloader section for the moment..
      erase_start++;
      write_address += SECTOR_SIZE;
      len -= SECTOR_SIZE;
      Serial.println("Done");
    }

    Serial.printf("Erasing flash sectors %d-%d", erase_start, erase_sector_end);
    for (uint16_t i = erase_start; i < erase_sector_end; i++)
    {
      ESP.flashEraseSector(i);
      Serial.print("."); yield(); // reset watchdog
    }  
    Serial.println("Done");
    
    Serial.printf("Downloading rom to 0x%06X-0x%06X in %d byte blocks", write_address, write_address+len, sizeof(buffer));
    //Serial.println();
    while(len > 0)
    {
      size_t size = stream->available();
      if(size >= sizeof(buffer) || size == len) 
      {
        int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        //Serial.printf("address=0x%06X, bytes=%d, len=%d\n", write_address, c, len);
        ESP.flashWrite(write_address, (uint32_t*)&buffer[0], c);
        write_address +=c; //increment next write address
        len -= c; //decremeant remaining bytes
      }
      Serial.print("."); yield(); // reset watchdog
    }
    http.end();
    Serial.println("Done");

    if(bootloader)
    {
      Serial.println("Erasing bootloader sector 0");
      ESP.flashEraseSector(0);
      Serial.println("Done");
      
      Serial.printf("Writing bootloader to 0x%06X-0x%06X", 0, SECTOR_SIZE);
      ESP.flashWrite(0, (uint32_t*)&bootrom[0], SECTOR_SIZE);
      Serial.println("Done");
    }

    return true;
  }
  Serial.println("Retry counter exceeded - giving up");
  return false;
}



void loop()
{
  server.handleClient();
}
