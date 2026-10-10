# Pendiente: a quién atacan los enemigos (aggro) y la respuesta a los golpes del invitado

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco, siguiendo los saltos a `.xtls`);
- re-análisis de las capturas con la copia del otro jugador como objetivo válido
  (`enemies_target_copy=1` o el forzado de `aggro_test`): `065037`, `101713`, `111538`, `114648`, y de
  las sesiones de relevo de enemigos (`082648`, `084001`, `095647`), más las 33 de `D:\cdmp-grabaciones`
  para las estadísticas nativas;
- lectura de `aggro_test.cpp`, `guest_hits.cpp`, `enemy_relay.cpp`, `remote_puppet.cpp`, `coop.cpp`.

El juego no se ejecutó. En las capturas de un PC, la copia (marioneta `0xA010F001`) es el eco del propio
jugador, desplazada **4 m** (`offset_m=4`) y con **300 ms** de retraso.

Marcas: **[verificado]** leído en el código; **[captura]** medido; **[inferido]** pendiente de prueba en vivo.

Notación: **H** anfitrión (dueño de la IA de los enemigos), **G** invitado, **E** enemigo, **P** jugador de
H (`0xA0100001`), **G'** la copia de G en H (`0xA010F001`), copia de enemigo `0xB01F0000+n`.
CTC = `CurrentTargetChangeReq` (2984), CTCAck = `CurrentTargetChangedAck` (3040), ATA =
`PostProcessAttackResult_AttackTargetAck` (2922), AA = `PostProcessAttackResult_AttackerAck` (2509),
AAR = `ApplyActionRestrictionAck` (2162), OPBD = `OnPostBuffDamagedAck` (2681).

Scripts y volcados en `build/research/p_aggro/` (§8).

---

## 0. Resumen

### A. Aggro

1. **No hay tabla de amenaza numérica.** [verificado + captura]
   - Cada enemigo tiene una **lista de aggro** (quién está "en combate" con él) en su componente de
     objetivo (`[[E+0x68]+0x90]`, listas en `+0x120`). Se rellena con `AddAggro 0x221D470`.
   - El cliente la informa al servidor (`AggroRelationListReq`) solo por pertenencia: en el cable cada
     entrada lleva siempre los mismos valores (`0x000F4240`/`0x000F4240`) y la clave. No hay daño acumulado.
   - **Quién entra:** el que fija `SetCurrentTarget` (lo añade él mismo), el que hace daño (el
     **procesador cliente del buff de daño 1021/1023** solo hace eso: `AddAggro(fuente)`), y la
     detección.
   - **Quién se elige:** lo decide el gráfico de IA con `AIFunction_SetCurrentTarget` (motivo `0x18`,
     objetivo sacado de una variable: `TopAggro`, `EventFromActor`, `PriorityDetectTarget`…),
     `…NearAggro` (`0x1D`, **el más cercano de la lista**, distancia 3D) y `…RandomAggro` (`0x1E`).
     La tabla completa de motivos está en el §1.3.
2. **Con la copia como objetivo válido, la IA nativa ya reparte, pero muy sesgada hacia P.** [captura]
   - G' está en la lista de aggro (en 68 de 109 `AggroRelationListReq` de `114648`).
   - Contando solo enemigos activos a ≤30 m: G' recibe el **10-33 %** del tiempo de objetivo
     (P el 67-90 %), aunque estaba a 4 m de P. Llega a haber 32 enemigos a la vez sobre P y 6 sobre G'.
   - Las primeras adquisiciones (`0x18`) van a P incluso cuando G' está más cerca (11 de 14 en `111538`).
     Los cambios P↔G' son casi siempre `0x1D` hacia el más cercano (25 de 27). Cada objetivo se mantiene
     7-15 s (mediana).
3. **Pegar a un enemigo que ya apunta a un jugador no le hace cambiar.** [captura + verificado]
   - P pegó a enemigos que apuntaban a G': no cambiaron a P en 1,5 s en **13 de 15** casos (sí más tarde,
     por `0x1D`).
   - El ATA inyectado con atacante G' tampoco cambió a un enemigo que apuntaba a un NPC (`111538`,
     293,24 s). Los golpes reales de P sí lo hacen en 26-30 ms (`0x18`). Diferencia probable: el buff de
     daño 1021 (aggro) que no inyectamos [inferido].
   - El cambio por golpe de `0x89715B0` (motivo `0x14`) **es solo para víctimas jugador**: exige
     `víctima->vf178()` = "es jugador". No sirve para enemigos.
4. **El forzado con CTCAck es efímero.** [verificado + captura]
   - `SetCurrentTarget` rechaza una petición de la IA si su secuencia (`[IA+0x3B8]`) es menor que
     `comp+0x170`. Con `force=1` en el Ack, la nuestra no se comprueba.
   - La secuencia avanza a **~1,6-1,9 por segundo** en combate (mediana de saltos entre CTC: 6-13). Así
     que `seq+1` bloquea a la IA menos de un segundo.
   - En `065037`, un forzado duró 24,5 s y el otro se deshizo en **16 ms** por `0x1D`.
   - Con el forzado no siempre sale el CTC al servidor.
5. **Ausencia, muerte y distancia no limpian el objetivo de forma fiable.**
   - `IsTargetable` no mira la distancia [verificado]. `copy_state_normal()` no mira la muerte
     (`Status+0x273`; ver `pendiente-reaccion-muerte-invitado.md`).
   - Al retirar la copia en `101713`, tres enemigos siguieron "apuntando a G'" sin ningún CTC [captura;
     estaban inactivos, poco concluyente].
   - Lo nativo que sí limpia: los resets con `force=1`/`seq 0` (motivos `0x02`/`0x09`/`0x04`; `0x09` =
     reinicio total de IA `0x208B450`).
6. **Afecta a la política (hallazgo compartido con `pendiente-proyectiles.md`).** [verificado en código]
   - El relevo deja `0xA010F001` sin traducir (`map_field_locked` lo salta porque `is_network_key`).
   - En el PC de G, un enemigo que en H apunta a G' aparece apuntando a **la copia de H**. Su ATA
     relevado nombra como atacante a la copia de H.
   - Hay que intercambiar P↔G' al relevar: el de H pasa a `0xA010F001` y `0xA010F001` pasa a "tu jugador".

### B. Respuesta a los golpes

7. **En H no se ve el impacto del golpe de G.** [verificado + captura]
   - El 100 % de los ATA reales (2.216) llevan `+0x81 = +0x82 = 1`: "el atacante ya lo pintó".
   - `+0x81 = 0` activa `0x45B700`, el efecto de impacto (posición, cuaternión y `AttackInfo`), con la
     condición de que `+0x20 & 0x20`, que se cumple en todas las plantillas. Es la ruta de "atacante
     remoto" del MMO.
   - Campos medidos: `+0x69` = **punto de impacto** (= `DoFrameEventReq` entrada `+0x0E2`);
     `+0x83`/`+0x8F` = **posición del atacante**; `+0x9B` = su orientación; `+0x75`/`+0xAB` = vectores de
     dirección.
   - Una plantilla vieja pinta en su sitio viejo: hay que reescribirlos.
8. **Los números de daño solo salen si el atacante es el jugador local.** [verificado]
   - OPBD → `0x632EC0` → `.xtls 0x901D160` compara `atacante == clave del jugador local` antes de meter
     el número en la cola de la UI.
   - En H, el golpe de G no puede mostrar número por la vía nativa (el overlay sí podría).
   - **En G, un OPBD inyectado sobre la copia con atacante = su propio jugador sí lo mostraría.**
9. **En G, el efecto local y la pausa de golpe probablemente ya salen.** [inferido fuerte]
   - El 100 % de los AA reales llevan `+0x9E = +0x9F = 1`.
   - Con `+0x9F = 1`, `0x45C690` salta la pausa de golpe (hitstop, ventana de ticks en el struct
     `+0x90..+0xA8`). Con `+0x9E = 1` se salta el efecto del atacante.
   - Es decir: el cliente predice localmente el impacto y la pausa al detectar el contacto. G lo tiene
     igual sobre las copias.
   - Lo que G **no** tiene:
     - el número de daño;
     - las banderas de "resultado del golpe" en su control de personaje (`0x9871E20`, posibles ramas de
       combo que exigen acierto);
     - el evento de IA del atacante (`0x2083AB0`);
     - la reacción inmediata de la copia.
10. **Latencias.**
    - En H, nativo:
      - DFE → ATA en 8,4 ms;
      - AAR y buff en el mismo lote;
      - reacción `StartActionReq` del enemigo a los 18-30 ms.
    - **Relevo H→G medido en eco** (acción, ATA o AAR del original → la de la copia): **417-653 ms** de
      mediana, de los que 300 ms son de playout [captura]. `VaryHPAck`: 235-442 ms.
    - **G ve reaccionar a la copia ≈0,5-0,8 s después de su golpe** en eco, o ≈0,4 s en dos PC con
      RTT de 60 ms [inferido].
    - **En H el enemigo reacciona ~300 ms ANTES de que se vea el tajo de G'**: `GuestHit` se aplica al
      llegar, y las acciones de la copia esperan `delay_ms` [verificado en `coop.cpp`].

### Recomendación corta

- **Aggro:** política en H con cuatro piezas:
  - elegibilidad de G' en el gancho `IsTargetable`: viva (`+0x273`), presente, ≤40 m y enlace fresco;
  - cambio inmediato por golpe con CTCAck;
  - **filtro al inicio de `SetCurrentTarget` `0x2217290`**: solo intercambia P↔G' según la asignación
    de la política, en las llamadas de IA `0x18`/`0x1D`/`0x1E`;
  - reparto con histéresis.
- **Respuesta:**
  - en H, ATA con `+0x81 = 0` y posiciones de G, sincronizado con el playout;
  - en G, OPBD y AA cosméticos al instante, un retraso de relevo propio para enemigos y la deduplicación
    del relevo.
  - El tambaleo predicho en G queda para una fase 2.

---

## 1. El sistema de aggro del juego

### 1.1 Lista de aggro [verificado + captura]

| Pieza | Dirección | Qué hace |
|---|---|---|
| `AddAggro` | `0x221D470(comp, &entradas, u8, modo)` | Exige `actor->vf188(5)` (con IA). Inserta con `0x221D750` y, salvo `modo == 9`, añade al propio actor en la lista del otro (aggro mutuo). Entradas de 20 B `{u32 clave, 3×u16 relación, …}` |
| Quién la llama | `0x2217A35` (dentro de `SetCurrentTarget`), `0xE181C10` (buff de daño), `0x20A24F0` (`CommonAggroBuffProcessor`), `0xA26D00` (tarea de estado de IA), `0x221B250` (NearAggro), `0xE6C3290` | — |
| Buff de daño en el cliente | `ClientDamageBuffProcessor` (vtable `0x55B96A8`), vf2 `0x931420` → `.xtls 0x9AA6590` → `0x209A090` → `.xtls 0xE181C10` | **Solo** `AddAggro(víctima.comp, fuente)` si la víctima no es jugador (`vf178`). El HP viene por `VaryHPAck` |
| Buff de aggro | `AggroBuffData` / `CommonAggroBuffProcessor` vf2 `0x20A24F0` | `AddAggro(…, modo 2)` con la fuente (provocar) |
| Sincronización | `ClientTargetActorComponent` slot 44 `0xA27C00` | Manda `AggroRelationListReq` si la lista cambió. Si se vacía, `ChangeAggroRelationValidTickReq` (u32 actor │ u32 │ u64 tick de caducidad) |

Formato observado de `AggroRelationListReq`: `u8 04 │ u32 actor │ u32 contador │ n × {u32 hash, u32, u32, u32 clave}`
(`a1_arl_dump.py`). Ejemplo con P y G':
`da af fb b0 40 42 0f 00 40 42 0f 00 01 00 10 a0 │ da af fb b0 40 42 0f 00 40 42 0f 00 01 f0 10 a0`.
- **Los dos jugadores tienen los mismos valores**: es pertenencia, no amenaza.
- No hay Nak por la clave de G'.

### 1.2 Elección de objetivo [verificado]

- `SetCurrentTarget 0x2217290(comp, res*, objetivo, u64* seq, u8 a5, u8 saltar_seq, u8* motivo)`.
  - Con `saltar_seq == 0` y `*seq < comp+0x170`, rechaza.
  - Si el objetivo ya es el actual, no hace nada.
  - Si no, guarda `comp+0x170 = *seq`, `comp+0x178 = objetivo` y `AddAggro(objetivo)`, y manda el CTC.
  - En la entrada (para un gancho): `rcx` comp, `r8d` objetivo, `r9` &seq, **motivo = `*(u8*)[rsp+0x38]`**,
    `saltar_seq = [rsp+0x30]`. La clave del enemigo está en `[[comp+8]+0x60]`.
- Las funciones de IA llaman con `saltar_seq = 0` y la secuencia de la IA (`[r13+0x3B8]`). Las rutinas
  del componente de objetivo y el CTCAck (`0xB68520`) usan `saltar_seq = 1` / `force`.
- **NearAggro** (`0x22EDDD0` → `.xtls 0xEC7E8C0` → `0x17231B0`): calcula la distancia al cuadrado a cada
  entrada de la lista (con desplazamiento de sector) y elige la mínima. **La distancia ya es nativa.**
- **SetCurrentTarget de IA** (`0x22ED9C0` → `0xEC76A00` → `0x17221A0`): el objetivo sale de
  `0x207C8F0(ctx, sel)`, un selector de 44 valores del enum `ActorTargetType` (cadenas: `TopAggro`,
  `CurrentTarget`, `EventFromActor`, `EventTargetActor`, `PriorityDetectTarget`, `SavedTarget`,
  `BattleLoopPartner`…). No se ha descodificado qué selector usa cada gráfico.
- `IsTargetable 0x22141E0`: reglas en `companeros-mercenarios.md` §3.3.
  - **No hay regla de distancia.**
  - La rechaza `TS+0x3DC` (marca de "movida por red"), que es lo que salta hoy el gancho.

### 1.3 Motivos del CTC (llamadores de `0x2217290`; `settarget_callers.txt`) [verificado]

| Motivo | Origen | Visto en capturas |
|---|---|---|
| `0x02` / `0x04` / `0x09` | Limpiezas con `force` y `seq 0`. `0x09` = `0x208B450`: reinicio de IA (vacía el aggro con modo 9 y la detección) | A 0, sí |
| `0x0C` | `0x2221D90` (componente común) | — |
| `0x13` | `ClientTargetActorComponent` `0xA2E8F0`/`0xA2F770` (mantenimiento) | — |
| **`0x14`** | `0x89715B0`: golpe a un **jugador** → su objetivo pasa a ser el atacante | No (no genera CTC) |
| `0x15`, `0x28`, `0x2D`, `0x30` | Rutinas de objetivo `0xA31D30`/`0xA36D00` | — |
| `0x17` | `CharacterControl` vf82 `0x232B120` | — |
| **`0x18`** | `AIFunction_SetCurrentTarget` | La mayoría (P y NPC) |
| `0x19` / `0x1A` | `…SetTargetDockingChild` / `…SetEndBehaviorTarget` | — |
| **`0x1B`** | `AIFunction_ClearCurrentTarget` | A 0 |
| **`0x1D`** | `AIFunction_SetCurrentTargetNearAggro` | Cambios P↔G' |
| `0x1E` | `AIFunction_SetCurrentTargetRandomAggro` | — |
| `0x1F` / `0x21` / `0x22` | `…ReportNearTargetFromStageBattleTarget` / `…SetBattleLoopPartner` / `…ClearAggro` | — |
| `0x20` / `0x24` / `0x26` / `0x27` | IA vf124 `0x91E790` / Status vf97 `0x6269B0` / `0x21C6870` / Sequencer `0x9EE9E0`, `0x1F5C100` | `0x25`/`0x26` hacia NPC |

---

## 2. Lo que hace hoy la IA con dos jugadores (capturas)

### 2.1 Reparto (`a8_active_split.py`, `a2_split.py`) [captura]

Muestreo de 1 s, enemigos activos (alguna Req en los últimos 2 s) a ≤30 m de P:

| Sesión | Segundos-enemigo sobre P | Sobre G' | Parte de G' | Máximo a la vez P / G' |
|---|---|---|---|---|
| `065037` (forzado a mano) | 658 | 326 | 33 % | 9 / 6 |
| `101713` | 1.377 | 154 | 10 % | 23 / 2 |
| `111538` | 1.883 | 731 | 28 % | 32 / 6 |
| `114648` | 1.426 | 422 | 23 % | 16 / 6 |

Cambios P↔G' y tiempo que se mantiene el objetivo:

| Sesión | P→G' (`0x1D` / `0x18`) | G'→P (`0x1D` / `0x18`) | Elegido = el más cercano (`0x1D`) | Mantener P→G' / G'→P (mediana) |
|---|---|---|---|---|
| `111538` | 18 / 6 | 12 / 3 | 25 de 27 | 7,1 s / 10,0 s |
| `114648` | 12 / 6 | 10 / 0 | 15 de 20 | 11,5 s / 8,5 s |

- Primera adquisición (`0x18`) con G' más cerca: en `111538` se eligió a P en 11 de 14 casos.
- **Los enemigos sí atacan a G'**: 2-4 `DoFrameEventReq` con G' de objetivo por sesión, más 2-3 con G'
  alcanzado de rebote.

### 2.2 Golpes y cambio de objetivo (`a5_switch_cause.py`, `a4_injected_ata.py`) [captura]

| Caso | Cambio a quien pega en ≤1,5 s |
|---|---|
| P pega a E que apuntaba a G' | **2 de 15** (1,2 s, `0x1D`). Los demás, 2,7-9,7 s después, por `0x1D` |
| P pega a E que apuntaba a un NPC | 10 de 17. En `pieza4-capturas.md` §5.1: 26-30 ms (`0x18`) cuando no había jugador |
| ATA inyectado (atacante G') a E que apuntaba a un NPC (`111538` 293,24 s) | **No** en 12 s |
| ATA inyectado a E que apuntaba a P (264,25 s; 353,82 s) | No: cambió a los 7,7 s por `0x1D`, o a otro NPC |

- En el lote real de P van siempre el buff de daño (1021/1023) y el de golpe (1104/1105).
- El ATA inyectado no llevaba el 1021, que en el cliente es lo único que mete al atacante en la lista
  (§1.1).

### 2.3 Forzado con CTCAck (`a6_forced.py`, `a7_seq.py`)

| Inyección (`065037`) | Resultado |
|---|---|
| 93,88 s, E `B0100197` → G', seq 38 | `StartActionReq` con objetivo G' a los 45 ms. **Ningún CTC al servidor.** Se mantuvo 24,5 s; luego `0x1D` a un NPC (seq 78) |
| 181,30 s, E `B0100191` → G', seq 155 | A los **16 ms**, CTC → P con `0x1D` y seq 161: la IA lo deshizo al momento |

Secuencias en `111538` y `114648`: suben ~1,6-1,9 por segundo; saltos entre CTC con mediana de 11-13.
Hay saltos negativos tras forzados y `seq 0` en las limpiezas `0x02`/`0x09`.

### 2.4 Ausencia de la copia (`a3_absent.py`)

- `101713`, 158,42 s: la copia se retira (motivo `0x15`) con 3 enemigos apuntándole. No hubo ningún CTC
  después. Esos enemigos llevaban más de 30 s sin mandar nada, así que la prueba no es concluyente.
- Las salidas de G' observadas son `0x1D` (hacia P o NPC), `0x1B` (a 0) y `0x09` forzado.

---

## 3. Política de aggro propuesta

### 3.1 Objetivos

1. Repartir: con los dos cerca, ninguno debería cargar con más del 65 % de los enemigos a la vez.
2. Quien pega, atrae: tras un golpe de G (o de P) a un enemigo que apunta al otro, cambio a quien pega
   (con umbral y tiempo mínimo).
3. Respetar la distancia: no forzar hacia un jugador que esté más de 8 m más lejos que el otro, salvo por
   golpe.
4. Nunca hacia un G' no elegible: muerto, ausente, desconectado, a más de 40 m del enemigo, o en carga o
   menú.

### 3.2 Reglas (en H; tick de 250 ms más eventos)

| Regla | Disparador | Acción |
|---|---|---|
| R0 Elegibilidad | Siempre | `G'` elegible si: `puppet_spawned()`, no muerta (`Status+0x273 == 0` además de lo que ya mira `copy_state_normal`), último paquete de G hace <2 s, `d(E,G') ≤ 40 m`. Si no, el gancho `IsTargetable` devuelve `false` para G' |
| R1 Retirada | E apunta a G' y G' deja de ser elegible | CTCAck: a P si P es elegible y `d(E,P) ≤ 40 m`; si no, objetivo 0 (motivo `0x1B`), `force=1` |
| R2 Golpe | GuestHit aplicado y E apunta a un NPC o a nada | CTCAck → G' inmediato (como el nativo de 26-30 ms) |
| R3 Amenaza | Golpe de X ∈ {P, G'} a E que apunta al otro | `amenaza_X(E)` = daño de los últimos 10 s. Cambiar si `amenaza_X ≥ 1,3 × amenaza_otro`, el objetivo lleva ≥4 s y X es elegible |
| R4 Reparto | CTC nativo de E hacia P o G' (`0x18`/`0x1D`/`0x1E`) | Si \|d(E,P) − d(E,G')\| < 5 m, nadie le pegó en 5 s y `n_destino − n_otro ≥ 2`: asignar al otro |
| R5 Tope | Igual que R4 | Como mucho 6 enemigos a la vez sobre G' (BDO usaba un tope de aggro por personaje; `bdo-emuladores-monstruos.md`). Los que sobren se quedan con P |
| R6 Histéresis | Siempre | Como mucho un cambio forzado por enemigo cada 4 s, salvo R1. Sin cambios si E está muerto o en reinicio (`0x02`/`0x09`) |

- Estado por enemigo, indexado por clave (nunca por dirección): asignación `A(E) ∈ {P, G', libre}`,
  objetivo actual (de sus CTC), daño por atacante con sello de tiempo, último cambio forzado.
- Se excluyen jefes y monturas hasta validarlos en su propio estudio.

### 3.3 Mecanismos (de menos a más invasivo)

| # | Mecanismo | Para qué | Riesgo | Esfuerzo |
|---|---|---|---|---|
| M1 | **`IsTargetable` con elegibilidad**: ampliar el gancho actual (inline al inicio, ya instalado) | R0. Impide nuevas adquisiciones de un G' no válido. Las rutinas de mantenimiento probablemente sueltan al actual (`0x13`/`0x15`/`0x28`) [inferido] | Bajo | 0,5 días |
| M2 | **CTCAck** (`aggro_test::force_target_on_puppet` generalizado): `seq = [comp+0x170] + 1` leído justo antes, `force = 1`, motivo `0x18` para fijar y `0x1B` para soltar | R1, R2 y R3 (cambio inmediato) | Bajo, pero dura menos de 1 s sin M3. **Nunca** usar `seq + N` grande: bloquea a la IA unos N/1,8 s | 0,5 días |
| M3 | **Filtro al inicio de `SetCurrentTarget` `0x2217290`** (el mid-hook `g_set_target` ya está en la cabecera de la función, permitido). Si el actor es un enemigo local, el motivo ∈ {`0x18`, `0x1D`, `0x1E`}, el objetivo ∈ {P, G'} y `A(E)` es el otro y es elegible, se reescribe `r8d` | Que la asignación aguante sin pelear con la IA (R3-R5). `AddAggro` del sustituto lo hace la propia función | Medio. Corre en el hilo del juego: tabla sin bloqueos y lectura de `[rsp+0x38]` protegida con SEH. Solo intercambia jugadores, nunca toca NPC ni limpiezas | 1,5-2 días |
| M4 | **`ApplyBuffAck` 1021** (aggro) con fuente G' en el lote del golpe | Que G' entre en la lista "como un golpe real" y quizá reproducir el cambio nativo `0x18` de 26-30 ms | Medio. Otros procesadores del buff sin revisar; id del rango propio `0x8000F000+n` | 0,5 días + prueba A2 |
| — | Tocar la lista de aggro a mano | — | Alto (llamadas internas) | Descartado |

**Recomendado:** M1 + M2 ya, M3 después de la prueba A4 y M4 como experimento.

### 3.4 En el PC de G (con el arreglo del hallazgo 6)

- La IA no corre en G: las copias reciben el objetivo por `StartActionAck +0x16C` (y en el futuro por
  CTCAck relevado).
- Hay que **intercambiar P↔G' en el emisor**: `0xA0100001` → `0xA010F001` y `0xA010F001` → la clave del
  jugador del receptor, o un marcador reservado que el receptor resuelva.
- Hay que hacerlo en `StartActionReq +0x16C`, ATA `+0x04`, AAR `+0x49`, `VaryHPAck +0x54`,
  `NotifyHandleDeadAck +0x04` y los objetivos de proyectil.
- Cambia el significado del cable: subir `kVersion`.

---

## 4. Respuesta a los golpes del invitado

### 4.1 En H: efecto, sonido y sangre [verificado + captura]

`0x451020` (ATA en el cliente), en orden:
1. `0x45C8A0` (empuje si `flags & 4`);
2. **si `struct+0x90` (cable `+0x81`) == 0**, `0x45B700(comp, víctima, atacante, &acc, AttackInfo,
   &+0x94, &+0xAC, &+0xA0, &+0xBC, local)`;
3. `0x45D3E0`;
4. evento de IA `0x2083D90`;
5. `0x45D800` → `0x89715B0` (solo víctimas jugador).

`0x45B700`:
- monta el descriptor del impacto con el bloque de banderas copiado del ATA (`local+0x1B8..+0x1D0`);
- exige `local+0x1D0 & 0x20` (= cable `+0x20`: 0x37/0x23/0x27/0x60 en las plantillas);
- lanza los efectos con `0x45B450` (×4) y termina en `0x45BEE0`, que mira el byte global `0x6CA0088` y
  la condición de `AttackInfo` del atacante.

Campos de posición del ATA (`b4_ata_positions.py`, 45 golpes de `064320`) [captura]:

| Cable | Coincide con | Distancia mediana |
|---|---|---|
| `+0x69` | `DoFrameEventReq`, entrada `+0x0E2` (punto de impacto) | 0,00 m |
| `+0x83`, `+0x8F` | Posición del MoveData del **atacante** | 0,00 m |
| `+0x75`, `+0xAB` | Vectores pequeños/unitarios de la entrada (`+0x0EE`/`+0x0FA`) | — |
| `+0x9B` | Cuaternión del atacante | — |

Ningún ATA de las capturas tiene `+0x81 = 0` (`b5_ata_flags.py`: 2.216 de 2.216 con `1/1`, también
NPC→NPC y NPC→jugador).

**Receta B1 (es la prueba R3):**
- plantilla del mismo hash;
- `+0x81 = 0`, `+0x82 = 1`;
- `+0x69` = posición de E en H + (impacto − posición de la copia) medido en G;
- `+0x83`/`+0x8F` = posición de G' en H; `+0x9B` = rotación de G';
- `+0x75`/`+0xAB` = los vectores de G.

Hace falta ampliar `GuestHit`:
- `u64` reloj de juego de G en el golpe;
- `vec3` impacto relativo a la copia;
- 2×`vec3` de dirección;
- `u16` id de golpe.

Son ~50 B más y suben `kVersion`.

**Sincronía:**
- `guest_hits_on_event` aplica al llegar, pero el tajo de G' se reproduce `delay_ms` (300) después.
- Propuesta: aplicar el daño al llegar y programar ATA + AAR + buff a `ts_G + offset + delay_ms`, como
  `puppet_event`.
- Coste: la reacción que vuelve a G se retrasa lo mismo (por eso conviene la fase 2 del §4.3).
- Alternativa: un `delay_ms` menor para G' en combate.

**Números en H:** la UI nativa los filtra (§0.8). Si se quieren, que los dibuje el overlay de ImGui (ya
proyecta posiciones del mundo) con el daño aplicado y en otro color. No inyectar OPBD con atacante = P,
porque mentiría y dispararía el vf198 de estado.

**AAR `+0x1C`:** `0x48B400` → `.xtls 0x98CD370` llama a `0xB55290` sobre un actor del gestor del
jugador local con la clave del enemigo. Efecto sin identificar (¿contador de golpes o combo de P?).
Prueba R1e.

### 4.2 En G: respuesta inmediata (fase 1, sin predicción de animación)

| # | Qué | Cómo | Por qué es seguro |
|---|---|---|---|
| G1 | **Número de daño** | Al ver su `DoFrameEventReq` a una copia, inyectar OPBD (53 B): `+0x00` copia, `+0x04`/`+0x08` su jugador, `+0x0C` daño estimado ×1000 (base × defensa con las tablas de `guest_hits`; H puede mandar su defensa aprendida de la variante), `+0x2C` id, `+0x34` = 1 | El filtro de la UI pide atacante == jugador local [verificado]. La copia no tiene servidor que lo contradiga |
| G2 | **Banderas de acierto** | AA cosmético: plantilla del último AA real del mismo hash en G (o semilla), `+0x04` = copia, `+0x9E = +0x9F = 1` como los reales | `+0x9F = 1` salta la pausa de golpe (ya es local). Activa `0x9871E20` (resultado del golpe para el gráfico de acciones) y el evento `0x2083AB0` [inferido] |
| G3 | **Retraso propio para enemigos** | `[coop] enemy_delay_ms` (p. ej. 120) para `EnemyAct`/`EnemyPos`/ATA/AAR, en vez de los 300 del jugador | Recorta ~180 ms de reacción. Riesgo: más tirones si hay pérdidas |
| G4 | **Deduplicar el relevo** | En G, descartar los ATA/AAR relevados cuyo atacante/fuente sea su propia G' en H (tras el intercambio del §3.4: "yo") o, como mínimo, forzar `+0x81 = 1`. A los ATA de golpes de P relevados a G ponerles `+0x81 = 0` (G es observador) | Evita el doble efecto. Da a G el impacto de los golpes del anfitrión |

Cifras para dimensionar (`b3_relay_latency2.py`, eco en un PC) [captura]:

| Sesión | Acción | ATA | AAR | `VaryHPAck` |
|---|---|---|---|---|
| `082648` | 417 ms | 426 | 422 | 235 |
| `084001` | 611 | 605 | 605 | 442 |
| `095647` | 653 | 621 | 661 | 291 |

### 4.3 Fase 2: tambaleo predicho en G y deduplicación

- **Predicción:**
  - en el golpe de G a una copia con P(AAR) ≥ 0,95 para esa clave, inyectar en la copia el trío
    ATA + AAR + buff de golpe con el mismo id (receta de `pieza4-tambaleo.md` §5.1);
  - id en un rango propio del invitado `0x8000E000+n`;
  - atacante = su jugador;
  - prueba F2: no se sabe si en modo 2 `0x48B400`/`0x207AA10` lanzan la acción.
- **Deduplicación con id de golpe:**
  - H marca el primer `StartActionReq` de E en los 100 ms siguientes al lote inyectado de `GuestHit` N,
    y su ATA/AAR, con la causa N (`EnemyAct` + `u16`; subir `kVersion`);
  - G descarta el ATA/AAR de N y, si su predicción sigue en curso (<600 ms), la acción de reacción de
    N;
  - acepta la siguiente (la salida `A73D6EC4`, etc.);
  - si H dio otro resultado (bloqueo `84`, esquiva `87`), G acepta la acción relevada aunque se vea un
    corte.
- **Sin id** (solo heurística): descartar en G la primera acción del conjunto de reacciones de
  `pieza4-tambaleo.md` §4.2 que llegue antes de 1,5 s tras su golpe. Es frágil con varios golpes
  seguidos.

---

## 5. Pruebas en vivo (un PC, `cdmp_peer` en eco, `offset_m=10-15`, copia de `save` antes)

| # | Qué | Predicción | Decide |
|---|---|---|---|
| A1 | Línea base con la build actual, `guest_hits=1`. Medir con `a8`/`a2` | G' recibe el 10-33 %; un golpe a la copia de un enemigo que apunta a P no le hace cambiar en 1,5 s | Referencia |
| A2 | M4: buff 1021 con fuente G' en el lote de `GuestHit` | E que apuntaba a un NPC o a nada → CTC `0x18` a G' en 20-40 ms | Si el aggro nativo por golpe basta para R2 |
| A3 | M2 en R2 | `StartActionReq +0x16C` = G' en ≤50 ms; la IA lo mantiene hasta el siguiente `0x1D` (que irá al más cercano) | Duración real del forzado |
| A4 | M3 con `A(E)` fijado por R3/R4 | Los CTC de la IA salen ya con G' (motivos `0x18`/`0x1D`); sin vaivén P↔G' en menos de 4 s; reparto del 40-60 % con los dos a <5 m | Si el filtro es estable |
| A5a | Elegibilidad por distancia: `offset_m=50` | Los enemigos que apuntaban a G' pasan a P (R1) en ≤0,5 s | R0/R1 |
| A5b | Muerte de la copia (con el arreglo `+0x273`) | `IsTargetable` = false y CTC fuera de G' | R0 |
| A5c | Cortar `cdmp_peer` | Copia retirada y retarget forzado a P | R1 |
| A6 | `seq+1`, `+4`, `+8` en CTCAck | La IA tarda ~0,5 / 2 / 4 s en volver a cambiar | Semántica de la secuencia |
| B1 (=R3) | ATA `+0x81 = 0` con las posiciones de G | Chispas, sangre y sonido en el original, en el punto de impacto (captura `CopyFromScreen` 2560×1440). Sin sacudida de cámara en H | Efecto en H |
| B2 | Programar la inyección a `+delay_ms` | La reacción coincide con el tajo de G' | Sincronía |
| B3 | G1 (OPBD en la copia) | Número de daño sobre la copia al instante | Números en G |
| B4 | Golpear una copia lejos de su original y mirar | Chispas y pausa de golpe locales sin ningún Ack | Confirma §0.9 |
| B5 | G3, `enemy_delay_ms=120` | Relevo de acciones ~250-350 ms en eco; sin tirones apreciables | Ajuste del retraso |
| B6 | G2 (AA cosmético) | Sin Nak; las combinaciones que piden acierto siguen sobre la copia | Banderas de acierto |
| R1e | AAR `+0x1C = 0` frente a 1 en los golpes de G | Ver si cambia algo en P (¿contador?) y si el tambaleo sigue | `0xB55290` |
| F2 | Trío AAR + buff sobre una copia en G | ¿Tambaleo local en modo 2? | Viabilidad de la fase 2 |

---

## 6. Plan recomendado

1. **Arreglos base (1 día):**
   - el intercambio P↔G' en el relevo (§3.4, `kVersion`);
   - `+0x273` en `copy_state_normal`;
   - R0 + R1 (M1/M2).
2. **Respuesta en H (1 día):** `GuestHit` v2 (impacto, direcciones, reloj, id) y B1 + B2.
3. **Respuesta en G (1 día):** G1, G3 y G4. G2 si B6 sale bien.
4. **Política (2 días):** R2/R3 con M2; M3 tras A4; R4/R5 al final, con interruptores en el ini
   (`aggro_policy=0/1`, `aggro_max_on_guest=6`, `aggro_leash_m=40`).
5. **Fase 2 (2-3 días):** tambaleo predicho + deduplicación con id, si F2 sale bien.

---

## 7. Preguntas abiertas

- Qué selector de `ActorTargetType` usa la mayoría de los gráficos con `0x18`, y por qué prefiere a P en
  la primera adquisición (`PriorityDetectTarget`/`CheckAggroToPlayer`?).
- Cómo avanza la secuencia de IA `[IA+0x3B8]`: A6.
- Si las rutinas de mantenimiento sueltan un objetivo que `IsTargetable` rechaza, y con qué motivo.
- Qué hace `0xB55290` (AAR `+0x1C`) y si `0x45BEE0` sacude la cámara de H con un atacante remoto.
- Si el CTCAck forzado sin CTC al servidor deja estado incoherente en el servidor (no se ha visto
  ningún efecto).
- Otros procesadores del buff 1021 en el cliente (`ClientDamageBuffProcessor` vf1 `0x2099F50`, vf11
  `0x9315D0`).

---

## 8. Scripts y volcados (`build/research/p_aggro/`)

Ejecutar con `build/research/venv/Scripts/python.exe` desde esa carpeta. Usan `../pieza4/pk.py`.

| Script | Qué hace |
|---|---|
| `a0_census.py` | CTC por clase de objetivo, motivo y `force`; tamaños de ARL/ValidTick; ARL con G' |
| `a1_arl_dump.py <sesión> [n]` | Volcado de `AggroRelationListReq`, `ChangeAggroRelationValidTickReq` y CTC |
| `a2_split.py` | Segundos-enemigo por objetivo, cambios P↔G' con motivo y distancias, tiempos de mantenimiento, golpes de P a E sobre G' |
| `a3_absent.py`, `a3b.py` | Qué pasa al dejar G' o al retirarla |
| `a4_injected_ata.py` | Objetivo del enemigo antes y después de cada ATA inyectado con atacante G' |
| `a5_switch_cause.py` | Golpes de P a enemigos que no le apuntaban: lote S>C y cambio |
| `a6_forced.py` | CTCAck forzados de `065037` y lo que siguió |
| `a7_seq.py` | Ritmo de la secuencia del CTC |
| `a8_active_split.py` | Reparto P/G' con enemigos activos a ≤30 m |
| `b1_attackerack.py` | Bytes fijos del AA y del ATA (`+0x9E`/`+0x9F`) |
| `b2_relay_latency.py`, `b3_relay_latency2.py` | Latencia del relevo original → copia |
| `b4_ata_positions.py` | Significado de `+0x69`/`+0x75`/`+0x83`/`+0x8F`/`+0xAB` del ATA |
| `b5_ata_flags.py` | `+0x81`/`+0x82` en todos los ATA |
| `ctx_calls.py <rva>`, `vt_of.py <clase>`, `slot.py <off> <clase>` | Contexto de llamadas, vtables y ranuras |
| `settarget_callers.txt`, `addaggro_callers.txt` | Contexto de las llamadas a `0x2217290`/`0x221D470` |
| `*.asm` | Volcados citados |

### Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x2217290` | `SetCurrentTarget`; `[rsp+0x30]` = saltar la secuencia; `[rsp+0x38]` = &motivo; `0x2217A35` = `AddAggro` |
| `0x221D470` / `0x221D750` | `AddAggro` / inserción en la lista |
| `0x17221A0` (vía `.xtls 0xEC76A00`) | `AIFunction_SetCurrentTarget` (`0x18`); selector `0x207C8F0` |
| `0x17231B0` (vía `0xEC7E8C0`), `0x221B250` | `AIFunction_SetCurrentTargetNearAggro` (`0x1D`), más cercano |
| `0x1725730` / `0xEC81C20` | `RandomAggro` (`0x1E`) / `ClearCurrentTarget` (`0x1B`) |
| `0x208B450` | Reinicio de IA (motivo 9: aggro, objetivo y detección) |
| `.xtls 0x89715B0` | Cambio del objetivo de un jugador a su atacante (`0x14`) |
| `0x931420` → `.xtls 0x9AA6590` → `.xtls 0xE181C10` | `ClientDamageBuffProcessor`: `AddAggro(fuente)` |
| `0x20A24F0` | `CommonAggroBuffProcessor` vf2 |
| `0x4FE260` / `0x8A67D0` | `vf178`: `ClientActor` (usuario local y `+0x94`) / `ClientUserActor` (`+0x94`) |
| `0x45B700`, `0x45B450`, `0x45BEE0` | Efecto de impacto del ATA (`+0x81 == 0`) |
| `0x45C690` | AA: pausa de golpe (struct `+0x90..+0xA8`) y retroceso. Salta si struct `+0xCB` (cable `+0x9F`) ≠ 0 |
| `.xtls 0xA38F720`, `.xtls 0xA796DC0`, `.xtls 0xF5D7A90` | AA: handler, núcleo, deserializador |
| `.xtls 0x9871E20` (`0x86BC10`) | Banderas de resultado del golpe en el control de personaje |
| `0x2083AB0` | Evento de IA del atacante |
| `0xB70890` → `0x632EC0` → `.xtls 0x901D160` | OPBD: número de daño solo si atacante == jugador local |
| `.xtls 0x98CD370` → `0xB55290` | AAR `+0x1C` (efecto desconocido sobre el jugador local) |
