# Pendiente: recompensas y progreso para los dos jugadores

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco, tablas `staticinfo` ya
  extraídas en `build/research/ids/out/gd/`);
- re-análisis de las 50 capturas (`bin64\cdmp_packets_20261004-*`, incluida la `114648` con las
  muertes V2/L1, y `D:\cdmp-grabaciones\*`).

El juego no se ejecutó. Continúa [pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) (§1 botín,
§8 resultados en vivo) y [pieza4-dano-servidor.md](pieza4-dano-servidor.md).

Marcas:
- **[verificado]**: leído en el código o en los datos estáticos;
- **[captura]**: medido en capturas;
- **[inferido]**: deducción pendiente de prueba en vivo.

Notación: **H** anfitrión, **G** invitado, **E** enemigo de H, **gemelo** = el enemigo nativo
equivalente en el mundo de G, jugador `0xA0100001`.

Scripts y volcados en `build/research/p_rewards/` (§9).

---

## 0. Resumen

1. **Las muertes por truco no dan experiencia.** [captura + verificado]
   - Al matar a un humanoide de forma normal (motivo 8), el servidor manda
     `AddSubLevelExperienceAck`:
     - es la experiencia de **puntos de habilidad** (subnivel 521 `SkillPoint_Kliff`);
     - llega 1-2 ms después de `NotifyHandleDeadAck`;
     - **172 de 172** muertes (las 4 que faltan son animales, que no dan experiencia).
   - Con motivo 16 (`KillCheatReq`) o 15 (estado fatal y `DieByRunOutFatalReq`): **0 de 9**,
     incluso con una ventana de 6 s.
   - Control en la misma sesión (`114648`): la muerte normal de 367,570 sí dio experiencia
     (+15). Las 8 por truco, a su alrededor, y de las mismas familias de bandidos, no.
   - La experiencia sale del **evento de juego `KillActor` (0x61)** que recibe el asesino.
     - Lo emite `0x29431A0`, llamado desde Status vf96.
     - Antes de emitirlo, el servidor busca a quién dar el crédito. Lo encuentra cuando el golpe
       trae datos de ataque o con un campo de "último atacante" del muerto.
     - `KillCheatReq` y `VaryStatCheatReq` no llevan datos de ataque, así que nadie recibe el
       crédito [inferido].
   - **Hoy, cada remate de G con `KillCheatReq` le quita a H entre 10 y 15 puntos de
     experiencia** y el crédito de la muerte.
2. **El remate por colisión sí da el crédito completo.** [captura]
   - Las muertes de X2 (`111538`, 170,271 y 179,123) se rematan con `AttackByCollisionReq` y el
     jugador como dueño.
   - Salen con motivo 8, **experiencia (+15)**, buff 20001 y botín según la variante.
   - Es la vía para que la muerte cuente igual que una normal, en H y en G.
3. **Recompensas que da una muerte** [verificado + captura]:
   - el botín de la variante (ya documentado);
   - la experiencia de puntos de habilidad, por categoría del muerto: 10, 15, 25, 140 o 1000;
   - el buff 20001 `Skill_Active_Killer` (`RedLightning` + `CombatHigh`);
   - +1 a la **barra de liberación del campamento**. No depende del asesino: también suben las
     muertes a manos de soldados aliados.
   - Al completar la barra:
     - `CompleteQuestGaugeAck`;
     - desafíos y misiones;
     - objetos;
     - **+100 de contribución de facción**.
   - Los desafíos de maestría (`Challenge_SealedArtifact_*`) también cuelgan de `KillActor`.
   - No existe "experiencia de personaje": `AddExperienceAck` aparece 0 veces en 50 capturas.
4. **Para G: el único camino que da todo de forma nativa es la "muerte espejo" (R6)** en su
   propio servidor, sobre el gemelo.
   - Rematar al gemelo con **`VaryStatCheatReq` a 1 HP y después `AttackByCollisionReq`**,
     con el jugador de G como dueño. Da:
     - el botín;
     - la experiencia;
     - el crédito de misiones y desafíos;
     - la barra del campamento;
     - el buff.
   - Plan B: `KillCheatReq`. Da botín, buff y, con toda probabilidad, barra de campamento, pero
     ni experiencia ni crédito.
   - Requisito: que la supresión 3g **oculte y congele** el gemelo en vez de no crearlo.
5. **Dar objetos sin gemelo** (`SpawnItemToGroundByCheatReq`, `CreateItemFromTrItemValueCheatReq`)
   es posible sobre el papel.
   - Pasan por el mismo permiso que `KillCheatReq`.
   - Pero el formato está sin decodificar (no hay capturas).
   - No dan experiencia ni crédito, y un formato mal hecho escribe objetos inválidos en la
     partida. **Solo como último recurso.**
6. **Riesgos** [verificado + inferido]:
   - **Telemetría:** la muerte del **propio jugador** envía un evento con `dead_reason`, que con
     motivo 16 se llama `DevCheat`.
     - Es otra razón para que la pieza 2 nunca mate al jugador con trucos. Hoy no lo hace:
       deja 1 HP con `VaryStatByAcc`.
   - **Sin marca de trampa:** en los manejadores de los trucos no se ve ninguna marca de
     "trampa usada" en la partida ni en el perfil.
   - **Logros de Steam:**
     - van por el cliente (`PlatformAchievementManager_Steam`), desde scripts y desafíos;
     - las muertes por truco no los activan: se pierde progreso, no se gana nada indebido.

---

## 1. Qué se mide al morir un enemigo [captura]

`r_after.py 1.0` y `r_after.py 6.0` miran los mensajes que siguen a cada `NotifyHandleDeadAck`
(S→C) en las 50 capturas, por tipo de muerto, asesino y motivo.

| Muerto / asesino / motivo | Muertes | `AddSubLevelExperience` | `UpdateQuestGauge` | `LootDrop` | `ApplyBuff@P` 20001 |
|---|---|---|---|---|---|
| NPC / jugador / **8** | 176 | **172** (las 4 restantes: conejo y ciervos) | 92 | 69 | 176 |
| NPC / jugador / **16** (`KillCheatReq`) | 5 | **0** (también a 6 s) | 0 | 1 | 5 |
| NPC / jugador / **15** (fatal → `DieByRunOutFatalReq`) | 4 | **0** (también a 6 s) | 0 | 2 | 4 |
| NPC / otro NPC / 8 | 41 | 1 | **7** | 5 | — |
| NPC / otro NPC / 7 (caída) | 15 | 0 | 0 | 0 | — |
| NPC / `A0101740` (jugador de otra partida) / 8 | 9 | 9 | 0 | 3 | — |

- **Variantes de las muertes por truco** (`114648`):
  - Bleed Bandits `54138`, `54119`, `54121`, `54140`, `54109`, `54122` y `54143`, más un
    `Wandering_Mercenary_54605`;
  - en `111538`, una `54117`.
  - Las mismas familias dan experiencia cuando mueren de forma normal (por ejemplo, la `54117`
    en `064320` 129,8 y en `111538` 170,271).
- **El remate X2** (colisión con el jugador como dueño, motivo 8) dio experiencia 2 de 2 veces.
  - El X2 con dueño gimmick (203,813) no la dio: el asesino no era un jugador.
- **La barra de campamento** (`UpdateQuestGaugeAck`):
  - solo existe donde hay un campamento sin liberar: hay 0 barras en `111538` y `114648`, también
    en las muertes normales. **No hay datos de cómo se comportan las muertes por truco**;
  - en `084001` y `090008`, soldados de Hernand (`53224`, `53205`, `500068`) y un mercenario
    errante mataron bandidos, y la barra subió igual (+1).
- El buff 20001 llega igual con 8, 15 y 16 (`r_buffs.py`, `r_b20001.py`). El payload es
  idéntico salvo la instancia y el nivel (`+0x0C`, que varía también entre muertes normales).
- Totales globales (`r_global.txt`):
  - `AddExperienceAck`: 0;
  - `AddSubLevelExperienceAck`: 192;
  - `UpdateQuestGaugeAck`: 108;
  - `CompleteChallengeAck`: 6;
  - `CompleteQuestGaugeAck`: 4.

### 1.1 Formatos [captura]

| Mensaje | Formato |
|---|---|
| `AddSubLevelExperienceAck` 2381 (27 B) | `u32 jugador \| i64 0x7FFF…FF \| u8 0 \| u32 subnivel \| u64 cantidad \| u16 0` |
| `UpdateQuestGaugeAck` 3088 (20 B) | `u32 jugador \| u32 barra \| u16 0 \| u32 +1 \| 6×0` |
| `CompleteQuestGaugeAck` 2738 (8 B) | `u32 jugador \| u32 barra` |
| `LootDropCharacterRewardListAck` 2897 | `u8 4 \| u32 muerto \| n × 37 B {u8 tipo, u32 clave de objeto, u32, u64 cantidad, u32 0x19, …}` |

Distribución de la experiencia de las 192 `AddSubLevelExperienceAck`:

| Subnivel | Cantidades |
|---|---|
| 521 | 15 ×143, 10 ×32, 25 ×3, 140 ×3 |
| 601 | 1 ×6, 100 ×1 |
| 701 | 1 ×4 |

- **Botín real de `114648`:**
  - `Quest_Paper_Kwe_Baris` (objeto de misión, en casi todas las muertes);
  - `Boss_Reward_Tracker`, `Trade_Fur_01`, `Strawberry`;
  - la clave 30 (¿dinero?);
  - 700001 con tipo 0.

  **Ojo:** el botín incluye objetos de misión.

---

## 2. Dónde decide el servidor [verificado]

### 2.1 Cadena de la muerte

```
golpe → registro de daño → Status vf43 (0x2941B60 → .xtls 0x10433BD0)
   NotifyHandleDeadAck (escritor 0x29927E0)
   → 0x1797850 → .xtls 0xC76CF30 (post-muerte)
        si asesino != null: Status vf96 0x2941D60(status, asesino, motivo, ahora, a5, ¿datos de ataque?, &registro)
             → 0x29431A0  (eventos de juego)
        Status vf97 0x2942350(status, asesino, ahora, motivo, a5)  (botín y telemetría)
```

- vf43 recibe del registro de daño:
  - el asesino;
  - el motivo (`r9b`);
  - los bytes `+0x2E`, `+0x30` y `+0x31`;
  - el puntero al registro.
- `KillCheatReq` los pasa en `0x2C41226-0x2C4125D` (`[rbp-0x33]` motivo, `[rbp-0x2F]` = `+0x31`).
- `+0x31` llega a `0x29431A0` como 3.er argumento (`r8b`) y decide si se usa el bloque de datos de
  ataque (`arg6`) [verificado el flujo; el significado "datos de ataque" es inferido].

### 2.2 `0x29431A0`: los tres eventos de juego de una muerte

Enum `GameEventType` (registro en `0x15CC…` sobre la tabla `0x6A6A240`, `gameevent_enum.txt`):
- `0x61` = `KillActor`;
- `0x62` = `DeadActor`;
- `0x6B` = `PlayerDead`.

| Evento | Destinatario | Condición | ¿Depende del motivo o del asesino? |
|---|---|---|---|
| `PlayerDead` 0x6B | El muerto | El muerto es un jugador (vf `0x178`) | No |
| `DeadActor` 0x62 (`0x29432F2`) | El dueño del grupo o escenario del muerto (`0x28FB8B0` → `0x2AFFF50`) | El muerto pertenece a un grupo | **No**: se emite antes de resolver al asesino |
| `KillActor` 0x61 (`0x2943B8B`) | El **acreditado** | El acreditado existe, es distinto del muerto y **es jugador** | **Sí**, por cómo se resuelve el acreditado |

Cómo se resuelve el acreditado (`0x29433EF-0x2943771`):

1. Si la `CharacterInfo` del muerto tiene el byte `+0x157`: se busca por `[asesino+0x90]`.
2. Si no, **con datos de ataque** (`r8b != 0`):
   - si el asesino es jugador, el acreditado es el asesino;
   - si no (proyectil o NPC), el dueño del ataque (`arg6+8`).
3. **Sin datos de ataque**: se busca la clave guardada en `[[muerto+0x68]+0x20]+0x250`
   (¿último atacante?).
4. Si no hay acreditado: solo se reintenta si el **motivo es `0xB`** (`Fire`), por el dueño del
   fuego. Con cualquier otro motivo **no hay `KillActor`**.

**Lectura** [inferido]:
- `KillCheatReq` y `VaryStatCheatReq` llaman a `0x17AE080` con los argumentos de ataque a 0, así
  que el registro sale sin datos de ataque.
- El crédito depende entonces de `+0x250`, que no parece apuntar al jugador.
- Resultado: no hay `KillActor` y no hay nada de lo que cuelga de él.
- Encaja con el 0 de 9 de §1. Falta ver en vivo qué es exactamente `+0x250`.

### 2.3 La experiencia cuelga de `KillActor`

- **`.xtls 0x107C73D0`** registra el manejador de subniveles `0x2C14650` (→ `0x2C12EC0` →
  `.xtls 0x107CCA50`) con `0x1ABB4D0` para estos eventos:
  `{0x61 KillActor, 0x3E Interaction, 0x3D InteractionStart, 0x1E DoCrime, 0x21 ChangeWantedState,
  0x07 Sell, 0x02 MoveItemToWareHouse, 0x31 LearnKnowledge, 0x2D HireMercenary}`.
- **`.xtls 0x107CCA50`** recorre los subniveles del jugador. Para cada entrada `{u8 evento,
  condición, u64 cantidad}` con el evento igual:
  1. evalúa la condición (`0x1AAD770`);
  2. llama a **`0x2C0FC00`** (→ `.xtls 0x107CBD10`), que escribe `AddSubLevelExperienceAck` con
     el escritor `0x2A66410`.
- **Datos** (`sublevelinfo` 521 `SkillPoint_Kliff`, `r_sinfo.py`):
  - evento `0x61` (`KillActor`);
  - requisito `0xF642E` = `CheckCharacterKey(Kliff) && CompleteMission(Mission_Intro_Abyss_Tutorial)`.
  - Cantidades por categoría del muerto:

    | Condición | Categoría | Experiencia |
    |---|---|---|
    | `0xF642F` | `Tier_1` (o `BountyHunter_Elite`) | 10 |
    | `0xF6430` | `Tier_2` | 15 |
    | `0xF6431` | `Tier_3` | 25 |
    | `0xF6432` | `Elite` | 140 |
    | `0xF6433` | `Boss` | 1000 |

  - Las variantes de Oongka (522) y Damian (523) son iguales.
- 601 `Contribution_Hernandian` y 701 `LiberationRefugee` **no salen de muertes**:
  - salen de interacciones (rescatar o registrar), con `InteractionDoStepReq` justo antes;
  - y de completar la barra (+100), como en `090931` 162,059;
  - las entradas de 601 con `0xF6474` (asesinato con testigos) restan 100.
- `ServerStatusExpBuffProcessor` vf2 (`0x2ACCAC0`, buff de tipo 9 → subnivel) es otra puerta,
  pero no se usa en las muertes. El buff 20001 no es de experiencia: es `Skill_Active_Killer` →
  `BuffLevel_RedLightning` (1000027) y `BuffLevel_CombatHigh` (1000078).

### 2.4 Misiones, desafíos, campamentos y logros

- **Misiones de "elimina N" y desafíos.** Búsqueda heurística en `missioninfo`
  (`r_mission_events.py`: evento + clave de condición). `KillActor` aparece en:
  - los desafíos de maestría `Challenge_SealedArtifact_Mastery_*`, por ejemplo "matar saltando con
    lanza", "con el HP al 100 %" o "esquivando con pistola";
  - los desafíos `…_Vehicle_*`, a caballo o en cañón;
  - `Mission_Varnia_Thomas_1` y `Mission_MarniDragon_Boss_Valgash_0`.

  Las misiones sin condición no salen en esta búsqueda, así que es una cota inferior [inferido].
  **Las muertes por truco no las hacen avanzar** [inferido, por el mismo evento que la
  experiencia].
- **Liberar campamentos.**
  - La barra (`QuestGauge_Her_RoccaHillBanditCamp`, `…North_WatchTower`, `…StoneTowerVillage`,
    `…EmbernestBeacon`…) sube +1 por muerte de un miembro, la cause quien la cause [captura].
  - Al completarse llegan, en el mismo lote:
    - `CompleteQuestGaugeAck`;
    - `CompleteChallengeAck` ×2 (`Mission_Node_Her_EmbernestBeacon_Surround_0/1`);
    - `UpdateMissionListAck`;
    - `AddItemToInventoryAck` (objeto 657);
    - contribución 601 **+100**.
  - Lo más probable es que la barra cuelgue de `DeadActor`, que no depende del motivo, así que
    **las muertes por truco cuentan** [inferido; prueba T2].
- **Logros de Steam.**
  - Cliente: `PlatformAchievementManager_Steam` (vtable `0x57D6B28`) y
    `PascriptComponentProperty_PlatformAchievementKey`. Se disparan desde scripts y desafíos.
  - `platformachievementinfo` tiene 34 logros: `Swordmaster`, `Spearmaster`, `Gunmaster`,
    `BattlefieldWarlord`, `BeastButcherer`, `UltimateHunter`, `TheUltimateWarrior`…
  - Los de maestría tienen pinta de colgar de los desafíos `SealedArtifact_Mastery_*`, es decir,
    de `KillActor` [inferido].
  - El servidor no envía nada de logros: no hay ningún mensaje Troc de logros.
- **Recompensas, búsqueda y captura (bounties) y facciones.**
  - Los objetivos de búsqueda y captura son actores de escenario (`CheckSpawnReason(Wanted_…)`)
    que solo existen en el mundo de quien aceptó el encargo. No se analizó más.
  - La contribución de facción sale de completar barras e interacciones, no de cada muerte.

### 2.5 Botín (repaso de `pieza4-botin-reaccion.md` §1.2)

- Lo decide vf97 (`0x2942730` → … → `0x2B83960`) según la variante (máscara 2).
- El motivo solo importa si vale 0 o 3, así que 8, 15 y 16 dan lo mismo.
- No depende de `KillActor`.
- Cae en el mundo del servidor que mata, para el usuario de ese field.
- Se recoge con una acción del jugador (`DoFrameEventReq` → `AddItemToInventoryAck`, en 58 casos
  de `r_global`). Eso exige que el objeto exista **en ese servidor**: G no puede recoger el botín
  del mundo de H.

### 2.6 Telemetría y marcas de "trampa"

- **vf97 `0x2942350` (`0x29425E6-0x29426E1`)**:
  - si el muerto es jugador y el embudo de telemetría está activo, construye
    `CharacterEventFunnelFormat` con `dead_reason` = nombre del motivo (tabla `0x57C7BD0`), más la
    posición;
  - nombres del motivo: 0 `SpawnAsDead`, 6 `Drowning`, 7 `Falling`, **8 `BuffDamage`**, 11 `Fire`,
    14 `KillSelf`, **15 `Fatal`**, **16 `DevCheat`**, 18 `Pressed`;
  - con los enemigos no se envía nada [verificado].
- **`KillCheatReq` (`0x2C40C00`) y `VaryStatCheatReq` (`0x2C3FBF0`)**:
  - no escriben ninguna variable global ni en el perfil;
  - las llamadas a `0x14276D0`/`0x1427C10` son la construcción del Nak de error;
  - el permiso es `[[actor+0xA0]+0x68]+0x130` vf `0x140`.

  [verificado en lo leído; no es exhaustivo: el código mutado de `.xtls` no se recorrió entero]

---

## 3. Qué recibe hoy cada jugador

| Recompensa | H, golpe propio (motivo 8) | H, remate de G con `KillCheatReq` (16) | H, remate de G a 0 HP → fatal (15) | G hoy |
|---|---|---|---|---|
| Botín (variante con máscara 2) | Sí | Sí | Sí | **No** |
| Buff 20001 | Sí | Sí | Sí | No |
| Experiencia de puntos de habilidad (10-1000) | Sí | **No** | **No** | No |
| `KillActor` (misiones, desafíos de maestría) | Sí | **No** [inferido] | **No** [inferido] | No |
| Barra de campamento (+1) y su recompensa | Sí | Sí [inferido] | Sí [inferido] | No |

---

## 4. Opciones para G (y para no quitarle nada a H)

### A. Muerte espejo con remate por colisión (R6 + X2). **Recomendada**

Cuando E muere en H, G mata a su gemelo en **su** servidor:
1. `VaryStatCheatReq(gemelo, 0x000F4240, modo 0, −(hp−1), 0, 0)`.
   - Con HP desconocido, usar el modo 1 (fijar) a 1 HP. El modo 1 está sin probar (T3).
   - Deja el gemelo a 1 HP sin entrar en estado fatal, porque fatal solo ocurre a 0.
2. `AttackByCollisionReq(gemelo, jugador de G, 0, pos(gemelo), dir, 0xF2EE2678, 0)`.
   - Quita unos 7 HP con el jugador como dueño (X2).
   - Muerte con motivo 8, datos de ataque y asesino = jugador de G.
3. Resultado en el mundo de G, todo de forma nativa:
   - botín de la variante;
   - experiencia;
   - `KillActor` (misiones y desafíos);
   - buff 20001;
   - barra de campamento.

**Requisitos y riesgos**

- **El gemelo tiene que existir en el servidor de G.** Si la supresión 3g decide "no crear"
  (filtrar `ActivateSpawningPoolReq` y compañía), esta opción muere. Tiene que ser "ocultar y
  congelar" (ver el informe de 3g).
- **Emparejar por variante y punto de aparición** (`ids-enemigos-verificacion.md`), nunca por
  dirección.
- **Posición**:
  - el botín cae donde muere el gemelo. Si está congelado en su sitio de aparición, puede
    quedar lejos de donde murió la copia;
  - antes del remate se puede mover el gemelo a la última posición de E: el cliente de G
    controla sus NPC, así que basta un `MoveActorReq` inyectado. Esto depende de 3g [inferido].
- **La colisión la simula el cliente** (`CreateProjectileAck` → `DoFrameEventReq` de tipo 1).
  Con el gemelo oculto o sin físicas, puede que no impacte [inferido; prueba T4].
  - Alternativa: después de 1 s sin `NotifyHandleDeadAck`, `KillCheatReq`. Pierde la experiencia y
    el `KillActor`.
- **Participación:** solo hacer el remate espejo si G participó (le golpeó, o estaba a menos de
  X m durante la pelea). Así se evita botín gratis en peleas lejanas.
  - Si G no participó, dejar vivo al gemelo: el mundo de G solo cambia cuando G está presente.
- **Actores de misión:**
  - excluir los gemelos aparecidos por escenario o secuenciador (`cd_seq_quest_*`, stage);
  - excluir los que generan `GameEventLostMissionImportantActorAck`.

  Matarlos en G podría romper misiones de G.
- **Duplicación:**
  - cada mundo suelta su propio botín (botín por jugador, como en otros cooperativos) y cada
    jugador cobra su campamento;
  - es idempotente: un `KillCheatReq` o una colisión sobre un muerto no hace nada (L1);
  - aun así, marcar cada id de enemigo como "espejado" una sola vez.
- **Esfuerzo:** medio. El remate por colisión y `VaryStatCheatReq` ya están en el código (X2 y
  `guest_hits.cpp`). Falta el emparejamiento con el gemelo, que depende de 3g.

### B. Muerte espejo con `KillCheatReq` (R6 tal cual)

- Un solo mensaje y sin depender de la física.
- Da botín, buff y, probablemente, barra de campamento.
- **No da experiencia ni crédito de misiones o desafíos.**
- Es el plan B de A.
- Esfuerzo: bajo, con los mismos requisitos del gemelo.

### C. Crear objetos con trucos, sin gemelo

- **Peticiones:**
  - `SpawnItemToGroundByCheatReq` (2433): deserializador `0x2A03090` → núcleo `0x2C445D0`;
  - `CreateItemFromTrItemValueCheatReq` (2583): `0x2A02EB0` → `0x2C443F0`.
- **Permiso:** el mismo componente (`[[actor+0xA0]+0x68]+0x130` vf `0x140`, tipo 0) [verificado].
  Con `vf40 = mov al,1` debería pasar [inferido].
- **Origen de los objetos:** H reenvía la lista de su `LootDrop`. Las claves de objeto son datos
  estáticos, válidos en los dos PCs.
- **Formato sin decodificar:**
  - `SpawnItemToGround` lee: `u32`, `u64`, `u16`, vec3, dos estructuras (`0x141FA90`/`0x141FB70`),
    `u32`, otra estructura (`0x12AD8D0`), `u8`, `u64` y `u8`;
  - `CreateItem…` lee un `TrItemValue` completo.
- **Contras:**
  - sin experiencia, sin crédito y sin campamento;
  - **riesgo de partida corrupta** si el `TrItemValue` sale mal;
  - meter objetos de misión (`Quest_Paper_*`) fuera de orden.
- **Uso:** solo si 3g no deja gemelos. Filtrar los objetos de misión.
- Esfuerzo: alto, porque hay que decodificar el formato sin capturas.

### D. Golpes nativos de G sobre su gemelo (redirigir `DoFrameEventReq`)

- G cambia en sus `DoFrameEventReq` C→S la clave de la copia por la del gemelo. Su servidor
  calcula el daño y la muerte de forma nativa.
- Riesgos:
  - el servidor valida distancia y estado. Si el gemelo está congelado en otra posición, rechaza
    el golpe;
  - el HP del gemelo se desincroniza del de E.
- Esfuerzo y riesgo: altos. No recomendado.

### Descartado

- **Inyectar `AddSubLevelExperienceAck` o `LootDrop` S→C en el cliente de G.** Solo cambia la
  interfaz: el estado (y la partida) vive en el servidor de G [inferido].
- **Dar experiencia con una Req.**
  - `AddExperienceByGmReq` (2853, `0x2994CB0`) es la experiencia de personaje, que no se usa
    (`AddExperienceAck`: 0).
  - `VarySubLevelExpByResourceReq` (2441, `.xtls 0x104E1600` → `0x2C111C0`) gasta un recurso.
  - No hay ninguna Req que sume experiencia de subnivel gratis [verificado en la lista de 1.115
    mensajes].

---

## 5. Plan recomendado

1. **En H, ya (independiente de 3g): cambiar el remate letal de `guest_hits.cpp`.**
   - Hoy: `KillCheatReq`. Pasa a ser:
     1. `VaryStatCheatReq` hasta 1 HP;
     2. `AttackByCollisionReq(E, 0xA0100001, …, 0xF2EE2678)`;
     3. `KillCheatReq` como reserva si no llega `NotifyHandleDeadAck` en ~500 ms.
   - Así H recupera la experiencia y el `KillActor` de las muertes que remata G, y deja de haber
     motivos 15 y 16.
   - **No mandar nunca un `VaryStatCheatReq` que deje 0 HP.** Evita el estado fatal y el
     `DieByRunOutFatalReq` (motivo 15, sin crédito). El caso "HP desconocido" se cubre con el modo
     1 (fijar a 1) o con el colchón de `on_fatal`.
2. **En G: muerte espejo (opción A)**, cuando 3g mantenga gemelos ocultos y congelados.
   - Nuevo evento del protocolo `EnemyKilled {id estable, variante, punto de aparición, pos final,
     participó_G}`. Hay que subir `kVersion`.
   - Reglas:
     - solo con participación de G;
     - excluir actores de escenario o misión;
     - una sola vez por id.
   - Remate: `VaryStatCheatReq` a 1 HP y colisión. Reserva: `KillCheatReq`.
3. **No usar la opción C** salvo que 3g descarte los gemelos. En ese caso, decodificar primero
   `CreateItemFromTrItemValueCheatReq` con una prueba propia y filtrar los objetos de misión.
4. **Pieza 2:** mantener "nunca matar al jugador con trucos". El motivo 16 en el propio jugador
   se registra en la telemetría como `DevCheat`.

---

## 6. Pruebas en vivo

Un PC, `cdmp_peer` en eco. Antes, copia de la partida en `backups/`.

| # | Prueba | Predicción | Decide |
|---|---|---|---|
| **T1** | Sobre un bandido vivo de `Tier_2` (cualquier Bleed Bandit): `VaryStatCheatReq` hasta 1 HP y después `AttackByCollisionReq(E, A0100001, 0, pos, dir, 0xF2EE2678, 0)`. Control: `KillCheatReq` sobre otro bandido de la misma familia | Con colisión: `NotifyHandleDeadAck` (motivo 8, asesino `A0100001`), **`AddSubLevelExperienceAck(521, 15)` a ≤2 ms**, 20001 y botín si tiene máscara 2. Con `KillCheatReq`: motivo 16, **sin** `AddSubLevelExperienceAck` | §2.2-2.3 y el cambio del punto 1 del plan |
| **T2** | En un campamento con la barra activa (sin liberar): `KillCheatReq` sobre un miembro | `UpdateQuestGaugeAck +1`. Si no llega, la barra depende de `KillActor` y la opción B pierde también el campamento | §2.4, campamentos |
| **T3** | `VaryStatCheatReq(E, HP, modo 1, valor 1, 0, 0)` | `VaryHPAck` con hp = 1000 (1 HP), sin `NotifyHandleFatalAck` | Fijar a 1 HP con HP desconocido |
| **T4** | Simulación de R6 en un PC: con el gemelo oculto como lo deje 3g (o un enemigo "fuera" de la burbuja), T1 sobre él | Si el proyectil impacta: muerte con motivo 8 y experiencia. Si no hay `DoFrameEventReq` de tipo 1 ni `VaryHPAck` en 1 s: la colisión no sirve con gemelos ocultos y se queda el plan B | Opción A con 3g |
| **T5** | Misión de "elimina N" activa (o un desafío `SealedArtifact` de lanza o pistola): rematar uno con T1 y otro con `KillCheatReq` | `UpdateMissionListAck` (progreso) solo con T1 | `KillActor` → misiones (§2.4) |
| T6 | Después de T1/T5: la pantalla de habilidades (puntos de 521) y guardar y cargar | La experiencia persiste tras recargar. Captura con `CopyFromScreen` | Que el progreso se guarda |

---

## 7. Preguntas abiertas

1. ¿Qué es `[[muerto+0x68]+0x20]+0x250`, el crédito sin datos de ataque? Si fuera el "último
   atacante", podría rellenarse antes del remate y `KillCheatReq` daría `KillActor`. Basta con
   leerlo en memoria antes y después de un golpe normal.
2. ¿La barra de campamento cuelga de `DeadActor`? (T2)
3. ¿Impacta la colisión en un gemelo oculto o congelado? (T4, depende de 3g)
4. ¿Qué logros dependen de los desafíos con `KillActor`? Hay que cruzar
   `platformachievementinfo` con las misiones `Challenge_*`; la tabla no se parseó entera.
5. ¿El botín de la variante incluye objetos de misión que G no debería recibir (por ejemplo
   `Quest_Paper_Kwe_Baris`)? Con la opción A los da el servidor de G según sus propias reglas,
   así que no es un problema. Con la C, sí.
6. ¿Se envía de verdad la telemetría `CharacterEventFunnelFormat`, y adónde? (§2.6)

---

## 8. Relación con otros temas

- **Supresión 3g (estudio aparte):** la opción A necesita que el servidor de G **conserve** los
  gemelos, ocultos y quietos, y poder moverlos (`MoveActorReq`) antes del remate.
  - Si 3g los filtra al aparecer, G se queda sin experiencia ni crédito (solo con la opción C, y
    con riesgo).
- **`guest_hits.cpp`:** el remate actual (`KillCheatReq`) y el colchón `on_fatal`
  (`DieByRunOutFatalReq`) producen los motivos 16 y 15, que **no dan experiencia a H**. Cambiarlo
  como en §5.1.
- **`enemy_relay`:** el evento de muerte que ya se reenvía a G (`NotifyHandleDeadAck` traducido)
  puede llevar el id estable, la variante, el punto de aparición y la posición final para el
  remate espejo.

---

## 9. Scripts, volcados y direcciones (build 2976)

Carpeta `build/research/p_rewards/`. Ejecutar con `build/research/venv/Scripts/python.exe`. Usan
`../pieza4/pk.py`.

| Script / fichero | Qué hace |
|---|---|
| `r_global.py` → `r_global.txt` | Recuento global de los opcodes de recompensa y progreso en las 50 capturas |
| `r_after.py <ventana>` → `r_after_1.txt`, `r_after_6.txt`, `r_after_*.tsv` | Mensajes tras cada muerte, por muerto, asesino y motivo |
| `r_buffs.py`, `r_b20001.py` | Buffs que recibe el jugador tras cada muerte; payloads de 20001 frente a la experiencia |
| `r_dump.py <op> [n] [sesión]` | Volcado hexadecimal de un opcode |
| `r_writers.py` | Escritores de los Ack de progreso (desde `combat/ackwriters.pkl`) |
| `r_sinfo.py <tabla> [clave…]` | Lista o vuelca registros de `staticinfo` (`sublevelinfo`, `skill`, `questgaugeinfo`, `conditioninfo`, `iteminfo`…) |
| `r_gameevent_enum.py` → `gameevent_enum.txt` | Enum `GameEventType` completo (110 valores) |
| `r_scan61.py <imm>` → `scan61.txt` | Funciones que escriben un tipo de evento (`0x61`/`0x62`) |
| `r_mission_events.py [tabla]` | Misiones y subniveles con evento y condición (heurístico) |
| `r_cixp.py`, `r_enum.py`, `r_st250.py` | Intentos auxiliares: experiencia en `CharacterInfo` (no está), tablas de nombres, escritores de `+0x250` (sin resultado) |
| `d_29431a0_killevent.asm` | Emisión de `PlayerDead`, `DeadActor` y `KillActor` |
| `d_2941d60_vf96.asm`, `d_c76cf30_postdeath.asm`, `d_10433bd0_vf43.asm` | Cadena de la muerte |
| `d_0x107c73d0.asm`, `d_0x107cca50.asm`, `d_0x2c14650.asm`, `d_0x2c12ec0.asm` | Registro y manejador de la experiencia por evento |
| `d_2accac0_expbuffproc.asm` | `ServerStatusExpBuffProcessor` |
| `d_0x2a03090.asm`, `d_2c445d0_spawnitem_core.asm`, `d_0x2a02eb0.asm`, `d_0x2994cb0.asm`, `d_0x104e1600.asm`, `d_2c111c0_varysublevelres.asm` | Peticiones de objetos y experiencia por truco o GM |
| `d_ach_*.asm` | `PlatformAchievementManager_Steam` |
| `up_2a66410.txt`, `up_29431a0.txt` | Árboles de llamadores (`pieza4/uptree.py`) |

### Direcciones

| RVA | Qué es |
|---|---|
| `0x29431A0` | Eventos de juego de una muerte: `PlayerDead` 0x6B, `DeadActor` 0x62 (`0x29432F2`), `KillActor` 0x61 (`0x2943B8B`, se emite en `0x2943CA1` con `0x1804360`) |
| `0x29433EF`-`0x2943771` | Resolución del acreditado (datos de ataque / `+0x250` / `Fire` 0xB) |
| `0x2941D60` | `ServerStatusActorComponent` vf96 (llama a `0x29431A0`; motivo en `r8b`) |
| `.xtls 0xC76CF30` (vía `0x1797850`) | Post-muerte: `+0x272`/`+0x273`, vf96 (`+0x300`) solo si hay asesino, vf97 (`+0x308`) |
| `.xtls 0x10433BD0` (vía `0x2941B60`) | Status vf43: `NotifyHandleDeadAck` y post-muerte; argumentos del registro de daño |
| `0x2C41226`-`0x2C4125D` | `KillCheatReq` → vf43 con el registro `[rbp-0x60]` |
| `0x15CCD27`… / `0x6A6A240` | Registro del enum `GameEventType` |
| `.xtls 0x107C73D0` | Registra el manejador de subniveles para 9 tipos de evento (`0x1ABB4D0`) |
| `.xtls 0x107CCA50` (`0x2C14650` → `0x2C12EC0`) | Experiencia de subnivel por evento: condición `0x1AAD770` y suma con `0x2C0FC00` |
| `0x2C0FC00` → `.xtls 0x107CBD10` | Sumar experiencia de subnivel |
| `0x2A66410` | Escritor de `AddSubLevelExperienceAck` |
| `0x2ACCAC0` | `ServerStatusExpBuffProcessor` vf2 (buff de tipo 9 → subnivel) |
| `0x2942350` (`0x29425E6`-`0x29426E1`) | vf97: telemetría `CharacterEventFunnelFormat` `dead_reason` (solo jugadores) |
| `0x57C7BD0` | Nombres de los motivos de muerte (8 `BuffDamage`, 15 `Fatal`, 16 `DevCheat`…) |
| `0x2A43C90` / `0x2A44190` / `0x2A41E20` / `0x2A476A0` / `0x2A47230` | Escritores de `UpdateQuestGauge` / `CompleteQuestGauge` / `UpdateMissionList` / `CompleteChallenge` / `SyncChallenge` (el primero, por descarte) |
| `0x2992330` / `0x29EDBE0` | Escritores de `AddExperienceAck` (no se usa) / `AddItemToInventoryAck` |
| `0x2A03090` → `0x2C445D0` | `SpawnItemToGroundByCheatReq` (permiso vf `0x140`(0)) |
| `0x2A02EB0` → `0x2C443F0` | `CreateItemFromTrItemValueCheatReq` |
| `0x2994CB0` → `0x2C3EBC0` | `AddExperienceByGmReq` |
| `0x2A68D60` → `.xtls 0x104E1600` → `0x2C111C0` | `VarySubLevelExpByResourceReq` (4 B, gasta un recurso) |
| `0x57D6B28` | vtable de `PlatformAchievementManager_Steam` (`0x18422E0`, `0x1842350`, `0x1842430`, `0x18411D0`) |

### Datos estáticos

| Tabla | Clave | Contenido |
|---|---|---|
| `sublevelinfo` | 521/522/523 | `SkillPoint_Kliff/Oongka/Damian`: evento `KillActor`, requisito `0xF642E`, categorías `0xF642F`-`0xF6433` → 10/15/25/140/1000 |
| `sublevelinfo` | 601 | `Contribution_Hernandian` (+1 interacción, +100 barra, −100 asesinato con testigos `0xF6474`) |
| `sublevelinfo` | 701 | `LiberationRefugee` |
| `skill` | 20001 | `Skill_Active_Killer` → `BuffLevel_RedLightning` (1000027) y `BuffLevel_CombatHigh` (1000078) |
| `questgaugeinfo` | `0xF4242`, `0xF4256`, `0xF4262`, `0xF43A7`… | Barras de liberación de campamentos |
| `platformachievementinfo` | — | 34 logros de Steam |
