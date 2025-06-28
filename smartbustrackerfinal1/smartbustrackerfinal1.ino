#include <SD.h>
#include <SPI.h>
#include <MFRC522.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// === OLED ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_SDA 14
#define OLED_SCL 27
TwoWire I2Cone = TwoWire(1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2Cone, -1);

// === IR Sensors ===
#define ENTRY_SENSOR 34
#define EXIT_SENSOR  35
int peopleCount = 0;
bool entryFlag = false;
bool exitFlag = false;

// === GPS ===
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
#define GPS_RX 2
#define GPS_TX 4

// === SIM800L ===
HardwareSerial sim800(2);
#define SIM_TX 25
#define SIM_RX 26
String simStatus = "No Response";
unsigned long lastSIMCheck = 0;

// === RFID + SD === (shared SPI)
#define SD_CS    13
#define RFID_SS  5
#define RFID_RST 22
MFRC522 rfid(RFID_SS, RFID_RST);
bool sdReady = false;

// === Globals ===
String lastRFID = "None";
String lastLat = "NoFix";
String lastLon = "NoFix";
int lastBalance = -1;
const int FARE = 10;
String smsSenderNumber = "";
String ownerNumber = "+918685846997";

// === User Data ===
struct Passenger {
  String uid;
  int balance;
};
Passenger users[] = {
  {"23E928DA", 60},
  {"BE443302", 45}
};
const int NUM_USERS = sizeof(users) / sizeof(users[0]);

// === SETUP ===
void setup() {
  Serial.begin(115200);

  // OLED Init
  I2Cone.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("❌ OLED failed"); while (1);
  }
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Smart Bus Tracker");
  display.display();

  // IR Sensor
  pinMode(ENTRY_SENSOR, INPUT);
  pinMode(EXIT_SENSOR, INPUT);

  // GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("📡 GPS started");

  // SIM800L
  sim800.begin(9600, SERIAL_8N1, SIM_RX, SIM_TX);
  delay(1000);
  sim800.println("AT+CMGF=1");
  delay(500);
  sim800.println("AT+CNMI=2,2,0,0,0");
  delay(500);
  Serial.println("📶 SIM800L ready");

  // Shared SPI Init
  SPI.begin(18, 19, 23); // SCK, MISO, MOSI
  pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
  pinMode(RFID_SS, OUTPUT); digitalWrite(RFID_SS, HIGH);

  if (!SD.begin(SD_CS)) {
    Serial.println("❌ SD Card init failed");
    sdReady = false;
  } else {
    Serial.println("✅ SD Card Ready");
    sdReady = true;
  }

  rfid.PCD_Init();
  Serial.println("✅ RFID ready");
}

// === MAIN LOOP ===
void loop() {
  handleIRSensors();
  handleGPS();
  handleRFID();
  handleSIM800L();
  updateDisplay();
  delay(200);
}

// === IR SENSORS ===
void handleIRSensors() {
  int entryState = digitalRead(ENTRY_SENSOR);
  int exitState = digitalRead(EXIT_SENSOR);

  if (entryState == LOW && !entryFlag) entryFlag = true;
  if (entryState == HIGH && entryFlag) {
    peopleCount++;
    entryFlag = false;
    Serial.println("✅ Entered");
  }

  if (exitState == LOW && !exitFlag) exitFlag = true;
  if (exitState == HIGH && exitFlag) {
    if (peopleCount > 0) peopleCount--;
    exitFlag = false;
    Serial.println("🚪 Exited");
  }
}

// === GPS ===
void handleGPS() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  if (gps.location.isValid()) {
    lastLat = String(gps.location.lat(), 6);
    lastLon = String(gps.location.lng(), 6);
  } else {
    lastLat = "NoFix";
    lastLon = "NoFix";
  }
}

// === RFID ===
int getUserIndex(String uid) {
  for (int i = 0; i < NUM_USERS; i++) {
    if (users[i].uid == uid) return i;
  }
  return -1;
}

void handleRFID() {
  digitalWrite(SD_CS, HIGH);     // Disable SD
  digitalWrite(RFID_SS, LOW);    // Enable RFID

  if (!rfid.PICC_IsNewCardPresent()) {
    digitalWrite(RFID_SS, HIGH);
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    digitalWrite(RFID_SS, HIGH);
    return;
  }

  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();
  lastRFID = uidStr;

  int idx = getUserIndex(uidStr);
  String logLine;

  if (idx != -1 && users[idx].balance >= FARE) {
    users[idx].balance -= FARE;
    peopleCount++;
    logLine = "UID: " + uidStr + " | -₹" + String(FARE) + " | Bal: ₹" + String(users[idx].balance);
    lastBalance = users[idx].balance;
  } else if (idx != -1) {
    logLine = "UID: " + uidStr + " | ❌ Low Bal: ₹" + String(users[idx].balance);
    lastBalance = users[idx].balance;
  } else {
    logLine = "UID: " + uidStr + " | ❌ Unregistered";
    lastBalance = -1;
  }

  digitalWrite(RFID_SS, HIGH); // Disable RFID

  if (sdReady) {
    digitalWrite(SD_CS, LOW);  // Enable SD
    File file = SD.open("/log.txt", FILE_APPEND);
    if (file) {
      file.println(logLine);
      file.close();
    }
    digitalWrite(SD_CS, HIGH);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1000);
}

// === SIM800L ===
void handleSIM800L() {
  static String smsLine = "";
  while (sim800.available()) {
    char c = sim800.read();
    smsLine += c;

    if (c == '\n') {
      smsLine.trim();

      if (smsLine.startsWith("+CMT:")) {
        int quote1 = smsLine.indexOf("\"") + 1;
        int quote2 = smsLine.indexOf("\"", quote1);
        smsSenderNumber = smsLine.substring(quote1, quote2);
      } else if (smsLine.indexOf("TRACK") >= 0 && smsSenderNumber == ownerNumber) {
        String reply = "🚌 Bus Info:\n";
        if (lastLat != "NoFix") {
          reply += "📍 " + lastLat + "," + lastLon + "\n";
          reply += "🌍 https://maps.google.com/?q=" + lastLat + "," + lastLon + "\n";
        } else {
          reply += "📍 GPS not fixed\n";
        }
        reply += "👥 People: " + String(peopleCount) + "\n";
        reply += "🆔 RFID: " + lastRFID + "\n";
        reply += "💰 Bal: " + (lastBalance == -1 ? "N/A" : "₹" + String(lastBalance));
        sendSMS(reply, smsSenderNumber);
        smsSenderNumber = "";
      }

      smsLine = "";
    }
  }

  if (millis() - lastSIMCheck > 10000) {
    sim800.println("AT");
    lastSIMCheck = millis();
  }
}

void sendSMS(String message, String phoneNumber) {
  Serial.println("📤 Sending SMS to: " + phoneNumber);
  sim800.println("AT+CMGF=1");
  delay(500);
  sim800.print("AT+CMGS=\"");
  sim800.print(phoneNumber);
  sim800.println("\"");
  delay(500);
  sim800.print(message);
  sim800.write(26); // Ctrl+Z
  delay(3000);
  Serial.println("✅ SMS Sent!");
}

// === OLED DISPLAY ===
void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Smart Bus Tracker");

  display.print("Count: "); display.println(peopleCount);
  display.print("Entry IR: "); display.println(digitalRead(ENTRY_SENSOR) == LOW ? "Low" : "High");
  display.print("Exit IR: "); display.println(digitalRead(EXIT_SENSOR) == LOW ? "Low" : "High");
  display.print("RFID: "); display.println(lastRFID);
  display.print("Bal: ");
  if (lastBalance == -1) display.println("--");
  else { display.print("₹"); display.println(lastBalance); }
  if (lastLat == "NoFix") display.println("GPS: No Fix");
  else {
    display.print("Lat: "); display.println(lastLat);
    display.print("Lon: "); display.println(lastLon);
  }
  display.print("SIM: "); display.println(simStatus);
  display.print("SD: "); display.println(sdReady ? "Ready" : "Fail");
  display.display();
}