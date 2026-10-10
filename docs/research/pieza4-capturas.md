# Pieza 4 (el invitado golpea a los enemigos del anfitrión): lo que dicen las capturas

Build **1.0.0.2976** (PE `0x6AB28F00`). Análisis de solo lectura de 47 capturas del sniffer:
- las 14 de hoy (`bin64\cdmp_packets_20261004-*`), con copias de enemigos;
- las 33 de `D:\cdmp-grabaciones` (3-10-2026), entre ellas la sesión real a dos jugadores
  `20261003-183530`.

`20261003-105149` está truncada a 1 KB por paquete y no entra en los cálculos de objetivos ni de daño.
El juego no se ejecutó.

Marcas: **[captura]** = medido en las capturas; **[inferido]** = deducción que falta probar en vivo.

Notación:
- **G** = invitado (el que golpea copias);
- **H** = anfitrión (dueño real del enemigo);
- copia = `0xB01F0000+n`; jugador = `0xA0100001` (`0xA0101740` tras el relogin de 183530);
- marioneta = `0xA010F001`.

Scripts y salidas en `build/research/pieza4/` (§7).

Sesión ↔ log del mod (las horas de arranque coinciden):

| Captura | Log | Captura | Log |
|---|---|---|---|
| 064320 | `cdmp_aggro_test1` | 081450 | `cdmp_enemy_auto` |
| 065037 | `cdmp_aggro_test2` | 082648 | `cdmp_enemy_3c` |
| 072727 | `cdmp_enemy_copy_test1` | 084001 | `cdmp_finisher_blink` |
| 073323 | `cdmp_enemy_copy_test2` | 090008 | `cdmp_invisible_enemies` |
| 074207 | `cdmp_enemy_copy_crash3` | 090931 | `cdmp_invisible2` |
| 075200 | `cdmp_enemy_copy_ok` | 095647 | `cdmp_sanitized_test` |
| 080253 | `cdmp_enemy_3b` | 101713 | `cdmp_hits_test1` |

---

## 0. Resumen

1. **Los golpes a copias salen solos y el servidor los ignora en silencio.**
   - El cliente de G manda un `DoFrameEventReq` normal con la copia en la lista de objetivos:
     **74 golpes** en 7 sesiones (56 cuerpo a cuerpo, 18 flechas) [captura].
   - La respuesta del servidor es **ninguna**: ni Ack de combate, ni Nak (`1013`), ni error [captura].
   - Si el mismo golpe alcanza además a enemigos reales, a esos sí los procesa con normalidad
     (17 golpes mixtos) [captura].
   - Cada golpe trae todo lo que la pieza 4 necesita:
     - clave de la copia;
     - accessor: hash de la acción + índice del frame;
     - hueso y punto de impacto;
     - MoveData de la copia.
2. **El daño es casi determinista.**
   - Por golpe limpio, `daño = base[hash de acción + frame] × defensa[variante]` acierta con
     ±1 HP el **83 %** de los golpes [captura].
   - La defensa tiene prácticamente dos clases: **1,0** y **≈0,86**.
   - Ruido medido:
     - críticos ×2 (≈4 %);
     - golpes bloqueados (flags `84`, 7-20 % del daño, 11 % de los golpes);
     - frames que nunca hacen daño (flags `E3`, fijos por frame, 18 %).
   - El jugador solo usa **27 claves de acción/frame** en todas las capturas.
   - Una tabla persistente y sembrada conoce la acción en el 66-79 % de los golpes desde el primero.
     Reiniciada en cada sesión, conoce el 9 % al empezar y el 73 % pasado el golpe 80.
3. **En la sesión real a dos (183530), el cliente de H no produjo nada cuando el amigo luchaba.**
   - La marioneta hizo 2.899 acciones, 61 de ellas ataques conocidos.
   - Aun así: 0 `DoFrameEventReq` con la marioneta de atacante, 0 listas de objetivos con la
     marioneta y 0 `CurrentTargetChangeReq` hacia ella.
   - Su HP relevado solo sube (regeneración) [captura].
4. **Reacción del servidor a un golpe real** (≈9 ms después), en este orden:
   `AttackTargetAck` → `ApplyActionRestrictionAck` → `ApplyBuffAck`(objetivo) → `VaryHPAck` →
   `OnPostBuffDamagedAck` → `AttackerAck` → `VarySharpnessAck`.
   - Al morir: `NotifyHandleDeadAck` + `LootDrop…` + `UnapplyBuff`×4-8 + `ClearAllyGroupDataAck`,
     y `RemoveActorAck` ~30 s después.
   - Para que el golpe de G se vea y pese en H basta con:
     - **HP real en el servidor de H** (`VaryStatByAccInputReq`);
     - **`AttackTargetAck` + `ApplyActionRestrictionAck` inyectados** con atacante = marioneta.

     Con eso, el cliente de H lanza solo la reacción del enemigo, como hace con cualquier golpe
     [captura + inferido].
5. **Aggro:**
   - El 95 % de los enemigos ya apuntaba al jugador antes del primer golpe [captura].
   - Cuando apuntaban a otro NPC o a nadie, cambiaron al atacante a los **26-30 ms** (motivo
     `0x18`, 7 de 8 casos) [captura].
   - El único que apuntaba a la marioneta tardó 9,7 s y no cambió por el golpe [captura].
   - Un objetivo se mantiene una mediana de 4-9 s.
   - Diseño: cambio inmediato si el enemigo no apunta a un jugador; si apunta al jugador de H,
     cambio por amenaza con histéresis de ≥5 s (§5).

---

## 1. Golpes a las copias de enemigos

### 1.1 Cuántos y dónde [captura]

`q1_table.py`, `q1_survey.py`. "Golpe" = `DoFrameEventReq` C>S con atacante `0xA0100001` y la copia
en la lista de objetivos (entradas de 665 B al final; ver `combate-coop.md` §1.4).

| Sesión (log) | Copias creadas | Golpes a copia (cuerpo a cuerpo/flecha) | Solo copia | + su original | + otros reales | Golpes a enemigos reales |
|---|---|---|---|---|---|---|
| 075200 (`enemy_copy_ok`) | 9 | 3 (3/0) | 2 | 0 | 1 | 26 |
| 081450 (`enemy_auto`) | 28 | 4 (4/0) | 3 | 0 | 1 | 43 |
| 082648 (`enemy_3c`) | 18 | 8 (8/0) | 4 | 1 | 3 | 75 |
| 084001 (`finisher_blink`) | 67 | 16 (11/5) | 12 | 0 | 4 | 66 |
| 090008 (`invisible_enemies`) | 35 | 1 (0/1) | 1 | 0 | 0 | 102 |
| 090931 (`invisible2`) | 11 | 1 (1/0) | 0 | 0 | 1 | 30 |
| 095647 (`sanitized_test`) | 20 | 41 (29/12) | 35 | 2 | 4 | 143 |
| **Total** | 188 | **74 (56/18)** | **57** | **3** | **14** | 485 |

- Hay 77 entradas de objetivo con copia, porque 3 golpes alcanzaron a dos copias.
- En 072727, 073323, 074207 y 080253 hubo copias, pero el jugador no les pegó.
- Las sesiones de antes de hoy no tienen copias.
- Las flechas usan el accessor de tipo 1:
  - `u8 1, u8 1`, y luego un u32 que es un contador (`01A01000`, `06A01000`…), no un hash;
  - el paquete mide 1.319 B (1.307 + 12).

  La tabla de daño no puede usar ese campo como clave para las flechas (§2.4).
- En las listas mixtas, las claves `0xB01F…` son las mayores y quedan al final. Así se cumple el
  orden creciente que exige el servidor, que sí procesa a los objetivos reales de esos golpes
  (los 485 golpes con algún objetivo real recibieron `AttackTargetAck`) [captura].

### 1.2 Qué más envía el cliente de G alrededor [captura]

`q1_copyhits.py` (salida completa en `out_q1_copyhits.txt`), `q1_side.py`, `q1_copykeys.py`.

| Mensaje C>S | Relación con la copia | Recuento |
|---|---|---|
| `StartActionReq` del jugador con `+0x16C` (objetivo fijado) = copia | En 41 de los 74 golpes, el último `StartActionReq` del jugador apuntaba a **esa** copia (3 a otra copia, 30 a un enemigo real) | 299 en total, en 9 sesiones |
| `OnHitProjectileReq` (`+0x0C` objetivo) | Uno por cada flecha que acierta a una copia, justo antes del `DoFrameEventReq` de tipo 1 | 18 del jugador (+1 de un arquero enemigo `0xB01001C6` sobre una copia) |
| `AttackByCollisionReq` | **Ninguno** del jugador. 7 de un enemigo real (`0xB0100AFE`, 084001, t = 454-455 s) contra copias, sin respuesta | 7 |
| `CurrentTargetChangeReq` hacia una copia | Todos de **enemigos reales del mundo local** que pasan a apuntar a una copia (motivo `0x18`). El jugador nunca envía CTC | 45 |
| `EchoActionSessionIDReq` de la copia | Respuesta del cliente a los `AttackTargetAck`/`ApplyActionRestrictionAck`/`VaryHPAck` **inyectados** por el relevo (§4.3) | 120 |
| `SetQuestDialogReq` (4 B = la clave) | El cliente informa del NPC con el que se podría interactuar | 5.882 |
| `GimmickExecuteFrameEventReq`/`UpdateGimmickReq` (`+20`), `GamePlayTriggerInOutReq` (`+152`), `StartAIFunctionReq` (`+29`), `CallAIEventFromAIFunctionReq` (`+121`), `CreateProjectileFromFrameEventReq` (`+0`: **dueño = copia**), `OnDemandEquipDockingGimmickSpawnReq` | Efectos secundarios de que la copia exista y actúe en el cliente | 1.061 / 848 / 959 / 153 / 82 / 89 / 38 |

### 1.3 Respuesta del servidor [captura]

`q1_compare.py`. Mensajes S>C cuya clave en `+0x00` es el objetivo, en los 120 ms siguientes al golpe:

| Respuesta | Golpe a enemigo real (485) | Golpe a copia (74) |
|---|---|---|
| `AttackTargetAck` | 100 % (mediana 8,4 ms) | 4 (5 %), **todas con atacante `0xA010F001`**: relevos del mod del golpe simultáneo al original, no respuestas a este golpe |
| `AttackerAck` | 100 % (8,9 ms) | 0 |
| `ApplyBuffAck` | 87 % | 0 |
| `OnPostBuffDamagedAck` | 61 % | 0 |
| `VaryHPAck` | 58 % | 7 (relevos, mediana 42 ms) |
| `ApplyActionRestrictionAck` | 44 % | 2 (relevos) |
| `NotifyHandleDeadAck` | 20 % | 0 |
| `MoveActorAck`/`StartActionAck` | — | 37/14 (el movimiento relevado de la copia) |

- **Ningún `Nak` (1013)** en las sesiones de hoy.
- Los Nak y Fail que aparecen (`q1_naks.py`) son ruido de fondo, o Req que nombran la copia por
  otros motivos:
  - `UnspawnOnDemandGimmickNak` ×59 y `SpawnOnDemandGimmickNak` ×37: del jugador, también en sesiones
    sin copias;
  - `CatchNak` ×8 (agarres del jugador);
  - `RegisterLoadGimmickFromActorImpulseNak` ×26, ante una Req con la clave de una copia (y ×88 con la
    marioneta);
  - `InteractionPreemptionStepFailAck` ×3 (copia).
- Todas las S>C que llevan una clave de copia son de tipos que inyecta el relevo (`ActorLoadData`,
  `Move`, `StartAction`, `Rotate`, `VaryHP`, `Dead`, `AttackTarget`, `ActionRestriction`,
  `RemoveActor`) o ecos de campos de una Req en un Nak o en `CreateProjectileAck +0x3D` (`q1_copykeys.py`).
- Conclusión: **un golpe a una copia no tiene ningún efecto en el servidor de G**. No hay que
  filtrarlo, y sirve como disparador limpio de la pieza 4.

### 1.4 Comparación con golpes reales en las mismas sesiones [captura]

- Los golpes a copias usan las mismas acciones que los golpes reales: `F104F0B7/4`, `5A2EA805/4`,
  `267BE8EF/11-13`, `0F357AC7/14-49`, `F0CB3CF9/6-8`, flechas…
- El hueso aparece en unos dos tercios de las entradas, tanto en las copias como en los golpes
  reales con daño (69 %): `Bip01 Spine2`, `Head`, `L Calf`…
- El tamaño del paquete es el mismo (1.307 B por objetivo, +665 B por cada objetivo más).
- La única diferencia en el cable es el silencio del servidor.
- En el cliente de G, la copia solo "reacciona" cuando llega el relevo desde H. Hoy eso pasaba
  solo porque el mismo golpe tocaba también al original (§4.3).

---

## 2. Tablas de daño (jugador → enemigo real)

### 2.1 Cómo se emparejan los campos [captura]

`q2_blows.py` → `blows.csv` (1.145 filas = golpe × objetivo real; 902 sobre enemigos, es decir,
cargas de tipo 3/4/6; el resto son objetos de tipo 7).

- **Id del ataque** (u64) compartido en la ráfaga:
  - `OnPostBuffDamagedAck +0x2C`;
  - `VaryHPAck +0x30` (299 de 314);
  - `AttackTargetAck +0x2D` o `+0x41` (unaligned);
  - `AttackerAck +0x6C`/`+0x78`.

  Hay que emparejar por este id y no por ventana de tiempo: los golpes rápidos llegan a menos de
  120 ms y se mezclan.
- **Campo de daño:** `OnPostBuffDamagedAck +0x0C` u32 = daño×1000.
  - Coincide con la caída de `VaryHPAck` (`+0x08` antes − después) en **292 de 292** golpes no
    letales.
  - En los 58 letales, el `VaryHPAck` se recorta a 0 y `OnPostBuffDamagedAck` trae el daño entero
    (overkill).
- **Variante:** `characterinfo` en `rec+0x18` del último `ActorLoadDataAck` del enemigo; nombre
  según `build/research/ids/characterinfo_keys.tsv`.
- `AttackTargetAck +0x08` (flags del resultado) clasifica el golpe:

| Flags `+0x08` (`+0x10`) | Golpes | Qué es |
|---|---|---|
| `F7` (7) | 44,7 % con daño + 7,6 % sin daño | Golpe limpio |
| `E3` (7) | 18,2 %, nunca daño | **Fijo por frame:** `25B2FA4C/5`, `D5F3475B/6`, `F30D7687/3`… son siempre `E3`. Frames de contacto que no hacen daño |
| `84` (3) | 10,9 % | **Bloqueado o en guardia:** 1-3 HP (7-20 % del daño limpio) |
| `87` (4) | 7,1 %, sin daño | Situacional, en frames que sí suelen dañar (¿esquiva o parada?) [inferido] |
| `E7`/`EF`/`E1` | 3 % | Variantes raras, con o sin daño |
| — | 5,4 % | Golpe a un cadáver (HP previo 0) |

### 2.2 Estabilidad [captura]

`q2_within.py`, `q2_mode.py`, `q2_ratio.py`, `q2_miss.py`. Solo golpes `F7` con daño > 0 y objetivo vivo:

| Clave | Claves con ≥2 muestras | Muestras iguales a la moda |
|---|---|---|
| variante + acción + frame | 86 | **86 %** (4 % ×2, 2 % ×0,5, 8 % otros) |
| sesión + variante + acción + frame | 50 | 89 % |
| HP máx + acción + frame | 66 | 75 % |
| acción + frame (cualquier variante) | 38 | 59 % |

- **No depende de la sesión:** el mismo jugador hace el mismo daño el día 3 y el 4. Por ejemplo,
  `F104F0B7/4` hace 14 contra Bleed Bandits en 10 sesiones. El equipo del jugador no cambió
  [captura].
- **Sí depende de la variante:**
  - el ajuste multiplicativo (`q2_seed.py`) da casi solo dos clases;
  - **1,0:** `NHM_Bleed_Bandits_*` de espada/hacha/arco, `NHM_Bandit_OneHandSword`;
  - **≈0,86:** `NGM_Wolf_Pursuer_*`, `Bleed_Bandits_SwordTowerShield`, algunos `OneHandBow`,
    `Bandit_TwoHandAxe`.

  El HP máximo no sirve como sustituto: hay élites de 200.000 con factor 1,0 [captura].
- **Críticos:** un ×2 exacto en el 4 % de los golpes limpios (56 = 2×28, 26 = 2×13, 46 = 2×23).
  No tiene que ver con el hueso: no son tiros a la cabeza [captura]. Lo calcula el servidor; el
  cliente no puede predecirlo [inferido].
- **Nivel:** no hay un campo de nivel identificado. Con estas capturas, la variante explica la
  diferencia [inferido].

### 2.3 Tabla (las 18 acciones/frames más usadas) [captura]

`q2_report.py`. Daño en HP. Formato `valor×veces`. P(AAR) = fracción de golpes limpios con
`ApplyActionRestrictionAck` (aturdimiento).

| acción/frame | hash de ataque | golpes | daño F7 (factor 1,0) | daño F7 (factor ≈0,86) | bloqueado (84) | ×2 | P(AAR) |
|---|---|---|---|---|---|---|---|
| `flecha (tipo 1)` | `10481FDD` | 122 | 28×40, 17×9, 56×6 | 23×12, 14×8, 6×2 | 2×9 | 6 | 0.96 |
| `F104F0B7/4` | `41275A91` | 43 | 14×16, 28×1 | 12×12 | 1×7, 2×2 | 1 | 1.00 |
| `5A2EA805/4` | `3BAC94EB` | 42 | 13×17, 26×1 | 11×4 | 1×9 | 1 | 0.95 |
| `0F357AC7/49` | `C7D0779C` | 38 | 14×9 | 12×10, 23×2 | 1×4 | 0 | 0.81 |
| `0F357AC7/39` | `D900FDCF` | 35 | 7×8 | 5×14 | 1×6 | 0 | 0.95 |
| `1FBA327A/56` | `302B95B1` | 34 | 28×8, 13×2, 56×2 | 23×12, 7×1 | 2×3, 1×1 | 2 | 0.96 |
| `0F357AC7/24` | `DA27944C` | 34 | 7×6, 14×2 | 6×12, 1×1 | 1×6 | 2 | 0.90 |
| `267BE8EF/13` | `1B0CD1E5` | 32 | 13×17, 26×2 | 11×4 | 1×3 | 2 | 0.91 |
| `267BE8EF/11` | `44C56700` | 30 | 7×12, 14×3 | 6×2, 12×1 | 1×8 | 3 | 1.00 |
| `70D78997/7` | `9F117782` | 28 | 28×4, 56×2 | 23×8 | — | 2 | 1.00 |
| `F0CB3CF9/8` | `C2EEF304` | 28 | 14×7 | 12×6, 14×1 | 1×6 | 0 | 1.00 |
| `F0CB3CF9/6` | `DF75AD48` | 27 | 13×9 | 11×8, 7×1 | 1×5, 2×1 | 0 | 0.94 |
| `0F357AC7/14` | `7EA682EF` | 26 | 7×6 | 5×6, 11×1 | 1×7 | 0 | 1.00 |
| `E3D7CB5A/31` | `C139B667` | 24 | 39×11 | 32×1 | 3×7 | 0 | 0.00 |
| `E3D7CB5A/24` | `A8F51345` | 19 | 28×8, 16×1, 13×1 | — | 2×4, 4×1 | 0 | 1.00 |
| `26AA9809/6` | `63135955` | 16 | 13×5 | 11×6, 21×1 | 1×1 | 0 | 1.00 |
| `26AA9809/8` | `81048142` | 15 | 14×3, 28×2 | 12×4 | — | 2 | 1.00 |
| `25B2FA4C/2` | `651C89AC` | 12 | — (siempre `E3`) | — | — | 0 | 0.00 |

Notas:
- **acción + frame → hash de ataque** (`AttackTargetAck +0x24`): es una función en 57 de 64 claves.
  Las 7 restantes tienen dos valores (por ejemplo `D5F3475B/16`) [captura]. G no ve el hash de
  ataque de sus golpes a copias (no hay `AttackTargetAck`), así que **la clave de G tiene que ser
  acción + frame**.
- **P(AAR) es casi binaria por clave** (≥0,8 o 0), así que se puede tabular [captura].
- **Flechas:** el `AttackTargetAck` lleva 10 hashes distintos (`10481FDD` ×72, `9683E1EB` ×21 con
  14/17 HP, `5A494E51`…), pero el `DoFrameEventReq` de tipo 1 no trae ninguno. Para distinguir el
  tipo de flecha en G hará falta el `CreateProjectileFromFrameEventReq` previo del jugador
  [inferido]. Mientras tanto, "flecha" = 28 × factor.
- Tablas completas: `seed_base.csv` (27 claves) y `seed_defence.csv` (52 variantes).

### 2.4 Cobertura de una tabla aprendida [captura]

`q2_cover.py`, `q2_curve.py`, `q2_factor.py`. La tabla aprende solo de los golpes `F7`, en orden
cronológico.

**Probabilidad de que la clave ya se conozca al dar el golpe n**:

| Golpe n.º | Acción+frame (tabla reiniciada en cada sesión) | Variante+acción (reiniciada) | Acción+frame (persistente entre sesiones) | Variante+acción (persistente) |
|---|---|---|---|---|
| 1-5 | 9 % | 2 % | 66 % | 26 % |
| 6-10 | 15 % | 3 % | 49 % | 12 % |
| 11-20 | 24 % | 6 % | 66 % | 18 % |
| 21-40 | 43 % | 16 % | 66 % | 27 % |
| 41-80 | 60 % | 17 % | 73 % | 30 % |
| 81-200 | 73 % | 6 % | 79 % | 34 % |

- Combate típico (separados por >20 s):
  - 33 combates con ≥3 golpes con daño;
  - mediana de **13 golpes** (máximo 69);
  - **7 claves de acción distintas** por combate.
- En total hay 29 claves de acción, 340 pares variante+acción y 58 variantes.
- **La clave variante+acción no llega a cubrirse nunca** (hay demasiadas variantes). La que se
  llena es acción+frame.
- **Error del modelo**, golpes limpios, tabla persistente:

  | Nivel de la predicción | Golpes | Dentro de ±1 HP |
  |---|---|---|
  | exacta (variante+acción) | 41 % | 75 % |
  | factorizada (base × factor) | 42 % | 61 % |
  | solo base | 10 % | 49 % |

  - Contando todos los golpes (bloqueos, críticos, cadáveres), un 53 % queda dentro del 20 %.
  - El ajuste global base × defensa sobre todas las muestras (`q2_seed.py`) da **83 % ±1 HP**:
    con la tabla ya sembrada, el error es pequeño.

---

## 3. La sesión real a dos jugadores (`20261003-183530`)

`q3_puppet.py`, `q3_window.py`, `q3_fights.py`, `q3_attacks.py`. 1.606 s; el jugador de H relogueó y
pasó a `0xA0101740`.

| Qué | Recuento [captura] |
|---|---|
| `StartActionAck` inyectados en la marioneta | 2.899 (168 acciones distintas) |
| …de ellas, acciones de ataque conocidas (hashes de accessor de golpes reales) | 61, repartidas en los 26 minutos (máximo 8 por minuto) |
| `DoFrameEventReq` del cliente de H con atacante = marioneta | **0** |
| `DoFrameEventReq` con la marioneta en la lista de objetivos | **0** |
| `CurrentTargetChangeReq` hacia la marioneta | **0** |
| `VaryHPAck` de la marioneta (relevo) | 88, **todos subidas** (1.060 → 1.125 HP, t = 447-513 s: regeneración tras daño sufrido en el mundo del amigo) |
| Otras Req que nombran la marioneta | `UpdateGimmickReq` ×176 (225-233 s), `OnDemandEquipDockingGimmickSpawnReq` ×9, `CallAIEventFromAIFunctionReq` ×1 (599 s), `RegisterLoadGimmickFromActorImpulseReq` (→ Nak) |
| Golpes reales de H | 492 `DoFrameEventReq` del jugador; los enemigos, 701 |

- Los 13 ataques de la marioneta con posición conocida ocurrieron a 1-8 m de algún NPC de H.
- Aun así, el mundo de H nunca registró que el amigo peleaba.
- Lo que ve H del combate de G es la animación (`StartActionAck`), sin consecuencias:
  - un actor en modo de control 2 no ejecuta frame events de servidor;
  - la IA de H no lo consideraba objetivo (el forzado `TS+0x3DC` de `aggro_test` es posterior).

  [captura + inferido]
- Esto confirma que la pieza 4 tiene que **originar el golpe en G** (su `DoFrameEventReq` a la
  copia) y **aplicarlo a mano en H**.

---

## 4. Reacción del servidor a un golpe real y qué inyectar en H

### 4.1 Orden y tamaños (≈9 ms después del `DoFrameEventReq`) [captura]

`q4_seq.py`. `@T` = clave del objetivo en `+0x00`; `@P` = clave del jugador atacante.

Secuencia típica de un golpe limpio (`F7`) con aturdimiento:

```
UpdateElementalAck@P (118) > AttackTargetAck@T (199/211) > ApplyActionRestrictionAck@T (77)
  > ApplyBuffAck@T (77) > VaryHPAck@T (92) > OnPostBuffDamagedAck@T (53) > ApplyBuffAck@T
  > AttackerAck@P (160/172) > VarySharpnessAck@P (8) > ApplyBuffAck@T
```

Variantes:
- **sin daño** (`E3`): `AttackTargetAck` > `ApplyBuffAck@T` > `AttackerAck`;
- **bloqueado** (`84`): se añaden `DataDefinedRegenerateValueAck@T` (90) y `VaryHPAck` +
  `OnPostBuffDamagedAck` con 1-3 HP;
- **muerte**:

  ```
  AttackTargetAck > [ApplyActionRestriction] > VaryHPAck(hp=0) > [OnPostBuffDamaged] > NotifyHandleDeadAck (19)
    > ApplyBuffAck@P / VaryDataDefinedStaticStatAck@P (41) ×n > UnapplyBuffAck@T ×4-8
    > LootDropCharacterRewardListAck@P (79/116/153) > ClearAllyGroupDataAck@T (12)
    > ElementalMaterialFuelAck@T (17) > AttackerAck@P
  ```

  `CreateProjectileAck`/`UnequipItemOnlyAck`@T aparecen cuando el muerto suelta el arma.

Campos útiles:

| Mensaje | Campos (offset del payload) |
|---|---|
| `AttackTargetAck` | `+0x00` objetivo, `+0x04` atacante, `+0x08` flags (§2.1), `+0x10` u32 (7/3/4), `+0x20` u32 (35/39/32/96), `+0x24` hash de ataque, u64 ids en `+0x2D`/`+0x41`, posición/impacto (`combate-coop.md` §1.2), **accessor de 12 B al final = el del `DoFrameEventReq`** |
| `ApplyActionRestrictionAck` | `+0x00` objetivo, hash de ataque hacia `+0x38`, `+0x49` fuente |
| `ApplyBuffAck@T` | Ids de buff `+0x04`: 1021/1022/1023/1104/1105/1106/34/10159/1120, fuente `+0x18` = atacante |
| `ApplyBuffAck@P` | 1081 (×855) y 0: lo que gana el atacante al acertar |

Muerte, en segundos tras el golpe letal (127 muertes):

| Mensaje | Retardo |
|---|---|
| `NotifyHandleDeadAck`, `UnapplyBuff`, `ElementalMaterialFuel`, `AttackerAck` | 0,009-0,010 (mismo lote) |
| `LootDropCharacterRewardListAck` | 0,010 (61 de 127) |
| Cliente: `EchoActionSessionIDReq`, `ChangeAIStateReq`, `ChangeAggroRelationValidTickReq`, `ChangeBattleStateReq`, `StartActionReq` (acción de muerte) | 0,027-0,030 |
| `OnDemandEquipDockingGimmickUnspawnReq` | 2,4 (mediana) |
| `RemoveActorAck` | **30 (mediana; 4,7-114)** (67 de 127 dentro de la ventana de 120 s) |

### 4.2 Lo que hace el cliente con un `AttackTargetAck` [captura]

`q4_inject.py`.

**Enemigo real controlado por este cliente** (85 casos en 112627). En los 100 ms siguientes, el
enemigo envía:

| Req | Fracción |
|---|---|
| `StartActionReq` (su reacción) | 84 % |
| `ChangeAIStateReq` | 69 % |
| `EchoActionSessionIDReq` | 56 % |
| `StartAIFunctionReq` | 51 % |
| `CurrentTargetChangeReq` | 15 % |

**Copia (modo 2) con `AttackTargetAck` inyectado** (257 casos):

| Req | Fracción |
|---|---|
| `EchoActionSessionIDReq` | 38 % |
| `StartActionReq` y demás | 0 |

Las otras inyecciones dan:

| Inyección | `EchoActionSessionIDReq` |
|---|---|
| `ApplyActionRestrictionAck` | 82 % |
| `VaryHPAck` | 17 % |
| `NotifyHandleDeadAck` | 78 % |

El servidor no se queja del eco: no hay Nak. **La inyección S→C sobre la copia es inofensiva**, y la
reacción visible de la copia tiene que llegar como `StartActionAck` relevado.

### 4.3 Qué necesita H para que el golpe de G "se vea y pese" [inferido salvo lo marcado]

El enemigo E es real en H (H controla su IA) y el atacante es la marioneta P = `0xA010F001`.

| # | Mensaje en H | ¿Hace falta? | Por qué |
|---|---|---|---|
| 1 | **`VaryStatByAccInputReq(E, 0x000F4240, −daño×1000)` C→S** a su servidor | **Sí**: es el daño real | El servidor emite `VaryHPAck` real, que el relevo 3c ya manda a la copia de G [captura: el relevo existe]. **Falta probar (T2)** si a 0 HP emite `NotifyHandleDeadAck`, botín y `RemoveActorAck` |
| 2 | **`AttackTargetAck` S→C inyectado**: `+0x00` E, `+0x04` P, flags `F7`, `+0x10` 7, `+0x24` hash de ataque (de la tabla acción+frame → hash, §2.3), accessor = el del golpe de G, posiciones = las de E en H | **Sí**: es la reacción | En H, E es local: el cliente lanza solo `StartActionReq`/`ChangeAIState`/`Echo` (84 %), como con un golpe propio [captura: comportamiento con golpes reales]. Esa reacción vuelve a G por el relevo de acciones de 3b. Con flags `84` en vez de `F7`, la reacción sería de bloqueo |
| 3 | **`ApplyActionRestrictionAck`** (`+0x49` = P) | Sí cuando P(AAR) ≥ 0,8 para esa clave | Aturdimiento y derribo. Plantilla: el último AAR real del mismo hash de ataque en H |
| 4 | `OnPostBuffDamagedAck` (E ← P, daño) | Probablemente | Es el número de daño en pantalla, si el juego lo muestra para golpes ajenos. Barato (53 B) |
| 5 | `ApplyBuffAck@T` 1021-1023/1104-1106 (fuente P) | Opcional | Estado de golpe (aturdimiento por nivel). Probar si cambia la reacción |
| 6 | `AttackerAck`, `VarySharpnessAck`, `ApplyBuffAck@P` (1081) | **No en H** | Son del atacante. Pero **G pierde lo suyo**: su servidor no le da el buff 1081 ni el filo por golpes a copias. Valorar un `ApplyBuffAck`/`VaryStat` local en G si 1081 resulta ser un recurso de combate |
| 7 | Muerte: `NotifyHandleDeadAck`, `LootDrop…`, `RemoveActorAck` | **No inyectar** | Si (1) mata de verdad, los emite el servidor de H, y el relevo 3c ya manda `Dead` y `EnemyRemove` a G. Inyectarlos sin el servidor desincroniza (`combate-coop.md` §4.1) |

En G, sobre la copia:
- inyectar en el acto un `AttackTargetAck` cosmético (atacante = jugador de G) para que el impacto
  no espere el viaje de ida y vuelta;
- descartar luego el `AttackTargetAck` relevado de H para ese mismo golpe (mismo accessor).

[inferido]

---

## 5. Aggro

### 5.1 Medidas [captura]

`q5_aggro.py`, `q5_more.py`, `q5_case.py`, `q5_hold.py`. `CurrentTargetChangeReq`:
`u32 actor | u32 objetivo | u64 seq | u8 force | u8 motivo`.

Objetivo de cada enemigo (cargas de tipo 3/6) al recibir el **primer** golpe del jugador:

| Objetivo previo | Enemigos | CTC hacia el jugador después |
|---|---|---|
| ya era el jugador | 189 (95 %) | No hace falta (10 lo reenvían más tarde, mediana 9 s) |
| otro NPC | 5 | 5: a los **0,027-0,030 s** (×4, motivo `0x18`) y a 0,85 s (×1, motivo `0x1D`) |
| ninguno todavía | 3 | 3, a los **0,026-0,030 s** (motivo `0x18`) |
| la marioneta (prueba de aggro de 065037) | 1 | A los **9,7 s** (motivo `0x1D`): el golpe **no** le hizo cambiar |

- Los 26-30 ms son el mismo lote que la reacción al golpe: `AttackTargetAck` a los ~9 ms, y el
  cliente responde ~20 ms después con `Echo`/`StartAIFunction`/`ChangeAIState`/**CTC**/`StartAction`.
- Tiempo que se mantiene un objetivo antes de cambiar (mediana):

  | Cambio | Mediana | p10-p90 |
  |---|---|---|
  | jugador → nada | 9,4 s | — |
  | NPC → NPC | 5,0 s | — |
  | NPC → jugador | 5,6 s | 0,5-18 s |
  | jugador → NPC | 5,0 s | — |
- Motivos de CTC en todas las capturas:
  - `0x18` hacia el jugador (4.706);
  - `0x18` hacia otros (915);
  - `0x1B`/`0x02`/`0x09` hacia 0 (al perderlo);
  - `0x1D` hacia otro o hacia el jugador (124).
- Efecto secundario de hoy: **enemigos reales del mundo local pasaron a apuntar a copias**
  (45 CTC, motivo `0x18`). Uno (`0xB0100AFE`) les pegó con `AttackByCollisionReq`. Las copias
  cuentan como hostiles para los enemigos locales. En dos PC no importa, porque G no tendrá
  enemigos propios, pero ensucia las pruebas en un PC.

### 5.2 Diseño "G le pegó, así que ahora va a por la copia de G en H" [inferido]

1. H lleva el **objetivo actual de cada enemigo** con sus propios `CurrentTargetChangeReq` (ya los
   lee `aggro_test`).
2. Al aplicar un golpe de G sobre E:
   - **Si E no apunta a ningún jugador** (0, otro NPC, nada): cambiar ya, como el juego nativo
     (26-30 ms). Inyectar `CurrentTargetChangedAck` (3040) con objetivo P, `force` = 1, motivo
     `0x18` y `seq + 1` (receta de `aggro_test`) unos 20 ms después del `AttackTargetAck`.
   - **Si E apunta al jugador de H:** usar amenaza.
     - Daño acumulado por atacante en una ventana de 10 s.
     - Se cambia a P cuando `amenaza(G) > 1,3 × amenaza(H)`.
     - **Mínimo 5 s** con el mismo objetivo, la mediana nativa de "NPC → jugador" y "jugador → NPC".
     - Volver a H con la misma regla.
   - **Si E ya apunta a P:** nada.
3. Requisitos ya conocidos: P tiene que ser objetivo válido (`aggro_set_copy_targetable`,
   `TS+0x3DC`). El daño del enemigo a P ya lo cubre la pieza 2.
4. Evitar el bucle: el `CurrentTargetChangeReq` que el propio cliente de H emitirá tras la
   inyección no debe tomarse como "cambio espontáneo".

---

## 6. Recomendaciones para la pieza 4

1. **Disparador en G:** `DoFrameEventReq` C→S del jugador cuya lista de objetivos tenga
   `is_enemy_copy_key()`.
   - No se filtra ni se modifica: el servidor de G lo ignora sin Nak (§1.3).
   - Por cada copia se envía a H un evento **`GuestHit`**:
     `u16 net_id | u32 hash de acción | u32 índice de frame | u8 tipo de accessor | u8 hueso (índice o hash) | vec3 impacto relativo a la copia | u16 daño_base_G×10`.
   - Sube `kVersion`.
   - Los golpes mixtos (copia + reales) se tratan igual: los reales ya los procesa su servidor.
2. **Daño:** repartirlo entre los dos lados, porque el atacante pone la base y el enemigo pone la
   defensa.
   - **G** mantiene `base[acción+frame]`:
     - **persistente en disco**;
     - sembrada con `seed_base.csv`;
     - actualizada con los golpes limpios de G a enemigos reales (flags `F7`, emparejados por id
       de ataque, §2.1), cuando G los tenga.

     G manda la base. Si no la conoce, manda 0.
   - **H** mantiene `defensa[variante]`, aprendida de sus propios golpes `F7` (clases ~1,0 y
     ~0,86), con 1,0 por defecto.
     - Aplica `daño = round(base_G × defensa)`.
     - Si la base es 0, usa la suya propia para esa clave.
     - Si no la tiene, usa la mediana de las bases (~13 HP).
   - **Clave "sin daño":** si en H esa acción+frame da siempre `E3`, el golpe no hace daño (18 % de
     los golpes; deterministas, §2.1).
   - No intentar predecir críticos (4 %) ni bloqueos (11 %).
     - Bloqueos, opción más adelante: si E está en una acción de guardia en H, aplicar el 10 %.
     - Error esperado: ~83 % de golpes dentro de ±1 HP con la tabla sembrada.
3. **Aplicación en H** (§4.3), en este orden:
   1. `VaryStatByAccInputReq`;
   2. `AttackTargetAck` inyectado (atacante P, hash de ataque y P(AAR) por tabla, accessor de G);
   3. `ApplyActionRestrictionAck` si P(AAR) ≥ 0,8;
   4. `OnPostBuffDamagedAck`.

   Nada de muertes ni botín inyectados.
   - **Antes:** prueba T2 (`combate-coop.md` §6) para ver si `VaryStat` hasta 0 mata de verdad.
   - **Si no mata:** usar como último golpe la opción (c1) "atacante suplente", o dejar al enemigo
     a 1 HP y avisar.
4. **Aggro:** §5.2.
5. **En G:** golpe cosmético inmediato sobre la copia y deduplicación del relevo de H (§4.3).
   - HP y muerte siguen llegando por el relevo fiable (3c).
   - Comprobar si G echa en falta el buff 1081 y el filo.
6. **Pruebas en un PC:**
   - Las copias de hoy ya son el banco de pruebas: el golpe a la copia vuelve por el eco y se
     aplica sobre su **original** (H = G = el mismo PC).
   - El original ya cambia de HP por los golpes directos. Para medir solo la pieza 4, golpear copias
     lejos de su original (offset del espejo de ≥10 m). Hoy, 3 golpes de 74 tocaron a la vez la
     copia y su original.
   - Desactivar la hostilidad entre los enemigos locales y las copias (§5.1), o pelear con un solo
     grupo, para que no se mezclen daños.
7. **Datos que faltan:**
   - capturas con otras armas o con otro equipo, para ver si la base escala con el ataque del
     jugador (aquí el equipo no cambió);
   - el `CreateProjectileFromFrameEventReq` de cada flecha, para separar tipos de flecha;
   - que el campo de la acción no cambie con la orientación del golpe (en estas capturas no cambia).

---

## 7. Scripts y salidas (`build/research/pieza4/`)

Ejecutar con `build/research/venv/Scripts/python.exe <script>`. El índice de cada captura se
guarda en `cache/<sesión>.npz`.

| Script | Qué hace |
|---|---|
| `pk.py` | Lector común: índice numpy de registros, nombres de opcode, `dfe()` (atacante, accessor, objetivos), `bone()`, mapa captura ↔ log, pares original → copia del log |
| `q1_survey.py`, `q1_table.py` | Golpes del jugador por clase de objetivo y sesión |
| `q1_copyhits.py` → `out_q1_copyhits.txt` | Cada golpe a copia con su contexto de ±1,5 s |
| `q1_compare.py` | Respuestas del servidor a golpes a copias y a golpes reales; Nak por sesión |
| `q1_copykeys.py`, `q1_side.py`, `q1_naks.py` | Censo de paquetes con claves de copia; Req secundarias; origen de cada Nak |
| `q2_blows.py` (+ `q2_blows_lib.py`) → `blows.csv` | Una fila por golpe × objetivo real: accessor, flags, hash de ataque, daño, HP, variante, hueso |
| `q2_ids.py`, `q2_dump.py` | Enlace por id de ataque; volcado hexadecimal de una ventana |
| `q2_stats*.py`, `q2_table.py`, `q2_within.py`, `q2_ratio.py`, `q2_miss.py`, `q2_mode.py`, `q2_crit.py`, `q2_sess.py` | Estabilidad, flags, críticos, dependencia de la variante y de la sesión |
| `q2_seed.py` → `seed_base.csv`, `seed_defence.csv` | Ajuste base × defensa (tablas semilla) |
| `q2_cover.py`, `q2_curve.py`, `q2_factor.py` | Simulación de la tabla aprendida (cobertura y error) |
| `q2_report.py` | La tabla de la §2.3 |
| `q3_puppet.py`, `q3_window.py`, `q3_fights.py`, `q3_attacks.py` | Sesión a dos jugadores |
| `q4_seq.py`, `q4_inject.py`, `q4_buffs.py`, `q4_abc.py` | Orden de la reacción; respuesta del cliente a Ack inyectados; buffs; `AttackByCollisionReq` |
| `q5_aggro.py`, `q5_more.py`, `q5_case.py`, `q5_hold.py` | Latencia del cambio de objetivo, motivos, tiempos de mantenimiento |

Nota sobre `AttackByCollisionReq` (opción c3 de `combate-coop.md`) [captura]:
- Las 10 del jugador sobre enemigos vivos (173354, hash `81048142`) **no tuvieron respuesta**.
- Las 6 de 105149 (atacante 0, hash `F2EE2678`) sí dieron `AttackTargetAck`/`VaryHPAck`, pero con
  1 HP fijo y el ataque atribuido a otro actor.

No sirve para entregar el daño de G.
