# Copias de enemigos de grupo: originales invisibles y "loginCharacter 실패"

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`)
y solo valen para esta build. Investigación de solo lectura: capstone sobre el exe en disco
(`cddis.py`, `lin2.py`, `horse2/xtrace.py` para el código reubicado en `.xtls`), las capturas
`cdmp_packets_20261004-090931.bin` (log `captures/cdmp_invisible2.log`),
`cdmp_packets_20261004-090008.bin` (log `captures/cdmp_invisible_enemies.log`) y
`captures/cdmp_packets_20261003-112627.bin`. El juego no se ejecutó y no se tocó el código del mod.

Marcas:
- **[verificado]**: leído en el desensamblado (se da la dirección).
- **[captura]**: medido en las capturas o en los logs (se da el script).
- **[inferido]**: deducción; la §6.8 dice cómo confirmarla en un PC.

Offsets "end-N" = desde el final del payload del `ActorLoadDataAck` (`u16 len | registro`), igual que
en `foreign.py`. Offsets "+N" = desde el inicio de ese payload.

---

## 0. Resumen

1. **Las secciones con claves ajenas no son de TargetFormation ni de Faction** (sus `vf11` son
   `0x3600C0` = `mov al,1; ret`, no leen nada) [verificado]. Son tres cosas:
   - **`ClientPartyActorComponent`** (slot `0x108`, `vf11 0x21D8020`): número de grupo global,
     líder, lista de miembros, FormationKey, índice propio, seguidores y líder otra vez
     (end-411…end-333) [verificado + captura].
   - **`ClientTargetActorComponent`** (slot `0x90`, `vf11 0x2213D30` → `.xtls 0xE674C90`): su último
     campo, `+0x198`, es la clave del líder (end-1175 en ese registro) [verificado + captura].
   - **El propio actor**: tras los componentes, el unpack (`0x17EA310` → `.xtls 0xC867CC0`) lee
     `u32 → actor+0xAC` (**padre**) y `u16 n + n × u32 → actor+0xB0` (**hijos**). Es el final del
     registro (end-6 y "la lista cerca de end-20") [verificado].
2. **Las dos copias rotas son distintas según el papel del original** [verificado + captura]:
   - **Copia de un miembro** (`+0xAC` = líder real): el bucle de actores (`0x8B7D78`) no la mete
     nunca en la cola de login porque su "padre" existe. Se queda en estado `+5E = 8`, `+CF = 0`
     para siempre (32 de 32 copias de miembro en los dos logs).
   - **Copia de un líder** (`+0xB0` = hijos reales): el cliente monta su "grupo de login" con la
     copia **y sus hijos reales**, ejecuta el post-proceso de creación de componentes **sobre los
     reales** (`0x4FCF10`, en un hilo trabajador) y luego falla en el primer hijo real porque no
     está en estado 8 (`eErrNoUserNotLogin`). La copia se destruye (9 de 9 líderes: "NOT in the
     game's actor list").
3. **Por qué se vuelven invisibles los originales** [verificado; el efecto visual, inferido]:
   - el post-proceso del CharacterControl (`vf18 0x85E440` → `.xtls 0x984F340`,
     "onCharacterResourceLoadCompleteThread") pone el bit `2` de `Status+0x390` (y `0x200`,
     `0x2000`, `0x4000` según el estado) en **cada hijo real**;
   - el manejador de cambio de banderas (`0x6293A0` → `.xtls 0x8FF5920`) pone el alfa del modelo a
     `0.0` si `Status+0x390|0x394 ≠ 0` (y no tiene el bit 8). Esto vale para los tipos 1 y 3 a 9;
   - esos bits solo se limpian en el final normal del login (`CC vf24 0x85E790` → `0x470430` →
     `.xtls 0x89C1230` → `0x4703C0`), que los hijos reales nunca vuelven a pasar.

   **Resultado: los hijos reales del líder copiado quedan vivos, con IA y atacando, pero con alfa
   0.**
4. **"Actor AsyncPostProcessForCreate에서 loginCharacter 실패했습니다"** sale de `0x4FD2E0`, el
   cierre del login de un grupo [verificado + captura]:
   - el código `2920878040` es `hashlittle("eErrNoUserNotLogin", 0xC5EDE)`, de la comprobación
     `actor+0x5E ≠ 8`;
   - el segundo "valor" del log (`0x400_0000000N`) es `capacidad 0x400 | N actores en el grupo`, y
     `N = 1 + hijos` en los 9 casos.
5. **El servidor no interviene.** Tras inyectar las copias no llega ningún mensaje de grupo sobre
   los originales, y los hijos del líder copiado siguen mandando `StartActionReq`/`MoveActorReq`
   [captura]. Todo es estado local del cliente.
6. **Recomendación:** la copia tiene que llegar como un enemigo **solo**:
   - cola del actor: `padre = 0` y `0 hijos`;
   - bloque de Party sustituido por la plantilla de 50 B de un NPC solo, con el registro acortado;
   - `Target+0x198` = la clave de la copia;
   - validación final contra **todas** las claves de actores conocidas, rechazando la copia si
     queda alguna.

   Los tres localizadores encuentran su sección en 1.104 de 1.104 cargas de tipo 3 y 1.391 de
   1.391 de tipo 6 de tres capturas. El prototipo deja limpias 332 de 347 cargas hostiles; las 15
   restantes son jinetes enganchados a un carro (tipo 7), que no se deben copiar todavía (§6).
7. **En los mensajes relevados** viajan sin traducir: el padre de `StartActionReq` (`+0x116`,
   gimmicks de tipo 7), su objetivo (`+0x16C`: el caballo del anfitrión u otro bandido), y el
   atacante o la fuente de `AttackTargetAck` y `ApplyActionRestrictionAck` (el caballo del
   anfitrión). Hay que mapearlos campo a campo (§5 y §6.5).

---

## 1. Qué componente lee cada sección (pregunta 1)

### 1.1 El unpack del actor

`0x17EA310` salta a `.xtls 0xC867CC0` (código reubicado y legible) [verificado]:

```text
u32                      → actor+0x90  (usuario de control)
para cada {comp, id} de actor+0x78 (16 B por entrada, actor+0x80 entradas):  comp->vf11(stream)
u32                      → actor+0xAC  (clave del actor padre)
u16 n, n × u32           → vector actor+0xB0 (claves de los hijos)
```

- `actor+0xAC` es lo que el cliente llama "Parents": si el padre no existe, el actor se destruye
  con `eErrNoParentsNotExist` (global `0x146CD8EF0`, `0x8B8487`) [verificado].
- En los campamentos, el padre es el líder del grupo y los hijos son los demás miembros. En los
  jinetes, el padre es el carro (tipo 7) [captura].

### 1.2 Registro de un miembro (`0xB01000A3`, 7.798 B, captura 090931)

| Offset | Bytes de ejemplo | Campo | Lector |
|---|---|---|---|
| end-1199 | `00000000` | `Target+0x178` | Target `vf11` (`.xtls 0xE674C90`) |
| end-1195 | `FFFFFFFF` | `Target+0x17C` | idem |
| end-1191 | `00000000` | `u32 n` entradas de 12 B (siempre 0 aquí) | idem |
| end-1187 | `FF×8` | `Target+0x118` u64 | idem |
| end-1179 | `00000000` | `Target+0x168` | idem |
| **end-1175** | `9F0010B0` | **`Target+0x198`: clave del líder.** En solos y líderes es la propia clave | idem |
| end-411 | `04000000 01020000` | **Party u64 nº de grupo** (`Party+0x18`) = `0x00000201_00000004`. El bit 9 de la mitad alta (`0x200`) marca un **grupo global** | Party `vf11 0x21D8020` |
| **end-403** | `9F0010B0` | **u32 líder**: argumento de "crear grupo" (`vf40 0xA06F00`) si no existe | idem |
| end-399 | `00000000` | `Party+0x20`: FormationKey de creación (0 en miembros, `0xDC` en el líder) | idem |
| end-395 | `00×12` | `Party+0x70` (12 B) | idem |
| end-383 | `01` | `u8 Party+0x24` (el "count byte" del enunciado: no es un contador) | idem |
| end-382 | `08000000` | **u32 nº de miembros** | idem |
| **end-378** | 8 × u32 | **lista de miembros**: cada uno entra con `vf44 0xA06360` ("añadir miembro"). Incluye al propio actor; en la copia, su hueco tiene ya la clave de la copia | idem |
| end-346 | `DC000000` | u32 → `u16 Party+0x26` por la tabla `0x38AA20`. Es una **FormationKey**: 220, 218, 213, 214, 1001, 1002, 1010, 4, 7 y 1 están en `formationinfo` [captura, `ids/out/gd`] | idem |
| end-342 | `04000000` | **`Party+0x28`: índice del propio actor en la lista** (A3 es el 4.º) | idem |
| end-338 | `01` | `u8 Party+0x2C` | idem |
| end-337 | `00000000` | `u32 m` + m × u32 → vector `Party+0x30` (seguidores; solo en el líder) | `0x21179C0` (`.xtls 0xE3B1B70`) |
| **end-333** | `9F0010B0` | **`Party+0x40`: líder** (0 en el líder) | Party `vf11` |
| … | | TransformSync (las 2-3 Transform), Navigation (`vf11 0x21A2EF0`, 12 B), Input (nada) | |
| end-14 | `09 00000000 00 0000` | Hierarchy (`.xtls 0xC1707A0`): `u8 +0x18`, `u32 +0x1C`, u8, `u16 m`, m × u32 hijos de jerarquía (0 en todos los hostiles) | |
| **end-6** | `9F0010B0` | **`actor+0xAC`: padre (líder)** | unpack del actor |
| end-2 | `0000` | `u16 n` hijos (`actor+0xB0`) | idem |

La sección "`dc 00 00 00 | n | 01 …`" del enunciado es `FormationKey | índice propio | u8 +0x2C`.

### 1.3 Registro de un líder (`0xB0100099`, 7.858 B)

- **Party:**
  - nº de grupo `…03 | 0x201`; líder = la propia clave; `Party+0x20 = 0xDC`;
  - **6 miembros** (él incluido, end-406);
  - `FormationKey 0xDC`, índice 0, `u8 1`;
  - **`m = 5` seguidores (end-373)**, `Party+0x40 = 0`.
- **Cola:** `actor+0xAC = 0` y **`n = 5` hijos** (la "lista cerca de end-20").
- `Target+0x198` = la propia clave.

Es la variante `06 00 00 00` / `05 00 00 00` del enunciado [captura, `keyocc.py`, `tails.py`].

### 1.4 Lo que no lleva claves [verificado + captura]

| Componente | Slot | `vf11` | Conclusión |
|---|---|---|---|
| CommonTargetFormation (no hay clase Client) | `0x190` | `0x3600C0` (`mov al,1; ret`) | no serializa nada |
| Faction (Common y ClientSelf) | `0x188` | `0x3600C0` | no serializa nada; la facción va en la cabecera `+0x1C` |
| Ai | `0x58` | `0x206DBD0` | serializa mucho, pero `xref3.py` no encuentra claves de actores en él en ninguna carga hostil |

**Cobertura** [captura, `xref3.py` sobre 112627, 090931 y 090008]:
- en las cargas de tipo 3, toda clave de otro actor cargado aparece en una de estas posiciones:
  - Party (líder, miembros, seguidores, `+0x40`);
  - `Target+0x198`;
  - cola `+0xAC`/`+0xB0`;
  - o las claves del usuario y el jugador del anfitrión, que ya traduce `translate()`;
- **excepto en los jinetes de carro**: su padre es un tipo 7, y la clave del carro sale además en
  `+220/232`, `~+5.550…5.860` (`FFFFFFFF | clave`) y en el `Path.parent` de la última Transform.

---

## 2. Qué hace el cliente con esas listas (pregunta 2)

### 2.1 Party: la copia se une al grupo real [verificado]

`vf11` de Party (`0x21D8020`):
1. Lee el nº de grupo. Si `(nº >> 41) & 1`, es global y lo busca en el gestor
   `[[0x146D69190]+0x70]` con `vf51 0xA062E0` → `.xtls 0x9D0BDA0` → `0xA08980`.
2. **Si existe, se engancha a él**, guardando el puntero en `Party+0x50` y la bandera en `+0x58`.
   Si no existe, lo crea (`vf40`) con `{líder, nº, 100, +0x20, +0x24}`.
3. Para cada miembro llama a `vf44` → `0x21D74D0(grupo, clave)`:
   - si la clave ya está en el array `grupo+0x58` (entradas de 16 B, contador `+0x60`), no hace
     nada;
   - si no, **la añade al final**.
   - Sin grupo enganchado devuelve `eErrNoInvalidPartyKey` y el unpack entero falla.

Con la carga de la copia, el nº de grupo es el del campamento real, que existe en el mismo mundo.
Así que:
- **la copia se mete como miembro extra en el grupo real**. No sustituye al original: el array no se
  indexa por `+0x28`;
- en dos PC, el nº de grupo (serie por mundo, `0x201_0000xxxx`) cae en un grupo **cualquiera** del
  invitado [inferido].
- Al retirar la copia, `vf6` (`.xtls 0x9D164B0`) la saca con `0xA08FD0(nº, clave)`. No se ha visto
  que cierre el grupo real, pero no está analizado a fondo [inferido].

### 2.2 Padre e hijos: el login va por grupos [verificado]

Bucle de actores pendientes, `0x8B7B50`:

| Paso | Dirección | Qué hace |
|---|---|---|
| a | `0x8B7D78` | Si `actor+0xAC ≠ 0`: busca el padre (`0x464290`). **Si existe, salta el actor** (no lo encola). Si no existe, lo destruye (motivo `0xE`, `eErrNoParentsNotExist`) |
| b | `0x8B7DDA` | Si no tiene padre, `+5E & 8` y `+0xCA`: lo encola para el login |
| c | `0x8B8283` | Primera vez (`+0xCB == 0`): `0x4FC930` → `0x4FD530` (`.xtls 0x8C14600`) monta el **grupo de login** = `[actor] + hijos de actor+0xB0, recursivo` (`0x4FD660`). Cada hijo se busca por clave; si falta uno → `eErrNoInvalidActorKey` y el actor sigue esperando. Comprueba que los recursos de todos estén listos y lanza `AsyncLoginCompleteThreadProcess` |
| d | hilo trabajador, `0x4FCF10` | Para **cada actor del grupo, sin mirar su estado**: `0x4FA530` (recalcula `+0xC8`) y `vf18` (`+0x90`) de **todos sus componentes** ("postProcessForCreate") |
| e | `0x8B8328` | Después (`+0xCC`): `0x4FD2E0`. Para cada actor del grupo: si `+5E ≠ 8` → **`eErrNoUserNotLogin`** y fin. Si no: `vf15` (`0x1436A50`, estado 8 → `0x10`) y `vf40` (`0x17E94E0`, **loginCharacter**: `vf19` de cada componente, `+0x95 = 1`) |
| f | `0x8B8334` | Si `0x4FD2E0` falla: el primer actor del grupo (el líder) recibe `+0x98 = error`, `+0x9C = 0xB`, `vf16` (estado `0x20`) y `vf34`; es decir, se destruye |

### 2.3 Qué pasa con cada tipo de copia

| Original | Copia | Reales |
|---|---|---|
| **Solo** (`+0xAC = 0`, sin hijos) | login normal (`+5E 0x10`, `+CF 1`) | nada |
| **Miembro** (`+0xAC` = líder real) | se salta en (a): **estado 8 para siempre**, sin `vf18` ni login, probablemente sin dibujar [inferido]. Si el líder real desaparece, se destruye por (a) | **se une a su grupo de Party real** (§2.1) |
| **Líder** (`+0xB0` = hijos reales) | grupo `[copia, hijo real 1…N]`; en (d) `vf18` corre sobre los reales; en (e) la copia hace login y luego falla en el primer hijo real (`+5E = 0x10`); en (f) se destruye | **`vf18` del CharacterControl re-ejecutado: invisibles** (§2.4) |
| **Líder cuyo hijo ya no existe** | (c) falla con `eErrNoInvalidActorKey` en cada frame: estado 8 para siempre, sin error en el log | nada |

### 2.4 El camino que oculta a un miembro real

1. `CC vf18` = `0x85E440` → `.xtls 0x984F340`. Su mensaje de error es
   "onCharacterResourceLoadCompleteThread를 실패하였습니다" [verificado]. Con el recurso cargado:
   - `Status->0x1795490(0x2000, set)` si `[…+0x15E]`;
   - `(0x200, set)` si `[…+0xB0]+0x320`;
   - `(0x4000, set)` si `Status+0x273`;
   - y **`(2, set)` siempre**, sin fundido (`xmm3 = 0`).
2. `0x1795490` hace OR del bit en `Status+0x390` (o `+0x394` con el 5.º argumento). Lo propaga a
   los hijos de Hierarchy (`[comps+0x1B8]+0x20`). Si cambia el valor combinado, llama a
   `Status vf80` [verificado].
3. `Status vf80` = `0x6293A0` → `.xtls 0x8FF5920` [verificado]:
   - si el valor cruza `0x80`, cambia la navegación (`0x21BE880`). Es el mismo umbral que usa el
     minimapa para ocultar (`mapa-minimapa.md`);
   - para los tipos de actor 1 y 3 a 9 (tabla `0x629684`), **`alfa = (bit 8) ? c : (banderas > 0 ?
     0.0 : 1.0)`**, escrito en `[[CC+0xE8]]+0x100` y aplicado con `0xA5A330(…, fundido)`. El
     `1.0` está en `0x145D730B4`.
4. Los bits 2 y `0x4000` solo se limpian en `0x4703C0` [verificado]. Su único llamador útil es
   `.xtls 0x89C1230`, al que se llega desde `CC vf24` (`0x85E790`, que en `0x85E9F1` llama a
   `0x470430` → `0x89C1230`, y antes a `0x2329D70`) [verificado]. Que `vf24` corra al final del
   login normal es [inferido].
   - Ni el líder copiado (destruido) ni sus hijos (ya en `0x10`, nunca vuelven a pasar por ahí)
     lo ejecutan.
   - **Los hijos reales se quedan con `Status+0x390 ≥ 2`, es decir, con alfa 0** [inferido: falta
     medir `+0x390` en ellos, §6.8].
5. Que se vean "a veces" encaja con lo anterior: solo pasa cuando el modo automático copia a un
   líder (9 de 46 copias en los dos logs) [captura].

**Riesgo extra:**
- `vf18` de componentes (CC, Status, EquipSlot, Detect, Docking, Party) corre en un hilo
  trabajador sobre actores vivos.
- `0x4FCF10` los bloquea uno a uno (`0x393650`), pero esas funciones no se diseñaron para un actor
  ya activo.
- **Es una vía plausible de cierre**, que se reportaría a Pearl Abyss [inferido].

### 2.5 En dos PC es peor [inferido]

- Las claves `0xB01xxxxx` son contadores por mundo: en el invitado, las claves de los miembros
  reales del anfitrión nombran a actores **cualquiera** del invitado.
- Una copia de líder ocultaría NPC o animales al azar del invitado.
- Una copia de miembro esperaría a un "padre" ajeno o se destruiría.
- Y el nº de grupo se uniría a un grupo ajeno.

---

## 3. "loginCharacter 실패" (pregunta 3)

- **Origen:** `exe+0x4FD4D0` es la dirección de retorno de la llamada al registrador `0x35D8D0` en
  `0x4FD2E0` (`0x4FD4CB`). El texto `0x5586970` es el de este caso [verificado].
- Hay dos ramas con el mismo texto [verificado]:
  1. `0x4FD3AA`: el actor del grupo tiene `+5E ≠ 8` → código = `[0x146CDB92C]` =
     **`eErrNoUserNotLogin`** (registrado en `0x1428659`);
  2. `0x4FD3E0`: `vf40` (loginCharacter) devuelve un error.
- **El código del log es `2920878040 = 0xAE190FD8 = hashlittle("eErrNoUserNotLogin", 0xC5EDE)`**,
  la misma función que usa el registro de errores `0x1427790` → `0x1364780` [verificado]. Así que
  es la rama 1: **un actor del grupo no estaba en estado 8**, es decir, un hijo real ya logueado.
- **Los "valores" del log son marcos viejos de `0x4FD2E0`** [verificado + captura]:

  | Valor | Qué es |
  |---|---|
  | `0x13ECBC0` | puntero al búfer en pila del vector de grupo |
  | **`0x400_0000000N`** | **capacidad 0x400, N actores en el grupo** |
  | `0x14557D660` | vtable de un attacher |

- **Correlación con las capturas** [captura, `leaders.py`]: N = 1 + hijos del líder copiado en
  **9/9** errores:

  | Sesión | Líder copiado (hijos) | N del log |
  |---|---|---|
  | 090008 | `0x11F` (4) | 5 |
  | 090008 | `0x1B6` (2) | 3 |
  | 090008 | `0x373` (4) | 5 |
  | 090008 | `0x44E` (4) | 5 |
  | 090008 | `0x47F` (1) | 2 |
  | 090008 | `0x446` (7) | 8 |
  | 090008 | `0x4B5` (5) | 6 |
  | 090931 | `0x099` (5) | 6 |
  | 090931 | `0x3F9` (2) | 3 |

- **No tiene que ver con el identificador de aparición:**
  - en 090931, `strip_spawn_identity` ya estaba activo ("camp slot removed" / "schedule id
    removed") y los fallos siguen;
  - el único éxito (`0xB0100081`, "schedule id removed") era un **solo** [captura].
- El comentario de `strip_spawn_identity` ("fought its original for the slot") **no describe la
  causa real**.
- El otro error, "Actor unpackInformation에 실패했습니다" (código `0xA5C2E5FE`, logs 3c/auto),
  **no** es `eErrNoInvalidPartyKey` (`0x37C687D9`). No se ha investigado.

---

## 4. Verificación con las capturas (pregunta 4)

### 4.1 Papel de cada original frente al resultado de su copia [captura, `leaders.py`]

| Sesión | Copias | Solo → login OK | Miembro → estado 8, `+CF 0` | Líder → destruida + error | Líder con hijo ya retirado |
|---|---|---|---|---|---|
| 090931 | 11 | 1/1 | 8/8 | 2/2 | — |
| 090008 | 35 | 3/3 | 24/24 | 7/7 | 1 (`0x48D`, hijo `0x48E` retirado a 259 s: estado 8, sin error) |

### 4.2 Mensajes de los originales alrededor de la copia [captura, `origmsgs.py`, ±25 s]

- Los hijos del líder copiado `0x99` (`0x9A…0x9E`) siguen mandando `StartActionReq` (8-33),
  `MoveActorReq` (12-19) y `ChangeAIStateReq` (10-34) tras la inyección. Están vivos y con IA.
  `0x9C`/`0x9D` incluso disparan proyectiles.
- **Ningún mensaje de grupo sobre los originales:** ni `ClearAllyGroupDataAck`, `JoinPartyAck`,
  `DeletePartyMemberAck`, `ClosePartyAck`, `OverriedOwnershipStateTypeyAck` ni
  `ChangeAiControlAck`.
  - `AggroRelationListReq`, `ChangeAggroRelationValidTickReq` y `ReportFromViewerReq` salen al
    mismo ritmo que antes.
  - `LeavePartySelfReq`/`DeletePartyMemberAck` (47) y `ClearAllyGroupDataAck` (581) son de
    **otros** actores (animales y bandidos que salen de su grupo o mueren). **Ninguno lleva una
    clave `0xB01F…`**: el servidor nunca supo de las copias.
- Las claves de los originales sí aparecen en `ActorLoadDataAck` S>C posteriores: son las
  **cargas de las otras copias** (listas de miembros). Por ejemplo, `0x9A` va en las de
  `0xB01F0004` y `0xB01F0006`.
- **Formato visto de paso** [captura]:
  - `DeletePartyMemberAck` = `u32 0 | u64 nº de grupo | u32 clave | u8 1`;
  - `LeavePartySelfReq` = `u32 clave`;
  - `ClosePartyAck` = `u32 líder | u64 nº`.
- **Error en `orig.py`:** toma `dr == 0` como C>S. En el sniffer, `1` = C>S y `2` = S>C
  (`troc_sniffer.cpp:32`), así que todos los mensajes salen como "S>C".

### 4.3 Lo que no se puede ver en la captura

`Status+0x390` de los hijos no viaja por red. **La invisibilidad hay que medirla en memoria**
(§6.8).

---

## 5. Claves ajenas en los mensajes relevados (pregunta 5)

Medido en los mensajes de los NPC hostiles (`rec+0x1C` ∈ {`0xF4250`, `0xF42C0`}) de 112627 y
090931, contra el conjunto de claves cargadas [captura, `srcmsgs.py`]. Lo inyectado a las copias
en 090931 está en `relayed.py`. Las coincidencias sueltas en floats (1-4 casos en offsets
cambiantes) se han descartado.

| Mensaje (relevado como) | Offset (Req → Ack) | Qué es | Valores vistos | Hoy `translate()` |
|---|---|---|---|---|
| StartActionReq → StartActionAck | `+0x00` → `+0x00` | actor | propio | ✓ |
| | **`+0x116` u64 → `+0x117`** | `Path.parent` | **tipo 7 (asiento o carro) 165**; jugador del anfitrión 4 (remate) | solo el jugador. **El gimmick pasa crudo** |
| | **`+0x16C` → `+0x16D`** | objetivo | jugador 2.027; **caballo del anfitrión (`0xB0100003`, T5) 177**; **otro bandido 42** | solo el jugador |
| RotateActorReq → RotateActorAck | `+0x00`; `+0x54` u64 padre → `+0x54` | actor, padre | solo propio (103 mensajes) | ✓ |
| VaryHPAck | `+0x00`; **`+0x54`** | actor, fuente | jugador 81 | ✓ (otras fuentes pasarían crudas) |
| NotifyHandleDeadAck | `+0x00`; **`+0x04`** | muerto, asesino | jugador 16 | ✓ (idem) |
| PostProcessAttackResult_AttackTargetAck (199/211 B) | `+0x00` objetivo; **`+0x04`** atacante; **`+0xBC`** (tras `u8 01` en `+0xBB`) atacante; **`+0xCE`** | | `+0x04`: jugador 88, **caballo 16**, propio 2; `+0xBC`: jugador 8, **caballo 16**; `+0xCE`: jugador 5 | caballo crudo |
| ApplyActionRestrictionAck (77 B) | `+0x00`; **`+0x49`** | fuente | jugador 50, **caballo 5** | caballo crudo |
| CurrentTargetChangeReq (solo para `g_aimed`) | `+0x04` | objetivo | jugador 92, caballo 23, otro bandido 19 | — |

**Inyectado en 090931** [captura, `relayed.py`]: 2.340 MoveActorAck, 496 StartActionAck, 24
RotateActorAck, 7 AttackTargetAck, 5 VaryHPAck, 2 ApplyActionRestrictionAck y 1 NotifyHandleDeadAck.
- Las claves reales están solo en StartActionAck `+0x16D`, VaryHPAck `+0x54`, NotifyHandleDead
  `+0x04`, AttackTarget `+0x04` y ApplyActionRestriction `+0x49`.
- En esa sesión todas iban ya traducidas a `0xA010F001`.
- **No se coló ninguna clave real porque no hubo caballo ni asientos**, no porque el código lo
  impida.

---

## 6. Recomendación (pregunta 6)

### 6.1 Principio

> **Una copia no debe nombrar a ningún actor que no sea ella misma o una de nuestras claves de red
> (marioneta, caballo copia y copias de enemigos).**

- El co-op no gana nada con que la copia pertenezca a un grupo: va en modo 2 y su comportamiento lo
  dicta la red.
- Agrupar copias entre sí (padre = copia del líder) tampoco compensa:
  - el login exige que **todo** el grupo esté presente y en estado 8 a la vez (§2.2);
  - las copias llegan y se retiran por separado;
  - un hijo que falta bloquea al líder para siempre (caso `0x48D`);
  - un hijo sin padre se destruye.

### 6.2 La carga, paso a paso (anfitrión, en `send_enemy`, después de `translate()`)

Se edita de atrás hacia delante, para no mover offsets ya calculados.

1. **Cola del actor** (final del registro):
   - recorrer `n = 0…64` y tomar el menor con `u16[len-2-4n] == n` y n claves con forma de clave
     (`v >> 20` ∈ {`0xB01`, `0xA01`}). `+0xAC` está en `len-6-4n`;
   - comprobar Hierarchy justo antes: `u8 | u32 | u8 | u16 m | m × u32`, con `u16 m` en
     `+0xAC - 2 - 4m`;
   - **si `m > 0`, rechazar** (ningún hostil lo tiene);
   - **si el padre es un actor de tipo 7 o 5 (carro o montura), rechazar la copia** (§6.4);
   - si no, **reemplazar `[+0xAC … final]` por `00 00 00 00 | 00 00`** (padre 0, 0 hijos). El
     registro se acorta `4n` bytes.
2. **Party.** El ancla es la clave de la copia dentro de la lista de miembros:

   ```text
   para cada aparición p de copy_key (desde +8):
     para idx = 0…63:
       s = p - 4·idx;   n = u32[s-4];          exigir idx < n ≤ 100
       e = s + 4·n;     exigir u32[e+4] == idx  (Party+0x28)
       m = u32[e+9];    exigir m ≤ 100
       start = s - 33
       exigir (u64[start] >> 32) == 0x201 (siempre en las capturas; solo "bit 9" deja pasar
              falsos anclajes como 0xF42), u32[start+8] con forma de clave,
              byte[s-5] ≤ 1, byte[e+8] ≤ 1
       bloque = [start, e + 13 + 4·m + 4)
   ```

   **Reemplazar el bloque por la plantilla de 50 B de un NPC solo:**

   ```text
   00 00 00 00 00 00 00 00   u64 nº de grupo = 0  (bit 41 a 0: no se busca ni se crea grupo)
   00 00 00 00               u32 líder (arg. de creación)
   00 00 00 00               u32 Party+0x20
   00 00 00 00 00 00 00 00 00 00 00 00   Party+0x70
   01                        u8  Party+0x24
   00 00 00 00               u32 nº de miembros = 0  (sin grupo, un miembro daría eErrNoInvalidPartyKey)
   00 00 00 00               u32 FormationKey
   FF FF FF FF               u32 índice propio = -1
   00                        u8  Party+0x2C
   00 00 00 00               u32 nº de seguidores = 0
   00 00 00 00               u32 Party+0x40 líder = 0
   ```

   - Es **byte a byte** el bloque de los NPC solos: está **una sola vez** en las 764 cargas solas
     de tipo 3 y en las 727 de tipo 6 [captura].
   - El registro se acorta `4·(n+m)` bytes.
   - Si no se encuentra el bloque y el registro sí tenía padre o hijos, no pasa nada (hay grupos de
     solo padre e hijos, sin Party). Si la carga ya trae la plantilla, no hay nada que hacer.
3. **`Target+0x198`.** Buscar el patrón exacto
   `u32 | FF FF FF FF | 00 00 00 00 | FF×8 | u32 | CLAVE`, con CLAVE = copia o padre original:
   - aparece **exactamente una vez** en 1.104/1.104 cargas de tipo 3 y 1.391/1.391 de tipo 6
     [captura];
   - **si CLAVE es el padre, escribir la clave de la copia**: los solos y los líderes llevan su
     propia clave.
4. **Longitud:** `u16 [+0] = tamaño - 2`. El `n` del `EnemySpawn` ya sale de `load.size()`.
   `puppet_spawn_enemy` busca la posición desde el final y no se ve afectado. Los offsets de T0
   (`strip_spawn_identity`) están antes de Party.
5. **Validación final** (obligatoria):
   - recorrer el registro byte a byte (las claves no van alineadas) y buscar cualquier `u32` que
     sea la clave de un actor conocido del anfitrión;
   - no cuentan la copia ni nuestras claves de red (`0x9010F001`, `0xA010F001`, `0xB010F00x`,
     `0xB01F…`), que es en lo que `translate()` ya convirtió al usuario y al jugador del anfitrión;
   - **si queda alguna, no mandar la copia** y registrar offset y clave;
   - hace falta un conjunto vivo de claves: todo registro de **cualquier** tipo de
     `ActorLoadDataAck` (también los de varios registros), menos los `RemoveActorAck`. Hoy
     `g_loads` solo guarda tipos 3 y 6 de un registro.
   - **No vale "cualquier `0xB01xxxxx`"**: los floats pequeños negativos (`0xB01…` ≈ −5·10⁻¹⁰)
     dan falsos positivos en las Transform (vistos en `relayed.py`).

**Prototipo** (`build/research/invis/sanitize.py` + `test_san.py`) sobre las 347 cargas hostiles
de tipo 3 de las tres capturas [captura]:
- **230 de grupo y 102 solas quedan limpias**;
- 15 tienen residuo, todas jinetes (padre de tipo 7);
- tras la edición, la cola se vuelve a leer como `padre 0, 0 hijos` y la longitud cuadra.

### 6.3 Por qué es robusto

- Ningún paso usa offsets fijos desde el final:
  - la cola se lee desde el final con su propio contador;
  - Party se ancla en la clave propia más tres comprobaciones internas (contador, índice y nº de
    grupo `0x201`);
  - `Target+0x198` usa un patrón de 24 B con 1 aparición por carga.
- Si algo no se encuentra, la validación final decide. **Ante la duda, no se copia.**

### 6.4 Lo que no se debe copiar todavía

- **Jinetes** (padre de tipo 7 o 5). La clave del carro va en al menos cuatro campos más:
  `+220/232` (cerca de la cabecera), `FFFFFFFF | clave` hacia `+5.550…5.860`, el `Path.parent` de
  la última Transform, y otros. Copiarlos exige copiar también el carro (pieza 3f).
- **Tipo 4** (personajes con nombre) y tipo 6 (animales en manada): mismo esquema de Party y cola
  [captura]. La receta vale igual, pero hoy no se copian.

### 6.5 Mensajes relevados: mapeo campo a campo (anfitrión, antes de enviar)

| Clase | Valor en el mundo del anfitrión | Valor para el receptor |
|---|---|---|
| **E** | enemigo con copia enviada (`g_sent`) | su clave de copia |
| **H** | jugador del anfitrión | `0xA010F001` |
| **U** | usuario del anfitrión | `0x9010F001` |
| **C** | caballo del anfitrión (`g_horse_key` de `local_capture`; `SpawnInfo.horse_key`) | `0xB010F002` si la copia del caballo existe; si no, X |
| **G** | marioneta del invitado en el anfitrión (`0xA010F001`/`0xB010F00x`) | jugador real del invitado. En el eco de un PC coincide con H |
| **X** | cualquier otra clave | "ninguno": `FF×8` en padres u64, `0` en u32 |

| Mensaje | Campos a mapear |
|---|---|
| StartActionReq (antes de convertirlo en Ack) | `+0x00` E; **`+0x116` u64 padre**: H/C/E y, si no, `FF×8`; **`+0x16C` objetivo**: E/H/C/G y, si no, `0`. Revisar también `+0xAA` (hoy nunca es una clave en los hostiles) |
| RotateActorReq | `+0x00` E; `+0x54` u64 padre como arriba |
| VaryHPAck | `+0x00` E; `+0x54` fuente: E/H/C/G y, si no, 0 |
| NotifyHandleDeadAck | `+0x00` E; `+0x04` asesino: igual |
| AttackTargetAck | `+0x00` E; `+0x04`, `+0xBC`, `+0xCE`: igual. Si el atacante no se puede mapear, se puede **descartar** el mensaje, que solo es visual |
| ApplyActionRestrictionAck | `+0x00` E; `+0x49`: igual |

- Después del mapeo, **la misma validación por conjunto de claves de §6.2.5** sobre el mensaje
  completo. Si queda alguna clave conocida no mapeada, se descarta el mensaje.
- `replace_all` sobre el mensaje entero sigue valiendo para E, H y U. **Lo que falta son los campos
  con valores que no son E, H ni U**: hoy pasan crudos.

### 6.6 Defensa en el receptor (`enemy_on_spawn` / `puppet_spawn_enemy`)

El receptor conoce su propio mundo. Antes de inyectar:
- leer la cola (§6.2.1) y exigir `padre == 0` y `0 hijos`; si no, no inyectar;
- exigir que Party no tenga el bit 41 en el nº de grupo (o que la plantilla sola esté presente);
- recorrer el registro y, para cada `u32` con `v >> 20 ∈ {0xB01, 0xA01, 0x901}` que no sea una
  clave de red nuestra, comprobar `game::find_actor(v)`. **Si existe un actor local con esa clave,
  rechazar.**

Es una lectura protegida por clave, sin escrituras. En dos PC es la única barrera contra versiones
del anfitrión sin la limpieza.

### 6.7 Sobre `strip_spawn_identity`

- Las capturas no relacionan el identificador de aparición con ninguno de los dos fallos.
- **Solo o con él, el resultado depende únicamente del papel en el grupo** [captura].
- Se puede dejar, pero hay que corregir el comentario y no contar con él como arreglo.

### 6.8 Prueba en un PC (eco con `cdmp_peer`)

**Preparación:**
- copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`;
- **recargar la zona antes de cada prueba** (viaje rápido o pantalla de carga): los originales ya
  ocultos no se arreglan solos (§2.4) y contaminarían el resultado.

**A. Confirmar la causa (antes del arreglo; solo lecturas protegidas, como `read_seen`):**
1. Ctrl+F6 sobre **el líder** de un campamento. Su clave sale en `+0xB0` de la cola; en el log, es
   el original de la copia que queda "NOT in the game's actor list".
2. Para cada hijo real (claves de la cola del líder en `g_loads`), registrar antes y 1-2 s después:
   - `u16 +5E`;
   - `Status = [[a+0x68]+0x20]`: `u32 +0x390` y `+0x394`.
3. **Esperado:**
   - `+5E` sigue en `0x10`;
   - `+0x390` pasa de `0` a un valor con el bit `0x2` (a veces `0x200`/`0x2000`/`0x4000`);
   - los hijos dejan de verse, aunque siguen atacando;
   - un error loginCharacter con `0x400_0000000N`, N = 1 + hijos.
4. Opcional, para la Party:
   - componente `P = [[a+0x68]+0x108]`;
   - si `byte[P+0x58]`, grupo `G = [P+0x50]`: `u32 G+0x60` (miembros), `u32 G+0xA0` (líder);
   - con una copia de **miembro**, `G+0x60` sube en 1 y la clave de la copia aparece en el array
     `G+0x58` (16 B por entrada, clave en `+0`).

**B. Tras el arreglo:**

| Comprobación | Esperado |
|---|---|
| Log del anfitrión por copia | Una línea "sanitized": padre y hijos quitados, `party nº/n/m`, `Target+198`, **residuo 0** |
| Ctrl+F7 en un campamento entero (líder + miembros) | Todas las copias `exists … +5E 0x10 login +CF 1` en el informe de 2 s |
| Errores del juego | **Ningún** "loginCharacter" ni "NOT in the game's actor list" (salvo retiradas) |
| Hijos y originales | `Status+0x390/0x394` sin cambios (0 en reposo); visibles en la captura de pantalla (`CopyFromScreen`, recorte del campamento) |
| Party de la copia | `byte[P+0x58] == 0` (sin grupo enganchado); `G+0x60` del grupo real sin cambios |
| Jinetes de carro | Rechazados con un log claro; ninguna copia inyectada |
| Mensajes relevados | Contadores de campos mapeados a "ninguno" por tipo de mensaje; ningún mensaje descartado por la validación en una pelea normal |

---

## 7. Hallazgos laterales

1. **La marioneta y el caballo copia también llevan Party** [captura, 090931]:
   - la carga del jugador marioneta `0xA010F001` trae el nº de grupo **`0x201_00000000`** (el grupo
     del jugador, serie 0) con líder `0xA010F001`;
   - la del caballo copia trae el mismo nº y la lista `[0xA0100001, 0xB0100002, 0xB0100003]`, con
     el **compañero `0xB0100002` sin traducir**;
   - en el receptor, la serie 0 es **su propio grupo de jugador**, así que la marioneta se une a él
     y el caballo añade una clave que en dos PC es el compañero del invitado [inferido: efecto en
     la interfaz de grupo no medido];
   - la misma plantilla de §6.2 (o traducir esa lista y usar otro nº) lo resolvería.
2. **Si una copia quedara dentro de un grupo real** y el cliente decidiera sacarla, mandaría
   `LeavePartySelfReq` con su clave `0xB01F…` al servidor. No ha pasado en las capturas. Es otra
   razón para la plantilla sola.
3. Con la puesta a cero de `strip_spawn_identity`, las copias de miembro sin padre válido pasarán a
   la cola normal. **Es el comportamiento buscado.**

---

## 8. Direcciones (build 2976)

| RVA | Qué es |
|---|---|
| `0x17EA310` → `.xtls 0xC867CC0` | Unpack del actor: `+0x90`, `vf11` de cada componente, **`+0xAC` padre**, **`u16 n` + hijos → `+0xB0`** |
| `0x21D8020` | `ClientPartyActorComponent vf11` (vtable `0x55C01F0`, slot `0x108`) |
| `0xA062E0` → `.xtls 0x9D0BDA0` → `0xA08980` | Party `vf51`: buscar grupo global (bit 41 del nº) en `[[0x146D69190]+0x70]` |
| `0xA06F00` / `0xA06360` → `0x21D74D0` | Party `vf40` crear grupo / `vf44` añadir miembro (array `+0x58`, contador `+0x60`, líder `+0xA0`) |
| `0x21179C0` → `.xtls 0xE3B1B70` | Lector `u32 m + m × u32` (`Party+0x30`) |
| `0x21D8510` → `.xtls 0xE5948C0` | Party `vf18`: fija el líder en Target (`0x221EBD0(…, 0xB)`) |
| `0x9D164B0` (`vf6`) | Party: salir del grupo (`0xA08FD0`) |
| `0x2213D30` → `.xtls 0xE674C90` | `ClientTargetActorComponent vf11` (`… +0x118 u64, +0x168, +0x198`) |
| `0x15EC8E0` → `.xtls 0xC1707A0` | Hierarchy `vf11` (`u8, u32, u8, u16 m, m × u32`) |
| `0x3600C0` | `vf11` vacío (TargetFormation, Faction, Input, Minimap…) |
| `0x8B7B50` | Bucle de actores pendientes: padre (`0x8B7D78`), cola de login (`0x8B7DDA`), post-proceso (`0x8B8283`), cierre (`0x8B8328`), destrucción (`0x8B8334`) |
| `0x4FC930` | Lanza `AsyncLoginCompleteThreadProcess` (vtable `0x55860E0`) para el grupo |
| `0x4FD530` → `.xtls 0x8C14600`; `0x4FD660` | Grupo de login = actor + hijos `+0xB0` (recursivo); hijo ausente → `eErrNoInvalidActorKey` |
| `0x4FCF10` | Post-proceso de creación (hilo trabajador): `0x4FA530` + `vf18` de cada componente, **sin mirar el estado** |
| `0x4FD2E0` | Cierre del login del grupo: `+5E == 8` o `eErrNoUserNotLogin`; `vf15` + `vf40` (loginCharacter `0x17E94E0`). Log en `0x4FD4CB` (retorno `0x4FD4D0`) |
| `0x85E440` → `.xtls 0x984F340` | `CC vf18`: `Status` bits `2`/`0x200`/`0x2000`/`0x4000` |
| `0x1795490` | Pone o quita un bit en `Status+0x390` (`+0x394`) y llama a `Status vf80` |
| `0x6293A0` → `.xtls 0x8FF5920` | `Status vf80`: alfa `0.0` si hay banderas (tipos 1 y 3-9); navegación al cruzar `0x80` |
| `0x85E790` → `0x470430` → `.xtls 0x89C1230` → `0x4703C0` | `CC vf24`: único sitio que limpia los bits `2` y `0x4000` |
| Globales de error (código = `hashlittle(nombre, 0xC5EDE)`) | `0x146CDB92C eErrNoUserNotLogin` (`0xAE190FD8`), `0x146CDB9E4 eErrNoInvalidActorKey`, `0x146CD8EF0 eErrNoParentsNotExist`, `0x146CF6D88 eErrNoInvalidPartyKey`, `0x146CDB934 eErrNoUserAlreadyLogouting` |

## 9. Scripts (`build/research/invis/`)

Hay que ejecutarlos desde un directorio de trabajo: dejan cachés `.pkl` allí. `survey.py <bin…>`
crea las cachés de cargas que usan los demás.

| Script | Qué hace |
|---|---|
| `foreign.py`, `orig.py` | Los del enunciado. **`orig.py` etiqueta mal la dirección (1 = C>S)** |
| `cap.py` | Lector de la captura 090931 y nombres de mensajes |
| `recparse.py` | Localizadores: `tail()`, `party()`, `target198()` |
| `survey.py`, `survey_lib.py`, `allkeys.py` | Cachés de cargas y claves de todos los registros, y estadísticas de grupos |
| `keyocc.py`, `tails.py` | Apariciones de la clave propia y volcado del final de una carga |
| `leaders.py <bin> <log>` | Papel de cada original (solo, miembro o líder) frente al resultado de su copia y al N del error |
| `origmsgs.py <claves>` | Mensajes por original ±25 s alrededor de la copia |
| `xref3.py <bin…>` | Clasifica toda clave de otro actor dentro de las cargas (Party, Target, cola, desconocido) |
| `srcmsgs.py <bin…>` / `relayed.py` | Claves en los mensajes de los NPC hostiles / en lo inyectado a las copias |
| `sanitize.py`, `test_san.py <bin…>` | Prototipo de la limpieza de §6.2 y prueba sobre las capturas |
| `ptypes.py` | Tipos de padre e hijos en las cargas hostiles |
