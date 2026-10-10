# Ids de enemigos: verificación contra la build 2976

Build **1.0.0.2976** (PE `0x6AB28F00`). Las 37 capturas de `D:\cdmp-grabaciones` llevan ese mismo
timestamp en la cabecera `CDPK`. Es un análisis de solo lectura: se leyeron los PAZ del juego y las
capturas, sin ejecutar el juego y sin escribir nada en `D:\SteamLibrary`.

Marcas:
- **[verificado]**: comprobado en los datos extraídos o en las capturas.
- **[inferido]**: deducido; hay que confirmarlo en vivo.

Comprueba unas afirmaciones externas sobre los ids de enemigos.

**Offsets:** salvo que se diga otra cosa, son relativos al inicio del registro de tipo 3, contando su
`u16 size`. Es el mismo origen que usa `local_capture.cpp` (`payload[2] == 3`, clave en `+3`).

---

## 0. Resumen

1. **`+0x18` es la clave de `characterinfo`** **[verificado]**:
   - las 5 332 cargas de NPC de 33 capturas la tienen en la tabla;
   - el nombre interno siempre encaja con el nombre UTF-16 ("Bleed Bandit" → `NHM_Bleed_Bandits_*`).
2. **`+0x1C` no es de `characterinfo`: es la clave de la tabla `faction`** **[verificado]**:
   - 1000016 = `Faction_BleedBlade`, 1000128 = `Faction_Bandit`, 1000131 = `Faction_Celeste_Hernand`,
     1000000 = `Faction_Graymane`, 1000006 = `Faction_GoldleafGuildhouse`;
   - vale 0 en animales y civiles;
   - el comentario de `local_capture.cpp` ("character info") está mal.
3. **El "guid" de 16 B de `factionnodespawninfo` no es un guid: es un cuaternión de rotación**
   **[verificado]**:
   - las 46 179 entradas tienen norma 1, y el 89 % vale `(0,0,0,1)`;
   - el `u32` de `+0x1C` es un `float` (radio: 1, 2, 3, 5, 10…);
   - en 2976 cada actor ocupa **66 B**, no 65: el parser de NattKh se desalinea.
4. **Aun así, el registro de carga sí trae identidad de aparición**, en un bloque que empieza en la
   **primera Transform (T0)**. T0 se localiza siempre con una firma fija (§2.1). Hay tres fuentes:

   | Fuente | Cargas | Campo (relativo a T0) | Estable entre sesiones |
   |---|---|---|---|
   | **Secuenciador** (`.paschedule`) | 3 429 (64 %) | `T0+42` u32 `hashlittle(nombre del schedule, 0xC5EDE)` + `T0+46` u32 id del elemento | **Sí** en los schedules de lugar: misma especie en 511 de 514 grupos, la misma posición exacta en 289. **No** en las plantillas `cd_seq_spawn_doc*` (275 cargas), que se instancian en muchos sitios |
   | **Nodo de facción** (`factionnodespawninfo`) | 523 (10 %) | `T0+390` u32 clave del nodo + `T0+394` u16 índice del slot | **Sí** el nodo y el slot. **El índice del actor no viaja.** **La variante (`+0x18`) se sortea en cada sesión** |
   | **Secuencia** (`.paseq`) sin schedule | 469 (9 %) | `T0+1043` u32 hash del `.paseq` + `T0+1047` u32 `0x20000000 \| id` | **Sí**: los 53 grupos vistos en 2 o más capturas tienen la misma posición exacta y la misma variante |
   | Nada | 911 (17 %) | Solo posición | No: pescadores, transeúntes, luciérnagas, bandidos en carro (aparición ambiental) |

5. **La clave de actor (`0xB01xxxxx`) no sirve como identidad** **[verificado]**:
   - cambia en cada carga, también dentro de la misma sesión: un mismo `(schedule, id)` recargado
     llega con una clave nueva;
   - el `u16` de `T0+50` (y el de `T0+1051`) es un **serial de la instancia** del schedule, distinto
     en cada sesión.

**Consecuencia para el mod** **[inferido]**:
- emparejar por `(+0x18, posición de aparición)` funciona para los enemigos de schedule;
- **falla para los de nodo de facción**, porque cada mundo sortea otra variante en el mismo punto
  (§4.3);
- la clave de emparejamiento debería ser `(tipo de origen, hash o nodo, id o slot)`, con la posición
  solo para desempatar (§6).

---

## 1. `characterinfo` (tarea 1)

### 1.1 Ruta y formato

- **En 2976 la ruta es `gamedata/characterinfo.staticinfoheader` / `.staticinfobody`**, grupo
  `0008/1.paz` **[verificado]**:
  - en el índice de los 2 052 523 ficheros del PAMT **no existe ninguna ruta `binarystaticinfo__`**;
  - el cambio a `binarystaticinfo__/bin/` que menciona la fuente externa es de una versión posterior.
- Tamaños: cabecera de 58 002 B; cuerpo de 1 250 144 B comprimido, 29 002 615 B en LZ4.
- Cabecera: `u16 n = 7250 | n × {u32 key, u32 offset}`. Cuerpo: `u32 key | u32 L | nombre UTF-8`…
  Las 7 250 claves coinciden con las del cuerpo **[verificado]**.
- Lista completa (clave, hex, índice, offset, nombre): `build/research/ids/characterinfo_keys.tsv`.

### 1.2 Ejemplos de esas afirmaciones

| Clave | Nombre en 2976 | |
|---|---|---|
| 1001770 | `Boss_Ogre_55515` | ✓ |
| 30191 | `Animal_Domestic_Bear_Domestic_30191` | ✓ |
| 1003918 | `Riding_Wolf_1001` | Coherente con "Silver Fang" |
| 1003917 | `Riding_Bear_1001` | Coherente con "White Bear" |

- El sufijo del nombre no es la clave: 1002494 = `Animal_Crow_Wild_30121`, 1001614 =
  `Animal_WaterDeer_Wild_30164` **[verificado]**.
- Hay 111 entradas con "bleed", por ejemplo `NHM_Bleed_Bandits_OneHandSword_54109` (54109) o
  `NHM_Bleed_Bandits_SwordTowerShield_54115` (1002204).

---

## 2. Campos del registro (tarea 2)

### 2.1 Disposición

| Offset | Campo | Estado |
|---|---|---|
| `+0x00` | `u16 size` | |
| `+0x02` | `u8 tipo = 3` | |
| `+0x03` | `u32` clave del actor (`0xB01…`), **nueva en cada carga** | [verificado] |
| `+0x14` | `u32` propietario (`0x90100000` casi siempre) | [verificado] |
| `+0x18` | `u32` **clave de `characterinfo`** | [verificado, 5 332/5 332] |
| `+0x1C` | `u32` **clave de `faction`** (0 = sin facción) | [verificado] |
| `+0x20` | `u32 n` + `n` caracteres UTF-16 del nombre visible | [verificado] |
| tras el nombre | ~4,9–6 KB de longitud variable: estadísticas, equipo, una ruta `character/appearance/…app_xml`… | Sin interpretar |
| **T0 − 18** | firma `0C 00 \| 8 × 00 \| FF FF FF FF` (+ 4 B, normalmente 0) | [verificado: aparece **exactamente una vez** en las 5 332 cargas y siempre precede a T0] |
| **T0** | Transform: `f32 pos[3]`, `f32 quat[4]`, `f32 scale[3] = 1,1,1` | [verificado] |
| `T0+40` | `u8 = 1`, `u8` 0 o 1 (sin identificar) | |
| `T0+42` | `u32` hash del **schedule** (0 si no hay) | [verificado] |
| `T0+46` | `u32` **id del elemento** dentro del schedule (0, 4, 8…) | [verificado] |
| `T0+50` | `u16` serial de la instancia del schedule (por sesión), `u16 0x2000` | [verificado] |
| `T0+390` | `u32` **clave de `factionnodespawninfo`** (si no hay schedule) | [verificado] |
| `T0+394` | `u16` **índice del slot** en ese nodo | [verificado] |
| `T0+1043` | `u32` hash del `.paseq` (si no hay schedule) | [verificado] |
| `T0+1047` | `u32 0x20000000 \| id` | [verificado] |
| `T0+1051` | `u32` serial (por sesión) | [verificado] |
| final | 2–3 Transforms más | La última es la "aparición" que usa hoy `local_capture.cpp` |

Notas:
- Entre `+0x1C` y el nombre **no hay nada**: el nombre empieza en `+0x20`.
- Cuando hay schedule, el bloque `.paseq` se desplaza (`T0+1043…1083`) y repite el mismo hash del
  schedule con id 0.
- **T0 frente a la última Transform:**
  - coinciden en 4 515 cargas;
  - en 817 la T0 está en **coordenadas locales**, porque el actor va enganchado a un padre: por
    ejemplo, los bandidos en carro tienen T0 = `(-0.0, 6.6, -0.16)`;
  - la última Transform es siempre la posición en el mundo.
- Las T0 a mitad de registro no tienen un offset fijo desde el nombre (5082, 4964, 5923…). Por eso
  hay que buscar la firma.

### 2.2 Tabla de ejemplos

24 cargas de 7 capturas.

| Captura | Actor | Propietario | `+0x18` | Nombre interno | `+0x1C` | Nombre | Origen (bloque T0) | Posición (última Transform) |
|---|---|---|---|---|---|---|---|---|
| 112627 | `B010006E` | `90100000` | 54138 | `NHM_Bleed_Bandits_OneHandAxe_54138` | 1000016 `Faction_BleedBlade` | Bleed Bandit | seq `cd_seq_faction_hernand_beacon_ii_0100` id 3 | (-10302.6, 639.9, -3367.9) |
| 112627 | `B0100081` | `90100000` | 30032 | `Animal_Female_Deer_Wild_30032` | 0 | Female Deer | sched `cd_seq_spawn_doc_animal_deer_extralarge_idle_01_schedule` #8 | (-10355.7, 628.6, -3410.4) |
| 112627 | `B01000AB` | `90100000` | 50051 | `NHM_Bandit_OneHandBow_50051` | 1000128 `Faction_Bandit` | Bandit | — | (-10333.2, 600.7, -3549.8) |
| 112627 | `B01000D5` | `90100000` | 32345 | `Animal_Baby_Boar_Wild_32345` | 0 | Boar Piglet | sched `cd_seq_spawn_doc_animal_bison_extralarge_idle_01_schedule` #20 | (-10373.7, 597.2, -3523.1) |
| 112627 | `B01000DE` | `90100000` | 3374 | `NHM_Citizen_Miner_3374` | 0 | Stranger | sched `cd_seq_quest_carin_mine_9000_45` #88 | (-10304.5, 638.1, -3401.8) |
| 112627 | `B01000E5` | `90100000` | 1002062 | `Animal_Insect_firefly_cluster` | 0 | Firefly Colony | — | (-10355.0, 619.5, -3475.0) |
| 112627 | `B010018D` | `90100000` | 500782 | `NHM_Wandering_Mercenary_TwoHandSpear_500782` | 0 | Wandering Freesword | — | (-10225.2, 607.5, -3548.5) |
| 112627 | `B01001C6` | `90100000` | 30194 | `Animal_Boss_Cucubird_Wild_30194` | 0 | Alpha Kuku Bird | sched `cd_m10_m04_wildanimal_cucubird_00_0003` #112 | (-10139.9, 624.7, -3412.2) |
| 112627 | `B01001C7` | `90100000` | 30156 | `Animal_KukuBird_Wild_30156` | 0 | Kuku Bird | sched `cd_m10_m04_wildanimal_cucubird_00_0003` #116 | (-10129.9, 624.7, -3419.1) |
| 112627 | `B0100200` | `90100000` | 3178 | `NHM_Soldier_Sheriff_OneHandBow_3178` | 1000131 `Faction_Celeste_Hernand` | Hernandian Guard | nodo 1000000 `Node_Her_HernandCastle` slot 9 | (-10188.5, 635.5, -3285.0) |
| 112627 | `B010030A` | `90100000` | 1002494 | `Animal_Crow_Wild_30121` | 0 | Hungry Crow | — | (-10214.6, 624.6, -3431.3) |
| 112627 | `B010086A` | `90100000` | 1001619 | `NOM_Bandit_Seotda_TwoHandWarHammer` | 0 | Gambling Den Guard | — | (-10663.6, 620.7, -3834.2) |
| 112627 | `B010087A` | `90100000` | 53217 | `NHM_Pedro_OneHandSword_53217` | 1000131 `Faction_Celeste_Hernand` | Pedro | seq `cd_seq_contents_wanted_her_0010` id 4 | (-10744.7, 622.5, -3828.1) |
| 112627 | `B01008EF` | `90100000` | 51307 | `NHM_Greyfur_OneHandBow_51307` | 1000000 `Faction_Graymane` | Greymane | sched `cd_seq_quest_crowman_boss_4000_schedule_dlc` #484 | (-10588.5, 638.2, -3775.6) |
| 112627 | `B0100968` | `90100000` | 1003940 | `Animal_Rat_Wild_30056` | 0 | Gray Rat | sched `cd_seq_quest_meetalustain_0101` #12 | (-10665.4, 605.6, -3841.9) |
| 112627 | `B0100D98` | `90100000` | 52345 | `NHM_Goblin_Market_Soldier_TwoHandSpear_52345` | 1000006 `Faction_GoldleafGuildhouse` | Shakatu's Soldier | sched `cd_seq_node_goldleafguildhouse_subinner_0003` #4 | (-10161.1, 616.9, -4714.3) |
| 115132 | `B0100090` | `90100000` | 54123 | `NHM_Bleed_Bandits_TwoHandSpear_54123` | 1000016 `Faction_BleedBlade` | Bleed Bandit | nodo 1000010 `Node_Her_EmbernestBeacon` slot 0 | (-10211.7, 617.3, -3504.6) |
| 120000 | `B0100099` | `90100000` | 54111 | `NHM_Bleed_Bandits_OneHandSword_54111` | 1000016 `Faction_BleedBlade` | Bleed Bandit | nodo 1000010 `Node_Her_EmbernestBeacon` slot 0 | (-10197.5, 624.8, -3465.5) |
| 120000 | `B01001B0` | `90100000` | 50009 | `NHM_Bandit_OneHandBow_50009` | 1000128 `Faction_Bandit` | Bandit | — | (-10333.2, 600.7, -3549.8) |
| 124539 | `B01002E7` | `90100000` | 50002 | `NHM_Bandit_OneHandDagger_50002` | 1000128 `Faction_Bandit` | Bandit | sched `cd_seq_spawn_docu_npc_extralarge_0018` #0 | (-9943.2, 567.8, -3505.4) |
| 131004 | `B010020B` | `90100000` | 3203 | `NHM_Slave_3203` | 0 | Slave | seq `cd_seq_faction_bleed_moru_slave_0300_01` id 3 | (-10124.2, 545.6, -3832.6) |
| 183530 | `B0100173` | `90100000` | 53222 | `NHM_Hernand_Soldiers_OneHandBow_53222` | 1000131 `Faction_Celeste_Hernand` | Hernandian Guard | seq `cd_seq_quest_crowman_boss_4000_dlc` id 4 | (-10602.1, 637.6, -3771.7) |
| 183530 | `B0102AE2` | `9010173F` | 1001614 | `Animal_WaterDeer_Wild_30164` | 0 | Water Deer | sched `cd_seq_spawn_docu_animal_deer_small_0001` #0 | (-10977.2, 534.8, -4372.4) |
| 200328 | `B0100146` | `90100000` | 53222 | `NHM_Hernand_Soldiers_OneHandBow_53222` | 1000131 `Faction_Celeste_Hernand` | Hernandian Guard | seq `cd_seq_quest_crowman_boss_4000_dlc` id 4 | (-10602.1, 637.6, -3771.7) |

### 2.3 Nombre visible frente a nombre interno, en todas las cargas

| Nombre UTF-16 | Familia interna de `+0x18` (número de cargas) |
|---|---|
| Bleed Bandit | `NHM_Bleed_Bandits` 376, `…_AbyssGaruntlet` 37, `NGM_Bleed_Bandits` 5 |
| Bandit | `NHM_Bandit` 113, `NGM_`/`NTM_`/`NOM_Bandit` 6 |
| Hernandian Guard | `NHM_Hernand_Soldiers` 772, `NHM`/`NDM_Soldier_Sheriff` 45, `NTM_Soldier` 19, `NHM_Unique_Guard` 8 |
| Kuku Bird / Alpha | `Animal_KukuBird_Wild` 702 / `Animal_Boss_Cucubird_Wild` 32 |
| Greymane | `NHM_Greyfur` 24, `NHM_Unique_Greyfur` 7 |
| Shakatu's Soldier | `NHM`/`NGM_Goblin_Market_Soldier` 18 |
| Wandering Freesword | `N*_Wandering_Mercenary` 432 |

Todas encajan **[verificado]**.

---

## 3. `factionnodespawninfo` y las otras tablas (tarea 3)

### 3.1 Formato en 2976

- Rutas en `0008/1.paz`: `gamedata/factionnodespawninfo.staticinfo*` y `gamedata/factionnode.*`.
- Cuerpo: 700 943 B comprimido, 3 156 408 B en LZ4.
- Cabecera: `u16 1119 | {u32 key, u32 off}`.

He escrito un lector propio (`fns.py`). Toma la **disposición** documentada por
NattKh/CrimsonDesertModdingTools `factionnode_parser.py`, pero no copia su código. Ese repositorio
**no tiene licencia**: lo trato como `reference/`, solo para consulta, y la copia está en
`build/research/ids/ref/`.

Formato:
- **Nodo:** `u32 key | u32 L | nombre | u8 bloqueado | u32 clave de factionnode | f32 min[3] | f32 max[3]
  | u32 nSlots`.
- **Slot:** 16 B de datos (`xxxxxxxx 25130009 0ffe0001 00000000`; parece un id de 8 B + relleno) |
  `u8 tiene` | `u8 flag` | `u32 nActores`.
- **Actor (66 B):**
  - `+0x00` `f32 pos[3]`;
  - `+0x0C` **`f32 quat[4]`**;
  - `+0x1C` **`f32 radio`**;
  - `+0x20` `u8 flag` (0/4/8);
  - `+0x21` `f32[3]` y `f32[3]` (casi siempre 0);
  - `+0x39` `u32` (0 o `0xC0B33333` = −5,6);
  - `+0x3D` `u32 = 0xFFFFFFFF`;
  - **`+0x41` `u8 = 0`**: el byte que falta en el parser público.

Resultado **[verificado]**:

| | Con 65 B por actor (NattKh) | Con 66 B por actor |
|---|---|---|
| Nodos | 992 bien, 127 fallan; todos los actores tras el primero son basura | **1119/1119**, con 0 bytes sobrantes |
| Slots | 198 | 1 613, en 248 nodos con actores |
| Actores | 4 342 | **46 179** |

- Los 16 B "guid" de las 46 179 entradas son cuaterniones unitarios: 40 928 valen `(0,0,0,1)`.
- **No hay ningún id por individuo en esta tabla.** La identidad natural es `(nodo, slot, índice de
  actor)`.
- Volcado completo: `build/research/ids/fns_actors.tsv`.
- La clave del registro de carga es la de **`factionnodespawninfo`** (1000000 =
  `Node_Her_HernandCastle`), no la de `factionnode` (1000044).

### 3.2 Búsqueda en las cargas

| Búsqueda | Resultado |
|---|---|
| Los 1 613 bloques de slot de 16 B (también por trozos de 4 y 8 B) | **0 apariciones** [verificado] |
| Los cuaterniones o el radio como id | No aplica: no son ids |
| **`u32 clave de nodo + u16 slot`** | En **158 de 158** cargas cuya posición coincide exactamente (bit a bit) con un único actor de la tabla, el par `(nodo, slot)` de ese actor aparece **una vez**, siempre en `T0+390` [verificado] |
| El mismo campo en todas las cargas | 523 cargas con clave válida: 345 de `Node_Her_EmbernestBeacon` (1000010) y 178 de `Node_Her_HernandCastle` (1000000); siempre con slot < nSlots [verificado] |
| El índice del actor (u8, u16 o ×4, en cualquier offset fijo desde T0+390 o desde el final) | **No está** [verificado, máximo 7 de 144 coincidencias, ruido] |
| Posiciones de las cargas en las 134 tablas `gamedata` | Solo coinciden `factionnodespawninfo` (86), `actionpointinfo` (82, todas de NPCs de schedule sentados o apoyados) y `stageinfo` (6) [verificado] |

¿La posición de carga es la del actor de la tabla? [verificado]

| | Cargas |
|---|---|
| Coincide exactamente | 161 de 523 |
| Dentro del radio del actor más cercano del slot | 126 |
| Entre 1 y 2 radios | 91 |
| Más de 2 radios (hasta 20 m) | 145 |

- Todas son la primera carga de su clave en su captura.
- **La T0 es la posición al cargar, no el punto de aparición** **[inferido]**: el NPC ya se había
  movido, o apareció con un desplazamiento dentro del radio.
- Un slot puede tener muchos actores (Embernest, slot 0: 117), así que recuperar el índice por
  cercanía es **ambiguo** si el NPC se movió.

### 3.3 `spawningpoolautospawninfo` y `terrainregionautospawninfo`

| Tabla | Entradas | Cabecera | Qué contiene |
|---|---|---|---|
| `spawningpoolautospawninfo` | 140 | `u32 count` + `{u32 key, u32 off}` | "Sockets": fruta, `FireFly_Socket`, `Roof_Crow_Socket`, `DeadBody_Socket`, `Faction_Socket`… |
| `terrainregionautospawninfo` | 134 | Igual | Biomas, `10xx_SideWalk_*` (transeúntes), `300x_Wagon_*`, `Fish_*` |

- Las claves son enteros pequeños (1–6 000) y no son individuos **[verificado]**.
- Encajan con las 911 cargas sin id (pescadores, transeúntes, luciérnagas, cuervos, bandidos en carro)
  **[inferido por los nombres]**.

### 3.4 Lo que sí identifica: los schedules del secuenciador

- El `u32` de `T0+42` es `hashlittle(nombre, 0xC5EDE)` del nombre de un fichero
  `sequencer/*.paschedule` (grupo `0014`) **[verificado]**:
  - se resolvieron los **124 de 124** hashes vistos;
  - por ejemplo, `0x1430F46F` = `cd_m11_m04_monster_beacon_01_0003` y `0xBB148509` =
    `cd_m10_m04_wildanimal_cucubird_00_0003`.
- Dentro del `.paschedule`, cada elemento empieza por `u32 hash | u32 id = 4·k | u32 k | u32 L |
  nombre…`.
  - Ejemplo: en `cd_m11_m04_monster_beacon_01_0003`, los ids van de 0 a 52 (k = 0…13).
  - Los ids vistos en las cargas son {0, 4, 8, 12, 32, 36, 40, 44, 48, 52} ⊂ esos **[verificado]**.
- `stageinfo` contiene, para 43 de esos schedules, el nombre del stage, el mismo hash y una cadena
  decimal de 64 bits (`13655826763622646017`), quizá el uuid del stage. No aparece en las cargas.
- Los guids de 16 B de los elementos del `.paschedule` (por ejemplo `63f2323f…d584`) **no aparecen**
  en las cargas [verificado].

---

## 4. Estabilidad entre sesiones y recargas

Cada captura es un arranque del juego. Comparar capturas equivale a comparar dos mundos, que es lo
que pasa entre los dos PCs **[inferido]**.

### 4.1 Schedules de lugar: identidad estable [verificado]

`(hash, id)` en 2 o más capturas, sin contar las plantillas `doc`:

| Misma posición exacta | < 1 m | < 10 m | > 10 m | Misma variante `+0x18` |
|---|---|---|---|---|
| 289 | 83 | 19 | 123 | 511 de 514 |

- Los de más de 10 m son NPCs que caminan (ciudad) y aves.
- Los 3 con otra variante son ratas de `cd_seq_quest_meetalustain_0101`.

Ejemplo, `cd_m11_m04_monster_beacon_01_0003` #32 (Bleed Bandit):

| Captura | Actor | `+0x18` | Serial | Posición |
|---|---|---|---|---|
| 112627 | `B010007C` | 54109 | 325 | (-10283.62, 640.45, -3382.56) |
| 173354 | `B010015B` | 54109 | 372 | (-10283.62, 640.45, -3382.56) |
| 174623 | `B0100175` | 54109 | 367 | (-10283.62, 640.45, -3382.56) |

Ejemplo, `cd_seq_quest_crowman_boss_0300_schedule_dlc` #480 (Hernandian Guard 53213):
- la misma posición exacta (-10576.86, 642.47, -3780.00) en 5 cargas de 3 capturas;
- incluidas recargas en la misma sesión con otra clave y otro serial (183530: `B01001AE` / 374 y
  `B01018A6` / 1616).

### 4.2 Recargas dentro de una sesión [verificado]

| Caso | Grupos |
|---|---|
| Clave nueva, mismo serial, misma posición | 75 |
| Clave nueva, serial nuevo (schedule recreado), misma posición | 407 |
| Clave nueva, mismo serial, otra posición (se movió) | 198 |
| Clave nueva, serial nuevo, otra posición | 40 |
| Variante distinta | 1 |

`(hash, id)` se mantiene; la clave del actor nunca.

### 4.3 Nodos de facción: el slot es estable, la variante no [verificado]

`Node_Her_EmbernestBeacon` (1000010), slot 0, actor 45, posición exacta (-10200.85, 622.90, -3481.05)
en 9 capturas:

| Captura | Actor | `+0x18` |
|---|---|---|
| 112627 | `B0100170` | 54122 `…OneHandBow_54122` |
| 122125 | `B01001CB` | 54109 `…OneHandSword_54109` |
| 135216 | `B0100085` | 54144 `…OneHandBow_54144` |
| 143150 | `B010009D` | 1000436 `…AbyssGaruntlet_1` |
| 145248 | `B01000AD` | 1002204 `…SwordTowerShield_54115` |
| 160139 | `B010009C` | 54118 `…OneHandDagger_54118` |
| 170612 | `B01000A0` | 54145 `…OneHandBow_54145` |
| 172953 | `B010008A` | 54120 `…OneHandDagger_54120` |
| 174623 | `B010009B` | 54119 `…OneHandDagger_54119` |

- Lo mismo en `Node_Her_HernandCastle`, slot 33, actor 1: 3178, 3178, 53222, 1000002, 53208.
- **Cada mundo sortea qué bandido o guardia sale en cada punto.**
- En los dos PCs, el "mismo" enemigo de campamento tendrá casi seguro otra `+0x18`: emparejar por tipo
  falla.

### 4.4 Plantillas `cd_seq_spawn_doc*`: no identifican [verificado]

- `cd_seq_spawn_doc_animal_deer_extralarge_idle_01_schedule` #8 aparece en 3 sitios distintos dentro
  de la misma captura (112627), cada uno con su serial (355, 480, 532).
- Lo que distingue la instancia es el serial, que cambia por sesión. Hay que añadir la posición.

### 4.5 Secuencias `.paseq` sin schedule [verificado]

- `(T0+1043 hash, T0+1047 id)` en 2 o más capturas: 53 de 53 grupos con la misma posición exacta y
  la misma variante.
- Por ejemplo `cd_minigame_duel_unarmed`, `cd_seq_quest_crowman_boss_4000_dlc` (guardias),
  `cd_seq_node_her_hernandcastle_daily_0700` (Bandit).

---

## 5. Sin id: lo que queda (tarea 4)

- **Entre `+0x1C` y el nombre no hay nada**: `+0x20` ya es el `u32 n` del nombre.
- Tras el nombre vienen ~5 KB de longitud variable, con estadísticas (`1 000 000`, `0x3B9ACA00`…),
  equipo y la ruta `character/appearance/…app_xml`. **Ningún campo de esa zona es constante por
  individuo:**
  - en los grupos de "la misma posición en varias capturas" de la clase sin id, la única ventana de 4 B
    constante y distinta entre grupos es la propia T0 (`T0−3…T0+26`), es decir, la posición y el
    cuaternión [verificado].
- En la clase sin id (911 cargas) hay posiciones fijas entre sesiones con variantes distintas:
  - puntos de pesca: Fisherman, Freesword o Porter en (-10948.3, 544.0, -4390.4);
  - bandidos en carro, con T0 local `(-0.0, 6.6, -0.16)`.
  - Son sockets o puntos de aparición ambiental: el punto es estable, el individuo no.

---

## 6. Recomendación para el mod

No he tocado el código. Todo esto es **[inferido]**.

1. **Localizar T0:**
   - buscar `0C 00 00×8 FF FF FF FF` a partir de `0x24 + 2n`;
   - T0 = posición encontrada + 18;
   - comprobar que en `T0+28` hay `(1,1,1)` y que el cuaternión es unitario;
   - leer siempre con límites, porque el registro es de longitud variable.
2. **Clave de identidad entre PCs**, en este orden:
   - `u32 [T0+42] ≠ 0` → `('S', hash, u32 [T0+46])`. Si el hash es de una plantilla `doc` (se puede
     marcar con una lista de hashes por build), añadir la posición redondeada (celda de ~4 m) de la
     última Transform.
   - Si no, `u32 [T0+390]` es una clave de `factionnodespawninfo` (lista de 1119 por build) y
     `u16 [T0+394] < nSlots` → `('F', nodo, slot)` más un **emparejamiento por cercanía**:
     - asignar dentro del slot el actor de la tabla más cercano a la posición;
     - o emparejar directamente las posiciones de los dos PCs dentro del mismo `(nodo, slot)`.
     - **No usar `+0x18` para emparejar**: difiere entre mundos.
   - Si no, `u32 [T0+1043] ≠ 0` y `[T0+1047] & 0xF0000000 == 0x20000000` →
     `('Q', hash, [T0+1047] & 0xFFFF)`.
   - Si no, sin id: `(+0x18, posición)`, como ahora, aceptando que puede no tener pareja.
3. Hay que corregir el comentario `+0x1C u32 character info` de `local_capture.cpp`: es la facción.
4. **Consecuencia de diseño:** en los campamentos de facción, el enemigo "equivalente" del otro PC
   puede ser otra variante (arco frente a lanza). Si el co-op necesita el mismo enemigo en los dos
   mundos, el anfitrión tendría que dictar la variante, o el cliente aceptar la diferencia.
5. Todos los offsets y hashes son de **2976**:
   - la firma de T0, `T0+42/46/50`, `T0+390/394` y `T0+1043/1047` se comprobaron en 5 332 cargas;
   - las claves de `factionnodespawninfo` y los nombres de schedule se regeneran con los scripts.

### Pruebas en vivo propuestas

| # | Prueba | Qué mirar |
|---|---|---|
| V-1 | Los dos PCs (anfitrión + `cdmp_peer` con dos partidas grabadas) cerca de `Node_Her_EmbernestBeacon` | ¿Coinciden `(1000010, 0)` y las posiciones T0? ¿Difiere `+0x18`? |
| V-2 | Matar un Bleed Bandit de `cd_m11_m04_monster_beacon_01_0003` y esperar el respawn | El `(hash, id)` se mantiene; ¿cambia el serial? |
| V-3 | Registrar el `u8` de `T0+41` frente a sucesos (muerte, alerta) | Significado sin identificar |

---

## 7. Archivos (`build/research/ids/`, en `.gitignore`)

| Archivo | Qué hace |
|---|---|
| `ext.py`, `extgd.py` | Extraen desde el índice PAMT de `../arrow/index.tsv` (lectores de CrimsonForge, MIT). `out/gd/` tiene las 134 tablas `gamedata` |
| `ci.py` → `characterinfo_keys.tsv` | Las 7 250 claves con su nombre interno |
| `fns.py` → `fns.pkl`, `dumpfns.py` → `fns_actors.tsv` | Lector propio de `factionnodespawninfo` (66 B por actor) |
| `grab.py` → `loads.pkl`; `recs.py` → `npc.pkl` | Opcode 2005 de las 33 capturas v2; las 5 332 cargas de tipo 3 `0xB01…` |
| `common.py` | `transforms()`, `name()`, utilidades de vecinos |
| `m22.py`, `m26.py` → `hash_names.pkl`, `stemhash.pkl` | `hashlittle(…, 0xC5EDE)` de 1,15 M nombres de fichero |
| `m9.py`, `m12.py`, `m13.py` | `(nodo, slot)` en `T0+390` |
| `m19.py`, `m20.py`, `m23.py`, `m34.py`, `m37.py` | Schedule en `T0+42`: estabilidad, serial, recargas |
| `m41.py`, `m42.py`, `m49.py` | Referencias `.paseq` en `T0+1043` |
| `m47.py` | Firma de T0, única en 5 332/5 332 |
| `table.py` → `table.md`; `stab.py` | Tablas de §2.2 y §4 |
| `ref/` | Copia de consulta de los parsers de NattKh (sin licencia; no reutilizada) |
