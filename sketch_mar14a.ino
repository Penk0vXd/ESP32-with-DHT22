#if defined(ESP32)
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #error "Please select ESP32 or ESP8266 board"
#endif
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "esp32-hal-ledc.h"  // За PWM функциите

// Настройки за сензора
#define DHTPIN 4       
#define DHTTYPE DHT22  

DHT dht(DHTPIN, DHTTYPE);
AsyncWebServer server(80);

const char* ssid = "";
const char* password = "";

#define WIFI_TIMEOUT 10000 // 10 секунди таймаут за WiFi
#define SENSOR_READ_INTERVAL 5000 // 5 секунди между четенията

// Структура за съхранение на измерванията
struct Measurement {
    float temperature;
    float humidity;
    unsigned long timestamp;
};

#define HISTORY_SIZE 24 // Пазим последните 24 измервания
Measurement measurements[HISTORY_SIZE];
int measurementIndex = 0;

// Аларми за температура
#define TEMP_ALARM_HIGH 30.0
#define TEMP_ALARM_LOW 10.0

// NTP настройки
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

// Добавяме след другите дефиниции
#define DATA_FILENAME "/measurements.csv"

// В началото на файла, при другите дефиниции
#define BUZZER_PIN 5  // D5 пин за buzzer
#define TEMP_BUZZER_THRESHOLD 35.0  // Праг за включване на buzzer
#define BUZZER_ON_TIME 300   // 0.3 секунди звук
#define BUZZER_OFF_TIME 200  // 0.2 секунди пауза
#define BUZZER_DUTY 255      // Максимална сила (0-255)

// Добавяме функция за управление на buzzer
void handleBuzzer(float temperature) {
    static unsigned long lastToggle = 0;
    static bool state = false;
    const unsigned long now = millis();
    
    if (temperature >= TEMP_BUZZER_THRESHOLD) {
        if (now - lastToggle >= (state ? BUZZER_ON_TIME : BUZZER_OFF_TIME)) {
            state = !state;
            lastToggle = now;
            digitalWrite(BUZZER_PIN, state ? HIGH : LOW);
        }
    } else if (state) {
        state = false;
        digitalWrite(BUZZER_PIN, LOW);
    }
}

// Оптимизирана функция за запис
void saveMeasurement(float temp, float hum, unsigned long timestamp) {
    if (SPIFFS.usedBytes() > SPIFFS.totalBytes() * 0.9) { // Проверка за 90% запълване
        SPIFFS.remove(DATA_FILENAME);
    }
    
    static const size_t BUFFER_SIZE = 32;
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "%lu,%.1f,%.1f\n", timestamp, temp, hum);
    
    File file = SPIFFS.open(DATA_FILENAME, FILE_APPEND);
    if (file) {
        file.print(buffer);
        file.close();
    }
}

// Функция за четене на всички измервания
String readAllMeasurements() {
    String json = "[";
    File file = SPIFFS.open(DATA_FILENAME, FILE_READ);
    if (!file) {
        return "[]";
    }
    
    bool firstLine = true;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() > 0) {
            // Парсване на CSV ред
            int firstComma = line.indexOf(',');
            int secondComma = line.indexOf(',', firstComma + 1);
            
            String timestamp = line.substring(0, firstComma);
            String temp = line.substring(firstComma + 1, secondComma);
            String hum = line.substring(secondComma + 1);
            
            if (!firstLine) json += ",";
            json += "{\"time\":" + timestamp + 
                    ",\"temp\":" + temp + 
                    ",\"hum\":" + hum + "}";
            firstLine = false;
        }
    }
    json += "]";
    file.close();
    return json;
}

// Добавете тази функция
void checkStorage() {
    Serial.printf("Общо: %d байта\n", SPIFFS.totalBytes());
    Serial.printf("Използвани: %d байта\n", SPIFFS.usedBytes());
    Serial.printf("Свободни: %d байта\n", SPIFFS.totalBytes() - SPIFFS.usedBytes());
}

// Оптимизирана функция за WiFi връзка
void reconnectWiFi() {
    static unsigned long lastAttempt = 0;
    const unsigned long RETRY_INTERVAL = 30000; // 30 секунди между опитите
    
    if (WiFi.status() != WL_CONNECTED && 
        millis() - lastAttempt >= RETRY_INTERVAL) {
        lastAttempt = millis();
        
        Serial.println("Reconnecting to WiFi...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
    }
}

void formatSPIFFS() {
    if(!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    
    Serial.println("Форматиране на SPIFFS...");
    SPIFFS.format();
    Serial.println("SPIFFS форматиран успешно");
}

void setup() {
    Serial.begin(115200);
    
    // Конфигурираме buzzer пина като обикновен цифров изход
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    // Добавете това преди останалия код в setup()
    formatSPIFFS();
    
    // Инициализираме SPIFFS само веднъж
    if (!SPIFFS.begin(true)) {
        Serial.println("Critical error: SPIFFS cannot be initialized!");
        while(1) delay(1000);
    }
    
    dht.begin();
    
    WiFi.begin(ssid, password);
    Serial.println("Connecting to WiFi...");
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }

    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Главна страница
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Метеостанция</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background: linear-gradient(45deg, #ee7752, #e73c7e, #23a6d5, #23d5ab);
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: rgba(255, 255, 255, 0.95);
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 0 10px rgba(0,0,0,0.1);
        }
        .data-container {
            display: flex;
            justify-content: space-around;
            margin-bottom: 20px;
        }
        .data-box {
            text-align: center;
            padding: 20px;
            background: white;
            border-radius: 10px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
            width: 45%;
        }
        .data-label {
            font-size: 1.2em;
            color: #666;
        }
        .data-value {
            font-size: 2em;
            font-weight: bold;
            color: #2c3e50;
        }
        .chart-container {
            margin: 20px 0;
            padding: 20px;
            background: white;
            border-radius: 10px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
        }
        .last-update {
            text-align: center;
            color: #666;
        }
        h1 {
            text-align: center;
            color: #2c3e50;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Метеостанция</h1>
        <div class="data-container">
            <div class="data-box">
                <div class="data-label">Температура</div>
                <div class="data-value">
                    <span id="temp">--</span>°C
                </div>
            </div>
            <div class="data-box">
                <div class="data-label">Влажност</div>
                <div class="data-value">
                    <span id="humidity">--</span>%
                </div>
            </div>
        </div>
        <div class="chart-container">
            <canvas id="measurementChart"></canvas>
        </div>
        <div class="last-update">
            Последно обновяване: <span id="lastUpdate">--:--:--</span>
        </div>
    </div>

    <script>
        const FETCH_INTERVAL = 5000;
        let chart;

        function initChart() {
            const ctx = document.getElementById('measurementChart').getContext('2d');
            chart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [{
                        label: 'Температура °C',
                        borderColor: '#ff6384',
                        data: []
                    }, {
                        label: 'Влажност %',
                        borderColor: '#36a2eb',
                        data: []
                    }]
                },
                options: {
                    responsive: true
                }
            });
        }

        async function updateData() {
            try {
                const response = await fetch('/data');
                const data = await response.json();
                
                document.getElementById('temp').textContent = data.temperature.toFixed(1);
                document.getElementById('humidity').textContent = data.humidity.toFixed(1);
                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
                
                // Обновяване на графиката
                const historyResponse = await fetch('/history');
                const history = await historyResponse.json();
                
                chart.data.labels = history.map(m => new Date(m.time * 1000).toLocaleTimeString());
                chart.data.datasets[0].data = history.map(m => m.temp);
                chart.data.datasets[1].data = history.map(m => m.hum);
                chart.update();
            } catch (error) {
                console.error('Error:', error);
            }
        }

        document.addEventListener('DOMContentLoaded', () => {
            initChart();
            updateData();
            setInterval(updateData, FETCH_INTERVAL);
        });
    </script>
</body>
</html>
        )rawliteral";

        request->send(200, "text/html", html);
    });

    // API за данните
    server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
        float h = dht.readHumidity();
        float t = dht.readTemperature();

        if (isnan(h) || isnan(t)) {
            request->send(500, "application/json", "{\"error\": \"Грешка при четене от DHT сензора!\"}");
            return;
        }


        String json = "{\"temperature\": " + String(t, 1) + 
                      ", \"humidity\": " + String(h, 1) + "}";
        request->send(200, "application/json", json);
    });

    // Добавяме нов endpoint за историята
    server.on("/history", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = readAllMeasurements();
        request->send(200, "application/json", json);
    });

    server.begin();

    timeClient.begin();
    timeClient.setTimeOffset(7200); // GMT+2 за България
}

void loop() {
    static unsigned long lastMeasurement = 0;
    unsigned long currentMillis = millis();
    
    if (currentMillis - lastMeasurement >= SENSOR_READ_INTERVAL) {
        lastMeasurement = currentMillis;
        
        float h = dht.readHumidity();
        float t = dht.readTemperature();
        
        if (!isnan(h) && !isnan(t)) {
            // Добавяме проверка за buzzer
            handleBuzzer(t);
            
            timeClient.update();
            unsigned long timestamp = timeClient.getEpochTime();
            
            // Запазваме измерването в паметта и във файловата система
            measurements[measurementIndex].temperature = t;
            measurements[measurementIndex].humidity = h;
            measurements[measurementIndex].timestamp = timestamp;
            
            measurementIndex = (measurementIndex + 1) % HISTORY_SIZE;
            
            // Запазваме във файл
            saveMeasurement(t, h, timestamp);
        }
    }
    
    // Проверка на WiFi връзката
    reconnectWiFi();
    
    delay(1000);
}
