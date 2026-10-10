# Pendiente: jefes y encuentros con guion en el cooperativo

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`) y
solo valen para esta build.

Investigación de solo lectura:
- capstone sobre el exe en disco (`cdpe.py`, `cddis.py`, `horse2/xtrace.py` para el código en `.xtls`);
- los datos del juego extraídos de los PAZ: tablas `gamedata`, ficheros de UI y los
  `.pastage`/`.paseqc`/`.paseq` del secuenciador;
- las 58 capturas de paquetes (`bin64\cdmp_packets_*`, `D:\cdmp-grabaciones`, `captures\`);
- el código actual de `src/mod/net/`.

No se ejecutó el juego ni `cdmp_peer`, y no se tocó el código del mod.

Marcas:
- **[verificado]**: leído en el código o en los datos del juego;
- **[captura]**: medido en las capturas;
- **[inferido]**: deducción que hay que confirmar en vivo (la §6 dice con qué prueba).

Notación:
- **H**: anfitrión; **G**: invitado;
- copia: `0xB01F0000+n`;
- jugador: `0xA0100001`;
- marioneta del otro jugador: `0xA010F001`.

Scripts, tablas y volcados en `build/research/p_boss/` (§9).

---

## 0. Resumen

1. **Un combate de jefe es una "etapa" del secuenciador, por jugador y dirigida por el cliente.**
   [verificado + captura]
   - Cada jefe tiene un `.pastage` (gráfico de etapa) y un `.paseq` (línea de tiempo):
     - el `.paseq` hace aparecer al jefe (`ActorTrackNodeData_SpawnCharacter`, nodo `Boss_00`) y
       las cinemáticas;
     - el `.pastage` decide las transiciones.
   - Todas las condiciones hablan de **`Player.`**, el jugador local: `CheckEnterTrigger`,
     `CheckAggroToPlayer`, `condition_uiInput`…
   - El cliente manda `StageBranchStateReq`/`UpdateStageChartEventReq` (C>S) y el servidor contesta
     con `StageBranchStateAck`/`StageOnBranchEventAck`. **Todos llevan la clave de usuario del
     jugador** (`04 │ 0xA0100001 │ id de etapa u64 …`).
2. **125 etapas muestran la barra de jefe** (`BossNameTagView2`), de 3.552 `.pastage` [verificado].
   Hay tres familias muy distintas (§2.2):
   - **34 "bossencounter"** (`*_bossencounter_stand/ambush`): sin `noDie`, sin barrera, sin pantalla
     de carga ni recuperación de HP. Empiezan con `Boss_00.CheckAggroToPlayer()`. **Son el mejor
     primer objetivo.**
   - **59 de misión** (`cd_seq_quest_*`):
     - `noDie` en 47;
     - barrera en 29;
     - pantalla de carga (`set_loading_stage`) en 14;
     - estado `groggy` en 15.
   - **24 `cd_boss_*`**: `noDie` en 23 y barrera en 13.
3. **En el mundo de G el jefe no existe.** [inferido, muy probable]
   - Solo lo crea la etapa activa de H.
   - Por eso **la copia de H es la única forma de que G lo vea**, y no hay gemelo local que
     suprimir. Salvo que G esté justo en la misma misión: §3.9.
   - El relevo actual ya quita la identidad de secuencia (`strip_spawn_identity`, `T0+1043`): la
     copia llega como un enemigo ambiental.
4. **Lo que se rompe hoy si H pelea contra un jefe con G al lado:**

   | # | Problema | Gravedad | Detalle |
   |---|---|---|---|
   | a | **`KillCheatReq` en el golpe letal de G** y **`DieByRunOutFatalReq` tras un golpe de G** (`guest_hits.cpp`) | Alta | Pueden matar al jefe fuera del guion: sin `BossPhase`, sin la cinemática final y sin `set_battleEnd`. La barrera se quedaría puesta y la misión de H se atascaría [inferido] |
   | b | **`VaryStatCheatReq` no pasa por la comprobación de inmune o invulnerable** (`0x209CB30`) | Alta | G puede quitar HP en ventanas de invulnerabilidad y durante las cinemáticas [inferido de `pieza4-dano-servidor.md` §1.1] |
   | c | **El tambaleo inyectado** (AAR + buff `CC_Push…`) | Media | Puede meter al jefe en una reacción que su IA de fases no espera |
   | d | **Copias con tipo de registro ≠ 3** | — | El relevo solo copia el tipo 3. Si un jefe llega como tipo 4 (personaje con nombre) o 5 (vehículo: tanques, dragón, avión), hoy no se copia. Un tipo 5 además tiene el riesgo de cierre conocido (`0x925F0E`) |

5. **Lo que sí funciona solo con lo que ya hay** [inferido]:
   - HP y muerte del jefe por `VaryHPAck` y `NotifyHandleDeadAck`;
   - acciones y posiciones de su IA, que corre en el cliente de H;
   - sus golpes a la marioneta, por `DoFrameEventReq` → `PlayerHit`;
   - el cambio de fase, porque lo decide la IA de H al ver el HP: también sirve el HP que quite G.
6. **Señales limpias para el mod** [captura]:
   - **`SetAvoidByCutSceneAck` (2561)**: `u8 4 │ u32 usuario │ u64 id de etapa │ u8 bandera │ u32
     usuario`. Bandera 1 al empezar una cinemática de etapa y 0 al acabar (338,0 → 345,8 s en
     `084001`).
   - La carga del jefe lleva en `T0+1043` el hash del `.paseq`; la tabla de los 125 está en
     `boss_tables.tsv`.
7. **Barra de vida del jefe en G**:
   - la nativa la enciende la etapa (`set_ui_hpBar` → `UIControl` sobre `BossNameTagView2` con
     `selector-stagechart-self`). Sin etapa, G no la verá [inferido];
   - lee los stats `Hp`, `KnockOut` (barra de aturdimiento) y `Fury` del actor [verificado en el
     HTML];
   - **la vía segura es dibujarla en el overlay de ImGui** con el HP relevado.
8. **Crédito y misiones**:
   - la etapa, la recompensa y el avance de misión son de H;
   - **G no avanza su misión ni recibe botín** (como en Genshin);
   - para que avanzaran los dos haría falta que cada mundo jugara su propia etapa con jefes gemelos
     (§3, enfoque D): eso queda para más adelante.

**Recomendación** (§5):
- **ya**:
  - modo "jefe seguro" en `guest_hits`: nada de `KillCheat` ni de remate de "fatal", sin tambaleo,
    HP ≥ 1, daño escalado y nada durante las cinemáticas;
  - detección de "combate de jefe";
  - barra de jefe en el overlay;
- **fallback**: pausar el combate compartido con jefes de misión que cargan nivel o cambian de
  campo;
- **después**: tipos 4 y 5, partes del jefe, agarres y jefes gemelos.

---

## 1. Evidencia

### 1.1 Datos del juego [verificado]

**Personajes** (`characterinfo`, `build/research/ids/characterinfo_keys.tsv`):
- 120 entradas con "Boss" en el nombre: `Boss_*` (jefes), `MiddleBoss_*` (semijefes),
  `Animal_Boss_*`, `Riding_*_Boss` y clones. Ejemplos:

  | Clave | Nombre interno |
  |---|---|
  | 55504 / 55505 | `Boss_ReedDevil_DualSword` / `…_Clone_DualSword` |
  | 1000561 → 55529 | `Boss_Caliburn_PreAwakening` → `Boss_Caliburn_Awakening` (+ `…_Clone_1…4`) |
  | 60010 / 1000271 / 1001338 | `Boss_Ludvig` / `…_Awakening` / `…_OneArmed` (variantes por fase) |
  | 1001690 | `Boss_WhiteBear_51164` |
  | 60003 / 60004 | `Boss_Dragon_*` |
  | 30194 | `Animal_Boss_Cucubird_Wild` ("Alpha Kuku Bird", el único jefe visto en capturas) |

- Hay enumeraciones de tipo de jefe registradas en el exe (`enumdump.py`):

  | Función | Valores | Para qué [inferido] |
  |---|---|---|
  | `0x1980E0` | `Empty, Tier_1, Tier_2, Tier_3, Elite, Boss` | `CharacterTier` (`_characterTier` de `characterinfo`) |
  | `0x198250` | `None, Boss, FieldBoss, Ally, Mercenary, Vehicle` | tipo de etiqueta o barra de vida |
  | `0x197350` | `Phase, QTE, Skill, BossPhase, Reset, Runaway, Fail` | estados de IA que lee la etapa (`condition_AI_*`, `AIReadyToNextState(Boss_00, X)`) |
  | `0x196B70` | `Spawn, Action, Quest, World, QuestBoss(8), ClientOnly, SequencerSpawn, NPCScheduleSpawn…` | motivo de aparición |
  | `0x198EA0` | `…, AddBossDamagePercent, …` | bonificaciones; los jefes tienen su propio multiplicador de daño |

**UI de jefe** (`ui/bossnametagview2.html`):
- script `UIGamePlayControlRootBossNameTag`, con `selector-stagechart-self="#BossNameTagBody"`: la
  controla la etapa;
- componentes `BossNameTag` y `BossNameTagMulti` (varios jefes);
- barras ligadas a `statName="Hp"`, `"KnockOut"` y `"Fury"`, con tres fondos de fase
  (`boss-hp-progress-bar-max-bg-phase-1..3`);
- el cliente tiene además una tarea periódica "`RefreshNearBossList`" (cadena `0x55AE1F0`, la
  registra `0x7C7240`; el constructor de la tarea, `0x7DB0C0`, está virtualizado). Qué lista no se
  pudo leer.

**Niveles y modos aparte** (`idx_boss.txt`):
- `leveldata/*boss*_phase00_00.palevel`, `boss_story*.palevel`, `bossrematch*.palevel`,
  `abyss_bossrush_000N_phase00_00.palevel`: capas de nivel por fase y modos "revancha" y "boss rush"
  en su propio nivel;
- gimmicks propios del jefe: `gimmick_boss_ice_*`, `…_skullknight_pillar_*`,
  `…_marni_golemdragon_part_*`, `gimmick_attach_circusmachine_boss_*` (partes rompibles),
  `boss_reeddevil_totem_01`.

### 1.2 Cómo es un combate de jefe (`.pastage` + `.paseq`) [verificado]

Etapa típica de jefe de misión (`cd_boss_blissredknight_9000`):

```
PLAY_0 ── Player.CheckFrontEnterTrigger(Trigger_00) ──► SCENE_1 (cinemática de entrada)
            set_characterBarrier, set_limit_safeZone
PLAY_2 (pelea): set_noDie, set_allyGroup_exclusiveA/B, set_keepAggroPlayer("Player"),
                set_ui_hpBar(BossNameTagView2), set_ui_ccBar(CCGauge), set_battleBoundary, set_phase
   ├─ AIReadyToNextState(Boss_00, BossPhase) ──► SCENE_3 (cinemática de fase o final)
   ├─ Player.CheckEnterTrigger(Trigger_01)_MatchCount<1 ──► Fail (el JUGADOR salió del área)
   └─ AIReadyToNextState(Boss_00, Reset) ──► Reset (recuperación de HP: _recoverHpOnReset)
Terminate: set_battleEnd
```

Encuentro de mundo (`cd_seq_whitebear_bossencounter_stand`):

```
PLAY_0 ── Boss_00.CheckAggroToPlayer() ──► PLAY_1 (set_aggroPlayer, set_ui_hpBar_full, set_battleBoundary)
   ├─ AIReadyToNextState(Boss_00, BossPhase) ──► PLAY_2
   ├─ !Boss_00.CheckAggroToPlayer() ──► PLAY_3 (set_battleEnd_onExit)
   └─ AIReadyToNextState(Boss_00, Runaway) ──► Terminate (huye)
```

En el `.paseq`/`.paseqc` (reflexión con nombres de campo) aparecen:
- `ActorTrackNodeData_SpawnCharacter` (`_spawnCharacterKey`, `_spawnReason`, `_gamePlayLevel`);
- `SequencerControl_AI`, `SequencerControl_Input` y `SequencerControl_Off`;
- `GameData_CatchData` (agarres con guion);
- `SequencerCustomEventQuickTimeEvent`;
- `_reviveTransform` y `_recoverHpOnReset`;
- `_sequencerBoundaryMin/Max` y `_filterBoundaryMin/Max`;
- `_isGamePlayTriggerEnabledOnServer`.

En el exe hay procesadores de etapa en los dos lados: `ClientSequencerStage_StageChartProcessor` y
`ServerSequencerStage_StageChartProcessor`. Hay 66 clases `StageChart_Function_*` (contando las
bases); entre ellas:
- `SetPhase`, `KeepAggro`, `SetAggro`, `SetBattleTarget`, `ForceLockOn`;
- `UIControl`, `LetterBox`, `InputBlock`, `Loading`;
- `CompleteStage`, `Reward`, `PushReward`, `BattleEnd`, `UnsummonActor`.

### 1.3 Capturas [captura]

Ninguna de las 58 capturas tiene un combate de jefe:
- HP máximo de cualquier NPC ≤ 220;
- 0 cargas de `Boss_*`; solo 39 del `Alpha Kuku Bird` (tipo 3).

Lo que sí se ve es el tráfico de etapas de misiones normales (`scan_caps.py`, `stage_msgs.py`):

| Mensaje (op) | Dir. | Total | Formato / nota |
|---|---|---|---|
| `UpdateStageChartEventReq` (2591) / `Ack` (2111) | C>S / S>C | 5.818 / 5.536 | 17 B: `u64 id de etapa │ u64 evento │ u8` / Ack 22 B: `04 │ usuario │ …` |
| `StageBranchStateReq` (2436) / `Ack` (2981) | C>S / S>C | 2.027 / 2.983 | Req 14 B (id de etapa); Ack 29/37 B con `04 │ usuario │ id │ … │ hash` |
| `StageOnBranchEventAck` (2524) | S>C | 2.027 | 18 B, `04 │ usuario │ id` |
| `PrepareSequencerAck` (2661) / `DeleteSequencerAck` (2810) | S>C | 1.023 / 781 | Llevan `usuario │ usuario │ id │ posiciones` |
| **`SetAvoidByCutSceneAck` (2561)** | S>C | 12 | **Inicio y fin de cinemática de etapa**. Llega junto a `UpdateStageChartEventAck` y `StageOnBranchEventAck` |
| `UpdateStageStateAck` (2264) | S>C | 2.857.130 | 29 B, ~190/s: el estado de todas las etapas activas (ruido) |
| `GamePlayTriggerInOutReq` (3068) / `SequencerTriggerVolumeInOutReq` (2767) | C>S | 67.669 / 7.010 | El cliente comunica entradas y salidas de disparadores |
| `CatchActionResultAck` / `ReleaseCatchResultAck` / `CatchNak` | S>C | 89 / 63 / 10 | Agarres del jugador |
| `ReviveReq` / `ReviveAck` | | 3 / 6 | |

Conclusiones:
- la etapa es **por usuario** (todo lleva `0xA0100001`);
- **la empuja el cliente** con su jugador.

### 1.4 La vida tiene un mínimo por stat [verificado]

`0x17B3230` → `.xtls 0xC7E0860` → `0x17B3E10` → **`.xtls 0xC7E7960`** (núcleo del cambio de stat):
- con delta negativo, `nuevo = actual + delta`, recortado a **`[stat+0x28]` (mínimo)**;
- si ya estaba en el mínimo, no cambia nada;
- con delta positivo, recorta a `[stat+0x18] + [stat+0x20]` (máximo).

Toda la ruta de HP pasa por ahí, incluidas `VaryStatCheatReq` y `KillCheatReq`
(`0x17AE080` → `0x17AC040` → `0x17B3230`).

- Si `set_noDie` sube ese mínimo, el jefe no muere ni con trucos [inferido].
- `SetMinStatRateAck` (2645, 105 B, handler `0xB77060`) es el candidato a escribirlo: llega en la
  carga de cada actor con `u32 0x000F4240` (HP) y tres valores. Falta verlo cambiar en un jefe con
  `noDie` (prueba B0).

### 1.5 El mod hoy [verificado en el código]

| Pieza | Qué hace | Con un jefe |
|---|---|---|
| `enemy_relay` | Copia solo cargas de **tipo 3** (`candidates()`, `load[2] != 3`). Quita la identidad de spawn, el grupo, el padre y los hijos | El jefe se copia si es tipo 3. Sus partes de tipo 7 (gimmicks `attach_*`) no |
| `player_hits` | `DoFrameEventReq` de un enemigo relevado con la marioneta entre los objetivos → `PlayerHit` → G se quita HP con `VaryStatByAccInputReq` (nunca por debajo de 1; como mucho la mitad del máximo por golpe) | Funciona para golpes directos del jefe. **No** para golpes cuyo atacante es un gimmick, un proyectil de un gimmick o una invocación no relevada |
| `guest_hits` | Golpe de G a la copia → en H: `VaryStatCheatReq` (o **`KillCheatReq` si es letal**), `DieByRunOutFatalReq` si queda "fatal" antes de 1,5 s, y tambaleo AAR + buff | Los tres puntos de §0.4 (a-c) |

---

## 2. Qué se rompe, punto por punto

### 2.1 Entrada al área e instancia

- **El terreno sí está en el mundo de G**: 89 de 125 etapas comprueban `CheckField(100)` (el campo
  principal).
- **Lo que no está en G:**
  - el jefe;
  - la barrera (`set_characterBarrier`, 47 etapas);
  - los gimmicks del área: pilares, hielo, tótems, partes;
  - las capas de nivel por fase (`set_loading_stage`, 15 etapas; `*_phase00_00.palevel`).
- **Consecuencias en G** [inferido]:
  - la copia del jefe flota o se hunde donde la capa de fase cambia el suelo;
  - G puede entrar y salir del área sin barrera;
  - si H entra por un `set_loading_stage`, en H hay pantalla de carga (retirada en masa con motivo
    19 → `EnemyReset`) y luego un nivel que G no tiene.
- **Modos aparte**:
  - revancha (`bossrematch*.palevel`, `MoveFieldAck` tras `BossRematchNoticePanel/Rematch`) y "boss
    rush" (`abyss_bossrush_*`): **H cambia de campo**. G se queda en el suyo y no hay nada que
    compartir. Fallback obligatorio.
- **Los disparadores solo miran al jugador de H**:
  - la entrada de G no arranca nada;
  - si **H** sale del área (`Trigger_01_MatchCount<1`, 111 de 125), la etapa falla aunque G siga
    peleando.

### 2.2 Familias de etapa (`fights.py`, 125 etapas con barra de jefe)

| Familia | n | `noDie` | barrera | límite de batalla | aggro fijo al jugador | falla al salir | carga de nivel | recupera HP | `groggy` | `kill` | límite de montura |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `*_bossencounter_*` | 34 | 0 | 0 | 14 | 20 | 20 | 0 | 0 | 0 | 0 | 0 |
| `cd_boss_*` | 24 | 23 | 13 | 24 | 24 | 24 | 0 | 9 | 2 | 6 | 20 |
| `cd_seq_quest_*` | 59 | 47 | 29 | 52 | 50 | 59 | 14 | 20 | 15 | 16 | 37 |
| otras (duelos, facciones…) | 8 | 3 | 5 | 8 | 8 | 8 | 0 | 3 | 5 | 5 | 8 |

- Fases (`condition_AI_BossPhase`) en 118 de 125.
- Seis etapas tienen dos jefes (`Boss_00` y `Boss_01`): `cd_boss_titan_9999_new`,
  `…ludvig_boss_9900`, `…mjordin_boss_9000_dlc`, `…marni_machineknight_9000`…

### 2.3 Cinemáticas y transiciones de fase

- **En H:**
  - la cinemática (`SCENE_n`) la reproduce el cliente de H desde el `.paseq`, con el jefe bajo
    `SequencerControl_AI` (animaciones de la línea de tiempo);
  - la entrada de H se bloquea (`SequencerControl_Input`, `LetterBox`).
- **En G** [inferido]:
  - es probable que la copia no reciba `StartActionReq` durante la cinemática: se queda quieta o
    con su última acción, y luego "salta" a la posición final (la posición se lee de la memoria de
    H a 15 Hz, así que el salto es corto);
  - G sigue pudiendo pegarle (ver §2.5) y el jefe de H no le pega.
- **Señal para el mod**: `SetAvoidByCutSceneAck` con bandera 1 o 0 en H. El id de etapa `u64` la
  liga a la `StageBranchState` [captura].
- **Fases con otro actor**:
  - `Boss_Ludvig` → `…_Awakening`, `Caliburn_PreAwakening` → `Awakening`, clones `…_Clone_N`;
  - son cargas y retiradas nuevas: el relevo ya las trata como enemigos nuevos [inferido].
  - Hay que **conservar la etiqueta "jefe" por etapa, no por clave**.

### 2.4 Agarres, QTE e invocaciones

- **Agarre de combate a la marioneta en H** [inferido]:
  - el servidor de H no tiene la marioneta (solo existe en el cliente), así que no saldrá
    `CatchActionResultAck` para ella: el agarre falla en H;
  - en G la copia hace la animación de agarre en el aire.
  - **Plan**: tratarlo como un golpe (`PlayerHit` con el daño aprendido). Nada de enganchar al
    jugador de G.
- **Agarres con guion** (`GameData_CatchData` en el `.paseq`): son de cinemática, sobre el jugador
  de H.
- **QTE**:
  - solo 1 de 125 etapas usa `Player.CheckQteResult`; los QTE van en la línea de tiempo
    (`SequencerCustomEventQuickTimeEvent`);
  - los hace H con `RequestQuickTimeEventResultReq` (2236, C>S, por jugador); G mira.
- **Invocaciones y gimmicks de ataque** (tótems, hielo, torretas, `SummonGimmickFrameEventResultAck`
  2913, `NotifySummonCharacterAck` 2965):
  - su `DoFrameEventReq` tiene como atacante el gimmick, no el jefe, así que `player_hits` lo ignora
    (`enemy_variant()` = 0);
  - G no recibe esos golpes. Los proyectiles disparados por el propio jefe sí, si su
    `DoFrameEventReq` sale con el jefe como atacante [inferido].

### 2.5 Daño de G al jefe

- **Invulnerabilidad** [inferido de §1.4 y `pieza4-dano-servidor.md` §1.1]:
  - `VaryStatCheatReq` va directo a `0x17AE080` y no pasa por `CommonDamageBuffProcessor`
    (`0x209A220` → `0x209CB30`, inmune o invulnerable);
  - así que G quita HP en ventanas de invulnerabilidad (escudos, fases, cinemáticas).
  - Lo que sí respeta es el **mínimo del stat** (§1.4).
- **Letal**:
  - `KillCheatReq` mata fuera del guion si `noDie` no es un mínimo de stat;
  - y si lo es, el jefe se queda en el mínimo y el log dirá "lethal" sin muerte.
  - En los 23+47 jefes con `noDie`, la muerte buena es la que lleva la IA a `BossPhase`.
- **Remate de "fatal"**:
  - 22 etapas usan `condition_groggy`: los jefes tienen un estado "fatal/groggy" nativo;
  - `on_fatal` manda `DieByRunOutFatalReq` si G golpeó en los últimos 1,5 s. En un jefe eso se
    salta la secuencia de remate [inferido].
- **Aturdimiento**:
  - la barra `KnockOut` del jefe solo la llena el daño nativo de H; el de G no la toca
    (`VaryStatCheatReq` solo cambia el HP);
  - el AAR inyectado no pasa por esa barra.
- **Escalado**:
  - el HP del jefe es el de un jugador y no se puede subir su máximo;
  - con dos jugadores el combate se acorta;
  - `AddBossDamagePercent` existe como bonificación, pero no para esto.

### 2.6 Aggro y objetivo

- `set_keepAggroPlayer` (102 de 125) fija el aggro al jugador de H: el jefe casi no mirará a la
  marioneta. Para estabilidad es bueno.
- **Ojo en los encuentros**: `!Boss_00.CheckAggroToPlayer()` cierra la etapa
  (`set_battleEnd_onExit`).
  - Si el jefe pasa su aggro a la marioneta (gancho `enemies_target_copy`, AAR de G con atacante =
    marioneta), **el encuentro de H puede terminar solo**.
  - Para jefes: no forzar el aggro a la marioneta [inferido].

### 2.7 Crédito, botín y misiones

- La muerte la registra `0x17AC040` con el asesino que haya: H, o G vía `VaryStatCheatReq` (fuente =
  jugador de H).
- La etapa cierra con `CompleteStage`, `Reward` y `PushReward` **en el servidor de H**, para el
  jugador de H.
- **G no avanza nada** [inferido]:
  - su mundo no tiene esa etapa activa, o la tiene pero sin el jefe de H;
  - el botín sale en el mundo de H según la variante (`pieza4-botin-reaccion.md` §1).
- Avanzar a G haría falta:
  - con su propia etapa y su propio jefe (enfoque D); o
  - con trucos de desarrollador: `CompleteQuestByCheatReq` 2627 (`0x2A48930`),
    `StageChartForceBranchDevReq` 2476 (`0x2A5D330`). Saltan contenido y escriben en la partida.
    **No.**

### 2.8 UI en G

- **Sin barra nativa** [inferido]: la enciende la etapa (`set_ui_hpBar` es un `UIControl` sobre
  `BossNameTagView2`).
- **Sin icono de jefe en el mapa** (`MapIcon_ActorEnemyBoss`, cadena `0x566B418`, lo usa
  `0xDA18F0`). Puede que salga si el icono depende del `CharacterTier` (prueba B1).
- **El overlay** ya dibuja barras de HP (Hito 3): una barra de jefe arriba y centrada, con el nombre
  UTF-16 del registro de carga, es inmediata.

### 2.9 Si G también está en esa misión

- Si G entra en el disparador de su propia etapa, su mundo arranca su cinemática y su jefe: **doble
  jefe**.
- El mod debería detectarlo en G: una carga propia con `T0+1043` en la tabla de jefes, dentro de la
  burbuja de H.
- Ante eso: avisar y **no relevar la copia de H** mientras exista el jefe propio (o al revés). No
  bloquear Req de etapa: escriben estado de misión.

---

## 3. Enfoques

| | Enfoque | Qué da | Riesgo | Esfuerzo |
|---|---|---|---|---|
| **A** | **Jefe como enemigo relevado + reglas "jefe seguro"** | G ve el jefe y le pega; las fases, cinemáticas y el final los lleva H de forma nativa; G recibe los golpes directos | Bajo-medio: escribe en memoria y en el servidor de H igual que hoy | S-M (1-2 días) |
| **B** | **Pausa en zona de jefe** (fallback) | Sin combate compartido: G ve "H está en un combate de jefe". El relevo y los golpes se apagan para ese jefe | Muy bajo | S (½ día) |
| **C** | **Espectador en cinemáticas** (parte de A) | Durante `SetAvoidByCutSceneAck=1` en H, G no hace daño, no recibe golpes y ve un aviso | Muy bajo | XS |
| **D** | **Jefes gemelos** (cada mundo juega su etapa) | Avance de misión y botín para los dos | Alto: dos IA que divergen, exige el mismo punto de misión, emparejar por `(hash .paseq, nodo)` y sincronizar HP con trucos en los dos servidores | L (semanas) |

Detalle de A:
1. **Marcar jefes en H.** Una carga es "jefe" si cumple cualquiera de estas:
   - su `characterinfo` (`+0x18`) está en la lista de `Boss_`/`MiddleBoss_` de esta build;
   - su `T0+1043` está en la tabla de 125 etapas (`boss_tables.tsv`).

   Es una tabla por build, como las de facciones. Viaja como bandera en `EnemySpawn`.
2. **Tipos**:
   - aceptar el tipo 4 solo si es jefe;
   - **seguir excluyendo el tipo 5** (vehículos: tanque, avión, dragón) hasta resolver la rama de
     vehículo.
3. **`guest_hits` con jefe**:
   - sin `KillCheatReq`: el golpe se recorta a `hp − 1` y el remate es de H;
   - sin `DieByRunOutFatalReq`;
   - sin tambaleo;
   - multiplicador `boss_guest_damage` (por defecto 0,5);
   - **descartar el golpe** si H está en cinemática (`SetAvoidByCutSceneAck=1`) o si en los últimos
     ~3 s los golpes nativos de H a ese jefe salieron con 0 de daño (señal de invulnerabilidad; hay
     que guardar el `OnPostBuffDamagedAck` por objetivo).
4. **`player_hits` con jefe**:
   - igual, salvo durante las cinemáticas de H (no hay golpes);
   - añadir como atacante válido cualquier carga de tipo 7 cuya carga contenga la clave del jefe:
     armas y partes ("accesorios" de `enemigos-del-anfitrion.md` §5) [inferido].
5. **No forzar el aggro** del jefe a la marioneta (§2.6).
6. **G**:
   - barra de jefe en el overlay (HP de `VaryHPAck`, máximo de la carga o del primer `VaryHPAck`;
     nombre del registro);
   - aviso "cinemática del anfitrión".

Detalle de B (cuándo se activa):
- H recibe `MoveFieldAck` estando en una etapa de jefe, o la etapa es de la lista con
  `set_loading_stage` (15) o es de revancha o "boss rush";
- o el jefe es de tipo 5;
- G recibe `BossZone{on, nombre}` y el overlay lo muestra.
- Las copias del jefe y de sus esbirros se retiran con `EnemyReset` y se reponen al terminar
  (`set_battleEnd` → `StageBranchState` de la etapa a `Terminate`, o simplemente la retirada del
  jefe).

---

## 4. Protocolo propuesto

Hay que subir `kVersion`.

| Nuevo | Tipo | Formato | Uso |
|---|---|---|---|
| `EnemySpawn.flags` bit `kBoss` | (campo existente) | — | La copia es un jefe: aplica las reglas de §3 A.3-6 en los dos lados |
| `RelKind::BossState` | fiable, ordenado | `u16 net_id │ u8 estado (0 fin, 1 pelea, 2 cinemática, 3 pausa) │ u32 hash de etapa │ u8 n jefes` | Barra y avisos en G; G deja de mandar `GuestHit` y H deja de mandar `PlayerHit` en el estado 2 o 3 |
| (sin cambio) `GuestHit` | | | H aplica las reglas de jefe al recibirlo: G no tiene que saberlas |

---

## 5. Plan recomendado

1. **Etapa J0, solo lectura (sin escrituras)**: captura de un combate de jefe real (prueba B0).
   Confirma el tipo de registro del jefe, `T0+1043`, `SetAvoidByCutSceneAck`, `SetMinStatRateAck`
   con `noDie` y el estado "fatal".
2. **Etapa J1, reglas de seguridad** (aunque no se copie ningún jefe aún):
   - `is_boss(key)` en H con la tabla por build;
   - en `guest_hits`: fuera `KillCheat`/`DieByRunOutFatal`/tambaleo para jefes, daño × 0,5, HP ≥ 1;
   - descarte durante las cinemáticas.

   Es barata y quita el riesgo de atascar una misión.
3. **Etapa J2**:
   - `BossState` y la barra en el overlay de G;
   - la pausa de zona (B) para cambio de campo, carga de nivel, revancha, "boss rush" y tipo 5.
4. **Etapa J3**: copia de jefes de tipo 4 con la bandera de jefe y accesorios (partes y armas de
   tipo 7) como atacantes válidos.
5. **Más adelante**:
   - agarres como golpe;
   - escalado fino;
   - la barra `KnockOut` (buscar el id del stat);
   - el enfoque D si hace falta avanzar misiones juntos.

Primer jefe para probar: **un "bossencounter"** (34; sin `noDie`, sin barrera ni carga). Si no hay
ninguno a mano, un jefe de misión de familia `cd_boss_*` **con copia de `save` antes**.

---

## 6. Pruebas en vivo (un PC, `cdmp_peer --mode relay`)

**Antes de todo:** copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/` antes de cada prueba
que escriba (B2-B4). En el eco este PC es H y G a la vez: la copia del jefe aparece a 4 m del jefe
real, y tus golpes a la copia vuelven como `GuestHit` sobre el original.

| # | Prueba | Montaje | Qué mirar | Predicción |
|---|---|---|---|---|
| B0 | Captura de un jefe, **solo lectura** | Sniffer con `record=1`, `enemies=0`. Pelear contra un jefe de encuentro y, si se puede, uno de misión | Tipo de registro y `+0x18` de la carga del jefe; `T0+1043` frente a `boss_tables.tsv`; `SetAvoidByCutSceneAck` 1/0; `SetMinStatRateAck` del jefe al entrar en la fase con `noDie`; `NotifyHandleFatalAck` (2899) en `groggy`; `ChangeAIStateReq` alrededor de la fase; `DoFrameEventReq` con atacante gimmick o invocación | Tipo 3 en encuentros; `T0+1043` en la tabla; `SetAvoid` = 1 al entrar en `SCENE_n`; el mínimo de HP sube con `noDie` (si §1.4 es la vía) |
| B1 | Copia del jefe | `enemies=1`, `enemy_offset_m=6` (los jefes son grandes), Ctrl+F6 sobre el jefe | Que la copia se crea (`+0xCF==1`) y la contención; si sale barra nativa o icono de jefe en el mapa para la copia | Se crea; **sin barra nativa**; la copia se queda quieta en las cinemáticas y salta al acabar |
| B2 | Golpe letal de "G" a un jefe (con J1) | Pegar a la copia con el jefe a poco HP | Log: "boss: lethal clamped"; ningún `KillCheatReq`; el HP se queda ≥ 1; el remate nativo de H lleva a `BossPhase` y a la cinemática final | Etapa completa, sin bloqueo |
| B3 | Golpe de "G" durante una cinemática o invulnerabilidad | Pegar a la copia con `SetAvoid=1` | Ningún `VaryStatCheatReq` mientras dure | 0 inyecciones |
| B4 | Sin J1 (para medir el riesgo, **solo con copia de `save`**) | Reglas viejas, golpe letal a la copia con el jefe con `noDie` activo | ¿`KillCheatReq` mata o el HP se queda en el mínimo? | Si se queda en el mínimo, `noDie` = mínimo de stat (§1.4) |
| B5 | Aggro en un encuentro | Pegar a la copia hasta que el jefe mire a la marioneta (en el eco, tu propia copia) | ¿La etapa pasa a `PLAY_3`/`battleEnd_onExit`? | Posible fin prematuro: confirma §2.6 |
| B6 | Barra en el overlay (J2) | — | Barra de jefe en el overlay de "G" que baja con los `VaryHPAck` relevados; captura con `CopyFromScreen` recortada | Coincide con la nativa de H |

Lo que el eco no prueba:
- el mundo de G sin el jefe ni la barrera;
- el doble jefe de §2.9;
- la latencia.

---

## 7. Preguntas abiertas

| # | Duda | Cómo se decide |
|---|---|---|
| Q1 | ¿De qué tipo es el registro de un `Boss_*`: 3 o 4? ¿Y los dragones, tanques y el avión: 5? | B0 |
| Q2 | ¿`set_noDie` es el mínimo del stat (`[stat+0x28]`, `SetMinStatRateAck`) o un buff o estado aparte? | B0 + B4 |
| Q3 | ¿`RefreshNearBossList` enseña la barra o el icono de un jefe cercano sin etapa (jefes de campo)? | B1 |
| Q4 | ¿La IA de H manda `StartActionReq` del jefe durante la cinemática, o solo `SetActionIndexBySequencerReq` (2113)? | B0 |
| Q5 | ¿Las ventanas de invulnerabilidad dejan rastro en el cable (buff, `OnPostBuffDamagedAck` con 0)? | B0 |
| Q6 | ¿Las partes rompibles del jefe (gimmicks `attach_*`) son hijos (`+0xB0`) del jefe? La copia los quita | B0 + `foreign.py` sobre la carga |
| Q7 | ¿El filtro del secuenciador (`_filterBoundary`) esconde la marioneta en las cinemáticas de H? | B1 (que se vea la copia de uno mismo en el eco) |
| Q8 | ¿Los "bossencounter" se repiten tras derrotarlos, o son de una vez? | B0 o jugando |

---

## 8. Riesgos

| # | Riesgo | Mitigación |
|---|---|---|
| R1 | Misión de H atascada por una muerte fuera del guion | J1: nada de `KillCheat` ni remate de "fatal" en jefes; HP ≥ 1 |
| R2 | Cierre del juego con jefes vehículo (tipo 5) | Excluir el tipo 5 (ya pasa) |
| R3 | La copia del jefe flota en niveles por fase | Pausa (B) en etapas con `set_loading_stage` |
| R4 | Encuentro cerrado por aggro a la marioneta | No forzar el aggro en jefes |
| R5 | Doble jefe si G está en la misma misión | Detectar el jefe propio en G y no relevar |
| R6 | Saltarse fases con el daño de G | Multiplicador + descarte en invulnerabilidad y cinemáticas; el remate siempre es de H |

---

## 9. Apéndice: direcciones y scripts (build 2976)

| RVA | Qué es |
|---|---|
| `0x7C7240` | Inicialización del cliente; registra la tarea "`RefreshNearBossList`" (cadena `0x55AE1F0`; constructor de la tarea `0x7DB0C0`, virtualizado) |
| `0x1980E0` | Enumeración `CharacterTier`: `Empty, Tier_1..3, Elite, Boss` |
| `0x198250` | Enumeración `None, Boss, FieldBoss, Ally, Mercenary, Vehicle` (tipo de etiqueta [inferido]) |
| `0x197350` | Enumeración `Phase, QTE, Skill, BossPhase, Reset, Runaway, Fail` (estados de IA que lee la etapa) |
| `0x196B70` | Enumeración de motivo de aparición (`QuestBoss` = 8, `SequencerSpawn`…) |
| `0x198EA0` | Enumeración de bonificaciones (incluye `AddBossDamagePercent` = 4) |
| `0x17B3230` → `.xtls 0xC7E0860` → `0x17B3E10` → `.xtls 0xC7E7960` | Cambio de stat: recorte al mínimo `[stat+0x28]` con delta negativo y al máximo `[+0x18]+[+0x20]` con delta positivo |
| `0xDA18F0` | Usa `MapIcon_ActorEnemyBoss` (`0x566B418`) |
| `0x15CE070` | Usa `Actor_Enemy_Boss` y `Actor_Enemy_Elite` |

| Mensaje | Op | Dir. | Handler o escritor |
|---|---|---|---|
| `SetAvoidByCutSceneAck` | 2561 | S>C | `0xC09000` |
| `StageBranchStateReq` / `Ack` | 2436 / 2981 | C>S / S>C | `0x2A5C4F0` / `0xC073A0` |
| `UpdateStageChartEventReq` / `Ack` | 2591 / 2111 | C>S / S>C | `0x2A5CC70` / `0xC08970` |
| `StageOnBranchEventAck` | 2524 | S>C | `0xC07D00` |
| `PrepareSequencerAck` | 2661 | S>C | `0xC05BB0` |
| `SetMinStatRateAck` | 2645 | S>C | `0xB77060` |
| `NotifyHandleFatalAck` | 2899 | S>C | `0xB76900` |
| `NotifySummonCharacterAck` | 2965 | S>C | `0xC10780` |
| `MoveFieldAck` | 2174 | S>C | `0xB87360` |
| `RequestQuickTimeEventResultReq` | 2236 | C>S | `0x298C790` |
| `DieByRunOutFatalReq` | 2321 | C>S | `0x29947E0` |
| `VaryStatCheatReq` / `KillCheatReq` | 2703 / 2279 | C>S | `0x29BF3C0` / `0x29BF730` |
| `CompleteQuestByCheatReq` / `StageChartForceBranchDevReq` | 2627 / 2476 | C>S | `0x2A48930` / `0x2A5D330` (no usar) |
| `SetBossRematchLevelReq` | 2760 | C>S | `0x2A608D0` |

Scripts en `build/research/p_boss/` (se ejecutan con `build/research/venv/Scripts/python.exe`):

| Script / fichero | Qué hace |
|---|---|
| `strx.py <cadena>…` | RVA de una cadena y sus `lea` con la función que las contiene |
| `enumdump.py <func>…` | Vuelca tablas de nombres de enumeración (`lea r8 │ mov edx │ call 0x44D8B0`) |
| `idx_boss.py` → `idx_boss.txt` | Ficheros del PAZ con "boss" por tipo y carpeta |
| `pull.py`, `pull_stages.py`, `scan_all_stages.py` | Extraen UI y etapas (`out/`, `out/stages/`); `bossbar_stages.txt` = las 125 etapas con barra |
| `stagestr.py <fichero>` | Cadenas de un `.pastage`/`.paseq` en orden |
| `tagfreq.py` → `tagfreq.txt` | Frecuencia de etiquetas `set_*`/`condition_*`/`Player.*` en las etapas de jefe |
| `fights.py` → `fights.tsv` | Mecánicas por etapa (tabla de §2.2) |
| `stage_hashes.py` → `boss_tables.tsv` | `hashlittle(nombre, 0xC5EDE)` de las 125 etapas (lo que llevaría `T0+1043`) y las 120 claves `Boss` de `characterinfo` |
| `scan_caps.py` → `scan_caps.txt` | Jefes y HP máximos en las 58 capturas, más el recuento de mensajes de etapa |
| `stage_msgs.py <captura>` | Tamaños y muestras de los mensajes de etapa, secuenciador y cinemática |
| `ci_type.py` | Intento fallido de hallar el byte de `characterinfo` que decide el tipo de registro: los campos son de longitud variable |
