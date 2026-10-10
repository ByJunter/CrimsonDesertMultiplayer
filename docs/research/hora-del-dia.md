# Hora del día (y clima) entre anfitrión e invitado

Build analizada: **1.0.0.2976**, PE timestamp `0x6AB28F00`, base `0x140000000`. Todas las
direcciones son RVA salvo que se indique `0x14…`. Análisis **estático**: el juego no estaba
abierto, así que no hay lecturas en vivo. Lo que dependa de valores en tiempo de ejecución está
marcado como pendiente de verificar.

Problema (prueba de dos PC, 2026-10-05): en el mismo momento era de día en el anfitrión y de
noche en el invitado.

**Por qué pasa.** Cada PC ejecuta su propio servidor dentro del proceso. La hora sale de la
partida guardada de cada uno (`FieldSaveData::_fieldTimeRaw`) y avanza por separado. Las
diferencias de carga, pausas, sueños y guardados hacen que las dos horas se separen. Nada en el
mod las sincroniza (`reference/CrimsonDesertCoop` tampoco lo hace).

## 1. Cómo representa el juego la hora

### 1.1 `pa::FieldTime` (32 bytes en memoria, 31 en el cable) — confianza **alta**

Sale del deserializador `0x20CF3F0` (→ `.xtls 0xE2B2260`, solo reubicado) y de las
conversiones `0x20D3460` / `0x20D3360`. El RTTI incluye `VariantItem<FieldTime>`,
`GameMeta_ClassTemplate<FieldTime>`, `getCurrentFieldTime` y `getSecondsOfDay`.

| Off. | Tipo | Campo | Notas |
|---|---|---|---|
| +0x00 | u32 | día | contador de días desde el inicio |
| +0x04 | u32 | hora | 0–23 |
| +0x08 | u32 | minuto | 0–59 |
| +0x0C | u32 | segundo | 0–59 |
| +0x10 | u32 | milisegundo | 0–999 |
| +0x14 | u8 | día de la semana | `día % 7` (lo calcula `fromRaw`) |
| +0x16 | u16 | **escala de tiempo** | vale 12 por defecto (todos los constructores ponen `0xC`) |
| +0x18 | u64 | **ancla** | ms del reloj del proceso (base del servidor) en que se fijó el valor |

En el cable el orden es el mismo, sin el byte de relleno +0x15: 5×u32, u8, u16 y u64, es decir,
31 bytes.

**Tiempo "raw".** `toRaw` está en `0x20D3460` y `fromRaw(ft, raw)` en `0x20D3360`. Primero
calcula `ms = ((h·60+m)·60+s)·1000+ms`. Un día raw son `0x4819080` = 75.600.000 unidades
(21 h):
- de 03:00 a 21:00: `raw = día·75,6M + ms − 5,4M`, a velocidad normal;
- de 21:00 a 03:00: `raw = día·75,6M + (ms [+64,8M]) / 2`. Las 6 horas de noche ocupan 3 h raw y
  pasan el doble de rápido.

**Cómo avanza.** `CommonField::GetCurrentFieldTime` (`0x20E55A0`, sin mutar) sigue estos pasos:
1. Si el nivel tiene hora fija (`_useFixedFieldTime`, byte `+0x6A` de la info de nivel), la
   devuelve (`+0x48`). Esto ocurre en mazmorras y zonas especiales.
2. Si no, toma la copia global (§1.2). Si `field+0x17C == 0` (no pausado), calcula
   `ahora = 0x1416B80() + desfase` y
   `raw' = toRaw(ft) + (ahora − ft.ancla) · ft.escala`, y luego aplica
   `fromRaw(ft, raw')` y `ft.ancla = ahora`.

Con escala 12, un día dura 75,6M/12 ms ≈ **105 min reales** (90 de día y 15 de noche). La
comunidad dice "1 min real ≈ 12 min de juego, unas 2 h por día", lo que cuadra.

### 1.2 Las dos copias globales — confianza **media-alta** (verificar en vivo)

| Dirección | Qué es | Quién la escribe |
|---|---|---|
| `0x146A6D218` (32 B) | FieldTime **del cliente** (hilo sin la marca TLS `+0x1EC`) | `0x21A1CC0`, `0xA38B00` (SetFieldTimeAck), `0x219D7F0`, `0x2BC0A30` |
| `0x146A6D238` (32 B) | FieldTime **del servidor** (marca TLS `gs:[0x58]→[+0x1EC] != 0`) | mismas funciones, rama TLS ≠ 0 |

Todas eligen la copia según la marca TLS `+0x1EC`. La asignación cliente/servidor sale de dos
indicios:
- con la marca puesta, `0x20E55A0` no suma el desfase cliente→servidor;
- con la marca puesta, `0x1416B80` usa el reloj real en lugar del de fotograma.

Hay que confirmarlo leyendo las dos copias en vivo.

**Reloj ("ahora")** — confianza alta:
- `0x1416B80`: en el hilo principal del cliente devuelve el **ms del fotograma**,
  `[0x146C8CD68] + [0x146C8CD70]/1000`: el segundo campo está en **microsegundos** (división
  entre 1000 con la constante mágica `0x0624DD2F1A9FBE77` en `0x1416BB3`; verificado en vivo el
  2026-10-05). Una versión anterior de este informe decía `/1e6`, y era un error. En el
  servidor devuelve `(reloj_ns − [0x146EFBC70]) / 1e6` (`0x1416AF0`). Las dos son ms desde una
  época del proceso. Detalle en §8.2.
- `[0x146D69438]` (i64): **desfase cliente→servidor en ms**. Lo escribe el manejador de
  `ServerTimeInfoAck` (`0xBDF310`): `desfase = tick_servidor − ahora_cliente`.
  `[0x146D69430]` = `unix_servidor − time()`.
- `ServerTimeInfoAck` (2188, S→C, cada 5 s, 20 B: `u32 0 | u64 tick ms del servidor | u64
  segundos Unix`) **no** lleva la hora del juego. Solo sincroniza relojes. Visto 1:1 en todas
  las capturas.

**Consecuencia.** El ancla `+0x18` está en la base de tiempo **del servidor de ese PC**. Un
FieldTime copiado tal cual de otro PC tendría un ancla sin sentido: la hora saltaría (ancla en
el pasado) o se congelaría (ancla en el futuro, porque `if (ahora > ancla)` no avanza). **El
receptor tiene que reconstruir el ancla con su propio reloj.**

### 1.3 Escritores y caminos de red — confianza **alta** en el formato, **media** en los efectos

| Mensaje | Opcode | Dir. | Formato | Manejador |
|---|---|---|---|---|
| `SetFieldTimeReq` | 2125 | C→S | `FieldTime(31) | u8 flag | u32 clave` = **36 B** | `0x29BFD70` → `.xtls 0x1049D430` → `ServerFieldComp::SetFieldTime` `0x2BC8970(comp=[[actor+0x68]+0x1A0], &ft, flag, &clave)` |
| `SetFieldTimeAck` | 2816 | S→C | `u32 clave_actor | FieldTime(31)` = **35 B** | `0xB88990` → `.xtls 0xA41D560` → `0xA38B00(mgr=[[[0x146D69188]+0x30]+0x1148], &ft, 1, 0, 0)` |
| `SyncWeatherStateReq` | 2560 | C→S | 6 × 4 B (24 B), ver §3 | `0x29C5DF0` → `CommonField::SetWeatherState` `0x20E6FE0` |
| `UpdateFieldNightAndDay` | 2017 | temporizador interno del servidor | — | `0x2C3BC70` (cada 10 ticks llama a `0x2C3BCD0`; probablemente lee la hora, no la fija; confianza baja) |
| `SetTimeWrapCoolTimeReq/Ack`, `SetTimeWrapCoolEndTimeAck` | 2669 / 2731 / 2529 | | enfriamiento de "saltar tiempo" (`_timeWrapCoolTime`, `CheckCanTimeWrap`) | al cargar llega siempre `SetTimeWrapCoolEndTimeAck = 01 00 10 A0 | u64 0x3463A4` |

Qué hace `0x2BC8970` (servidor), en orden:
1. comprueba que el actor está en su field (`0x17FA6C0`, `field+0x180`) y `0x206AE30`;
2. llama a `0x2842C70` (sin identificar);
3. **`0x21A1CC0(comp, &ft)`**: `comp+0x450 = ft`, y si es el field actual también escribe la
   copia global;
4. **difunde `SetFieldTimeAck` con `ToNearSendTargetFinder`** (`0x299AED0`), que llega
   también al propio cliente;
5. solo si `flag != 0`, busca `clave` en una tabla (`[0x146D6C1F8]`) y hace cálculos de
   posición. Parece ser el punto donde "despiertas" al dormir en cama u hoguera. **Con
   `flag = 0` solo cambia la hora.**

Qué hace `0xA38B00` (cliente, al recibir el Ack): si hay actor/field, llama a `0x21A1CC0` sobre
su componente, que escribe `comp+0x450` y la copia global del cliente. Después llama a
`0xA39330` y publica un evento, probablemente `UITimeOfDayEvent_UPDATE_TIME`. Si no hay field,
como durante una carga, solo escribe la global.

`0x2BC0A30` (servidor, al entrar en un field) toma la hora guardada del nivel (tabla en
`+0x5D0`) o unas por defecto (globales `0x146CF1158…12B8`), le suma `[this+0x80]·1000` ms y la
instala. Ver §4.

**En las capturas no aparece ni un `SetFieldTimeReq` ni un `SetFieldTimeAck`.** Se revisaron
todas las de `D:\cdmp-grabaciones`, las de `bin64` y la del Shadow del 2026-10-05. Es decir:
- no hay una sincronización periódica de la hora entre servidor y cliente;
- la hora solo viaja cuando alguien la cambia (dormir, esperar, eventos de secuenciador
  `GamePlayEvent_SetTimeOfDay`);
- en esas sesiones nadie durmió.

### 1.4 Cadenas útiles

- Datos de guardado: `_fieldTimeRaw`, `_modifiedFieldTimeSecondsOfDays`,
  `_resetTimeSecondsOfDays`, `_currentFieldTime`, `_timeWrapCoolTime` y `_useTimeWrapCoolTime`.
- Por nivel: `_useFixedFieldTime` y `_fixedFieldTime`.
- Debug del motor: `PearlAbyssEngine.Debug.TimeOfDayForward/Backward(x2)` y
  `CycleTimeOfDayNext/Prev`.
- Comandos de consola: `/settimeofdaylowerlimit`, `/settimeofdayupperlimit` y `changeweather`.
  Seguramente están desactivados en la versión de tienda; no se probó.

## 2. Solicitudes de truco y Acks inyectables

- **No hay ningún `…CheatReq` de hora o clima** en los 1.115 mensajes. Los de truco son de
  stats, kill, spawn, misiones, objetos, etc. (`troc_messages_2976.csv`).
- No hace falta: **`SetFieldTimeReq` con `flag = 0` es justo "pon esta hora"**, y el propio
  servidor responde con `SetFieldTimeAck` a su cliente. Se inyecta con `sniffer_inject_c2s`
  (`u16 2125 | u8 slot 0 | u16 36 | payload`), igual que `VaryStatCheatReq`. Confianza
  **media-alta**: formato y cadena de llamadas confirmados; efectos secundarios de `0x2842C70`
  sin verificar.
- Alternativa solo para el cliente: `puppet_inject_s2c(2816, u32 0 | FieldTime)`. Cambia lo que
  se ve, pero el servidor del invitado sigue con su hora: horarios de NPC, tiendas y la hora que
  se guarda. Además se revierte al entrar en otro field, porque `0x2BC0A30`/`0x219D7F0`
  reinstalan la hora del servidor. Sirve como **plan B** o para depurar.
- Escribir directamente en `0x146A6D218/238` es posible, pero no se recomienda. Habría que
  escribir también `comp+0x450` del field, y la interfaz no recibe el evento. El paquete hace
  todo eso por nosotros sin tocar memoria.

## 3. Clima — confianza **media-baja**

El **clima lo simula el cliente**: los componentes de script `weatherSimulator`,
`weatherTimeSimulator`, `weatherCloudSimulator`, `weatherSurfaceSimulator` y
`worldWeatherEffect`. El cliente se lo comunica al servidor con `SyncWeatherStateReq` (C→S)
cuando cambia; no lo envía periódicamente.

`SetWeatherState` (`0x20E6FE0`) calcula `field+0x178` (`TypedFlag32<WeatherState>`) a partir de
estos campos:

| Off. | Interpretación | Uso en `0x20E6FE0` |
|---|---|---|
| +0x00 f32 | lluvia | `> 0` → estado 1 |
| +0x04 f32 | nieve | `> 0` → estado 2 (gana a la lluvia); si ninguna de las dos → `0x20` (despejado) |
| +0x08 f32 | **temperatura (°C)** | no se usa aquí. En las capturas va de 8,9 a 19,4. En el invitado bajó de 19,4 a 11,5 durante 30 min de noche |
| +0x0C f32 | ¿humedad/nubes? (0,40–0,49) | `>= umbral [0x146CF0F28]` → `|0x10` |
| +0x10 f32 | ¿niebla/lluvia ligera? (0,007–0,22) | `>= umbral [0x146CF0FC8]` → `|8` |
| +0x14 f32 | ¿viento? (2,9–4,5) | `>= umbral [0x146CF0F78]` → `|4` |

Lo que no sabemos: si el simulador es **determinista dado FieldTime + región** (entonces basta
con sincronizar la hora) o si usa estado o aleatoriedad propios. Hay más vías posibles, pero
están sin investigar:
- un **buff que cambia el clima** (`ClientChangeWeatherBuffProcessor`, `ChangeWeatherBuffData`,
  `GameGlobalEffectInfo_Weather`), que se podría inyectar como `ApplyBuffAck`;
- el comando `changeweather`;
- escribir en el componente `weatherSimulator`.

**Recomendación:** primero sincronizar la hora y medir. Con la hora alineada, comparar los
`SyncWeatherStateReq` de los dos PC en una prueba real. Si coinciden, ya está. Si no, abrir una
investigación propia sobre el clima.

## 4. Dormir, esperar y viaje rápido

- **Dormir o esperar** (cama, hoguera u olla; 3/6/12 h; disponible desde el capítulo 3) es el
  "TimeWrap". Tiene enfriamiento (`SetTimeWrapCoolTimeReq`, `CheckCanTimeWrap`), y casi seguro
  el cambio de hora viaja como `SetFieldTimeReq` con `flag = 1` y la clave de la cama. **No
  está capturado**, así que conviene grabar uno. Confianza media.
- **Viaje rápido o cambio de field:** el servidor reinstala la hora al entrar (`0x2BC0A30`).
  Suma `[this+0x80]` segundos, que podría ser el tiempo de viaje, y sale de la tabla de hora por
  nivel. Para el invitado significa que **un cambio de field puede pisar la hora sincronizada**
  si su servidor tiene otra. Inyectar en el servidor (`SetFieldTimeReq`), en vez de solo en el
  cliente, lo evita en gran parte. Aun así hay que resincronizar después de `MoveFieldCompleteAck`
  o `LoadingCompleteReq`. Confianza baja-media.
- **Dormir en el invitado:** su servidor cambiaría la hora y se separaría de la del anfitrión.
  Fase 1: el anfitrión manda, y la siguiente resincronización devuelve al invitado a su hora
  (salto visible). Fase 2: detectar en el invitado el C→S `SetFieldTimeReq` con el sniffer y
  reenviarlo al anfitrión como petición ("el invitado quiere dormir N h"), o bloquearlo.
- **Pausa o menús:** `field+0x17C` congela `GetCurrentFieldTime`. No sabemos si la pausa del
  anfitrión llega a activarlo. Por eso hace falta resincronizar cada cierto tiempo y no basta con
  enviar la hora una vez.
- **Guardado:** si el invitado guarda, su partida se queda con la hora del anfitrión
  (`_fieldTimeRaw`). Es aceptable. Opcional: recordar la hora original y reponerla al
  desconectar.

## 5. Diseño propuesto

### 5.1 Anfitrión: leer su hora (solo lectura)

Cada **5 s** y además **al momento** cuando el sniffer vea en el anfitrión un S→C
`SetFieldTimeAck` (durmió o hubo un evento) o un `MoveFieldCompleteAck`:
1. Leer con protección SEH/`VirtualQuery` los 32 B de `0x146A6D218` (cliente), el desfase
   `[0x146D69438]` y "ahora" (`[0x146C8CD68] + [0x146C8CD70]/1000`, porque el segundo campo
   está en µs; no hace falta llamar a `0x1416B80`, ver §8.2).
2. Calcular `raw_ahora = toRaw(ft) + max(0, ahora + desfase − ft.ancla) · ft.escala`. Las
   funciones `toRaw`/`fromRaw` se reimplementan en el mod: son unas pocas líneas de aritmética
   y se pueden probar con tests unitarios.
3. Enviar `TimeSync`.

### 5.2 Protocolo

Nuevo `EventKind::TimeSync = 13`, no fiable porque es periódico. Los saltos se envían 3 veces
seguidas o como `RelKind` fiable. **Hay que subir `kVersion` de 6 a 7.**

```
u32 seq | u64 raw (ms de juego "raw" en el momento de enviar) | u16 escala | u8 flags
(bit0 = salto/forzar, bit1 = hora fija de nivel, bit2 = pausado)
```
16 B, serializado campo a campo en `protocol.cpp`.

### 5.3 Invitado: aplicar

1. Al recibirlo: `raw_obj = raw + (rtt/2)·escala` (el RTT ya existe con Ping/Pong).
2. Leer su propia hora como en §5.1 y calcular `Δ = raw_obj − raw_local`.
3. Si `|Δ| < 2 min de juego` (≈10 s reales) y no hay `bit0`, no hacer nada. Así se evitan
   inyecciones continuas, eventos de interfaz y recálculo de horarios.
4. Si no, construir `FieldTime ft = fromRaw(raw_obj)` con `ft.escala = escala`,
   `ft.ancla = ahora_local + desfase_local` (base del servidor del invitado) y
   `ft.díaSemana = día % 7`.
5. Inyectar **`SetFieldTimeReq`**: `sniffer_inject_c2s(u16 2125 | 0 | u16 36 | ft(31) | u8 0 |
   u32 0)`. Esto actualiza el servidor del invitado, y su servidor envía `SetFieldTimeAck` a su
   cliente.
6. Comprobar el `SetFieldTimeAck` en el sniffer del invitado. Si no llega en 1 s: registrar el
   fallo y, como plan B, `puppet_inject_s2c(2816, u32 0 | ft)`.
7. Limitar a una inyección cada 5 s, salvo saltos.

**Sin hooks y sin escrituras en memoria:** solo lecturas protegidas e inyección de paquetes.
Todo se hace en el hilo worker; la inyección C→S ya sale desde dentro de un `PostPacket` real.
**Antes de la primera prueba, copia de seguridad de las partidas** (la hora se guarda).

## 6. Plan de pruebas

**Fase 0 — sonda de solo lectura, un PC, sin peer.** En `cdmp.log`, cada 5 s:
- las dos globales decodificadas (día, h:m:s, escala y ancla);
- "ahora" + desfase;
- la hora calculada.

Hay que confirmar cinco cosas:
- (a) `0x146A6D218` es la hora del cliente y coincide con la hora del mapa o la interfaz
  (captura con `CopyFromScreen`);
- (b) la escala es 12;
- (c) avanza ≈ 12 s de juego por segundo real de día y 24 de noche;
- (d) en una mazmorra con hora fija la global sigue avanzando;
- (e) qué pasa en pausa/menú.

**Fase 1 — inyección en el propio juego, sin peer.** Partidas en `backups/`. Una tecla de
desarrollo inyecta `SetFieldTimeReq` con la hora actual + 6 h (`flag = 0`). Comprobar:
- el S→C `SetFieldTimeAck` en `cdmp_packets` (35 B, los mismos 31 B que enviamos);
- el cambio visual (día → noche);
- que la sonda de la fase 0 lee la hora nueva en las **dos** globales;
- que no hay efectos raros: teletransporte, fatiga, tiendas.

Después, viaje rápido y comprobar que la hora se mantiene. **Grabar también un "dormir 3 h"
real** para ver el `SetFieldTimeReq` del juego (flag y clave) y si el viaje rápido cambia la
hora.

**Fase 2 — loopback con `cdmp_peer`.** Añadir a `cdmp_peer` un modo que envíe `TimeSync` con
una hora fija (por ejemplo 22:00, con `bit0`) y luego cada 5 s. El juego es el invitado.
Comprobar:
- que se hace de noche y se mantiene;
- que no reinyecta en cada paquete (umbral);
- que tras un cambio de field vuelve a cuadrar.

Hacer también la prueba al revés: el juego es el anfitrión y `cdmp_peer` registra los
`TimeSync` recibidos. Comparar con la sonda.

**Fase 3 — dos PC (Shadow como invitado).** Cargar las partidas con horas distintas. En 10 s la
del invitado debe saltar a la del anfitrión. Probar:
- que el anfitrión duerma 6 h: el invitado le sigue al momento;
- que el invitado duerma: en la fase 1 debe volver a la hora del anfitrión.

Comparar los `SyncWeatherStateReq` de los dos lados para decidir si hace falta trabajo aparte
para el clima (§3). Por eso hay que **grabar `cdmp_packets` también en el anfitrión**: hoy solo
existe la del Shadow.

## 7. Resumen de confianza

| Hallazgo | Confianza |
|---|---|
| Disposición de `FieldTime` (día/h/m/s/ms/díaSemana/escala/ancla), `toRaw`/`fromRaw`, noche al doble de velocidad | Alta |
| Formatos de `SetFieldTimeReq` (36 B) y `SetFieldTimeAck` (35 B) | Alta |
| `SetFieldTimeReq` con flag 0 → el servidor fija la hora y envía el Ack a su cliente | Media-alta |
| Globales `0x146A6D218` (cliente) / `0x146A6D238` (servidor) | Media-alta (falta verificar en vivo cuál es cuál) |
| Reloj `0x1416B80`, desfase `[0x146D69438]` de `ServerTimeInfoAck` | Alta |
| `ServerTimeInfoAck` no lleva la hora del juego | Alta |
| No hay sincronización periódica de la hora (0 paquetes en todas las capturas) | Alta |
| Dormir = `SetFieldTimeReq` con flag 1 y clave de cama | Media (sin captura) |
| El viaje rápido o cambio de field reinstala la hora del servidor | Baja-media |
| Clima simulado en el cliente; disposición de `SyncWeatherStateReq` | Media (lluvia/nieve/temperatura), baja (resto) |
| Ningún CheatReq de hora o clima; la referencia no sincroniza la hora | Alta |
| Tablas CE públicas con hora del día | No se encontraron (solo inventario, stats y temperatura del jugador) |

## Herramientas usadas

- `build/research/horse2/xtrace.py <rva>` (con `XV=1`) para el código reubicado o mutado.
- `cddis.py`, `rawref.py <rva…>` (referencias a `0x6A6D218/238`, `0x6D69438`) y
  `exe_strings_all.txt`.
- Capturas: `grep` de los opcodes 2125/2816/2560/2188/2529 en `D:\cdmp-grabaciones\*.log`,
  `bin64\cdmp_packets_*.log` y las capturas del invitado (Shadow) del 2026-10-05.

Fuentes externas (comunidad, solo para cuadrar la duración del día y cómo saltar tiempo):
thegameswiki.com "weather-and-day-night-cycle" y "how-to-skip-time-and-sleep",
games.gg "crimson-desert-how-to-skip-time".

## 8. Especificación para implementar (análisis estático, 2026-10-06)

Build **1.0.0.2976**, PE timestamp `0x6AB28F00`, base `0x140000000`. Direcciones en RVA. Todo
sale de leer las instrucciones (`cddis.py`, `horse2/xtrace.py` para `.xtls`, `rawref.py`,
`callers.py`); no se ejecutó el juego. Las constantes mutadas de `.xtls` se resolvieron leyendo
el dword referenciado. Confianza: **A** = alta (instrucciones leídas de principio a fin),
**M** = media (lectura parcial o interpretación), **B** = baja (hipótesis).

### 8.1 `toRaw` (`0x20D3460`) y `fromRaw` (`0x20D3360`) — confianza A

Las dos son funciones hoja, sin llamadas y sin mutar. Toda la aritmética es entera y sin signo
de 64 bits. Los campos u32 se extienden con ceros y no se valida ningún rango: una hora ≥ 24
produce un raw sin sentido. Las divisiones por constante (`0x719F9BF48F16B2E3 >> 89` = /75,6M;
`0x0624DD2F1A9FBE77` con sub/shr/add/shr 9 = /1000; `0x8888888888888889 >> 5` = /60;
`0xAAAAAAAAAAAAAAAB >> 4` = /24; `0x24924925` = /7 en 32 bits) se comprobaron emulando las
instrucciones contra `/` y `%` de C con 200.000 valores aleatorios: coinciden siempre.

```cpp
constexpr uint64_t kRawPerDay = 75'600'000;          // 0x4819080 (21 h de raw por día)
// FieldTime en memoria: u32 day,hour,minute,second,ms | u8 weekday (+0x14) | pad (+0x15)
//                       | u16 scale (+0x16) | u64 anchor (+0x18)  = 32 B

uint64_t to_raw(const FieldTime& f) {                 // 0x20D3460
    uint64_t t = ((uint64_t(f.hour) * 60 + f.minute) * 60 + f.second) * 1000 + f.ms;
    uint64_t base = uint64_t(f.day) * kRawPerDay;     // imul rax,rax,0x4819080
    if (t < 10'800'000) return base + (t >> 1);       // 00:00-02:59:59.999  (cmp 0xA4CB80; jb)
    if (t < 75'600'000) return base + t - 5'400'000;  // 03:00-20:59:59.999  (sub 0x5265C0)
    return base + ((t + 64'800'000) >> 1);            // 21:00-23:59:59.999  (add 0x3DCC500; shr 1)
}

void from_raw(FieldTime& f, uint64_t raw) {           // 0x20D3360; NO toca scale ni anchor
    uint64_t d = raw / kRawPerDay;
    uint64_t r = raw - d * kRawPerDay;
    uint64_t t = r < 5'400'000  ? r * 2                    // cmp 0x5265C0; add r10,r10
               : r < 70'200'000 ? r + 5'400'000            // cmp 0x42F2AC0
               :                  r * 2 - 64'800'000;      // lea r10,[r10*2-0x3DCC500]
    f.day = uint32_t(d);
    f.weekday = uint8_t(uint32_t(d) % 7);             // calculado sobre el día truncado a 32 bits
    f.ms = uint32_t(t % 1000);
    f.second = uint32_t(t / 1000 % 60);
    f.minute = uint32_t(t / 60'000 % 60);
    f.hour = uint32_t(t / 3'600'000 % 24);
}
```

- **Redondeo:** en las ventanas de noche, `toRaw` trunca (`shr 1`), así que un ms impar se
  pierde y la ida y vuelta devuelve el par anterior (23:59:59.999 → .998). De día la ida y vuelta
  es exacta.
- **Noche al doble de velocidad:** el raw avanza de forma lineal con el tiempo real; es `fromRaw`
  el que convierte un raw de noche en el doble de ms de juego. No hay que hacer nada aparte.
- `0x20D34D0` es una variante `fromSeconds(u64 s)`: día = s/86400 y ms = 0. Solo la usa
  `0x2879F90` (§8.5).

Ejemplos (día 10, escala 12) para los tests unitarios (`weekday` del día 10 = 3):

| Hora | `to_raw` | `from_raw(to_raw)` |
|---|---|---|
| 00:00:00.000 | 756.000.000 | igual |
| 00:00:00.003 | 756.000.001 | 00:00:00.002 |
| 02:59:59.000 | 761.399.500 | igual |
| 02:59:59.999 | 761.399.999 | 02:59:59.998 |
| 03:00:00.000 | 761.400.000 | igual |
| 06:00:00.000 | 772.200.000 | igual |
| 12:00:00.000 | 793.800.000 | igual |
| 20:59:59.999 | 826.199.999 | igual |
| 21:00:00.000 | 826.200.000 | igual |
| 23:30:00.000 | 830.700.000 | igual |
| 23:59:59.999 | 831.599.999 | 23:59:59.998; `+1` → día 11 00:00:00.000, `weekday` 4 |

Extrapolación (§8.2) con escala 12:
- 12:00:00 más 1 s real (`raw + 12.000`) → 12:00:12;
- 22:00:00 más 1 s real → 22:00:24;
- `ft` = día 10 20:59:00.000 con `anchor` = 1.000.000 y `now` = 1.010.000: raw = 826.140.000 +
  10.000·12 = 826.260.000 → **21:02:00.000**. Son 5 s reales de día (60 s de juego) y 5 s de
  noche (120 s de juego).

### 8.2 `GetCurrentFieldTime` (`0x20E55A0`) y el lector del mod — confianza A (copias globales: A)

```cpp
FieldTime CommonField::GetCurrentFieldTime() {             // 0x20E55A0, sin mutar
    u16 lvl = this->vf[0x60]();                            // clave de nivel
    if (lvl != 0xFFFF && LevelInfo(lvl)->[+0x6A])          // 0x4A2010; _useFixedFieldTime
        return LevelInfo(lvl)->[+0x48];                    // _fixedFieldTime, 32 B, sin extrapolar
    bool srv = TLS[gs:58 → [0]]+0x1EC != 0;
    FieldTime ft = srv ? G[0x6A6D238] : G[0x6A6D218];      // copia LOCAL de 32 B
    if (this->[+0x17C] != 0) return ft;                    // congelado
    u64 now = 0x1416B80() + (srv ? 0 : i64[0x6D69438]);    // lea r9,[rax+rbx]
    if (now <= ft.anchor) return ft;                       // cmp r9,r10; jbe (sin signo)
    u64 raw = to_raw(ft) + (now - ft.anchor) * u64(ft.scale);  // imul rdx,rcx (u16 extendido)
    ft.anchor = now;                                       // solo en la copia local
    from_raw(ft, raw);
    return ft;
}
u64 0x1416B80() {                                          // "ahora" en ms
    if (byte[0x6C8CD35] == 0 && TLS+0x1EC != 0) return 0x1416AF0();  // servidor: reloj real
    if (u64[0x6C8CD68] == 0) return 0x1416AF0();
    return u64[0x6C8CD68] + u64[0x6C8CD70] / 1000;         // fotograma: ms + µs/1000
}
// 0x1416AF0 = (i64)(steady_ns() − [0x6EFBC70]) / 1'000'000  (imul 0x431BDE82D7B634DB; sar 18)
```

**Qué copia es cuál — confianza A.**
- `0x6A6D218` es la del **cliente** (marca TLS `+0x1EC` = 0) y `0x6A6D238` la del
  **servidor**. Tres pruebas independientes:
  1. con la marca a 0 se suma el desfase cliente→servidor `[0x6D69438]`, que solo escribe el
     manejador de `ServerTimeInfoAck`;
  2. con la marca a 0, `0x1416B80` usa el reloj de fotograma;
  3. en `0x21A1CC0` (→ `.xtls 0xE52E900`) la rama del servidor solo escribe `0x238` si el field
     del actor es el actual, y la del cliente escribe `0x218` sin condición.
- El manejador del cliente `0xA38B00` usa la misma selección por TLS.

**La copia se actualiza de forma perezosa — confianza A.**
- `0x20E55A0` copia la global a la pila y nunca la reescribe.
- La lista completa de escritores de `0x6A6D218/238` (búsqueda de todas las referencias
  RIP-relativas en el ejecutable) es: `0xA38B00`, `0x219D7F0`, `0x2BC0A30` y `.xtls 0xE52E900`
  (= `0x21A1CC0`).
- `0x21A1CC0` tiene exactamente 3 llamadores: `0xA38B00`, `0x2BC0A30` y `0x2BC8970`.
- Por tanto, la global guarda `(hora, ancla)` del último cambio, y la hora actual siempre es la
  extrapolación. **El mod puede leerla desde su propio hilo sin llamar a nada del juego:**

```cpp
// Lector del anfitrión (hilo del mod; todo con safe_read/SEH)
FieldTime ft;  read 32 B de exe+0x6A6D218 dos veces y repetir si difieren (el juego la copia
               con vmovups ymm sin cerrojo y otra hebra puede estar escribiéndola)
u64 cd68 = u64[exe+0x6C8CD68], cd70 = u64[exe+0x6C8CD70];
if (cd68 == 0) → sin reloj; no enviar (el juego usaría 0x1416AF0; no emularlo)
u64 now_srv = cd68 + cd70 / 1000 + (u64)i64[exe+0x6D69438];   // base de tiempo del servidor
u64 raw = to_raw(ft);
if (now_srv > ft.anchor) raw += (now_srv - ft.anchor) * ft.scale;
```

**Precauciones:**
- *Nivel con hora fija* (confianza M). `0x2BC0A30` instala en la global del servidor el
  `_fixedFieldTime` del nivel. Después `MoveFieldCompleteAck` lo lleva a la global del cliente
  (§8.5), así que dentro de una mazmorra de hora fija **la global contiene la hora fija**, con su
  ancla de datos estáticos (probablemente 0, sin verificar).
  - Regla para el lector: si `ft.anchor == 0`, márcalo como "hora fija" (bit1) y no lo
    extrapoles ni lo inyectes.
  - Confirmar en la sonda de la fase 0 dentro de una mazmorra.
- *`field+0x17C`* (confianza M). Es un byte por field que congela la extrapolación.
  - `0x219D7F0` lo pone a 0 al recibir `LoadingComplete` en los dos lados (§8.5) y reinstala
    `comp+0x450` con **su ancla antigua**. Al descongelar, la hora salta lo que "habría corrido",
    así que el congelado solo se nota durante la carga.
  - El lector no lo comprueba porque necesitaría el puntero al field. No pasa nada si no se
    envía durante una carga.
- *Escala*: se multiplica tal cual. Si se envía 0, el invitado se queda congelado. Hay que enviar
  la del anfitrión, que normalmente es 12.

### 8.3 `SetFieldTimeReq` (2125, C→S) — formato A, efectos M

**En el cable.** El payload tiene que medir **exactamente 36 B**, little-endian y sin
alineación. El paquete completo mide 41 B: `2D 08 | 00 | 24 00 | payload`.

| Off. | Tipo | Campo |
|---|---|---|
| 0 | u32 | día |
| 4 | u32 | hora (0–23) |
| 8 | u32 | minuto |
| 12 | u32 | segundo |
| 16 | u32 | ms |
| 20 | u8 | día de la semana (`día % 7`; el servidor lo recalcula en cada `fromRaw`) |
| 21 | u16 | escala |
| 23 | u64 | **ancla** (ms, base de tiempo del servidor de ese PC) |
| 31 | u8 | flag (**0**) |
| 32 | u32 | clave (**0**) |

Pruebas en `.xtls 0x1049D430` (vf2 de `0x29BFD70`):
- `total − 5 == u16[hdr+3]` y, si el contexto no es de confianza, `len ≤ 0x7FC1`.
- Si `byte[instancia_estática+0x21] != 0`, se descarta en silencio (instancia `0x6A72300`).
- `ft` local: todo a 0, salvo escala = 12.
- `0x20CF3F0` (→ `.xtls 0xE2B2260`) lee 4 B en +0, 4 en +4, 4 en +8, 4 en +0xC y 4 en +0x10,
  luego 1 en +0x14, 2 en +0x16 y 8 en +0x18.
  - Dos constantes estaban mutadas: el tamaño `0x63630004 + [0xE5A4B00]` = 4 y el
    desplazamiento `[0xDCE2764] ^ 0xA48D4828` = 0xC.
  - **El ancla viaja en el cable.**
- Después lee 1 B (flag) y 4 B (clave) y exige `posición == len`; si no, devuelve un código de
  error.
- Si `[ctx] == null`, sale en silencio.
- Si no, llama a `0x2BC8970(comp=[[actor+0x68]+0x1A0], &ft, flag, &clave)`.
  - `sniffer_inject_c2s` va siempre a la sesión 1, la del jugador del invitado (como
    `VaryStatCheatReq`), así que el actor es el jugador.

**Qué ancla espera el servidor.**
- `0x21A1CC0` copia los 32 B **tal cual**, ancla incluida. No hay normalización.
- El servidor extrapola con "ahora" sin desfase. Con `[0x6C8CD35] == 0` es su reloj real
  `0x1416AF0`, que es la base que el desfase de `ServerTimeInfoAck` reproduce en el cliente.
- Por tanto: **`anchor = ahora_cli_invitado + desfase_invitado`**, calculado en el mismo instante
  para el que se calculó la hora (`ft = from_raw(raw_objetivo)`).
  - Si el paquete sale unos ms después da igual: el servidor extrapola desde el ancla.
  - Ancla en el futuro: la hora se congela hasta alcanzarla (`now <= anchor`).
  - Ancla en el pasado por Δ ms: la hora adelanta Δ·escala.
  - **Nunca 0** (saltaría el tiempo de actividad ×12) ni `UINT64_MAX` (congelada para siempre).

**Qué hace `0x2BC8970`, en orden** (sin mutar, leída entera):
1. `0x17FA6C0(actor)`: si el actor no está enganchado a un field, **sale en silencio**.
2. `field+0x180 == [[comp+8]+0xA0]+0x60` (field actual); si no, sale en silencio.
3. `0x206AE30([actor+0xA0])`: si no es válido, sale en silencio.
4. `0x2842C70([[[actor+0x68]+0x188]+0x18])`: el componente **de facción**. El mismo puntero que
   `companeros-mercenarios.md` identifica como tabla de relaciones de facción.
   - Primero lee la hora actual (`0x21A1DA0` → `0x20E55A0`).
   - Luego recorre los slots de la tabla estática `[[[0x6D69530]]+8]+0x310` y compara dos marcas
     de tiempo (`0x17AD8F0` frente a `0x17AD670(now+desfase)`).
   - Los slots vencidos se reinician (`0x285BE90`, que también lee `GetCurrentFieldTime`) y se
     notifican al dueño (`0x29A7CA0` con `ToTargetSendTargetFinder`).
   - Interpretación (confianza **B**): caducan relaciones de facción temporales, por ejemplo
     hostilidad por un delito. Es lo mismo que pasa al dormir.
5. `0x21A1CC0(comp, &ft)`: `comp+0x450 = ft` y global del servidor `0x6A6D238 = ft`.
6. `0x299AED0`: difunde `SetFieldTimeAck` (`op 0xB00`, `u32 [actor+0x60] | FieldTime(31)` con
   `0x20CF330`) con `ToNearSendTargetFinder(incluirse = 1)`. En el servidor del invitado, el
   único destinatario es su propio cliente.
7. Solo si `flag != 0`, viene la rama de "despertar":
   - con clave ≠ 0, busca la clave en `[0x6D6C1F8]` (tabla estática, p. ej. la cama) y saca de
     ella una posición;
   - con clave 0, usa la posición actual (`0x4F8220`);
   - después `0x2B97A20` (`[actor+0x68]+0x110`) y `0x2BC4950`: teletransporte o reaparición.
   - **Con flag 0 no se ejecuta nada de esto.**

**No hay** comprobación de rango, de "hora hacia atrás" ni de enfriamiento: el enfriamiento de
TimeWrap es otro sistema. **Fallos silenciosos:** los puntos 1–3, `[ctx]` nulo y el byte `+0x21`
de la instancia. Pasan sobre todo **durante una carga**. La única forma de confirmar que funcionó
es ver el S→C 2816 en el sniffer del invitado.

**Efectos sobre la partida — confianza M.**
- La hora inyectada pasa a ser la del servidor del invitado: `comp+0x450`. Al salir del field se
  copia al mapa por nivel (§8.5) y al guardar se escribe en `_fieldTimeRaw`.
- Horarios de NPC: el temporizador 2017 cambia el flag noche/día en ≤ 10 ticks (§8.6).
- `GetCurrentFieldTime` tiene 171 puntos de llamada (tiendas, misiones, cultivos…). No se
  revisaron uno a uno.
- Retroceder la hora puede alargar cosas medidas en tiempo de juego (enfriamientos). Riesgo
  medio-bajo.
- Copia de seguridad de las partidas antes de probar.

### 8.4 `SetFieldTimeAck` (2816, S→C) — confianza A

Payload de **35 B**: `u32 clave_actor | FieldTime(31)`. Va al manejador `0xB88990` y de ahí a
`.xtls 0xA41D560`:
- Valida la longitud.
- Lee la clave. Si `clave != 0`, `(clave & 0x30000000) != 0x10000000` y `0x8B53C0(mgr)`, lo
  **rechaza**. Con **clave 0** pasa siempre; las claves de jugador, como `0x90100000`, también.
- Lee `FieldTime` y exige `posición == len`.
- `0x8A67E0` busca el actor solo para llamar a `comp->vf[0x150](0)`.
- Llama a **`0xA38B00(mgr=[[[0x6D69188]+0x30]+0x1148], &ft, inmediato=1, 0, 0, 0)`**, que
  **ignora el actor del paquete**:
  - `mgr+0x50 = 1`;
  - el jugador local es `[[mgr+0x10]+0x50]`, validado con `vf[0xC0](4, 0x10)`;
  - si está enganchado, `0x21A1CC0(comp_cliente, ft)` escribe `comp+0x450` y la global del
    cliente;
  - si no hay actor, escribe la global directamente;
  - si hay actor sin enganchar, no escribe **nada**;
  - al final, `0xA39330(mgr, &ft)` y un evento de interfaz (`0xCAEFC0` → `0x12B6E50`).
- Con `inmediato = 0`, que usan otros llamadores (`0xA384A0`, `0xA38850`, `0xA389B0`: la
  interfaz de dormir), hace una transición animada o de secuenciador (`0xB7AFC0`).

**¿Sirve inyectar solo el Ack como plan B?** Sí, y es seguro: solo toca el cliente, no cambia
el estado del servidor ni la partida guardada. Se inyecta con `clave = 0` y la misma ancla que
en §8.3, porque el cliente también extrapola con `ahora + desfase`. Limitaciones:
- el servidor sigue con su hora: el flag noche/día de los NPC y la hora guardada no cambian;
- el siguiente `MoveFieldCompleteAck`, que lleva la hora del servidor, lo deshace (§8.5).

### 8.5 Cambio de field, viaje rápido y carga — confianza M (orden salida→entrada sin confirmar)

- **Servidor, al moverse:** el temporizador interno `MoveFieldOnceTimer` (2027) llama a
  `.xtls 0x1082C360`, que pasa por `0x2BBF9C0` y `.xtls 0x106DE260` hasta `0x2BBFA80`, y este
  llama a **`0x2BC0A30(ctx, field)`**:
  - Solo si `field+0x180` es el field actual:
    - Nivel de hora fija: `ft = LevelInfo+0x48`.
    - Si no, busca en el **mapa por nivel** `comp+0x5D0` (clave `u32 LevelInfo[0]`):
      - si existe, `ft = entrada+0x10`;
      - si no, inserta (`0x2BD3170`) una por defecto: globales `0x6CF1158` (día), `0x6CF11A8`
        (h), `0x6CF11F8` (min), `0x6CF1248` (s) y `0x6CF12B8` (día de la semana), con escala 12
        y ancla = ahora.
    - `raw = toRaw(ft) + u32([ctx+0x80]·1000)`. Ojo: es un `imul` de **32 bits**. `ctx+0x80`
      son los segundos que se suman al moverse; su origen no se ha identificado.
    - `fromRaw` y escritura en la global del servidor. **No cambia el ancla:** la entrada del
      mapa conserva la suya, así que la hora "siguió corriendo" mientras no estabas.
  - Siempre: `0x21A1CC0(comp, GetCurrentFieldTime(field))`.
- **Servidor, al salir:** `0x2BD0FD0` (desde `0x2BC2280` / `.xtls 0x106F03B0`) guarda
  `mapa[nivel] = {GetCurrentFieldTime, posición}` con `0x2BD3340`.
  - Excepción: con su argumento-flag puesto y `LevelInfo+0x6B == 0`, borra la entrada.
  - Las entradas del mapa se cargan de la partida en `0x2BC2970` (`fromRaw(entrada+0x30 =
    _fieldTimeRaw)`, ancla = ahora de la carga).
  - El único escritor más es `0x2879F90`, un salto con `fromSeconds`.
- **Cliente:**
  - `MoveFieldCompleteAck` (2144; `0xB87B90` → `0x9FA450` → `.xtls 0x9CE7940`) **lleva un
    `FieldTime`** y llama a `0xA38B00(…, inmediato=1)`. La hora del cliente pasa a ser la del
    servidor.
  - `LoadingCompleteReq` (2445, en el servidor; `.xtls 0x1049EA30` → `0x2BCAF00` →
    `0x219D7F0`): global del servidor = `comp+0x450` del jugador y `field+0x17C = 0`.
  - `LoadingCompleteAck` (2762; `0xB8BB00` → `0x9FAD10` → `.xtls 0x9CEAC50` → `0x219D7F0`):
    lo mismo con la copia del cliente.
- **Conclusión:**
  - Dentro de la **misma clave de nivel**, la hora inyectada sobrevive al viaje: se guarda al
    salir y se recarga al entrar, más `ctx+0x80` segundos.
  - Al cambiar a otra clave de nivel, se usa la hora propia de ese nivel (mapa o valores por
    defecto). En niveles de hora fija, la fija.
  - **El invitado tiene que resincronizar después de cada `MoveFieldCompleteAck` y
    `LoadingCompleteAck`.** Conviene esperar unos cientos de ms tras el Ack, porque durante la
    carga `SetFieldTimeReq` falla en silencio (§8.3, puntos 1–2). El umbral de §5.3 evita
    reinyectar si ya cuadra.

### 8.6 ¿Algo fija la hora periódicamente? — confianza A: no

- Los escritores de las globales y los llamadores de `0x21A1CC0` son los de §8.2, y todos son
  eventos: Ack, movimiento de field, carga y `SetFieldTimeReq`.
- `UpdateFieldNightAndDay` (2017) solo lee:
  - `0x2C3BC70` → `.xtls 0x10870540` (contador `[0x6DAA928]`, cada 10 ticks) → `0x2C3BCD0`;
  - por cada field del servidor, `noche = (GetCurrentFieldTime().hora − 4) > 16` en sin signo,
    es decir, de 21 a 23 y de 0 a 3;
  - si cambia respecto a `field+0x2E0`, avisa a los actores (`0x2BF9B60` sobre
    `[actor+0x68]+0xF0`).
- `ServerTimeInfoAck` solo sincroniza relojes.
- **Nada pelea con la inyección**, salvo lo que el propio invitado haga (dormir o cambiar de
  nivel) y §8.5.

### 8.7 Resumen para el código

1. **Tests unitarios:** `to_raw`/`from_raw` con la tabla de §8.1 y el ejemplo de extrapolación
   20:59 → 21:02.
2. **Anfitrión:** lector de §8.2 cada 5 s y al ver 2816/2144/2762 en su sniffer. Se envía
   `raw`, `escala` y flags (bit1 si `anchor == 0`).
3. **Invitado:**
   - `raw_obj = raw + (rtt/2)·escala`;
   - `ft = from_raw(raw_obj)`, `ft.scale = escala`, `ft.weekday = día % 7`;
   - `ft.anchor = ahora_cli + desfase` (los dos del invitado, leídos en el mismo instante);
   - se inyecta `2D 08 00 24 00 | ft(31) | 00 | 00 00 00 00`.
4. **Verificación:** en ≤ 1 s debe llegar un S→C 2816 de 35 B con los bytes 4..34 iguales a
   nuestros 31. Si no llega y no hay carga en curso, se reintenta. Plan B: inyectar S→C
   `2816 | 00 00 00 00 | ft(31)`.
5. **Resincronizar** tras 2144 y 2762.

| Hallazgo §8 | Confianza |
|---|---|
| Aritmética exacta de `toRaw`/`fromRaw`, redondeo y día de la semana | A |
| Fórmula de `GetCurrentFieldTime`; `0x218` = cliente y `0x238` = servidor; global perezosa | A |
| Lector: `[CD68] + [CD70]/1000 + desfase`; el segundo campo en µs | A (y verificado en vivo) |
| Formato de `SetFieldTimeReq` (36 B, el ancla viaja y se usa tal cual) | A |
| Condiciones de fallo silencioso de `0x2BC8970` | A |
| Flag 0 = solo cambia la hora (más `0x2842C70`) | A |
| `0x2842C70` = caducidad de relaciones de facción temporales | B |
| Ack 2816: formato, filtro de clave y efecto en el cliente | A |
| Hora fija en la global dentro de mazmorras; ancla 0 | M / B |
| Supervivencia al viaje dentro de la misma clave de nivel; orden salida→entrada | M |
| Nada fija la hora periódicamente | A |
