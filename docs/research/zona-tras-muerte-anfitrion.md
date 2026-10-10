# Zona de enemigos tras la muerte del anfitrión (prueba del 7 oct, tarde)

Build del juego 1.0.0.2976 (PE `0x6AB28F00`). Mod 0.1.0, build `6B865217`, protocolo 9, en los dos PC.
Invitado = Shadow (`[coop] suppress=1`, zona 150/170 m, `enemy relay false`).
Anfitrión = PC propio (`enemy relay true`).

Etiquetas: **[hecho]** = se ve en un log o en una captura (cito hora y línea); **[inferido]** = se
deduce de varios hechos; **[hipótesis]** = falta probarlo.

---

## 0. Resumen

1. El anfitrión murió (18:54:17 en su reloj) y, unos 15 s después, reapareció a **188 m** del invitado.
   El invitado lo tomó como un **salto** (`HostJumped`: más de 100 m en 1 s). Esa regla apaga la zona
   **y devuelve** los nativos **sea cual sea la distancia final**: devolvió 47 nativos en 35 grupos.
2. Durante los 75 s siguientes los nativos devueltos pelearon con el invitado y **lo mataron**
   (17:55:46). Al entrar en combate, el cliente pidió sus armas "on demand" (gimmicks de tipo 7).
3. Cuando el anfitrión volvió (zona ON a las 17:55:51) el barrido **sí funcionó**: mismas claves,
   21 grupos y 31 nativos retirados con `RemoveActorAck` motivo 25. Quedaron **6 grupos (8 nativos)**
   por dos reglas: 5 grupos (7 nativos) con **"holding a weapon"** y 1 cadáver. Eran exactamente los
   que habían peleado.
4. La regla del arma **no se suelta nunca en la práctica**. Los nativos salieron de combate a las
   17:55:41.409 (`ChangeBattleStateReq` estado 0), pero el servidor no retiró sus armas hasta la pantalla
   de carga de las 17:56:29. Por eso se quedaron como "fantasmas" en el invitado.
5. El anfitrión no los ve: son NPC del mundo del invitado y el invitado no releva nada
   (`enemy relay false`). Las copias del anfitrión **sí se volvieron a enviar**: 24 copias a las
   18:55:55, que coinciden en el sitio con algunos nativos del invitado, así que el invitado veía dobles.
6. Subir el apagado a 250 m (opción b) **por sí solo no habría servido**: la regla de salto apaga la
   zona igualmente. Hay que cambiar la regla de salto, y además arreglar la regla del arma.

---

## 1. Fuentes y relojes

| Fuente | Ruta |
|---|---|
| Log del invitado | `cdmp.log` del invitado (7 oct, tarde; 1.829 líneas) |
| Captura del invitado | `cdmp_packets_20261007-174707.bin` del invitado (354.325 paquetes, 544,6 s) |
| Log del anfitrión | `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\cdmp.log` |
| Captura del anfitrión | `…\bin64\cdmp_packets_20261007-184300.bin` |

**El desfase no es de 55 min 54 s, sino de ≈ +1 h 00 min 00,7 s (anfitrión = invitado + 1 h)**
[hecho]. Los 55:54 salen de comparar cuándo empezó cada log (17:46:53 y 18:42:47), pero cada juego se
arrancó en un momento distinto. Las parejas que encajan:

| Invitado | Anfitrión | Qué |
|---|---|---|
| 17:49:48.726 "the host answers request 1" | 18:49:49.417 "the guest's enemy zone ON (request 1)" | petición 1 |
| 17:54:20.845 last remote `(-10266.14, -2322.54)` hp 0 | 18:54:22.724 Local `(-10266.14, -2322.54)` hp 0 | el anfitrión muerto |
| 17:54:36.667 "zone OFF (the host travelled)" | 18:54:37.447 "the guest's enemy zone off (request 1)" | retirada |
| 17:55:51.536 "the host answers request 2" | 18:55:52.186 "the guest's enemy zone ON (request 2)" | petición 2 |

La línea del anfitrión de las 18:45:42.684 ("request 1") es anterior al arranque de este log del invitado
(17:46:53, que equivale a las 18:46:54 del anfitrión). Será de una sesión anterior del invitado
[hipótesis]: en la carpeta compartida de logs solo está este `cdmp.log`.

**Hora de las capturas** [hecho]. La columna `t` es el tiempo desde el primer paquete:
- Invitado: t0 = **17:47:31.862**. Anclas: los 24 `RemoveActorAck` inyectados del log (17:54:31.956)
  están en t = 420,094; los 21 del barrido (17:55:52.112) en t = 500,250; la ráfaga de motivo 19 en
  t = 537,176 (17:56:29.038; el log dice 17:56:29.101).
- Anfitrión: t0 = **18:43:17.133**. Ancla: `Inject C>S: opcode 2703 … posted` a las 18:54:31.365 está en
  t = 674,232.

Scripts usados (locales, no están en el repo): un lector del `.bin` con el formato de
`tools/scripts/analyze_capture.py` (`<QBBHHHII>` y luego el paquete); listados de `RemoveActorAck`
(clave y motivo); armas de tipo 7 y `OnDemandEquipDockingGimmickSpawnReq`; señales de combate; y la
lista de actores de tipo 3 cargados (nombre en la carga +0x20/+0x24, facción en +0x1C, posición por el
marcador de `enemy_load::last_transform`).

---

## 2. La secuencia, paso a paso

Las horas son del invitado. Entre corchetes va la hora del anfitrión cuando hace falta.

### 2.1 Primera zona (17:49:48 – 17:54:36)

- 17:49:48.756 `zone ON (host 5 m away, 25 natives to hide)`, seguido de 25 `removed group` (33
  nativos) [hecho, líneas 725–772].
- Hasta 17:53:48 entran 16 cargas descartadas más, y el servidor retira 2 de las guardadas
  (`0xB01009F9` y `0xB0100A2B`, "Black Bear", variante 54006, el mismo spawner que rota) [hecho,
  líneas 1064–1134].
- Estado a las 17:54:34.198: `held 47, dropped 16, swept 33, deferred 0, restored 0, restore failed 0,
  server removed while held 2` [hecho, línea 1296]. **Ningún grupo se aplazó en la primera zona**:
  los nativos estaban en reposo y sin armas.

### 2.2 Muerte y reaparición del anfitrión

| Hora (invitado) | Anfitrión | Qué pasa | Evidencia |
|---|---|---|---|
| 17:54:15.8 | 18:54:12.6 | El anfitrión tiene 94 hp; pelea con su campamento | host Local hp 94 |
| 17:54:20.8 | 18:54:17.7 | **El anfitrión, muerto** (hp 0) en `(-10266, -2322)`, a 4,5 m del invitado | invitado "last remote … hp 0.0"; host Local hp 0 |
| 17:54:21–31 | | El invitado sigue golpeando la copia `0xB01F0034` | "Piece 4: we hit the copy 0xB01F0034" |
| — | 18:54:31.4–32.5 | El anfitrión reaparece en `(-10172.66, -2156.90)`. Su servidor le retira todo: 71 claves con motivo 19, 22 con motivo 28 y 14 con motivo 45 | captura del anfitrión, t = 675,366 |
| 17:54:31.836 | | El relevo del anfitrión manda `EnemyRemove`; el invitado quita **todas** las copias (24 `RemoveActorAck` motivo 21, de `B01F0019` a `B01F0036`) | log: 20 líneas "enemy copy … removed" e "Inject: delivered opcode 2516" ×24 a las 17:54:31.956; captura t = 420,094 |
| 17:54:35.9 | 18:54:37.7 | El anfitrión ya está vivo (hp 300) a **188,4 m** del invitado | "last remote (-10172.66, …, -2156.90) hp 300.0" |
| **17:54:36.667** | 18:54:37.447 | **`zone OFF (the host travelled), restoring 47 in 35 groups`**. El invitado manda `EnemyZone{off}`; el anfitrión responde "back to copying only the enemies that fight / removing 0 enemy copies" (ya no quedaba ninguna) | log del invitado, línea 1302; host 18:54:37.447 |

**Cómo lo detectó el invitado** [hecho en el código]: `EnemyZoneMachine::jumped()`
(`src/common/enemy_zone.h`). La posición de la copia del anfitrión (`puppet_position()` menos el
desplazamiento) se movió más de `kJumpM` = 100 m en menos de `kJumpWindowMs` = 1 s, y seguía allí
`kJumpConfirmMs` = 300 ms después. En `tick()` eso es `why = HostJumped, restore = true`, **antes** de
mirar la distancia y la histéresis. Un "travelled" es eso: un salto de posición, no un viaje rápido de
verdad.

El retraso de ≈ 4–5 s entre la reaparición (18:54:32) y el salto (17:54:36.667, que son las 18:54:37.4)
será lo que tarda la copia del anfitrión en recolocarse en el invitado (el anfitrión pasa por la
reaparición, los estados llegan con 200 ms de retardo y la copia se teleporta al final) [inferido].

### 2.3 La devolución y lo que pasó después (17:54:36 – 17:55:51)

- **35 grupos, 47 nativos**, devueltos entre 17:54:36.667 y 17:54:40.391, cada uno con sus mensajes de
  estado guardados ("held 287 s; 6 messages") [hecho, 35 líneas "restored group"].
- Se reinyectan **las mismas claves**: el servidor nunca las había retirado (hecho: en el segundo
  barrido salen otra vez `0xB0100880`, `0xB010088B`…).
- Las comprobaciones a 2 s dicen `exists: state +5E 0x10 login +CF 1` para casi todos [hecho].
- **"restore failed 5" no son fallos de inicio de sesión**: el servidor retiró esos 5 él solo, con
  motivo 25, a las 17:54:41.565 (captura t = 429,703), entre 1 y 3 s después de devolverlos:
  `0xB01008DA`, `0xB01008F7`, `0xB01009D3`, `0xB0100A25`, `0xB0100A3B`. Iban junto a otras 8 claves que
  no eran nuestras [hecho]. A los 5 s, `check_logins()` no los encontró conectados, los dio por
  fallidos e inyectó otro `RemoveActorAck` (17:54:43.138–45.403, captura t = 431,276–433,541) que ya
  no hacía nada. El aviso "your enemies come back when you move away (about 250 m)" que imprime es
  engañoso en este caso.
  - Por qué los retiró el servidor: estaban a unos 80 m del invitado. El servidor manda bajas con motivo
    25 en ráfagas cada 5 s (t = 429,7 / 439,6 / 444,7 / 449,6 / 454,6…). Dos de ellos, `A25` y `A3B`,
    son del spawner de horario que rota (variante 54006). Será la limpieza periódica del servidor
    (horarios, rotación de spawns) y no una distancia [hipótesis].
- **Los nativos devueltos atacaron al invitado** [hecho, captura]:
  - `CurrentTargetChangeReq` (C>S 2984), 41 veces, de 17:54:37.833 a 17:55:34.9, de nativos que
    apuntan al invitado (`B010089E`, `B010089C`, `B010089B`, `B010088C`…).
  - `ChangeBattleStateReq` (2257) en estado 1 desde las 17:54:38.045 (`B01008B8`).
  - `DoFrameEventReq` (2380) de ataques: `B01008B8`, `B01008D0`, `B01008AC`, `B01008AD`.
  - `AttackTargetAck` (2922): el último, `B01008AD → A0100001` a las 17:55:41.406.
  - En el log del invitado, la vida baja de 300 (17:54:51) a **0 (17:55:46)**, con
    "learnt attack … of enemy type 54014 / 336434581 / 1000934".
- **Armas "on demand" pedidas durante esa pelea** [hecho, captura]:

| Nativo (grupo) | Petición C>S 2249 | Arma de tipo 7 | ¿La retiró el servidor antes de la pantalla de carga? |
|---|---|---|---|
| `0xB010089C` (grupo de 3, "Wolf Tracker") | 17:54:38.291 y 17:55:24.807 | `A86`, `ABA` (sin petición, a las 17:54:48.528) y `B4E` | `A86` motivo 48 a las 17:54:48.529; `ABA` y `B4E` **no** (motivos 9 y 45 a las 17:56:29.04) |
| `0xB01008B8` ("Black Bear") | 17:54:39.154 | `A87` | **no** (motivo 45 a las 17:56:29.04) |
| `0xB01008D0` ("Black Bear") | 17:54:39.682 | `A8A` | motivo 48 a las 17:55:37.496, tras morir |
| `0xB01008CE` ("Black Bear") | 17:54:47.609 | `AB8`, `AB9` | **no** (motivo 45 a las 17:56:29.04) |
| `0xB010089B` ("Black Bear") | 17:55:32.815 | `B80`, `B81` | **no** (motivo 45 a las 17:56:29.04) |
| `0xB010088C` ("Black Bear") | 17:55:33.433 | `B82` | **no** (motivo 45 a las 17:56:29.04) |

- `0xB01008D0` murió: `NotifyHandleDeadAck` a las 17:55:35.960, y su asesino es el invitado
  (`A0100001`) [hecho].
- **Fin del combate**: a las 17:55:41.409 el cliente manda `ChangeBattleStateReq` con **estado 0**
  para `B01008AD`, `B010088C`, `B010089B`, `B010089D`, `B010089E`… [hecho]. Aun así, **ninguna arma
  se retiró** en los 48 s siguientes.

### 2.4 El anfitrión vuelve: segunda zona (17:55:51)

- Distancias anfitrión–invitado (de las líneas `Local` y `last remote` del invitado) [hecho]:
  188 m (17:54:35.9), 165, 180, 174, 193, 193, 192 m (17:55:06), **161 m (17:55:11), 134 m
  (17:55:16)**, 92, 64, 46, 9 m (17:55:36). **En ningún momento pasó de 193 m.**
- A ≤ 150 m desde ≈ 17:55:13. `Arming` dura 2 s y, como el invitado peleaba, pasó a **`WaitFight`**
  (espera 10 s sin combate, 60 s como mucho) [inferido: no hay línea de log de ese estado, pero la
  petición 2 llegó a las 17:55:51.5, unos 10,1 s después de la última señal de combate, a las
  17:55:41.4].
- 17:55:51.550 **`zone ON (host 10 m away, 27 natives to hide)`** [hecho, línea 1580 del log]. Para entonces el invitado estaba **muerto** (hp 0 desde las 17:55:46).
- El barrido de las 17:55:51.58–51.94:
  - **21 grupos retirados (31 nativos)**: `0xB0100880` (2), `0xB010088B`, `0xB010089A`,
    `0xB01008AB` (4), `0xB01008C3`, `0xB01008C7`, `0xB01008C8` (2), `0xB01008CF`, `0xB01008D2` (2),
    `0xB01008DB`, `0xB01008F6`, `0xB01008F8`, `0xB01008F9`, `0xB01008FA`, `0xB0100900`, `0xB0100902`,
    `0xB0100995` (2), `0xB01009D7` (4), y los nuevos `0xB0100B0E`, `0xB0100B20` y `0xB0100B21`
    (cargados con la zona apagada).
  - Confirmado en la captura: t = 500,250–500,251, 21 `RemoveActorAck` **motivo 25** con esas mismas
    claves (los de 19 y 27 B son los grupos de 2 y 4) [hecho].
  - **6 grupos aplazados** (es el `deferred 6`) [hecho, líneas 1583–1594 del log]:
    - `kept 0xB010088C (group of 1): holding a weapon`
    - `kept 0xB010089B (group of 1): holding a weapon`
    - `kept 0xB010089C (group of 3): holding a weapon`
    - `kept 0xB01008B8 (group of 1): holding a weapon`
    - `kept 0xB01008CE (group of 1): holding a weapon`
    - `kept 0xB01008D0 (group of 1): dead (a corpse to loot)`
- Lista de los actores de tipo 3 que seguían en el cliente del invitado justo después del barrido
  (t = 500,5), sacada de la captura [hecho]. Los únicos **hostiles** (facción ≠ 0) eran esos 8:
  `B010088C`, `B010089B` (1000148), `B010089C`, `B010089D`, `B010089E` (1000018), `B01008B8`,
  `B01008CE` y `B01008D0` (1000148). El resto son civiles y fauna con facción 0 ("Pailune Refugee",
  "Haldin", "Female Deer", "Deer Hunter", "Wandering Freesword"…).
- **Respuesta a la pregunta 1**: los nativos **sí** se ocultaron de verdad en el segundo barrido. No
  hubo claves nuevas, la lista de grupos no estaba vacía y no se perdió nada por estar "en combate"
  (llevaban 10,1 s en calma, por encima de `kFightMs`). **Los 8 que quedaron se aplazaron por la regla
  del arma (7) y la del cadáver (1).** El barrido los vuelve a intentar cada segundo (`sweep()` a 1 Hz;
  `g_deferred_told` solo evita repetir la línea del log), pero la regla del arma depende de que llegue
  una baja del gimmick, y no llegó.

### 2.5 Final de la sesión (17:56:26 – 17:56:40)

- 17:56:26.508: 24 `RemoveActorAck` inyectados, motivo 21 (copias). El anfitrión salía del mundo
  (host 18:56:24.932 "no controlled actor") [inferido por la hora].
- 17:56:29.04: el invitado reaparece tras su muerte. Pantalla de carga, motivo 19: el servidor retira
  todo, incluidos los 8 aplazados (motivos 19 y 28) **y sus armas** (motivo 45; `ABA` con motivo 9)
  [hecho, captura t = 537,176–537,185]. `zone OFF (loading screen), nothing to give back`. El
  "server removed while held" sube de 2 a 33: son los 31 del segundo barrido [hecho, líneas
  1790 y siguientes].
- 18:56:25.032 (anfitrión) y 17:56:40.421 (invitado): `Game error: "Terminate 시작."`. Los dos juegos se
  cerraron: primero el anfitrión y después el invitado, al que se le acabó el tiempo del enlace a las
  17:56:29.838 [hecho].

Los fantasmas, por tanto, estuvieron a la vista unos **38 s** (17:55:51 – 17:56:29), con el invitado
muerto en el suelo. Si la sesión hubiera seguido, se habrían quedado mientras el servidor no retirara
sus armas. En `pendiente-3g-suprimir-enemigos-invitado.md` §1.5, 160 armas no se retiran en toda
una captura [hecho anterior].

---

## 3. Por qué el anfitrión no ve esos enemigos (pregunta 2)

- **Es lo normal** [hecho]. `0xB010088C` y los demás son NPC del servidor del invitado. Las claves de
  los dos mundos no coinciden: el mismo campamento en el anfitrión es `0xB01009EF`, `0xB01009EB`…
  El invitado tiene `enemy relay false` y no manda nada al anfitrión: 0 líneas "sending enemy" en su
  log, y 0 "copy … queued" en el del anfitrión. En la fase 5 solo releva el anfitrión.
- **Las copias del anfitrión sí se volvieron a enviar** [hecho]: a las 18:55:55.106–55.108 el
  anfitrión manda 24 copias ("zone; … 4 recent actions"), de `B01F0037` a `B01F004E`, el máximo
  `kZoneMax` = 24. El invitado las pone en cola de 17:55:54.536 a 17:56:02.5, y casi todas aparecen
  ("exists … login +CF 1"). 6 aparecen como `is NOT in the game's actor list` (`B01F0041`–`45`) y
  conviene mirarlo aparte.
- **Dobles** [hecho]: varias copias caen en el mismo sitio que un nativo aplazado, porque es el mismo
  spawn en los dos mundos. La copia `B01F003E` está en `(-10264.8, -2320.6)` y el nativo `B01008B8`,
  devuelto, en `(-10264.8, -2320.6)`. `B01F003A` está en `(-10260.3, -2344.8)` y `B010088C` en
  `(-10260.4, -2345.5)`. `B01F003B` está en `(-10260.6, -2329.1)` y `B010089B` en `(-10260.0, -2329.5)`.
  El invitado veía su nativo armado **y** la copia del gemelo; el anfitrión solo la suya.
- Por qué se perdieron las primeras copias: al reaparecer, el servidor del anfitrión le retiró todo
  (motivo 19, captura del anfitrión t = 675,366). Su relevo mandó `EnemyRemove` y el invitado borró las
  24 copias a las 17:54:31.836, **5 s antes** de apagar la zona [hecho]. Con la zona encendida, el
  relevo del anfitrión vuelve a mandar copias de los enemigos que su servidor cargue a ≤ 150 m de él
  (`zone_candidates()` en `enemy_relay.cpp`, medido desde **el anfitrión**) [hecho en el código].

---

## 4. Causas

| # | Causa | Tipo |
|---|---|---|
| C1 | `EnemyZoneMachine::tick()`: un `HostJumped` apaga la zona con devolución **aunque el anfitrión caiga dentro de `off_m`**. La reaparición fue a 188 m, y con 250 m de histéresis también se habría apagado | hecho (código y log) |
| C2 | Al devolver junto al invitado, los nativos lo atacan. Así sacan armas, quedan marcados como "armados" y llegan a matarlo | hecho (captura) |
| C3 | `sweep()`: `armed.count(k)` → "holding a weapon". `g_weapon_of` solo se limpia con un `RemoveActorAck` del gimmick o del NPC. El servidor no retira las armas al acabar el combate (estado 0 a las 17:55:41.409; ninguna baja hasta las 17:56:29). **El aplazamiento no tiene fin** | hecho |
| C4 | `check_logins()` cuenta como "restore failed" (y vuelve a retirar) claves que **el servidor** retiró después de devolverlas | hecho, solo cosmético |
| C5 | `suppress_observe_s2c()` (tipo 7): borra `g_arming` con la primera arma, así que la segunda (`AB9`, `B81`) o un arma sin petición (`ABA`) no se apuntan. Aquí no cambió nada, pero hace que `g_weapon_of` esté incompleto para la nueva regla de §5.3 | hecho en el código |

---

## 5. Diseño del arreglo

### 5.1 Las tres opciones, contrastadas con los datos

**(a) La muerte o reaparición del anfitrión como "pausa"** (mantener la zona un tiempo de gracia).
- Aquí habría bastado: el anfitrión volvió a ≤ 150 m 38 s después del salto (17:54:36.7 → ~17:55:14).
  Con 60 s de gracia no se habría devuelto nada, el invitado no habría peleado con sus nativos, no habría
  armas y no habría fantasmas.
- Se puede detectar: `ClientSession::remote_state()->health` llega en cada `PlayerState` (el invitado
  vio "hp 0.0" de 17:54:20.8 a 17:54:30.9). **En el momento del salto ya tenía 300 hp**, así que la
  condición tiene que ser "estuvo muerto hace poco" (≤ 30 s), no "está muerto ahora".
- Coste: durante la pausa, la zona del invitado alrededor de sí mismo se queda vacía (sus nativos
  ocultos y el anfitrión sin copias allí). Las copias del anfitrión aparecen alrededor del anfitrión, que
  es lo correcto.
- No necesita protocolo nuevo: el invitado simplemente no manda `EnemyZone{off}`, y el anfitrión sigue
  relevando alrededor de sí mismo.

**(b) Histéresis mayor (encender a 150 m, apagar a 250 m).**
- **Sola no basta**: C1 apaga por salto antes de mirar la distancia. Tiene que ir con "un salto que
  cae dentro de `off_m` no apaga".
- Con eso, aquí no se habría apagado nunca (máximo, 193 m).
- Ya se puede configurar: `config.cpp` acepta `suppress_off_m` hasta `radius + 100` = 250 m.
- Coste: entre 150 y 250 m el invitado ve su alrededor vacío. Es el precio de no perder la zona.
- Un viaje de verdad (por ejemplo +400 m, como en el test de `tests.cpp` de la línea 1179) sigue
  apagando.

**(c) Al volver a encender, barrer TODOS los nativos hostiles.**
- El barrido **ya** barre los devueltos (mismas claves). Lo que falta es soltar la regla del arma (C3).
- Riesgo de borrar un enemigo que está peleando con el invitado: la regla de 10 s (`g_fought`) lo
  cubre, y se puede reforzar con el estado de combate (`ChangeBattleStateReq`).
- Riesgo de armas flotando (§1.3–1.5 de los documentos de fase 5, [inferido], sin probar): se evita
  retirando también sus gimmicks de tipo 7 en el mismo barrido. **El cliente acepta `RemoveActorAck`
  de gimmicks**: el servidor los retira así con motivo 45 [hecho, 17:56:29.04].
- Al devolverlos, no hace falta devolver el arma: **el servidor da otra en cada petición aunque ya
  exista una**. `B010089C` tenía viva `ABA` y a las 17:55:24.807 pidió otra y recibió `B4E` en 33 ms
  [hecho]. Aun así, hay que confirmarlo en el laboratorio (prueba L1c de la fase 5).

### 5.2 Recomendación: (a) + (b) + (c), en este orden de importancia

1. **Pausa por reaparición y saltos cortos** (C1). Es lo que evita el problema de raíz.
2. **Apagar a 250 m** (b), como red de seguridad para reapariciones cercanas y paseos.
3. **Regla del arma por estado de combate, con las armas incluidas en el barrido** (C3). Esto evita los
   fantasmas siempre que se devuelva (enlace caído, viaje de verdad, fin de la gracia).
4. Arreglos menores: C4 y C5.

Nada cambia el formato del protocolo: **`kVersion` se queda igual**.

### 5.3 Cambios exactos

#### `src/common/enemy_zone.h` (máquina de estados)

- `Settings`: añadir `uint64_t respawn_grace_ms = 60000;` y cambiar `off_m` por defecto a `250.0f`.
- Constante nueva: `static constexpr uint64_t kRespawnWindowMs = 30000;` (un salto hasta 30 s después de
  ver al anfitrión muerto cuenta como reaparición).
- `Input`: añadir `bool host_dead = false;` (el `PlayerState` del anfitrión tiene `health <= 0` y
  `health_max > 0`).
- `Step`: añadir `bool paused = false;` (empieza la gracia) y `bool resumed = false;` (el anfitrión
  vuelve dentro de `off_m`), solo para el log y el HUD.
- Miembros nuevos: `uint64_t host_dead_ms_ = 0, grace_until_ms_ = 0;` y
  `uint64_t grace_until() const { return grace_until_ms_; }`.
- En `tick()`:
  ```cpp
  if (in.host_dead) host_dead_ms_ = in.now_ms;
  ...
  const bool respawn = host_dead_ms_ && in.now_ms - host_dead_ms_ <= kRespawnWindowMs;
  const bool in_grace = state_ == State::On && in.now_ms < grace_until_ms_;
  ...
  else if (jumped == Why::HostJumped && state_ == State::On && (respawn || (ok && d <= s_.off_m))) {
      // The host died and reappeared, or jumped but stays near: keep the zone (no give-back).
      if (respawn) { grace_until_ms_ = in.now_ms + s_.respawn_grace_ms; st.paused = true; }
  }
  else if (jumped != Why::None) why = jumped, restore = jumped != Why::YouJumped;
  else if (!in.host && in.now_ms - host_seen_ms_ > kHostGoneMs && !in_grace) why = Why::HostGone;
  ...
  if (ok && d <= s_.off_m) { far_since_ms_ = 0; if (grace_until_ms_) { grace_until_ms_ = 0; st.resumed = true; } }
  ...
  if (why == Why::None && far_since_ms_ && in.now_ms - far_since_ms_ >= kOffAfterMs &&
      in.now_ms >= grace_until_ms_) why = Why::Far;   // Far, with restore, when the grace ends
  ```
  En `enter()` y `off()`, poner `grace_until_ms_ = 0`. `LoadingScreen`, `YouJumped`, `NotInWorld`,
  `LinkLost` y `HostStopped` no cambian: la gracia no los pospone.
- Actualizar el comentario de la cabecera (el diagrama de `On → Off`).

#### `src/mod/net/native_suppress.h` / `.cpp`

- `ZoneInput`: añadir `bool host_dead = false;`. `SuppressSettings`: `off_m = 250.0f` y
  `uint32_t respawn_grace_s = 60;`.
- `suppress_configure()`: pasar `respawn_grace_s * 1000` a `EnemyZoneMachine::Settings`, y añadirlo a la
  línea del log de configuración.
- `suppress_tick()`: `z.host_dead = in.host_dead;`.
- `apply()`:
  - Con `st.paused`: `log::info("Suppress: the host died and reappeared {:.0f} m away: zone kept {} s
    (your enemies stay hidden)", …)`.
  - Con `st.resumed`: "the host is back: zone continues".
  - Si la gracia acaba en `Far`, la salida ya hace la devolución (`st.restore`).
  - Durante la gracia, el centro (`g_center_x/z`) sigue al anfitrión. Así, `decide()` y `sweep()`
    retiran los nativos del invitado que haya junto al punto de reaparición, que es donde el anfitrión
    releva sus copias (no hay dobles allí). Los ya guardados cerca del invitado siguen guardados, porque
    no se devuelve nada mientras dura `On`.
- `update_hud()`: con `g_zone.on() && now < g_zone.grace_until()`, mostrar
  `"Zona en pausa: el anfitrión vuelve ({} s)"`.
- **Regla del arma** (C3), en `suppress_observe_c2s()`, `ChangeBattleStateReq`:
  - Guardar **también el estado 0**: `g_battle[npc] = {state, now}`, un `std::map<uint32_t,
    std::pair<uint8_t,uint64_t>>` con tope `kMaxSmallSet`.
  - Si el estado es ≠ 0, actualizar `g_fought` como ahora. Si es 0, solo `g_battle`.
  - Borrarlo en `RemoveActorAck` junto a `g_fought`.
- En `suppress_observe_s2c()`, `ActorLoadDataAck` de tipo 7 (C5): no hacer `g_arming.erase(a)` con la
  primera arma, y apuntar todas las que lleguen dentro de `kArmingMs`. Además, si la carga nombra una
  clave que esté en `g_natives`, apuntarla aunque no haya petición (caso `ABA`). Para eso, cambiar
  `g_weapon_of` a "gimmick → nativo", que ya lo es, y buscar los gimmicks por nativo al barrer.
- `sweep()`: cambiar la línea `else if (armed.count(k)) why = "holding a weapon";` por:
  ```cpp
  else if (armed.count(k) && fighting_or_fresh(k, now)) why = "holding a weapon in a fight";
  ```
  - `fighting_or_fresh` es verdadero si el último `ChangeBattleStateReq` de `k` es ≠ 0, si el último
    estado 0 tiene menos de `kFightMs` (10 s), o si no se ha visto ningún estado y el arma tiene menos
    de 30 s.
  - En este caso, todos los aplazados por el arma se habrían barrido a las 17:55:51.4, 10 s después del
    estado 0 de las 17:55:41.409.
  - Si un grupo armado se barre, después del `RemoveActorAck` del grupo (motivo 25) se inyecta **otro**
    `RemoveActorAck` con las claves de sus gimmicks y **motivo 45**. Es el mismo orden que usa el
    servidor: el NPC y luego sus armas con motivo 45. Se borran de `g_weapon_of` y el log lo dice
    ("… and its 2 weapons").
  - No se guardan las armas para devolverlas: el cliente las vuelve a pedir en el siguiente combate y el
    servidor las da (§5.1 c). Si la prueba muestra que un NPC devuelto pelea sin arma, guardar la carga
    de tipo 7 en `Held` y reinyectarla detrás del NPC en `restore_tick()`.
- `kept … dead (a corpse to loot)` se queda igual (decisión de diseño de §4.4).
- `check_logins()` (C4): antes de contar `restore_failed`, si `removed_lately(k)` vale para alguna clave
  del grupo, contarlo como `server_removed` y **no** volver a inyectar ni mostrar el aviso.

#### `src/mod/net/coop.cpp` — `CoopLink::update_enemy_zone()`

```cpp
if (const auto& rs = session_.remote_state()) in.host_dead = rs->health_max > 0 && rs->health <= 0.0f;
```
Va junto a `in.host`. Así solo se usa información que ya llega (`PlayerState`), y no hay protocolo
nuevo.

#### Configuración y kits (no tocados en esta investigación)

- `src/mod/config.cpp`: el valor por defecto de `suppress_off_m` pasa a `radius + 100`. Añadir
  `suppress_respawn_grace_s` (0–180, 60 por defecto; 0 = comportamiento actual).
- `kit/coop/cdmp.ini.invitado`: `suppress_off_m=250` y `suppress_respawn_grace_s=60`, con un comentario
  en español.

### 5.4 Riesgos

| Riesgo | Mitigación |
|---|---|
| El anfitrión reaparece lejos (por ejemplo a 1 km) y no vuelve: el invitado pasa 60 s sin enemigos | Se acepta. La gracia se puede configurar; al acabar se devuelve todo (`Far`) |
| El anfitrión muere y el invitado sigue peleando con copias que ya no existen (el servidor del anfitrión retiró todo) | Pasa igual sin el arreglo (17:54:31.8). Las copias vuelven cuando el servidor del anfitrión las recarga a ≤ 150 m de él |
| Zona de 150–250 m vacía para el invitado | Es el precio de la histéresis. El HUD lo explica |
| Barrer un nativo armado que va a atacar | Solo se barre tras 10 s en estado 0 y sin señal de combate (`g_fought`) |
| Armas sueltas al barrer | Se retiran sus gimmicks (motivo 45). Mapeo más completo (C5) |
| Un NPC devuelto sin arma | Según la captura, el servidor da otra al pedirla. Si no, devolver la carga de tipo 7 |
| Un muerto + salto del anfitrión mientras el **invitado** también está muerto o cargando | `LoadingScreen` y `NotInWorld` siguen ganando (apagado sin devolución) |

### 5.5 Pruebas

- **Unitarias** (`tests/tests.cpp`, junto a los tests de salto de las líneas 1162–1189):
  1. Con la zona encendida, `host_dead` durante 5 s y luego un salto a 190 m: no apaga y `st.paused`.
  2. Sigue a 190 m durante 59 s: no apaga. A los 60 s: `turned_off && restore && why == Far`.
  3. Vuelve a ≤ 250 m antes de los 60 s: `st.resumed`. Si luego se aleja, apaga a los 5 s normales.
  4. Salto con el anfitrión vivo a +200 m (con `off_m` 250): no apaga. A +400 m: `HostJumped` con
     devolución (el test actual sigue valiendo).
  5. Durante la gracia, una pantalla de carga del invitado: `LoadingScreen` sin devolución.
  6. Durante la gracia, falta la copia del anfitrión 3 s: no apaga.
- **Sin juego**: `cdmp_peer` + `cdmp_host` (la zona solo funciona en prueba en seco por eco), para ver que
  compila y que los logs salen.
- **Dos PC (Shadow de invitado)**:
  1. Zona encendida en un campamento. El anfitrión se deja matar y reaparece. Comprobar en el invitado
     "zone kept 60 s" y que no hay "restoring".
  2. Repetir con el anfitrión volviendo en más de 60 s: devolución, combate y reencendido. Comprobar que
     los "holding a weapon in a fight" se barren unos 10 s después del estado 0, con "and its N weapons",
     sin armas flotando (captura de pantalla).
  3. Leer la captura: `RemoveActorAck` 25 del NPC y luego 45 de sus gimmicks. En el siguiente combate de
     ese NPC devuelto, comprobar que hay `OnDemandEquipDockingGimmickSpawnReq` y una carga de tipo 7.

---

## 6. Hechos e hipótesis

**Hechos**
- La zona se apagó por `HostJumped` 5 s después de la reaparición, con el anfitrión a 188 m. Se
  devolvieron 47 nativos en 35 grupos.
- Los 5 "restore failed" los retiró el servidor (motivo 25, 17:54:41.565) después de devolverlos.
- Los nativos devueltos pelearon con el invitado y lo mataron. Pidieron armas de tipo 7 que el servidor
  no retiró al acabar el combate (estado 0 a las 17:55:41.409); solo con la pantalla de carga de las
  17:56:29.04 (motivo 45).
- En la segunda zona se barrieron 31 nativos (motivo 25, mismas claves) y quedaron 8: 7 por "holding a
  weapon" y 1 cadáver. Eran los únicos hostiles cargados en la zona.
- El anfitrión no los ve porque son de otro mundo y el invitado no releva nada. Las copias del anfitrión
  volvieron (24 a las 18:55:55) y algunas se solapan con nativos aplazados.
- Con la regla actual, 250 m no habría evitado el apagado.

**Inferencias e hipótesis**
- El retraso de 4–5 s del salto es la recolocación de la copia [inferido].
- `WaitFight` retrasó el encendido 38 s [inferido por las distancias y los 10,1 s de calma].
- Por qué el servidor retiró con motivo 25 los 5 recién devueltos (limpieza periódica cada 5 s, spawner
  de horario) [hipótesis].
- Retirar los gimmicks con `RemoveActorAck` motivo 45 evita armas flotando, y el NPC devuelto recibe un
  arma nueva al pedirla [hipótesis apoyada en la captura; falta la prueba L1c].
- La petición 1 de las 18:45:42 del anfitrión es de un proceso anterior del invitado [hipótesis].
