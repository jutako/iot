/*

An Arduino program that monitors a blinking led and reports pulse counts to
various web based services. Data is sent to:
* Blynk App (https://www.blynk.cc/)
* MariaDB database via PHP script that interpretes the POST request
* MQTT message broker (http://mqtt.org/)

Custom MariaDB back-end is used since Blynk service averages stored data over
longer time intervals to save space.
MQTT enables other devices to get updates from this device and act accordingly.

Additional features include:
* Arduino OTA over the air software updates (works only on Windows)

Hardware:
* Wemos D1 mini pro (https://wiki.wemos.cc/products:d1:d1_mini_pro)
* LM393 base photodetector module, such as https://www.ebay.com/itm/LM393-light-Sensor-Module-3-3-5V-input-light-Sensor-for-Arduino-Raspberry-pi/251903122999?hash=item3aa6989e37:g:RvsAAOSw7aBVHgsY

Insipiration and code for MQTT and Arduino OTA found in:
https://github.com/bruhautomation

Howto:
* install all required libararies to your Arduino IDE
* remember to fill-in all strings that have angle brackets "<>"
* comment out unnecessary features

Authors: Jussi Korpela and Andreas Henelius

*/

/* Libraries */
#include <ESP8266WiFi.h> // for wifi connection
#include <BlynkSimpleEsp8266.h> // for Blynk functionality, including wifi!
#include <ESP8266HTTPClient.h> // for HTTP POST requests

/* for arduinoOTA */
#include <ArduinoOTA.h>

/* for MQTT */
#include <PubSubClient.h>
#include <ArduinoJson.h>


/* ========================== Settings ========================== */

// Wifi:
char ssid[] = "<your-wifi-ssid>"; // wifi ssid
char pass[] = "<your-wifi-password>"; // wifi password

// Blynk:
char auth[] = "<your Blynk project token>"; // Blynk project token
const unsigned long MSG_INTERVAL = 5000; // Blynk update interval in milliseconds
/*
To get all data:

pulses:
http://blynk-cloud.com/8eb45ed4de104c8e9477c43d98cf579a/data/V2
*/

// for arduinoOTA over the air updates:
const char* device_name = "<device name for Arduino OTA>"; // also for MQTT!
const char* ota_password = "<device password for Arduino OTA>";

// for HTTP POST requests
const char* POST_URL = "<URL for a resource that stores this data>";

// for MQTT:
//const char* mqtt_server = "192.168.10.56";
#define mqtt_server "<MQTT server IP / URL>"
const char* mqtt_user = "<MQTT user name>";
const char* mqtt_password = "<MQTT password>";
const int mqtt_port = 1883;
const char* mqtt_pubtopic = "powermeter/json";
const int BUFFER_SIZE = 300;


/* Define hardware settings */
#define interruptPin2 D3 // counts pulses
#define ledPin D4 // blinks when sending data
/*
Photodetector digital out at D3:
* D3 at state HIGH when external light is off and detector's green led is off
* D3 at state LOW when external light is on and detector's green led is on

Photodetector, D3 pin and a "FALLING" intrrupt gives one interrupt when external light goes on (and D3 goes LOW)
and very often another interrupt when external light goes off (D3 goes back HIGH)  detector state change.
The latter does not always happen.

Photodetector, D3 pin and a "RISING" intrrupt give one or more interrupts when light source goes out (D3 goes back HIGH).

Changing photodetectors sensitivity double interrupts sometimes disappear.
Best setting seems to be just above the threshold that triggers the detector.
*/


/* Global variables */
volatile int count = 0; // Pulse counter
const double pulses_per_kwh = 1000.0; //How many pulses equal one kWh?

/* Initialize Blynk timer */
BlynkTimer timer;

/* Initialize HTTPClient */
HTTPClient http;

/* Initialize MQTT */
WiFiClient espClient;
PubSubClient client(espClient);



/* ========================== Helper functions ========================== */

/* MQTT reconnect */
/* Try to connect once. Return connection state. */
boolean reconnect() {
    if (!client.connected()) {
        client.connect(device_name, mqtt_user, mqtt_password);
    }
    return client.connected();
}


/* Send state via MQTT as JSON */
void sendState(int val_blink_count, double val_power) {
    StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();

    root["pulses"] = (String)val_blink_count;
    root["power"] = (String)val_power;

    char buffer[root.measureLength() + 1];
    root.printTo(buffer, sizeof(buffer));

    Serial.println(buffer);
    client.publish(mqtt_pubtopic, buffer, true);
}


/* Handle sending of data at Blynk timer intervals */
/* All sending should be complete before next timer wake up! */
void blynkTimerEvent() {
  Serial.println("timer event started...");
    digitalWrite(ledPin, LOW);

    int current_count = count;
    count = 0;

    // (V0) energy
    double val_energy = current_count / pulses_per_kwh;
    //Serial.println(val_energy);
    Blynk.virtualWrite(V0, val_energy);

    // (V1) power
    double val_power = (current_count / pulses_per_kwh) / (MSG_INTERVAL / 1000.0 / 3600.0);
    //Serial.println(val_power);
    Blynk.virtualWrite(V1, val_power);

    // (V2) pulse count
    Blynk.virtualWrite(V2, current_count);

    digitalWrite(ledPin, HIGH);

    // Send to POST_URL
    http.POST(String("pulses=") + String(current_count) + String("&energy=") + String(val_energy) + String("&power=") + String(val_power));

    /* MQTT
    Send data if connection is available, otherwise try to reconnect once.
    This step is just nice to have, not critical.
    */
    if (!client.connected()) {
        Serial.println("MQTT: reconnecting ...");
        reconnect(); // but not send to save time
    } else {
        Serial.println("MQTT: sending ...");
        sendState(current_count, val_power);
    }
}


/* Increments counter via pin interrupt */
void incrementCounter() {
    count++;
}


/* ========================== Main program ========================== */

/* Setup of main program*/
void setup() {

    Serial.begin(9600); // open the serial port at 9600 bps:

    /* Led pin (to see when device sends data) */
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, HIGH); // turn off

    /* Set up photodetector pin */
    pinMode(interruptPin2, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(interruptPin2), incrementCounter, FALLING); //interrupt to increment counter

    /* Set up http */
    http.begin(POST_URL); //Specify request destination
    http.addHeader("Content-Type", "text/plain"); //Specify content-type header

    /* ArduinoOTA */

    // Port defaults to 8266
    // ArduinoOTA.setPort(8266);

    // Hostname defaults to esp8266-[ChipID]
    ArduinoOTA.setHostname(device_name);

    // No authentication by default
    ArduinoOTA.setPassword(ota_password);

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
        else // U_SPIFFS
        type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin(); // start arduinoOTA


    /* Set up Blynk */
    Blynk.begin(auth, ssid, pass);
    timer.setInterval(MSG_INTERVAL, blynkTimerEvent);


    /* Connect MQTT */
    client.setServer(mqtt_server, mqtt_port);
    client.connect(device_name, mqtt_user, mqtt_password); //call after Blynk has established wifi (?)

}


/* Main program */
void loop() {
    ArduinoOTA.handle();
    Blynk.run();
    timer.run();
}
