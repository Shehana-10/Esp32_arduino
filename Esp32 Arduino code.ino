#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <stdint.h>
#include <ArduinoJson.h>

// ===== Pin Definitions =====
#define DHTPIN         4
#define DHTTYPE        DHT11
#define MQ2PIN         39
#define VIBRATIONPIN   25
#define SOUNDPIN       26
#define FLAMEPIN       27
#define BUZZERPIN      5

// ===== Ethernet Pins =====
#define ETH_CS         13
#define ETH_RST        12
#define SPI_SCK        18
#define SPI_MISO       19
#define SPI_MOSI       23

// ===== Thresholds =====
#define TEMP_THRESHOLD      30.0
#define HUMIDITY_THRESHOLD  70
#define GAS_THRESHOLD       500
#define VIBRATION_THRESHOLD 1
#define SOUND_THRESHOLD     1000

// ===== Network Configuration =====
byte mac[] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED};
IPAddress localIP(192,168,43,20);
IPAddress gateway(192,168,43,21);    // Should be your laptop IP where MQTT broker runs
IPAddress subnet (255,255,255,0);
IPAddress dnsIP(192,168,43,21);      // Should be your laptop IP

// ===== MQTT Configuration =====
IPAddress mqttBroker(192,168,43,21); // MQTT broker runs on your laptop, not USB-Ethernet adapter

// ===== State Variables =====
bool flamePreviouslyDetected = false;

// ===== Globals =====
DHT            dht(DHTPIN, DHTTYPE);
EthernetClient ethClient;
PubSubClient   mqtt(ethClient);

// Timing
unsigned long lastHB   = 0;
const unsigned long HB_INTERVAL   = 60000;

// Notification helpers (sends to MQTT)
void sendNotification(const char* sensor, float value, const char* type, const char* message) {
  DynamicJsonDocument doc(256);
  doc["sensor"] = sensor;
  doc["value"] = value;
  doc["type"] = type;
  doc["message"] = message;
  
  String json;
  serializeJson(doc, json);
  
  mqtt.publish("datacenter/notification", json.c_str());
  
  Serial.print("📣 Notification -> ");
  Serial.println(json);
}

// MQTT reconnect helper
void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker at ");
    Serial.print(mqttBroker);
    Serial.print("...");
    if (mqtt.connect("ESP32_Environmental_Monitor")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial);
  Serial.println("\n=== ESP32 Environmental Monitor ===");

  // Initialize sensors and buzzer
  dht.begin();
  pinMode(MQ2PIN, INPUT);
  pinMode(VIBRATIONPIN, INPUT);
  pinMode(SOUNDPIN, INPUT);
  pinMode(FLAMEPIN, INPUT);
  pinMode(BUZZERPIN, OUTPUT);
  digitalWrite(BUZZERPIN, LOW);

  // Initialize Ethernet
  Ethernet.init(ETH_CS);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  Ethernet.begin(mac, localIP, gateway, subnet);
  delay(1500);
  Serial.print("ESP32 IP: ");
  Serial.println(Ethernet.localIP());
  
  // Test network connectivity
  Serial.print("Testing ping to gateway (");
  Serial.print(gateway);
  Serial.println(")...");
  
  Serial.print("Testing ping to MQTT broker (");
  Serial.print(mqttBroker);
  Serial.println(")...");

  // Initialize MQTT
  mqtt.setServer(mqttBroker, 1883);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(30);
}

void loop() {
  // Maintain MQTT connection
  reconnectMQTT();
  mqtt.loop();

  // Publish environmental data every HB_INTERVAL
  if (millis() - lastHB >= HB_INTERVAL) {
    lastHB = millis();

    // Read environmental sensors
    float temperature = dht.readTemperature();
    float humidity    = dht.readHumidity();
    int   gas         = analogRead(MQ2PIN);
    int   vibration   = digitalRead(VIBRATIONPIN);
    int   sound       = analogRead(SOUNDPIN);
    int   flame       = digitalRead(FLAMEPIN);

    // ✅ Print sensor readings to Serial
    Serial.println("📟 Sensor Data Readings:");
    Serial.print("🌡 Temperature: "); Serial.print(temperature); Serial.println(" °C");
    Serial.print("💧 Humidity: ");    Serial.print(humidity);    Serial.println(" %");
    Serial.print("🟡 Gas: ");         Serial.println(gas);
    Serial.print("🛑 Vibration: ");   Serial.println(vibration);
    Serial.print("🔊 Sound: ");       Serial.println(sound);
    Serial.print("🔥 Flame: ");       Serial.println(flame == true ? "Detected" : "None");
    Serial.print("🕒 Time since boot: "); Serial.print(millis() / 1000); Serial.println(" s");
    Serial.println("------------------------------------");

    String envPayload = "{";
    envPayload += "\"temperature\":" + String(temperature, 1) + ",";
    envPayload += "\"humidity\":"    + String(humidity, 1)    + ",";
    envPayload += "\"gas\":"         + String(gas)            + ",";
    envPayload += "\"vibration\":"   + String(vibration)      + ",";
    envPayload += "\"sound\":"       + String(sound)          + ",";
    envPayload += "\"flame\":"       + String(flame);
    envPayload += "}";
    mqtt.publish("datacenter/sensor_data", envPayload.c_str());
    
    Serial.println("Published environmental data");

    // Notifications based on thresholds…
    bool anyAlert = false;
    if (!isnan(temperature) && temperature > TEMP_THRESHOLD) {
      anyAlert = true;
      sendNotification("temperature", temperature, "critical",
                       ("Critical! Datacenter temperature is " + String(temperature,1) + "°C.").c_str());
    }
    if (!isnan(humidity) && humidity > HUMIDITY_THRESHOLD) {
      anyAlert = true;
      sendNotification("humidity", humidity, "critical",
                       ("Critical! Humidity is " + String(humidity,1) + "%.").c_str());
    }
    if (gas > GAS_THRESHOLD) {
      anyAlert = true;
      sendNotification("gas", gas, "critical",
                       ("High CO₂ levels: " + String(gas) + "ppm.").c_str());
    }
    if (vibration > VIBRATION_THRESHOLD) {
      anyAlert = true;
      sendNotification("vibration", vibration, "warning",
                       ("Vibration detected: " + String(vibration)).c_str());
    }
    if (sound > SOUND_THRESHOLD) {
      anyAlert = true;
      sendNotification("sound", sound, "warning",
                       ("Sound level is high: " + String(sound)).c_str());
    }
    if (flame == 1 && !flamePreviouslyDetected) {
      flamePreviouslyDetected = true;
      anyAlert = true;
      
      Serial.println("🔥 Flame detected!");
      sendNotification("flame", 1, "critical", "🔥 Flame detected! Fire risk!");

      // 🔔 Buzzer alert for flame
      for (int i = 0; i < 6; ++i) {
        digitalWrite(BUZZERPIN, HIGH); delay(150);
        digitalWrite(BUZZERPIN, LOW);  delay(150);
      }

    } else if (flame == 0 && flamePreviouslyDetected) {
      // Flame was previously detected but now it's gone
      flamePreviouslyDetected = false;
      Serial.println("🔥 Flame not detected.");
    } else if (flame == 0 && !flamePreviouslyDetected) {
      // Flame never detected yet — print flame status anyway
      Serial.println("🔥 Flame not detected.");
    }

    // 🔔 Buzzer for other alerts (temperature, gas, etc.)
    if (anyAlert) {
      for (int i = 0; i < 4; ++i) {
        digitalWrite(BUZZERPIN, HIGH); delay(200);
        digitalWrite(BUZZERPIN, LOW);  delay(200);
      }
    }
  }

  delay(200);
}