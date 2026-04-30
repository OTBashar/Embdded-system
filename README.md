# Smart Plants Irrigation System

A smart irrigation prototype built with two ESP32 microcontrollers.  
The system measures soil moisture, checks the water tank level, controls a 5 V water pump through a relay, and provides a web dashboard for monitoring and manual control.

## Overview

The project is designed to reduce irregular plant watering. One ESP32 is placed near the plant to read soil moisture. A second ESP32 is placed near the water tank and pump to read the water level, control the relay, and host the dashboard.

The two ESP32 boards communicate over the same Wi-Fi network using simple HTTP requests.

## Main Features

- Soil moisture monitoring using an analog soil moisture sensor.
- Water tank level monitoring using an analog water level sensor.
- Automatic irrigation when the soil is dry.
- Manual pump control from a web dashboard.
- Timed override mode for controlled pump testing.
- Water-level safety check to avoid running the pump when the tank is low.
- Bilingual web dashboard with Arabic and English labels.
- Battery-powered pump stage using a 3.8 V battery stepped up to 5 V.

## System Architecture

```text
ESP32 Soil Node                 ESP32 Main Controller                 User Device
----------------                ----------------------                -----------
Soil moisture sensor   --->     HTTP GET /read                         Browser dashboard
GPIO34                          Water level sensor on GPIO34           Manual commands
/read JSON endpoint             Relay control on GPIO26
                                5 V pump control through relay
                                /status dashboard API
