# La copia "camina en el sitio" tras una pausa (menú / inventario) (2026-10-05)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas solo
para esta build. Investigación de solo lectura sobre la prueba de dos PC del 5 de octubre (anfitrión =
PC propio, invitado = Shadow). No se ejecutó el juego ni se tocó código.

Marcas: **[verificado]** leído en el código; **[captura]** medido en las capturas; **[inferido]**
deducción pendiente de prueba en vivo.

Datos:
- Anfitrión: `bin64\cdmp.log` y `cdmp_packets_20261005-204149.bin` (t = 0 ↔ **20:42:02,7** hora del
  anfitrión, calibrado con el campo unix de `ServerTimeInfoAck` y con la primera inyección de la copia).
- Invitado: el `cdmp.log` del invitado (5 oct) y `cdmp_packets_20261005-202422.bin` (t = 0 ↔
  **21:24:45,9** hora del anfitrión; el reloj de Shadow va 3599 s por detrás).
- Todas las horas de este documento son **hora del anfitrión** salvo que se diga otra cosa.
- Scripts: `build/research/pausa/` (`extract.py` saca de un `.bin` los Req propios, los Ack de la copia
  `0xA010F001` y los `ServerTimeInfoAck`; `lag.py` calcula, para cada Ack inyectado, `lag = reloj de
  juego del receptor − ts del paquete`; `timegaps.py` lista las pausas del reloj; `join.py` empareja
  Ack del anfitrión con Req del invitado por posición exacta y da el desplazamiento usado).

---

## 0. Resumen

> **Arreglado y probado (5 oct, 23:12-23:14, un PC con `cdmp_peer`; commits `0bd48b6` y `b22bee2`).**
> - F1 (re-sellado y retención en pausa), F2 (reloj del juego) y F4 (resincronizar tras silencio)
>   están hechos. F3 queda cubierto por F2 en cuanto el reloj del juego es de fiar.
> - **Reloj del cliente:** `exe+0x1416B80` = `[0x6C8CD68]` (ms) + `[0x6C8CD70]` **/ 1000**
>   (microsegundos, no nanosegundos; comprobado en vivo), más `[0x6D69438]`. Lo lee
>   `game_clock.cpp` sin llamar al juego, y solo se usa cuando coincide con nuestras Req (27 ms en
>   la prueba).
> - Resultado: se detectaron 3 pausas (6, 5 y 36 s), con la copia retenida y liberada. No hubo
>   ningún "stuck", y en la prueba la copia se movió enseguida al cerrar el menú.
> - Falta probar el caso de "el emisor en pausa", que necesita dos PC (P1-P6 de §5.3).

1. **El menú para el reloj de juego, no la red.** [captura] Mientras un jugador está en un menú, su
   `ServerTimeInfoAck` (S>C 2188, `u64 ms` en +4, el mismo reloj que firma los Req) sigue llegando cada
   5 s pero **no avanza**, y no sale ningún Req. Al cerrar, los Req siguen desde donde se quedó el
   reloj: el reloj del que pausa pierde exactamente la duración de la pausa frente al del otro PC.
2. **El fallo más largo (25-30 s) es del lado que pausa, como receptor.** [captura] Durante su pausa:
   - el mod sigue estimando su reloj con el reloj de pared (`capture_game_clock_ms`), así que el
     desfase no cambia;
   - el `OnRecvPacket` del juego sigue corriendo (cada `ServerTimeInfoAck`), así que se le **inyectan
     muestras de la copia con `ts` hasta P segundos en el futuro** de su reloj parado, y el búfer de
     interpolación las acepta.

   Al volver, su primer Req corrige la estimación, el desfase baja P de golpe y **todas las muestras
   nuevas tienen `ts` menor que la última aceptada: el cliente las descarta** (regla `ts_nueva ≤
   ts_última`). Las acciones (`StartActionAck`) sí se aplican: **la copia corre en el sitio durante
   unos P segundos**, hasta que las `ts` nuevas superan a las viejas o salta el vigilante de atasco.
3. **El fallo del otro lado (el que ve la copia del que pausó) dura ~1-5 s** [captura] y tiene dos
   piezas:
   - el **primer `Move` sintetizado** del emisor tras la pausa (`capture_synth`) lleva una `ts`
     extrapolada con el reloj de pared, P segundos por delante de su reloj real. Esa única muestra
     **devuelve el `ClockMapper` al desfase viejo**, y los siguientes ~20 eventos reales se inyectan
     con `ts` P segundos en el pasado, hasta que salta el reinicio por "20 muestras tardías";
   - ese reinicio toma **una sola muestra** como desfase nuevo, calculada con el reloj estimado del
     receptor, que puede estar mal: en la MARCA 3, el anfitrión había perdido 0,92 s de reloj de
     juego en un tirón sin mandar ningún Req propio. Resultado: **5 s de muestras 0,6-0,85 s en el
     futuro**, la copia quieta (`T ≤ ts[0]`) y animándose en el sitio. Se arregló sola cuando el
     anfitrión mandó un `StartActionReq` (21:43:04,1).
4. **El arreglo de fondo es no confiar en un reloj de juego extrapolado con el reloj de pared**:
   - en el receptor, leer el reloj real del cliente al inyectar, no inyectar movimiento mientras el
     mundo local está pausado y acotar/re-sellar las `ts` (§4, F1-F2);
   - en el emisor, no sintetizar movimiento con `ts` extrapoladas a través de una pausa (F3);
   - en el `ClockMapper`, resincronizar en el acto tras un silencio (F4).

---

## 1. Hechos de base

### 1.1 El menú para el reloj de juego [captura]

`ServerTimeInfoAck` llega cada 5 s (reloj de pared) en las dos capturas. `timegaps.py` marca los
intervalos en que su contador de ms avanza mucho menos que 5 s:

| PC | t (captura) | Hora (anfitrión) | Avance del reloj de juego | Qué fue |
|---|---|---|---|---|
| Anfitrión | 2904-2964 | 21:30:27-21:31:27 | 0-1,9 s por cada 5 s | pausa ~50 s |
| Anfitrión | 3064-3084 | 21:33:07-21:33:27 | 1,4 / 4,7 / 2,6 s | pausa ~6 s |
| Anfitrión | 3416,9-3449,1 | **21:38:59,6-21:39:31,8** | **0,0 s** (fijo en 3369407) | pausa 32 s |
| Anfitrión | 3649-3654 | 21:42:51-21:42:56 | 4,08 s en 5 s | **tirón: −0,92 s** |
| Invitado | 529-564 | **21:33:35-21:34:10** | 0,0 s (fijo en 507680) | pausa ~26 s |
| Invitado | 919-1067 | **21:40:05-21:42:33** | 0,0 s durante 140 s | pausa ~144 s |
| Invitado | 1069-1093 | **21:42:35-21:42:59** | 0,0 s | pausa ~19 s |

- El contador de `ServerTimeInfoAck` es **el mismo reloj que las `ts` de los Req**: a t = 3409,120 el
  anfitrión recibe `Time 3362494` y a t = 3409,130 envía `MoveActorReq ts=3362494`.
- En la pausa larga del invitado, su último `MoveActorReq` antes de 21:40:05 lleva `ts ≈ 864200` y el
  primer Req tras 21:42:33 lleva `ts = 866048`: **1,8 s de reloj de juego en 150 s de pared**.
- El log lo refleja en el desfase del `ClockMapper`, que salta en ambos PC a la vez y con signo
  contrario:
  - anfitrión: `2541594 → 2686014` (+144,4 s) a las 21:42:35 y `→ 2706274` (+19,7 s) a las 21:43:00;
  - invitado: `−2541569 → −2685966` y `→ −2705544`.

### 1.2 Durante la pausa el cliente sigue recibiendo e inyectando [captura]

- En la pausa del anfitrión (21:38:59-21:39:32), la cola de la copia solo se vacía cuando llega un
  paquete real, es decir, en cada `ServerTimeInfoAck`. Se inyectaron 1 Ack a t = 3434,12, **130 a t =
  3444,12** y 1 a t = 3448,83.
  - Eso coincide con `pendiente-robustez.md` §2.4: en el menú, la cola se entrega a ráfagas.
  - `kStaleMovementMs` (`remote_puppet.cpp:166`, 2 s) tira los que vencieron hace más de 2 s. Los que
    pasan llevan `ts` con el reloj estimado (que sigue corriendo) y, frente al reloj real parado,
    quedan **15-30 s en el futuro**:
    - 21:39:16,8: `ActAck lag −15707`;
    - 21:39:26,8: `MoveAck lag −25082`;
    - 21:39:31,5: `ActAck lag −29588`.
- Esto responde a la pregunta 3 de `pendiente-robustez.md` §6: **sí, el juego se pausa y su reloj se
  para**.

### 1.3 Lo que hace el búfer de la copia con esas `ts` [verificado, `movimiento-remoto-cliente.md` §2]

- **Insertar** (`setInterpolatePositionData`, `exe+0x11B1FD0`):
  - si `ts_nueva ≤ ts_última`, la muestra se **descarta**;
  - si `ts_nueva − ts_última > 1000`, el búfer se reinicia;
  - como máximo guarda 30 muestras.
- **Leer** cada frame con `T = now − 200`:
  - si `T ≤ ts[0]`, **no escribe la posición: el actor se queda donde está**;
  - no hay extrapolación.
- `StartActionAck` escribe en el mismo búfer de posición (con su `ts` en +0x160), pero **la acción
  (la animación de correr o andar) arranca igual**. Con la posición congelada y la acción de
  locomoción puesta, la copia corre en el sitio.

### 1.4 El código implicado [verificado]

- Receptor, `coop.cpp:364-378`. Cada evento con `ts` alimenta el `ClockMapper` con
  `capture_game_clock_ms()`, el reloj **propio** estimado. Después,
  `puppet_event(ev, offset + delay, delay)`.
- `local_capture.cpp:931-935`, `capture_game_clock_ms()`: devuelve
  `g_clock_ts + (steady_ms − g_clock_at)`, es decir, la `ts` del **último Req propio** más el reloj de
  pared transcurrido.
  - Solo se reancla en `note_clock` (`:325`, llamado en `:694`), cuando el jugador local envía un Req.
  - Si el juego local se pausa o pierde tiempo en un tirón sin enviar Req, la estimación se adelanta
    al reloj real.
- `clock_mapper.h:29-50`:
  - toma el mínimo de los últimos 4 s;
  - salta si la diferencia pasa de 250 ms;
  - se reinicia tras 20 muestras tardías seguidas (`kLateRunToReset`). Tras el reinicio, el desfase es
    **la primera muestra** que llega.
- `remote_puppet.cpp:246-252`, `rebase_timestamp`: `ts += g_ts_shift`. Se aplica a Move (+0xD5),
  StartAction (+0x160) y Rotate (+0x9C) (`:266`, `:293`, `:304`).
- `remote_puppet.cpp:1292`: el paquete se encola con `due = now + delay`, **independiente de su `ts`**.
- `remote_puppet.cpp:1377-1403`, `puppet_take_injection`: entrega todo lo vencido y tira el movimiento
  vencido hace más de 2 s. **No mira el reloj de juego ni si el mundo local está pausado.**
- Emisor, `local_capture.cpp:395-426`, `capture_synth`. Si no hay `MoveActorReq` desde hace 150 ms y la
  posición en memoria cambió 5 cm, emite un Move con `ts = g_last_move_ts + (now − g_last_move_ms)`
  (`:415`): **reloj de pared desde el último Move real, que estaba antes de la pausa**.
- `local_capture.cpp:428-448`, `capture_horse_synth`: usa `capture_game_clock_ms()`, con el mismo
  defecto. Afecta al caballo sin jinete.
- `enemy_relay.cpp:959` (`EnemyPos`): usa `capture_game_clock_ms()`, con el mismo defecto (ya anotado en
  `pendiente-robustez.md` §2.10).

---

## 2. Mecanismo A: el receptor estaba en pausa (fallo largo, 25-30 s)

### 2.1 Caso del anfitrión, 21:38:59-21:39:50 (la copia del invitado vista en el anfitrión)

| Hora | Qué pasa | Evidencia |
|---|---|---|
| 21:38:59,6 | El anfitrión entra en un menú: su reloj se para en 3369407 | `Time` fijo de t = 3419 a 3444 |
| 21:39:02-21:39:31 | Llegan eventos del invitado, que se mueve (events in 15240 → 15823). El desfase no cambia, porque la estimación del anfitrión corre con el reloj de pared | log: `clock offset 2571880 → 2571829` |
| 21:39:26,8 | Ráfaga de 130 Ack inyectados con `ts` 25 s por delante del reloj real; el búfer los acepta | `MoveAck ts=3394489 lag −25082` |
| 21:39:31,8 | El anfitrión cierra el menú | `Time` vuelve a avanzar |
| 21:39:32,6 | Primer Req propio: `note_clock` corrige la estimación 30 s hacia atrás | `MoveReq ts=3370733` |
| 21:39:34,3 | Las muestras bajan de golpe; el desfase cae 30 s (21:39:35: `2541625`) | `ActAck ts=3372389 … BACKWARD −27197` |
| 21:39:34-21:39:48 | 274 `MoveAck` con `ts` menor que la última aceptada (≈3399586): **todos descartados**. La copia no se mueve; sus acciones sí se reproducen | `lag.py` |
| 21:39:48,6 | El vigilante de atasco la recarga | `Puppet: the copy is stuck (11.7 m … for 4 s)` |

Sin el vigilante, la copia se habría quedado así hasta que las `ts` nuevas superaran 3399586, es
decir, unos 22 s más. El vigilante solo actúa si la copia se aleja más de 6 m de su objetivo; con
movimientos cortos no salta.

### 2.2 Caso del invitado, 21:33:38-21:34:31 (la copia del anfitrión vista en Shadow; marcas 1 de los dos PC)

| Hora | Qué pasa | Evidencia (captura del invitado) |
|---|---|---|
| 21:33:38-21:34:05 | El invitado está en pausa (reloj fijo en 507680) y recibe la copia del anfitrión con `ts` cada vez más en el futuro | `MoveAck lag −228 … −25178` |
| 21:34:06,8 | Al reanudar, las `ts` bajan 25 s | `RotAck … BACKWARD −25478`, `MoveAck … BACKWARD −25373` |
| 21:34:06-~21:34:31 | Las muestras nuevas tienen `ts` menor que 533679 y se descartan: la copia del anfitrión corre en el sitio | log del invitado, 20:34:08: `clock offset −2575869` (−24 s) |

El anfitrión pulsó su MARCA 1 a las 21:34:26,9, dentro de ese intervalo. El reloj del anfitrión
también se paró un rato (21:33:07-27 y 21:33:57-21:34:07). El fallo largo lo produce la pausa del
**invitado como receptor**, no la del anfitrión.

### 2.3 Por qué ocurre (cadena causal)

1. En pausa, el `now` del cliente se para, pero `capture_game_clock_ms()` no
   (`local_capture.cpp:934`).
2. `on_event` sigue calculando `offset = estimación − ts_remota`, que se mantiene estable
   (`coop.cpp:374`).
3. `puppet_take_injection` entrega en cada paquete real que llega (`troc_sniffer.cpp:221`). Las `ts`
   inyectadas quedan P segundos por delante del `now` parado, y el búfer las acepta porque son
   crecientes.
4. Al reanudar, la estimación se corrige solo cuando sale el primer Req propio
   (`local_capture.cpp:694`). Si el jugador no se mueve, la estimación sigue adelantada y las muestras
   siguen en el futuro: la copia sigue quieta.
5. Tras la corrección, el `ClockMapper` baja el desfase de golpe (`clock_mapper.h:45`, diferencia
   mayor de 250 ms). Todas las muestras nuevas son `≤ ts_última` y se descartan, durante unos P
   segundos.

---

## 3. Mecanismo B: el emisor estaba en pausa (fallo corto; MARCA 3, 21:42:59-21:43:04,5)

### 3.1 Reanudación tras 144 s (21:42:34): muestras en el pasado durante 0,24 s

`join.py` sobre la captura del anfitrión (t = 3631,56-3631,80):

| Orden | Paquete inyectado | Origen en el invitado | Desplazamiento usado |
|---|---|---|---|
| 1 | `ActAck ts=3552336` | `ActionReq ts=866048`, el primer evento tras la pausa | **nuevo**, 2686288. La ventana de 4 s estaba vacía y se toma la muestra |
| 2 | `MoveAck ts=3552379`, pos (−9977.11, **615.29**, −3255.71) | **sin Req que coincida**: es un Move de `capture_synth`. Su `ts` ≈ 864200 + 146 s de pared = 1010407 | **viejo**, 2541972. La muestra sintética, de valor ≈ desfase viejo, es el nuevo mínimo y el mapper **vuelve atrás** |
| 3-20 | `ActAck ts=3408034`, `MoveAck 3408046…3408271` (19 paquetes) | `ActionReq 866062`, `MoveActorReq 866074…866299` | **viejo**: `ts` **144 s en el pasado**, `BACKWARD −144302` |
| 21 en adelante | `MoveAck ts=3552653…` | `MoveActorReq 866311…` | nuevo, 2686342: la 20.ª muestra tardía reinició el mapper |

- Los paquetes 3-20 son exactamente las `kLateRunToReset = 20` muestras tardías del `ClockMapper`.
- El cliente los descarta, porque son `≤ ts_última`, o les aplica un snap (S2/S3 de
  `movimiento-remoto-cliente.md`).
- Aquí el efecto visible es corto (~0,25 s), pero el problema es estructural: **una sola muestra con
  `ts` de reloj de pared deshace la resincronización**.

### 3.2 Reanudación tras 19 s (21:42:59,4): 5 s en el futuro (lo que se vio en la MARCA 3)

| Hora | Qué pasa | Evidencia |
|---|---|---|
| 21:42:51-56 | El anfitrión pierde **0,92 s** de reloj de juego (tirón; quizá la carga de equipo de la copia, `Puppet: equipment change` a las 21:42:51,2) [inferido]. Su último Req propio fue a las 21:42:51,6 (t = 3648,93); desde entonces `capture_game_clock_ms()` va 0,92 s adelantado | `Time 3570005 → 3574080` en 5 s |
| 21:42:59,408 | El invitado reanuda. Primero llega un Move sintético (sin Req que coincida) y después ~10 Move reales con el desfase viejo (19 s en el pasado) | `MoveAck ts=3577505`, después `ts=3558250 lag +18418 BACKWARD` |
| 21:42:59,53 | Reinicio por muestras tardías. La primera muestra tras el reinicio fija el desfase con la estimación inflada del anfitrión: el desplazamiento pasa a 2706574 (y a 2706835 a las 21:43:01,5), cuando lo correcto era ≈2705956 | `join.py`: `shift=2706574` |
| 21:42:59,5-21:43:04,4 | **Todas** las muestras de la copia llegan 0,55-0,85 s en el futuro. A ~65 Hz, 30 muestras cubren ~0,45 s: el búfer nunca tiene una muestra con `ts ≤ T = now − 200`, así que **no escribe la posición**. La copia queda en (−9978,4, −3251,3) mientras el invitado recorre un lazo de ~10 m; las acciones de locomoción se reproducen | `lag −556 … −848` en todos los `MoveAck` |
| 21:43:04,1 | El anfitrión envía un `StartActionReq` (t = 3661,42): `note_clock` reancla su reloj | `ActReq ts=3581376` |
| 21:43:04,5 | El desfase baja 847 ms. Hay 0,85 s más de muestras descartadas (`BACKWARD −831`) y después todo vuelve a la normalidad (`lag +50`) | log 21:43:05,7: `clock offset 2705614` |
| 21:43:08,3 | El anfitrión pulsa la MARCA 3 | |

El desfase entre el reloj estimado y el real del receptor (aquí 0,92 s, por un tirón) está presente
en todo el juego. Aquí se nota porque la resincronización tras la pausa **reconstruye el desfase desde
cero con una muestra**, justo cuando la estimación estaba mal. Sin pausa, el error también entra en
la ventana de 4 s, pero en general se corrige antes, con el siguiente Req propio.

### 3.3 Con caballo

- Al montar, los `Move` del caballo se derivan de los del jinete (`remote_puppet.cpp:1015-1038`) con
  la misma `ts`, así que heredan el mismo fallo.
- Además, `capture_horse_synth` (`local_capture.cpp:433`) firma con `capture_game_clock_ms()`, que se
  extrapola a través de la pausa: tras un menú, los primeros movimientos del caballo sin jinete
  también salen con `ts` adelantada. [inferido; las capturas de hoy no aíslan un caso a caballo]

---

## 4. Arreglo propuesto

Por prioridad. F1 y F2 resuelven el mecanismo A, que es el largo. F3 y F4 resuelven el B.

### F1. Receptor: re-sellar las `ts` al inyectar, con el reloj real del cliente (`troc_sniffer.cpp`, `remote_puppet.cpp`)

1. **Leer el reloj real del cliente dentro de `on_recv_packet`** (`troc_sniffer.cpp:213`). Es el hilo
   NetworkThread del juego, el mismo donde el handler del Ack calcula
   `now = exe+0x1416B80() + [exe+0x6D69438]` (`movimiento-remoto-cliente.md` §2;
   `pieza4-tambaleo.md:109`).
   - Hay que llamarlo igual que el juego, con comprobación de los bytes de cabecera
     (`offsets.h`: `kClientClock`, `kClientClockBias`) y bajo SEH.
   - Antes, confirmar en estático la condición de la bandera TLS `+0x1EC`, para saber si el sesgo se
     suma en ese hilo.
   - Publicarlo en atómicos: `g_game_now_ms` y `g_game_now_steady`, con el `steady_ms` del momento.
2. **Detectar la pausa local:** si entre dos llamadas `Δgame < Δsteady / 2` con `Δsteady ≥ 200 ms`,
   el mundo local está pausado (`g_local_paused = true`). Se apaga cuando el reloj vuelve a avanzar.
3. **`puppet_take_injection` no entrega movimiento** (`Pending::movement`) mientras `g_local_paused`.
   - Al reanudar, `kStaleMovementMs` tira lo viejo, como ya hace (`remote_puppet.cpp:1383`).
   - Las cargas y los estados (`movement=false`) siguen entrando.
4. **Re-sellar y acotar las `ts` justo antes de `guarded_inject`**, con la recomendación 2 de
   `movimiento-remoto-cliente.md` §8.
   - Afecta a `MoveActorAck` (+5+0xD5), `StartActionAck` (+5+0x160) y `RotateActorAck` (+5+0x9C) de
     las claves de copia (jugador, cuerpo, caballo y enemigos `0xB01F…`).
   - `ts = clamp(ts, now_real − 500, now_real + 300)`. En juego normal, los `lag` medidos están entre
     −15 y +250 ms, así que la cota no actúa.
   - Monotonía estricta por actor y por búfer (posición = Move + StartAction; rotación = Rotate +
     StartAction): `ts = max(ts, última_inyectada_del_búfer + 1)`.
   - Con la cota superior, una muestra nunca adelanta más de 300 ms. Si el desfase baja, se descartan
     como mucho ~0,3 s, no 25 s.
5. Efecto sobre los casos medidos:
   - A, anfitrión 21:39 y invitado 21:34: no se inyecta nada en pausa; al volver, nada queda por
     delante del `now` real;
   - B, 21:42:59: las muestras 0,85 s en el futuro quedan en `now + 300`; la copia sigue al otro con
     ~0,5 s de retraso extra en vez de quedarse quieta;
   - B, 21:42:34: las 19 muestras 144 s en el pasado quedan en `now − 500` en vez de descartarse o
     provocar un snap.

### F2. Receptor: el `ClockMapper` debe usar el reloj real (`coop.cpp:365`, `local_capture.cpp:931`)

- `capture_game_clock_ms()` debería devolver el ancla de F1:
  - mientras no haya pausa, `g_game_now_ms + (steady − g_game_now_steady)`;
  - **congelado** mientras `g_local_paused`;
  - reanclado en cada paquete S>C, no solo en los Req propios.
- Como respaldo sin F1:
  - reanclar también con cada `ServerTimeInfoAck` (S>C 2188, `u64` en +4; mismo reloj, verificado en
    §1.1) desde `capture_observe_s2c`;
  - con dos `ServerTimeInfoAck` seguidos que avanzan menos de la mitad del reloj de pared, marcar
    "pausado".
  - Tiene una resolución de 5 s, peor que F1, pero corrige el caso del tirón (0,92 s) a los ≤ 5 s y
    acota el adelanto en pausa a ≤ 5 s.
- Lo mismo beneficia a `enemy_relay.cpp:959` (`EnemyPos`) y a `capture_horse_synth`.

### F3. Emisor: no firmar con reloj de pared a través de una pausa (`local_capture.cpp:415`, `:433`)

- En `capture_synth`, usar `ts = capture_game_clock_ms()` (reloj real con F2). Sin F2:
  - `ts = g_clock_ts + min(now − g_clock_at, 500)`;
  - **no sintetizar** si `now − g_clock_at > 1000` y aún no hay Req propio nuevo. Tras una pausa, el
    primer Req real llega a los pocos ms; no hace falta adelantarse.
- En `capture_horse_synth`, lo mismo. Es el arreglo que ya proponía `pendiente-robustez.md` §2.10:
  "no extrapolar más de 500 ms sin Req propio".
- Así se quita la muestra 2 de la tabla de §3.1, que es la que devuelve el mapper al desfase viejo.

### F4. `ClockMapper`: resincronizar en el acto tras un silencio (`clock_mapper.h:29-50`)

- Guardar la llegada de la última muestra. Si pasó **más de 1000 ms** sin muestras y la nueva difiere
  del desfase en más de `kJumpThreshold` (en cualquier sentido), vaciar la ventana y tomar la nueva,
  sin esperar a las 20 tardías.
- Sin un silencio previo se mantiene el comportamiento actual (tirones cortos).
- Opcional: con un salto de más de 250 ms, ignorar durante ~1 s las muestras que darían un mínimo
  coherente con el desfase anterior a la pausa, como hacía la muestra sintética de §3.1. Con F3 no
  debería hacer falta.

### F5 (opcional). Señal explícita de pausa en el protocolo

- El emisor puede detectar su propia pausa sin RE: un Req propio cuya `ts` avanza mucho menos que el
  reloj de pared desde el anterior (`note_clock`).
- Podría avisar con un evento `ClockJump{pausa_ms}` o un contador de época en cada `GameEvent`; el
  receptor resincronizaría y vaciaría el movimiento encolado.
- **Cambia el formato:** hay que subir `kVersion` (6 → 7, `protocol.h:28`).
- Con F1-F4 no es necesaria. Solo aporta si F1 no se puede hacer.

### Qué no basta

- Bajar `kStaleMovementMs` o el umbral del vigilante (`watch_stuck`, `remote_puppet.cpp:1304`):
  - no evita que se acepten muestras futuras;
  - el vigilante solo arregla los casos de más de 6 m durante 4 s, y con una recarga (fundido).
- Recargar la copia al detectar una bajada del desfase de más de 1 s: funciona como parche, porque
  la carga vacía el búfer, pero cuesta un fundido en cada menú.

---

## 5. Plan de pruebas

### 5.1 Sin juego (`.\scripts\build.ps1`)

1. Tests de `ClockMapper` (`tests/tests.cpp`):
   - **silencio + tardía:** muestras a 25 ms con desfase D, 3 s de silencio y después muestras D +
     19000 → la primera ya da `offset == D + 19000` (F4);
   - **sintética:** D, silencio, D + P (real), D (sintética), D + P … → el desfase no vuelve a D (F4,
     y F3 lo evita en origen);
   - **regresión:** jitter de ±80 ms sin silencio → el desfase sigue el mínimo como hoy.
2. Test del re-sellado de F1 como función pura: `(ts, now, última) → ts'`. Casos:
   - lag normal → sin cambios;
   - +25 s → `now + 300`;
   - −144 s → `now − 500`;
   - monotonía con Move y StartAction intercalados.
3. Prueba completa con `cdmp_peer` + `cdmp_host` (`build\e2e\`): el eco no cambia.

### 5.2 En un PC (eco por loopback, `cdmp_peer`)

- La copia es de uno mismo, así que una pausa local para a la vez al emisor y al receptor. Sirve para
  el mecanismo A y para la regresión.
- Con `offset_m` por defecto (eco a 2,5 m): andar, abrir el inventario 30 s, cerrarlo y **quedarse
  quieto 5 s**; después andar. Repetir con el mapa y con el menú de pausa, y a caballo.
- Criterio: en `cdmp.log`, nada de `the copy is stuck`. Con `lag.py` sobre la captura:
  - **ningún Ack de la copia con `lag < −300`** mientras `Time` está parado;
  - ningún `BACKWARD` de más de 300 ms al reanudar.

### 5.3 Dos PC (Shadow invitado, `docs/PRUEBA-DOS-PC.md`)

Grabar con el sniffer en los dos lados y marcar con Ctrl+F12 al abrir y al cerrar cada menú.

| # | Quién | Acción | Esperado |
|---|---|---|---|
| P1 | I | Andar en círculo; inventario 30 s; cerrar y seguir andando | En A, la copia se para durante el menú y anda en cuanto I anda (≤ 0,5 s). Sin `BACKWARD` largos en la captura de A |
| P2 | A | Lo mismo, visto desde I | Ídem en I |
| P3 | A | Abrir el menú **mientras I corre**; cerrar a los 30 s y no moverse | La copia de I en A llega a su sitio en ≤ 1 s sin que A se mueva (F2). Hoy se quedaba corriendo en el sitio unos 30 s |
| P4 | A e I | Los dos en menús que se solapan, en distinto orden | Sin `stuck` ni copia corriendo en el sitio |
| P5 | I | A caballo: inventario 20 s, cerrar y galopar | Jinete y caballo copia se mueven juntos en ≤ 0,5 s |
| P6 | A | Provocar un tirón (cambiar equipo, abrir el mapa del mundo), sin moverse, mientras I corre | La copia de I no se adelanta ni se para (caso de la MARCA 3) |

Medir en cada caso, con `build/research/pausa/lag.py` y `join.py`:
- el `lag` de los Ack de la copia: objetivo entre −300 y +500 ms siempre;
- el número de `BACKWARD`;
- el tiempo entre el primer Req del emisor tras el menú y el primer `MoveAck` aceptado (monótono).

En el log, revisar las líneas `ts shift` y `clock offset`: tras una pausa deben saltar una sola vez
y sin rebotes.

### 5.4 Pendiente de confirmar en estático antes de F1

- Que `exe+0x1416B80` se puede llamar desde el hook de `OnRecvPacket`. Debe devolver lo mismo que
  usa `setInterpolatePositionData` en ese hilo (camino en caché por frame frente a reloj monotónico;
  bandera TLS `+0x1EC`).
- Si no, buscar la variable del frame que cachea el reloj y leerla con `safe_read`. Como respaldo
  queda F2 con `ServerTimeInfoAck`.
