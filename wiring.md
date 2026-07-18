Relay	ESP32#2 GPIO
16 CHANNEL RELAY
Relay 1	GPIO16
Relay 2	GPIO17
Relay 3	GPIO18
Relay 4	GPIO19
Relay 5	GPIO21
Relay 6	GPIO22
Relay 7	GPIO23
Relay 8	GPIO25
Relay 9	GPIO26
Relay 10	GPIO27
Relay 11	GPIO32
Relay 12	GPIO33
Relay 13	GPIO4
Relay 14	GPIO5
Relay 15	GPIO12
Relay 16	GPIO13



Relay	ESP32#3 GPIO
8CHANNEL RELAYS
Relay 1	GPIO16
Relay 2	GPIO17
Relay 3	GPIO18
Relay 4	GPIO19
Relay 5	GPIO21
Relay 6	GPIO22
Relay 7	GPIO23
Relay 8	GPIO25
4CHANEL RELAY
Relay 9	GPIO26
Relay 10	GPIO27
Relay 11	GPIO32
Relay 12	GPIO33




ESP32#4 EXTRAS
Device	GPIO	Notes
MG996R Servo #1 (Camera Pan)	D18 (GPIO18)	PWM output
MG996R Servo #2 (Camera Tilt)	D19 (GPIO19)	PWM output
Active Buzzer	D26 (GPIO26)	Digital output
Red Alert LED
And Green	D27 (GPIO27)
D28 GREEN	Digital output
TB6600 STEP	D32 (GPIO32)	Pulse output
TB6600 DIR	D33 (GPIO33)	Direction output
TB6600 ENABLE	D25 (GPIO25) (optional)	Can be tied active if you don't need software enable
________________________________________
ESP32#1 Wiring
1. Servo #1 (Pan)
Servo	ESP32
Signal	D18
VCC	External 5V
GND	Common GND
________________________________________
2. Servo #2 (Tilt)
Servo	ESP32
Signal	D19
VCC	External 5V
GND	Common GND
________________________________________
3. Active Buzzer
Buzzer	ESP32
SIG	D26
VCC	5V
GND	GND
________________________________________
4. Red LED
If using a normal LED:
LED	ESP32
Anode (+)	D27 through 220Ω resistor
Cathode (-)	GND
________________________________________
5. TB6600
TB6600	ESP32
PUL−	D32
DIR−	D33
ENA−	D25 (optional)
PUL+	3.3V (or 5V if your driver requires it)
DIR+	3.3V (or 5V if your driver requires it)
ENA+	3.3V (or 5V if used)

Yes. Since you're using an external 5V power supply, it's actually better to power the sensors directly from it where appropriate. Here's the complete wiring.
________________________________________
1. 5V Power Supply
Power Supply	Connect To
+5V	ESP32 VIN (5V pin)
GND	ESP32 GND
This powers the ESP32.
The ESP32 then generates its own 3.3V output for the 3.3V sensors.
________________________________________
2. DS18B20 #1 (Module)
DS18B20	Connect To
VCC	ESP32 3.3V
GND	Power Supply GND (or ESP32 GND—they are the same common ground)
DAT	ESP32 GPIO4
________________________________________
3. DS18B20 #2
DS18B20	Connect To
VCC	ESP32 3.3V
GND	Power Supply GND
DAT	ESP32 GPIO5
________________________________________
4. TDS Sensor #1
TDS Board	Connect To
V	ESP32 3.3V
GND	Power Supply GND
A	ESP32 GPIO34
Probe +	TDS Probe
Probe -	TDS Probe
________________________________________
5. TDS Sensor #2
TDS Board	Connect To
V	ESP32 3.3V
GND	Power Supply GND
A	ESP32 GPIO35
Probe +	TDS Probe
Probe -	TDS Probe
________________________________________
6. DFRobot DO Sensor
DO Sensor	Connect To
+	ESP32 3.3V
-	Power Supply GND
A	ESP32 GPIO33
________________________________________
7. pH Sensor
PH4502C	Connect To
V+	Power Supply +5V
G	Power Supply GND
PO	ESP32 GPIO32
Ignore TO and DO.
________________________________________
8. MQ137
MQ137	Connect To
VCC	Power Supply +5V
GND	Power Supply GND
AO	ESP32 GPIO36
DO	ESP32 GPIO25 (optional)
________________________________________
9. Ultrasonic #1 (JSN-SR04T)
JSN-SR04T	Connect To
5V	Power Supply +5V
GND	Power Supply GND
Trig	ESP32 GPIO16
Echo	ESP32 GPIO17 (through recommended voltage divider)
________________________________________
10. Ultrasonic #2
JSN-SR04T	Connect To
5V	Power Supply +5V
GND	Power Supply GND
Trig	ESP32 GPIO18
Echo	ESP32 GPIO19 (through recommended voltage divider)
________________________________________
11. Ultrasonic #3
JSN-SR04T	Connect To
5V	Power Supply +5V
GND	Power Supply GND
Trig	ESP32 GPIO21
Echo	ESP32 GPIO22 (through recommended voltage divider)
________________________________________
ESP32 Connections
ESP32 Pin	Connected Devices
VIN (5V)	Power Supply +5V
GND	Power Supply GND
3.3V	DS18B20 #1, DS18B20 #2, TDS #1, TDS #2, DO Sensor
GPIO4	DS18B20 #1 Data
GPIO5	DS18B20 #2 Data
GPIO32	pH PO
GPIO33	DO Analog Output
GPIO34	TDS #1 Analog Output
GPIO35	TDS #2 Analog Output
GPIO36	MQ137 AO
GPIO25	MQ137 DO (optional)
GPIO16	Ultrasonic #1 Trig
GPIO17	Ultrasonic #1 Echo
GPIO18	Ultrasonic #2 Trig
GPIO19	Ultrasonic #2 Echo
GPIO21	Ultrasonic #3 Trig
GPIO22	Ultrasonic #3 Echo
________________________________________
Power Distribution Summary
Directly from the 5V Power Supply
•	ESP32 VIN
•	pH Sensor
•	MQ137
•	Ultrasonic #1
•	Ultrasonic #2
•	Ultrasonic #3
From the ESP32 3.3V Pin
•	DS18B20 #1
•	DS18B20 #2
•	TDS #1
•	TDS #2
•	DO Sensor
Connected to Power Supply GND
•	ESP32 GND
•	DS18B20 #1 GND
•	DS18B20 #2 GND
•	TDS #1 GND
•	TDS #2 GND
•	DO Sensor GND
•	pH Sensor GND
•	MQ137 GND
•	Ultrasonic #1 GND
•	Ultrasonic #2 GND
•	Ultrasonic #3 GND

