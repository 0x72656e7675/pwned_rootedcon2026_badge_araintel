# badge_rootedcon2026_araintel

Proyecto para un badge ESP32 con pantalla ST7735 y una app principal: `pwned_rootedcon_2026_araintel`.

La app arranca en una portada `2026 ROOTEDCON ESP32`, muestra la crew en pixel art y permite interactuar con cada personaje en modo tamagotchi.

## Requisitos

- `PlatformIO`
- una placa `ESP32 DOIT DEVKIT V1` o compatible
- pantalla `ST7735`
- botones cableados como en este proyecto
- cable USB para alimentación y flasheo

## Uso rápido

Clona el repo y entra en la carpeta:

```bash
git clone <url-del-repo>
cd badge_rootedcon2026_araintel
```

Compilar:

```bash
pio run -e pwned_rootedcon_2026_araintel
```

Listar puertos serie disponibles:

```bash
pio device list
```

Flashear:

```bash
pio run -e pwned_rootedcon_2026_araintel -t upload --upload-port <puerto>
```

Monitor serie:

```bash
pio device monitor -p <puerto> -b 115200 --filter default
```

Ejemplos de `<puerto>` según sistema:

- macOS: `/dev/cu.usbserial-XXXX` o `/dev/cu.usbmodemXXXX`
- Linux: `/dev/ttyUSB0` o `/dev/ttyACM0`
- Windows: `COM3`, `COM4`, `COM5`, etc.

Si PlatformIO detecta el puerto automáticamente, puedes probar también:

```bash
pio run -e pwned_rootedcon_2026_araintel -t upload
```

## Qué hace la app

Tiene dos vistas:

- `HOME`: portada RootedCON con selección de personajes
- `PET`: vista de interacción/cuidado

Controles en `HOME`:

- `UP` o `LEFT`: personaje anterior
- `DOWN` o `RIGHT`: personaje siguiente
- `SELECT`: entrar en el personaje

Controles en `PET`:

- `UP`: dar comida
- `RIGHT`: jugar
- `DOWN`: dormir
- `LEFT`: limpiar
- `SELECT`: cariño
- `EXTRA`: volver atrás

## Mapa de pines

### Pantalla ST7735

| Función | GPIO |
|---|---:|
| `MOSI` | `23` |
| `MISO` | `-1` |
| `SCLK` | `18` |
| `CS` | `5` |
| `DC` | `16` |
| `RST` | `4` |
| `BL` | `12` |
| `SCREEN_EN` | `21` |
| `AUX_POWER` | `0` |

Notas:

- `BL` está en activo bajo.
- `SCREEN_EN` se pone en alto para habilitar la pantalla.
- La orientación actual se fija en el código con `setRotation(3)`.

### Botones

Todos los botones están en `INPUT_PULLUP` y se consideran pulsados en `LOW`.

| Botón | GPIO |
|---|---:|
| `UP` | `27` |
| `DOWN` | `15` |
| `LEFT` | `25` |
| `RIGHT` | `26` |
| `SELECT` | `13` |
| `EXTRA` | `33` |

## Plano rápido

```text
ESP32 -> TFT
23 -> MOSI
18 -> SCLK
 5 -> CS
16 -> DC
 4 -> RST
12 -> BL
21 -> SCREEN_EN
 0 -> AUX_POWER

ESP32 -> Botones
27 -> UP
15 -> DOWN
25 -> LEFT
26 -> RIGHT
13 -> SELECT
33 -> EXTRA
```

## Estructura del proyecto

- `platformio.ini`: entorno PlatformIO
- `rooted2026_renvu_badge.csv`: tabla de particiones
- `esp32/pwned_rootedcon_2026_araintel.cpp`: app principal

## GitHub

El proyecto ignora archivos locales y artefactos de build:

- `.pio/`
- `.DS_Store`
- `.vscode/`
- `platformio_override.ini`

Así el repo se puede subir limpio a GitHub sin ficheros temporales de cada máquina.
