# Pieza 3: el invitado ve los enemigos del anfitrión

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`)
y solo valen para esta build. Investigación de solo lectura: capstone sobre el exe en disco, las
capturas de `D:\cdmp-grabaciones` (sobre todo `cdmp_packets_20261003-112627.bin`, 856 s y unos 10
combates) y el código actual del mod. El juego no se ejecutó.

Marcas:
- **[verificado]**: leído en el desensamblado, o comprobado en vivo en una prueba anterior (se cita);
- **[captura]**: medido en las capturas;
- **[inferido]**: deducción que hay que confirmar en vivo (la §8 dice con qué prueba).

Scripts en `build/research/enemigos/` (lista en la §10). Se ejecutan con
`build/research/venv/Scripts/python.exe <script> <captura.bin> …`.

---

## 0. Resumen

1. **Los enemigos son registros de `ActorLoadDataAck` de un solo registro**, tipo 3 (personas),
   6 (animales), 5 (monturas) o 4 (personajes con nombre). Pesan de 6,6 a 11 KB (mediana 7,7 KB).
   - Dueño `rec+0x05` = 0 y usuario de control `rec+0x12` = usuario local `0x90100000` en los 1.235
     registros [captura].
   - Para la copia hay que reescribir:
     - la clave (a un rango propio, `0xB01F0000 + net_id`);
     - `rec+0x12`, con un usuario ajeno, para que el modo de control sea 2;
     - las 3 copias de la posición, la rotación y las claves de jugador del anfitrión que aparezcan.
2. **Sí, la copia de un enemigo tendrá el mismo problema de "capa sin acción" que el caballo.**
   - En el anfitrión, todo NPC empieza su primera acción a los **0,26 s** de cargarse (mediana; 1.232
     de 1.235) [captura]. La copia, en modo 2, no la empezará nunca sola.
   - **El "byte de restauración" no sale del paquete en la práctica.** El deserializador del CC lo
     lee del registro, pero en todos los registros capturados la zona vale 0. En el caballo copia pasó
     de 0 a 1 justo cuando se aplicó su primer `StartActionAck` (log en vivo).
   - Primera acción recomendada: **el último `StartActionReq` de ese enemigo en el anfitrión**,
     convertido en Ack, inyectado justo detrás de la carga (igual que el caballo).
   - La contención de `horse_diag` se generaliza con un test de rango de clave.
3. **Los NPC no emiten posiciones a ritmo continuo.**
   - `MoveActorReq` llega cada 1,5 s de mediana en pleno combate (p90 3,5 s), y 444 de 1.235 NPC no
     mandan ninguno [captura].
   - Una copia en modo 2 solo interpola muestras: con eso daría saltos y reinicios del búfer (>1 s).
   - **Las posiciones hay que leerlas de la memoria del anfitrión** (como ya se hace con el caballo),
     a 15 Hz, y mandarlas en lotes compactos.
4. **Coste por enemigo en combate** (bytes del juego, sin comprimir) [captura]:
   - `StartActionReq` 1,39/s (1,0 KB/s);
   - `ChangeAIStateReq` 2,16/s (1,46 KB/s);
   - HP, buffs, objetivo y efectos, unos 0,2 KB/s.

   Con 7 enemigos a la vez (el máximo medido) son 11 KB/s sin IA y 22 KB/s con IA. Con compresión por
   mensaje bajaría a ~7 KB/s. Cada carga son unos 7,7 KB por canal fiable.
5. **Supresión en el invitado: no bloquear las Req de spawn.**
   - Esas Req también crean objetos (tipo 7) y estado de grupos de spawn que el servidor guarda.
   - Mejor **descartar en el cliente las cargas de NPC hostiles** (por facción, `rec+0x1A`) dentro de
     la "burbuja" del anfitrión, y retirar con un `RemoveActorAck` inyectado las que ya estén.
   - Solo se puede probar con dos PC; en el eco de un PC no se suprime nada.
6. **Protocolo** (`kVersion` 5 → 6):
   - `net_id` (u16) asignado por el anfitrión y `gen` (u16) por reutilización;
   - un mensaje fiable de aparición con "paquete de inyección" (carga + primera acción + HP);
   - un flujo fiable ordenado para estado (HP, buffs, objetivo, muerte, retirada);
   - eventos no fiables para acciones y lotes de posiciones (25 B por enemigo y muestra).
   - **El anfitrión traduce todas las claves a claves del invitado antes de enviar.**
7. **Prueba en un PC:** con `cdmp_peer --mode relay` los enemigos del propio PC vuelven como copias
   desplazadas 4 m. El riesgo principal es la realimentación: el sniffer ve las cargas inyectadas de
   las copias y no debe tomarlas por enemigos propios (§7).

---

## 1. La carga (`ActorLoadDataAck` 2005)

### 1.1 Qué registros son enemigos

`ActorLoadDataAck` = lista `[u16 len][registro]…`. En 112627 [captura, `a1.py`]:

| Firma de registros | Paquetes | Tamaño del registro (mín / mediana / p90 / máx) | Dueño `rec+0x05` |
|---|---|---|---|
| `(7,)` gimmick/objeto | 2.427 | 6.075 / 6.262 / 6.537 / 11.769 | 0 |
| **`(3,)` NPC persona** | **648** | **6.614 / 7.709 / 8.377 / 10.961** | **0** |
| **`(6,)` animal** | **443** | 6.605 / 6.782 / 6.852 / 9.533 | 0 (1 del usuario: la mascota) |
| `(5,)` montura | 82 | 6.786 / 8.235 / 8.800 / 9.037 | 0 (1 del usuario: tu caballo) |
| `(4,)` personaje con nombre | 27 | 6.796 / 8.061 / 8.569 / 11.902 | 0 (y 33 de usuario) |
| `(0,4)`, `(0,1)`, `(0,5)`, `(0,6)` | 37 | — | usuario (preámbulos de mercenarios y del jugador) |

- **Todas las cargas de NPC del mundo son de un solo registro.**
- Los 19 actores que pelearon con el jugador en esa captura (le pegaron o recibieron golpes) eran
  14 "Bleed Bandit" de tipo 3, 3 ciervas de tipo 3 y 2 personajes con nombre de tipo 4 con dueño 0
  ("Anders", "Arnold") [captura, `a4.py`].
- Los insectos y pájaros también fijan al jugador como objetivo (`CurrentTargetChangeReq`, para huir):
  **"apunta al jugador" no basta para decir "enemigo"** (272 actores lo hacen) [captura, `a3.py`].

Cabecera del registro (offsets dentro del registro; en el payload súmale 2) [captura, `a2.py`;
coincide con `local_capture.cpp`]:

| Offset | Campo | Valor en NPC del mundo |
|---|---|---|
| `+0x00` | u8 tipo de registro | 3 / 4 / 5 / 6 |
| `+0x01` | u32 clave | `0xB01xxxxx` |
| `+0x05` | u32 dueño (usuario del contenedor de hijos) | 0 |
| `+0x09` | u8 índice en el contenedor de hijos | `0xFF` |
| `+0x0A` | 8 B | `FF…FF` |
| **`+0x12`** | **u32 usuario de control = `actor+0x90`** (decide el modo) | **`0x90100000` en los 1.235** |
| `+0x16` | u32 variante del personaje | p. ej. 54109 (Bleed Bandit) |
| **`+0x1A`** | **u32 facción o grupo** [inferido por la tabla de abajo] | ver tabla |
| `+0x1E` | u32 n + UTF-16 nombre | "Bleed Bandit" |

`rec+0x1A` separa bien a los hostiles [captura, `c3.py`]:

| `rec+0x1A` | Nombres | Hostiles / total |
|---|---|---|
| `0x000F4250` | Bleed Bandit | 14 / 87 |
| `0x000F42C0` | Bandit | 0 / 14 (no hubo pelea) |
| `0x000F4246` | Shakatu's Soldier, Ugmon | 0 / 18 |
| `0x000F42C3` | Hernandian Guard, Pedro | 0 / 158 |
| `0x000F4240` | civiles con nombre, perro | 2 / 67 |
| `0` | animales, civiles | 3 / 808 |

### 1.2 Qué hay que reescribir para la copia

| Qué | Dónde | Valor en la copia | Por qué |
|---|---|---|---|
| Clave | `rec+0x01` y **cualquier otra aparición** (`replace_u32`) | `0xB01F0000 + net_id` | Rango libre: el contador del servidor llegó a `0x32E2` en 27 min (`b5.py`) y crece ~8/s, así que necesitaría unas 34 h para llegar a `0xF0000` [captura]. **Nunca** la dirección de memoria (regla del proyecto, CONTRIBUTING.md) |
| Usuario de control | `rec+0x12` | ajeno: **`0x9010F001`** (el usuario de la marioneta del anfitrión) | `0x4FA530` fuerza `+0xC8 = 0` si `actor+0x90` ≠ usuario local, y `0x88E8E0` da modo 2 por la regla 3 (`actor+0x90 != 0`) [verificado, `mirror-ack-formats.md` §5, `movimiento-remoto-cliente.md` §1.1] |
| Dueño e índice | `rec+0x05` / `rec+0x09` | **sin cambiar (0 / `0xFF`)** | Con dueño 0 se usa la fábrica `0x4631C0`, que no exige un usuario "logined". El caballo con dueño 0 y `+0x90` ajeno **sí se creó** (intento B del informe del caballo, que luego cayó por la falta de acción) [verificado en vivo] |
| Jugador del anfitrión | `0xA0100001` (aparece ~1 vez por registro) | clave de la copia del anfitrión en el invitado (`0xA010F001`) | Puede ser ruido o una relación real; traducirlo es inocuo [captura, `c1.py`] |
| Marioneta del invitado en el anfitrión | `0xA010F001` / `0xB010F001` / `0xB010F002` si aparecen | clave del jugador real del invitado (de su `SpawnInfo.player_key`) / 0 | La copia apunta al invitado de verdad |
| Posición | las **3 copias** (893 de 1.092 registros; 2 o 4 en el resto) | posición actual (leída de memoria en el anfitrión) + desplazamiento | Mismo método que `spawn_horse`: el último Transform es `escala (1,1,1)` seguido de un padre `FF×8`; la posición está 28 B antes y se sustituyen todas sus apariciones [captura] |
| Rotación | quat 16 B antes del marcador de escala | rotación actual | Como `kLoadRotationFromEnd` en el jugador |
| **No tocar** | facción, variante, equipo, datos del CC | — | La copia tiene que ser idéntica |

Riesgos de esta receta:
- Que el CC no termine el "login" de un actor con `+0x90` ajeno y dueño 0. Si `actor+0xCF` no pasa a
  1, sus Move/Rotate/StartAction Ack se reintentan sin fin (`0x8B53C0`, `mirror-ack-formats.md` §3)
  [inferido]. La etapa 3a lo mide.
- Alternativa si falla: un usuario inexistente (`0x9010F0E0`).
  - El riesgo es que la relación de facción (`0xA35B90`) o la interfaz busquen ese usuario y no lo
    encuentren.
  - Ventaja de `0x9010F001`: los ganchos `RideOn::Enter` y `PointClimb::Enter` de `remote_puppet` ya
    fuerzan el éxito por `owner == kPuppetUser`, así que los enemigos montados también
    funcionarían [verificado en el código].

### 1.3 Cuántos y cuánto pesan

[captura, `b6.py`, 112627, muestreo cada 5 s]

| Situación | NPC vivos (tipos 3-6) | a < 30 m | a < 50 m | a < 80 m | personas a < 50 m | peleando |
|---|---|---|---|---|---|---|
| Campo abierto | 50-75 | 4-20 | 12-30 | 25-49 | 1-25 | 0 |
| Pelea en el campamento (t = 274-294 s) | 55-59 | 9-19 | 13-27 | 42-44 | 10-21 | **4-7** |
| Ciudad (t = 602-665 s) | 324-331 | 73-95 | 154-166 | 228-237 | 120-131 | 0 |

- Ritmo de cargas de NPC: 1.235 en 856 s (1,4/s de media). Pico de 270/s al cargar zona; p90 de
  7/s [captura].
- **Coste de cargas:** un campamento de 7 son unos 54 KB fiables (unos 49 fragmentos de 1.100 B). En
  ciudad, relevar todo serían más de 1 MB, así que **no** se releva a la población civil (§5).

---

## 2. La primera acción

### 2.1 Mismo problema que el caballo [verificado + captura]

- En el anfitrión, la IA local arranca la primera acción de **todos** los NPC enseguida [captura,
  `b4.py`]:
  - `StartActionReq`: 1.232 de 1.235, a 0,264 s de mediana (p10 0,21 s, p90 0,39 s);
  - `ChangeAIStateReq`: 1.232, a 0,238 s;
  - `MoveActorReq`: solo 791, a 1,8 s de mediana.
- En el invitado la copia está en modo 2 (sin IA). Si su capa principal tiene gráfico (`+0x110`) y no
  tiene acción (`+0x10`/`+0x18`), la actualización del CC se cierra en `0x1F63FE6`, `0x232D5FF` o
  `0x925F0E`. El último solo ocurre con la rama de vehículo, es decir, con monturas del mundo de tipo 5
  (`INFORME-CABALLO-CRASH.md` §5).
- Que las capas de un NPC de tipo 3 o 6 tengan gráfico propio desde la carga no está medido. El
  caballo (tipo 5) sí lo tenía y el jugador copia (tipo 1) no. **Hay que suponer que sí** [inferido].

### 2.2 La idea del "byte de restauración"

Lo verificado en el código:
- **Deserializador del CC** (`ClientCharacterControlActorComponent` vf11 = `0x2320200`, lo llama el
  unpack `0x17EA310`):
  1. u32 → `CC+0x18`;
  2. por cada una de las 2 capas: u8 "vacía"; si vale 0, u16 → `capa+0x38`, **u32 hash de acción →
     `capa+0x3C`** y u64 → `capa+0xF8`;
  3. `[CC+0x40]+0x28 = capa0+0xF8`;
  4. u8 bandera (+ u8 → `[CC+0x30]+0x1B`), `0x23307D0`×2 → `[CC+0x40]+8/+0xC` (función virtualizada
     por Denuvo), u8 → `+0x10`, cadena → `+0x38`, u64 → `+0x20`;
  5. **u8 → `[CC+0x40]+0x30` (el byte de restauración)**, u8 → `+0x6E`, …
- **`0x8646D0` (restaurar tras cargar recursos):**
  - si `[[CC+0x40]+0x30] == 0`, no hace nada;
  - si no, monta por capa `{gráfico capa+0x110, hash capa+0x3C, id capa+0x38, posición [CC+0x20]}` y
    llama a `0x863880`, que rearranca la acción, y luego a `0x85ED00` y `0x2329D70`;
  - el modo de control solo afecta al cálculo del tiempo de reanudación (`vf188(5)`, `0x864936`), no a
    la restauración.

Lo medido:
- **En los registros, el bloque de capas trae una acción válida** [captura, `b2.py`]: 1.188 de 1.235
  NPC (el resto, con dos coincidencias ambiguas o ninguna), además del jugador y el caballo.
  - Hash `0x26107D3C` en personas y monturas; `0x866C7489`, `0xA9D16F76`… en animales.
  - Ejemplo de un Bleed Bandit: `00 | 0000 | 3C7D1026 | 5E02010000000000 | 00 | 0100 | 3C7D1026 | …`.
  - Le siguen siempre **17 bytes a cero** antes del siguiente componente (`FF` + cadena
    `character/appearance/…`).
- Que el byte de restauración caiga dentro de esos 17 ceros es lo más probable, pero no se pudo fijar
  su offset: `0x23307D0` está virtualizada y las cuentas de tamaños no cuadran con un bloque CC
  completo [inferido]. Sea cual sea el offset, **en todos los registros capturados vale 0**.
- **En vivo (`captures/cdmp_horse_diag_ok.log`):**
  - la copia del caballo tenía `restoreByte=0` al pasar por `0x8646D0` (+66264 ms), con los datos
    `[CC+0x40]+0x30 = 00 …` y `+0xF8 = 0`;
  - tenía **`restoreByte=1` justo después de aplicarse su primer `StartActionAck`** (+67029 ms);
  - el caballo real lo tenía a 1 durante toda la sesión.

  **Por tanto, es estado de ejecución** ("tengo una acción que restaurar"), no un dato que mande el
  servidor [verificado en vivo].

Conclusión:
- Poner el byte a 1 **en el paquete** no es viable sin el offset exacto.
- Ponerlo a 1 **en memoria** sí es posible. En un mid-hook de solo copias en la entrada de
  `0x8646D0` (ya enganchada por `horse_diag`):
  - si `capa0+0x110 != 0` y `capa0+0x3C != 0`, escribir `[[CC+0x40]+0x30] = 1`;
  - así el motor rearranca la acción del registro por su ruta nativa (`0x863880`) sin esperar a la
    red.
- Es una escritura en la memoria del juego, sobre un componente de nuestra copia. Queda como
  **experimento opcional 3a′**, con copia de `save` antes. La contención sigue siendo la red de
  seguridad [inferido].

### 2.3 Qué primera acción darle

- **Recomendada:** el **último `StartActionReq`** que el anfitrión vio de ese enemigo, guardado por
  clave (725 B). Se convierte con la receta del espejo (§3.3) y se inyecta **en la misma tanda que la
  carga, con el mismo instante de entrega** (la cola es estable, así que queda justo detrás).
  - Hay que reescribir su posición (Ack `+0x8F` y `+0xD7`) con la posición actual y su `ts`
    (`Ack+0x160`) con el reloj del invitado.
  - Si se cuela una acción de ataque a medias, la copia la empieza desde el principio: es aceptable.
- Si el enemigo **aún no ha mandado ninguna** (carga de hace menos de 0,4 s), se retiene la carga hasta
  su primer `StartActionReq`, como hace `horse_event`.
- **Fallback sin red:** el hash del bloque de capas del propio registro (`0x26107D3C`…), vía 3a′.

### 2.4 ¿Se generaliza la contención? Sí

- `horse_diag.cpp`:
  - `on_cc_update` (`0x85FBA0`) y `on_cc_post` (`0x860D30`) solo actúan si `key == kCopyHorse`.
  - Basta con cambiar esa comparación por `is_network_copy(key)`: claves de la marioneta, `0xB010F002`
    y el rango `0xB01F0000…0xB01FFFFF`.
  - Es una lectura de `[CC+8]+0x60` por actor y por frame, en hilos trabajadores: barata y ya
    protegida con SEH [verificado en el código].
- `remote_puppet.cpp`:
  - `puppet_layer_without_action` (guarda de `0x2329700`) usa `is_puppet_key`: hay que ampliarla igual;
  - `action_layer_advance` (`0x1F63F70`) ya actúa sobre cualquier actor con `+0x18` nulo;
  - **pero los dos ganchos solo se instalan con `horse=1`**: hay que instalarlos también con
    `enemies=1`.
- `on_restore` / `on_pending` (solo log) siguen sirviendo para el diagnóstico de 3a con
  `horse_diag_watch` ampliado a una lista de claves.

---

## 3. Estado vivo por enemigo

### 3.1 Catálogo con ritmos

Ritmos medidos solo en las **ventanas de combate** de los 19 enemigos de 112627 (363 s de
enemigo×tiempo) [captura, `a4.py`; formatos de `combate-coop.md`, `mirror-ack-formats.md` y los
volcados de `a8.py`/`a9.py`].

"Traducir": **E** = clave de un enemigo relevado → `0xB01F0000+net_id`; **H** = jugador del
anfitrión → copia del anfitrión en el invitado (`0xA010F001`); **G** = marioneta del invitado en el
anfitrión → jugador real del invitado; **X** = cualquier otra clave → 0 o descartar.

| Mensaje (op) | Dir. en el anfitrión | Ritmo por enemigo en combate | Tamaño | Qué recibe la copia (Ack) | Campos a traducir | Fiable |
|---|---|---|---|---|---|---|
| **StartActionReq** (2223) | C>S | **1,39/s** (mediana entre envíos 0,54 s) | 725 | StartActionAck 2991 (receta §3.3) | `+0x00` E; Req `+0xAA` (u32, a veces clave), `+0x116` (padre u64), **`+0x16C` objetivo** (E/H/G/X); `ts` Ack `+0x160` | no |
| Posición (memoria del anfitrión) | — | 15 Hz si se mueve (> 5 cm), 1 Hz en reposo | 25 B/muestra | MoveActorAck 2697 sintetizado (235 B, §3.4) | — | no |
| MoveActorReq (2556) | C>S | 0,43/s (gaps 1,5 s / p90 3,5 s) | 514 | se sustituye por las muestras de memoria; como mucho sirve de plantilla de la cola `+0xDF` | `+0x00` E, `+0x8C` padre, `+0x20` | — |
| RotateActorReq (2828) | C>S | 0,005/s (raro en NPC) | 176 | RotateActorAck 2687 | `+0x00` E, `+0x54` padre | no |
| **ChangeAIStateReq** (2734) | C>S | **2,16/s** (ráfagas: 446 de 782 a menos de 50 ms de la anterior) | 672 | ChangeAIStateAck 2404 (mismo formato, `ack = E + req[4:]`) | `+0x00` E; `+0x298`/`+0x29C` u32 (en el jugador `FFFFFFFF`) [inferido: pueden ser claves] | no; deduplicar |
| **CurrentTargetChangeReq** (2984) | C>S | 0,16/s | 18 | **CurrentTargetChangedAck 3040**: `actor │ objetivo │ u64 sec │ u8 forzar │ u8 motivo` | `+0` E; **`+4` objetivo**: E/H/G/X; `forzar` = 1; secuencia y motivo copiados (nunca 0x15) | sí |
| UpdateAIDestinationReq (2405), StartAIFunctionReq (2291), AggroRelationListReq (3006), EchoActionSessionIDReq (2350), ChangeBattleStateReq (2257), ProcessAfterBeginFrameEventReq (2189) | C>S | 0,23 / 0,41 / 0,10 / 0,15 / 0,04 / =SA | 57 / 120 / 25-41 / 12 / 5 / 4 | **ninguno** (son del controlador; `ChangeBattleState` no se reenvía, `combate-coop.md` §1.1) | — | — |
| DoFrameEventReq (2380) | C>S | 0,074/s | 517-3.302 | no se reenvía (lo visual va en la acción). Si el objetivo es G → evento de golpe al invitado (3h) | atacante `+0x01`; objetivos en `len−n·665` | — |
| **VaryHPAck** (2147) | S>C | 0,17/s | 92 | igual (relevo) | `+0x00` E; `+0x54` fuente (H/G/E/X); **`+0x40` u32 secuencia por actor: no se toca** | sí |
| ApplyBuffAck / UnapplyBuffAck (2506/3007) | S>C | 0,70 / 0,28 /s | 77 | igual | `+0x00` portador, `+0x18` lanzador, `+0x1C` portador, `+0x21` fuente etiquetada | sí (pares apply/unapply) |
| UpdateElementalAck (2642) | S>C | 0,41/s | 118 | igual | `+0x00` E, `+0x28` fuente | no |
| PostProcessAttackResult_AttackTargetAck (2922) | S>C | 0,20/s | 199/211 | igual: efecto de impacto en la copia | `+0x00` objetivo, `+0x04` atacante | no |
| AttackerAck (2509) | S>C | 0,02/s | 160/172 | igual | `+0x00` atacante, `+0x04` objetivo | no |
| ApplyActionRestrictionAck (2162) | S>C | 0,12/s | 77 | opcional: la reacción ya llega como acción | `+0x00` objetivo | no |
| OnPostBuffDamagedAck (2681) | S>C | 0,17/s | 53 | opcional (números de daño) | `+0x00` objetivo, `+0x04` atacante, `+0x08` fuente | no |
| **NotifyHandleDeadAck** (2530) | S>C | 1 por muerte | 19 | igual | `+0x00` E; `+0x04` asesino (H/G/E/X); `+0x08` u8 motivo (8 = matado) | **sí** |
| **RemoveActorAck** (2516) | S>C | 1 por retirada | 10 (+4·n) | igual, con el motivo del anfitrión | `04 │ u32 0 │ u8 motivo │ n × u32 clave` (E) | **sí** |
| Create/RemoveProjectileAck (2250/2751) | S>C | 0,04/s | 165-289 / 81 | igual (flechas de los arqueros) | `+0x00` dueño, `+0x3D` objetivo, `+0x1D`/`+0x35`/`+0x39` | no |
| LootDropCharacterRewardListAck (2897), RemoveLootableAck (2822) | S>C | 1 por muerte / por saqueo | 5-153 / 8 | **no relevar** (el servidor del invitado no conoce el cadáver) | `+0x01` muerto | — |
| ClosePartyAck (2172), ClearAllyGroupDataAck (2402), DataDefinedRegenerateValueAck (2586), ElementalMaterialFuelAck (2614) | S>C | ráfaga al morir | — | no relevar al principio | — | — |

Notas de formato [captura, `a9.py`/`a8.py`, Bleed Bandit `0xB01006D8`, t = 485,9-488,6 s]:
- **VaryHPAck (92 B):**

  | Offset | Campo |
  |---|---|
  | `+0x00` | clave |
  | `+0x04` | stat (`0x000F4240` = HP) |
  | `+0x08` | i64 HP×1000 |
  | `+0x10` | i64 (0) |
  | `+0x18` | i64 HP máx×1000 |
  | `+0x30` | u64 id de evento: el mismo número que llevan `ApplyBuffAck` y `OnPostBuffDamagedAck` del mismo golpe |
  | **`+0x40`** | **u32 secuencia por actor**: 1, 2, 3, 4, 5 en sus 5 cambios de HP |
  | `+0x4C`/`+0x50` | `FFFFFFFF` en el golpe mortal |
  | `+0x54` | u32 fuente |
  | `+0x58` | u32 (`0x100`) |

- La comprobación de esa secuencia en el cliente no se pudo leer: la aplicación del stat salta a
  `0x17AD2D0`, virtualizada. Lo prudente es **reenviar la secuencia del anfitrión tal cual**. La
  copia nace con los stats que serializó el servidor del anfitrión, así que la numeración es la misma
  [inferido].
- **NotifyHandleDeadAck (19 B):**
  `D80610B0 010010A0 08 00 01 00000000 000000 0A` = muerto, asesino, motivo 8, …
- **Secuencia de muerte:** en el mismo lote llegan:
  - el `VaryHPAck` final (hp 0);
  - `NotifyHandleDeadAck`, `LootDropCharacterRewardListAck` y `ClosePartyAck`;
  - `ClearAllyGroupDataAck` y los `UnapplyBuffAck` del muerto.

  Entre **15 y 75 s** después llega el `RemoveActorAck` del cadáver (motivo 25 o 19) [captura,
  `a6.py`].

### 3.2 Motivos de `RemoveActorAck`

- **Formato** (handler `0xB62C70`): `u8 04 │ u32 X │ u8 motivo │ n × u32 clave` [verificado].
  - `X` lo busca aparte (en las capturas siempre vale 0).
  - Por cada clave llama a `0x2068340(actor, out, motivo, …)`.
  - Se salta los actores de usuario (`u16 +0x5E == 1`) y al jugador local (`+0x96`), salvo con
    motivo 7.
  - **Admite varias claves por mensaje.**
- **Motivos medidos** [captura, `a6.py`/`a7.py`]:

  | Motivo | Cuándo | Actores |
  |---|---|---|
  | 19 | pantalla de carga (`GameLoadingStartReq`; borra todo, cadáveres incluidos) | NPC |
  | 25 | despawn del servidor sin Req (distancia o tiempo; cadáveres) | NPC y animales |
  | 28, 80 | guardias y bandidos (sin causa clara) | NPC |
  | 21 | tras `SceneLevelOnDemandGimmickUnspawnReq` | objetos |
  | 57, 67 | tras `DeactivatePoolSpawnReq` | objetos |
  | 65 | `UpdateGimmickBodyMotionTypeReq` | objetos |
  | 7 | caso especial del handler | — |

- La marioneta usa 0x15 (21), verificado en vivo. **Para los enemigos, reenviar el motivo del
  anfitrión.** Si el motivo cambia el fundido, el invitado verá la misma salida.

### 3.3 Recetas Req → Ack (las mismas del espejo)

Para `K'` = clave del invitado (`0xB01F0000+net_id`):

```text
StartActionAck (2991), 725 B = K' + req[0x04:0x8E] + req[0x2D4] + req[0x8E:0x2D4]
   claves:   Ack +0xAB (u32), +0x117 (padre u64: montura relevada → su copia; si no, FF×8), +0x16D (objetivo E/H/G/X)
   posición: Ack +0x8F (MoveData) y +0xD7 (Path) → desplazamiento del eco; ts Ack +0x160 → reloj del invitado
ChangeAIStateAck (2404), 672 B = K' + req[0x04:]          (exige u32 +0x26C != 0)
RotateActorAck (2687), 177 B = K' + req[0x04:0xA4] + 00 + req[0xA4:0xB0]   (padre +0x54; ts +0x9C)
CurrentTargetChangedAck (3040), 18 B = K' + objetivo' + req[0x08:0x10] + 01 + req[0x11]
```

- `CurrentTargetChangedAck` en una copia en modo 2 [verificado, `companeros-mercenarios.md` §4]:
  - `[[CC+0x60]+0x10] & 2` es 0, así que se aplica;
  - `SetCurrentTarget` no llama a `IsTargetable`;
  - el emisor de `CurrentTargetChangeReq` no reenvía nada al servidor del invitado, porque
    `actor+0x90` no es el usuario local.

### 3.4 Movimiento: muestras leídas de memoria

- En el **anfitrión**:
  - cada 66 ms, por cada enemigo relevado: `game::actor_world_position(key)` y
    `actor_world_rotation(key)` (lecturas protegidas, `actor_lookup.cpp`);
  - si se movió más de 5 cm, o pasó 1 s, se añade al lote `EnemyPos` con la marca de tiempo del reloj
    del juego (`capture_game_clock_ms()`).
- En el **invitado**, `MoveActorAck` de 235 B construido desde cero. Del Ack el cliente remoto solo
  usa posición, quat, `ts` y la cola (`movimiento-remoto-cliente.md` §3) [verificado]:

```text
+0x00 K' | +0x04 pos | +0x10 quat | +0x20 u32 0 | +0x24 Transform{pos, quat, escala 1,1,1}
+0x4C Path{local = pos/quat/escala 1, vel 0, vec3 0, padre FF×8, Transform mundo 0, 0, 0}
+0xD4 u8 0 | +0xD5 u64 ts (rebasado) | +0xDD u8 0 | +0xDE u8 0 | +0xDF f32,f32 (0 o los del último Move real) | +0xE7 f32 0
```

- Reglas del búfer (§2 del mismo documento):
  - `ts` estrictamente creciente **por actor, contando Move y StartAction juntos**;
  - nunca más de 1 s entre muestras aceptadas. En reposo, alternar ±1 mm en Y cada segundo.
- Esto evita los saltos que darían los Move nativos, que llegan cada 1,5 s.

---

## 4. Suprimir los enemigos propios del invitado

### 4.1 Las Req de spawn [captura, `a5.py`/`b7.py`]

| Req (op) | Veces en 856 s | Tamaño | Qué carga después |
|---|---|---|---|
| AutoSpawnRoadReq (2550) | 1.170 | 5 + 24·n: `{u32 id, u16, u16, vec3 pos, f32}` | NPC de camino (personas, animales, carros) |
| ActivateSpawningPoolReq (2424) | 724 | 1 + 54·n (id u64 de la pool) | **objetos de tipo 7** (301 identificados) y algunos NPC y animales |
| SceneLevelOnDemandGimmickSpawnReq (2928) | 956 | 25 | casi todo objetos (tipo 7) |
| MeshGroupAutoSpawnReq (2809) | 173 | 71 | animales (99) y NPC (19) |
| TerrainRegionVoxelSpawnReq (2327) | 47 | 38 | fauna de la región |
| RegisterSpawnDistanceByExternalReq (2817) | **0 en las 38 capturas** | — | — (solo aparece `Reset…` 3102) |
| Desactivación: DeactivatePoolSpawnReq (2119), DeactivateMeshGroupAutoSpawnReq (2259), TerrainRegionVoxelSpawnDeactivationReq (2690) | 863 / 75 / 39 | — | retiradas con motivos 57/67… |

La mayoría de las cargas de tipo 3 no se pueden atribuir a un id de Req, porque el servidor no copia
el id de la pool en el registro.

### 4.2 Opciones

| Opción | Qué se pierde | Riesgo para misiones y partida | Veredicto |
|---|---|---|---|
| **A. Bloquear Req de spawn en `PostPacket`** (no reenviar) | También objetos (pools de tipo 7), civiles y fauna. Al desbloquear, el cliente no las repite hasta reentrar en la región | **Alto** [inferido]: las pools y los gimmicks tienen estado guardado (`SyncFieldGimmickSavedByGimmickStateAck`) y las `Deactivate*` de pools nunca activadas pueden dar Nak 1013 | No |
| **B. Descartar en el cliente las cargas de NPC hostiles dentro de la burbuja** (y retirar con `RemoveActorAck` inyectado, motivo 0x15, las que ya estén) | Solo los hostiles locales. Civiles y fauna siguen siendo los del invitado | **Medio-bajo** [inferido]: el servidor del invitado sigue teniendo esos NPC, sin IA de cliente (quietos). No mueren, así que no hay cambios en la lista de muertos ni en contadores. Las misiones de "matar X" no avanzan en el invitado (como en Genshin) | **Sí** |
| C. Dejarlos y ocultarlos | Nada, pero siguen atacando al invitado (invisibles) | Alto | No |
| D. Emparejar "gemelos" por variante y punto de aparición | — | Los spawns no son deterministas: entre sesiones, solo 3 de 94 NPC caen en el mismo punto exacto y el resto queda a 1-15 m [captura, `b8.py`] | No |

**Receta B (solo con dos PC):**
1. La burbuja: el invitado está a menos de `R_burbuja` = 100 m de la posición del anfitrión (su
   `PlayerState`).
2. En `on_recv_packet` del invitado, para cada `ActorLoadDataAck` de un solo registro de tipo 3 o 6
   con `rec+0x1A` en el conjunto de **facciones hostiles** y su posición de carga dentro de la
   burbuja: **no entregarlo**.
   - Se hace saltando la función con el stub `ret`, como en `horse_diag`. El mid-hook está en la
     entrada de `0x35CA00`, que devuelve void [inferido: hay que comprobar que el llamador no espera
     un resultado].
   - El conjunto de facciones lo comparte el anfitrión (fiable):
     - semilla `{0xF4250, 0xF42C0}`;
     - más cualquier `rec+0x1A ≠ 0` de un NPC que haya fijado o golpeado a un jugador (o a la
       marioneta).
3. Al entrar en la burbuja: `RemoveActorAck` inyectado (motivo 0x15) para los NPC locales ya
   cargados que cumplan lo mismo.
4. **Nunca** tocar el tipo 4 (personajes con nombre: "Anders" y "Arnold" eran de tipo 4 y tal vez de
   misión), ni NPC cuya carga llegue junto a `CreateFactionSpawnStageAck` (2195).
5. Al salir de la burbuja o desconectar: dejar de filtrar.
   - Los NPC descartados **no vuelven** hasta que la región se recargue (alejarse, viaje rápido,
     pantalla de carga). Es una limitación aceptada, que se documenta en la interfaz.
6. **Apagar el daño espejo** (`EventKind::Damage`, `apply_remote_damage`) mientras el anfitrión tenga
   la autoridad. Si no, los golpes del anfitrión se aplicarían también a gemelos locales del
   invitado.

---

## 5. Área de interés

**Qué releva el anfitrión** (de los actores que su sniffer vio cargar: tipos 3, 6, 5 y 4 con dueño 0,
y **nunca** claves del rango de copias ni de la marioneta):
1. **Comprometidos**:
   - un `CurrentTargetChangeReq` hacia el jugador del anfitrión o hacia la marioneta;
   - golpeados (`AttackTargetAck` como objetivo) o atacantes (`DoFrameEventReq` con un jugador o la
     marioneta como objetivo);
   - `ChangeBattleStateReq` con estado ≠ 0.

   Se quedan "pegados" hasta su muerte o retirada.
2. **Hostiles por facción** (`rec+0x1A` en el conjunto de §4.2), aunque estén quietos: así el
   invitado ve el campamento antes de la pelea.
3. **Sus accesorios:**
   - objetos de tipo 7 cuya carga contiene la clave de un enemigo relevado: las armas "on demand"
     llegan como 2-3 cargas de tipo 7 de 6,5 KB tras `OnDemandEquipDockingGimmickSpawnReq`
     [captura, `b9.py`];
   - su montura, si la `Path.parent` de sus Move apunta a una de tipo 5.

**Filtro espacial**, con la posición del invitado **en el mundo del anfitrión**: posición de la
marioneta (`actor_world_position(0xA010F001)`) o su `PlayerState`:
- carga si está a menos de 60 m; retirada si pasa de 80 m (histéresis);
- como mucho 24 a la vez: primero los comprometidos y después por distancia;
- la retirada por área se manda con el motivo 0x15.

**Cuándo:**
- **Carga**: en cuanto un actor entra en el conjunto. Si aún no tiene `StartActionReq`, se espera a
  la primera (≤ 0,4 s).
- **Retirada**: con el `RemoveActorAck` real (con su motivo), al salir del área, o en bloque
  (`EnemyReset`) si el anfitrión pasa por una pantalla de carga (motivo 19 en masa) o cae el enlace.
- **Entrada tardía** (el invitado conecta o vuelve de una carga), en este orden y todo por el flujo
  fiable ordenado:
  1. por cada enemigo vivo dentro del área, `EnemySpawn` (carga con posición y rotación actuales +
     última acción + último `VaryHPAck` como instantánea de HP + último objetivo + buffs activos
     cacheados);
  2. después, el flujo normal.
  - Antes de que la marioneta del anfitrión esté cargada en el invitado no se manda nada: hace falta
    su usuario `0x9010F001` y el reloj.
- **Orden**: `EnemySpawn` → estado (HP, objetivo, buffs) → `NotifyHandleDead` → `RemoveActor`, en el
  mismo flujo ordenado.
  - Los eventos no fiables (acciones y posiciones) llevan `(net_id, gen)`.
  - El invitado descarta los que no correspondan a una aparición ya aplicada o a una `gen` vigente.
  - Así nadie ve vivo un enemigo que ya murió (lección de Genshin y BDO).
- **Reutilización de `net_id`**: no antes de 10 s tras su retirada, y con `gen++`.

---

## 6. Protocolo (`src/common/protocol.h`)

`kVersion` 5 → **6**. El anfitrión **traduce todas las claves a claves del invitado** antes de enviar:
- E → `0xB01F0000+net_id`;
- H → `0xA010F001`;
- G → `SpawnInfo.player_key` del invitado.

Así el invitado solo inyecta y no necesita tablas. El desplazamiento del eco y el rebase de `ts` sí se
hacen en el invitado, porque dependen de su reloj.

| Nuevo | Tipo | Formato | Uso |
|---|---|---|---|
| `RelKind::EnemySpawn = 5` | fiable, **ordenado** (prefijo u32 de secuencia, como `Relay`) | `u16 net_id │ u16 gen │ u8 flags │ u32 clave_anfitrión (diagnóstico) │ u8 n │ n × {u16 op │ u16 len │ payload}` | Paquete de inyección: `ActorLoadDataAck` + primera `StartActionAck` + `VaryHPAck` + `CurrentTargetChangedAck` + buffs. Unos 8-9 KB |
| `RelKind::EnemyState = 6` | fiable, ordenado (mismo flujo) | `u16 net_id │ u16 gen │ u16 op │ payload` | VaryHP, Apply/UnapplyBuff, CurrentTargetChanged, NotifyHandleDead, RemoveActor (con el motivo) |
| `RelKind::EnemyReset = 7` | fiable | `u16 n │ n × u16 net_id` (n = 0: todos) | Pantalla de carga, salida del área en bloque, desconexión |
| `EventKind::EnemyAck = 9` | no fiable | `u16 net_id │ u16 gen │ u16 op │ payload` (≤ 1.024) | StartActionAck (725), ChangeAIStateAck (672, deduplicado), RotateActorAck, AttackTarget/AttackerAck, proyectiles |
| `EventKind::EnemyPos = 10` | no fiable, **por lotes** | `u8 n │ n × {u16 net_id │ u8 gen_lo │ u32 ts │ f32 x,y,z │ i16 qx,qy,qz}` = 25 B por muestra (w se reconstruye ≥ 0) | 15 Hz. Hasta 40 muestras por datagrama |

- `decode` de `GameEvent` hoy rechaza `kind > 8` y el de `RelData` rechaza `kind > 4`: hay que
  ampliar los dos límites.
- `event_timestamp` debe devolver 0 para las nuevas clases. El invitado usa el desfase de reloj que ya
  calcula `CoopLink::clock_` con los eventos de la marioneta.
- **Compresión (opcional, etapa 3e):** zlib por mensaje deja StartAction en 153 B de media y
  ChangeAIState en 82 B [captura, `c2.py`]. No hay zlib en el proyecto; un XOR-delta contra el último
  enviado por `net_id` + RLE logra algo parecido, porque el 50 % de los ChangeAIState consecutivos
  difieren en menos de 10 bytes.
- **Ancho de banda estimado** por enemigo en combate (datos del juego + 7 B de cabecera de evento):

  | Mensaje | B/s |
  |---|---|
  | StartAction | ~1.020 |
  | posiciones | ~375 |
  | estado (HP, buffs, efectos, objetivo, impactos) | ~210 |
  | **total sin IA** | **≈ 1,6 KB/s** |
  | ChangeAIState sin comprimir (+1.460) | ≈ 3,1 KB/s |
  | todo comprimido | ≈ 1,0 KB/s |

  Con **7 enemigos** (el máximo medido): **11 / 22 / 7 KB/s**. Hay que sumar las cargas (7,7 KB por
  enemigo, unos 54 KB por campamento) y unos 2-3 KB/s de cabeceras UDP y del protocolo.

---

## 7. Plan de prueba en un PC (eco con `cdmp_peer`)

**Montaje:**
1. `cdmp_peer --mode relay --delay 50`; en `cdmp.ini`:
   - `[coop] enemies=1` (con el eco, este PC es a la vez anfitrión e invitado);
   - `enemy_offset_m=4` (más que los 2,5 m de la marioneta: las copias tienen colisión).
2. Copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/` antes de cada prueba: la inyección
   escribe en el cliente.
3. Ir a un campamento de Bleed Bandits. En 112627 estaba cerca de (−10.445, 664, −3.346).

**Qué mirar:**

| Paso | Señal de éxito | Dónde |
|---|---|---|
| Carga | `Inject: delivered opcode 2005`; `find_actor(0xB01F0001)` existe; `+0x5E == 0x10`, **`+0xCF == 1`**, `+0xC8 == 0`, `+0x90 == 0x9010F001`; modo `0x88E8E0` = 2 | `cdmp.log` + un volcado tipo `aggro_test::log_fields` |
| Primera acción | `HorseDiag … COPY 0xB01F0001` con `cur(+10)=true` antes de 1 s; contención ≤ 2 saltos; ningún cierre | `horse_diag` ampliado |
| Movimiento | Error entre la copia y el original (retrasado `delay + 200 ms`, sin desplazamiento) < 0,5 m de mediana; `count` del búfer `mc+0x3D0+0x10` nunca baja a 1 (sin reinicios) | log de seguimiento cada 1 s |
| Acciones | La copia ataca, bloquea y reacciona con el original | captura de pantalla (`CopyFromScreen`, recortada) |
| HP y muerte | La barra de la copia baja con los golpes al original; la copia muere en el mismo golpe y desaparece con el cadáver real (motivo 25) | log + captura |
| Objetivo | La copia "mira" a tu marioneta. En el eco, H = tu propia copia | log de `CurrentTargetChangedAck` |
| Carga | Bytes/s por clase de mensaje en el log de `CoopLink::describe` | log |

**Riesgos del eco y cómo evitarlos:**
1. **Realimentación.**
   - El sniffer ve las cargas y Acks **inyectados** (`on_recv_packet` los captura al reentrar).
     `capture_observe_s2c` aprendería las copias `0xB01F…` como enemigos (`g_enemies`, `g_has_hp`) y
     el relevo las volvería a mandar.
   - **Hay que excluir el rango `0xB01F0000-0xB01FFFFF`** en el aprendizaje de NPC, en la selección
     del relevo, en `capture_is_enemy` y en `apply_remote_damage`. Hoy solo se excluyen las claves de
     la marioneta.
2. **Colisión:** la copia a 4 m puede empujar al original o bloquear su camino. Si molesta, subir el
   desplazamiento o desplazar en Z.
3. **La IA local apuntando a las copias:** `IsTargetable` rechaza las copias por `TS+0x3DC`, la marca
   de "movido por red" [verificado en vivo, commit 6a906a9]. El gancho
   `enemies_target_copy` solo afecta a `0xA010F001`.
4. **Doble HUD:** dos barras de vida o dos iconos de enemigo en el minimapa. Es esperable en el eco.
5. **Cierres:** el riesgo nuevo son los tipos 5 (monturas del mundo, rama de vehículo, `0x925F0E`) y 6.
   Empezar solo con tipo 3 y meter 5 y 6 en 3f.

**Lo que el eco no prueba:**
- la supresión del §4 (en el eco no hay segundo mundo);
- la latencia real;
- el `+0xCF` con un usuario ajeno distinto del propio.

Lo primero se puede probar en seco: modo "dry-run" que solo registra qué cargas se descartarían.

---

## 8. Plan por etapas (de lo más pequeño a lo más grande)

### 3a. Una copia quieta (carga + primera acción + contención)
- **Anfitrión:**
  - guardar por clave de NPC (tipos 3/6/5/4 con dueño 0):
    - la carga (payload);
    - el último `StartActionReq` (725 B);
    - el último `VaryHPAck`;
    - el último `CurrentTargetChangeReq`.
  - Tecla de desarrollo (p. ej. Ctrl+F6): el NPC con HP más cercano (`capture_nearest_actor`) →
    `EnemySpawn` con `net_id = 1`.
  - Reescrituras de la §1.2 (en un solo paso con un mapa de claves para que H y G no se pisen).
- **Invitado:**
  - inyectar todo el paquete con el mismo instante de entrega;
  - `is_network_copy()` en `horse_diag` y en los guardas de `remote_puppet`, instalados con
    `enemies=1`;
  - `horse_diag_watch` con una lista de claves.
- **Decide:** si se crea y si `+0xCF`/modo 2 son correctos. Si `+0xCF` no llega a 1, probar el usuario
  inexistente (§1.2).
- **3a′ (opcional, escribe en memoria):** el byte de restauración puesto a 1 en `0x8646D0` (§2.2).
  Decide si la ruta nativa basta y la contención deja de actuar.

### 3b. Movimiento y acciones
- `EnemyPos` a 15 Hz desde la memoria del anfitrión → `MoveActorAck` sintetizado (§3.4).
- `StartActionReq` → `EnemyAck(StartActionAck)` con la traducción de objetivo y padre, y
  `RotateActorReq`.
- **Medida:** el error de seguimiento y los reinicios del búfer (§7).

### 3c. Vida, muerte y retirada
- `VaryHPAck`, `NotifyHandleDeadAck` y `RemoveActorAck` por `EnemyState`, con la instantánea de HP
  dentro de `EnemySpawn`.
- **Medida:**
  - que la secuencia `+0x40` del anfitrión se acepta (la barra baja);
  - que la regeneración no "revive" la copia, porque el invitado no tiene servidor para ella.

### 3d. Objetivo, IA y efectos
- `CurrentTargetChangedAck` (forzar = 1).
- `ChangeAIStateAck` detrás de un interruptor, deduplicado: el último por enemigo cada 100 ms.
  Comprobar primero si cambia algo visible (postura de combate, giro de cabeza); si no, dejarlo
  apagado.
- `Apply/UnapplyBuff`, `UpdateElemental`, `AttackTargetAck`/`AttackerAck` (impactos) y proyectiles.

### 3e. Área de interés automática y varios enemigos
- La selección del §5, el `net_id`/`gen` definitivos y los lotes `EnemyPos`.
- La entrada tardía: desconectar y reconectar a mitad de una pelea.
- `EnemyReset` en las pantallas de carga.
- Compresión si el ancho de banda lo pide.
- **Medida:** un campamento de 7 enemigos, con los bytes/s en el log.

### 3f. Accesorios
- Armas "on demand" (cargas de tipo 7 que contienen la clave del enemigo) y su retirada.
- Monturas del mundo y jinetes: padre traducido, `RideOn::Enter` forzado por `owner == kPuppetUser`.
- Animales (tipo 6).

### 3g. Supresión en el invitado (dos PC)
- La receta B del §4.2, con dry-run primero.
- Conjunto de facciones compartido.
- Daño espejo apagado.

### 3h. Puente a la pieza 4 (no es parte de esta)
- En el anfitrión, un `DoFrameEventReq` de un enemigo cuya lista de objetivos lleva la marioneta
  `0xA010F001` → `EnemyHit` al invitado con `{net_id, hash e índice del accessor, punto de impacto,
  hueso}`.
- El invitado se aplica el daño con `VaryStatByAccInputReq` sobre su jugador e inyecta un
  `AttackTargetAck` visual (`companeros-mercenarios.md` §5).
- **Ojo:** `IsTargetable` del jugador **invitado** puede rechazar las copias de enemigos por
  `TS+0x3DC`, impidiendo que las fije para pegarles. Habrá que ampliar el gancho de `aggro_test` en
  la pieza 4 [inferido].

---

## 9. Riesgos y preguntas abiertas

| # | Riesgo o duda | Cómo se decide |
|---|---|---|
| R1 | `actor+0xCF` (login) no llega a 1 en una copia con dueño 0 y `+0x90` ajeno, y sus Ack se reintentan sin fin | 3a: volcado de campos |
| R2 | Las capas de los NPC de tipo 3/6 tienen gráfico desde la carga (y por eso la contención es necesaria) | 3a: `horse_diag` (`chart=`) |
| R3 | La secuencia `+0x40` de `VaryHPAck` y la regeneración en la copia | 3c |
| R4 | `ChangeAIStateAck` en una copia con "cerebro" de IA: ¿activa algo de IA local? `0x9245B0` sale si no hay cerebro; con un NPC sí lo hay [inferido] | 3d, con el interruptor |
| R5 | Monturas del mundo (tipo 5): rama de vehículo, cierre `0x925F0E` | 3f, con la contención activa |
| R6 | Saltar el handler `0x35CA00` para descartar cargas en el invitado | 3g: dry-run; comprobar el llamador |
| R7 | NPC de misión descartados por la supresión | Excluir el tipo 4 y la facción 0; interruptor en el ini |
| R8 | **Hallazgo lateral:** las claves de la marioneta (`0xB010F001-F003`) pueden chocar en sesiones largas. El contador `0xB01` llegó a `0x32E2` en 27 min (~8/s): `0xF001` se alcanzaría en unas 2 h. El de `0xA01` también sube tras un relogin (`0xA0101740` en 183530) | Mover la marioneta al rango alto (`0xB01FFFF0+`) [captura, `b5.py`] |
| R9 | Las copias del eco empujan a los originales | Desplazamiento mayor |
| R10 | Ancho de banda en zonas densas | Límite de 24 y prioridad a los comprometidos |

---

## 10. Direcciones y scripts (build 2976)

| RVA | Qué es |
|---|---|
| `0x2320200` | `ClientCharacterControlActorComponent` vf11: deserializa el CC del registro (capas `+0x38`/`+0x3C`/`+0xF8`; `[CC+0x40]+0x30` byte de restauración) |
| `0x8646D0` | restaura las acciones de las capas tras cargar recursos (solo si `[[CC+0x40]+0x30] != 0`) → `0x863880`, `0x85ED00`, `0x2329D70` |
| `0x23307D0` | helper del deserializador del CC (virtualizado, `.xtls`) |
| `0xB62C70` | `RemoveActorAck` (cliente): `u8 04 │ u32 │ u8 motivo │ n × u32 clave`; `0x2068340(actor, out, motivo, …)` |
| `0xB75020` → `0x62FA00` → `0x17AD2D0` | `VaryHPAck` (cliente) → aplicación del stat (virtualizada) |
| `0xB68520` | `CurrentTargetChangedAck` (cliente) |
| `0x85FBA0` / `0x860D30` / `0x2329700` / `0x1F63F70` | puntos de contención (ya enganchados) |
| `0x35CA00` | `ClientNet::OnRecvPacket` (supresión del §4.2) |

Scripts en `build/research/enemigos/` (dejan cachés `.pkl` en el directorio desde el que se lanzan):

| Script | Qué hace |
|---|---|
| `cap.py` | lector de capturas con caché, separación en registros |
| `a1.py` | tipos y tamaños de los registros de `ActorLoadDataAck` |
| `a2.py` | cabecera de registro (dueño, `+0x12`, variante, nombre) |
| `a3.py` / `a4.py` | mensajes por enemigo; enemigos "hostiles" y ritmos en las ventanas de combate (guarda `hostile_*.pkl`) |
| `a5.py` / `a7.py` | Req de spawn; motivos de `RemoveActorAck` frente a la Req previa |
| `a6.py` | motivos de `RemoveActorAck` por tipo de actor y muerte |
| `a8.py <bin> <clave> t0 t1` / `a9.py` | línea de tiempo de un actor / volcado de VaryHP y muerte |
| `b1.py` / `b2.py` / `b3.py` | bloque de capas del CC en los registros |
| `b4.py` | tiempo desde la carga hasta la primera acción |
| `b5.py` | máximo serial de clave por captura |
| `b6.py` | NPC vivos y cercanos al jugador a lo largo del tiempo |
| `b7.py` / `b8.py` | atribución de cargas a ids de spawn / emparejamiento entre sesiones |
| `b9.py` | qué sigue a las Req "on demand" de los enemigos |
| `c1.py` / `c2.py` / `c3.py` | claves dentro de los registros / redundancia y compresión / `rec+0x1A` frente a hostilidad |
| `rb_scan.py` | busca escrituras de `[[x+0x40]+0x30]` |
