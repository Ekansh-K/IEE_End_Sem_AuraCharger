#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>
#include <MySQL_Connection.h>
#include <MySQL_Cursor.h>

WiFiClient client;
MySQL_Connection conn((Client *)&client);

const int SAMPLES = 10;
float voltageBuffer[SAMPLES];
float currentBuffer[SAMPLES];
float powerBuffer[SAMPLES];
float tempBuffer[SAMPLES];
int sampleIndex = 0;
unsigned long lastSampleTime = 0;
unsigned long lastDatabaseUpdate = 0;

char user[] = "sql12756859";
char password[] = "N9VtB5WPwr";
IPAddress server_addr(172,31,25,66);

const char* AP_SSID = "ESP32_BatteryMonitor";
const char* AP_PASSWORD = "12345678";
const int TEMP_PIN = 4;    
const int SDA_PIN = 21;    
const int SCL_PIN = 22;    
const int LED_PIN = 5;     
const int NUM_LEDS = 8;    
const float FULL_CHARGE_VOLTAGE = 4.2;
const float MIN_VOLTAGE = 3.0;
const float NOMINAL_CAPACITY = 2000.0;
float socInitial = -1.0;  // -1 indicates not set
float batteryCapacity = -1.0;  // in mAh
bool parametersSet = false;
unsigned long previousMillis = 0;
const long SAMPLE_INTERVAL = 10;  // 10ms between samples
const long UPDATE_INTERVAL = 1000;  // 1s for database updates

Adafruit_INA219 ina219;
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

float voltage = 0.0;
float current = 0.0;
float power = 0.0;
float temperature = 0.0;
float soc = 0.0;
float soh = 100.0;
float baselineVoltage = 0.0;
float baselineCurrent = 0.0;
bool isCalibrating = false;
unsigned long lastCapacityCheck = 0;
float totalCapacity = 6000.0;

float getAverageReading(float buffer[]) {
    float sum = 0;
    for(int i = 0; i < SAMPLES; i++) {
        sum += buffer[i];
    }
    return sum / SAMPLES;
}

void connectToDatabase() {
    Serial.println("Connecting to database...");
    if (conn.connect(server_addr, 3306, user, password)) {
        Serial.println("Database connection successful.");
    } else {
        Serial.println("Database connection failed.");
    }
}

void updateLEDColor(float percentage) {
    percentage = constrain(percentage, 0, 100);
    uint8_t red = map(percentage, 0, 100, 255, 0);
    uint8_t green = map(percentage, 0, 100, 0, 255);
    for(int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(red, green, 0));
    }
    strip.show();
}

void readSensors() {
    voltage = ina219.getBusVoltage_V();
    current = ina219.getCurrent_mA();
    power = ina219.getPower_mW();
    sensors.requestTemperatures();
    temperature = sensors.getTempCByIndex(0);
    
    if (lastCapacityCheck > 0) {
        unsigned long timeDiff = millis() - lastCapacityCheck;
        totalCapacity += (current * timeDiff) / 3600000.0;
    }
    lastCapacityCheck = millis();
}

float calculateSoC() {
    if (!parametersSet) return -1.0;
    
    static float currentSoC = socInitial;
    float deltaTime = 1.0;  // 1 second interval
    
    // Calculate SOC using average current
    float deltaSOC = (current * deltaTime) / (batteryCapacity * 36.0); // Convert to proper units
    currentSoC += deltaSOC;
    
    // Bound SOC between 0 and 100
    currentSoC = constrain(currentSoC, 0.0, 100.0);
    
    // Voltage-based correction
    float voltageBasedSoC = ((voltage - MIN_VOLTAGE) / (FULL_CHARGE_VOLTAGE - MIN_VOLTAGE)) * 100.0;
    return (currentSoC * 0.7) + (voltageBasedSoC * 0.3);
}

float calculateSoH() {
    if (baselineVoltage > 0) {
        return (voltage / baselineVoltage) * 100.0;
    }
    return 100.0;
}

void calibrateBattery() {
    float voltageSum = 0;
    float currentSum = 0;
    
    for(int i = 0; i < 10; i++) {
        readSensors();
        voltageSum += voltage;
        currentSum += current;
        delay(1000);
    }
    
    baselineVoltage = voltageSum / 10.0;
    baselineCurrent = currentSum / 10.0;
    totalCapacity = 0.0;
    isCalibrating = false;
}

void handleRoot() {
    String html = "<html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;} ";
    html += ".data{margin:10px;padding:10px;border:1px solid #ccc;} ";
    html += ".setup{background:#f0f0f0;padding:20px;margin-bottom:20px;} ";
    html += "</style>";
    
    // JavaScript for real-time updates
    html += "<script>";
    html += "function updateData(){";
    html += "fetch('/data').then(r=>r.json()).then(data=>{";
    html += "if(data.parametersSet){";
    html += "document.getElementById('values').innerHTML=`";
    html += "<div class='data'>Voltage: ${data.v}V</div>";
    html += "<div class='data'>Current: ${data.c}mA</div>";
    html += "<div class='data'>Power: ${data.p}mW</div>";
    html += "<div class='data'>Temperature: ${data.t}°C</div>";
    html += "<div class='data'>State of Charge: ${data.soc}%</div>";
    html += "<div class='data'>State of Health: ${data.soh}%</div>`;";
    html += "document.getElementById('setup').style.display='none';";
    html += "}});}";
    html += "setInterval(updateData,1000);</script>";
    html += "</head><body>";
    
    // Battery parameters setup form
    html += "<div id='setup' class='setup'>";
    html += "<h2>Battery Parameters Setup</h2>";
    html += "<form action='/setBatteryParams' method='POST'>";
    html += "Initial Battery Percentage (%): <input type='number' name='socInitial' min='0' max='100' required><br><br>";
    html += "Battery Capacity (mAh): <input type='number' name='capacity' min='0' required><br><br>";
    html += "<input type='submit' value='Set Parameters' style='padding:10px'>";
    html += "</form></div>";
    
    html += "<h2>Battery Monitoring System</h2>";
    html += "<div id='values'>Please set battery parameters first...</div>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

void handleData() {
    String json = "{";
    json += "\"parametersSet\":" + String(parametersSet);
    if (parametersSet) {
        json += ",\"v\":" + String(voltage, 3);
        json += ",\"c\":" + String(current, 2);
        json += ",\"p\":" + String(power, 2);
        json += ",\"t\":" + String(temperature, 2);
        json += ",\"soc\":" + String(soc, 1);
        json += ",\"soh\":" + String(soh, 1);
        json += ",\"capacity\":" + String(batteryCapacity, 0);
    }
    json += "}";
    server.send(200, "application/json", json);
}

void handleSetBatteryParams() {
    if (server.hasArg("socInitial") && server.hasArg("capacity")) {
        socInitial = server.arg("socInitial").toFloat();
        batteryCapacity = server.arg("capacity").toFloat();
        parametersSet = true;
        soc = socInitial;
        server.sendHeader("Location", "/");
        server.send(303);
    } else {
        server.send(400, "text/plain", "Missing parameters");
    }
}
void handleCalibrate() {
    if (!isCalibrating) {
        isCalibrating = true;
        calibrateBattery();
        server.send(200, "text/plain", "Calibration completed");
    } else {
        server.send(400, "text/plain", "Calibration in progress");
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin(SDA_PIN, SCL_PIN);
    
    if (!ina219.begin()) {
        Serial.println("Failed to find INA219 chip");
        while (1);
    }
    
    strip.begin();
    strip.setBrightness(50);
    strip.show();
    
    sensors.begin();
    
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println("Access Point Started");
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());

    connectToDatabase();
    
    server.on("/", handleRoot);
    server.on("/data", HTTP_GET, handleData);
    server.on("/calibrate", HTTP_POST, handleCalibrate);
    server.on("/setBatteryParams", HTTP_POST, handleSetBatteryParams);
    server.begin();

}

void loop() {
    unsigned long currentTime = millis();
    if (currentTime - lastSampleTime >= 10) {
        voltageBuffer[sampleIndex] = ina219.getBusVoltage_V();
        currentBuffer[sampleIndex] = ina219.getCurrent_mA();
        powerBuffer[sampleIndex] = ina219.getPower_mW();
        sensors.requestTemperatures();
        tempBuffer[sampleIndex] = sensors.getTempCByIndex(0);       
        sampleIndex++;
        lastSampleTime = currentTime;      

        if (sampleIndex >= SAMPLES) {
            voltage = getAverageReading(voltageBuffer);
            current = getAverageReading(currentBuffer);
            power = getAverageReading(powerBuffer);
            temperature = getAverageReading(tempBuffer);
            if (lastCapacityCheck > 0) {
                unsigned long timeDiff = currentTime - lastCapacityCheck;
                totalCapacity += (current * timeDiff) / 3600000.0;
            }
            lastCapacityCheck = currentTime;
            soc = calculateSoC();
            soh = calculateSoH();

            updateLEDColor(soc);

            if (conn.connected()) {
                MySQL_Cursor *cursor = new MySQL_Cursor(&conn);
                char query[256];
                sprintf(query, "INSERT INTO battery_data (current, voltage, power, temperature, soc, soh) VALUES (%f, %f, %f, %f, %f, %f)",
                        current, voltage, power, temperature, soc, soh);
                cursor->execute(query);
                delete cursor;
            }
            
            sampleIndex = 0;  
        }
    }
    
    server.handleClient();

    if (!conn.connected() && (currentTime - lastDatabaseUpdate > 500)) {
        connectToDatabase();
        lastDatabaseUpdate = currentTime;
    }
}