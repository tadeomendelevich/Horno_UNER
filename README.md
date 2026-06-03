# Horno de Leudado de Pan — ESP32 + TRIAC

**Universidad Nacional de Entre Ríos · Facultad de Ciencias de la Alimentación**  
Electrónica de Potencia

Control de temperatura y humedad de un horno de leudado mediante disparo por ángulo de fase de un TRIAC, con dashboard web accesible desde cualquier dispositivo vía MQTT (HiveMQ Cloud) o red local (HTTP embebido).

## Hardware

| Componente | Función | Pin ESP32 |
|---|---|---|
| ESP32 DevKit V1 | Microcontrolador | — |
| AM2320 | Temperatura + Humedad (I2C) | SDA 21 / SCL 22 |
| PC814 | Detección cruce por cero | GPIO 27 |
| BC547 + MOC3021 + TIC226 | Disparo TRIAC | GPIO 18 |

Red: 220 V / 50 Hz

## Stack

- **Firmware:** ESP-IDF C++ (PlatformIO)
- **Broker MQTT:** HiveMQ Cloud (TLS port 8883, gratuito)
- **Dashboard remoto:** HTML/JS estático en Vercel (WebSocket WSS port 8884)
- **Acceso local:** Servidor HTTP embebido en la ESP32 (puerto 80)

## Arquitectura

```
ESP32 ──publica──► HiveMQ Cloud ◄──suscribe── Dashboard (Vercel)
      horno/datos  (cada 2 s)   horno/control  (slider / botones)
```

## Funcionalidades

- Ajuste de potencia 0–100 % con slider, botones preset (25/50/75/100 %) y parada de emergencia
- Gráfica didáctica del disparo TRIAC (senoide + zona de conducción)
- Indicador de estado MQTT en tiempo real
- Diseño responsive (celular y PC)
- Acceso remoto desde cualquier red vía dashboard en Vercel

## WiFi — provisioning y fallback

La ESP32 no tiene credenciales hardcodeadas en producción. El flujo al arrancar es:

1. Carga credenciales desde NVS (flash). Si no hay, usa las de fábrica (`FCAL`).
2. Intenta conectarse (hasta 10 reintentos).
3. **Si falla** y existe un backup (última red que funcionó exitosamente), intenta esa red.
4. Si el backup funciona, revierte las credenciales en NVS automáticamente.
5. Si ambas fallan → **modo AP**: levanta la red `Horno-Config` (pass `horno1234`) y sirve un portal de configuración en `http://192.168.4.1`.

El backup se guarda en NVS con claves `ssid_bak` / `pass_bak` cada vez que una conexión es exitosa. Así, si se carga una red incorrecta por el portal, la ESP recupera sola la conexión sin intervención.

### Cambiar red desde la app

En modo normal, el dashboard incluye un botón "Cambiar red WiFi" (`/wifi`) que abre el mismo portal de configuración.

## Configuración rápida

1. Flashear con PlatformIO: `pio run --target upload`
2. Al primer arranque sin NVS, conecta a la red de fábrica (`FCAL`).
3. Para cambiar la red: usar el botón del dashboard o conectarse a `Horno-Config` / `http://192.168.4.1`.

## Temas MQTT

| Topic | Dirección | Contenido |
|---|---|---|
| `horno/datos` | ESP32 → Dashboard | JSON: temp, hum, power, zc, fire |
| `horno/control` | Dashboard → ESP32 | JSON: `{"power": 0-100}` |
