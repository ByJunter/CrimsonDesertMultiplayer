# Pendiente: robustez, ciclo de vida y preparación para jugar en dos PC

Fecha: 2026-10-04 · Build del juego: 1.0.0.2976 (PE `0x6AB28F00`, base `0x140000000`) · Código
auditado: `ed408ef` (protocolo `kVersion` 5).

Investigación de solo lectura: no se tocó `src/`, el juego ni las partidas. Scripts y salidas en
`build/research/p_robust/` (lista en §9). Etiquetas: **[verificado]** = leído en el código,
**[captura]** = medido en `cdmp_packets_*.bin` o en los `cdmp_*.log`, **[inferido]** = deducción
sin prueba directa.

---

## 0. Resumen

- **El cooperativo de personajes ya aguanta lo básico** en la sesión real `20261003-183530`
  (26 min, Steam): carga de la copia, dos viajes rápidos, una vuelta al menú con relogin y
  recarga de la copia. Lo nuevo de hoy (copias de enemigos, golpes en las dos direcciones)
  **no tiene ciclo de vida**: nada se limpia ni se reenvía tras una reconexión, una pantalla de
  carga del invitado o una recarga de partida.
- **El amigo tiene un kit viejo** (`dist/cdmp-coop-20261003`, `cdmp.asi` de las 23:04 del 10-03).
  Habla el mismo `kVersion` 5, así que **conecta**, pero rechaza en silencio los `GameEvent` 9-12 y
  los `RelData` 5-7. Lo peor: los `RelData` rechazados **nunca se confirman** y el anfitrión los
  reenvía **para siempre** (4 veces/s cada fragmento). Con las copias automáticas encendidas, el
  enlace se va saturando hasta romperse.
- **`kModBuild` vale 1 desde el primer día** y `CDMP_VERSION` es `0.0.1`: ni el log ni el Hello
  distinguen dos builds. Hoy no hay forma de saber por el log qué versión tenía el amigo.
- **Fallo latente del canal fiable:** si hay más de 16 mensajes multifragmento a medias, se
  descarta uno cuyos fragmentos ya se confirmaron. Ese mensaje **se pierde para siempre**, y si es
  de la cola ordenada (equipo, muerte, buffs), la cola se atasca.
- **Varias fugas de estado entre "mundos":**
  - las claves del servidor siguen subiendo tras un relogin, así que las claves fijas de la
    marioneta (`0xB010F001`...) chocan a las ~2 h de sesión;
  - las colas de inyección (S→C y C→S) entregan, al volver de un menú, paquetes de un mundo que
    ya no existe.
- **La configuración del kit no sirve para probar lo nuevo:**
  - `horse=1` y `damage=1`;
  - no existen `guest_hits`, `enemies_target_copy` ni `[dev] enemy_relay`;
  - el `cdmp.ini` del juego del anfitrión tiene `offset_m=10`, que es de pruebas en un PC.
- **Telemetría insuficiente para depurar a distancia:**
  - `cdmp.log` se trunca en cada arranque, así que el log de un cierre se pierde si el amigo vuelve
    a abrir el juego;
  - no se registran los reenvíos del canal fiable, el tamaño de las colas ni la config efectiva.

**Bloqueantes para la próxima prueba en dos PC** (detalle en §6): misma build en los dos PC con un
apretón de manos que lo compruebe, ack de tipos desconocidos, reinicio del relevo de enemigos por
época de sesión, no tirar `EnemySpawn` mientras se carga, orden spawn → estado → retirada,
perfiles de `cdmp.ini` para anfitrión e invitado, log rotado con marcas y estadísticas, y arreglo
del desalojo de parciales.

---

## 1. Estado por módulo: qué guarda y quién lo limpia [verificado]

| Módulo (hilo) | Estado | Se limpia en | No se limpia en |
|---|---|---|---|
| `ClientSession` (red) | nonce, época, canal fiable (`outgoing_`, `incoming_`, `completed_`) | timeout de 5 s, Bye, Hello con otro nonce, Welcome con otro nombre | — |
| `CoopLink` (red) | spawn recibido, último spawn, reloj (`ClockMapper`), secuencia ordenada, pin del mapa | `forget_partner` (enlace perdido, cambio de época o de compañero) | **no** toca copias de enemigos, golpes ni colas de inyección |
| `remote_puppet` | `g_spawned`, caballo, `g_queue` (inyección S→C), `g_last_move`, `g_enemy_pos` | `puppet_remove`/`puppet_forget` purgan solo los paquetes `movement=true` | los paquetes de enemigos (`movement=false`) y los cargados antes de un menú siguen en cola |
| `enemy_relay`, emisor (escritor del sniffer + red) | `g_loads`, `g_actions`, `g_sent` (original→copia), `g_known`, `g_last_hp`, `g_aimed`, `g_refused`, `g_outgoing` (sin límite), `g_out_events` (≤512) | `RemoveActorAck` del propio servidor (por clave); `remove_sent` (Ctrl+Shift+F6/Ctrl+F7) | **reconexión, cambio de compañero, relogin** |
| `enemy_relay`, receptor (red) | `g_copies`, `g_report_due` | `EnemyRemove` | **enlace perdido, pantalla de carga propia, relogin** |
| `local_capture` (escritor) | plantillas (generación), claves propias, `g_enemies`, `g_actors`, `g_has_hp`, reloj de juego | nueva plantilla de usuario/cuerpo (claves); mapas con tope (8192/4096) | `g_enemies` no se vacía al cambiar de mundo |
| `player_hits` | tablas aprendidas, `g_own_hp`, `g_own_hp_max` | — | relogin (HP del personaje anterior) |
| `guest_hits` | tablas aprendidas, `g_cheated`, `g_last_blow` | — | (lo aprendido sirve entre mundos; `g_cheated` caduca a 1,5 s) |
| `troc_sniffer` | `g_c2s_queue` (≤16, se vacía solo dentro de un `PostPacket` real) | — | **menús y cargas**: lo encolado se envía en el mundo siguiente |

---

## 2. Escenarios: qué pasa hoy y cómo hacerlo robusto

### 2.1 Pantallas de carga, viaje rápido y teletransporte

**Lo que se ve en las capturas:**
- Cada carga empieza con `GameLoadingStartReq` (2437, C>S) y termina con `LoadingCompleteReq`
  (2445) + `LoadingCompleteAck` (2762). [captura]
  - El byte de `LoadingCompleteReq` vale `01` en la carga de una partida y `00` en un viaje rápido
    o al reaparecer (183530 en t = 91,8 y 755,9; 080253 en 133,4). [captura]
- El servidor retira **todos** los NPC con `RemoveActorAck` de motivo 19 (183530: 43 claves en
  t = 82, 88 en 659 y 38 en 747). [captura, `life.py`]
- En 183530, H viajó 566 m en t = 82-92 y **la marioneta sobrevivió**: no hubo recarga, y Reqs
  del cliente la nombran en 225 s (`pieza4-capturas.md` §3). G viajó al mismo punto en t = 87
  (salto de 3.473 m de la marioneta). [captura, `jump.py`]

**Qué hace el mod hoy:**

| Quién carga | Marioneta | Copias de enemigos |
|---|---|---|
| Anfitrión | Su copia del invitado: si el juego la tira, la detección de "copia desaparecida" la recarga (8 s de gracia + 3 s) [verificado]. En el invitado, la copia del anfitrión se queda quieta durante la carga; si pasan 8 s sin `PlayerState` se retira, y después se recarga con `last_spawn_` [verificado] | El motivo 19 llega por `enemy_observe_s2c`, que manda un `EnemyRemove` por cada copia: **funciona** [verificado + captura] |
| Invitado | Igual que arriba | **Nadie avisa al anfitrión.** `g_copies` del invitado queda obsoleto y el anfitrión no reenvía nada (`g_sent` dice que ya las mandó). Si el juego del invitado tiró las copias, ya no vuelven; si no las tiró, siguen recibiendo `EnemyPos` [verificado; que el juego las tire, inferido] |
| Cualquiera, durante la carga | — | Un `EnemySpawn` que llega con `local_in_world_ == false` **se confirma y se descarta** (`coop.cpp:384`). Ese enemigo no vuelve a enviarse [verificado] |

- **`local_in_world_` parpadea también fuera de las cargas:**
  - "physics basis not a rotation" sale 117 veces en los logs de `captures/`, con ventanas de unos
    50-120 ms [captura];
  - cada parpadeo puede tragarse un `EnemySpawn`, un `GuestHit`, un `PlayerHit` o un `Damage`.
- **Teletransporte largo de la marioneta:**
  - más de 15 m entre muestras no rellena huecos (`kGapMaxJump`) y salta, que es lo correcto;
  - si el otro queda muy lejos y el juego tira la copia, se recarga cada ~11 s, en bucle
    [inferido: no hay ningún límite de distancia antes de recargar].

**Cómo hacerlo robusto:**
1. **Detectar la carga propia en el sniffer** (C>S 2437 / 2445; el escritor ya ve todos los
   paquetes) y publicar un `world_epoch` atómico que sube en cada `GameLoadingStartReq`. Con él:
   - se purgan de `g_queue` y `g_c2s_queue` los paquetes de épocas anteriores;
   - se borran `g_copies` (inyectando `RemoveActorAck` por si siguen vivas) y se manda al anfitrión
     un `RelKind::EnemyResync` ("perdí tus copias");
   - con `LoadingCompleteReq` y el jugador ya en el mundo, el anfitrión reenvía `EnemySpawn` de
     todo `g_sent` vivo y dentro del área.
2. **Guardar los `EnemySpawn` que llegan fuera del mundo** (como `remote_spawn_`) y aplicarlos al
   volver; si antes llega su `EnemyRemove`, se descartan.
3. **Debounce de `local_in_world_`:** "fuera del mundo" solo tras 1 s seguido sin lectura, o al ver
   2437. Los parpadeos de 50-120 ms no deben cortar nada.
4. **No recargar la marioneta a más de ~150 m** (el pin del mapa ya cubre ese caso) y limitar las
   recargas a una cada 30 s, con un log que diga por qué.

### 2.2 Cambios de región y streaming

- **El anfitrión** retira copias:
  - por `RemoveActorAck` (motivos 19, 25, 28...);
  - en modo automático, por distancia (80 m, 3 s seguidos) o por desaparición (1,5 s).
  [verificado]
- **No hay área de interés del invitado:** las copias se eligen por la distancia al anfitrión
  (40 m), no al invitado. Un invitado a 2 km recibe igual 8 copias, que se cargan lejos de él y
  gastan red. [verificado]
  - Propuesta (ya en `enemigos-del-anfitrion.md` §5): filtrar por la posición de la marioneta del
    invitado en el mundo del anfitrión (`actor_world_position(0xA010F001)`). Cargar a < 60 m,
    retirar a > 80 m.
- **Copias duplicadas:** `enemy_on_spawn` retira una copia con la misma clave antes de volver a
  cargarla (bien). Con el reenvío propuesto en 2.1 hace falta una generación por copia, para
  descartar estados y eventos de la anterior (§2.8).
- **Copias rezagadas:** como `EnemySpawn` y `EnemyRemove` no van ordenados, un `Remove` que adelanta
  a su `Spawn` deja una copia **huérfana para siempre**. [verificado: canal sin orden]

### 2.3 Muerte y reaparición

- **Reaparecer es una pantalla de carga:**
  - `ReviveLoadingReq` (2362) → `GameLoadingStartReq` → `ReviveReq`/`ReviveAck` →
    `NotifyHandleReviveAck` → `LoadingCompleteReq 00`;
  - sin carga nueva del usuario: **la generación de plantillas no cambia y no se reanuncia el
    Spawn**;
  - visto en 080253 (t = 119,7 → 130,6) y 090008 (t = 149 y 204). [captura, `allsess.py`]
- **Lo que se releva:** muerte y reaparición van por la cola fiable ordenada (`kRelayedAcks`: 2530,
  2899, 3053, 2475); en 080253 se ve `NotifyHandleReviveAck` de `0xA010F001`. [captura]
- **Dos agujeros:**
  - **[verificado]** `deliver_ordered` tira el relevo si la marioneta no está cargada. Si el otro
    muere mientras su copia se está recargando, la copia vuelve viva y de pie.
    - Arreglo: guardar el último estado vital (muerto o vivo) y aplicarlo tras cada spawn.
  - **[verificado]** `PlayerHit` contra un jugador muerto no hace nada (`hp - 1000 ≤ 0`): bien. Pero
    `g_own_hp` es del personaje anterior tras un relogin, y se recorta mal hasta el siguiente
    `VaryHPAck`.
- **Muere el anfitrión:** la reaparición con carga retira sus enemigos (motivo 19) y salen los
  `EnemyRemove`. **Muere el invitado:** es el caso 2.1 (sus copias quedan obsoletas).

### 2.4 Salir al menú, recargar la partida o relogin

**Medido en 183530** (relogin de H en t = 659,5):
- `LogoutCharacterToFieldServerAck` → 13,6 s sin ningún paquete S>C → en el menú, un S>C cada
  ~5 s → carga nueva en t = 716. [captura]
- **La cola de inyección siguió entregando en el menú, a ráfagas.**
  - En t = 673, **722 paquetes** de la marioneta, con 14 s de reloj de juego, entraron en una sola
    llamada a `OnRecvPacket`.
  - Hubo más ráfagas en 679, 684 y 690 (`burst.py`).
  - Fueron inofensivos esta vez (no hubo cierre), pero se inyectaron para un actor y un mundo que ya
    no existían. [captura]
- **Las claves cambian:**
  - usuario `0x90100000` → `0x9010173F`, jugador `0xA0100001` → `0xA0101740`;
  - el contador de serie es **común a todas las clases** y **no se reinicia** con el relogin.
  [captura, `life.py`/`serial.py`]
- **Lo que funciona:** la nueva plantilla sube la generación y se reanuncia el Spawn; la copia
  desaparecida se recarga (t = 716,2). [captura]

**Lo que falla o se arriesga:**
- `enemy_relay`, `g_known`, `g_loads`... se limpian solo gracias al `RemoveActorAck` de motivo 19,
  y `g_sent` también. Pero `g_aimed`, `g_refused` y `g_sampled` quedan.
  - Riesgo bajo, porque las claves no se reutilizan dentro del proceso: el contador sigue subiendo.
    [captura]
- **`g_c2s_queue`** (daño `PlayerHit`, `KillCheatReq`/`VaryStatCheatReq` de `GuestHit`) se publica
  en el primer `PostPacket` del mundo nuevo, **con claves del mundo viejo**.
  - Un Cheat/VaryStat contra una clave que no existe es una incógnita. Se sabe que contra un
    "gimmick" sin estadísticas cerró el servidor (`local_capture.h`). [inferido]
- **Claves fijas a `0xA0100001`:** `aggro_test.cpp:101,274` y `mod.cpp:482` (prueba T3) dejan de
  funcionar tras un relogin. [verificado]
- **Choque de claves (R8 de `enemigos-del-anfitrion.md`, confirmado):** el contador llegó a
  `0x32E2` en 1.606 s (8,1/s) en 183530, a 4,2/s en 200328 y a 0,4/s en 131648.
  - A 8/s, la serie `0xF001` llega en **~2,1 h**, y con ella `0xB010F001` (cuerpo), `0xB010F002/3`
    (caballo) o `0xA010F001`.
  - Las copias `0xB01F0000+n` aguantan ~33 h.
  - Arreglo: mover la marioneta a `0xB01FFFF0+`, `0xA01FFFF1` y `0x901FFFF1`. [captura]

**Arreglos propuestos:**
1. `world_epoch` (2.1). Cada paquete en `g_queue` y `g_c2s_queue` guarda su época y su hora; al
   sacarlo se descarta si es de otra época o si un `movement` lleva más de 1,5 s de retraso.
2. Al cambiar la clave propia del jugador:
   - reiniciar `player_hits` (`g_own_hp`) y el `enemy_relay` del emisor (con un `EnemyRemove` de
     todo);
   - mandar `RelKind::Despawn`, que ya existe en el protocolo pero **nadie envía**.
3. Al salir del mundo más de 2 s, mandar `Despawn`: el otro quita la copia enseguida, sin esperar
   los 8 s de silencio.

### 2.5 Desconexión y reconexión

**El nonce de sesión (v5) funciona como se diseñó** [verificado]:
- un Hello repetido con el mismo nonce no reinicia nada;
- un nonce nuevo provoca "reconnected", reinicio del canal fiable y `++epoch`;
- `CoopLink` olvida al compañero y se reanuncia.

**Fallos:**
1. **Las copias de enemigos no se reinician** [verificado]:
   - `forget_partner` no llama a nada de `enemy_relay`;
   - en el anfitrión, `g_sent` sigue lleno y `send_enemy` devuelve en `enemy_relay.cpp:488`, así que
     **el invitado reconectado no vuelve a recibir copias** de los enemigos que ya peleaban;
   - en el invitado, `g_copies` sigue lleno: tras una caída del enlace, las copias quedan
     **congeladas en su mundo** hasta que llegue un `EnemyRemove` que nunca llega.
2. **Mientras está desconectado, el anfitrión no vacía `g_outgoing`**, que no tiene límite.
   - En modo automático se siguen creando spawns y estados.
   - Al reconectar, todo se manda al nuevo compañero de golpe (estados de copias que no tiene).
     [verificado]
3. **Hellos viejos:**
   - Un Hello retrasado con un nonce **anterior** se toma como reinicio y lo resetea todo.
   - Steam entregó 12 Hellos encolados en la primera prueba.
   - Es poco probable tras un reinicio real del juego. Arreglo: recordar los últimos 4 nonces y
     aceptar solo uno no visto antes. [verificado + inferido]
4. **UDP:** el invitado reabre en un puerto efímero nuevo. El anfitrión ignora sus Hello hasta su
   propio timeout (≤ 5 s); es aceptable. [verificado]

**Arreglo:** en `CoopLink`, al cambiar la época:
- invitado: `enemy_reset_receiver()` (`RemoveActorAck` de todas las `g_copies`, que luego se borran);
- anfitrión: `enemy_reset_sender()` (vaciar `g_outgoing` y `g_out_events`, y o bien reenviar
  `EnemySpawn` de todo `g_sent` vivo, o vaciar `g_sent` para que el modo automático vuelva a
  elegir);
- además, vaciar las colas de `player_hits`/`guest_hits`.

### 2.6 Cinemáticas y diálogos

- **No hay detección.**
  - En 49 capturas, `SetAvoidByCutSceneAck` (2561) sale 10 veces y `PrepareSequencerAck`/
    `DeleteSequencerAck` 819/612 veces: también son secuenciadores de ambiente, no sirven de
    marca.
  - `SetQuestDialogAck` sale 8.371 veces. [captura, `cuts.py`]
- **Qué pasa hoy** [verificado salvo lo marcado]:
  - **El emisor en cinemática:** si se mueve sin `MoveActorReq`, `capture_synth` le fabrica Moves
    desde memoria, y su copia hace el recorrido en el otro mundo [inferido].
  - Si el lector de memoria falla durante la cinemática, el otro retira la copia a los 8 s y la
    recarga después.
  - **El receptor en cinemática:**
    - siguen llegando acciones de copias;
    - los `PlayerHit` le quitan vida en plena cinemática;
    - los `GuestHit` dañan a sus enemigos.
- **Propuesta:** buscar una marca de cinemática en memoria (fuera de este tema; pregunta abierta
  §8). Mientras no la haya, ignorar `PlayerHit` cuando el lector de memoria dice que el jugador no
  está controlado.

### 2.7 Montura, nadar, planear y escalar

| Acción | Estado | Riesgo |
|---|---|---|
| Escalar, salientes, planear | Probado en vivo con el amigo (`INFORME-CABALLO-CRASH.md` §1) [verificado en docs] | Deslizamiento leve |
| Caballo | Las copias funcionan desde `6a906a9`: contención en `horse_diag`; al desmontar se recarga el jinete [verificado] | **4 volcados "[Crash] 덤프 보내는 중..." (enviando volcado) en los logs de `captures/`**, de las pruebas del caballo del 10-03 [captura]. El kit lo trae encendido (`horse=1`) y el código lo trae apagado por defecto. Con dos PC el jinete se recarga en cada desmontaje |
| Nadar | **Sin probar en ningún documento** [verificado] | Desconocido: `capture_synth` cubre los Moves que falten, pero no se sabe si el cliente de la copia acepta el método de natación remoto |
| Montura de un enemigo | `sanitize_load` rechaza las copias con padre de tipo 5/7 ("rides a mount", no se copia) [verificado] | Bien |

### 2.8 Muchos enemigos: ancho de banda y backlog

**Medido** (`bw.py`, coste por evento = payload + 19 B de nuestras cabeceras + ~40 B de transporte):

| Sesión | Copias | Mensajes/s entrantes | KB/s |
|---|---|---|---|
| 095647 | 8-10 | 70-89 | 19-25 |
| 081450 (automático) | 6-9 | 52-83 | 16-24 |
| 183530 (real, sin enemigos) | 0 | 56-88 (ráfaga de 148 en el relogin) | 18-22 (44 en la ráfaga) |

- Los logs de esas sesiones dan **~140-155 datagramas/s en cada sentido** en pleno combate
  (`Net: sent`). El RTT del eco sube de ~100 a 120-150 ms con `cdmp_peer --delay 50`. [captura]
- Steam permite 256 KB/s por defecto, así que **el volumen no es el problema**. Lo son:
  - **un datagrama por evento:** `EnemyPos` son 40 B de payload en un datagrama propio, 15 Hz por
    copia. Con 24 copias (el tope del diseño) serían ~360 datagramas/s solo de posiciones. Hay que
    agruparlas: 25 B por muestra, hasta 40 por datagrama (diseño del §6 de
    `enemigos-del-anfitrion.md`);
  - **las ráfagas fiables:** `ReliableChannel::send` manda todos los fragmentos a la vez. Ocho
    `EnemySpawn` (~8 KB cada uno) y un Spawn de jugador (~31 KB) son ~100 datagramas de golpe. Va
    bien a 256 KB/s, pero sin ventana alguna;
  - **el desalojo de parciales** (`reliable.cpp:59,66`) [verificado]:
    - el receptor confirma cada fragmento al recibirlo;
    - si hay 16 parciales y llega el primer fragmento de otro mensaje, borra `incoming_.begin()`,
      que es el id más bajo y no el más viejo;
    - el emisor ya no reenvía los fragmentos confirmados, así que **el mensaje no se completa
      nunca**;
    - es posible con pérdida y ráfagas (8 spawns + Spawn de jugador = 9 multifragmento a la vez,
      más los parciales huérfanos de una época anterior) y catastrófico si cae en la cola ordenada
      (`ordered_held_` se llena y se descartan equipo, muerte y buffs).
    - Arreglo: **ventana de envío** de ≤ 8 mensajes multifragmento sin confirmar por emisor (el resto
      espera en cola), `kMaxPartials` de 64 y desalojo por edad. Test nuevo: 20 mensajes de 8
      fragmentos con un 10 % de pérdida.
  - **sin tope de backlog:** `outgoing_` no tiene límite ni prioridades. Con un compañero que no
    confirma (kit viejo, §2.11), cada fragmento se reenvía 4 veces por segundo para siempre.
    - Arreglo:
      - el receptor confirma y descarta los `RelData` de tipo desconocido;
      - tope por tipo (los `EnemyState` pendientes de una copia se colapsan en el último);
      - el emisor reporta `resent()` y los fragmentos pendientes.
- **Orden de los mensajes de enemigo:** spawn, estado y retirada van por el canal fiable **sin
  orden**.
  - Un `NotifyHandleDeadAck` de un fragmento puede llegar antes que su `EnemySpawn` de 6-8
    fragmentos; entonces `enemy_on_state` lo tira (`!g_copies.count`) y la copia **no muere
    nunca**. [verificado]
  - El diseño ya pedía la secuencia u32 de `Relay`/`Equip` (`enemigos-del-anfitrion.md` §6).
  - Arreglo mínimo: los `RelKind` 5-7 entran en la cola ordenada (`ordered_out_`/`ordered_in_`),
    o llevan una secuencia por copia y el receptor retiene hasta 2 s los estados de copias que aún
    no ha visto.
- **El retardo del estado no sigue la config:** `enemy_on_state` inyecta con 300 ms fijos
  (`enemy_relay.cpp:994`). Con otro `delay_ms`, la muerte se desfasa de las acciones. [verificado]

### 2.9 Pérdida y latencia por Steam: qué debe ser fiable

**Datos de la sesión real** [captura, `pupflow.py`]:
- marioneta en 183530: 49.545 Moves, separación mediana de 16 ms y percentil 99 de 96 ms;
- 145 huecos de más de 0,5 s y 69 de más de 2 s (incluyen paradas reales del jugador y las
  cargas).
- **No hay log del mod de esa sesión** (ni en `captures/` ni en `D:\cdmp-grabaciones`): la pérdida
  y el RTT reales por Steam siguen sin medir.

| Mensaje | Hoy | Coste de perderlo | Coste de duplicarlo | Propuesta |
|---|---|---|---|---|
| `PlayerHit` (11) | no fiable, sin número de secuencia | el invitado recibe menos daño | doble daño | u16 de secuencia + deduplicar; repetir los 2 últimos en el siguiente datagrama de golpes |
| `GuestHit` (12) | no fiable, sin número de secuencia | el enemigo no recibe ese golpe; si era el letal, muere con el siguiente (la vida manda desde el anfitrión, así que es coherente) | **dos `KillCheatReq`** (el `dead` local solo cambia al ver `NotifyHandleDeadAck`) | secuencia + redundancia ×3 + deduplicar; **cerrojo por enemigo** "kill enviado" de 2 s |
| `EnemyAct`/`EnemyPos` | no fiables | un tirón | nada (la interpolación lo absorbe) | se quedan así, agrupados por lotes |
| `EnemySpawn`/`State`/`Remove` | fiables sin orden | — | — | **ordenados** (§2.8) |
| `Damage` (8, espejo) | no fiable | menos daño en el espejo | doble | apagarlo cuando esté `guest_hits` (§3) |

¿Deben ser fiables las muertes? **La muerte la decide siempre el anfitrión** y viaja como
`EnemyState` fiable. Lo que hay que cuidar es el **golpe del invitado**: con redundancia ×3 y un
1 % de pérdida, se pierden 1 de cada 10⁶ golpes, sin la latencia de 250 ms de un reenvío. El canal
fiable entero (ack por mensaje) duplicaría el número de datagramas en combate.

### 2.10 Deriva de reloj

- **`ClockMapper`** [verificado]:
  - toma el mínimo en una ventana de 4 s y se corrige a 1 ms por muestra;
  - salta enseguida si la diferencia pasa de 250 ms;
  - se reinicia tras 20 muestras tardías (el otro reloj se paró).

  Cubre la deriva normal y las pausas, y las pruebas con eco dan desfases estables de 8-80 ms.
  [captura]
- **Dependencias frágiles:**
  - **Solo se alimenta con eventos de la marioneta** (`coop.cpp:337`): sin marioneta no hay
    desfase, y `EnemyAct`/`EnemyPos` se tiran. Tras la recarga de la marioneta del anfitrión
    (tras sus cargas), el desfase de los enemigos se queda viejo hasta que la marioneta reciba 20
    eventos. [verificado]
  - **El reloj de juego del emisor de enemigos se extrapola con `steady_ms` desde el último Req
    propio** (`capture_game_clock_ms`). Si el juego del anfitrión se pausa sin Reqs (menú), los
    `ts` de `EnemyPos` siguen subiendo. Al reanudar, el primer Req real queda **por detrás** y las
    muestras dejan de ser monótonas: tirones en las copias. [inferido; falta una prueba con
    pausa]
- **Arreglo:**
  - alimentar el `ClockMapper` también con `EnemyPos` (lleva `ts`);
  - en el emisor, no extrapolar más de 500 ms sin Req propio (enviar el último `ts` + 1).

### 2.11 Versiones distintas del mod

- **Línea de tiempo:**
  - `kVersion` 5 llegó con `74f299a` (10-03 19:19);
  - después se añadieron, **sin subir la versión**, `RelKind` 5-7 (`d4cbec0`), `EventKind` 9-10
    (`d5380cf`), 11 (`b6caed3`) y 12 (`b9cf037`).
  - `decode` rechaza el datagrama entero con un `kind` desconocido (`protocol.cpp:261,275`).
  [verificado, `git log`]
- **Qué tiene el amigo:** el kit `dist/cdmp-coop-20261003` (asi de las 23:04 del 10-03) no tiene
  ninguna cadena de "Enemy relay". Acepta `GameEvent` 1-8 y `RelData` 1-4. [verificado, `strs.py`]

**Lo que vería cada uno con el anfitrión actual y el invitado con el kit del 10-03:**

| Función | Efecto visible |
|---|---|
| Conexión, personaje, equipo, relevos, minimapa, pin | Funcionan (mismo formato) |
| Daño espejo (`Damage` 8) | Funciona en los dos sentidos si los dos tienen `damage=1` (el kit sí) |
| Copias de enemigos (H con Ctrl+F7) | **G no ve nada.** Cada `RelData` 5-7 se rechaza sin confirmar. H lo reenvía cada 250 ms para siempre, y "reliable pending" crece sin parar en el log de H |
| … con el tiempo | Más reenvíos que capacidad: la cola de Steam y el tiempo de cola suben, la copia del otro se retrasa, y si el RTT pasa de 5 s salta el timeout. La reconexión vacía el backlog y el ciclo empieza otra vez **[inferido]** |
| `EnemyAct`/`EnemyPos` (9-10) | Se rechazan uno a uno; solo sube `rejected` en el log de G |
| `PlayerHit` (11) | **G nunca recibe daño** de los enemigos de H |
| `GuestHit` (12) | G no lo emite: sus golpes a copias (que tampoco ve) no hacen nada |

**Con `kVersion` distinto:**
- `decode` rechaza **todo**, Hello incluido;
- el HUD dice "buscando al otro jugador..." para siempre;
- en el log, solo `rejected N` crece. **No hay ningún mensaje que diga "versión distinta".**
  [verificado]

**Propuesta:**
1. Subir `kVersion` a 6 ya, porque el formato cambió.
2. **El Hello no cambia nunca de formato:** si llega uno de otra versión, se decodifica de todos
   modos y se avisa en el log y el HUD ("el otro jugador tiene la versión X del mod; actualizad los
   dos").
3. `kModBuild` = hash corto de git o fecha de compilación, generado por CMake. Se registra al
   arrancar junto al SHA-256 de `cdmp.asi`.
4. Compatibilidad hacia delante: `decode` acepta cualquier `kind`; las capas de arriba ignoran los
   desconocidos y **confirman** los `RelData` desconocidos.
5. Hello con una máscara de capacidades (copias de enemigos, golpes, caballo...). Cada lado activa
   solo lo que los dos soportan.

### 2.12 El kit del amigo, más viejo que el código

- **`cdmp.ini` del kit** (`kit/coop/cdmp.ini`, igual que el de `dist/`) [verificado]:
  - trae `horse=1` y `damage=1`;
  - no trae `guest_hits`, `guest_hit_stagger`, `enemies_target_copy` ni `[dev] enemy_relay`.
- **Si el amigo copia solo el `cdmp.asi` nuevo** y se queda con su `cdmp.ini`, las funciones nuevas
  quedan **apagadas en silencio** (valores del código) y el daño espejo encendido.
  - El log solo imprime 6 valores de la config (`mod.cpp:649`).
- **`LEEME.txt` del kit** no habla de enemigos, golpes, teclas ni de qué enviar después de una
  prueba.
- **`cdmp_peer.exe` del kit** (10-03 19:18) decodifica y recodifica todo lo que reenvía, así que
  como relé descartaría los `kind` nuevos. Solo afecta al modo UDP con relé. [verificado]
- **Propuesta:**
  - `package_coop.ps1` genera **dos ini** (`cdmp.ini.anfitrion` y `cdmp.ini.invitado`) con
    `profile=<fecha>`;
  - el mod avisa si el perfil del ini es anterior a la build;
  - al arrancar se registra **toda** la config efectiva.

---

## 3. Configuración: valores por defecto antes de enviar

| Clave | Código | Kit (`kit/coop`) | `cdmp.ini` actual del juego de H | Prueba en dos PC, **anfitrión** | Prueba en dos PC, **invitado** | Kit público (cuando esté) |
|---|---|---|---|---|---|---|
| `[net] transport/role` | udp / guest | steam / guest | udp / guest (eco) | steam / **host** | steam / guest | steam |
| `[sniffer] record` | 1 | 0 | 1 (por defecto) | **1** | **1** (solo para la prueba) | 0 |
| `[coop] offset_m` | 0 | 0 | **10** (de pruebas en un PC) | **0** | **0** | 0 |
| `[coop] horse` | 0 | **1** | 0 | 0 en la fase de combate, 1 en la de caballo | igual | 0 hasta probarlo en dos PC |
| `[coop] damage` (espejo) | 1 | 1 | 0 | **0** si `guest_hits=1` (si no, doble daño) | **0** | 0 cuando la pieza 4 funcione |
| `[coop] enemies_target_copy` | 0 | — | 1 | **1** | **0** (sin la supresión 3g, sus enemigos duplicados también pegarían a la copia de H) | 1 solo en el anfitrión |
| `[coop] guest_hits` | 0 | — | 1 | **1** (aplica los golpes) | **1** (necesario para *enviar* `GuestHit`, `guest_hits.cpp:298`) | 1 |
| `[coop] guest_hit_stagger` | 0 | — | 1 | 0 en la primera pasada, 1 en la segunda | indiferente | 0 hasta verlo en dos PC |
| `[dev] enemy_relay` | 0 | — | 1 | **1** + Ctrl+F7 | **0** (con 1 y Ctrl+F7, también copiaría sus enemigos a H) | pasar a `[coop] enemies=auto`, **solo con `role=host`** |
| `[dev] aggro_test`, `piece4_test`, `combat_tests` | 0 | — | 0 | 0 | 0 | 0 |
| `[overlay] self_marker` | 1 | 0 | (1) | 0 | 0 | 0 |

**Reglas que el código debería imponer, no el ini** [propuesta]:
- con `guest_hits=1`, el daño espejo queda apagado (con una línea en el log);
- el relevo de enemigos (emisor) y la aplicación de `GuestHit` solo con `role=host`;
- el envío de `GuestHit` y la recepción de `EnemySpawn` solo con `role=guest`;
- `offset_m≠0` solo con `cdmp_peer`.

**Dependencia de la supresión 3g:** con `offset_m=0`, la copia del enemigo de H
aparece **encima** del enemigo local equivalente de G. El objetivo de un golpe de G puede incluir a
los dos, y el daño espejo los duplicaría. Para la primera prueba de combate, buscar un grupo de
enemigos que solo exista en el mundo de H (ya muerto en el de G), o aceptar el doble enemigo.

---

## 4. Riesgo de cierre: hooks, inyección y carreras

### 4.1 Hooks [verificado]

Todos están al inicio de una función, con comprobación de los bytes del prólogo, y desactivan su
función si la firma no coincide.

| Hook (RVA) | Tipo | Hilo | Lecturas protegidas | Qué hace en el hilo del juego | Riesgo |
|---|---|---|---|---|---|
| `PostPacket` `0x35C670` (sniffer) | mid | cualquiera que envíe C>S | sí (SEH) | copia al anillo (SRW ~1 memcpy); **publica `g_c2s_queue`** con `guarded_post` y **`log::info` por paquete inyectado** | medio: llama a la función del juego con SEH. Tras una excepción desactiva `g_post_fn`, pero el estado del juego puede quedar a medias |
| `OnRecvPacket` `0x35CA00` (sniffer) | mid | NetworkThread del cliente | sí | vacía **toda** la cola vencida en un solo paso (ráfagas de cientos tras un menú, §2.4); `log::info` por carga y retirada | medio: ráfagas viejas; un AV dentro del handler se captura, pero /EHsc no deshace los locks del juego, así que **cuelgue posible en vez de cierre** [inferido] |
| Escritor de `OtherEquipItemAck` `0x29ED1F0` (el de docking está desactivado) | mid | hilo del servidor | sí | `TryAcquire` + giro ≤1000, sin reservas de memoria | bajo |
| Logger de errores `0x35D8D0` | mid | ~2.900 llamadores | sí | `TryAcquire`, anillo de 32 | bajo |
| `PointClimb::Enter` `0x691400`, `RideOn::Enter` `0x681C80` | inline | hilo de juego | sí | fuerza `1` solo para la marioneta | bajo (probado en vivo) |
| Capas de acción `0x1F63F70` / `0x2329700` | mid → `ret` | hilos de animación | sí | se salta la actualización de copias sin acción | bajo-medio: depende de un `ret` en `VirtualAlloc` |
| `horse_diag` (`0x85FBA0`, `0x860D30`, `0x8646D0`, `0x2329D70`) | mid | **cada frame por personaje** | sí | lee la clave; para copias, **`log::info` en hilos del juego** (~1,4 líneas/s con copias, 410-420 líneas por sesión en `cdmp_enemy_auto.log` y `cdmp_sanitized_test.log`) | medio: tirones (ver §4.3) |
| `IsTargetable` (`0x22141E0`) | inline | IA | sí | contadores atómicos | bajo |
| `SetCurrentTarget`, minimapa (`0x9E89C0`) | mid / inline | IA / UI | sí | — | bajo |
| Overlay (Present ×2, ResizeBuffers ×2, ECL) | inline diferido | hilo de present | — | probado con DLSS y FG | bajo |

### 4.2 Datos de red que llegan al juego sin validar [verificado]

`enemy_on_state` (VaryHP 92 B, NotifyHandleDead 19 B) y `enemy_on_event` (`AttackTargetAck`
199/211 B, `ApplyActionRestrictionAck` 77 B) inyectan el payload recibido **sin comprobar su
tamaño**. Un compañero con otro formato (otra versión) o un mensaje corrupto mete bytes arbitrarios
en los handlers del juego.
- Arreglo: lista blanca de opcode + tamaño exacto (o rango), como ya hace `kRelayedAcks`
  (`rule->size`).

### 4.3 Carreras y bloqueos [verificado salvo lo marcado]

- **Log en hilos del juego:**
  - `log::write` toma un `std::mutex` y hace `FlushFileBuffers` **en cada línea**;
  - lo llaman los hooks del sniffer (inyecciones) y `horse_diag` (frames);
  - un flush lento (carpeta sincronizada en la nube, disco lleno) bloquea el NetworkThread o el hilo de animación mientras
    otro hilo tiene el mutex [inferido: no medido].
  - Arreglo: cola sin locks para los hilos del juego y flush de un hilo propio cada 250 ms (o
    inmediato con WRN/ERR).
- **`g_enemy_pos`, `g_ref_pos`, `g_ts_shift`, `g_last_move`** (`remote_puppet`) no tienen lock. Hoy
  solo los toca el hilo de red en cooperativo; `puppet_remove_actor` se documenta como "cualquier
  hilo" y borra en `g_enemy_pos`. Mientras solo lo llame la red, no hay carrera.
  - Arreglo: documentarlo como "solo hilo de red", con un assert de hilo en debug.
- **Sink de `CoopLink`:** `emit()` copia `g_sink` y lo llama fuera del lock. Un `capture_set_sink({})`
  en el destructor puede dejar al hilo escritor usando un `CoopLink` destruido.
  - Hoy solo pasaría al parar el mod, y en el juego nunca se para (`DllMain` no llama a `stop`).
    Riesgo nulo en la práctica.
- **`find_actor`** lee la tabla hash del juego sin sincronización. `safe_read` evita AV, pero
  puede devolver un actor equivocado durante un rehash [inferido]. Hay que usarlo como pista, nunca
  para decidir algo destructivo.
- **`g_c2s_queue` (≤16):** se publica en el siguiente `PostPacket` real. En menús no hay
  `PostPacket`, se llena y los golpes se pierden ("NOT sent"); lo encolado sale en el mundo
  siguiente (§2.4).

### 4.4 Colas sin límite [verificado]

| Cola | Límite | Riesgo |
|---|---|---|
| `g_queue` (inyección S→C) | ninguno | crece en menús y pausas; ráfaga al volver |
| `enemy_relay::g_outgoing` | ninguno | crece desconectado |
| `ReliableChannel::outgoing_` | ninguno | crece sin fin con un compañero que no confirma |
| `CoopLink::outbox_` | 4.096 | descarta y lo cuenta (bien) |
| `g_out_events` / `hits` / `guest_hits` | 512 / 64 / 64 | descartan en silencio |

---

## 5. Telemetría para depurar a distancia

**Hoy:**
- cada 5 s se registran:
  - la línea `Net:` (enviados, recibidos, rechazados, RTT y estado de Steam: ping, KB/s, pendientes
    y cola);
  - la línea `co-op:` (marioneta, eventos, desfase de reloj, pendientes fiables, outbox
    descartado);
- los errores internos del juego, deduplicados. [verificado]

**Falta** (por orden de utilidad):
1. **Log rotado:** `cdmp.log` → `cdmp.prev.log` al arrancar, o un fichero con fecha (`log.cpp:30`
   usa `CREATE_ALWAYS`). Sin esto, el log de un cierre se pierde si el amigo vuelve a abrir el
   juego.
2. **Cabecera de identidad:**
   - hash de git, fecha de compilación y SHA-256 de `cdmp.asi`;
   - `kVersion`, config efectiva completa y SteamID propio;
   - hora UTC con fecha. El log solo tiene hora local sin fecha.
3. **Tecla de marca (Ctrl+F12):**
   - escribe `MARK n` en el log, la envía al otro (que también la registra) y aparece 3 s en el
     HUD;
   - sirve para alinear los dos logs y las capturas paso a paso del protocolo (§7).
4. **Estadísticas cada 5 s:**
   - `resent` del canal fiable y fragmentos pendientes **por tipo**;
   - `ordered_held_`;
   - longitud y retraso del más viejo en `g_queue` y `g_c2s_queue`, más los paquetes descartados
     por época;
   - eventos recibidos y enviados por `kind`;
   - rechazados **por motivo** (versión, `kind`, mal formado);
   - `g_copies` y `g_sent`, transiciones de `local_in_world_` y época de mundo.
5. **Transiciones de ciclo de vida con motivo:**
   - "carga propia (2437)", "carga terminada (2445, byte)";
   - "relogin: clave 0x… → 0x…";
   - "época de sesión N", "Despawn enviado/recibido".
6. **Volcado propio en un cierre:** no tocar el filtro de excepciones del juego. Basta con
   asegurar el flush periódico; el `crashpad` del juego ya hace el volcado.
   - Pedir al amigo la hora exacta y, si sale el diálogo de error, una captura de pantalla.
7. **HUD:** líneas de aviso (versión distinta, backlog fiable > 200 fragmentos, carga
   detectada, copias de enemigos N).

---

## 6. Lista priorizada

### P0: bloqueantes antes de la próxima prueba en dos PC

| # | Qué | Dónde | Esfuerzo |
|---|---|---|---|
| 1 | **Misma build en los dos PC, comprobada por el apretón de manos:** `kVersion` 6, `kModBuild` = hash de git, Hello decodificable entre versiones, aviso en el log y el HUD | `protocol.*`, `client_session.cpp`, `mod.cpp`, `CMakeLists.txt` | S |
| 2 | **Confirmar y descartar los `RelData`/`GameEvent` de tipo desconocido**; tope y prioridad en `outgoing_` | `protocol.cpp:261,275`, `reliable.cpp` | S |
| 3 | **Reiniciar el relevo de enemigos** al cambiar la época o el compañero (emisor: vaciar o reenviar `g_sent`/`g_outgoing`; receptor: retirar `g_copies`) | `coop.cpp:290`, `enemy_relay` | S |
| 4 | **No tirar `EnemySpawn` fuera del mundo** (retenerlo) y **debounce de `local_in_world_`** | `coop.cpp:384`, `coop.cpp` (update) | S |
| 5 | **Orden spawn → estado → muerte → retirada** (cola ordenada o secuencia por copia) | `coop.cpp` `on_reliable`, `enemy_relay` | M |
| 6 | **Validar el tamaño** de cada op S→C de enemigo antes de inyectarlo | `enemy_relay.cpp:934,994` | S |
| 7 | **Ventana de envío y desalojo de parciales por edad** (+ test con 20 mensajes y 10 % de pérdida) | `reliable.*`, `tests.cpp` | S |
| 8 | **Perfiles de ini** de anfitrión e invitado (§3), config efectiva completa en el log, `LEEME` nuevo con el protocolo de prueba; el kit lleva su SHA-256 | `kit/coop/`, `package_coop.ps1`, `config.cpp` | S |
| 9 | **Log rotado + tecla de marca + estadísticas** de §5.1-5.4 | `log.cpp`, `mod.cpp`, `coop.cpp` | S |
| 10 | Antes de la prueba, en un PC: `cdmp_peer --loss 5 --jitter 40 --delay 60` con Ctrl+F7, una carga, viaje rápido, morir y reconectar (matar `cdmp_peer` 10 s) | — | — |

### P1: justo después

- `world_epoch` desde el sniffer (2437/2445):
  - purga de `g_queue`/`g_c2s_queue` por época y por antigüedad;
  - `RelKind::EnemyResync` cuando el invitado carga.
- Relogin: reiniciar `player_hits` y el relevo del emisor; enviar `Despawn` al salir del mundo más
  de 2 s; quitar las claves fijas `0xA0100001` (`aggro_test.cpp:101,274`, `mod.cpp:482`).
- `GuestHit`/`PlayerHit` con secuencia, redundancia ×3, deduplicación y cerrojo de
  `KillCheatReq` por enemigo.
- Mover las claves de la marioneta al rango alto (choque a las ~2 h, §2.4).
- Área de interés por la posición del invitado; agrupar `EnemyPos`; roles (relevo solo en el
  anfitrión).
- Logger asíncrono, sin flush en los hilos del juego; `horse_diag` sin log por frame salvo en modo
  diagnóstico.
- `ClockMapper` alimentado también con `EnemyPos`; no extrapolar el reloj más de 500 ms.
- Retardo del estado de enemigo = `delay_ms` (no 300 fijos).

### P2

- Detección de cinemáticas y diálogos (§2.6); recordar los últimos nonces de Hello.
- Recargas de la marioneta con límite de distancia y de frecuencia.
- Prueba de nado; máscara de capacidades en el Hello.

---

## 7. Protocolo de la próxima prueba en dos PC

**Roles:** H = el anfitrión (`role=host`); G = el invitado. Duración: 60-75 min.
Se habla por voz y **antes de cada paso, H pulsa la marca** (Ctrl+F12, P0-9) y lo anuncia ("marca
7: G viaja"). Si la marca no está hecha, los dos anotan la hora del reloj de Windows con los
segundos.

### 7.0 Preparación (los dos)
1. **Copia de seguridad** de `%LOCALAPPDATA%\Pearl Abyss\CD\save` a otra carpeta.
2. **Mismo kit:** H manda el zip; G comprueba el `SHA256.txt` y reemplaza **los tres ficheros**
   (`winmm.dll`, `cdmp.asi`, `cdmp.ini.invitado` → `cdmp.ini`).
   - En el log de los dos debe salir la **misma** línea `=== … build <hash> ===`.
3. Cada uno pone su `player_name`. G deja `peer_steamid` vacío si H es el único amigo jugando.
4. `[sniffer] record=1` en los dos (unos 100 MB por sesión, ~8 MB comprimidos: medido 12,4×).
5. Cerrad OBS, grabadores o programas que pesen; si se puede, que G grabe vídeo.
6. **Nadie guarda la partida** durante la prueba.

### 7.1 Fase A: conexión y lo básico (15 min, `horse=0`, sin enemigos)
| Paso | Quién | Qué | Qué mirar |
|---|---|---|---|
| A1 | H y luego G | Abrir el juego y cargar la partida | HUD "conectado a …"; en el log, `Session: connected` y **ningún aviso de versión** |
| A2 | Los dos | Viaje rápido al mismo punto | La copia del otro aparece en < 5 s; el pin del mapa desaparece al cargarse la copia |
| A3 | Los dos | Andar, correr, saltar, escalar, planear 2 min | Teletransportes, deslizamientos |
| A4 | Los dos | **Nadar** 1 min (primera vez) | ¿La copia nada, flota o desaparece? |
| A5 | G | Cambiarse una pieza de equipo y sacar el arma | La copia en H lo refleja |

### 7.2 Fase B: ciclo de vida (20 min)
| Paso | Quién | Qué | Qué mirar |
|---|---|---|---|
| B1 | H | Viaje rápido lejos (> 1 km) y vuelta | En G, la copia de H desaparece o se recoloca y vuelve; nada de bucles de recarga |
| B2 | G | Lo mismo | Ídem en H |
| B3 | G | Abrir el mapa o el inventario 30 s (pausa) y cerrar | ¿Se pausa el juego? ¿Ráfaga de movimiento en H? |
| B4 | G | Morir (caída alta) y reaparecer | En H, la copia muere, reaparece y salta al punto de reaparición |
| B5 | H | Morir y reaparecer | Ídem en G |
| B6 | G | Salir al menú principal, esperar 20 s y recargar | En H, la copia se va a los 8 s (o con `Despawn`) y vuelve tras la recarga |
| B7 | G | **Cerrar el juego del todo** y volver a entrar | "reconnected" en H, sin "peer reconnected" repetidos |
| B8 | H | Salir al menú y recargar | Ídem desde el otro lado |

### 7.3 Fase C: combate (20 min; H con `[dev] enemy_relay=1`, `enemies_target_copy=1`, `guest_hits=1`, `damage=0` en los dos)
| Paso | Quién | Qué | Qué mirar |
|---|---|---|---|
| C1 | Los dos | Ir juntos a un grupo pequeño (2-4 enemigos) | — |
| C2 | H | **Ctrl+F7** (copias automáticas) y provocar a los enemigos | G ve las copias moverse y atacar (log de H: `Enemy relay: sending`; de G: `copy … exists`) |
| C3 | G | Pegar a una copia hasta matarla | En H el enemigo real pierde vida y muere; en G la copia muere; botín en H |
| C4 | H | Dejar que un enemigo pegue a la copia de G | G pierde vida (`the other player's enemy hit us`) |
| C5 | G | **Viaje rápido en pleno combate**, y volver | Copias congeladas o huérfanas en G (fallo conocido si P1 no está) |
| C6 | H | Viaje rápido en pleno combate | En G las copias deben irse (`EnemyRemove`) |
| C7 | (opcional) H | `guest_hit_stagger=1` (reiniciando el juego) y repetir C3 | ¿Se tambalea el enemigo en H? |

### 7.4 Fase D: carga (10 min)
| Paso | Quién | Qué | Qué mirar |
|---|---|---|---|
| D1 | Los dos | Campamento grande (≥ 8 enemigos), 2 min de pelea | En el log de H, "reliable pending", `resent`, `queue … ms` de Steam y RTT |
| D2 | Los dos | Ciudad con mucha gente, 2 min | `rejected`, outbox descartado, RTT |
| D3 | (opcional) H | Caballo (`horse=1`, reiniciando): montar, cabalgar y desmontar | Cierres: si sale "[Crash]" en el log, se acaba la fase |

### 7.5 Qué devuelve cada uno (un zip por jugador)
- `bin64\cdmp.log` **y** `cdmp.prev.log` (si hubo reinicios, todos los rotados);
- `bin64\cdmp_packets_<fecha>.bin` de la sesión (no hace falta el `.log` de paquetes, que se puede
  regenerar);
- su `cdmp.ini` y el `SHA256.txt` que usó;
- la lista de marcas con lo que vio en cada una (una línea por marca: "M7: la copia de H se quedó
  quieta 5 s");
- vídeo, si lo hay, y capturas del HUD en los momentos raros;
- si el juego se cerró: la hora exacta y la carpeta de volcados de crashpad del juego, si existe
  (ruta por confirmar).

### 7.6 Criterios de éxito para dar por buena la sesión
- **Sin cierres ni cuelgues.** Si hay cierre, es lo primero que se arregla.
- A1-A5 y B1-B8 sin pasos que necesiten reiniciar el juego.
- En el log:
  - la línea de `resent` no crece sin parar;
  - `g_queue` no pasa de unos cientos;
  - `rejected` = 0;
  - ningún "malformed" o "refused".
- Fase C: al menos una muerte de un enemigo real causada por G, y daño en G causado por un
  enemigo de H.

---

## 8. Preguntas abiertas

1. ¿El juego del **invitado** tira las copias de enemigos (actores creados por red) en su propia
   pantalla de carga, como hace a veces con la marioneta? En 183530 la marioneta **sobrevivió** a
   un viaje rápido y **no** sobrevivió a un relogin.
   - Se decide con la prueba C5 o en un PC: una copia por eco + viaje rápido + `report()` de la
     copia.
2. ¿Qué hace el servidor con `VaryStatCheatReq`/`KillCheatReq`/`VaryStatByAccInputReq` sobre una
   clave **que no existe**? (Lo encolado antes de un menú sale en el mundo nuevo.) Se puede probar
   en un PC con una clave inventada; mejor evitarlo con la época de mundo.
3. ¿El juego se pausa en el mapa o el inventario, y se para entonces su reloj de juego (los `ts`
   de los Req)? Afecta a la extrapolación del reloj (§2.10).
4. ¿Hay una marca de memoria fiable para "cinemática" o "diálogo"?
5. ¿El cliente acepta la natación de una copia remota?
6. Pérdida y RTT reales por Steam: no hay log del mod de la sesión 183530. La próxima prueba lo
   medirá.

---

## 9. Scripts (`build/research/p_robust/`)

| Script | Qué hace | Resultado usado |
|---|---|---|
| `life.py <sid>` | Cargas (2437/2445/2762), reapariciones, logout, recargas y retiradas inyectadas de la marioneta y las copias, cambios de clave del usuario, huecos S>C | §2.1, §2.4 |
| `pupflow.py <sid>` | Acks inyectados a la marioneta por cada 30 s, separación entre Moves, muertes del jugador | §2.9 |
| `burst.py <sid> a b` | Paquetes por segundo en un intervalo y el lapso de reloj de juego de los Moves de la marioneta (ráfagas viejas) | §2.4 |
| `jump.py <sid> keys` | Saltos de más de 30 m del jugador (Req propios) y de la marioneta | §2.1 |
| `bw.py <sid>` | Mensajes/s y KB/s entrantes estimados a partir de lo inyectado (marioneta + copias) | §2.8 |
| `allsess.py` | Mensajes de ciclo de vida en las 49 capturas | §2.3 |
| `cuts.py` | Mensajes de cinemática, diálogo y montura en las 49 capturas | §2.6 |
| `serial.py <sid…>` | Ritmo del contador de claves del servidor | §2.4 (choque de claves) |
| `strs.py <exe> <regex>` | Cadenas de un binario (kit del amigo) | §2.11 |

Todos usan `build/research/pieza4/pk.py` (índice en caché de los `.bin`) y el venv de
`build/research/venv`.
