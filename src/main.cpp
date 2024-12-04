#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include <ESP32Servo.h>

// Define DEBUG mode
// Uncomment the following line to enable debug mode
//#define DEBUG 1

#ifdef DEBUG
  #define DEBUG_PRINT(x)    Serial.print(x)
  #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

// Error printing macros (always print)
#define ERROR_PRINT(x)    Serial.print(x)
#define ERROR_PRINTLN(x)  Serial.println(x)

#define OPEN_DEGREES 90
#define CLOSE_DEGREES 180
#define CLOSE_DELAY 1000

// Constants for dispensing rate and acceptable range
#define DISPENSING_RATE 200.0    // grams per second (calibrate this value)
#define MIN_AMOUNT 5            // minimum amount in grams
#define MAX_AMOUNT 200          // maximum amount in grams

// Replace with your network credentials
const char* ssid = "WIFI-NAME"; // WiFi Name
const char* password = "WIFI-PASSWD"; // WiFi Password
const int WIFI_CHANNEL = 6; // Speeds up the connection in Wokwi

// Global variables
const char deviceToken[] = "5ebb0b4f32f107b4ac6c3f262217d652"; // Replace with your device token
const char serverUrl[] = "http://140.238.182.239:8000/api/"; // Replace with your URL

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800; // Adjust according to your timezone (-10800 = São Paulo)
const int daylightOffset_sec = 3600; // Adjust for daylight saving time if applicable

// Store parsed data
// Adjust size based on the complexity of your data using ArduinoJson Assistant
// Assume the maximum JSON document size is 1024 bytes
DynamicJsonDocument responseDoc(1024);
JsonArray scheduleArray;

Servo servo1;
int servo1Pin = 13;

// Function declarations
void servoOpen();
void servoClose();
bool getJsonFromServer(const char* endpoint, JsonDocument& doc);
void getDeviceInfo();
void getDeviceCommandFromQueue();
void checkSchedule();
void executeCommand(const char* command, const char* info);
bool sendGetRequest(const char* url, char* responseBuffer, size_t bufferSize);
void ensureWiFiConnection();
int timeToMinutes(const char* timeStr);

unsigned long lastRequestTime = 0;
unsigned long lastQueueCheckTime = 0;
unsigned long lastTimeSync = 0;

void setup() {
  Serial.begin(115200);

  servo1.setPeriodHertz(50);
  servo1.attach(servo1Pin);

  Serial.print("Connecting to WiFi... ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    ERROR_PRINTLN("WiFi Failed!");
    return;
  }

  Serial.println("Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Initial request
  getDeviceInfo();
}

void loop() {
  ensureWiFiConnection();

  unsigned long currentMillis = millis();

  // Synchronize time every 24 hours
  if (currentMillis - lastTimeSync >= 86400000 || lastTimeSync == 0) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    lastTimeSync = currentMillis;
  }

  // Check every minute for the GET request
  if (currentMillis - lastRequestTime >= 60000 || lastRequestTime == 0) {
    lastRequestTime = currentMillis;
    getDeviceInfo();
  }

  // Check queue every 30 seconds
  if (currentMillis - lastQueueCheckTime >= 5000 || lastQueueCheckTime == 0) {
    lastQueueCheckTime = currentMillis;
    getDeviceCommandFromQueue();
  }

  // Add other non-blocking tasks here

  // Optional: Enter light sleep to save power
  // esp_light_sleep_start();
}

// Function to ensure Wi-Fi connection
void ensureWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    ERROR_PRINTLN("Wi-Fi disconnected, attempting reconnection...");
    WiFi.disconnect();
    WiFi.reconnect();

    unsigned long startAttemptTime = millis();

    // Wait for connection or timeout after 10 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Reconnected to Wi-Fi!");
    } else {
      ERROR_PRINTLN("Failed to reconnect to Wi-Fi.");
    }
  }
}

// Function to send GET request and update schedule
void getDeviceInfo() {
  const char endpoint[] = "device";
  if (getJsonFromServer(endpoint, responseDoc)) {
    // Extract schedule array from the JSON response
    checkSchedule();
  }
}

// Function to send GET request to execute a command from queue
void getDeviceCommandFromQueue() {
  const char endpoint[] = "command";
  if (getJsonFromServer(endpoint, responseDoc)) {
    const char* statusStr = responseDoc["status"];
    const char* messageStr = responseDoc["message"];
    if (strcmp(statusStr, "error") == 0) {
      ERROR_PRINT("Error: ");
      ERROR_PRINTLN(messageStr);
    } else {
      // Extract command object
      JsonObject commandObj = responseDoc["command"];
      if (!commandObj.isNull()) {
        const char* commandName = commandObj["command"];
        const char* commandInfo = commandObj["info"];

        // Execute command
        executeCommand(commandName, commandInfo);
      } else {
        ERROR_PRINTLN("Command object not found in the response.");
      }
    }
  }
}

void executeSchedule(int scheduleId) {
  // Convert scheduleId to string
  char scheduleIdStr[12]; // Buffer to hold the scheduleId as string
  snprintf(scheduleIdStr, sizeof(scheduleIdStr), "%d", scheduleId);

  const char endpoint[] = "schedule";
  char fullUrl[256];
  snprintf(fullUrl, sizeof(fullUrl), "%s%s?token=%s&id=%s", serverUrl, endpoint, deviceToken, scheduleIdStr);
  
  char responseBody[1024];
  if (sendGetRequest(fullUrl, responseBody, sizeof(responseBody))) {
    DEBUG_PRINTLN("Response Body:");
    DEBUG_PRINTLN(responseBody);
  } else {
    ERROR_PRINTLN("Failed to send GET request in executeSchedule.");
  }
}

// Function to check if schedule time matches current time and trigger an action
void checkSchedule() {
  if (responseDoc["schedule"].is<JsonArray>()) {
    JsonArray scheduleArray = responseDoc["schedule"].as<JsonArray>();
    if (!scheduleArray.isNull()) {
      struct tm timeinfo;
      if (!getLocalTime(&timeinfo)) {
        ERROR_PRINTLN("Failed to obtain time from NTP server.");
        return;
      }

      int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

      char currentDateStr[11];
      snprintf(currentDateStr, sizeof(currentDateStr), "%04d-%02d-%02d",
               timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

      for (JsonObject scheduleEntry : scheduleArray) {
        const char* scheduledTime = scheduleEntry["time"];
        if (scheduledTime == nullptr) {
          ERROR_PRINTLN("Scheduled time is missing in schedule entry.");
          continue;
        }

        const char* lastExecuted = scheduleEntry["lastExecuted"];
        if (lastExecuted == nullptr) {
          // Treat as never executed
          lastExecuted = "";
        }

        if (strncmp(lastExecuted, currentDateStr, 10) == 0) {
          DEBUG_PRINTLN("Schedule already executed today. Skipping.");
          continue;
        }

        int scheduledMinutes = timeToMinutes(scheduledTime);
        if (scheduledMinutes < 0) {
          ERROR_PRINTLN("Invalid scheduled time format.");
          continue;
        }

        DEBUG_PRINT("Current Minutes: ");
        DEBUG_PRINTLN(currentMinutes);
        DEBUG_PRINT("Scheduled Minutes: ");
        DEBUG_PRINTLN(scheduledMinutes);

        // Allow for a minute of delay
        if (currentMinutes == scheduledMinutes) {
          Serial.println("Schedule match found! Executing command.");

          if (scheduleEntry["id"].isNull() || scheduleEntry["command"].isNull() || scheduleEntry["info"].isNull()) {
            ERROR_PRINTLN("Schedule entry is missing required fields.");
            continue;
          }

          int scheduleId = scheduleEntry["id"];
          const char* command = scheduleEntry["command"];
          const char* info = scheduleEntry["info"];

          // Update schedule lastExecuted on the server
          executeSchedule(scheduleId);

          // Execute the command
          executeCommand(command, info);

          // Optionally, update lastExecuted locally (for in-memory tracking)
          scheduleEntry["lastExecuted"] = currentDateStr;

          break; // Exit after executing one schedule
        }
      }
    } else {
      Serial.println("No schedule data available.");
    }
  } else {
    Serial.println("'schedule' key not found in the response.");
  }
}

// Function for executing commands
void executeCommand(const char* command, const char* info) {
  DEBUG_PRINT("Executing command: ");
  DEBUG_PRINT(command);
  DEBUG_PRINT(" with info: ");
  DEBUG_PRINTLN(info);

  // Convert 'info' to an integer amount of food in grams
  int amountInGrams = atoi(info);

  // Validate the input
  if (amountInGrams <= 0) {
    Serial.println("Invalid amount of food provided. Please enter a positive integer value.");
    return;
  }

  // Check if the amount is within acceptable range
  if (amountInGrams < MIN_AMOUNT || amountInGrams > MAX_AMOUNT) {
    DEBUG_PRINTLN("Requested amount is out of acceptable range.");
    return;
  }

  // Calculate the required delay time in milliseconds
  unsigned long delayTime = (unsigned long)((amountInGrams / DISPENSING_RATE) * 1000);

  servoOpen();
  delay(delayTime);
  servoClose();

  DEBUG_PRINT("Dispensed ");
  DEBUG_PRINT(amountInGrams);
  DEBUG_PRINTLN(" grams of food.");
}


void servoOpen()
{
  servo1.write(OPEN_DEGREES);
}

void servoClose()
{
  servo1.write(CLOSE_DEGREES);
}

// Reusable function to send GET requests with retry mechanism
bool sendGetRequest(const char* url, char* responseBuffer, size_t bufferSize) {
  const int maxRetries = 3;
  int retryDelay = 500; // Start with 500 ms
  for (int attempt = 1; attempt <= maxRetries; ++attempt) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(5000); // Set timeout to 5 seconds
      http.begin(url);

      int httpResponseCode = http.GET();
      if (httpResponseCode > 0) {
        if (httpResponseCode == HTTP_CODE_OK) {
          String payload = http.getString();
          payload.toCharArray(responseBuffer, bufferSize);
          http.end();
          return true;
        } else {
          ERROR_PRINT("Unexpected response code: ");
          ERROR_PRINTLN(httpResponseCode);
        }
      } else {
        ERROR_PRINT("HTTP GET request failed, error: ");
        ERROR_PRINTLN(http.errorToString(httpResponseCode).c_str());
      }
      http.end();
    } else {
      ERROR_PRINTLN("Wi-Fi not connected");
    }

    // Retry after delay
    delay(retryDelay);
    retryDelay *= 2; // Exponential backoff
  }
  return false;
}

// Function to get JSON data from server
bool getJsonFromServer(const char* endpoint, JsonDocument& doc) {
  char fullUrl[256];
  snprintf(fullUrl, sizeof(fullUrl), "%s%s?token=%s", serverUrl, endpoint, deviceToken);
  DEBUG_PRINTLN(fullUrl);

  char responseBody[1024];
  if (sendGetRequest(fullUrl, responseBody, sizeof(responseBody))) {
    DEBUG_PRINTLN("Response Body:");
    DEBUG_PRINTLN(responseBody);

    // Parse response JSON
    DeserializationError error = deserializeJson(doc, responseBody);
    if (!error) {
      DEBUG_PRINTLN("Response parsed successfully.");
      return true;
    } else {
      ERROR_PRINT("Failed to parse response: ");
      ERROR_PRINTLN(error.c_str());
    }
  } else {
    ERROR_PRINTLN("Failed to send GET request in getJsonFromServer.");
  }
  return false;
}

// Function to convert time string "HH:MM" to minutes since midnight
int timeToMinutes(const char* timeStr) {
  int hours = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
  int minutes = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
  return hours * 60 + minutes;
}