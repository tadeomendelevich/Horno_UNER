<div align="center">

# 🍞 Horno de Leudado — Control por TRIAC con Dashboard Web

**Control de temperatura y humedad en tiempo real con acceso remoto global**

[![C++](https://img.shields.io/badge/C++-ESP--IDF-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://docs.espressif.com/projects/esp-idf/)
[![ESP32](https://img.shields.io/badge/ESP32-DevKit%20V1-E7352C?style=for-the-badge&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![MQTT](https://img.shields.io/badge/MQTT-HiveMQ%20Cloud-660066?style=for-the-badge&logo=eclipsemosquitto&logoColor=white)](https://www.hivemq.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Build%20System-F5822A?style=for-the-badge&logo=platformio&logoColor=white)](https://platformio.org/)

*Universidad Nacional de Entre Ríos · Facultad de Ciencias de la Alimentación*  
*Cátedra: Electrónica de Potencia*

</div>

---

## 📋 Descripción

Sistema embebido para el control de temperatura y humedad de un **horno de leudado de pan** mediante **disparo por ángulo de fase de un TRIAC**. Incluye dashboard web accesible desde cualquier dispositivo en cualquier red vía MQTT (HiveMQ Cloud) y también desde la red local por HTTP embebido en la ESP32.

---

## ⚡ Hardware

| Componente | Función | Conexión |
|---|---|---|
| **ESP32 DevKit V1** | Microcontrolador principal | — |
| **AM2320** | Temperatura + Humedad (I²C) | SDA GPIO 21 / SCL GPIO 22 |
| **PC814** | Detección de cruce por cero | GPIO 27 |
| **BC547 + MOC3021 + TIC226** | Driver + disparo TRIAC | GPIO 18 |

> Red eléctrica: **220 V / 50 Hz**

---

## 🏗️ Arquitectura del sistema

```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32                                 │
│  AM2320 ──I²C──► Lectura T°/H%                              │
│  PC814  ──────► Cruce por cero ──► ISR ──► Disparo TRIAC    │
│                                                              │
│  HTTP Server (puerto 80)  ◄──► Red local                    │
│  MQTT Client ─────────────────────────────────────────────┐ │
└───────────────────────────────────────────────────────────┼─┘
                                                            │
                                                    HiveMQ Cloud
                                                   (TLS port 8883)
                                                            │
                                              Dashboard Web (Vercel)
                                              WebSocket WSS :8884
```

---

## 🛠️ Stack tecnológico

| Capa | Tecnología |
|---|---|
| **Firmware** | ESP-IDF C++ (PlatformIO) |
| **Broker MQTT** | HiveMQ Cloud — TLS port 8883 (gratuito) |
| **Dashboard remoto** | HTML/JS estático en Vercel — WebSocket WSS port 8884 |
| **Acceso local** | Servidor HTTP embebido en ESP32 (puerto 80) |

---

## ✨ Funcionalidades

- 🎚️ **Control de potencia 0–100%** con slider continuo
- 🔘 **Botones preset**: 25 / 50 / 75 / 100 % y parada de emergencia
- 📉 **Gráfica didáctica** del disparo TRIAC (senoide + zona de conducción)
- 📡 **Indicador de estado MQTT** en tiempo real
- 📱 **Diseño responsive** — funciona en celular y PC
- 🌍 **Acceso remoto** desde cualquier red vía dashboard en Vercel

---

## 📡 Tópicos MQTT

| Tópico | Dirección | Payload |
|---|---|---|
| `horno/datos` | ESP32 → Dashboard | `{"temp": X, "hum": X, "power": X, "zc": X, "fire": X}` |
| `horno/control` | Dashboard → ESP32 | `{"power": 0-100}` |

---

## 📶 WiFi — Provisioning y recuperación automática

La ESP32 **no tiene credenciales hardcodeadas**. El flujo al arrancar:

1. Carga credenciales desde **NVS** (flash no volátil). Si no hay, usa red de fábrica (`FCAL`).
2. Intenta conectarse (**hasta 10 reintentos**).
3. Si falla y existe un backup (última red exitosa), intenta con esa.
4. Si el backup funciona → **revierte credenciales en NVS automáticamente**.
5. Si ambas fallan → **modo AP**: levanta la red `Horno-Config` (pass: `horno1234`) y sirve un portal en `http://192.168.4.1`.

> El backup se guarda en NVS cada vez que una conexión resulta exitosa. Si se carga una red incorrecta desde el portal, la ESP se recupera sola.

### Cambiar red WiFi

En modo normal, el dashboard incluye un botón **"Cambiar red WiFi"** (`/wifi`) que abre el portal de configuración.

---

## 🚀 Configuración rápida

```bash
# 1. Clonar el repositorio
git clone https://github.com/tadeomendelevich/Horno_UNER.git
cd Horno_UNER

# 2. Flashear con PlatformIO
pio run --target upload
```

Al primer arranque sin NVS configurado, el dispositivo se conecta a la red de fábrica (`FCAL`).  
Para cambiar la red: usar el botón del dashboard o conectarse a `Horno-Config` → `http://192.168.4.1`.

---

<div align="center">

**Autor:** [Tadeo Mendelevich](https://github.com/tadeomendelevich)  
*Ingeniería en Sistemas de Información — UNER*

</div>
