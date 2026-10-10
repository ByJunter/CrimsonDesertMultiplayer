# Proyectiles (flechas, mosquetes, objetos lanzados) en cooperativo (2026-10-04)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Las direcciones son RVA (`exe+…`) y
valen solo para esta build. Investigación de solo lectura:
- 47 capturas del sniffer (`bin64\cdmp_packets_20261004-*`, incluidas `111538` y `114648`, y
  `D:\cdmp-grabaciones\*`);
- logs del mod (`captures\cdmp_piece4_experiments.log` = sesión `111538`, `bin64\cdmp.log` = `114648`);
- análisis estático con capstone sobre el exe en disco.

No se ejecutó el juego ni `cdmp_peer`. Scripts y volcados en `build/research/p_proj/` (§9).

Etiquetas: **[verificado]** = leído en el código del juego o del mod; **[captura]** = medido en
capturas o logs; **[inferido]** = deducción pendiente de prueba en vivo.

Notación:
- **H**: anfitrión (dueño real de los enemigos);
- **G**: invitado;
- copia de enemigo: `0xB01F0000+n`;
- jugador: `0xA0100001`;
- marioneta: `0xA010F001`;
- `pkey`: clave de `ProjectileInfo` (tipo de proyectil).

---

## 0. Resumen

1. **El accessor de tipo 1 no lleva un contador: lleva el proyectil entero.** [captura, 369/369]
   - Mide 24 B, no 12 (por eso el `DoFrameEventReq` mide 1.319 B):
     `u8 1 | u32 dueño | u32 índice del proyectil | u32 pkey | u8 | u32 a | u8 b | u32 tirador | u8`.
   - El "contador" de `pieza4-capturas.md` §1.1 (`01A01000`, `06A01000`…) era una lectura
     desalineada: los bytes 2-5 del dueño más el índice.
   - En los 369 golpes de tipo 1 sin truncar, `(dueño, índice, pkey)` coincide con el
     `CreateProjectileAck` de ese proyectil (`+0x1D`, `+0x21`, `+0x19`).
   - **`pkey` determina el hash de ataque y el daño** (tabla §1.3). Sirve como clave de daño para las
     piezas 2 y 4.
2. **(a) Las flechas de los enemigos de H sí alcanzan a la marioneta, y la pieza 2 ya las releva,
   pero con una clave basura.** [captura + log]
   - 10 `OnHitProjectileReq` + 10 `DoFrameEventReq` de tipo 1 con la marioneta como objetivo, todos
     de arqueros Bleed Bandit (`pkey 719C2056`). El servidor de H no responde nada.
   - `player_hits.cpp` lee el "hash" en `+0x1F9`, que en tipo 1 es `dueño>>8 | índice<<24`. Resultado
     en 9 golpes relevados: 5 con "damage not learnt" (15 HP por defecto) y 4 con valores aprendidos
     por casualidad (colisiones de esa clave). El daño real es de 24-30 HP.
   - Arreglo pequeño: clave `variante + pkey` cuando el accessor es de tipo 1.
3. **(b) Las flechas de G contra copias: `guest_hits.cpp` trata todas como una sola "flecha" de 28 HP.**
   [verificado + captura]
   - Con `pkey` se distinguen las flechas: `F653A168` = 28 HP (hash `10481FDD`) y `E912512D` = 17 HP
     (hashes `9683E1EB`/`7ED8767D`).
   - Arreglo pequeño: `act = pkey + campos a/b` en lugar de `kProjectileAct`.
4. **(c) Lo visual.**
   - **Flechas de los enemigos de H en el PC de G:** la copia ejecuta sola el frame event de proyectil
     (83 `CreateProjectileFromFrameEventReq` con dueño = copia, 0 respuestas del servidor).
     - El cliente **crea el proyectil localmente antes** de enviar esa Req [verificado: orden de
       llamadas en `0x78B720`].
     - Así que lo más probable es que **ya se vean** [inferido]. Una prueba visual sin código (T3) lo
       decide.
     - Defecto relacionado: el objetivo relevado `0xA010F001` en G significa "la marioneta de H", no
       "yo". Con dos PCs, la copia apuntaría al personaje de H (§4.3).
   - **Flechas de G en el PC de H:** **no se ven.** La marioneta reprodujo 109 acciones de disparo
     del jugador y no creó ni un proyectil (0 Req con dueño = marioneta) [captura].
     - Arreglo: relevar los `CreateProjectileAck`/`RemoveProjectileAck` de G como hace el MMO
       (ToNear), con los campos de clave traducidos.
5. **Recomendación** (§6), por orden:
   1. claves de proyectil en las piezas 2 y 4 (bajo riesgo, sin tocar el juego);
   2. T3, para ver si las flechas de las copias ya se ven;
   3. relevo de los Ack de proyectil del jugador hacia la marioneta (riesgo medio: un tipo nuevo de
      inyección S→C);
   4. corregir la identidad "marioneta = el receptor" en los relevos de enemigos (afecta a más cosas
      que los proyectiles).

---

## 1. Evidencia común

### 1.1 Ciclo de vida de un proyectil [captura + verificado]

Ejemplo: arquero `B01002DA` dispara a la marioneta (`111538`, t = 169,7 s):

```
169.708 > CreateProjectileFromFrameEventReq (142)  dueño B01002DA, accessor {0,0,D1396547,0,20},
                                                   pkey 719C2056, 2.ª clave 5724D7B8, objetivo +0x84 = A010F001
169.723 < CreateProjectileAck (289)                dueño, pkey +0x19, índice +0x21 = 1, objetivo +0x3D
169.905 > OnHitProjectileReq (28)                  B01002DA, índice 1, B01002DA, objetivo A010F001, punto
169.905 > OnAirFinishProjectileReq (12)
169.905 > DoFrameEventReq (1319)                   atacante B01002DA, accessor tipo 1, objetivo A010F001
          (sin respuesta del servidor: ni AttackTargetAck ni VaryHPAck)
169.938 > RemoveProjectileReq (77)  →  169.951 < RemoveProjectileAck (81)
```

El mismo disparo contra el jugador (t = 168,06 s, `B0100311`) recibe a los 3 ms
`AttackTargetAck` (211 B, flags `F7`, hash `38D90004`), `VaryHPAck` (fuente = el arquero),
`OnPostBuffDamagedAck` y `AttackerAck` (172 B, con el accessor de tipo 1 al final).

Tiempos (`tof.py`, medianas): Ack a los 8 ms; primer evento de fin (impacto, rebote o caída) a los
237 ms (flecha de bandido, p90 1,26 s), 82 ms (mosquete) y 53-212 ms (flechas del jugador).

### 1.2 Censo (47 capturas) [captura, `census.py`, `hits.py`, `shooters.py`]

| Mensaje | Dueño = mundo | Dueño = jugador | Dueño = copia | Dueño = marioneta |
|---|---|---|---|---|
| `CreateProjectileFromFrameEventReq` 2387 (C→S) | 444 | 172 | **83** (objetivo siempre `A010F001`) | **0** |
| `CreateProjectileAck` 2250 (S→C) | 666 | 203 | **0** | **0** |
| `OnHitProjectileReq` 2999 (C→S) | 189 | 324 | 0 | 0 |
| `RemoveProjectileReq`/`Ack` 2399/2751 | 654 | 203 | 0 | 0 |
| `OnFailedToCreateProjectileReq` 2935 | 0 | 0 | 0 | 0 |

`DoFrameEventReq` de tipo 1 por atacante y objetivo:

| Atacante → objetivo | Golpes |
|---|---|
| jugador → mundo | 190 |
| mundo → jugador | 91 |
| mundo → mundo | 59 |
| mundo → nadie (541 B) | 50 |
| **jugador → copia** | **18** |
| **mundo → marioneta** | **10** |
| mundo → copia | 1 |

### 1.3 `pkey` → hash de ataque → daño [captura, `arrows.py` → `arrows.csv`, `arrows_base.py`]

Golpes `F7` normalizados con la defensa de la variante (`seed_defence.csv`):

| pkey (`a`/`b`) | Quién dispara | Hash `AttackTargetAck +0x24` | Daño base (defensa 1,0) |
|---|---|---|---|
| `F653A168` (0/0) | jugador, flecha normal | `10481FDD` (siempre) | **28** (×2 en ~8 %: críticos, no tiros a la cabeza) |
| `E912512D` (3/1) | jugador, otra flecha | `9683E1EB` | **17** (16,3 con 0,857) |
| `E912512D` (6/2) | jugador | `7ED8767D` | **17** |
| `B4AF8D6F` (0/0) | jugador | `5A494E51` (flags `E1`) | 15 |
| `9A040230` (0/0) | jugador | `BDDD3405`/`AC823D18` | 14 / 7 (varios hashes) |
| `F2EE2678` (0/0) | colisión del mod (`AttackByCollisionReq`) | `8710E690` | 7 |
| `719C2056` (1/1) | arqueros Bleed Bandit (`54107/54111/54122/54139…`) | `38D90004` | al jugador: 24-30 según la variante (52 = crítico) |
| `84637121` (2/2) | **mosqueteros** `NGM_Wolf_Pursuer_OneHandMusket_1/2/3` | `D932950F` | al jugador: 6 (11 = crítico) |
| `384CAC36` | mosqueteros (otra acción, `754F1496`/`3D1F7CA0`) | — | 2 impactos en 20; sin fin en la mayoría (¿granada o humo?) |
| `53ADD11E` | `NHM_Antumbra_Wizard_1` | `0BF80BB7` | al jugador: 60 |
| `8903CF57` (7/1) | `Bleed_Bandits_DualAxe_1` (¿hacha lanzada?) | `BE8B26C1` | al jugador: 59 |
| `16A0E84B` | gimmick `70001` (trampa) | `9F90AFC4` | al jugador: 11 |

- `pkey` → hash es una función en todas las claves del jugador, salvo `9A040230` y `B4AF8D6F`, que
  tienen varios hashes.
- Los campos `a` (u32) y `b` (u8) del accessor separan los dos usos de `E912512D` (§0.1). Su
  significado es desconocido; pueden ser variante o carga del tiro [inferido].
- En el `CreateProjectileFromFrameEventReq`: `+0x00` dueño, `+0x04` accessor de la acción que dispara
  (12 B), `+0x10` pkey, `+0x14` segunda clave (¿flecha como objeto?), `+0x18` posición de salida,
  `+0x24` quat, `+0x4C` punto de apuntado, `+0x84` objetivo.
- En el Ack (289 B): `+0x60` salida, `+0x90` punto de apuntado y `+0xC4` quat.

### 1.4 Por dónde pasa en el código [verificado]

- **Cliente, al disparar** (frame event de tipo 7):
  `ClientFrameEventProjectile` (vt `0x5585540`) vf1 `0x7F8AB0` → `.xtls 0x970DB60` →
  `0x79E3E0` (arma un `FrameEventProjectileCreateDesc`, vt `0x55A1638`) → vf4 `0x78B720`:
  1. `0x796B40`: **crea el proyectil en local**; acaba en `ClientProjectileActorComponent` vf42
     (`+0x150`) `0x793660` → `.xtls 0x9447820`;
  2. si la clave del actor tiene el bit 30 (`[actor+0x60] & 0x40000000`), no envía nada;
  3. si no: `0xBF31B0` envía el 2387, y luego vf45 `0x7A51E0` → `.xtls 0x9461840`.
- **Cliente, al recibir `CreateProjectileAck`** (vf2 `0xBF3D50`):
  - si el dueño existe, exige `[actor+0x5E] == 0x10` y `[actor+0xCF] != 0`; si no, error
    `0x6CDB9D0` y no crea nada;
  - las claves de usuario (`& 0x30000000 == 0x10000000`) se saltan esta comprobación;
  - luego `[actor+0x68]+0x88` (`ProjectileComp`) → `0x794D80` → `.xtls 0x944D0C0` → **la misma vf42**;
  - si falla, `0xBF3BC0` (probablemente `OnFailedToCreateProjectileReq`).
  - **No mira el modo de control del dueño.**
- **Cliente, al impactar** (`0x81B8F0`, zona de `ClientProjectile_Shot`, vt `0x55AF148`):
  - procesa el evento (efectos, `0x7A5CB0`) siempre;
  - solo envía `OnHitProjectileReq` (thunk `0xBF0FE0` → `.xtls 0xA477710`) y
    `OnAirFinishProjectileReq` (`0xBF14B0` → `.xtls 0xA47DEF0`) si **`byte [proyectil+0xE8] == 0`**;
  - `RemoveProjectileReq` sale de `0xBF1E80` → `.xtls 0xA4950D0` (llamadores `0x79F120`/`0x79F330`).
  - El origen de `+0xE8` no está localizado (probablemente en vf42, en `.xtls`). Interpretación:
    "proyectil que no reporta este cliente" [inferido].
- **Servidor:**
  - `CreateProjectileFromFrameEventReq` vf2 `0x2A3E000` busca al dueño en `[0x6D69698]`
    (`0x2A82370`). **Si no existe, silencio, sin Nak.** Si existe, `0x2BDBCE0` exige
    `dueño+0x90 == sesión` y valida el accessor.
  - `OnHitProjectileReq` `0x2A3A1C0` valida el dueño del proyectil y el objetivo
    (`pieza4-dano-servidor.md` §2.e).
  - `DoFrameEventReq` de tipo 1 → `+0x88` → `0x2BE0D90` (`combate-coop.md` §1.4). Un objetivo que
    no existe en el servidor se ignora en silencio [captura: 10/10 con la marioneta].

---

## 2. Caso (a): los proyectiles de los enemigos de H contra la marioneta de G

### 2.1 Qué pasa hoy [captura + log]

- Los enemigos de H apuntan y disparan a la marioneta cuando `aggro_test` la hace objetivo válido:
  13 `CreateProjectileFromFrameEventReq` con objetivo `A010F001` y **10 impactos**, todos de
  `719C2056` (arqueros). No hay datos de mosquetes contra la marioneta.
- El cliente de H detecta el impacto como con el jugador: `OnHitProjectileReq(+0x0C = A010F001)` y
  `DoFrameEventReq` de tipo 1 con la marioneta en la lista. El servidor no contesta.
- **`player_hits.cpp` ya lo releva**, porque no mira el tipo de accessor:

  | Sesión | Impactos en la marioneta | Relevados | Daño enviado |
  |---|---|---|---|
  | `111538` | 5 | 5 | 5 × "damage not learnt" → 15 HP (5 % del máximo) |
  | `114648` | 4 | 4 | 26, 15 (no aprendido), 26, 23 |

  El daño real de `719C2056` al jugador es de 24-30 HP según la variante (§1.3).
- La "clave" que usa (`0x01B01002`, `0x02B01001`…) es `dueño>>8 | índice<<24`:
  - cambia con cada flecha;
  - choca entre arqueros con claves parecidas, y de ahí salen los valores "aprendidos" por casualidad;
  - el aprendizaje con flechas que te dan a ti tampoco sirve (`learnt attack 0x01B01003 …`).

### 2.2 Qué hace falta [verificado en el código del mod]

En `hits_observe_c2s`, si `p[0x1F7] == 1`:
- clave = `variante << 32 | u32(p + 0x1F7 + 9)` (pkey);
- opcional: mezclar `a`/`b` (`+0x1F7+14`, `+0x1F7+18`).

El aprendizaje funciona igual:
- el `DoFrameEventReq` de tipo 1 con tu jugador de objetivo va a `g_pending`;
- el `VaryHPAck` trae como fuente (`+0x54`) al dueño del proyectil [captura: `B0100311` en t = 168,199].

Semillas posibles por pkey, al daño de H: `719C2056` 26, `84637121` 6, `53ADD11E` 60, `8903CF57` 59.

- El formato de `PlayerHit` no cambia: `hash` pasa a llevar la pkey, y el receptor solo la registra.
- Lo que sigue pendiente, como en la pieza 2 cuerpo a cuerpo: G no reacciona (sin
  `AttackTargetAck` en su jugador), y H no sabe si G bloqueaba o esquivaba.

---

## 3. Caso (b): las flechas de G contra copias en su PC

### 3.1 Qué pasa hoy [captura + verificado]

- 18 flechas a copias: 17 `F653A168`, 1 `E912512D`. El servidor de G las ignora y `guest_hits.cpp`
  las convierte en `GuestHit` con tipo 1.
- `act_of()` devuelve `kProjectileAct = 1` para cualquier flecha, y H aplica:
  - la base de G para "flecha", o la suya;
  - si no hay, la semilla de 28.
- Fallos:
  - una flecha `E912512D` (17) quitaría 28;
  - el aprendizaje de G mezcla las dos flechas en una sola clave, aunque gana la moda;
  - el `AttackTargetAck` de plantilla para la reacción en H es el de **cualquier** flecha.

### 3.2 Qué hace falta

- `act_of()` para tipo 1: `act_key(pkey, a << 8 | b)`, con `pkey = u32(p + 0x1F7 + 9)`,
  `a = u32(p + 0x1F7 + 14)` y `b = p[0x1F7 + 18]`.
- `guest_hits_observe_c2s` manda `hash = pkey` y `frame = a << 8 | b`. El receptor rehace la misma
  clave con `type == 1`.
- Semillas que sustituyen a `{kProjectileAct, 28}`: `F653A168/0/0` 28, `E912512D/3/1` 17,
  `E912512D/6/2` 17. Son del equipo de este jugador; con otro arco cambian.
- El emparejamiento con su `AttackTargetAck` (`on_attack_target`) no cambia: llega a ~9 ms, como en
  cuerpo a cuerpo.
- `P(AAR)` de `F653A168` es 0,96 (`pieza4-capturas.md` §2.3), así que el tambaleo nativo sirve tal
  cual.
- Detalle en H: si la plantilla del `AttackTargetAck` es de una flecha (211 B), su accessor de tipo 1
  al final nombra a **H** como dueño. Con `+0x82 = 1` no se usa
  (`pieza4-botin-reaccion.md` §2), pero es más limpio reescribirlo a la marioneta.
- **Versión:** el formato de `GuestHit` no cambia, pero sí el significado de `hash`/`frame` con
  tipo 1. Por la regla del proyecto, subir `kVersion` (5 → 6).

---

## 4. Caso (c): lo visual

### 4.1 Flechas de los enemigos de H en el PC de G (copias)

- La copia reproduce la `StartAction` del arquero y **su cliente ejecuta el frame event de proyectil**:
  - 83 Req con dueño = copia, en 9 sesiones;
  - arqueros `D1396547/20` (`719C2056`) y mosqueteros `EA461F54/4` (`84637121`),
    `754F1496`/`3D1F7CA0` (`384CAC36`) [captura].
- El servidor de G no conoce a la copia: silencio, ni Ack ni Nak [verificado `0x2A3E000` + captura].
- Pero `0x78B720` crea el proyectil local (`0x796B40` → vf42) **antes** de enviar la Req, y no la
  envía si la creación falló [verificado]. Como la Req sale, la creación local funcionó: **lo más
  probable es que la flecha ya se vea en G** [inferido].
- No hay ni un `OnHit`/`OnAirFinish`/`RemoveProjectileReq` con dueño = copia. O el proyectil no
  reporta (`+0xE8`), o se destruye sin Ack. T3 lo decide.
- Si se ve, no hace falta código. El daño a G sigue llegando por la pieza 2 desde el impacto real
  en H. La flecha visual de G puede fallar o acertar distinto (latencia), sin consecuencias.
- Si no se ve: relevar los `CreateProjectileAck`/`RemoveProjectileAck` de H del enemigo copiado
  (opción C1b, §5). Riesgo de **duplicados** con la flecha local de la copia (índices distintos).

### 4.2 Flechas de G en el PC de H (marioneta)

- La marioneta reprodujo **109** `StartActionAck` con acciones de disparo del jugador en 22 sesiones
  (23 en `095647`), y no salió ni un `CreateProjectileFromFrameEventReq` con su clave [captura].
  - Así que el frame event de proyectil no corre para la marioneta, o falla al crearlo. Hipótesis:
    las flechas del jugador dependen de su munición o equipo, y la marioneta no tiene inventario
    [inferido].
- En el MMO, los demás veían las flechas por el `CreateProjectileAck` ToNear
  (`combate-coop.md` §2.1). El handler del cliente no mira el modo de control del dueño: solo que
  esté cargado (`+0x5E == 0x10`, `+0xCF != 0`) y que tenga `ProjectileComp` [verificado `0xBF3D50`].
- **Receta** (opción C2): relevar los Ack del jugador de G a la marioneta, por la vía `Relay`
  existente (`kRelayedAcks`, con el retardo de la marioneta).

| Campo `CreateProjectileAck` | Contenido [captura, `ackkeys.py`] | En H |
|---|---|---|
| `+0x00`, `+0x1D`, `+0x35`, `+0x39`, `+0x41` | dueño = jugador de G (`+0x41` en 149/178) | `0xA010F001` (ya lo hace el `replace_u32` del receptor) |
| `+0x21` | índice del proyectil (contador por dueño) | igual: sin choques, porque solo G crea proyectiles de la marioneta |
| `+0x2D` | u64 id de evento del servidor de G (el mismo que en `AttackTargetAck +0x2D/+0x41`) | ¿reescribir a un rango propio? [inferido] |
| `+0x3D` | objetivo: copia `0xB01F…` (6), mundo de G (106) o 0 | copia → su original en H (`enemy_original_of`); otro → 0 |
| `+0xE8` | otra clave del mundo (29 de 178) | igual que `+0x3D` |
| `+0x60`, `+0x90`, `+0xC4` | salida, apuntado, quat | en un PC con desplazamiento: `shift_x` |

`RemoveProjectileAck` (81 B): `+0x00` y `+0x04` = dueño; `+0x08` índice.

Qué se espera:
- en H sale una flecha de la marioneta, sincronizada con su animación;
- si el cliente de H la cree "propia" (`+0xE8 = 0`), mandará `OnHit`/`DoFrameEventReq` con atacante
  = marioneta, que su servidor ignora (no la controla la sesión, error `0x6CF7A04`);
- las piezas 2 y 4 no los miran (atacante ni enemigo de H ni su jugador), así que no hay doble daño.

### 4.3 Defecto de identidad en los relevos de enemigos [verificado en el código del mod]

- El emisor traduce su jugador a `0xA010F001` (`enemy_relay.cpp` `translate`), y deja **tal cual**
  su propia marioneta `0xA010F001`, que representa **al receptor**:
  - `is_network_key` → `map_field_locked` no la toca;
  - `start_action_ack` conserva las claves de marioneta en `+0x16D`.
- En G, `0xA010F001` es la marioneta de H. Así que cuando un enemigo de H apunta a G, su copia en G
  apunta (y dispara) **al personaje de H**.
- Con un PC no se nota, porque emisor y receptor son el mismo personaje.
- Los 83 disparos de copias llevan objetivo `A010F001` [captura].
- Afecta a `StartAction +0x16C`, `AttackTargetAck +0x04`, `VaryHPAck +0x54`,
  `NotifyHandleDeadAck +0x04` y a cualquier relevo futuro de proyectiles (`+0x84`, `+0x3D`).

---

## 5. Enfoques candidatos

| # | Qué | Caso | Esfuerzo | Riesgo |
|---|---|---|---|---|
| **A1** | Clave de proyectil (`variante + pkey`) en `player_hits` | (a) | ~20 líneas | Nulo: solo cambia una clave del mod |
| **B1** | Clave `pkey + a/b` en `guest_hits` (emisor y receptor), semillas por pkey, `kVersion` 6 | (b) | ~40 líneas | Nulo |
| **C1a** | Nada (si T3 muestra las flechas de las copias) | (c) H→G | 0 | — |
| C1b | Relevar `Create/RemoveProjectileAck` de H del enemigo copiado (`EnemyAct`, dueño → copia, objetivo traducido) | (c) H→G | Medio | Duplicados con la flecha local de la copia; un `CreateProjectileAck` inyectado con dueño en modo 2 está sin probar |
| **C2** | Relevar los `Create/RemoveProjectileAck` del jugador a la marioneta (`kRelayedAcks` + traducción de `+0x3D`/`+0xE8`) | (c) G→H | ~80-120 líneas | Medio: tipo nuevo de inyección S→C. El handler valida dueño y tamaño (`0xBF3D50`), y los datos (`ProjectileInfo`) son iguales en los dos PC. Probar antes con eco y copia de las partidas |
| **I1** | Identidad: el emisor cambia **su** `0xA010F001` por un marcador ("el receptor") antes de `translate`, y el receptor lo cambia por `capture_own_player_key()` | (a)/(c), y relevos en general | Medio | Bajo en el juego; toca piezas 2/3/4. Revisar a quién atribuyen los relevos (`killer`, `source`) |
| D | Hookear (mid hook) el `+0xE8` o el frame event de la marioneta para que dispare sola | (c) G→H | Alto | Alto: código virtualizado (`.xtls`); descartado |

---

## 6. Plan recomendado

1. **A1 + B1** (un solo cambio):
   - `player_hits.cpp`: si `p[0x1F7] == 1`, clave = `variante << 32 | pkey`;
   - `guest_hits.cpp`: `act_of` y `GuestHit` con `pkey` y `a << 8 | b`; semillas `F653A168` 28 y
     `E912512D` 17; quitar `kProjectileAct`;
   - `protocol.h`: `kVersion` 6;
   - tests unitarios con un `DoFrameEventReq` de tipo 1 sacado de una captura.
2. **T3** (solo mirar): ¿se ven las flechas de las copias en G? Decide entre C1a y C1b.
3. **C2**, desactivado por defecto (`[coop] projectiles=0`), con:
   - entradas para 2250 (tamaño 0 = cualquiera; 289/184/165/127 medidos) y 2751 (81);
   - traducción de objetivo: copia → original con `enemy_original_of`; otra clave → 0;
   - `shift_x` de las posiciones solo con el desplazamiento de eco;
   - log de cada Ack inyectado (dueño, índice, pkey, objetivo).
4. **I1** en un cambio aparte. Es prerrequisito de una prueba real a dos PC con arqueros: sin él, las
   copias en G disparan al personaje de H.

---

## 7. Pruebas en vivo (un PC con `cdmp_peer --mode relay`)

Antes de cualquier prueba que inyecte S→C (C2, C1b): copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save`
en `backups/`.

| # | Prueba | Montaje | Predicción |
|---|---|---|---|
| T1 | A1 | Ctrl+F7 cerca de un campamento con arqueros; marioneta como objetivo válido (`aggro_test`); dejar que te den a ti una vez y luego a la marioneta | Log "learnt attack … `719C2056`" tras tu primer impacto, y luego "hit the other player's copy (… 24-30 hp)". Ningún "damage not learnt" para variantes ya vistas. La captura muestra `DoFrameEventReq` de tipo 1 → marioneta, sin respuesta |
| T2 | B1 | Copias a ≥10 m de su original; flechas normales y luego `E912512D` a una copia | "we hit the copy … (attack F653A168/0/0, our base 28.0)" y "E912512D/3/1 … 17.0". El `VaryHPAck` del original baja 28 (o 24 con defensa 0,857) y 17 (o 14) |
| T3 | Visual H→G, sin código | Copia de un arquero a ≥10 m; mirar a la copia mientras dispara; captura con `CopyFromScreen` | **Predicción:** se ve la flecha de la copia (creación local). En la captura: `CreateProjectileFromFrameEventReq` con dueño `B01F…` y sin Ack ni `RemoveProjectileReq`. Si no se ve flecha: C1b |
| T4 | C2 | `projectiles=1`; disparar flechas con el eco | En la captura: `CreateProjectileAck` S→C inyectado con dueño `A010F001` ~retardo después del tuyo, y `RemoveProjectileAck` al final. Se ve una flecha salir de la marioneta, desplazada. Sin `OnFailedToCreateProjectileReq` ni error `0x6CDB9D0` en el log de errores del juego. Quizá `OnHitProjectileReq` con dueño `A010F001` sin respuesta |
| T5 | I1 (cuando exista) | Con eco, enemigo apuntando a la marioneta | En el receptor, la `StartAction` de la copia lleva `+0x16D` = tu jugador; con eco, la copia apunta a **ti** y no a la marioneta |

---

## 8. Preguntas abiertas

- Qué pone y quién pone `byte [proyectil+0xE8]`, y si un proyectil creado por Ack con dueño
  remoto lo tiene a 1. Decide si H reporta impactos de las flechas de la marioneta (T4).
- Por qué la marioneta no ejecuta el frame event de disparo y las copias sí: ¿munición, inventario o
  tipo de actor (`[actor+0x88]+1 == 1`)?
- Si la flecha local de una copia en G se destruye sola sin Ack (T3).
- Significado de los campos `a`/`b` del accessor de tipo 1 y de la segunda clave de la Req (`+0x14`).
- Mosquetes contra la marioneta: no hay ninguno en las capturas. `384CAC36` termina casi siempre
  sin evento: ¿granada o humo, con otra ruta de daño?
- Bloqueo y esquiva de G frente a proyectiles: H aplica el daño aunque G se protegiera en su pantalla.
- Si el u64 `+0x2D` del Ack necesita reescritura para no chocar con los ids de H.

---

## 9. Anexo

### 9.1 Scripts (`build/research/p_proj/`, con `build/research/venv/Scripts/python.exe`)

| Script | Qué hace |
|---|---|
| `census.py` → `census_out.txt` | Censo de los 27 mensajes de proyectil por sesión, dueño y tamaño |
| `seq.py <sid> t0 t1` | Línea de tiempo con hexadecimal (proyectiles + golpes + HP) |
| `hits.py` → `hits_out.txt` | `OnHitProjectileReq` y `DoFrameEventReq` de tipo 1 por atacante y objetivo |
| `acc1.py` | Bytes crudos del accessor de tipo 1 |
| `accmatch.py` | Comprueba accessor de tipo 1 ↔ `CreateProjectileAck` (369/369) |
| `arrows.py` → `arrows.csv`, `arrows_out.txt` | Golpe de tipo 1 × objetivo: pkey, campos, flags, hash, daño |
| `arrows_base.py`, `headshot.py` | Base por pkey normalizada con la defensa; críticos frente al hueso |
| `pkeys.py` → `pkeys_out.txt` | pkey por variante del tirador y resultado de cada proyectil |
| `shooters.py` → `shooters_out.txt` | Acciones de disparo, marioneta frente a copias, objetivos de la Req y del Ack |
| `ackkeys.py`, `ackfmt.py` | Campos de clave y posiciones en Create/Remove Req/Ack |
| `tof.py` | Tiempos Req → Ack → impacto → Remove |
| `reqwriters.py`, `vt_proj.py`, `scan_e8.py` | Estático: escritores de opcodes, vtables de proyectil, escrituras a `+0xE8` |
| `*.asm` | Volcados: `c_bf3d50` (Ack), `c_78b720`/`c_796b40`/`c_79e3e0`/`x_970db60` (disparo), `c_81b8f0` (impacto), `c_a477710` (`OnHit`), `s_2a3e000`/`s_2bdbce0` (servidor), `x_944d0c0`/`x_9447820`/`x_9461840` |

### 9.2 Direcciones (build 1.0.0.2976, RVA)

| Dirección | Qué es |
|---|---|
| `0xBF3D50` | Cliente: `CreateProjectileAck` vf2 (dueño cargado → `ProjectileComp` → `0x794D80`) |
| `0x794D80` → `.xtls 0x944D0C0` | Cliente: crear proyectil desde el Ack (vf42 + fallo → `0xBF3BC0`) |
| `0x793660` → `.xtls 0x9447820` | `ClientProjectileActorComponent` vf42 (`+0x150`): instanciar proyectil |
| `0x7A51E0` → `.xtls 0x9461840` | `ClientProjectileActorComponent` vf45 (tras enviar la Req) |
| `0x55A1438` / `0x55AF148` / `0x5902B70` | vtables `ClientProjectileActorComponent` / `ClientProjectile_Shot` / `CommonProjectile` |
| `0x5585540`, vf1 `0x7F8AB0` → `.xtls 0x970DB60` | `ClientFrameEventProjectile` (frame event de disparo) |
| `0x79E3E0` | Crea `FrameEventProjectileCreateDesc` (vt `0x55A1638`) |
| `0x78B720` | Desc vf4: crea en local (`0x796B40`), puerta del bit 30 de la clave, envía (`0xBF31B0`) |
| `0x81B8F0` | Cliente: eventos de impacto o fin; puerta `byte [proy+0xE8]` para `OnHit` (`0xBF0FE0`) y `OnAirFinish` (`0xBF14B0`) |
| `0xBF0FE0` → `.xtls 0xA477710` | Escritor de `OnHitProjectileReq` (→ `PostPacket 0x35C670`) |
| `0xBF14B0` → `.xtls 0xA47DEF0` / `0xBF1E80` → `.xtls 0xA4950D0` | Escritores de `OnAirFinishProjectileReq` / `RemoveProjectileReq` |
| `0x2A3E000` → `0x2A82370`, `0x2BDBCE0` | Servidor: `CreateProjectileFromFrameEventReq` (dueño desconocido = silencio; `dueño+0x90 == sesión`) |
| `0x2A3A1C0` | Servidor: `OnHitProjectileReq` |
| `0x2BE0D90` | Servidor: frame event de tipo 1 (proyectil) de `DoFrameEventReq` |
| `0x5B2D468` (vf42 `0x2BD7710`) | `ServerProjectileActorComponent` (crea y envía `CreateProjectileAck`) |
