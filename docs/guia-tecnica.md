# ds5haptics — guía técnica

Háptica HD del DualSense (PS5) por **Bluetooth en Windows**, usando el dongle común,
sin cable y sin Raspberry Pi.

- **Fase 1:** manda audio de háptica (archivos o señales de prueba) a los actuadores.
- **Fase 2:** convierte en vivo lo que suena en la PC (juegos, música, videos) en háptica.
- **Fase 3:** crea un DualSense USB virtual: los juegos ven un mando "por cable", con gatillos adaptativos,
  barra de luz y **háptica nativa** (la que el juego manda por audio), todo por Bluetooth.

## Uso diario, respaldo e instalación rápida

### Arranque con Windows

```
ds5haptics autostart on       arranca solo al iniciar sesión, sin ventana (con parlante)
ds5haptics autostart off      lo desactiva
ds5haptics autostart status   muestra si está activado
ds5haptics background         lo inicia ahora, sin ventana
ds5haptics stop               detiene el que corre sin ventana
```

En segundo plano espera a que prendas el mando (botón PS) y se conecta solo. Lo que
normalmente muestra la consola queda en `ds5haptics.log`, junto al programa.

### Ícono en la bandeja y estadísticas (botón de micrófono)

Mientras ds5haptics corre aparece un ícono con forma de mando junto al reloj
(Windows 11 lo puede dejar dentro de la flechita `^`: arrastralo a la barra o activalo en
Configuración > Personalización > Barra de tareas > Otros iconos de la bandeja del sistema).

- **Pasar el mouse por encima:** modo y batería.
- **Clic izquierdo o botón de micrófono** (o el atajo de `atajo_estadisticas`, si pusiste uno)**:** muestra u oculta el cuadro de estadísticas arriba a la derecha.
  Es semitransparente, deja pasar los clics al juego y no le quita el foco. Muestra batería
  (dibujo y porcentaje), juego y perfil, conexión Bluetooth, háptica con su nivel, gatillos,
  parlante con el volumen que pide el juego, y la latencia que agrega ds5haptics.
- **Clic derecho:** *Ver estadísticas*, *Iniciar con Windows* (con tilde) y *Salir*.
- **Después de *Salir*:** se vuelve a abrir con el acceso directo **ds5haptics** del menú Inicio
  (lo crea el programa la primera vez que corre) o con doble clic en `ds5haptics.exe`. Sin
  argumentos, el programa arranca en segundo plano (como `ds5haptics background`) y, si ya
  estaba corriendo, lo avisa. Desde una consola, sin argumentos muestra la ayuda.
- Los juegos en pantalla completa "exclusiva" pueden tapar el cuadro; en modo ventana sin
  bordes (borderless) siempre se ve.
- Nada depende del color: cada estado se dice con texto. `--no-tray` lo desactiva.

### Perfiles por juego (volumen del parlante y háptica)

Ejecutá **`perfiles.bat`** (o `ds5haptics config`): abre `ds5haptics.ini` en el Bloc de notas.

```
[general]
parlante = 1              volumen extra del parlante del mando (1 = normal, hasta 10)
haptica = 1               intensidad extra de la háptica
volumen_mando_max = no    "si" ignora el volumen de parlante que pide el juego y usa el máximo

[phantom]                 parte del nombre del .exe del juego
parlante = 3
```

- El programa detecta el juego en primer plano y aplica su perfil solo. Al guardar el archivo,
  los cambios se aplican en vivo.
- El volumen extra usa un limitador suave: los sonidos bajos (pisadas) suben y los fuertes
  no se distorsionan.
- El nombre exacto del juego aparece en `ds5haptics.log` como "Juego en primer plano: ...".
  También se registra el volumen de parlante que pide cada juego ("El juego fijó el volumen...").
- El perfil solo cambia al pasar a una app con perfil o a una ventana a pantalla completa:
  ir un momento al navegador no le quita el perfil al juego.

### Juegos que no admiten el DualSense: modo PS4 y modo Xbox

El mando virtual puede presentarse de tres formas (`modo` en `ds5haptics.ini`):

| Modo | Qué ve el juego | Qué funciona |
|---|---|---|
| `ps5` | DualSense por cable (por defecto) | Todo: háptica, gatillos adaptativos, parlante, luces |
| `ps4` | DualShock 4 por cable (054C:05C4) | Botones, sticks, panel táctil, giroscopio, vibración clásica, color de la barra |
| `xbox` | Mando de Xbox 360 (XInput) | Botones, sticks, gatillos, vibración clásica. Lo acepta casi cualquier juego |

Se elige por juego, en su perfil (probar primero `ps4`; si el juego no lo toma, `xbox`):

```
[nombredeljuego]          parte del nombre del .exe (aparece en ds5haptics.log)
modo = ps4
```

- Al pasar el juego a primer plano, el programa "desenchufa" el mando virtual y "enchufa" el
  del modo pedido (tarda unos segundos). Si el juego no detecta el cambio, reinicialo.
  **Al cerrar el juego vuelve solo al modo PS5** (barra azul).
- **Xbox automático:** si un juego sin perfil arranca y en 20 segundos no le habló al DualSense
  (un juego que lo admite pide su calibración o le manda luces y gatillos al arrancar), pero sí
  cargó una librería de mandos de Xbox (XInput o DirectInput), el programa lo pasa a modo Xbox y
  lo anota al final de `ds5haptics.ini` (`modo = xbox`), así la próxima vez arranca directo en
  Xbox. La primera vez puede hacer falta reiniciar el juego. Navegadores, reproductores de video
  y launchers nunca cuentan como juegos. Un perfil con `modo = ps5` no se evalúa nunca.
  Si un juego quedó mal anotado, cambiá su `modo` o borrá su perfil. Se desactiva con
  `xbox_automatico = no` en `[general]`.
- Para usar un modo siempre, ponelo en `[general]`.
- La barra de luz indica el modo: azul = PS5, violeta = PS4, verde = Xbox.
- En modo Xbox: Cruz = A, Círculo = B, Cuadrado = X, Triángulo = Y, Create o clic del panel = Back,
  Options = Start, PS = botón Guía (puede abrir la Game Bar de Windows).
- Para probar sin tocar el archivo: `ds5haptics virtual --mode xbox` (o `ps4`).

### Aviso de batería baja

Cuando al mando le queda 15% o menos y no está cargando, **la luz del botón de micrófono queda
pulsando**. Vuelve a la normalidad al enchufarlo. El umbral se cambia con `bateria_baja = 15`
en `[general]` (`0` lo desactiva). El mando informa la batería en pasos de 10%.
`ds5haptics battery` muestra el nivel actual, y cada cambio queda anotado en `ds5haptics.log`.

### Ahorro de batería

Lo que más gasta la batería del mando es la háptica, los gatillos, el parlante, la barra de luz y
la radio Bluetooth. Con un juego abierto, Windows manda audio todo el tiempo (aunque sea
silencio), y ds5haptics mandaba un reporte 0x36 (398 bytes) cada 10,7 ms: la radio del mando no
paraba nunca.

- `ahorro_bateria = si` (por defecto): si la háptica y el parlante están en silencio desde hace
  ~0,3 s, no se manda nada. Con el primer sonido se vuelve a mandar enseguida. El cuadro de
  estadísticas muestra el % de envíos evitados.
- `brillo_luz = 0 a 100`: escala todos los colores de la barra de luz (los del juego, los de DSX
  y los del modo). 0 la apaga.

Cuánto dura más la batería depende del juego: en uno con mucha háptica todo el tiempo, el ahorro
es poco; en uno con silencios, menús o diálogos, más.

### Luz según la pantalla

Con `luz_pantalla = si` y un juego abierto (`src/screen_capture.h`, `src/ambient_color.h`):

1. Unas 15 veces por segundo se toma la pantalla donde está el juego con la duplicación de
   escritorio de DXGI (la de los programas de grabación). La placa de video la achica a ~64 px
   de ancho (mipmaps): se leen unos miles de píxeles y casi no gasta CPU.
2. Se agrupan los píxeles por tono (24 grupos); cada uno pesa más cuanto más saturado y
   brillante es (el negro, los grises y el texto blanco casi no cuentan). Gana el grupo con más
   peso. Una pantalla casi sin color da un blanco tenue; una oscura, luz tenue.
3. El color cambia suave (~150 ms) y, con `luz_ritmo = si`, el brillo late con la háptica del
   juego: sube con cada golpe y baja en ~250 ms.
4. Mientras está activa, los colores que manda el juego o un mod de DSX no se aplican. Al cerrar
   el juego vuelve el color de siempre. `brillo_luz` también se aplica.

No ve contenido protegido (videos con DRM salen negros). Si cambia la resolución o el juego
pasa a otro monitor, la captura se vuelve a abrir sola.

### Mods hechos para DSX (experimental)

Algunos mods de la comunidad (por ejemplo, uno para RDR2) mandan a DSX por UDP, a
`127.0.0.1:6969`, mensajes JSON como este:

```json
{"instructions":[{"type":1,"parameters":[0,2,16,2,6,8]},{"type":2,"parameters":[0,255,0,0]}]}
```

`type 1` es un gatillo (`[mando, 1 = L2 / 2 = R2, modo, parámetros...]`), `type 2` el color de la
barra de luz (`[mando, R, G, B]`) y `type 7` vuelve todo a lo normal. ds5haptics escucha en ese
puerto (solo desde la misma PC), traduce cada modo de DSX al efecto equivalente del DualSense
(resistencia, arco, arma, vibración, galope, máquina) y lo manda directo al mando real, así que
anda en cualquier modo, incluso xbox y ps4. Responde como DSX, por si el mod lo espera.

- Se desactiva con `mods_dsx = no` en `[general]`.
- Si DSX está abierto, el puerto queda ocupado: ds5haptics lo avisa y sigue sin esta función.
- Al cambiar de modo o cerrar el juego, los gatillos que dejó un mod vuelven a la normalidad.
- El cuadro de estadísticas muestra "mod DSX" en la fila de gatillos mientras llegan mensajes.
- Los otros tipos de mensaje (LED de jugador, umbral de gatillo, LED del micrófono) se ignoran.

### Armar el respaldo (una vez, con internet)

1. Compilá con `build.bat` (que diga "Compilado CON soporte de parlante").
2. Ejecutá **`preparar-respaldo.bat`**. Baja los instaladores oficiales de usbip-win2 y
   HidHide (carpeta `instaladores\`) y el código de libopus (carpeta `deps\`), y arma
   **`ds5haptics-respaldo.zip`** en la carpeta de arriba, con todo, incluido tu `ds5haptics.exe`.
3. Guardá ese zip en un pendrive o en la nube. Podés borrar todas las versiones anteriores.

### Instalar en una PC nueva o recién formateada (sin internet)

1. Emparejá el DualSense por Bluetooth en Configuración de Windows y dejalo prendido.
2. Descomprimí `ds5haptics-respaldo.zip` y ejecutá **`instalar.bat`** (pide permisos de administrador).
   - Instala usbip-win2 (asistente: aceptá todo por defecto) y HidHide (silencioso).
   - Copia el programa a `C:\ds5haptics`.
   - Configura HidHide (autoriza el programa y oculta el mando Bluetooth a los juegos).
   - Activa el arranque con Windows.
3. Reiniciá la PC. Listo: ds5haptics arranca solo.

Si al instalar el mando estaba apagado, o HidHide pide reiniciar primero, ejecutá
`C:\ds5haptics\configurar-hidhide.bat` con el mando prendido. Para quitar todo: `desinstalar.bat`.

No hace falta instalar Visual Studio para usarlo: el `.exe` no depende de nada extra.
Solo hace falta para recompilar (y con `deps\` presente, sin internet).

## Cómo funciona

Por Bluetooth el mando no tiene placa de sonido, así que la háptica viaja dentro de
reportes HID de salida **0x32** (142 bytes). Cada reporte lleva 32 muestras estéreo de
8 bits con signo a **3000 Hz**, así que hay que mandar uno cada **10,667 ms**:

```
[0]        0x32                       Report ID
[1]        tag/seq (0)
[2..10]    sub-paquete 0x11: 91 07 FE 00 00 00 00 FF <contador>
[11..76]   sub-paquete 0x12: 92 40 + 64 bytes de muestras (L,R,L,R...)
[77..137]  ceros
[138..141] CRC32 sobre [0..137], semilla Bluetooth 0xA2
```

| Archivo | Qué hace |
|---|---|
| `src/ds5_bt_haptics.h` | Arma el reporte 0x32 y el CRC. Portable. |
| `src/haptic_audio.h` | Lee WAV, elige canales, remuestrea a 3 kHz con filtro anti-aliasing, genera señales de prueba. Portable. |
| `src/main_windows.cpp` | Busca el mando, lo pasa a modo completo, y envía los reportes con temporizador de alta resolución. |
| `src/haptic_live.h` | Fase 2: remuestreo por bloques, pasa-bajos, compuerta de ruido y búfer de baja latencia entre hilos. Portable. |
| `src/wasapi_capture.h` | Fase 2: captura "loopback" de una salida de audio de Windows con WASAPI. |
| `src/usbip_server.h` | Fase 3a: servidor USB/IP mínimo (exporta un dispositivo USB virtual a usbip-win2). Portable. |
| `src/virtual_dualsense.h` | Fase 3: el DualSense USB virtual (HID + placa de sonido de 4 canales, traducción Bluetooth ↔ USB, audio isócrono a ritmo real). Portable. |
| `tests/test_packet.cpp` | Compara 600 reportes byte a byte contra el layout original de SAxense. |
| `tests/test_live.cpp` | Simula el ritmo real de captura y envío (incluso con relojes desfasados) y verifica latencia, filtro y compuerta. |
| `src/virtual_pads.h` | Modos de compatibilidad (DualShock 4 y Xbox 360 virtuales), batería y reportes de vibración/LED. Portable. |
| `src/profiles.h` | Perfiles por juego (`ds5haptics.ini`). Portable. |
| `src/tray_ui.h` | Ícono en la bandeja y cuadro de estadísticas (Win32 + GDI, sin frameworks). |
| `src/speaker_bt.h` | Parlante del mando por Bluetooth: reporte 0x36 con háptica + audio en Opus. Portable. |
| `tests/test_speaker.cpp` | Comprime y descomprime audio con Opus y verifica el reporte 0x36. |
| `tests/test_pads.py` + `tests/test_pads_unit.cpp` | Enumeran y usan los mandos de PS4 y Xbox como lo haría Windows; cambio de mando en caliente; batería. |
| `tests/usbip_harness.cpp` + `tests/test_usbip_client.py` | Cliente USB/IP simulado que enumera y usa el DualSense virtual como lo haría Windows. |

## Compilar

Hacé **doble clic en `build.bat`**. Busca solo Visual Studio Build Tools o MinGW, y la
ventana queda abierta con el resultado.

Si dice que no hay compilador, instalá Build Tools desde PowerShell y volvé a ejecutarlo:

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

**MinGW-w64 (MSYS2)**:

```
g++ -std=c++17 -O2 -municode -Isrc src/main_windows.cpp -o ds5haptics.exe -lhid -lsetupapi -lwinmm -lole32 -lws2_32
```

Requiere Windows 10 1803 o posterior (temporizador de alta resolución).

## Probar, en este orden

1. Emparejá el mando por Bluetooth desde Configuración de Windows y **desconectá el cable**.
2. **Cerrá DSX, DS4Windows y Steam** (Steam Input también le manda reportes al mando y puede pisar los tuyos).
3. `ds5haptics list` — tiene que aparecer como `Bluetooth`. Anotá el valor de `out`.
4. `ds5haptics info` — tiene que decir `0x31 (modo completo)` y la **barra de luz se pone verde**. Eso confirma que la escritura por Bluetooth funciona.
5. `ds5haptics test pulses` — golpes alternando izquierda y derecha cada medio segundo.
6. `ds5haptics test sweep` — barrido de 40 a 400 Hz. Vas a notar dónde resuenan los actuadores.
7. `ds5haptics play algo.wav --gain 1.5` — cualquier WAV. Con 4 canales usa el 3 y el 4, que es lo que un juego manda por USB.

Para generar un archivo crudo como en SAxense:
`ffmpeg -i musica.mp3 -ac 2 -ar 3000 -f s8 musica.raw` y después `ds5haptics raw musica.raw`.

Durante la reproducción se muestra cuánto tarda cada escritura. Si el promedio se acerca
a 10 ms, el dongle no da abasto y vas a ver "atrasos".

## Fase 2: háptica en vivo

```
ds5haptics sources      lista las salidas de audio de Windows
ds5haptics live         captura la salida predeterminada y la convierte en háptica
```

La barra de luz se pone **naranja** en modo en vivo. Poné música o un juego: la línea de
estado muestra el nivel, el búfer y los cortes.

```
                   hilo de captura (WASAPI loopback)              hilo de envío (cada 10,667 ms)
audio de la PC ──► remuestreo a 3 kHz ──► pasa-bajos ──► compuerta ──► búfer ──► reporte 0x32 ──► mando
```

Ajustes de `live`:

| Opción | Por defecto | Para qué |
|---|---|---|
| `--level X` | 0.9 | Intensidad fija de la háptica (0.9 = 90%), sin importar el volumen. |
| `--gain G` | 1.0 | Ganancia extra sobre `--level`. Con 1.5 se siente más "golpeado" (satura a propósito). |
| `--lowpass Hz` | 300 | Deja pasar solo graves y golpes. Con `0` también se sienten voces y agudos. Probá entre 150 y 500. |
| `--gate X` | 0.001 | Debajo de este nivel de entrada no vibra (evita zumbido con ruido de fondo). `0` la desactiva. |
| `--latency ms` | 25 | Pre-búfer. Menos es más rápido; si ves "cortes", subilo a 35-50. |
| `--source N` | predeterminada | Capturar otra salida, por número de `sources`. |
| `--ch L,R` | 1,2 | Canales a usar. Con 5.1, `--ch 4,4` usa solo el canal de graves (LFE). |
| `--agc-release ms` | 1000 | Qué tan rápido se adapta cuando bajás el volumen. Menos = más rápido, pero "bombea" más con la música. |
| `--no-agc` | | Desactiva la intensidad fija: la háptica vuelve a seguir el volumen. |
| `--no-volcomp` | | No compensar el volumen de Windows. |

Ejemplos:

```
ds5haptics live                                 intensidad fija al 90%
ds5haptics live --gain 1.5                      más golpeado, como se sentía antes
ds5haptics live --lowpass 200 --level 1         solo golpes graves, al máximo
ds5haptics live --no-agc --gain 2               sin intensidad fija (sigue al volumen)
```

### Intensidad independiente del volumen

La háptica se mantiene al 90% sin importar el volumen, gracias a dos mecanismos:

- **Compensación del volumen de Windows:** el programa lee el volumen de la salida y lo
  deshace al instante (hasta +40 dB). Si Windows está en silencio (mute), no llega audio.
- **AGC (control automático de ganancia):** mide el nivel de la señal y la lleva siempre
  a `--level`. Cubre el volumen de YouTube, del reproductor o del juego, que no se pueden
  leer desde afuera. Al bajar mucho ese volumen, tarda 1 o 2 segundos en adaptarse.

La línea de estado muestra `AGC x3.2` (cuánto está amplificando) y `Win -12.0 dB`
(el volumen de Windows que se está compensando). El AGC amplifica como máximo 30 dB:
con un volumen extremadamente bajo (menos de ~3%), la háptica también baja.

Cualquier sonido de la PC se convierte en vibración (también notificaciones o Discord).
Para separar solo el juego hace falta la fase 3 o una salida de audio virtual.

## Fase 3: DualSense USB virtual

```
DualSense ──Bluetooth──► ds5haptics virtual (servidor USB/IP en 127.0.0.1:3240)
                              ▼
                         usbip-win2 (driver firmado)
                              ▼
         Windows ve un DualSense "por cable" (VID 054C, PID 0CE6)
```

El programa traduce en las dos direcciones:

- **Entradas:** botones, sticks, gatillos, giroscopio y touchpad, del reporte Bluetooth 0x31 al reporte USB 0x01.
- **Salidas del juego:** gatillos adaptativos, barra de luz, LEDs y vibración clásica, del reporte USB 0x02 al reporte Bluetooth 0x31.
- **Calibración:** los reportes de calibración y firmware se leen del mando real.

- **Háptica nativa (3b):** el mando virtual tiene la misma placa de sonido que el real (parlante de
  4 canales y micrófono). Los canales 3 y 4 que manda el juego se convierten en reportes 0x32
  por Bluetooth, igual que en las fases 1 y 2. Los canales 1 y 2 (parlante del mando) se descartan,
  y el micrófono devuelve silencio.

### Modo directo (predeterminado) y parlante

- Cada reporte de háptica sale apenas se junta el audio, sin pre-búfer (menor latencia).
  Con `--normal` se vuelve al modo con pre-búfer de 20 ms.
- **Parlante (experimental):** `ds5haptics virtual --speaker` manda también el sonido del
  parlante del mando (canales 1 y 2), comprimido en Opus junto con la háptica en el reporte
  0x36. Necesita compilar con CMake: `build.bat` descarga y compila libopus (licencia BSD)
  la primera vez, así que hace falta internet. Si CMake no está, compila igual pero sin parlante
  y lo avisa. El volumen y el enrutamiento al parlante los manda el propio juego.

**Importante:** al aparecer la placa de sonido virtual, Windows puede elegirla como salida
predeterminada y dejarías de escuchar la PC (pasa lo mismo con un DualSense real por cable).
Si ocurre, volvé a elegir tus parlantes o auriculares en Configuración > Sistema > Sonido.

### Instalación (una sola vez)

1. **usbip-win2:** descargá el instalador de la última versión desde
   https://github.com/vadimgrn/usbip-win2/releases e instalalo.
   **Si el instalador pide activar el "modo de prueba" (test signing), no lo actives y avisame.**
2. **HidHide** (para que los juegos no vean dos mandos): descargá el instalador desde
   https://github.com/nefarius/HidHide/releases, instalalo y reiniciá si lo pide.

### Primera prueba (sin HidHide todavía)

1. Mando emparejado por Bluetooth, sin cable. Cerrá DSX, DS4Windows y Steam.
2. Ejecutá `ds5haptics virtual`. La barra de luz se pone azul y el programa intenta conectarse
   solo con usbip-win2. Si falla con "acceso denegado", abrí la consola como administrador.
3. En la línea de estado tiene que aparecer `Windows: conectado` y `mando BT: ok ... 250 Hz`
   (aproximadamente).
4. En otra consola, `ds5haptics list` tiene que mostrar **dos** mandos: el Bluetooth real y uno **USB**.
5. Abrí un probador de gamepads en el navegador. El mando USB tiene que responder a botones y sticks.

### Configurar HidHide

1. Abrí **HidHide Configuration Client**.
2. En **Applications**, agregá `ds5haptics.exe`, para que el programa pueda seguir viendo el mando real.
3. En **Devices**, marcá el **DualSense Wireless Controller** que figura como Bluetooth
   (no el USB virtual) y activá **Enable device hiding**.

Desde ahí, los juegos ven un solo DualSense: el virtual "por cable".

### Probar en un juego

Usá un juego con soporte nativo de DualSense en PC (gatillos adaptativos y barra de luz). Si es
de Steam, desactivá Steam Input para ese juego. La línea de estado muestra cuántas `salidas del juego`
llegan: si ese número sube, el juego le está mandando efectos al mando.

| Síntoma | Qué probar |
|---|---|
| La línea de estado dice `sin audio del juego` | El juego no abrió el parlante del mando. Verificá en Configuración > Sonido que aparezca "DualSense Wireless Controller" como dispositivo de salida. |
| `háptica activa` pero no se siente | Probar `--strip-audio-flags` (no reenvía al mando los ajustes de audio del juego). Probar `--gain 2`. |
| La háptica se siente débil | `--gain 2` (la ganancia de la háptica nativa es 1.0 por defecto). |
| Se siente la háptica con demora | El modo directo ya es el predeterminado (en simulación, ~7 ms de demora propia contra ~21 ms del modo `--normal`). |
| `cortes` sube seguido (modo normal) | Subir el pre-búfer: `--latency 30`, o usar `--fast`. |
| `huecos` sube seguido | Si se siente entrecortado, probar `--normal`. |
| `--speaker` dice que la compilación no incluye el parlante | Falta CMake. Visual Studio Installer > Modificar > marcar "Herramientas de CMake de C++ para Windows", y volver a correr `build.bat`. |
| Con `--speaker` el parlante suena mal o con cortes | Pasame la salida de la consola. Sin `--speaker` todo vuelve a funcionar como antes. |
| No encuentra `usbip.exe` | Conectar a mano: `usbip attach -r 127.0.0.1 -b 1-1` (desde la carpeta de usbip-win2). |
| "No se pudo abrir el puerto 3240" | Otro programa USB/IP lo está usando (por ejemplo usbipd-win). Cerralo. |
| `Windows: enumerando` y no pasa a `conectado` | Pasame la salida completa de la consola. |
| El mando USB aparece pero no responde | Revisá que `mando BT` diga `ok` con unos 250 Hz. |
| El juego no hace efectos en los gatillos | Mirá si sube `salidas del juego`. Si sube y no se sienten, pasame qué juego es. |
| "El juego intentó escribir el feature 0x.." | Anotá el número y pasámelo: es información útil para la 3b. |

## Si algo falla

| Síntoma | Qué probar |
|---|---|
| No aparece el mando | Quitarlo y volver a emparejarlo. Verificar que no esté por USB. |
| "No se pudo abrir el mando" | Otro programa lo tiene tomado: cerrar DSX, DS4Windows, Steam. |
| `info` no pone la barra verde | El problema es la escritura en general, no la háptica. Probar `--setreport`. |
| Barra verde pero `0x01 (modo simple)` | Desconectar y reconectar el mando, repetir `info`. |
| Error 87 (parámetro incorrecto) al enviar 0x32 | Probar `--setreport`. Pasame la salida de `list` (el valor `out`). |
| Escribe sin errores pero no vibra | Probar `--seq`. Subir `--gain`. Pasame la salida completa. |
| `live` no vibra y el nivel queda vacío | No suena nada en esa salida. Revisar `sources` y probar `--source N`. |
| `live` con "cortes" frecuentes | Subir `--latency` a 40. |
| `live` vibra todo el tiempo, apenas | Subir `--gate` a 0.005. |
| La intensidad "respira" con la música | Subir `--agc-release` a 2000. |

## Créditos y licencia

Ver [CREDITS.md](../CREDITS.md). El código se distribuye bajo la licencia MPL-2.0 ([LICENSE](../LICENSE)).
