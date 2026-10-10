# La copia es el objetivo, pero casi nadie la ataca (2026-10-04)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas solo
para esta build. Investigación de solo lectura: capturas del sniffer, logs del mod y análisis estático
(capstone sobre el exe en disco). No se ejecutó el juego ni `cdmp_peer`.

Marcas: **[verificado]** leído en el código; **[captura]** medido; **[inferido]** deducción pendiente de
prueba en vivo.

Notación (como en `pendiente-aggro-y-respuesta-golpes.md`): **P** = tu jugador `0xA0100001`, **G** = la
copia `0xA010F001`, **E** = enemigo local (`0xB01…`, sin copias `0xB01F…`). CTC =
`CurrentTargetChangeReq`, DFE = `DoFrameEventReq`, SA = `StartActionReq`, SAIF = `StartAIFunctionReq`.

Sesiones (eco en un PC, `offset_m=10`, `delay 300 ms`, `enemies_target_copy=1`): `223107` (la de las
22:31-22:40), `222049`, `215215`, `205431`, `193440`, `201913`, `181940`, `174858`, `173434`, `184017`;
para algunas tablas también `114648`, `111538`, `101713`, `065037`. Scripts en `build/research/p_attack/`
(§6).

---

## 0. Resumen

1. **Los enemigos sí se acercan a G, pero casi nunca le lanzan un ataque normal.** [captura]
   - Con G de objetivo se mueven igual que con P: destino de IA sobre G (mediana 0,8 m), y el mismo
     tiempo a ≤ 4 m (27 % frente a 28 %).
   - **Los mismos 32 enemigos, a < 4 m de cada objetivo, empiezan 19,8 ataques/min contra P y 1,4/min
     contra G** (14 veces menos). A 2-4 m, la distancia de los ataques normales: 8,7 frente a 0,5/min.
   - Los **ataques a la carrera** (`*_run_attack*`, el que cierra distancia) sí salen contra G: 2,6/min a
     0-2 m (P: 0,9).
   - Golpes que llegan (DFE con G): mismos 19 enemigos a ≤ 5 m: **11,4/min a P y 2,2/min a G**.
2. **No es el estado de batalla del enemigo, la distancia, la orientación ni el servidor.** [captura]
   - El 77 % del tiempo con G de objetivo el enemigo ya está en `ChangeBattleState` 1. Aun así, 0,5
     golpes/min frente a 2,4 en P.
   - Encara a G casi tan bien como a P: 16° frente a 6° de mediana.
   - Ningún `StartActionNak`. Solo un `CatchNak` (un agarre a G).
3. **La "provocación" no se ve con claridad en las capturas.** [captura]
   - Un golpe de P a un E que apunta a G no subió los golpes a G en los 20 s siguientes (n = 12).
   - En los 6 s tras el golpe, con E aún sobre G y a < 4 m, sí hay más ataques (2,6 frente a 1,1/min),
     pero son solo 24 s de muestra. Encaja con la observación, sin probarla.
4. **El cuello de botella está en la "coordinación de atacantes" del lado del objetivo.** [verificado +
   inferido]
   - El componente de objetivo de **cada objetivo** (`[[X+0x68]+0x90]`) guarda listas de **quién le
     ataca**: por tipo de orden de batalla (`+0x98`), y ordenadas por distancia como **rango** (`+0x58`).
   - Las condiciones de IA `BattleRank`, `BattleRankCount`, `CheckIsTargetLastRank` y las posiciones de
     formación se calculan con esas listas del **objetivo actual**. Las de G y las de P funcionan
     distinto:
     - un enemigo se apunta en la lista del primer actor de su lista de aggro (**TopAggro**), no en la de
       su objetivo actual;
     - el rango se recalcula **a través del componente de IA del objetivo** (`[X.comps+0x58]+0x50`). La
       copia está en modo 2 y sin "cerebro" de IA.
   - Hipótesis principal [inferido]: G no tiene rango ni hueco válido, y el gráfico de IA no concede el
     ataque normal, aunque sí el ataque a la carrera.
5. **Plan:** primero una sonda en vivo de solo lectura (F0, §5) que lo decide en una sesión. Después,
   según el resultado, una de estas correcciones (§4):
   - F1: un byte de orden de batalla en la IA de G;
   - F2: apuntar al atacante en la lista de su objetivo actual;
   - F3: forzar el resultado de una condición concreta para G;
   - F4: provocar con un evento de golpe sin daño.

---

## 1. Medidas (capturas)

### 1.1 Qué hace E según su objetivo (`s4_split.py`, enemigos activos a ≤ 30 m, 5 sesiones) [captura]

| Por minuto de objetivo | P (9.986 s) | G (2.544 s) |
|---|---|---|
| `StartActionReq` | 104 | 79 |
| `ChangeAIStateReq` | 88 | 105 |
| `StartAIFunctionReq` | 8,4 | 13,3 |
| `UpdateAIDestinationReq` | 14,1 | 12,3 |
| `AggroRelationListReq` | 0,5 | 4,0 |
| `ChangeBattleStateReq` | 0,5 | 0,45 |
| DFE que alcanza al objetivo | 0,91 | 0,40 |
| Distancia E-objetivo (mediana / ≤ 3 m / ≤ 5 m) | 14,3 m / 6 % / 12 % | 12,0 m / 7 % / 17 % |
| \|destino de IA − objetivo\| (mediana) | 0,1 m | 0,8 m |

- `+0x16C` de la SA = el objetivo del CTC en el 99 % de los casos con G y en el 92 % con P.
- G suele estar **más cerca** que P del enemigo que le apunta (63-87 % de las muestras según sesión): no hay un problema
  de distancia.

### 1.2 Ataques por banda de distancia (`s26_att2.py`, 14 sesiones) [captura]

"Ataque" = SA cuyo hash es el *accessor* de algún DFE que alcanzó a otro actor (62 hashes). "A la
carrera" = los que se resuelven a `*_run_attack*` (§3.4).

| Distancia horizontal | P: s / normales/min / carrera/min | G: s / normales/min / carrera/min |
|---|---|---|
| 0-2 m | 779 / 7,47 / 0,92 | 185 / 2,60 / 2,60 |
| 2-4 m | 1.372 / **8,66** / 0,96 | 389 / **0,46** / 0,15 |
| 4-6 m | 1.726 / 1,81 / 0,24 | 480 / 0,00 / 0,00 |
| 6-10 m | 3.742 / 0,59 / 0,03 | 1.035 / 0,12 / 0,00 |

- **Mismos enemigos** (32, ≥ 3 s a < 4 m de cada uno): **19,75 frente a 1,40 ataques normales/min**.
- Con nombres resueltos (`s25_named_attacks.py`), a 0-3 m: `*_attackN` P 2,67/min, G **0,00**;
  `*_run_attackN` P 0,99, G 1,46.
- Golpes (DFE con el objetivo) por segundo cerca (`s7_hits_close.py`): ≤ 3 m P 6,5/min, G 2,3/min.
  Mismos 19 enemigos a ≤ 5 m de ambos: **11,4 frente a 2,2/min**.

### 1.3 Lo que no lo explica [captura]

| Factor | Medida | Script |
|---|---|---|
| Estado de batalla de E | Con `CBS` = 1: P 2,4 golpes/min, G 0,5. E está en batalla el 77 % del tiempo sobre G (P: 33 %) | `s5_battle.py` |
| Orientación de E al empezar el ataque (eje −Z del cuaternión de la SA `+0x9A`) | P mediana 6°, 75 % < 30°; G 16°, 63 % < 30° | `s23_facing.py` |
| Altura de G (copia flotando/enterrada por `offset_m=10`, `pendiente-copia-se-hunde.md`) | G con \|ΔY\| > 0,4 m el 64 % del tiempo cerca (P: 41 %). **Con \|ΔY\| < 0,4 m: P 7,9 golpes/min, G 1,6.** Influye en que el golpe conecte, pero no es la causa principal | `s22_height.py` |
| Motivo del CTC (`0x1D` = el más cercano pasa a ser TopAggro, §2.2) | G `0x18`: 1,4-1,8 golpes/min; G `0x1D`: 1,35; P `0x18`: 3,9; P `0x1D`: 9,7 | `s21_reason.py` |
| Rechazos del servidor | 0 `StartActionNak`; 1 `CatchNak` (agarre a G) | `s28_naks.py` |
| Destino de movimiento | Persigue a G igual que a P (52 % a < 0,5 m, P 72 %). El 22 % lejos de los dos: vuelta a casa | `s29_dest.py` |

### 1.4 Paquetes y eventos de IA (`s16_saif_named.py`) [captura]

`StartAIFunctionReq`: `+0 actor │ +4 gráfico (0xA17A8F3E) │ +8 paquete │ +0xC función │ +0x10 evento │
+0x14 índice`. Los nombres son `hashlittle(minúsculas, 0xC5EDE)` [verificado por coincidencia].

| Paquete / evento | P /min | G /min |
|---|---|---|
| `findtargetpackage` / `onpackageleavefunction` | 0,71 | **1,60** |
| `?` / `0x7242E492` (#53; le sigue un CTC: re-selección de objetivo) | 0,25 | **1,16** |
| `?` / `onbattle_firstaggro` | 0,30 | 0,51 |
| `battlewaitpackage` / `oncheckpointfirststart` | 0,39 | 0,37 |
| `?` / `onupdate` (#37) | 0,07 | 0,37 |
| `?` / `0xE69AB342` (tras recibir un golpe) | 0,39 | 0,17 |
| `?` / `onbattle_alarmstart` | 0,00 | 0,08 |

- Con G, la IA **sale y vuelve a `findtargetpackage`** el doble de veces y re-elige objetivo cuatro veces
  más. Es un bucle "buscar objetivo / esperar" sin llegar a la rama de ataque [inferido].
- Ejemplo (`s13_tl.py 222049 B01000D3`):
  - a 0,6-1,1 m de G hace `mace_weapon_out`, un `mace_run_attack1` (golpea a G) y luego solo
    `B9154043`/`0x00000000` (acciones sin resolver, no de ataque) durante 30 s;
  - al pasar a P por `0x1D`, a los 3 s hace `mace_run_attack2` y le golpea.

### 1.5 "Provocación" (golpe de P a un E que apunta a G) [captura]

| Medida | Resultado | Script |
|---|---|---|
| Golpes a G 20 s antes / después del primer golpe de P (n = 12) | 1,08 / 0,70 por min | `s8_provoke.py` |
| Golpes a G cerca, según si P ya le había pegado | nunca 3,0/min; < 30 s 2,1; > 30 s 1,6 | `s18_hitbefore.py` |
| Golpes a G, según si E ya había golpeado a P | sí 3,9/min; no 1,8 | `s18_hitbefore.py` |
| Ataques normales a G en los 6 s tras el golpe de P (E aún sobre G, < 4 m) | 2,55 frente a 1,09/min; golpes 5,1 frente a 1,3 (24 s de muestra, 2 golpes) | `s27_provoke2.py` |

- Lo que manda E en los 0,2 s tras el golpe: `EchoActionSessionID`, `StartAIFunction` (evento
  `0xE69AB342`), `ChangeAIState`, a veces `ChangeBattleState` y `ChangeAggroRelationValidTick`.
- Conclusión: hay un indicio (rama de contraataque tras recibir un golpe), sin significación. La
  diferencia de 14 veces existe también sin golpes.

### 1.6 Avisos sobre los datos

- `aggro_test` solo escribe las **60 primeras** líneas de "now targets the other player's copy" y de
  "ATTACK frame…" por sesión. Los recuentos del log no valen: usar las capturas.
- DFE solo sale cuando el golpe conecta. Un ataque fallado no deja rastro salvo su SA.
- El conjunto de "ataques" es aproximado. Solo hay 38 de 796 hashes de acción resueltos por nombre
  (§3.4).

---

## 2. Código: qué decide si E ataca a su objetivo

### 2.1 "Está en batalla" [verificado]

| Función | Qué hace |
|---|---|
| `0x17EC130 IsInBattle(actor)` | Tipo 1: falso si muerto (`Status+0x273`) o `Status+0x390\|0x394 ≥ 0x80`; si no, `0x221BB60(comp objetivo)`. Tipos 3-9 igual |
| `0x221BB60(comp)` | Si `actor->vf188(5)` (= `byte [actor+0xC8] != 0`, modo de control, `0x4FA620`): IA `[comps+0x58]`; si `[[IA+0x50]+0x40] == 3` → falso; si no, verdadero si hay alguien en las listas de aggro (`comp+0x128`/`+0x138`) o de atacantes (`+0xA0/+0xB0/+0xC0/+0xD0`). Sin modo de control: `byte comp+0x164` |
| `AICondition_CheckBattleState` vf1 `0x22E1EB0` | Selector `[cond+8]` (`0x207C8F0`) → `0x17EC130` |
| `0x22218F0` (cambio de estado de batalla) | Con modo de control → vf `+0x170` = `0xA280B0` → `.xtls 0x9DA9BE0` → `0xB65F30` → `.xtls 0xA3AC510`: `ChangeBattleStateReq` (op `0x8D1`) |

- La copia (modo 2) entra por la rama de listas. Su "en batalla" depende de que alguien la tenga en sus
  listas y de su byte de orden en `[[G.comps+0x58]+0x50]+0x40` (si existe) [inferido].

### 2.2 Listas de atacantes en el objetivo (`CommonTargetActorComponent`) [verificado]

| Offset (comp de X) | Contenido |
|---|---|
| `+0x18 + 16·i` (i < 4) | 4 listas de claves (sin identificar; las recorre `0x221D200` con las de `+0x58`) |
| `+0x58 + 16·t` (t < 4) | **Atacantes de X de tipo t ordenados por distancia** (rango). `{ptr, u32 n, u32 cap}`, entradas de 8 B `{u32 clave, …}` |
| `+0x98 + 16·t` | **Atacantes de X por tipo de orden de batalla t** (entradas de 8 B `{u32 clave, u16, …}`) |
| `+0x120`, `+0x130` | Listas de aggro (entradas de 20 B). **TopAggro = 1.ª entrada de la 1.ª no vacía** |
| `+0x164` | Byte "en batalla" (actor sin modo de control); el servidor lo escribe con `ChangeBattleStateReq` |
| `+0x170` / `+0x178` | Secuencia / objetivo actual (`pendiente-aggro…` §1.2) |

| Pieza | Dirección | Qué hace |
|---|---|---|
| Alta/baja de atacante | `0x221A820` → `.xtls 0xE6881E0` → `0x221D030(compX, alta, clave, u16, tipo)` | Busca X por clave y mete o quita mi clave de `X+0x98+16·tipo`. Tipo = `[[mi IA]+0x50]+0x40`. Luego recalcula "en batalla" y llama a `0x22218F0` |
| Quién la llama | `0x221D8A0`, `0x221EF50`, `0x221F360`, `0x2220170`, `0x221B250` | Las rutinas que tocan la lista de aggro: **al cambiar el TopAggro**, baja del antiguo y alta en el nuevo |
| NearAggro | `0x221B250` (desde `AIFunction_SetCurrentTargetNearAggro` `0x17231B0`) | **Mueve la entrada elegida al principio** (pasa a ser TopAggro) y re-registra |
| `SetCurrentTarget` | `0x2217290` | `AddAggro` **añade al final**. Con `0x18`, si P ya era el primero, E sigue apuntado como atacante **de P** aunque su objetivo sea G |
| Cambio de tipo de orden | `0x1EB3F90` (desde el buff `CommonChangeBattleOrderTypeBuffProcessor` vf2 `0x20A6500` y `0x2076DE0`) | Cambia `[IA+0x50]+0x40` y mueve mi entrada entre las listas `+0x98` del objetivo (`0x221ADD0`) |
| Rango | `AIOperatorValue_BattleRank` → `.xtls 0xED78A50` | Objetivo = contexto de IA `[+0x260]` (válido si `[+0x268]`). Si **el objetivo tiene IA** (`[X.comps+0x58]`), `0x1EB2BD0([IA_X+0x50], tipo, ahora)`: recálculo con límite de frecuencia, solo tipos 0 y 1, `0x221A940` = ordenar por distancia a X. Rango = índice de mi clave en `X+0x58+16·tipo` (**0 si no está**) |
| Nº de rangos | `AIOperatorValue_BattleRankCount` → `.xtls 0xED7E720` | `u32 [X + 0x60 + 16·sel]` |
| ¿Soy el último? | `AICondition_CheckIsTargetLastRank` vf1 `0x22DD930` → `0x170D900` | Compara mi clave con la última de `X+0x58+16·tipo` |
| Huecos alrededor del objetivo | `0x1EB2D10` (IA del objetivo; desde `.xtls 0xED04340` → `0x1738FA0`) | **Devuelve falso si no estoy en la lista de rango del objetivo**; si estoy, calcula hasta 6 posiciones |
| Tipo de orden | `AICondition_CheckBattleOrderType` → `.xtls 0xEB253D0` | Actor del selector: tipo 1 con bandera → byte de `Status` (`+0x1E4`); si no, `[[X.comps+0x58]+0x50]+0x40` |

- Quién rellena `+0x58` no está localizado (probablemente código `.xtls` con desplazamientos ofuscados).
  `0x221A940` solo **reordena** lo que ya hay.
- **Hipótesis principal** [inferido]: con G de objetivo, E no tiene rango ni hueco válido en G:
  - E está apuntado como atacante de P (TopAggro);
  - y/o G no recalcula rangos, porque es una copia en modo 2 sin IA activa.
  
  El gráfico de IA deja el ataque normal a quien tiene rango o hueco, y permite el ataque a la carrera de
  aproximación. Encaja con §1.2: carrera sí, normales no.
- Contraindicio: con `0x1D` (que pone a G de TopAggro) el ritmo contra G no mejora (1,35/min). Por eso el
  registro por TopAggro no basta para explicarlo: hay que mirar el estado de G (IA, listas) en vivo (F0).

### 2.3 Otras condiciones candidatas [verificado lo que se cita]

| Condición | Dirección | Qué lee del objetivo | Por qué podría fallar con G |
|---|---|---|---|
| `AICondition_CheckMeleeAttackable` | vf1 `0x22D76F0` → `.xtls 0xEA77550` → **`0x17033B0`** | Transform del objetivo (`TS+0x98..+0x108`), búsqueda de camino (`0x1EEBD20`, `0x1F06B80`) y vóxeles | Copia flotando/enterrada (`offset_m`), o camino a un punto fuera de la malla |
| `AICondition_CheckTargetForceAttackable` | vf1 `0x22D8380` → `0x22D8450` | `[[X+0x68]+0x70]+0x40` y el estado de `[X+0x68]+0x1A8` (`0x21B8B20`, interruptor por byte `+0x39`) | La copia en modo 2 no tiene estado de navegación propio |
| `AICondition_CheckExistInPlayerCamera` | vf1 `0x22E18F0` → `.xtls 0xEBA29F0` (virtual `+0x1D0` del contexto) | Cámara del jugador local | Si se aplica al objetivo, G a 10 m del centro de la pelea |
| `IsTargetable` | `0x22141E0` | `TS+0x3DC` (el mod lo fuerza en el gancho) | Ya cubierto para todos los llamadores |

- Tipo de actor (vf `0x178` "usuario local", `0x8A67D0`/`0x4FE260`): no aparece en las rutas de ataque
  vistas.

### 2.4 Nombres de acciones y paquetes (`s11`/`s12*`) [verificado por hash]

- `StartActionReq +0x04` = `hashlittle(nombre de acción, 0xC5EDE)`, y el *accessor* del DFE lleva el
  mismo hash.
- Resueltos (38):
  - ataques: `mace_run_attack1/2` (`34AFB6FB`/`864E2A86`), `mace_attack1/2`, `axe_attack1-3`,
    `spear_attack1/2`, `spear_run_attack1/3`, `swordshield_attack1/2`, `swordshield_run_attack1`,
    `bow_run_attack`, `attack1`, `attack_back`, `wolf_front_rush_c`;
  - armas y esquivas: `*_weapon_out/in`, `*_evade_b/l/r`;
  - otros: `wait` (`866C7489`), `wait_default` (`26107D3C`), `wait_hide` (`C34C44ED`),
    `keepfightstaggerstate`, `swap_enter_topstate`, `turn_front`, `sickle_combat_pose1`.
- Paquetes: `findtargetpackage` (`4EA7F942`), `battlewaitpackage` (`0CB49799`), `waitpackage`
  (`5337ED67`), `deadendpackage` (`9C25E62C`), `checkpath` (`5EAEC590`).
- Eventos: `onfirstupdate`, `onupdate`, `onpackageleavefunction`, `onstateenter/leavefunction`,
  `onbattle_firstaggro`, `onbattle_alarmstart`, `onbattle_hit`, `ondetectcomplete_fear/runaway`,
  `ondetected_attack`, `oncheckpointfirststart`.
- Sin resolver: `0x7242E492`, `0xE69AB342`, `0xE851DFDF` (58.000 SA, ¿locomoción?) y
  `0xB9154043`/`0xD2442A05`/`0x299481D6` (lo que hace E pegado a G).

---

## 3. Por qué la provocación podría funcionar [inferido]

- El golpe de P lanza en E el evento de IA de golpe (`0x2083D90` desde el ATA) y el evento
  `0xE69AB342`. Así entra en una rama de reacción y contraataque que ataca a **su objetivo actual** sin
  pasar por el rango o hueco del objetivo.
- Con E todavía sobre G, ese contraataque va a G. Es lo que se vio en la prueba, y lo que sugiere (sin
  significación) §1.5.

---

## 4. Correcciones propuestas (de menor a mayor riesgo)

Todas en el anfitrión, con interruptor en el ini y lecturas protegidas (`VirtualQuery`/SEH). Sin ganchos
a mitad de función, y la tabla por enemigo indexada por clave, nunca por dirección.

| # | Qué | Dónde | Riesgo / esfuerzo | Prueba en un PC y predicción |
|---|---|---|---|---|
| **F0** | **Sonda de diagnóstico** (§5). Decide entre F1, F2 y F3 | Lecturas en el hilo del mod y, opcionalmente, contadores en entradas de función | Muy bajo / 0,5 días | Ver §5 |
| **F1** | Si F0 ve que G tiene IA pero con `[[G.comps+0x58]+0x50]+0x40 == 3` (o distinto del de P): **una escritura protegida de ese byte** con el valor de P, al aparecer la copia y cada 5 s si cambia | Dato de la copia (nuestro actor) | Bajo / 0,5 días. Solo toca la copia; se revierte al quitarla | "En batalla" de G pasa a verdadero; los mismos enemigos a < 4 m pasan de **1,4 a ≥ 8 ataques normales/min** contra G. Si no cambia nada, se descarta |
| **F2** | Si F0 ve que los E que apuntan a G están en `P+0x98/+0x58` y no en `G+…`: **apuntar al atacante en la lista de su objetivo actual**. Gancho al **inicio** de `0x221D030` (`safetyhook::create_mid` en la cabecera; `rcx` comp de X, `dl` alta, `r8d` clave del atacante, `[rsp+0x28]` tipo). Si el atacante es un E local con objetivo G (`[[E.comps+0x90]+0x178] == 0xA010F001`) y X es P, cambiar `rcx` por el comp de G (alta y baja simétricas). Si E cambia de objetivo, dar de baja en G | Gancho de entrada | Medio / 1,5 días. Riesgo de contabilidad: una baja que no llegue deja a E en la lista de G. Mitigación: al retirar la copia, limpiar con la misma función (baja) | E entra en `G+0x98`; `BattleRankCount(G) > 0`; ataques normales a G ≥ 8/min. P deja de "contar" a E (puede subir lo que atacan a P otros E) |
| **F3** | Si F0 ve que una condición concreta (`0x17033B0` o `0x22D8450`) da "no" casi siempre con G y "sí" con P: **gancho inline en la entrada** de esa función (como `IsTargetable`). Si el objetivo es G, `copy_state_normal()` se cumple y la distancia horizontal es ≤ 3,5 m, devolver el valor que da con P | Gancho de entrada (inline permitido: es el inicio de la función) | Medio / 1 día. Con la copia flotando, el ataque puede salir y no conectar | Sube el número de SA de ataque contra G. Los golpes que conectan suben menos si \|ΔY\| > 0,4 m (§1.3) |
| **F4** | Sin depender de la causa: **provocación sintética**. Si E apunta a G, está a < 3,5 m y no ha empezado un ataque en 4 s, inyectar el evento de golpe: `PostProcessAttackResult_AttackTargetAck` sobre E con atacante G, plantilla de un ATA real del mismo tipo de enemigo, daño 0, `+0x81 = +0x82 = 1`, **sin** AAR ni buff de golpe. Así solo se dispara el evento de IA `0x2083D90` | Inyección S→C (vía que ya usa `aggro_test`) | Medio / 1 día. Puede sacar una reacción visible en E (tambaleo) o cambiar su objetivo. Antes, probar con una sola inyección (Ctrl+F5) | En los 2 s siguientes, E inicia un ataque normal contra G (> 50 % de las inyecciones). Si E pasa a P o se tambalea, descartar |
| — | Inyectar `StartActionAck` de ataque al propio E | — | Alto: el E es nuestro (IA local), la acción no pasaría por el servidor y su DFE podría ser rechazado | No recomendado |
| — | Reordenar la lista de aggro o llamar a `0x221B250`/`0x221D030` desde el mod | — | Alto: llamadas internas desde otro hilo | Descartado |

Mejoras complementarias de bajo riesgo:

- En las pruebas de ataques a la copia, usar `offset_m` de 3-4 m y terreno llano. Con 10 m la copia flota
  más de 0,4 m el 47 % del tiempo y los golpes conectan peor (§1.3).
- Quitar el tope de 60 líneas de `aggro_test`, o sustituirlo por contadores cada 10 s: CTC a G por
  motivo, SA de ataque con objetivo G y DFE con G.

---

## 5. F0: sonda de diagnóstico (un PC, `cdmp_peer` en eco, solo lectura)

Cada 2 s, para P y para G (lecturas protegidas), escribir en `cdmp.log`:

| Campo | Dirección |
|---|---|
| Modo de control | `byte [A+0xC8]` |
| IA / sub-IA / tipo de orden | `q[A.comps+0x58]`, `q[IA+0x50]`, `byte [sub+0x40]` (`A.comps = q[A+0x68]`) |
| Listas del comp de objetivo `C = q[A.comps+0x90]` | n de `C+0x20/+0x30/+0x40/+0x50`, `C+0x60/+0x70/+0x80/+0x90` (rango), `C+0xA0/+0xB0/+0xC0/+0xD0` (atacantes), `C+0x128/+0x138` (aggro); `byte C+0x164` |
| Contenido | Las claves de `C+0x58+16·t` y `C+0x98+16·t` (entradas de 8 B; máximo 16 por lista) |
| Para cada E con CTC a G en los últimos 5 s | `byte [[E.comps+0x58]+0x50]+0x40`, si su clave está en las listas de G o en las de P, y \|ΔY\| G-E |

Opcional, si lo anterior no decide: contadores (llamadas y resultado por clase de objetivo P/G) con
ganchos inline **en la entrada** de:

- `0x17033B0` (`MeleeAttackable`);
- `0x22D8380` (`ForceAttackable`);
- `0x22E1EB0` (`CheckBattleState`);
- `0x170D900` (`IsTargetLastRank`);
- `0x1EB2D10` (huecos);
- `0x221A940` (recálculo de rango: ¿se llama alguna vez con el comp de G?);
- `0x221D030` (altas y bajas de atacantes).

El objetivo se saca de `[[ctx+0x68]…]` según cada función. Si no se puede leer con seguridad, solo
contar. **No** enganchar `.xtls` (`0xED78A50`, `0xED7E720`): leer sus listas en su lugar.

Procedimiento:
1. Copia de `save` a `backups/`. Lanzar `cdmp_peer`, `offset_m=3`.
2. Pelear contra un grupo de bandidos con cuerpo a cuerpo, dejando la copia en medio, 3 minutos sin
   golpear a los que apuntan a G.
3. Luego golpear una vez a uno de ellos.

Predicciones de la hipótesis principal:
- con 3 o más E sobre P, P tiene listas de rango y atacantes no vacías;
- **con E sobre G, las listas de G siguen vacías** y esos E aparecen en las de P;
- o bien G no tiene IA, o su tipo de orden es 3.

Si no se cumple, la causa está en las condiciones de §2.3, y los contadores dirán cuál falla con G.

Elección:
- G sin IA o con tipo 3 → F1 (si hay IA) o F4;
- E en las listas de P → F2;
- una condición falla → F3.

---

## 6. Scripts (`build/research/p_attack/`)

Ejecutar con `..\venv\Scripts\python.exe <script> <sesiones…>` desde esa carpeta. `s2_build.py` crea
`out/<sesión>.pkl`; los demás lo leen.

| Script | Qué hace |
|---|---|
| `cap.py`, `common.py` | Lector (reusa `../pieza4/pk.py`), posiciones, intervalos de objetivo por CTC |
| `s1_ctc.py` | CTC de enemigos por clase de objetivo y motivo; DFE a P/G |
| `s2_build.py` | Extrae SA, DFE, ATA, CTC, CBS, SAIF, DEST, AIS, ARL, BUFF y posiciones (P y E por Req; G por los Ack inyectados) |
| `s4_split.py` | §1.1 |
| `s5_battle.py` | Estado de batalla de E y ataques/golpes |
| `s6`, `s15`, `s25`, `s26` | Acciones de ataque por distancia y emparejadas por enemigo (§1.2) |
| `s7_hits_close.py` | Golpes por segundo cerca |
| `s8`, `s18`, `s27` | Provocación (§1.5) |
| `s9_ais.py`, `s13_tl.py`, `s17_ctx.py` | Línea de tiempo de un enemigo; contexto de un evento SAIF |
| `s10`, `s16` | SAIF por paquete y evento (§1.4) |
| `s11`, `s12*` | Resolución de nombres por `hashlittle` (`out/saif_names.pkl`, `out/action_names.pkl`) |
| `s14_arl.py` | Orden de `AggroRelationListReq` (TopAggro en el cable) |
| `s19`, `s20` | Buffs aplicados a E según su objetivo |
| `s21_reason.py`, `s22_height.py`, `s23_facing.py`, `s24_dfe_kind.py`, `s28_naks.py`, `s29_dest.py` | §1.3 |
| `x_opsend.py`, `x_w164.py` | Estático: quién envía un opcode; escrituras a un desplazamiento |
| `x_scan98.py`, `x_scan2.py`, `x_sib.py`, `x_sib2.py` | Estático: quién lee o escribe las listas `+0x58`/`+0x98` del componente de objetivo (§7) |

---

## 7. Resultado de la sonda F0 y causa (2026-10-05)

Datos:
- Sonda en vivo (`src/mod/net/attack_probe.cpp`): `bin64\cdmp.log` del 5-10 07:12-07:30, `offset_m=3`,
  445 líneas "Attack probe".
- Captura `cdmp_packets_20261005-071235` (re-analizada con `s2_build.py`, `s26_att2.py` y
  `s7_hits_close.py`).

### 7.1 Lo que midió la sonda [captura]

| Campo | P `0xA0100001` | G `0xA010F001` |
|---|---|---|
| `byte [A+0xC8]` | 1 al cargar, después 2 | **0 siempre** |
| IA (`[comps+0x58]`) / tipo de orden `[[IA+0x50]+0x40]` | sí / 0 (2 al principio) | sí / 2 |
| `C+0x164` (en batalla, rama sin control) | 0 | 0 |
| Atacantes `C+0x98+16t` | llenas | **llenas** (1/2/0/0, 3/1/0/0, 4/0/0/0) |
| Rango `C+0x58+16t` | **iguales a las de atacantes** (4/1/0/0, las mismas claves) | **0/0/0/0 en las 87 lecturas** |
| `C+0x18+16i` | 0/0/0/N | llenas (4/0/0/1, 6/1/0/0…) |
| Aggro `C+0x120`/`+0x130` | 4-9 | 0/0 |

- E con objetivo G (190 líneas): "en atacantes de G: sí, en rango de G: no, en listas de P: no" casi
  siempre. Tipo de orden 0 (94), 1 (86) o 3 (10).
- **La hipótesis de F2 (E apuntado en las listas de P) es falsa.**
- En la captura de la sesión, los E que apuntan a G se quedan a 4-10 m (129 s) y casi no entran a
  < 4 m (10 s). Hay **0 ataques normales** a G y 10,9/min a la carrera a 0-2 m. P recibe 16,8-18,1
  ataques normales/min a 0-4 m. Es el patrón de "esperar en corro".

### 7.2 Qué es `byte [A+0xC8]` [verificado]

- Lo calcula **`0x4FA530`** (llamado desde `0x4F9010`, `0x4FCF10`, `0x8BE550` y `.xtls 0x9935090`; ya
  descrito en `mirror-ack-formats.md` §5): es el **estado de control** del actor.
  - `3` si es un gimmick (tipo 7).
  - `2` si su dueño `[A+0xA0]` es el `ClientUserActor` local y `byte [A+0x94] != 0`: **el personaje
    que controla el jugador**.
  - `1` en el resto: los actores que simula este cliente (NPC, enemigos y P mientras carga).
  - **`0` forzado** si `u32 [A+0x90]` (usuario dueño) no es el usuario local. La copia lleva
    `0x9010F001`, por eso queda en 0.
- `vf188(k)` (`0x4FA620`) lo lee: `vf188(5)` = "lo controla este cliente" (`!= 0`); `vf188(2)` = "es
  el jugador local".
- **No hay que cambiarlo.** Con un valor distinto de 0, la puerta 5 de Move/Rotate descarta los
  `MoveActorAck`/`RotateActorAck` y la copia se congelaría (`mirror-ack-formats.md` §5).
- Efectos de `+0xC8 = 0` en G [verificado]:
  - `IsInBattle(G)` (`0x221BB60`) va por la rama de `C+0x164`, que vale 0;
  - `AddAggro` exige `vf188(5)`, así que no hay aggro mutuo en G (aggro 0/0);
  - `0x22218F0` (inicio o fin de batalla) **nunca se dispara para G**: su "en batalla" no cambia nunca
    dentro de `0x221D030`.

### 7.3 Quién rellena la lista de rango `C+0x58` [verificado lo que hay; el escritor no está localizado]

Lectores de `C+0x58+16t` en código plano (todos):

| Función | Uso |
|---|---|
| `0x221A940` (desde `0x1EB2BD0` = `.xtls 0xDB76EC0`) | **Solo reordena** por distancia la lista que ya existe (copia `+0x58` → temporal → `0x221ABC0` → `0x2222CE0`). Con la lista vacía no crea nada. Tipos 2 y 3 nunca se reordenan |
| `0x221D200` | Junta claves de `+0x18` y `+0x58` (consulta) |
| `0x1EB2D10` | Huecos alrededor del objetivo; falso si el atacante no está en la lista |
| `0x170D900` | `CheckIsTargetLastRank` |
| `.xtls 0xED78A50` / `0xED7E720` | `BattleRank` / `BattleRankCount` |
| **`.xtls 0xC5DC280`** | **`AICondition_CheckAIBattleType`** (vf1 `0x22E0A30` → `.xtls 0xEB8BE70` → `0x1712510` → `.xtls 0xC5DC280`). §7.4 |

Lo que se descartó como escritor:
- `0x221D030` (alta/baja) solo toca `+0x98`. Su final (`jmp [vtbl+0x190]` → `.xtls 0x9DB0F90`) solo
  hace música e interfaz del jugador local (`0x948760`, `0x50A260`).
- `0x221B640` (= `.xtls 0xE689E50`, junta los atacantes de los tipos 0, 1 y 3) solo lo usan la
  música/UI (`0x948760`), `0xA36260` (lista `+0x208` de la UI, mantenimiento del componente) y
  `ConditionData_CheckBattle` (`0x22575E0`).
- La rama solo-jugador de la actualización del componente (`0xA26A60` → `0xA30520`/`0xA30A40`/
  `0xA30DA0`/`0xA31380`, exige `vf178()`) mantiene `+0x258` para la UI, no `+0x58`.

Conclusión [inferido fuerte]:
- La copia de `+0x98` a `+0x58` está en código `.xtls` ofuscado (desplazamientos calculados), y solo se
  ejecuta para objetivos **que controla este cliente**:
  - en P (`+0xC8 = 2`) el rango es igual a los atacantes;
  - en G (`+0xC8 = 0`) los atacantes están, pero el rango sigue vacío.
- Candidatos a la condición que lo salta:
  - la actualización de IA o de batalla del propio objetivo, que no corre en actores remotos;
  - o el arranque de batalla `0x22218F0` → `0x1EB29A0` (`.xtls 0xDB75040`) sobre la sub-IA, que en G
    nunca se dispara (§7.2).
- No hay ningún mensaje S→C que lo rellene ni que escriba `C+0x164`: en la tabla Troc solo existe
  `ChangeBattleStateReq`, sin Ack.

### 7.4 La puerta que corta el ataque normal: `AICondition_CheckAIBattleType` [verificado + inferido]

`.xtls 0xC5DC280(rcx = [obj_IA+0x68], rdx = ctx, r8 = cond)`:

1. Objetivo = `ctx+0x260` (válido si `byte ctx+0x268`). Si el objetivo no tiene IA (`[X.comps+0x58]`)
   → devuelve 2.
2. Mi tipo de orden `t = [[mi IA]+0x50]+0x40` y mi clave `[yo+0x60]`. Recálculo perezoso
   `0x1EB2BD0(sub-IA del objetivo, t, ahora)` (solo reordena).
3. `total` = n de rangos del objetivo (`C+0x60 + C+0x70 + C+0x80 + C+0x90`). Busco mi clave en
   `C+0x58+16t`, acumulando pesos (`0x22130C0`) de los que van delante.
4. **No estoy en la lista → estado 2.**
5. Si estoy:
   - estado 1 si `total ≤ umbral` (tabla por dificultad);
   - si no, estado 0 si mi peso < capacidad `0x1EB2850` (= `.xtls 0xDB73A70`, cupo por dificultad que
     baja con el tiempo), y 2 si la supera;
   - con `t == 2`, peso 0.
6. Resultado = `byte [cond+8] != estado` (**0 = la condición se cumple**, como `CheckBattleOrderType`).

Lectura [inferido]: estado 0/1 = "puedo atacar (con cupo o libre)", 2 = "esperar". Con G el estado es
**siempre 2**, porque su lista de rango está vacía. Las ramas de "ataque normal" del gráfico no se
cumplen y E espera en corro (`battlewaitpackage`, `findtargetpackage`). Los ataques a la carrera y los
contraataques tras un golpe no pasan por esta condición. Encaja con §1.2, §1.5 y §7.1.

### 7.5 Correcciones (revisión de §4)

| # | Qué | Riesgo | Veredicto |
|---|---|---|---|
| F1 | Escribir el tipo de orden de G | — | **Descartada**: el tipo de orden de G (2) no cuenta. Falta el rango |
| F2 | Apuntar a E en las listas de G | — | **Descartada**: ya lo está (§7.1) |
| — | Cambiar `G+0xC8` | Alto: la copia dejaría de aplicar Move/Rotate | No |
| — | Escribir `C+0x164 = 1` en G | Bajo (byte de nuestra copia; solo lo leen `IsInBattle` y la UI del jugador local) | **No arregla esto**: el rango sigue vacío y `0x22218F0` no se dispara. Solo ayudaría a condiciones `CheckBattleState(objetivo)`. Complemento opcional |
| — | Rellenar o enlazar el vector `C+0x58` de G (puntero, n y capacidad) | Alto: `0x221A940`/`0x2222CE0` reasignan y liberan ese vector (doble liberación) | No |
| **F5** | **Gancho inline en la entrada de `0x22E0A30`** (vf1 de `AICondition_CheckAIBattleType`, vtable `0x58C2BD8`; primeros 5 bytes `E9 3B B4 8A 0C` = `jmp .xtls 0xEB8BE70`; safetyhook reubica el salto). Detalle abajo | Bajo-medio: solo lecturas protegidas en el hilo del juego, sin escrituras ni llamadas internas. Si el significado de los estados es otro, el efecto es nulo o "ataques de más", y se apaga con una tecla | **Recomendada** |
| F4 | Provocación sintética (ATA sin daño) | Medio | Plan B si F5 no basta (p. ej. si otras ramas usan `BattleRank` o los huecos `0x1EB2D10`) |

Detalle del detour de F5 (`cond = rcx`, `obj_IA = rdx`, `ctx = r8`):
1. Llamar al original. Si devuelve 0, o si el objetivo `q[ctx+0x260]` (con `byte [ctx+0x268]`) no es
   la copia (`u32 [obj+0x60] == 0xA010F001`), devolver lo mismo.
2. Si es la copia (y `copy_state_normal()`), imitar lo que haría el motor si G tuviera rango:
   - `E = [[[obj_IA+0x68]]+8]` (comprobar `[E+0x60]`, `0xB01…`, y que su objetivo `C_E+0x178` sea G);
   - `t = [[[E.comps+0x58]+0x50]+0x40]`;
   - `i` = posición de E en `C_G+0x98+16t` (si no está, devolver el original);
   - `n` = suma de `C_G+0xA0` y `+0xB0`;
   - `estado = (n ≤ 2) ? 1 : (i < K ? 0 : 2)`, con **K = 2** (cupo propio de la copia, en el ini);
   - devolver `byte [cond+8] != estado`.
3. Tabla por clave (nunca por dirección) solo para contar. Todas las lecturas con SEH. Sin estado
   compartido con el hilo del mod salvo contadores atómicos.

**Prueba en un PC** (`cdmp_peer` en eco, `offset_m=3`, copia de `save` antes, grupo de bandidos cuerpo
a cuerpo, la copia en medio, sin golpear a los que apuntan a G):

| Fase | Qué | Predicción | Si falla |
|---|---|---|---|
| A (solo contar, sin forzar) | Llamadas a `0x22E0A30` por clase de objetivo y estado calculado | Con G: casi 100 % "no está en la lista" (estado 2). Con P: estados 0/1. Llamadas con G > 0 | Si no hay llamadas con G, la puerta es otra (`BattleRank`/huecos): pasar a F4 o enganchar `0x170D900`/`0x1EB2D10` igual |
| B (forzar, Ctrl+tecla para alternar) | Igual + `s26_att2.py`/`s7_hits_close.py` sobre la captura | E a < 4 m de G; ataques normales a G de **0 a ≥ 8/min** (P: 17-20); golpes a G a ≤ 3 m ≥ 5/min. Sin cierres ni Naks nuevos | Si las llamadas forzadas suben y los ataques no, la condición no es la única puerta: añadir el mismo trato a `0x170D900` y `0x1EB2D10`, o F4 |
| C (alternar) | Apagar el forzado en mitad de la pelea | Vuelve el corro a 4-10 m en < 10 s | — |

Riesgos que vigilar:
- Que varios E "entren" a la vez si `K` es alto (`K = 2` lo limita).
- Que el gráfico use el estado 1 o 0 para otras ramas (embestidas); se ve en la fase B.
- `0xC5DC280` puede usar un actor "padre" (`0x17EF5C0`) para jinetes: excluir E con padre.
