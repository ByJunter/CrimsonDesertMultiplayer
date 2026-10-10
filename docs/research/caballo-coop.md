# El caballo en el cooperativo: llamar, montar, cabalgar y desmontar

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`). Análisis de solo lectura: capturas del sniffer y capstone sobre el exe en disco.
El juego no se ejecutó.

Fuentes:
- Captura 2, `captures/cdmp_packets_20261003-112627.bin`: llamada, monta, desmonta, vuelve a montar
  y desmonta. Es la fuente principal.
- Captura 1, `…-105149.bin`: monta (2.265 Move con padre caballo). Usa el sniffer v1, así que las
  cargas están truncadas a 1.019 bytes.
- Las otras 14 capturas solo sirven para la plantilla de carga del caballo.

Los tiempos son segundos desde el inicio de la captura 2.

Scripts y salidas en `build/research/horse/`:

| Script | Qué hace |
|---|---|
| `scan.py` | Cuenta, por captura, los mensajes de caballo, vehículo y mercenario, y las claves padre |
| `timeline.py <cap> [clave]` | Paquetes que contienen la clave del caballo, intervalos de monta e histograma C>S |
| `window.py <cap> t0 t1` | Decodifica Move, StartAction y Rotate del jugador y del caballo en una ventana |
| `loads.py <cap> B0100003 --dump DIR` | Lista las `ActorLoadDataAck` del jugador y del caballo, y las guarda |
| `parentcheck.py <cap>` | Compara el Path de mundo del jinete con el Move propio del caballo |
| `loadparent.py` | Busca el campo padre en las cargas de todas las capturas |
| `wscan.py <regex>` | Escaneo estático de instrucciones |
| `vt.py <vt> [n]` | Imprime slots de una vtable |

Salidas: `scan_out.txt`, `tl_112627.txt`, `w_call.txt`, `w_ride.txt`, `loads_112627/*.bin` y
`d_*.asm`.

---

## 0. Resumen

1. **No existe un mensaje "montar".** `RideOnVehicleReq` (2589) no se envía nunca: 0 veces en 16
   capturas.
   - La monta usa el sistema de **interacción** (`InteractionPreemptionStep`/`DoStep` con la
     clave del caballo).
   - Después vienen `StartActionReq` del jugador con **padre = caballo** y luego `MoveActorReq`.
2. **Mientras se cabalga solo se mueve rápido la clave del jugador.**
   - El jugador envía `MoveActorReq` a unos 36 Hz (mediana 28 ms) con el padre `0xB0100003` en
     `+0x8C`.
   - **El Transform de mundo del Path (`+0x94` pos, `+0xA0` quat) es exactamente el del caballo.**
     Coincide con los Move del propio caballo con la misma marca de tiempo: 94/94, error mediano
     0,000 m, `|dot(quat)|` ≥ 0,9997. Lo mismo en la captura 1 (66/66).
   - El caballo envía su propio `MoveActorReq` solo a ~1 Hz (mediana **1.015 ms**), además de sus
     `StartActionReq` (galope, frenada…).
   - **Consecuencia:** el receptor puede **fabricar los Move del caballo** a ~36 Hz a partir de los
     Move del jugador, sin tráfico nuevo.
3. **El caballo es un actor del usuario que se carga al cargar la partida**, no al llamarlo.
   - Llega en una `ActorLoadDataAck` única (8.770–8.929 B), 0,1 s después de la del jugador.
   - Va en el formato "usuario": clave `0x90100000` en `+0x03`, actor en `+0x0E`, tipo `05` en
     `+0x0D`.
   - Su clave es **siempre `0xB0100003`** (índice 2 de los actores del usuario) en las 16
     capturas.
   - Llamarlo no lo carga: solo lo teletransporta (`CallVehicleMercenaryAck` → `AITeleportReq` →
     `MoveForceAck`).
4. **Mantener la clave padre no basta para que el clon "monte".**
   - En el modo 2 el Path (y su clave padre) de Move, Rotate y StartAction **solo se guarda** en
     las muestras de interpolación; nunca se aplica.
   - El vínculo jinete↔montura vive en el componente de vehículo, y lo escribe
     `CommonVehicleActorComponent::vf53` (`0x2228260`). Su parte que escribe el vínculo
     (`0x222AFD0`) y la versión cliente (`0xA45740`) están **mutadas por Denuvo**.
   - Ningún Ack del flujo capturado escribe el vínculo: `ChangeRideSeatAck` es un stub en este
     cliente.
5. **Receta recomendada, por fases:**
   - **Fase 1, visual y sin vínculo:**
     1. Cargar una copia del caballo con claves nuevas.
     2. Moverla con Move sintetizados desde el Path del jinete (montado) o desde los Move propios
        y la memoria (sin montar).
     3. Reenviar los StartAction y Rotate del caballo.
     4. Traducir la clave del caballo en los campos padre y objetivo del jinete.
     5. Parchear `RideOn::Enter` y `RideOff::Enter` para el clon, igual que `PointClimb`.
   - **Fase 2, opcional y experimental:** crear el vínculo real (inyección de los Ack de
     interacción o llamada a vf53) y deshacerlo con `RideOffByServerAck` (2938), que sí funciona
     en el cliente.

---

## 1. Mensajes alrededor del caballo (captura 2)

### 1.1 Cronología

| t (s) | Dir | Mensaje (opcode, tamaño) | Claves y campos |
|---|---|---|---|
| 42,813 | S>C | `ActorLoadDataAck` (2005, 12.807) | jugador, plantilla actual del clon |
| **42,906** | S>C | **`ActorLoadDataAck` (2005, 8.929)** | **caballo**: `+0x03` usuario `0x90100000`, `+0x0D`=05, `+0x0E`=`0xB0100003`; pos (-10521,16, 608,93, -4417,52) |
| 42,900–42,906 | S>C | `VaryHPAck`, `ApplyBuffAck`×n, `SetMinStatRateAck`, `JoinPartyAck`… | `+0`=caballo (estado inicial) |
| 43,27… | C>S | `RegionInOutReq`, `ChangeAIStateReq`, `StartActionReq`, `ProcessAfterBeginFrameEventReq` | `+0`=caballo: **el cliente simula al caballo** (su IA corre en el cliente) |
| **190,827** | S>C | **`CallVehicleMercenaryAck` (2955, 100)** | ver §1.3 |
| 190,846 | C>S | `CurrentTargetChangeReq` (2984, 18), `UpdateAIDestinationReq` (2405, 57), `ChangeAIStateReq`×2, `StartActionReq` | caballo, objetivo = jugador |
| 191,931 | C>S | `StartAIFunctionReq` (2291, 120), **`AITeleportReq` (2091, 230)** | caballo → punto de llamada |
| 191,945 | S>C | **`MoveForceAck` (2496, 232)**, `ChangeMoveSessionIDAck` (3097, 16: caballo, `0x90100000`, sesión 1) | |
| 191,957 | C>S | `EchoMoveSessionIDReq` (2624, 12) | |
| 192–208 | C>S | `UpdateAIDestinationReq`, `StartActionReq`, `MoveActorReq` (~1 Hz: 195,09 / 196,25 / 197,27) | el caballo camina hasta el jugador |
| 208,198 | C>S | `CurrentTargetChangeReq` (jugador → caballo), **`InteractionPreemptionStepReq` (2671, 25)** | `01 00 10 a0 03 00 10 b0 90 01 00 00 02 00 00 00 …01 00 00 00 00` |
| 208,200 | S>C | `InteractionPreemptionStepSuccessAck` (2663, 29) | `+0` jugador, `+4` jugador, `+8` caballo, `+0xC` 0x190, `+0x10` paso 2 |
| 208,583 / 208,696 | C>S | `StartActionReq` del jugador | objetivo (`+0x16C`) = caballo; A1 = `0x615DE982` / `0xA821A607` (acercarse) |
| 208,869 | C>S | **`InteractionDoStepReq` (2720, 16)** | `01 00 10 a0 03 00 10 b0 90 01 00 00 02 00 00 00` |
| 208,881 | S>C | `InteractionDoStepSuccessAck` (2916, 20) | `01 00 10 a0 01 00 10 a0 03 00 10 b0 90 01 00 00 02 00 00 00` |
| 208,899 | C>S | `StartActionReq` del jugador (A0 `0x26107D3C`, A1 `0xB5713BEA`, objetivo caballo, force=0) y del caballo (A0 `0x27FE18E9`) | inicio de la animación de montar |
| 208,870 → 210,839 | | **Hueco de 1,97 s sin Move del jugador** (la animación mueve el modelo); al volver ha subido 1,6 m | igual que un "vault": el relleno de huecos actual lo cubre |
| **210,810** | C>S | `StartActionReq` del jugador A0 `0x91605616` / A1 `0x375D7006`, **padre `+0x116` = caballo** | ya montado |
| 210,839 → 275,11 | C>S | `MoveActorReq` del jugador con padre = caballo, ~36 Hz | §1.2 |
| (en monta) | C>S | caballo: `MoveActorReq` ~1 Hz, `StartActionReq` (142 en 85 s), `VaryStatByAccInputReq` (2181, 16 B, ~2,7 Hz al esprintar) | |
| 275,136 | C>S | jugador A0 `0x787485E3` (padre caballo, objetivo un NPC); caballo `StartAIFunctionReq` y `ChangeAIStateReq` | desmonta para atacar; desde 275,223 Move con padre −1 |
| 279,19–280,31 | C>S/S>C | interacción paso 3 → jugador A0 `0x99826678`, padre caballo | vuelve a montar (hueco 278,54 → 280,405) |
| **301,609** | C>S | jugador A0 `0xF79C7424` / A1 `0xC8901641` (padre caballo); caballo `StartAIFunctionReq`, `ChangeAIStateReq`, StartAction `0xC8901641` | desmontar; el Move de 301,697 ya tiene padre −1 y **no hay hueco** |

**No aparecen** en ninguna captura:
- `RideOnVehicleReq`, `ChangeRideSeatAck`, `UpdateSeatDataReq/Ack`, `RideOffByServerAck`;
- un `RemoveActorAck` del caballo (no se despidió al caballo en ninguna captura);
- una segunda carga del caballo.

El `ActorLoadDataAck` de `0xB0100123`, unos 3,75 s después de la llamada, que mencionaba
`captura-2.md`, **no es el caballo**.

### 1.2 Quién se mueve al cabalgar

`MoveActorReq` del jugador mientras monta. Ejemplo en t = 210,839 (offsets del payload de la Req):

```
+000 01 00 10 a0  7b d5 21 c6 ad 82 1b 44 2c bb 58 c5   clave jugador | pos mundo del jinete (-10357.37, 622.04, -3467.70)
+010 98 6e 6d bb 85 8d 6a bf c8 e3 74 3b 5a 20 cd 3e   quat del jinete
+020 00 00 00 00 …                                     u32 +0x20 = 0 (siempre al cabalgar)
+04C 5f 0a cd 3b 00 6c ce 3f 8a dd 10 3f …             Path.local = offset sobre la silla (0.01, 1.61, 0.57)
+08C 03 00 10 b0 00 00 00 00                           Path.padre (u64) = 0xB0100003
+094 c9 d3 21 c6 77 1b 1b 44 2b b5 58 c5               Path.mundo pos = POSICIÓN DEL CABALLO (-10356.95, 620.43, -3467.32)
+0A0 00 00 00 00 03 05 6a bf 00 00 00 00 b8 90 cf 3e   Path.mundo quat = ROTACIÓN DEL CABALLO
+0B0 00 00 80 3f ×3                                    Path.mundo escala
+0D5 ts u64
```

Estadísticas:

| Medida | Captura 2 | Captura 1 |
|---|---|---|
| Move del jugador con padre = caballo | 2.964 | 2.265 |
| Separación entre ellos (mediana / p90) | 28 / 31 ms | 29 / 31 ms |
| Move propios del caballo mientras se monta | 1 por segundo, con una `ts` **idéntica** a la de un Move del jinete | |
| Error de `+0x94` frente a la pos del caballo (mediana / máx.) | 0,000 / 0,41 m (el máximo, en el cambio de monta) | 0,000 / 0,004 m |
| Distancia jinete–caballo | 1,45–2,39 m (mediana 1,78) | |

- El Move del jinete nunca trae `+0x94` a cero mientras el padre es el caballo.
- `+0xD4` vale 1 en 133 paquetes.

**El jugador nunca deja de enviar Move al cabalgar ni al desmontar.** El único hueco es la animación
de montar (≈ 2 s).

### 1.3 `CallVehicleMercenaryAck` (2955, 100 B)

```
03 00 10 b0 | 4e 51 13 18 | 00 | 01 00 10 a0 | 00×8 | 45 d9 21 c6 f8 1a 1b 44 0d d3 58 c5 | 00×12 00 00 80 3f | 00 00 80 3f ×3 | 01 | 00…
+0x00 u32 caballo   +0x04 u32 (0x1813514E, ¿id de llamada?)   +0x08 u8
+0x09 u32 dueño (jugador)   +0x15 vec3 destino (-10358.32, 620.42, -3469.19)
+0x21 quat (0,0,0,1)   +0x31 escala (1,1,1)   +0x3D u8 = 1
```

Sirve al emisor para saber la clave del caballo y su nueva posición. **El receptor no lo
necesita.**

---

## 2. La carga del caballo y cómo clonarla

### 2.1 Cabecera (igual que la del actor de usuario del jugador)

| Offset | Jugador (12.807 B) | Caballo (8.929 B) | Significado |
|---|---|---|---|
| `+0x00` | `09 00 00` | `09 00 00` | tipo de mensaje o versión |
| `+0x03` | `0x90100000` | `0x90100000` | usuario dueño |
| `+0x0B` u16 | `0x31FA` | `0x22D4` | **longitud − 13**. No es un contador como decía `captura-2.md`: 12.807 − 13 = 0x31FA y 8.929 − 13 = 0x22D4 |
| `+0x0D` u8 | 01 | **05** | clase: 1 jugador, 4 mercenario o NPC del usuario, **5 montura**, 6 otro (`0xB0100004`) |
| `+0x0E` | `0xA0100001` | **`0xB0100003`** | clave del actor |
| `+0x12` | `0x90100000` | `0x90100000` | usuario |
| `+0x16` u8 | 0 | **2** | índice en la lista de actores del usuario |
| `+0x1F` | `0x90100000` | `0x90100000` | usuario |
| `+0x23` u32 | 1 | **1.003.120** (`0x000F4E70`) | clave de CharacterInfo |

El caballo tiene **un solo paquete**: no lleva una carga de "cuerpo" aparte como el jugador.

Claves dentro del paquete de 8.929 B (offsets absolutos):

| Clave | Veces | Dónde |
|---|---|---|
| `0xB0100003` caballo | 20 | `+0x0E`; pares en 8046…8482, entradas de buff (lanzador y objetivo); 8561 |
| `0xA0100001` jugador | 6 | 259, 7514, 8528, 8553, 8578, 8582 (dueño y relaciones) |
| `0x90100000` usuario | 3 | `+0x03`, `+0x12`, `+0x1F` |
| `0xB0100002` | 1 | 8557 |

En 8549 hay una lista de grupo: `01 03 00 00 | 00 | jugador | 0xB0100002 | caballo | …`. Coincide
con el `JoinPartyAck` de 42,905.

### 2.2 Posición

La posición aparece **3 veces**: en +6615 y en las dos copias del Transform del final. Sus offsets
desde el final no son estables:

| Tamaño | Copia final 1 | Copia final 2 |
|---|---|---|
| 8.929 | 8721 | 8763 (fin −166) |
| 8.927 | 8721 | 8763 (fin −164) |
| 8.770 | 8564 | 8606 |

Localizador robusto:
1. `i = rfind(f32(1,1,1) + FF×8)`, que es la escala del último Transform seguida de su padre −1.
2. La posición está en `i − 28` y el quat en `i − 16`.
3. Hay que **reemplazar todas las apariciones** de esos 12 bytes de posición (salen 3), y el quat
   tras cada copia final.

### 2.3 Claves del clon (propuesta)

| Emisor | Clon en el receptor |
|---|---|
| usuario `0x90100000` | `0x9010F001` (ya en uso) |
| jugador `0xA0100001` | `0xA010F001` (ya en uso) |
| cuerpo `0xB01000xx` | `0xB010F001` (ya en uso) |
| **caballo `0xB0100003`** | **`0xB010F002`** |
| `0xB0100002` (lista de grupo) | `0xB010F003` o 0. Si se deja, apuntaría al `0xB0100002` **del receptor** |

Lo inyectado es `u16 2005 | u8 0xFF | u16 len | payload reescrito`, junto a los dos paquetes del
clon y en la misma cola. La longitud no cambia.

---

## 3. Qué necesita el receptor para que el clon monte

### 3.1 La clave padre en Move, Rotate y StartAction no crea el vínculo

- **Move Ack.** `0x48BC60` copia pos y quat a `TS+0x534/+0x540` y llama a `0x87DB50` y
  `0x87D9C0` con `&Path` (`rbp+0x48`).
  - `0x87D9C0` solo usa el Path para guardarlo en la muestra (`mc+0x3D0`), o en el modo 3 con
    `vf(0x1E0)==4`.
  - `updateInterpolatePositionData` nunca lee el Path de la muestra
    (`movimiento-remoto-cliente.md` §2).
- **StartAction Ack.** `0x48AD40` pasa `B+0x48` (Path) solo a esas mismas dos funciones.
  - `0x232B120(Z, B)` usa `B+0xEC`, el **u32 objetivo** (`Ack+0x16D`), con el componente
    `[actor+0x68]+0x90` (`0x2217290`), y además `B+0xFC`.
  - No hay ninguna escritura del vínculo de monta.
- **Rotate Ack.** `0x505DF0` ignora el Path en el modo 2.

### 3.2 Dónde vive el vínculo

`V = [actor+0x68]+0x48` es el `ClientVehicleActorComponent` (vtable `0x55B6108`; Common
`0x5939EA0`). Lo tienen tanto el jinete como el caballo.

**En el jinete:**
- `L = [V+0x18]` es el objeto vínculo;
- `u32 L+0x10` = **clave del vehículo montado**;
- `i8 L+0x14` = asiento (`0xFF` = ninguno).
- Lo leen `0x17EE200` ("vehículo del que soy jinete": lee `L+0x10` y busca la clave),
  `RideOn::Enter` y `RideOff::Enter`.

**En el vehículo:**
- `u32 V+0x48+4·asiento` = clave del jinete en ese asiento;
- `u8 V+0xC8` = número de jinetes;
- `V+0xEA` y `V+0xEB` son flags (`+0xEB` ≠ 0 = es vehículo con asientos).
- Lo lee `0x17EE530(actor, out, asiento)`.

**Quién lo escribe:**

| Función | Qué hace |
|---|---|
| `CommonVehicleActorComponent::vf53` = `0x2228260(V_vehículo, int* res, Actor* jinete, u8 flag, …)` ("RideOn") | Si el jinete ya montaba otro vehículo, lo desmonta (`0x2228E40`). Escribe el asiento (`0x2228552: mov [r15+rcx*4+0x48], eax`; `inc [r15+0xC8]`) y el vínculo del jinete con `0x222AFD0(L, clave_vehículo, asiento)` (**mutado**, `jmp .xtls`) |
| `ClientVehicleActorComponent::vf53` = `0xA45740` | `jmp 0x9DC90A0` (**mutado**); desde ahí se llama a `0x2228260` |
| vf52 = `0x2229F10` (Client, Common y Server) | cambio de asiento: mueve la clave del jinete en `V+0x48[]` |
| vf54 = `0xA45760` (cliente, no mutado) | probablemente RideOff |
| `0x2228E40` | RideOff; lo llaman vf53 y `RideOffByServerAck` |

### 3.3 Los Ack de monta del cliente

| Ack (opcode, vf2) | Payload | Efecto en este cliente |
|---|---|---|
| `ChangeRideSeatAck` (2659, `0xC1AC40`) | u32 clave, u8, u8, u32, u32 (14 B) | **Ninguno**: lee, busca el actor y vuelve. Es un stub |
| `UpdateSeatDataAck` (2470, `0xB64950`) | u32, u8, f32, f32 (13 B) | Escribe datos de asiento (bancos) en `[[Z]+0xD8]+0x198..0x1A0`. No es de caballos |
| **`RideOffByServerAck`** (2938, `0xB59C20`) | `+0` u32 **vehículo**, `+4` u32 **jinete**, `+8` u8 asiento, `+9` u8 = 0, `+0xA` u8 flag (11 B) | **Funciona**: si el vehículo tiene `V+0xEB` y el asiento cuadra (`0x2227FA0`, mutada, devuelve el asiento), llama a `0x2228E40` (RideOff). Sirve para desmontar al clon si antes se le vinculó |
| `InteractionDoStepSuccessAck` (2916, `0xBB4AC0`) | `+0` actor, `+4` solicitante, `+8` objetivo, `+0xC` id de interacción (0x190), `+0x10` paso (20 B) | Busca `+4` con `0x8ADFE0` y llama a `[[solicitante+0x68]+0x98]` → `0x9B6560` (**mutado**). Ejecuta el paso de interacción, que en el emisor es la monta. **Candidato a vínculo real** (§5, fase 2) |
| `CallVehicleMercenaryAck` (2955, `0xB693D0`) | 100 B | Ordena a la IA local traer el caballo. No aplica a un clon |

### 3.4 Métodos de movimiento de monta (vtables de `mr_methods.py`)

| Método | Índice | vtable | vf2 (modo 2) | Enter (vf7) | Exit (vf8) |
|---|---|---|---|---|---|
| RideOn | 6 | `0x5597138` | `0xAB0540` | `0x681C80` | `0x6829C0` (mutado) |
| RideOff | 7 | `0x5597190` | `0xAB0540` | `0x682C20` | `0x683820` (mutado) |
| Vehicle | 10 | `0x5596EB0` | `0xAB0540` | `0x69E410` (devuelve siempre 1) | `0x69E420` |

**`RideOn::Enter` (`0x681C80`) falla sin vínculo:**
- exige `TS+0x3DC == 0`, que exista `V` del jinete y `u32 [[V+0x18]+0x10] != 0`;
- luego `0x17EE200` (buscar el vehículo), `V_vehículo->vf40` (`0xA453F0`, Transform del asiento) y
  `0x877F90`.

**`RideOff::Enter` (`0x682C20`)** busca el vehículo por la misma clave (`0x8AE940`) y también falla
sin vínculo.

Igual que en PointClimb, el resultado de Enter se guarda en `método+0x44`, y si es 0 el frame se
salta la interpolación y el `SetPosition` (`0x890A78`). **Hipótesis:** el clon se congela durante
las acciones de montar y desmontar y salta al final. Mientras cabalga, si el método es Vehicle (10),
Enter devuelve 1 e interpola bien. Eso explica que el clon se deslice "en postura de montar".

---

## 4. Movimiento del caballo

| Situación | Fuente en el emisor | Qué hace el receptor |
|---|---|---|
| **Montado** (Move del jinete con `+0x8C` = caballo) | Move del jinete, ~36 Hz | **Sintetiza** un MoveActorAck del clon del caballo por cada Move del jinete (receta abajo) y **descarta** los Move propios del caballo (1 Hz y redundantes) |
| Sin montar, IA siguiendo o parado | Move propios del caballo, **~1 Hz** (mediana 1.015 ms, p10 168, p90 1.839) | Más de 1.000 ms entre muestras → **reinicio del búfer y "pop"** cada segundo. El emisor debe **sintetizar desde memoria** a 20 Hz (como `capture_synth`) o, en el espejo con retardo ≥ 1,1 s, el receptor puede interpolar entre muestras |
| Acciones del caballo | `StartActionReq` del caballo (265 en total, 142 montado) | Reenviar como `StartActionAck` (misma receta y rebase de `ts` en `+0x160`). Alimentan el mismo búfer |
| Rotaciones | `RotateActorReq` del caballo (9) | Reenviar |
| Llamada | `AITeleportReq` → `MoveForceAck` | Salto de cientos de metros: aceptar el pop o reenviar `MoveForceAck` (232 B, clave en `+0`). Opcional; ver pruebas |

**Receta del Move sintetizado del caballo** (sobre la Req de 514 B del jinete `r`; después se aplica
el `move_ack` de siempre):

```
h = copia(r)
h[0x00:4]   = clave del caballo del emisor   (luego se traduce a 0xB010F002)
h[0x04:12]  = r[0x94:0xA0]                   pos mundo del caballo
h[0x10:16]  = r[0xA0:0xB0]                   quat del caballo
h[0x20:4]   = 0
h[0x4C:12]  = r[0x94:0xA0]; h[0x58:16] = r[0xA0:0xB0]; h[0x68:12] = (1,1,1)   Path.local = mundo (sin padre)
h[0x8C:8]   = FF×8                            sin padre
h[0x94:0x40] = 0                              Path.mundo a cero, como en los Move propios del caballo
ts (+0xD5) y cola (+0xDE…) = las del jinete
```

- Usa la **misma `ts`** que el Move del jinete. Los dos clones interpolan con las mismas marcas de
  tiempo y el mismo algoritmo, así que el jinete se queda sobre la silla (offset ≈ (0, 1,6, 0,57)
  local).
- El desplazamiento lateral del espejo (`shift_x`) se aplica igual a `+0x04` y `+0x4C`.

---

## 5. Pasos de implementación

### Fase 1: caballo visible, sin vínculo (escritura mínima)

**Protocolo** (`protocol.cpp`, **sube `kVersion`**). Añadir a `SpawnInfo`:
- `horse_key` (u32);
- `horse` (bytes, ≈ 9 KB);
- `horse_pos` (Vec3) y `horse_rot`.

**Emisor** (`local_capture.cpp`):
1. **Plantilla.** En `capture_observe_s2c`, aceptar también la `ActorLoadDataAck` con `+0x03` ==
   `g_user_key` y `+0x0D` == 5: guardar el payload y `g_horse_key = u32(+0x0E)`. Llega 0,1 s
   después del jugador, en la misma generación.
   - Si llega antes que el paquete de usuario del jugador, guardarla igual (va por clave de
     usuario).
   - No aprender de claves del clon (`0xB010F002`).
2. **Última posición conocida** del caballo, para el spawn y el `SpawnInfo`. Se actualiza con:
   - `CallVehicleMercenaryAck` `+0x15`;
   - los Move propios del caballo (`+0x04`);
   - el `+0x94` del jinete.
3. **`capture_observe_c2s`:** aceptar Move, StartAction y Rotate cuyo `u32 +0` sea `g_horse_key`
   (mismos tamaños) y emitirlos con los mismos `EventKind`. La clave del payload distingue jugador
   y caballo, así que no hacen falta tipos nuevos.
4. **Síntesis desde memoria** del caballo sin montar:
   - buscar la clave → actor (receta de `movimiento-remoto-cliente.md` §7);
   - leer `TS+0xB4` + sector (`+0xC0`/`+0xC2`) ×1000 y el quat `TS+0xA4`;
   - emitir un Move del caballo cada 50 ms si se movió más de 5 cm y no hay Move del jinete con
     padre = caballo en los últimos 200 ms.
   - Plantilla: el último Move propio del caballo. `ts` = reloj de juego extrapolado.
5. Ignorar las Req con claves del clon (añadir `0xB010F002` a los filtros).

**Receptor** (`remote_puppet.cpp`):
1. **`puppet_spawn`:** si llega `info.horse`:
   - `replace_u32`: usuario → `0x9010F001`, jugador → `0xA010F001`, caballo → `0xB010F002`,
     `0xB0100002` → `0xB010F003`;
   - posición y quat con el localizador de §2.2, más el offset X;
   - encolar la tercera `ActorLoadDataAck` **después** de la del jugador.
2. **`puppet_event`:** despachar por `u32 +0`.
   - `== src_player`: como ahora, pero **traducir antes** `src_horse` → `0xB010F002` con
     `replace_u32`. Cubre el padre (`+0x8C`, `+0x116` Req y `+0x54`) y el objetivo (`+0x16C`).
     - `clear_parent()` pasa a dejar `0xB010F002` y poner a −1 cualquier otra clave padre: bancos,
       barcos… (en la captura 2 hay `0xB0100D4E` en 1.027 Rotate).
     - Si `+0x8C` era el caballo: encolar además el **Move sintetizado** del caballo (§4), con el
       mismo `due` y el mismo rebase de `ts`. Marcar `ridden_until = now + 200 ms`.
   - `== src_horse`: traducir a `0xB010F002` y aplicar las mismas recetas.
     - **Descartar sus Move mientras `ridden_until`**: su `ts` coincide con una ya inyectada y el
       cliente la tiraría de todos modos.
     - No llamar a `fill_gap_before` ni tocar `g_last_move`, `g_ref_pos` y similares (son del
       jinete). Usar un estado de "último Move" aparte para el caballo.
3. **Hooks** `RideOn::Enter` (`0x681C80`) y `RideOff::Enter` (`0x682C20`), los dos al inicio de
   función en `.rsrc` (prólogo `mov rax, rsp`).
   - Si la original devuelve 0 y `u32 [actor+0x90] == 0x9010F001`, devolver 1, igual que
     `point_climb_enter`.
   - Comprobar la firma de bytes como `kPointClimbEnterHead`.
   - Solo si la prueba P2 confirma la congelación.
4. **`puppet_remove`:** añadir un `RemoveActorAck` para `0xB010F002` antes que los del jugador.
   - El byte de tipo de los actores del usuario observado es `0x13` (y `0x25`); el actual `0x15`
     probablemente sirve.
   - Con los actores del usuario (725 y 760 s), el servidor real envió cada `RemoveActorAck`
     **dos veces**.
5. El relleno de huecos actual ya cubre el hueco de montar (1,97 s y +1,6 m, casi vertical).
   Conviene que también recoloque los StartAction congelados del jinete en ese hueco; ya lo hace.

### Fase 2 (opcional): vínculo real, para IK de piernas, sin colisión jinete–caballo y con los métodos de monta "de verdad"

- **Opción A (sin código nuevo de llamada).** Al ver `InteractionDoStepReq` del emisor con el
  objetivo = caballo, inyectar al clon:
  - `InteractionPreemptionStepSuccessAck` (2663): `[clon, clon, clon_caballo, 0x190, paso, …]`,
    copiando el resto de la Ack real;
  - `InteractionDoStepSuccessAck` (2916): `[clon, clon, clon_caballo, 0x190, paso]`.

  El emisor tiene que reenviar esas dos Ack S>C (las ve `capture_observe_s2c`) como evento nuevo,
  o el receptor las fabrica a partir de las Req. Riesgo: `0x9B6560` está mutado y puede hacer cosas
  locales (cámara o control).
- **Opción B.** En el hilo del juego, llamar a `V_caballo->vf53` (slot 53, `+0x1A8`) con
  `(V_caballo, &res, actor_clon, 0, asiento 0…)`.
  - La firma completa (argumentos de pila; `[rbp+0x168]` = 5.º) está sin cerrar: ver
    `d_2228260.asm`.
  - Más frágil. Hay que proteger con SEH.
- **Desmontar:** cuando el Move del jinete pase de padre caballo a −1, inyectar
  `RideOffByServerAck` (2938, 11 B) = `clon_caballo | clon | 00 | 00 | 00`.
- Con el vínculo puesto, comprobar que el jinete no "pelea" entre su búfer de red y el Transform
  del asiento. Si pelea, dejar de enviarle Move mientras monta y usar solo el caballo.

---

## 6. Riesgos

1. **Colisión de claves.**
   - El caballo **siempre es `0xB0100003`** (índice 2 del usuario) en cualquier partida. En el
     cooperativo el del amigo choca con el del receptor: **hay que traducirlo siempre**, y también
     en `+0x16C`/`+0x16D` (objetivo) y en `+0x20`/`+0xAA` cuando valgan eso.
   - Los serials de mundo crecen unos 250 por minuto: `0x0E91` en 14 min (captura 2) y `0x1023` en
     19 min (captura 1). `0xB010F001`/`F002` se alcanzarían en unas **4 horas** si el serial es de 16
     bits. Opciones:
     - usar claves más altas (`0xB01FF00x`), después de confirmar que el serial es de 20 bits;
     - o vigilar en `capture_observe_s2c` si el servidor real carga una de nuestras claves y, en
       ese caso, reasignarla.
2. **El caballo es un mercenario con datos de guardado.**
   - La copia es solo del cliente: el servidor del receptor no la conoce. **No inyectar**
     `MercenaryDataListAck`, `RefreshMercenaryDataAck` ni `CallVehicleMercenaryAck`, porque escriben
     en la lista de mercenarios o dan órdenes de IA del receptor.
   - El cliente puede emitir algunas Req con la clave del clon. Con el clon del jugador se vieron
     `MoveActorReq`, `RegionInOutReq` y `UpdateGimmickReq` de `0xB010F001`, de 1 a 4 veces. El
     servidor las ignora.
   - Revisar en vivo si aparece un segundo icono de caballo en el HUD o el minimapa.
3. **Modo de control del caballo clon.** Tiene que quedar en el modo 2: `+0x90 = 0x9010F001` y
   `+0xC8 = 0`.
   - Si el cliente lo pusiera en el modo 3, por el componente Vehicle (`0x88E8E0`, paso 5), correría
     IA local e ignoraría las posiciones de red.
   - Si `+0x5E != 0x10` o `+0xCF == 0`, sus Move quedarían reintentándose sin fin.
4. **Sin vínculo (fase 1):**
   - la cápsula del jinete clon puede chocar con el caballo clon y empujarlo;
   - las piernas no se ajustan a la silla (RiderIK y PoseModifierVehicle necesitan el vínculo);
   - congelaciones en montar y desmontar si no se parchea Enter.
5. **Caballo sin montar a 1 Hz:** pops cada segundo si no se sintetiza desde memoria.
6. **Teletransporte al llamarlo:** salto de hasta ~1 km (190,8 s: de (-10521, -4417) a (-10358,
   -3469)). En el modo 2 se recorre en unos 200 ms.
7. **Hilo:** todo lo inyectado va por la cola actual, dentro del hook de `OnRecvPacket`. La fase 2,
   opción B, exigiría ejecutar en el hilo del juego.

---

## 7. Pruebas en vivo que cierran lo que falta

Hay que hacer copia de `save` antes de las que escriben.

| # | Prueba | Cómo | Resultado esperado |
|---|---|---|---|
| P1 | El clon del caballo carga y acepta Move | Spawn con F9 + caballo; leer en el actor `0xB010F002` (gestor `[[exe+0x6D69188]+0x38]`): `+0x60`, `u16 +0x5E`, `+0xCF`, `u32 +0x90`, `+0xC8`, `byte [[+0x88]+1]` y el modo `0x88E8E0` | `0x10`, 1, `0x9010F001`, 0, tipo ∈ {1,3..9}, modo 2 |
| P2 | Métodos y Enter al montar | Leer cada 30 ms `byte mc+0xF0` del jugador real y del clon, y `método+0x44` del clon, al montar, cabalgar y desmontar | Jugador: 6 → 10 → 7 (hipótesis). Clon: `+0x44 = 0` en 6 y 7 sin el hook |
| P3 | Vínculo | Leer `u32 [[[clon+0x68]+0x48]+0x18]+0x10` y `u32 [[caballo_clon+0x68]+0x48]+0x48` | Fase 1: 0. Tras la opción A o B: `0xB010F002` y `0xA010F001` |
| P4 | Opción A | Inyectar las dos Ack de interacción para el clon y repetir P3 | Si no se vincula, descartar la A |
| P5 | Despedir al caballo y llamarlo desde lejos | Capturar con el sniffer | Mensajes y tipo de `RemoveActorAck` |
| P6 | Serial de 20 bits | Partida larga (más de 4 h) o buscar un `0xB0110000+` en una captura | Decide el rango de claves del clon |
| P7 | Colisión jinete–caballo en la fase 1 | Captura de pantalla con `CopyFromScreen` en el espejo con `mirror_offset_m=0` y 2 m | Ver si el jinete flota o se desplaza |

---

## 8. Prueba en vivo 1: el caballo no se creó

Build **1.0.0.2976**. Prueba del 2026-10-03 16:02 (espejo en un solo PC, offset 0). Captura
`captures/cdmp_packets_20261003-160139.bin`, log `captures/cdmp_160126_test_v4.log`. Análisis
estático de solo lectura; el juego no se ejecutó. Scripts y salidas en `build/research/horse2/`:

| Script / salida | Qué es |
|---|---|
| `loads.py` | Lista las 2005 con su cabecera |
| `win.py a b claves…` | Paquetes `#a..#b` con nombre y posiciones de las claves |
| `keyrefs.py claves…` | Qué mensajes (y en qué dirección) contienen cada clave |
| `diff.py` | Diff byte a byte carga real (`#1479`) frente a la copia (`#44068`); guarda ambas y las `RefreshMercenaryDataAck` |
| `xtrace.py <rva>` | Sigue código reubicado en `.xtls` (jmp/jcc) y lista llamadas; con `XV=1` imprime las instrucciones |
| `d_*.asm`, `x_*.txt` | Desensamblados citados abajo |

### 8.1 Qué hay en la captura

- Las tres inyecciones son `#44066` (12.971 B, usuario/jugador), `#44067` (19.149 B, cuerpo) y
  `#44068` (8.770 B, caballo), en ese orden y en 3 ms. El orden de la cola era el esperado.
- **El cliente nunca mandó nada con `0xB010F002` ni `0xB010F003`** (`keyrefs.py`). Tras el cuerpo
  sí mandó, a los 0,27 s, 4 `RegionInOutReq` y 1 `UpdateGimmickReq` de `0xB010F001`.
- Tras la carga real del caballo (`#1479`, t = 44,49) el cliente mandó `RegionInOutReq`,
  `ChangeAIStateReq` y `StartActionReq` de `0xB0100003` a los 0,3 s. Pero eso es porque su dueño es
  el jugador local (IA local); un caballo de otro dueño no tiene por qué mandar Req. La ausencia de
  Req es coherente con "no creado", no lo prueba. La prueba es la búsqueda en memoria.
- **La copia es idéntica a la real salvo las claves** (`diff.py`). La posición no cambió porque el
  caballo no se había movido. No hay campos de estado del usuario distintos: solo cambian
  usuario, jugador, caballo y el miembro de grupo (`+8400`).
- En la sesión real el servidor manda **antes** de cada actor de usuario (`#1461–#1478` para el
  caballo): `VaryHPAck`, `ApplyBuffAck`×n, `UpdateChangeBuffLevelAck`,
  `DataDefinedRegenerateValueAck`, **`RefreshMercenaryDataAck`** (2861; `+0` jugador, `+4`
  caballo, `+8` u64 n.º de mercenario = 12, datos 11,6 KB), **`JoinPartyAck`** (2425) y
  `SetMinStatRateAck`. Igual para `0xB0100002` (n.º 223) y `0xB0100004` (n.º 650). Nosotros no
  mandamos nada de eso.

### 8.2 Formato real de `ActorLoadDataAck`: una lista de registros

El manejador (`vf2 0xCB25A0` → `.xtls 0xA79EA60`, solo reubicado, legible) recorre el payload como
`[u16 len][registro]…` y llama a `0x8BD530(ClientActorManager, registro)` por cada uno. El
`OnReceive` propio (`0xCB23C0` → `0xA79D360`) solo encola el paquete.

El paquete del caballo son **dos registros**:

| Offset | Campo | Real | Copia |
|---|---|---|---|
| `+0x00` | u16 len del registro 0 | 9 | 9 |
| `+0x02` | registro 0: u8 tipo = 0 (usuario), u32 clave, u32 | `00 90100000 0` | `00 9010F001 0` |
| `+0x0B` | u16 len del registro 1 (= total − 13) | `0x2235` | `0x2235` |
| `+0x0D` | u8 **tipo de actor** (1 jugador, 4, **5 montura**, 6, 7 actor de mundo) | 5 | 5 |
| `+0x0E` | u32 clave | `0xB0100003` | `0xB010F002` |
| `+0x12` | u32 **dueño (usuario) del registro** | `0x90100000` | `0x9010F001` |
| `+0x16` | u8 **índice en el contenedor de hijos del usuario** | 2 | 2 |
| `+0x17` | u64 | −1 | −1 |
| `+0x1F` | primer u32 de los datos = **`actor+0x90`** (lo lee el unpack `0x17EA310`) | `0x90100000` | `0x9010F001` |

Lo que `captura-2.md` llamaba "clase" es el tipo del registro, y el `u16 +0x0B` es su longitud.
El cuerpo del jugador es un registro de tipo 7 con dueño 0 e índice `0xFF`.

### 8.3 Camino de creación de un registro con dueño (`0x8BD530`)

1. Registro de usuario (tipo 0): si el usuario ya existe (`0x8B3C00` lo encuentra vivo) **se
   salta**. El segundo registro `0x9010F001` de nuestro paquete del caballo es inocuo.
2. Registro de actor: lee tipo, clave, dueño, índice y u64. Si dueño == usuario local, comprueba
   que el actor no exista ya. Luego `0x8AF580(mgr, out, tipo, clave, dueño, índice, u64, bool)`:
   - **dueño ≠ 0:** `0x8ADFE0(dueño)` busca el usuario y **exige `u16 [usuario+0x5E] & 4`**
     (usuario "logined"). Si no, falla con "Actor 생성에 실패했습니다" (`0x55B6510`).
     Después llama a la fábrica `0x4634C0` (`.xtls 0x898D890`). Esta crea un
     `ClientChildOnlyInGameActor` e inicializa (`vf8 0x4F8FA0` → `0x2067F40`, sin analizar). Mete
     el actor en `slot[índice]` del `ClientChildContainerActorComponent` del usuario
     (`[[usuario+0x68]+0x18]`, 100 ranuras de 16 B: ptr, ocupado `+8`, reservado `+9`). Falla
     si la ranura ya está ocupada por otro. Luego `0x206B2D0` confirma la ranura (`vf40
     0x211A230`) y engancha el actor al contenedor del personaje principal `[usuario+0xD0]`
     (`vf41 0x211A2A0`).
   - **dueño == 0:** fábrica `0x4631C0(slot, clave, bool)`, sin usuario ni contenedor. Solo prohíbe
     el tipo 1 (jugador).
3. `0x17EA310` (unpack): `actor+0x90` ← u32 `+0x1F`, y luego `vf11` de cada componente. El de
   mercenario (`0x2154790`) solo lee dueño, n.º, u32, u8 y u8, y no falla por contenido.
4. `vf13` y `vf14` de cada componente ("processAfterLoadDb" y "onProcessLoadData"). El de mercenario
   cliente (`vf14 0x9E76A0`) **solo exige los datos de mercenario (`RefreshMercenaryDataAck`) si
   el dueño del componente es el jugador local**. En la copia el dueño es `0xA010F001` (offset
   8425 del paquete, n.º 12 en 8429), así que devuelve éxito. IA (`0x915940`) e inventario
   (`0x2135960`) no dependen del dueño.

Cada fallo pasa por el registrador `0x35D8D0` (`jmp .xtls`) con el texto coreano en el 12.º
argumento (`[rsp+0x58]` en la llamada) y el código en el 10.º (`[rsp+0x48]`). El juego no escribe
ningún log en disco.

### 8.4 Por qué no se creó (lo que se sabe y lo que no)

- **Descartado:** formato o longitudes (son idénticos al real), datos de mercenario (no se exigen
  con dueño remoto), rango de índice (100 ranuras), registro de usuario repetido (se salta) y
  orden de la cola.
- **No se pudo cerrar en estático:** la inicialización mutada `0x2067F40` y las transiciones de
  estado tras crear (`vf 0x60/0x68`, `[actor+0x5E]` 1→2→4…).
- **Hipótesis principal (suposición):** falla la parte "actor de usuario" del camino con dueño. O
  bien el usuario `0x9010F001` ya no tiene el bit 4 en `+0x5E` cuando llega el registro, o la
  ranura 2 o el enganche al personaje principal fallan. Esta parte no existe en el camino sin
  dueño, que es el que usa el cuerpo `0xB010F001`, y el cuerpo sí funciona.

### 8.5 Cargarlo como actor sin dueño (pregunta 3)

- Basta con poner a 0 el **dueño del registro** (`u32 +0x12`) y **dejar `+0x1F` = `0x9010F001`**.
  Así el actor se crea por `0x4631C0` (como el cuerpo) y conserva `actor+0x90 ≠ 0`.
- Por eso el modo de control (`movimiento-remoto-cliente.md` §1.1, regla 3) sigue siendo **2**:
  consume nuestros `MoveActorAck` y no ejecuta IA local. **No** hay que poner `+0x1F` a 0: sin
  usuario el modo lo decide el componente Vehicle (regla 5) y podría ser 3 (IA local, ignora el
  búfer de red).
- Consecuencias: no entra en la lista de actores del usuario clon. Probablemente no sale un
  segundo icono de caballo (suposición), y `RemoveActorAck` por clave sigue valiendo. El
  componente de mercenario sigue apuntando a `0xA010F001` y se registra en el clan del clon
  (inocuo). El vínculo de monta vive en el componente Vehicle y no depende de esto.
- Cambiar el tipo a 4 o 6 no aporta nada y cambiaría el comportamiento: hay que dejar el 5.

### 8.6 Cambio recomendado y cómo verificarlo

**A (primero, 4 bytes).** En `spawn_horse`, tras reescribir las claves: `u32 [horse+0x12] = 0`.
- No tocar `+0x03` (registro de usuario, se salta), `+0x16` ni `+0x1F`.
- Opcionalmente, quitar el registro 0 y enviar solo `[u16 len][registro 1]` (payload `+0x0B…`).
- Verificar: buscar `0xB010F002` en el mapa del gestor (`[[exe+0x6D69188]+0x38]`, el mismo que
  `0xB010F001`). Si aparece, leer `u32 +0x90` (= `0x9010F001`), `u16 +0x5E` (= `0x10` cuando
  termine) y el modo `0x88E8E0` (= 2), es decir, la P1 de §7. En la captura deberían aparecer
  `RegionInOutReq` de `0xB010F002` como con el cuerpo (suposición).

**B (si A no basta, diagnóstico).** Hook inline (safetyhook, inicio de función) en `0x35D8D0`.
- Registrar los argumentos 10 y 12 y la dirección de retorno cuando esta caiga en `0x8BD530–0x8BE042`,
  `0x8AF580–0x8AF94E` o en la fábrica `.xtls 0x898D890`.
- El texto dice la etapa exacta: "Actor 생성 실패" = creación o dueño; "unpackInformation";
  "postProcessForCreate"; "onProcessLoadData"; "Attach 처리".
- Además, leer el usuario `0x9010F001` (mismo mapa): `u16 +0x5E` (bit 4), `[+0xD0]` (= clon del
  jugador) y las ranuras 0–3 de `[[+0x68]+0x18]+0x18`.

**C (si se quiere el camino "usuario" completo).** Reproducir el preámbulo del servidor con las
claves traducidas: `RefreshMercenaryDataAck` (`+0` → `0xA010F001`, `+4` → `0xB010F002`, solo toca
el clan del clon) y `JoinPartyAck`, antes de la carga. Es más trabajo de protocolo (hay que
capturarlos en el emisor), y según §8.3 no debería hacer falta.

---

## 9. Cierre del juego al crear la copia del caballo

Build **1.0.0.2976**. Análisis de solo lectura (2026-10-03) del volcado
`captures/horse_crash_210557.dmp`, del log `captures/cdmp_210510_horse_crash.log` y de las capturas
`bin64/cdmp_packets_20261003-210446.bin` (la de la sesión del cierre; se corta en t = 56,54 s, antes
de inyectar el caballo), `…-205914.bin`, `…-160139.bin`, `…-131004.bin`, `…-134008.bin` y
`…-125936.bin`. El juego no se ejecutó. Scripts y salidas en `build/research/horse3/`:

| Script / salida | Qué hace |
|---|---|
| `mdmp.py <dmp>` | Lector de minidump: excepción, registros, módulos y paseo de la pila (`mdmp_out.txt`) |
| `allthreads.py` | Primeras direcciones de retorno del exe en cada hilo del volcado (`allthreads.txt`) |
| `keyscan_dmp.py` | Busca claves de actor en la memoria del volcado (no hay ninguna: el volcado solo trae pilas) |
| `lin2.py a b` | Desensamblado lineal con límites de `.pdata` (`l_*.asm`) |
| `recs.py <cap> [claves]` | Divide cada `ActorLoadDataAck` en registros (`recs_210446.txt`) |
| `win.py <cap> a b` | Paquetes `#a..#b` con nombre y posición de las claves (`win_210446_1361.txt`) |
| `firstact.py`, `firstact_pl.py` | Primer `StartActionReq` del caballo / jugador tras su carga |
| `spawnack.py`, `keyfirst.py`, `tail.py`, `dupcmp.py` | Cruces de cargas, Acks y claves; comparación de paquetes |
| `seqscan.py`, `slotw.py`, `w18.py`, `w58.py` | Escaneos estáticos de escrituras (sin resultados útiles: el escritor está fuera de `.rsrc` o inlined) |

### 9.1 Dónde se cierra (pregunta 1)

**Función:** `0x1F63F70` (en `.rsrc`). `.pdata` la parte en `0x1F63F70–0x1F63FAD` y un trozo encadenado
`0x1F63FAD–0x1F6415A`; por eso `cddis` la cortaba. Prólogo `48 8B C4 57 48 81 EC 80 00 00 00 C5 F8 29
78 D8`. Firma deducida:

```
float avanzar_tiempo_capa(Reproduccion** pr /*rcx = &capa+0x18*/, float t /*xmm1*/,
                          float dt /*xmm2*/, float ritmo /*xmm3*/, float* resto /*[rsp+0x28]*/)
  r = *pr;                       // 0x1F63FDA  mov rax,[rcx]      -> r = NULL
  datos = r->+0x10;              // 0x1F63FE6  mov rdx,[rax+0x10] -> lee 0x10: EL CIERRE
  ... secciones en [r+0x38] (+0x6C desplazamiento, +0x6E número), [r+0x48], [r+0x40]
```

Avanza el tiempo de la acción que reproduce una **capa de acción** del componente
`ClientCharacterControlActorComponent` (CC) recorriendo sus secciones, y devuelve el tiempo consumido.

**Pila** (reconstruida marco a marco con los prólogos; las direcciones de retorno cuadran):

| Marco | Función | Retorno | Datos del volcado |
|---|---|---|---|
| 0 | `0x1F63F70` | — | `rcx = 0x5A954D77F98` (= capa + 0x18), `rax = 0` |
| 1 | `0x2329700` (avanza el tiempo de las 2 capas) | `0x232985D` | `r14 = CC = 0x5A955142D00`; `rdi = capa = [CC+0x88] = 0x5A954D77F80`; **índice de capa `bl = 0` (la principal)** |
| 2 | `0x863E80` (actualiza las capas) | `0x8641CB` | `dt` de las dos capas = **6,4854 s** (`0x40CF88D9`) |
| 3 | `0x85FBA0` (actualización del CC; rama `[CC+0x1FC] != 1`) | `0x85FD4F` | |
| 4 | `0x8B56D0` (tarea paralela "actualizar actor", vía el thunk `0x8C4D50`) | `0x8B5766` | actor = `0x5A954D776C0`; `dt = [actor+0x108]` |
| 5 | `ParallelTaskWorker::vf3` `0x13FB350` | `0x13FB680` | hilo de trabajo 16452 |
| 6 | bucle de hilo `0x1414DE0` | `0x1414EBF` | |

`0x86B212` en la pila es basura de una llamada anterior a `CC->vf79` (`0x86B080`, el "ritmo" de la
capa). El hilo principal (16144) estaba esperando a los trabajadores dentro de `0x8BC770`, el tick del
gestor de actores.

**Qué es nulo.** La capa es un objeto de `0x118` bytes (constructor `0x2435B70`, todo a cero) en
`[CC+0x88]` (capa 0) y `[CC+0x90]` (capa 1):

| Campo de la capa | Significado (deducido) |
|---|---|
| `+0x00`, `+0x08`, `+0x10` | acción actual (`+0x08` datos con velocidad en `+0x27C`; `+0x10` nodo con estado en `+0xEC` y flags en `+0x10A`) |
| **`+0x18`** | **reproducción de la acción actual. Es el puntero nulo** |
| `+0x20` / `+0x24` | tiempo anterior / actual |
| `+0x110` | gráfico de acciones propio de la capa (se consulta con `0x24314C0(gráfico, id, out)`; si no hay, se usa `[CC+0x70+8·capa]`). **Suposición** |

`0x2329700` solo comprueba `[capa+0x110] != 0` (en el CC de `[[[CC+0x58]]+0x68]+0x40`; `[CC+0x58]` es
un objeto de 0x18 B con dos `float` 1,0 cuyo primer campo es un actor, casi seguro el propio) y que
`dt > 0` y `ritmo > 0`. **No comprueba `+0x18`.** Todos los demás consumidores sí lo comprueban
(`0x863E80`, `0x2436120`, `0x2436200`, `0x232FAB0`, `0x1F621C0`). Los otros dos que llaman a
`0x1F63F70` tampoco lo comprueban: `0x2BCBF50` y el mutado `.xtls 0xCA9B360`.

**Conclusión:** el actor tiene la **capa principal con gráfico pero sin ninguna acción en marcha**.
No falta mercenario, grupo, asiento ni estadísticas: falta **la primera acción**.

### 9.2 Por qué la copia no tiene acción y el caballo real sí

- **El caballo real lo arranca su propia IA.** En 6 de 6 capturas, tras su carga, el cliente manda
  `ChangeAIStateReq` ×2 y, **0,31–0,41 s después de la carga, `StartActionReq` con A0 =
  `0x27FE18E9`** (la espera). Luego la repite cada 5–13 s mientras está quieto (`firstact.py`).
- Todos los actores del mundo de una partida tienen `+0x1F` (`actor+0x90`) = usuario local, así que
  los simula el propio cliente (modo 3). Las únicas copias en modo 2 (sin IA local) son las nuestras.
  En `160139` hay 16 monturas (tipo 5) de dueño 0, todas con `+0x1F` = usuario local, y ninguna
  recibe `StartActionAck`: todas tienen IA local.
- **La copia está en modo 2:** nadie le arranca una acción hasta que llega un `StartActionAck` de la
  red, y el caballo del emisor quieto solo manda uno cada 5–13 s. Mientras tanto, en cuanto el CC
  carga el gráfico de la capa, `0x2329700` llama a `0x1F63F70` con la reproducción nula.
- El jugador copia también nace sin acción, y su primer `StartActionAck` llega a 0,9–1,5 s. El jugador
  real tampoco manda acción hasta 1,5–2,5 s tras la carga. **No se cierra**, así que su capa principal
  no tiene gráfico propio (`+0x110 = 0`) en ese intervalo (**suposición**). El caballo sí lo tiene.
- Encaja con las tres pruebas: A (no se creó) → sin cierre; B (sin dueño) y C (con dueño) → mismo
  cierre. El camino de creación da igual: el fallo es del actor montura sin acción.

**Pista que no encaja del todo.** El `dt` de ese actor es 6,4854 s. Coincide (±0,5 s) con el tiempo
desde la carga del caballo **real** (t = 51,26 s → 21:05:51,31; cierre ≈ 21:05:57,8), no desde la
copia (inyectada a las 21:05:56,885). Hay dos lecturas:
- **H1 (más probable).** Es la copia y el planificador de actualización lejana (las dos están a unos
  700 m del jugador) le asigna el `dt` acumulado de su grupo o celda. Encaja con que el fallo siga a la
  creación de la copia en B y en C.
- **H2.** Es el caballo **real** `0xB0100003`, al que la copia "despierta" o estropea (comparten el
  n.º de mercenario 12 y la posición +2,5 m).

El volcado no puede decidir: solo trae pilas y no hay ninguna clave en su memoria. Lo decide la
prueba V1 de §9.6.

### 9.3 El preámbulo del servidor (pregunta 2)

Caballo real, captura `210446`, `#1474–#1492`, todo en 8 ms, **antes** de la carga:

| # | Opcode | Tamaño | Campos con claves |
|---|---|---|---|
| 1474 | `VaryHPAck` 2147 | 92 | `+0` caballo |
| 1475/1478 | `ApplyBuffAck` 2506 | 77 | `+0`, `+0x18`, `+0x1C` caballo |
| 1476/1479 | `UpdateChangeBuffLevelAck` 2252 | 42 | `+0` |
| 1477 | `DataDefinedRegenerateValueAck` 2586 | 90 | `+0`, `+0x54` |
| 1480 | `VaryDataDefinedStaticStatAck` 2434 | 41 | `+0` |
| 1481 | `RefreshMercenaryDataAck` 2861 | 11.618 | `+0` jugador, `+4` caballo, `+8` u64 n.º 12, `+0x3E` caballo |
| 1482 | `VaryHPAck` | 92 | `+0` |
| 1483–1484 | `ApplyBuffAck` ×2 | 77 | |
| 1485 | `JoinPartyAck` 2425 | 42 | `+1`, `+5` caballo, `+9`, `+0x1E` jugador, `+0x22` `0xB0100002`, `+0x26` caballo |
| 1486 | `SetMinStatRateAck` 2645 | 105 | `+0` |
| 1487–1491 | `ApplyBuff` ×3, `VaryDataDefinedStaticStat`, `DataDefinedRegenerateValue` | | `+0` |
| 1492 | `ActorLoadDataAck` 2005 | 8.770 | |

Ninguno toca acciones ni capas: son vida, buffs, regeneración, ficha de mercenario y grupo. **No
arreglan el cierre (confianza media-alta).** Solo servirían para la barra de vida o el HUD del
caballo. Si alguna vez se inyectan, que sea **después** de la carga (con el actor sin terminar de
cargar, los Ack van a la cola de reintentos, `mirror-ack-formats.md`). Traducción: caballo →
`0xB010F002`, jugador → `0xA010F001`, `0xB0100002` → `0xB010F003`. Riesgo: `RefreshMercenaryDataAck` y
`JoinPartyAck` escriben listas de mercenarios y de grupo (§6.2).

### 9.4 Receta segura

**R1. Protección contra el cierre (obligatoria; confianza alta).** Hook al **inicio** de `0x1F63F70`
con `safetyhook::create_mid`; antes, comprobar la firma de §9.1.
- Si `*(void**)rcx == nullptr`: escribir `0.0f` en `*(float*)[rsp+0x28]`, poner `xmm0 = 0` y volver
  (`ctx.rip` → un stub `ret`).
- Es lo mismo que hace el llamador cuando el ritmo es ≤ 0 (`0x2329863`): la capa no avanza.
- **Cuidado:** `0x2329700` usa `r9` tras la llamada, porque el compilador sabe que `0x1F63F70` no lo
  toca. Un hook inline con detour en C++ lo machacaría. Hace falta un mid hook (restaura todos los
  registros) o un stub en ensamblador que no toque `r9`. Hay que comprobar que la versión de
  safetyhook acepta cambiar `ctx.rip`.
- Cubre las tres llamadas (`0x2329858`, `0x2BCC166`, `.xtls 0xCA9B497`).
- Opcional para diagnóstico: un mid hook de **solo lectura** en el inicio de `0x2329700` (rcx = CC)
  que registre una vez la clave `u32 [[CC+8]+0x60]` cuando `[capa+0x110] != 0 && [capa+0x18] == 0`.

**R2. Darle la primera acción a la copia (confianza media).** Inyectar un `StartActionAck` de
`0xB010F002` con la espera **`0x27FE18E9`** **justo detrás** de su `ActorLoadDataAck`, en la misma
tanda.
- Plantilla: el último `StartActionReq` del caballo del emisor con A0 = `0x27FE18E9` (725 B).
- Se convierte con `start_action_ack()` como en `horse_event`, con las claves traducidas.
- Hasta que la copia termine de cargarse (`+0x5E == 0x10`, `+0xCF == 1`), el Ack espera en la cola de
  reintentos y se aplica en cuanto puede.
- Variante sin cambiar el protocolo: **retener la carga del caballo hasta que llegue el primer evento
  StartAction del caballo del emisor** (cada 5–13 s quieto) y encolar carga + Ack juntos.
- Variante con protocolo: añadir `horse_action` a `SpawnInfo` (sube `kVersion`).
- Riesgo: si el Ack se aplica después del primer tick con el gráfico cargado, sigue cerrándose sin R1.

**R3. Mantener** el dueño (`+0x12`) y `+0x1F = 0x9010F001` (modo 2), y la carga 3 s después de los
paquetes del jugador. En la prueba A, enviada en la misma tanda, el usuario copia aún no estaba
"logined" (`0x8ADFE0` exige `u16 [usuario+0x5E] & 4`) y la creación falló en silencio
(**suposición**).

**Descartado como arreglo:** poner `+0x1F` = usuario local para que lo mueva la IA local (modo 3).
Arrancaría la espera, pero ignora la red y mandaría Req con `0xB010F002` al servidor. El cambio de
tipo ya se descartó en §8.5.

### 9.5 Comprobaciones en memoria antes y después de inyectar

Antes (si alguna falla, **no** inyectar el caballo y registrar el motivo):
1. Firma de `0x1F63F70` correcta y R1 instalado.
2. Usuario `0x9010F001` en el gestor (`[[exe+0x6D69188]+0x38]`) con `u16 +0x5E & 4`. Jugador
   `0xA010F001` con `u16 +0x5E == 0x10` y `byte +0xCF == 1`.
3. No existe ya un actor `0xB010F002`.
4. Hay plantilla de acción (R2).

Después, cada frame durante 5 s, con SEH. Con `a` = actor `0xB010F002`, `CC = [[a+0x68]+0x40]`:
- leer `u16 a+0x5E`, `byte a+0xCF`, `u32 a+0x90`, `float a+0x108`;
- para `capa = [CC+0x88]` y `[CC+0x90]`, leer `+0x00`, `+0x08`, `+0x10`, `+0x18` y `+0x110`.

Si R1 salta más de 2 s seguidos, quitar la copia (`RemoveActorAck` de `0xB010F002`) y registrarlo.
Hacer lo mismo con el caballo real `0xB0100003` antes y después de la carga (H2).

### 9.6 Pruebas en vivo (con copia de `save` en `backups/`)

| # | Qué | Resultado esperado |
|---|---|---|
| V1 | R1 + inyección como en la prueba C, con el registro de clave de `0x2329700` | Sin cierre. R1 salta y la clave registrada es `0xB010F002` (H1) o `0xB0100003` (H2) |
| V2 | V1 + R2 | Las alertas de R1 paran al aplicarse el Ack; `[capa0+0x18] != 0`; el caballo copia queda en espera |
| V3 | V2 con el caballo cerca (llamarlo antes de que el emisor capture) | Descarta efectos del planificador lejano |

### 9.7 El error "Actor 생성에 실패했습니다" del jugador copia (pregunta 3)

- Sale de la ruta `0x8BDC4A`: `0x8AF580` (crear actor con dueño) devolvió fallo. Todas las ramas
  llaman al registrador en `0x8BDF77`, de ahí "from `exe+0x8BDF7C`".
- Esa llamada solo pasa 12 argumentos. Los "valores" del log son variables viejas del marco de
  `0x8BD530`:
  - `0x9010F001A010F001` es `[rsp+0x70]`, es decir, clave `0xA010F001` y dueño `0x9010F001` del
    registro que falló;
  - `0x4` y `0x5A900000000` son basura de la pila;
  - **`0x146A44D70` es la dirección de una global de la sección de datos** (`.rdata` con escritura,
    a cero en disco, sin referencias directas). No es un texto ni información de tipo.
- **Qué registro es:** el paquete del cuerpo es `[registro cuerpo tipo 7][copia exacta, byte a byte,
  del paquete de usuario: registro de usuario + registro jugador tipo 1]` (`dupcmp.py`: idénticos en
  la real y en la copia).
  - Con el jugador real, el registro repetido se salta porque el dueño es el usuario local y el actor
    ya existe.
  - Con la copia no hay esa comprobación: `0x8AF580` intenta crear otra vez `0xA010F001` en la ranura
    0 ya ocupada y falla. Libera solo el objeto nuevo y el jugador ya creado no se toca.
- **Es inocuo.** Para quitarlo, mandar el paquete del cuerpo recortado a su primer registro (`2 +
  u16 [0]` bytes, 6.178 B), que equivale a lo que hace el cliente con el jugador real (confianza
  alta). No tiene nada que ver con el cierre del caballo.
