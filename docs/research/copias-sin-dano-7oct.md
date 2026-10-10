# Copias de enemigos sin daño ni reacción (prueba de dos PC, 7 oct noche)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Mod **228287A2 + cambios locales,
protocolo 9**. Solo investigación: no se ha tocado el código ni se ha ejecutado el juego.

**Lo que se vio en la prueba:**
- A los enemigos no se les puede hacer daño, y ni siquiera reaccionan, hasta que el anfitrión empieza la pelea.
- Si el anfitrión muere, los enemigos dejan de recibir daño hasta que revive.

**Fuentes:**

| Lado | Log | Captura |
|---|---|---|
| Invitado (Shadow) | log del invitado (`cdmp.log`, 7 oct, noche) | `cdmp_packets_20261007-191633.{bin,log}` |
| Anfitrión | `bin64\cdmp.1.log` | `cdmp_packets_20261007-201826.{bin,log}` |

**Relojes:**
- El reloj del anfitrión va **1 h y ~1 s** por delante del del invitado.
  - El primer golpe del invitado sale a las 19:24:19.950 (hora del invitado) y su rechazo en el anfitrión es a las 20:24:21.254.
  - Esa diferencia incluye los 200 ms de `delay_ms` y la red.
- **Captura del anfitrión:** hora = `20:18:42.640 + t`.
  - Anclaje: el primer `VaryStatCheatReq`, en t = 364,962, aparece en el log a las 20:24:47.604.
  - Los demás golpes cuadran con un margen de ±0,1 s.
- **Captura del invitado:** hora = `19:17:02.578 + t`.
  - Anclaje: su primer `DoFrameEventReq` contra una copia, en t = 437,372, aparece en el log como "we hit the copy" a las 19:24:19.950.

**Notación:**
- **H** = anfitrión; **G** = invitado.
- **P** = jugador de H, `0xA0100001`. **G'** = copia de G en H, `0xA010F001`.
- Copia de enemigo en G: `0xB01F0000+n`.

**Marcas:**
- **[hecho]**: leído en un log o en una captura, o en el código.
- **[hipótesis]**: inferido, sin prueba directa.

Los scripts usados están en `build/research/p_sindano7oct/` (§8).

---

## 0. Resumen

Son **dos causas distintas**. Ninguna es un rechazo del servidor.

1. **Antes de que H pelee, `apply_hit` descarta el golpe** con el motivo "it is not fighting either of us". **[hecho]**
   - La condición es `enemy_hostile_in_combat()` (`enemy_relay.cpp:1060`). Exige que el enemigo haya
     apuntado o pegado a P o a G' en los últimos 120 s (`g_at_us`).
   - Un enemigo tranquilo (aquí, un grupo de tres osos que deambulaba) nunca entra en `g_at_us`.
   - El descarte ocurre **antes** de inyectar el tambaleo o el `AttackTargetAck`, así que tampoco hay reacción.
   - Los osos no "vieron" a G', que estaba a 2-5 m de ellos y les pegó durante 7 s. Solo
     apuntaron a P cuando este llegó a ~10 m (`CurrentTargetChangeReq`, motivo `0x18`, 20:24:38.67).
     A partir de ahí, todos los golpes de G se aplicaron.
2. **Con H muerto, el daño se aplica mientras dura la animación de muerte (~6-11 s).** **[hecho]**
   - Después, el juego de H entra en el menú de reaparición y **se pausa entero**: el cliente y el
     servidor local del mismo proceso.
   - Durante esa pausa:
     - no sale ningún paquete C>S, y por tanto ningún `VaryStatCheatReq` inyectado;
     - casi no llega ningún S>C, así que tampoco hay tambaleo inyectado;
     - la IA y las copias de G se congelan.
   - Nuestros paquetes esperan en `g_c2s_queue` a un `PostPacket` real que no llega. A los 3 s se
     tiran (`Inject C>S: ... dropped, it waited 5734 ms`), y con 16 en cola los siguientes se
     rechazan (`(NOT sent)`).
   - El servidor **sí** acepta el daño con P muerto: hay 21 `VaryHPAck` con fuente P entre
     20:24:47 y 20:25:00, 14 de ellos con P ya muerto.

**Arreglo propuesto (detalle en §6):**
- **A**: aceptar el golpe de G sobre cualquier enemigo de zona que tenga copia, sin exigir que esté
  peleando. Además, marcarlo como peleando y hacer que apunte a G'.
- **B**: con el juego de H en pausa, no encolar nada, decírselo a G y no adelantar los remates.

---

## 1. Línea de tiempo (hora del anfitrión)

### 1.1 Antes de que H pelee: golpes rechazados

| Hora (H) | Qué pasa | Fuente |
|---|---|---|
| 20:22:30.75-31.25 | H manda las copias de los osos `0xB0100624` → `0xB01F001F`, `0xB0100623` → `0xB01F0020` y `0xB0100622` → `0xB01F0021` ("Black Bear", campamento de 3, padre `0x622`) | log H |
| 20:23:44-20:24:30 | Los osos deambulan despacio, de (-10302, -2334) a (-10279, -2286). P está parado en (-10236, -2324), a **43-51 m** de ellos. G' se acerca hasta quedar a **2-5 m** de los osos (20:24:20-20:24:30) | captura H (`MoveActorReq`/`MoveActorAck`, `pos.py`) |
| 20:22:30-20:24:38 | **Ningún** `CurrentTargetChangeReq`, `ChangeBattleStateReq` ni `DoFrameEventReq` de `0x622`/`0x623`/`0x624` | captura H (`bears.py`) |
| 19:24:19.95-19:24:29.18 (G) | G manda **34** golpes a las copias `1F`/`20`/`21`. Su log dice "we hit the copy …" para los 34 | captura y log G (`guest_dfe.py`) |
| 20:24:21.254-20:24:26.967 | H: "the other player's blow on our enemy 0xB01006xx (copy 0xB01F00xx, -1.0 m) refused: **it is not fighting either of us**" ×20. El tope del log es de 20 líneas y luego una cada 100; el resto no se registró | log H |
| 20:24:38.670-38.752 | Los tres osos: `CurrentTargetChangeReq` → **P** (`0xA0100001`), motivo `0x18`. P está a ~10 m | captura H |
| 20:24:39.647 | Primer golpe de P (`AttackTargetAck` sobre `0x624`, atacante P) | captura H |
| 20:24:47.604 → | "the other player hit our enemy 0xB0100622 … -13 hp … + stagger": **aceptado**. A partir de aquí todos se aplican | log H |

Notas:
- **[hecho]** La distancia sale `-1.0 m` en los rechazos porque `apply_hit` ni la calcula. La condición
  de combate se comprueba antes (`guest_hits.cpp:641`).
- **[hecho]** Por posiciones, G' estaba a 2-5 m del oso (límite: 15 m). La comprobación de distancia
  habría pasado.

### 1.2 Primera muerte de H

| Hora (H) | Qué pasa | Fuente |
|---|---|---|
| 20:24:53.741 | `NotifyHandleDeadAck`: P muere, matado por `0xB0100622` | captura H |
| 20:24:53.75 | `0x623`/`0x624` sueltan su objetivo (`CTC` → 0, motivo `0x02`) | captura H |
| 20:24:55.03-55.51 | Los tres pasan a G' ("now targets the other player's copy", `0x18`/`0x1D`) | log H |
| 20:24:54.06-20:25:00.88 | **14 `VaryStatCheatReq` publicados y respondidos** con su `VaryHPAck` (fuente P). El HP de `0x622` baja de 192 a 6 | captura H (`hp.py`), log H |
| 20:25:00.140 | "Puppet: this game is **paused** (menu)": el reloj del juego de H se para (`remote_puppet.cpp:2266`) | log H |
| 20:25:01-20:25:05 | **C>S = 0 paquetes/s**. S>C entre 0 y 7 por segundo | captura H (`persec.py`) |
| 20:25:01.173 | Golpe letal de G (hp 6): `VaryStatCheatReq(-5)` + colisión, **encolados** | log H |
| 20:25:01.787 | Reserva `KillCheatReq`, encolada | log H |
| 20:25:02.2-03.7 | 4 golpes letales más, encolados (VaryStat + colisión cada uno) | log H |
| 20:25:06.906 | "Inject C>S: opcode 2703/2815/2279 **dropped, it waited** 3875-5734 ms": se tiran 14 paquetes | log H |
| 20:25:06.898 | Solo pasa la última `KillCheatReq` (encolada a las 20:25:04.258; esperó 2,6 s): `NotifyHandleDeadAck` del oso con P como asesino (motivo `0x10`) | captura y log H |
| 20:25:07-20:25:11 | Recarga del mundo al reaparecer (S>C: 5.771, 19.463 y 21.563 paquetes/s). P reaparece en (-10172, -2157), a ~150 m de los osos | captura H, log H |
| 20:25:08.711 | "this game is running again" | log H |

### 1.3 Segunda muerte de H

| Hora (H) | Qué pasa | Fuente |
|---|---|---|
| 20:26:49.681 | P muere, matado por `0xB010096B` | captura H |
| 20:26:55.36-56.86 | 4 golpes de G a `0xB01009D5` aplicados: HP de 43 a 15 | log y captura H |
| 20:26:57.3-20:27:00.9 | 11 golpes más: el log dice siempre "hp 15". **C>S = 0** desde 20:26:57; los paquetes se quedan en la cola | log y captura H |
| 20:27:00.886 | "this game is paused (menu)" | log H |
| 20:27:00.896 | Golpe y `KillCheatReq` "**(NOT sent)**": la cola está llena (16) | log H |
| 20:27:00.908 | El juego manda un puñado de C>S (50 en ese segundo) y la cola se vacía: 4 tirados por viejos y 12 publicados de golpe. El enemigo queda "fatal" y muere | log y captura H |
| 20:27:01-20:27:52 | **51 s con C>S = 0 y S>C = 0**: menú de reaparición, mundo congelado | captura H |
| 19:27:00.6-19:28:30 (G) | G casi deja de golpear: 2 golpes en 90 s. Sus copias están congeladas porque H no relaya nada | captura G |
| 20:27:55.149 | "running again". P reaparece en (-10172, -2157) | log H |
| 20:28:30 → | Daño normal otra vez | log H (`Inject C>S … posted`) |

La tercera muerte (20:28:4x, pausa de 20:28:55.9 a 20:29:05.2) sigue el mismo patrón.

---

## 2. Qué hace el invitado al golpear una copia

- **[hecho]** `guest_hits_observe_c2s` (`guest_hits.cpp:520`) ve su `DoFrameEventReq` (atacante = su
  jugador, entrada de objetivo = `0xB01F…`).
- Para cada copia, encola un `GuestHit` de 17 B: copia, hash de acción, fotograma, tipo de accesor y
  base ×1000.
- **No mira nada del lado de H**: ni si el enemigo pelea, ni si H está vivo. Lo manda siempre que
  `[coop] guest_hits` esté activo.
- En G, la copia no tiene IA ni servidor que reaccione. Su reacción y su HP llegan solo por el relevo
  de H (`EnemyAct`/`EnemyState`).
- Si H descarta el golpe, G no ve nada: ni número, ni tambaleo, ni bajada de HP.
- El log de G solo registra "we hit the copy …" (las 50 primeras). No hay líneas tipo "blow", "ignored"
  ni "not a copy" en ningún lado.

## 3. Qué hace el anfitrión al recibirlo

Recorrido de un `GuestHit` en H:
1. `coop.cpp:441`: `guest_hits_on_event` (solo si `local_in_world_`, que siguió a `true` en las tres muertes).
2. Espera de `delay_ms` (200 ms) en `g_delayed`.
3. `guest_hits_take_events()` en el hilo de red.
4. `apply_hit()` (`guest_hits.cpp:620`).

Comprobaciones de `apply_hit`, en orden:

| # | Comprobación | Línea | ¿Disparó el 7 oct? |
|---|---|---|---|
| 1 | `enemy_original_of(copy)` (la copia es nuestra) | 629 | No: 0 líneas "not ours" |
| 2 | `capture_enemy_state(enemy)`: muerto o hp == 0 → se ignora **sin log** | 635 | Solo con cadáveres |
| 3 | **`enemy_hostile_in_combat(enemy)`** → "it is not fighting either of us" | **641** | **Sí: los ~34 golpes antes de 20:24:38** |
| 4 | Posición de G' y del enemigo | 650 | No |
| 5 | Distancia ≤ 15 m (80 m con flechas) + 2×offset | 652 | No |
| 6 | 50 ms entre golpes al mismo enemigo; 15 por segundo en total | 654-657 | No se registró |

`enemy_hostile_in_combat` (`enemy_relay.cpp:1060-1073`) exige, **todo** a la vez:
- que el enemigo no esté oculto por `suppress`;
- carga de tipo 3 con facción (`+0x1C` ≠ 0);
- **`g_at_us[key]` de menos de 120 s.**

`g_at_us` solo se rellena en tres casos:
- `CurrentTargetChangeReq` del enemigo hacia P o G' (`enemy_relay.cpp:1165-1175`);
- su `DoFrameEventReq` contra P o G' (`:1177-1193`);
- un `AttackTargetAck` con víctima P y atacante el enemigo (`:1090`).

Ni el golpe de P a un enemigo ni el de G lo rellenan. El de P sí rellena `g_engaged` (`:1089`), pero
esta comprobación no lo mira.

Los osos de §1.1 no hicieron ninguna de esas tres cosas hasta las 20:24:38. Por eso H descartó los
~34 golpes. **[hecho]**

Lo que se pierde con el descarte **[hecho, por el código]**:
- el `VaryStatCheatReq` (daño);
- la inyección de reacción de `guest_hits.cpp:769-799`: el lote AAR + buff de golpe con fuente G', o
  el `AttackTargetAck`;
- cualquier efecto de aggro.

Eso explica que **ni siquiera reaccionen**.

### 3.1 ¿Rechaza el servidor el daño? No

- **[hecho]** Los 21 `VaryStatCheatReq` contra `0x622` entre 20:24:47.6 y 20:25:00.9 tienen cada uno
  su `VaryHPAck` (fuente `+0x54` = P) a los 2-15 ms. HP en `+0x08` ×1000: 287 → … → 6.
- **[hecho]** 14 de ellos son posteriores a la muerte de P (20:24:53.741). El servidor no mira si
  el "atacante" (P) está vivo.
- **[hecho]** Los paquetes que se perdieron no llegaron al servidor: el propio mod los tiró en la
  cola (§4).
- **[no comprobado]** El efecto de la distancia P-enemigo en el servidor. Todos los golpes
  aceptados se dieron con P a menos de ~30 m. Según `pieza4-dano-servidor.md` (permiso de tipo 3),
  `VaryStatCheatReq` no tiene comprobación de distancia **[hipótesis]**.

### 3.2 ¿Por qué los osos no apuntaron a G'?

- **[hecho]**
  - G' estuvo a 2-5 m de los tres osos y les pegó 34 veces en 9 s sin que mandaran ningún CTC.
  - A P lo adquirieron a ~10 m (`0x18`).
  - `enemies target copy = true` en H, y la copia sí puede ser objetivo. Más tarde en la sesión:
    - los osos `0x920-0x923` y `0x936`/`0x937` apuntaron a G' con `0x18` (20:25:32-35), con P a 65-85 m;
    - y los propios `0x622-0x624` pasaron a G' al morir P (20:24:55).
- **[hipótesis]** La detección nativa de estos osos (en grupo, deambulando) no cuenta a G' como
  "jugador detectado".
  - Concuerda con `pendiente-aggro-y-respuesta-golpes.md` §2.1: la primera adquisición va a P en 11 de 14 casos, aun con G' más cerca.
  - También con que el ATA inyectado con atacante G' no hace cambiar de objetivo (§2.2 de ese documento).
- **[hipótesis]** Aunque el golpe se hubiera aplicado, el lote AAR + buff 1104/1105 podría no haber
  "despertado" su IA. Lo que mete al atacante en la lista de aggro es el buff de daño 1021, que no
  inyectamos (`pendiente-aggro` §1.1, M4).

---

## 4. Por qué no hay daño con el anfitrión muerto

### 4.1 Mecanismo [hecho, código + logs + captura]

- `sniffer_inject_c2s()` (`troc_sniffer.cpp:520-527`) no envía nada por sí misma. Encola en
  `g_c2s_queue`, con un **máximo de 16**; si está llena devuelve `false` → "(NOT sent)".
- La cola solo se vacía dentro de `on_post_packet` (`:179-201`): "Our own packets go in before this
  real one, on the same thread". Hace falta que **el juego** publique un C>S propio.
- Al vaciarla, lo que lleva más de `kMaxC2SWaitMs = 3000` ms en cola se tira (`:132-135`, `:188-190`).
- Con P muerto, la animación de muerte (~6-11 s) deja el juego corriendo. La IA local sigue mandando
  C>S, la cola se vacía y **el daño entra** (§1.2: HP de 192 a 6).
- Luego aparece el menú de reaparición y el juego **se pausa**:
  - el reloj de juego se para; es lo que mide `g_local_pause` en `puppet_take_injection`
    (`remote_puppet.cpp:2263-2267`);
  - **C>S = 0 y S>C ≈ 0** durante toda la pausa: 6 s en la primera muerte y 51 s en la segunda.
- El servidor de Crimson Desert está **en el mismo proceso** (`INFORME-CABALLO-CRASH.md`: "un
  cliente/servidor MMO dentro del propio proceso"). Con el juego en pausa, **nada** del mundo de H
  puede cambiar, por ninguna vía. **[hecho + hipótesis fuerte sobre el servidor]**
- Lo encolado en la pausa se tira a los 3 s o se rechaza con la cola llena. Algún paquete sobrevive
  y se publica de golpe al volver (los 12 de 20:27:00.908 y la `KillCheatReq` de 20:25:06.898), con
  el remate desordenado.
- Las inyecciones S>C de reacción (`puppet_inject_s2c_batch`) dependen de `on_recv_packet`. Con
  S>C = 0 tampoco salen.
- **Ni daño ni reacción.**

### 4.2 Lo que ve el invitado

- Mientras H está en pausa, el relevo de acciones de sus enemigos se para: no hay `StartActionReq`
  que relevar. En G las copias se quedan congeladas.
- G sigue pudiendo golpearlas, pero no pasa nada.
- En la segunda muerte, G dejó de intentarlo a los ~0,5 s de la pausa (19:27:00.6).
- El síntoma (dejan de recibir daño hasta que H revive) describe exactamente la pausa del menú de
  reaparición: los enemigos "vuelven" cuando H reaparece y el juego deja de estar en pausa.

### 4.3 Efectos secundarios que vale la pena arreglar a la vez [hecho, código]

- `apply_hit` se ejecuta igual durante la pausa y consume el límite de tasa y `g_last_hit`.
- Con un golpe letal, el golpe deja `g_finishing` **con la hora de pared**. La reserva `KillCheatReq`
  sale a los 600 ms (`guest_hits.cpp:593`), en plena pausa, y también se queda encolada.
- Lo encolado antes de una recarga se publica en el mundo nuevo con claves del viejo
  (`pendiente-robustez.md` §2.4).
- Los topes del log (`g_refused_log < 20`, `g_applied < 100`, `g_sent_log < 50`) ocultaron casi toda
  la sesión:
  - de 20:27:00 en adelante no hay ni un "hit our enemy";
  - de los ~34 rechazos solo se ven 20.

---

## 5. Causa raíz

1. **"Sin daño ni reacción hasta que H pelea":** el filtro de seguridad H6 de `apply_hit`
   (`guest_hits.cpp:641`) usa `enemy_hostile_in_combat()`.
   - Esa función define "hostil" como "apuntó o pegó a uno de los dos en 120 s".
   - Un enemigo tranquilo, al que G **empieza** la pelea, no cumple nunca esa condición mientras su
     IA no adquiera a alguien. Y no adquiere a G', o tarda mucho.
   - Es un círculo:
     - el golpe de G sería lo que lo metería en combate;
     - pero se descarta, incluida la reacción, por no estar en combate.
2. **"Sin daño con H muerto":** no es una regla del mod ni del servidor.
   - Es la **pausa del juego de H** en el menú de reaparición.
   - El mundo de H (cliente y servidor local) se congela, y la inyección C>S, que va a remolque de
     los `PostPacket` del juego, no tiene por dónde salir.

---

## 6. Arreglo concreto

### A. Aceptar el golpe que empieza la pelea (`enemy_relay.cpp` + `guest_hits.cpp`)

**A1. Nuevo predicado en `enemy_relay.cpp`**, junto a `enemy_hostile_in_combat`. Se declara en
`enemy_relay.h`.

```cpp
// The other player may hurt `key`: one of our zone enemies that has a copy
// on their side (they see it instead of their own, hidden one), or one that
// is fighting either of us. Never townsfolk, allies, quest NPCs or bosses:
// zone_check() leaves those out (security review H6).
bool enemy_guest_may_hit(uint32_t key) {
    if (suppress_is_hidden(key)) return false;
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    auto l = g_loads.find(key);
    if (l == g_loads.end() || l->second.size() < 0x20 || l->second[2] != 3 || u32_at(l->second.data() + 0x1C) == 0)
        return false;
    if (g_sent.count(key) && is_zone_enemy_locked(key, {g_partner_zone.caution, g_learnt_factions})) return true;
    auto a = g_at_us.find(key);
    return a != g_at_us.end() && now - a->second < kAimedMs;
}

// A blow of the other player's copy landed on `key`: it is fighting now.
void enemy_note_guest_blow(uint32_t key) {
    std::lock_guard lock(g_mutex);
    if (!g_loads.count(key)) return;
    g_engaged[key] = g_at_us[key] = GetTickCount64();
    learn_faction_locked(key);
}
```

- Hay que mover `enemy_guest_may_hit` detrás de las definiciones de `g_partner_zone`,
  `g_learnt_factions` e `is_zone_enemy_locked` (`:779-807`). `enemy_hostile_in_combat` ya está detrás
  (`:1060`).
- `ZoneRules{caution, learnt}` es el mismo predicado con el que `zone_tick` decidió mandar la copia,
  así que no se amplía nada que G no esté viendo ya.

**A2. En `apply_hit` (`guest_hits.cpp:641`):**
- cambiar `if (!enemy_hostile_in_combat(enemy))` por `if (!enemy_guest_may_hit(enemy))`, con el
  motivo de rechazo "it is not one of our zone enemies";
- después de `g_last_hit[enemy] = now;` (`:668`), llamar a `enemy_note_guest_blow(enemy)`.

Efectos:
- la reserva `KillCheatReq` de `guest_hits_take_events` (`:603`), que sigue usando
  `enemy_hostile_in_combat`, funciona con enemigos que solo ha tocado G;
- también funciona `local_capture.cpp:953`.

Las comprobaciones de distancia (15 m + 2×offset) y de tasa se quedan igual: son las que de verdad
acotan a un invitado tramposo.

**A3. Que el enemigo responda a G (aggro).**
- Al primer golpe aceptado sobre un enemigo cuyo objetivo actual no es P ni G', forzarlo hacia G':
  - generalizar `force_target_on_puppet()` (`aggro_test.cpp:336-371`) a
    `aggro_force_target(uint32_t enemy, uint32_t target, uint8_t reason)`;
  - leer el objetivo actual en `[[E+0x68]+0x90]+0x178` y la secuencia en `+0x170`, con el
    `read_target_seq` protegido por SEH que ya existe;
  - inyectar `CurrentTargetChangedAck` con `seq+1`, `force = 1` y motivo `0x18`;
  - **como mucho una vez cada 4 s por enemigo**.
- Es la regla R2 / mecanismo M2 de `pendiente-aggro-y-respuesta-golpes.md` §3.2-3.3.
- `SetCurrentTarget` ya hace `AddAggro(G')`. Con G' en la lista, `NearAggro` (`0x1D`) tiende a
  mantenerlo, porque es el más cercano.
- Alternativa o complemento: el buff 1021 con fuente G' en el lote del golpe (M4, experimental).

**Riesgos de A:**
- **Seguridad H6:** G puede quitar HP a un enemigo de zona que no le ha visto. Es lo que se quiere en
  cooperativo.
  - Siguen fuera: lo que no tiene copia, lo que `zone_check` excluye (civiles, animales sin facción,
    jefes, `MiddleBoss`, `Unique`) y lo que está a más de 15 m de G'.
  - Hay que confirmar que ningún NPC de misión hostil pasa `zone_check` (`enemy_load.cpp:154`).
- **[hipótesis]** `VaryStatCheatReq` sobre un enemigo fuera de combate debería aplicarse: el permiso
  de tipo 3 no mira la batalla (`pieza4-dano-servidor.md`). No hay ningún caso en esta captura; se
  comprueba en la prueba T1 (§7).
- **Un enemigo tranquilo puede tambalearse sin reaccionar después:** si A3 no lo engancha, recibe
  daño pero sigue deambulando. Se ve raro, pero es mejor que ser inmortal.
- **Plantillas:** el tambaleo y el ATA salen de los golpes de **P** en esa sesión (`g_attacks[...]`.
  `ata/aar/buff`, `guest_hits.cpp:685-714`).
  - Si H aún no ha golpeado nada desde que arrancó, `ata` está vacío: no hay reacción
    (`copy_here && ata.size() >= 0x30`, `:771`) y no se registra nada.
  - El 7 oct no pasó, porque H ya había peleado a las 20:20-20:22.
  - Conviene guardar una plantilla de ATA/AAR/buff en `cdmp_learnt.txt`, o al menos registrar
    "no reaction template yet".
- **CTCAck forzado:** dura poco si la IA vuelve a decidir (`pendiente-aggro` §0.4). No usar
  `seq + N` grande: bloquea la IA unos N/1,8 s.

### B. El anfitrión en el menú de muerte (pausa)

En una pausa no se puede quitar HP: el servidor local está parado. El arreglo es no estropear nada y
explicárselo a G.

**B1. `apply_hit`, al principio:**
- si `puppet_local_paused()` (ya existe, `remote_puppet.h:30`), descartar con el motivo "our game is
  paused (menu / death screen)";
- sin tocar `g_hit_rate`, `g_last_hit`, `g_cheated` ni `g_finishing`;
- con contador propio en el log de 5 s.
- Así no se encolan VaryStat ni remates que luego se tiran o se publican desordenados.

**B2. `guest_hits_take_events` (`:592`):**
- mientras `puppet_local_paused()`, no consumir los remates de `g_finishing`: `continue` sin borrar, o
  sumar a `it->second.ms` el tiempo de pausa;
- así la `KillCheatReq` de reserva no sale en plena pausa ni se tira a los 3 s.

**B3. Avisar a G:**
- H marca su `PlayerState` con un bit nuevo, `kStatePaused = 1 << 1`, en `StateFlags`
  (`protocol.h:117`). Se pone mientras `puppet_local_paused()` sea cierto;
- G:
  - deja de mandar `GuestHit`, o los manda y no espera nada;
  - muestra en el overlay "El anfitrión está en el menú: los enemigos están en pausa".
- El cable no cambia de tamaño, pero sí de significado. Por la regla del proyecto, subir `kVersion`
  a 10.
- G ya recibe `health = 0` de H y sabe que está muerto. Con el bit también distingue "muerto pero
  todavía en pie, el daño entra" de "en el menú, no entra".

**B4. `troc_sniffer.cpp`:**
- cambiar la caducidad fija de 3 s (`kMaxC2SWaitMs`) por la **época de mundo** que propone
  `pendiente-robustez.md` §2.4. Se descarta solo lo que es de otro mundo, o lo que lleva más de N s
  si no hubo pausa.
- registrar el rechazo con la cola llena, que ahora solo aparece como "(NOT sent)" en `guest_hits`.

**B5 (no recomendado ahora): impedir que el menú de muerte pause el juego en cooperativo.**
- Sería lo único que mantendría el combate vivo con H en el menú. Hay que localizar quién para el
  reloj de juego al abrir la pantalla de reaparición.
- Riesgo alto: estado de UI incoherente, el reloj de juego que ya usamos para las marcas de tiempo, y
  crashes que se envían a Pearl Abyss.
- Mejor que H reaparezca rápido, o una "gracia" en el diseño.

**Riesgos de B:**
- B1 hace que G pierda los golpes de la pausa. Hoy ya los pierde, pero de forma desordenada.
- B3 cambia el protocolo (`kVersion`) y obliga a actualizar los dos PC a la vez.

### C. Diagnóstico (barato y útil)

- Contadores por motivo en la línea de estado de 5 s: aplicados, rechazados por cada motivo,
  en pausa, "NOT sent" y tirados de la cola.
- Mejor eso que el tope de 20/100 líneas.
- Así la próxima sesión dirá cuántos golpes de G se perdieron y por qué.

---

## 7. Pruebas propuestas

Antes de cualquier prueba que escriba en memoria, copia de `save`.

| # | Montaje | Qué esperar | Decide |
|---|---|---|---|
| T1 | Con A1+A2. G pega a un enemigo de zona tranquilo, con H a más de 40 m sin pelear | En H, "hit our enemy … -N hp" desde el primer golpe; `VaryHPAck` con fuente P; HP de la copia baja en G | Que el servidor aplica el cheat fuera de combate |
| T2 | Con A3 | `CurrentTargetChangeReq` del enemigo → `0xA010F001` en ≤50 ms; ataques (`DoFrameEventReq`) contra G' | Si el CTCAck basta para engancharlo |
| T3 | Sin A3 | ¿El tambaleo solo despierta la IA? (CTC en los 2 s siguientes) | Si A3 hace falta |
| T4 | Con B1+B2: H muere y se queda 30 s en el menú mientras G pega | H: "our game is paused" ×N, **0** "dropped, it waited", 0 "(NOT sent)"; al reaparecer, ningún remate tardío | Limpieza de la pausa |
| T5 | H muere y G pega durante la animación de muerte (antes del menú) | Daño normal, como el 7 oct | Que no se rompe lo que ya funciona |
| T6 | Sesión nueva en la que G pega antes de que H haya golpeado nada | Daño sí; reacción: hoy ninguna, sin plantilla | Si hace falta guardar plantillas |

---

## 8. Scripts (`build/research/p_sindano7oct/`)

Ejecutar con `python -I`.

| Script | Uso |
|---|---|
| `cap.py` | Lector del `.bin` (formato `CDPK`) y utilidades (`targets()`, `hms()`) |
| `persec.py <log> <base_s> <t0> <t1> [nombres]` | Paquetes C>S y S>C por segundo, más los mensajes pedidos, sobre el `.log` de texto |
| `guest_dfe.py <bin> <base_s> <t0> <t1>` | `DoFrameEventReq` del invitado contra copias `0xB01F…` |
| `bears.py <bin> <base_s> <t0> <t1> <claves>` | CTC, `ChangeBattleState`, `DoFrameEventReq`, ATA, AAR y cheats de unas claves |
| `pos.py <bin> <base_s> <t0> <t1> <claves> <ops> [paso]` | Posiciones (`MoveActorReq` 2556 / `MoveActorAck` 2697, `+4` xyz) y distancias a la primera clave |
| `hp.py <bin> <base_s> <t0> <t1> <clave>` | `VaryHPAck` de una clave (HP en `+0x08` ×1000, fuente en `+0x54`) |

Bases usadas:
- anfitrión: `73122.64` (20:18:42.640);
- invitado: `69422.578` (19:17:02.578).

---

## 9. Resumen

1. Antes de que H pelee, G sí manda sus golpes: 34 `GuestHit` a las copias de tres osos.
2. H los descarta todos en `apply_hit` (`guest_hits.cpp:641`) con "it is not fighting either of us".
3. Ese filtro es `enemy_hostile_in_combat()` (`enemy_relay.cpp:1060`): exige que el enemigo haya
   apuntado o pegado a P o a G' en 120 s, y los osos no lo hicieron.
4. El descarte va antes de la inyección de tambaleo y de ATA: por eso tampoco reaccionan.
5. Los osos ignoraron a G' a 2-5 m y solo apuntaron a P al tenerlo a ~10 m (20:24:38). Desde
   entonces, todos los golpes entran.
6. El servidor nunca rechazó el daño: cada `VaryStatCheatReq` tiene su `VaryHPAck`, también con P muerto.
7. Con P muerto, el daño entra durante la animación de muerte (HP de 192 a 6).
8. En el menú de reaparición el juego de H se pausa entero (cliente + servidor local): C>S = 0 y
   S>C ≈ 0 durante 6-51 s.
9. Los `VaryStatCheatReq` inyectados esperan un `PostPacket` real que no llega. Se tiran a los 3 s o
   se rechazan con la cola llena (`troc_sniffer.cpp:135/523`).
10. Arreglo A: aceptar golpes sobre enemigos de zona con copia (`enemy_guest_may_hit`), marcarlos en
    combate (`enemy_note_guest_blow`) y forzarlos hacia G' con un CTCAck (`aggro_force_target`).
11. Arreglo B: con `puppet_local_paused()`, no encolar ni adelantar remates. Avisar a G con un bit de
    pausa en `PlayerState` (`kVersion` 10). Caducar la cola C>S por época de mundo.
12. Riesgos: H6 (revisar que `zone_check` excluye a los NPC de misión), el cheat fuera de combate
    (prueba T1) y la falta de plantillas de reacción en una sesión nueva.
