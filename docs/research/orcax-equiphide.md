# Investigación: localizar al jugador local (Orcax + CrimsonDesertTools/EquipHide)

Fecha: 2026-10-03 · Build analizada: `CrimsonDesert.exe` FileVersion **1.0.0.2976**,
PE TimeDateStamp `0x6AB28F00` (2026-09-22 14:21 UTC), ImageBase `0x140000000`,
SizeOfImage `0x173AB000`, 384.521.624 bytes, con Denuvo.

**Todas las RVAs de este documento valen solo para esa build.** Se obtuvieron leyendo el
exe en disco (sin lanzar el juego) con `tools/scripts/aob_scan_file.py`. Nada de lo que
aparece aquí se ha probado aún en memoria viva.

## 1. Fuentes

| Fuente | Qué es | Licencia | Versión del juego que apunta |
|---|---|---|---|
| [Orcax-1399/CrimsonDesert-player-status-modifier](https://github.com/Orcax-1399/CrimsonDesert-player-status-modifier) (clonado en `reference/orcax`, último commit `b56b956`, 2026-04-16) | ASI con mid-hooks de safetyhook: captura del puntero del jugador, descubrimiento de entradas de stats, multiplicadores de daño/stamina/espíritu, control de altura, montura/dragón. Notas de RE en `position_research.md`, `researches/*.md`. | **Sin archivo LICENSE** (todos los derechos reservados por defecto). Solo usamos hechos (bytes, offsets, registros), nunca código. | **v1.01.00** (marzo-abril 2026). Commits: `5ceeac2` 1.00.02 (2026-03-22), `1689676` "adapt for 1.00.03 (1.00.04)" (03-25), `539e470`/`1b68b1f` "v1.01.00" (03-29/30). Las RVAs que citan sus docs (`+36ADB8C`, `+192F5E0`…) son de esa época. |
| [tkhquang/CrimsonDesertTools](https://github.com/tkhquang/CrimsonDesertTools) (clonado en `reference/cdtools`, último commit `a529ca1`, 2026-09-21): `CrimsonDesertCore` (anclas compartidas + resolvedor del personaje controlado), `CrimsonDesertEquipHide`, `CrimsonDesertLiveTransmog` | Mods de equipamiento/transmog. Lo que nos interesa: las "escaleras" de AOB del singleton WorldSystem, del global `ClientActorManager`, del vtable de `ChildActor` y de `PlayerStatic`, y las cadenas de offsets de `cdcore/controlled_char.hpp`. | **MIT** (repo y cada mod). | **2.03.00 = FileVersion 1.0.0.2944** (lo dice `docs/research/README.md` y `live-transmog-source-of-truth.md`; commit "update for game version 2.03.00", 2026-09-18). Nuestra build (1.0.0.2976, compilada el 22-09) es **posterior**: probablemente un hotfix 2.03.x. Número público exacto: sin confirmar. |
| [tkhquang/DetourModKit](https://github.com/tkhquang/DetourModKit) (clonado en `reference/dmk`) | Librería genérica de hooking/scan (MIT). **No contiene firmas de Crimson Desert**; solo enlaza a los mods. Aporta la sintaxis de patrones que usa CDTools: `|` marcador de resultado, `[n-m]` hueco acotado, `4?` comodín de nibble. | MIT | — |
| `reference/CrimsonDesertCoop/include/cdcoop/core/game_structures.h` | Recopilación sin probar (ver `docs/ANALISIS-CrimsonDesertCoop.md`). | Sin licencia | Dice "v1.00.02 / v1.01.03 (marzo 2026)". |

Mapa de versiones que se puede deducir: v1.05.01 = FileVersion 1.0.0.1070 (doc
`combat-state-research.md` de CDTools); 2.03.00 = 1.0.0.2944; la nuestra = 1.0.0.2976.

## 2. Cómo es el exe en disco (importante para leer los resultados)

Denuvo **renombra las secciones**. Los nombres no significan lo que parecen; fíate de los
permisos:

| Nombre en disco | RVA | Permisos | Qué es en realidad |
|---|---|---|---|
| `.rsrc` | `0x00001000` (82 MB) | R-X | El `.text` original del juego. Casi todas las firmas caen aquí. |
| `.xdata` | `0x051EB000` | R-- | El `.rdata` original (vtables, RTTI COL). |
| `.rdata` | `0x06893000` | RW- | El `.data` original (singletons, TypeDescriptors RTTI). En disco solo hay ceros/inicializadores: los punteros se rellenan en tiempo de ejecución. |
| `.xtls` | `0x075C9000` (250 MB) | RWX | Sección de Denuvo: stubs ofuscados **y funciones del juego trasplantadas/mutadas** (código con estilo `mov r/m, r` tipo clang, rodeado de stubs `xchg [rsp],r9 / sub [rsp],imm / ret`). |
| `.text` | `0x16F10000` | R-- | No es el código del juego (sección de Denuvo). |

Consecuencias:

- El código del juego **sí es legible en disco** (no está cifrado): las firmas que no
  aparecen es porque el código cambió, no por Denuvo.
- Algunas funciones viven en `.xtls`. Se pueden leer, pero **parchear (hookear) código de
  `.xtls` es un riesgo extra**: es código que Denuvo controla y podría estar sujeto a
  comprobaciones de integridad. Orcax ya hookeaba una función de `.xtls` en 1.01 (spirit
  delta, `+C6A72AB`), así que no es imposible, pero para nosotros: **solo lectura o, como
  mucho, mid-hook probado con cuidado**. Lo marco como `[DENUVO]`.

## 3. Cadenas de punteros

### 3.1 Raíz estática "WorldSystem" (EquipHide/LiveTransmog) — RECOMENDADA

```text
[WorldSystem slot]            rva 0x06D69190  (resuelto por 3 firmas independientes, coinciden)
  +0x00 -> WorldSystem
    +0x30 -> pa::ClientActorManager      (vtable RTTI rva 0x055B62C8)
      +0x58 -> pa::ClientUserActor       (vtable RTTI rva 0x055B59F0)
        +0xD8 -> CCOIA controlado = pa::ClientChildOnlyInGameActor (vtable rva 0x05585C40)
```

- `+0x30` y `+0x58` **confirmados estáticamente en nuestra build**: el juego repite cientos
  de veces el idiom `mov rax,[rip→0x06D69190]; mov rcx,[rax+30h]; mov rax,[rbx+0A0h];
  cmp [rcx+58h],rax; jne` ("¿este actor es el UserActor?"), p.ej. en rva `0x00361B87`,
  `0x0123B25B`. Hay 3.104 lecturas del slot en el código; 1.071 van seguidas de
  `mov rcx,[rcx+30h]`.
- `+0xD8` (UserActor → CCOIA controlado) viene de CDTools 2.03.00; **no lo he podido
  confirmar con un idiom estático**. Verificar en vivo: el qword en `[CCOIA]` debe ser
  `0x140000000 + 0x05585C40`.
- **`game_structures.h` de CrimsonDesertCoop dice `ActorManager +0x28 → UserActor`: está
  mal para esta build** (es `+0x58`; CDTools dice que es el offset que más cambia entre
  parches y por eso lo "autocura" vía RTTI).
- `UserActor+0xD8` es null en menú/pantalla de carga; es la señal de "mundo listo" que usa
  EquipHide. En un cambio de personaje (radial) solo rota `+0xD8`; en carga de partida se
  realoca el UserActor entero.

### 3.2 Raíz estática "ClientActorManagerGlobal" (CDCore)

```text
[ClientActorManager slot]     rva 0x06DDA800  (2 firmas, coinciden; son las 2 únicas referencias)
  +0x00 -> pa::ClientActorManager   (el mismo objeto que WorldSystem+0x30)
    +0x58 -> pa::ClientUserActor
      +0x08 -> sub-manager de CCOIAs
        +0x30 -> CCOIA de Kliff (siempre presente)
        +0x38 -> CCOIA controlado
      +0x78 -> vector<ActorComponent*>; [2] = ClientChildContainerActorComponent (vtable 0x055B5FB0)
        +0x18 -> lista de 100 actores (stride 16: ptr + flag)
    descriptor de array de CCOIAs (nominal mgr+0x128): +0 ptr (stride 8), +8 count u32, +0xC cap u32
```

### 3.3 Dentro del CCOIA (ambas fuentes, unificadas)

```text
CCOIA (ClientChildOnlyInGameActor)
  +0x60 dword identidad: byte +0x60 id de sesión; byte +0x63 = 0xA0 Kliff, 0xB0 resto
  +0x68 -> tabla de componentes  (= lo que Orcax llama "actor")
     +0x20 -> componente de estado  (= "status marker" de Orcax; candidato ClientStatusActorComponent, vtable 0x0558D868 — sin verificar)
     +0x38 -> ClientEquipSlotActorComponent (vtable 0x055BCC38)   (+0x40 en carga en frío)
     +0x40 -> ClientCharacterControlActorComponent (vtable 0x055B5460) (+0x48 en carga en frío)
     +0x48 -> ClientVehicleActorComponent (vtable 0x055B6108)
  Todo componente: +0x08 = back-pointer al CCOIA dueño.
```

Unión de las dos fuentes (hipótesis fuerte, **verificar en vivo**): Orcax resuelve
`marker+0x08 → owner; owner+0x68 → "actor"; actor+0x20 == marker`. Eso encaja exactamente
con "componente +0x08 = back-pointer al CCOIA" y "CCOIA+0x68 = tabla de componentes" de
CDTools. O sea: **"actor" de Orcax = `[CCOIA+0x68]`**, y "marker" = `[[CCOIA+0x68]+0x20]`.
Los slots de la tabla se mueven entre carga en frío y estado estable: identificar cada
componente comparando su vtable con las RVAs RTTI de §6, no por posición fija.

Identidad del personaje (CDTools): `CCOIA+0x68 → [componente CharacterControl por RTTI] →
+0x40 → +0x38 = std::string` con la ruta del appearance (`cd_phm_macduff` = Kliff,
`cd_phw_damian` = Damiane, `cd_phm_oongka` = Oongka). Estable entre sesiones.

### 3.4 Stats (HP/stamina/espíritu) — Orcax

```text
marker ([tabla+0x20])
  +0x08 -> owner (CCOIA)
  +0x18 -> root (contenedor de stats); [root+0x00] == marker (validación)
     +0x58 -> array de StatEntry, stride 0x90
StatEntry: +0x00 int32 tipo (0 = HP, 17 = stamina, 18 = espíritu)
           +0x08 int64 actual    +0x18 int64 máximo   (valores x1000 según bbfox/CrimsonDesertCoop)
           +0x20/+0x38/+0x48/+0x50: escritos por la rutina de commit (ver stat-write)
```

- El stride **0x90 está confirmado en nuestra build** (`lea rax,[rdx+rdx*8]; shl rax,4;
  add rax,[rsi+58h]` en rva `0x017ADD2F`).
- El índice de cada stat **no es fijo**: el getter hace `slot = [[desc+0x58] + statInfo[+0x14]*4]`
  (`-1` = no existe) y luego `root+0x58 + slot*0x90`. Orcax asume HP en el slot 0 y
  stamina = HP+0x480 (slot 8), espíritu = HP+0x510 (slot 9) en 1.01; bbfox v29 (vía
  CrimsonDesertCoop) dice HP+0x510 / HP+0x5A0. **Recomendación: recorrer el array y buscar
  por el int32 de tipo**, validando `0 <= actual <= máximo`.

### 3.5 Posición — Orcax (`position_research.md`, v1.01)

```text
player_core      = [[actor+0x40]+0x08]     ("actor" de Orcax = [CCOIA+0x68]; +0x40 = CharacterControl en estado estable)
cache (NO autoritativa) = [player_core+0xA0] + 0x280/0x284/0x288   (se reescribe; editar no mueve al personaje)
posición autoritativa   = [[position_owner+0x248] + 0x90]  float x, y(arriba), z, w
position_owner+0x148    -> pa::engineScript::CharacterPhysicsMoveControllerScript (vtable 0x056FC840)
```

El puente `player_core → position_owner` **no está resuelto** en Orcax. El camino práctico
es el mid-hook de §4 (`r13` apunta directamente al vector del personaje controlado).

**Rotación: ninguna de las fuentes la tiene verificada.** `game_structures.h` pone un
cuaternión en `+0xA0` tras la posición, pero no viene de Orcax ni de CDTools: no fiarse.

### 3.6 Cadena "servidor" (LiveTransmog `PlayerStatic`)

```text
[PlayerStatic slot]  rva 0x06D69680
  +0x18 -> pa::NwVirtualAsyncSession (vtable 0x057260C0)
    +0xA0 -> pa::ServerUserActor (vtable 0x05B257A8)
      +0xD0 -> pa::ServerChildOnlyInGameActor controlado (vtable 0x05B1E2B0)
```

Interesante para co-op: el juego tiene internamente una separación cliente/servidor
(`Server*`/`Client*` actores y componentes, `ServerTransformSyncActorComponent`,
`ClientTransformSyncActorComponent`, `NwVirtualAsyncSession`). Vale la pena investigarlo.

## 4. Tabla de firmas y resultado del escaneo en disco

Leyenda de "Tipo": **entrada** = inicio de función; **mid** = mitad de función (solo
`safetyhook::create_mid`); **dato** = la firma solo sirve para resolver un puntero
RIP-relativo (no se hookea). "Resultado" = dónde está el punto útil relativo al inicio del
match. Disco: ÚNICO = 1 match; NO = 0 matches; MULTI = >1.

### 4.1 CrimsonDesertTools (2.03.00, MIT)

| Firma (fuente) | Tipo | Resultado / registros | Disco (1.0.0.2976) |
|---|---|---|---|
| `WorldSystem_P1_InlinedGetterThroughVCall` (`cdcore/anchors.hpp`) | dato | `mov rax,[rip+d32]` en +0 (disp +3, len 7) | ÚNICO rva `0x00366CE2` → slot `0x06D69190` (huecos reales: `77 04`, `EB 08`) |
| `WorldSystem_P2_StructField` | dato | marcador en el `mov rax,[rip]` tras el `jne` | ÚNICO rva `0x00366CD9`, instr. `0x00366CE2` → `0x06D69190` |
| `WorldSystem_P3_ContainerWalkToCall` | dato | +0 | ÚNICO rva `0x003619E6` → `0x06D69190` |
| `ClientActorManagerGlobal_P1_PublishStore` | dato | `mov [rip+d32], r13` en +0 | ÚNICO rva `0x008B9D8A` → slot `0x06DDA800` |
| `ClientActorManagerGlobal_P2_LeaCallBodyDisp8` | dato | `lea rdx,[rip+d32]` tras el `jne` | ÚNICO rva `0x008B9EC5`, instr. `0x008B9ECF` → `0x06DDA800` |
| `ChildActorVtbl_P1_AllocCtor` (`EquipHide/aob_resolver.hpp`) | dato | `lea rax,[rip+d32]` en +0xD | ÚNICO rva `0x0898D922` `[DENUVO .xtls]` → vtable `0x05585C40` |
| `ChildActorVtbl_P2_CtorStore` | dato | +0x9 | ÚNICO `0x0898D926` → `0x05585C40` |
| `ChildActorVtbl_P3_WiderCtorStore` | dato | +0x15 | ÚNICO `0x0898D91A` → `0x05585C40` |
| `ChildActorVtbl_RTTI` (`.?AVClientChildOnlyInGameActor@pa@@`) | dato | TypeDescriptor `0x06AA9F38`, COL `0x05DB3B80` | vtable `0x05585C40` — **coincide** con P1-P3 |
| `PlayerStatic_P1_WriterSite` (`LiveTransmog/aob_resolver.hpp`) | dato | `mov [rip+d32], r15` en +0 | ÚNICO rva `0x02771E8E` → slot `0x06D69680` |
| `PlayerStatic_P2_WriterSiteTlsTail` | dato | +0 | ÚNICO (mismo sitio) |
| `PlayerStatic_P3_SafeLaunchReader` | dato | `mov rcx,[rip+d32]` en +0xB | ÚNICO rva `0x00A0C817` → `0x06D69680` |

Nota sobre el `ChildActor vtable`: el constructor que carga el vtable está en `.xtls`
(Denuvo), pero no hace falta tocarlo: el vtable se obtiene igual por RTTI (§6) y es dato
en `.xdata`. Sirve para validar que un puntero es un CCOIA (`[p] == base+0x05585C40`).

Firmas antiguas de EquipHide copiadas en CrimsonDesertCoop (`WORLD_SYSTEM_P1/P2/P3`):
P1 y P3 (`48 83 EC 28 48 8B 0D …` / `48 8B 0D … 48 8B 49 ?? E8 … 84 C0 0F 94 C0`) → **NO**
en esta build. P2 (`80 B8 ?? ?? ?? ?? 00 75 ?? 48 8B 05 …`) → ÚNICO, mismo sitio que el P2 actual.

### 4.2 Orcax player-status-modifier (v1.01.00, sin licencia)

| Firma (`src/scanner.cpp`) | Tipo | Registros en el punto de hook (según Orcax) | Disco (1.0.0.2976) |
|---|---|---|---|
| player-pointer primary `49 8B 7D 18 49 8B 44 24 40 48 8B 40 68 48 8B 70 20`, hook +0x11 | mid | `rax` = "actor" (`[CCOIA+0x68]`), `rsi` = marker (`[rax+0x20]`) | **NO** |
| player-pointer fallback `49 8B 44 24 40 48 8B 40 68 48 8B 70 20`, hook +0xD | mid | igual | **NO** |
| player-pointer 1.01 primer intento (`539e470`, 2 patrones) | mid | — | **NO** |
| player-pointer 1.00.03/1.00.04 (`1689676`) `0F B6 ?? ?? 8B ?? ?? 48 8B 58 40 …`, hook +7 | mid (función en `0x028FA3B0`) | `rax` = `[rcx+0x68]` = "actor"; luego `rbx=[rax+0x40]`, `[rbx+8]` = player_core | **ÚNICO** rva `0x028FA3BA`, hook `0x028FA3C1`. Volvió a existir. **No sabemos si solo se llama para el jugador**: verificar en vivo. |
| player-pointer 1.00.02 (`5ceeac2`) `48 8B 42 68 48 8D 56 58 …`, hook +0 | mid | `rdx` = actor | **NO** |
| mount-pointer `48 8B C7 49 8B 7D 08 80 BF 94 00 00 00 00 0F 85 …`, hook +0x14 | mid | (no se usa en Orcax: código muerto) | **NO** (el trozo `80 BF 94 00 00 00 00 0F 85` sí es único, rva `0x0123B273`, pero el resto cambió) |
| position primary `49 3B F7 0F 8C ?? ?? ?? ?? 0F 28 C6 … 48 63 83 00 01 00 00`, hook +0x16 | **mid** | instr. `movups [r13],xmm0`: `r13` = float4 posición del personaje controlado (x, y=altura, z, w); `xmm0` = posición nueva (= vieja + `xmm6`); `rbx` = objeto de movimiento (lista de listeners en `+0xF8`, count `+0x100`) | **ÚNICO** rva `0x04282EA6`, hook `0x04282EBC` |
| position fallback `0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00`, hook +0xD | mid | igual | ÚNICO, mismo hook `0x04282EBC` |
| position contexto largo (`position_research.md`), hook +5 | mid | igual | ÚNICO, mismo hook |
| copiador de caché de posición (`+192F5E0` en 1.01) | entrada | `rdx` origen, `r8=[rcx+8]` destino | **NO** |
| damage primary/fallback `48 89 5C 24 ?? … 49 8B C1 49 8B E8 0F B7 DA 48 8B F1 4D 85 C9`, hook +0 | **entrada** (rva `0x017AE080`) | `rcx` = root de stats del objetivo, `dx` = id de stat, `r8` = ?, `r9` = delta (int64, negativo = daño), `[rsp+0x28]` = contexto de origen (`+0x68` → "actor" atacante); `[rsp]` = dirección de retorno | **ÚNICO** (ambos) |
| damage-slot antiguo `49 8B 77 38 44 8B 24 88 48 8D 4C 24 ?? 4A 8B 1C E3` (hook +4/+0x11, pre 04-06) | mid | semántica antigua (`r15`, `r12d`, `rbx`) no re-verificada | ÚNICO rva `0x017ADB03` (informativo) |
| stats primary `48 8D ?? ?? 48 C1 E0 04 48 03 46 58 ?? 8B ?? 24`, hook +0xC | **mid** (epílogo del getter) | `rax` = &StatEntry, `rsi` = root (`[rsi]` == marker) | **ÚNICO** rva `0x017ADD2F`, hook `0x017ADD3B` |
| stats fallback `48 C1 E0 04 48 03 46 58`, hook +8 | mid | igual | **MULTI(4)** — no usar |
| stat-write primary/fallback (`48 2B 47 18 … 48 FF 47 48 48 89 5F 08 …`) | mid | `rdi` = StatEntry, `rbx` = valor nuevo | **NO**. La función existe pero ahora vive en `.xtls`, mutada por Denuvo (ver 4.3) |
| stamina-ab00 `0F B7 D7 49 8B CE E8 ?? ?? ?? ?? 48 8B F0 …`, hook +0xB | mid | `rax` = StatEntry, `rbx` = delta, `r14` = root/dueño, `di` = id de stat | **ÚNICO** rva `0x017B3333`, hook `0x017B333E` |
| spirit-delta primary/fallback, hook +6 / +0xC | mid (en un `call`) | `rcx` = StatEntry, `r8` = delta | ÚNICO rva `0x0C7E087F`, hook `0x0C7E088B` `[DENUVO .xtls]` |

### 4.3 Firmas nuevas derivadas para esta build

| Nombre | Tipo | Notas | Disco |
|---|---|---|---|
| `new_xtls_statwrite` (`48 0F 4E C1 48 89 47 20 … 48 89 5F 08`), hook +0x21 | mid `[DENUVO]` | Sucesor del stat-write de Orcax: en `mov [rdi+8],rbx` → `rdi` = StatEntry, `rbx` = valor nuevo. Función con ofuscación de Denuvo (`sub esi,894C08BCh; lea r12,[rdi+rsi]`). **No hookear** salvo prueba muy controlada. | ÚNICO rva `0x0C7EFEF5`, hook `0x0C7EFF16` |
| `new_xtls_playerptr` (`48 8B 45 08 66 89 7C 24 20 … 48 8B 40 68 48 8B 70 20`), hook +0x24 | mid `[DENUVO]` | Muy probablemente el sucesor del player-pointer 1.01 de Orcax (mismo `call [rax+178h]` justo antes, mismo `mov rax,[rax+68h]; mov rsi,[rax+20h]`). En el hook: `rax` = "actor", `rsi` = marker. No sabemos si es exclusivo del jugador. | ÚNICO rva `0x0E364CDE`, hook `0x0E364D02` |

Todas las firmas del bloque `sigs` del final se re-escanearon con el script y dan **1 match**.

## 5. Recomendación para nuestro mod

1. **Localizar al jugador sin hooks**: resolver el slot WorldSystem con las 3 firmas de
   CDTools (exigir que coincidan), y en el hilo worker leer con `VirtualQuery`/SEH:
   `[[[[slot]+0x30]+0x58]+0xD8]`, validando cada eslabón por vtable RTTI (§6). Es
   exactamente lo que hacen EquipHide/LiveTransmog en 2.03.00, y +0x30/+0x58 están
   confirmados en nuestra build.
2. **Stats**: desde el CCOIA → `[CCOIA+0x68]` → `+0x20` (marker) → `+0x18` (root) → `+0x58`
   array; buscar entradas por tipo. Primero solo lectura.
3. **Posición**: el único sitio limpio es el mid-hook `0x04282EBC` (`r13`). Es mitad de
   función: **solo `create_mid`**, y lo primero es comprobar en vivo que `r13` solo cambia
   con el personaje controlado (Orcax lo afirma para 1.01). Alternativa sin hook: buscar en
   vivo el `position_owner` (`+0x248 → +0x90`) desde el CCOIA.
4. No tocar nada de `.xtls`.

## 6. Vtables RTTI (verificadas en disco: TypeDescriptor → COL → vtable)

Útiles para validar punteros en vivo (`*(uint64*)obj == 0x140000000 + rva`).

| Clase | vtable rva |
|---|---|
| `pa::ClientChildOnlyInGameActor` (CCOIA) | `0x05585C40` |
| `pa::ClientActorManager` | `0x055B62C8` |
| `pa::ClientUserActor` | `0x055B59F0` |
| `pa::ClientCharacterControlActorComponent` | `0x055B5460` |
| `pa::ClientEquipSlotActorComponent` | `0x055BCC38` |
| `pa::ClientVehicleActorComponent` | `0x055B6108` |
| `pa::ClientChildContainerActorComponent` | `0x055B5FB0` |
| `pa::ClientStatusActorComponent` | `0x0558D868` |
| `pa::ClientTransformSyncActorComponent` | `0x055BF248` |
| `pa::ClientGamePlayStateActorComponent` | `0x05574228` |
| `pa::engineScript::CharacterPhysicsMoveControllerScript` | `0x056FC840` |
| `pa::NwVirtualAsyncSession` | `0x057260C0` |
| `pa::ServerUserActor` | `0x05B257A8` |
| `pa::ServerChildOnlyInGameActor` | `0x05B1E2B0` |
| `pa::ServerStatusActorComponent` | `0x05B203F0` |
| `pa::ServerTransformSyncActorComponent` | `0x05B2CF60` |

Todas tienen COL con offset 0 (vtable primaria) y un único TypeDescriptor.

## 7. Preguntas abiertas (a resolver en vivo con el probe)

1. ¿`UserActor+0xD8` sigue siendo el CCOIA controlado en 1.0.0.2976? (vtable debe ser `0x05585C40`).
2. ¿`[CCOIA+0x68]+0x20` es el `ClientStatusActorComponent` (vtable `0x0558D868`) y `+0x18`
   el root de stats? ¿Qué slots tienen HP/stamina/espíritu ahora?
3. ¿El mid-hook de posición `0x04282EBC` solo dispara para el personaje controlado
   (y qué pasa al montar)? ¿Cómo se llega de `player_core` a `position_owner`?
4. Rotación/heading del jugador: sin fuente fiable. Hay que buscarla (candidatos:
   `ClientTransformSyncActorComponent`, el struct de `position_owner+0x248` cerca de `+0x90`).
5. ¿El sitio 1.00.03 (`0x028FA3C1`) se llama solo para el jugador?
6. Número de versión pública de nuestra build (¿2.03.01?).

## 8. Reproducir

```text
python tools/scripts/aob_scan_file.py "D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe" docs/research/orcax-equiphide.md
```

El script lee solo el bloque `sigs` de abajo. Acepta también la sintaxis extendida de
DetourModKit (`[n-m]`, `4?`, y `@` como marcador en lugar de `|`) para comprobar escaleras
originales; el probe en vivo solo entiende `AA` y `??`, así que el bloque usa solo eso.

Formato de la descripción: `@+0xNN` = desplazamiento del punto de hook desde el inicio del
match; `rip@+N` = la instrucción RIP-relativa empieza en +N (disp32 en +N+3, longitud 7,
destino = fin de instrucción + disp32); `fn` = el match es una entrada de función.
Los valores entre corchetes son las RVAs esperadas en 1.0.0.2976.

```sigs
cdt_worldsystem_p1 | 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00 48 8B 41 20 83 78 28 00 77 ?? 33 C9 EB ?? 48 8B 40 20 48 8B 48 10 48 8B 01 FF 50 40 | dato rip@+0 -> slot WorldSystem; WS+0x30 ActorMgr +0x58 UserActor +0xD8 CCOIA [match 0x00366CE2, slot 0x06D69190] | CrimsonDesertTools anchors.hpp (MIT, 2.03.00)
cdt_worldsystem_p2 | 80 B8 ?? ?? ?? ?? 00 75 ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00 | dato rip@+9 -> slot WorldSystem [match 0x00366CD9, slot 0x06D69190] | CrimsonDesertTools anchors.hpp (MIT, 2.03.00)
cdt_worldsystem_p3 | 48 8B 05 ?? ?? ?? ?? 48 8B 58 58 48 8D 54 24 ?? 48 8B CB | dato rip@+0 -> slot WorldSystem [match 0x003619E6, slot 0x06D69190] | CrimsonDesertTools anchors.hpp (MIT, 2.03.00)
cdt_actormgr_global_p1 | 4C 89 2D ?? ?? ?? ?? 49 8D 85 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 49 8D 85 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 0F B6 85 ?? ?? ?? ?? 88 05 | dato rip@+0 (store) -> slot ClientActorManager; mgr+0x58 UserActor [match 0x008B9D8A, slot 0x06DDA800] | CrimsonDesertTools anchors.hpp (MIT, 2.03.00)
cdt_actormgr_global_p2 | 48 83 C6 10 49 83 EE 01 75 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 90 C6 44 24 ?? 00 C5 F9 EF C0 | dato rip@+0xA (lea rdx) -> slot ClientActorManager [match 0x008B9EC5, slot 0x06DDA800] | CrimsonDesertTools anchors.hpp (MIT, 2.03.00)
cdt_childactor_vtbl_p1 | 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 | dato rip@+0xD (lea) -> vtable CCOIA; codigo en .xtls (Denuvo), solo leer [match 0x0898D922, vtbl 0x05585C40 = RTTI] | EquipHide aob_resolver.hpp (MIT, 2.03.00)
cdt_childactor_vtbl_p2 | 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C | dato rip@+9 -> vtable CCOIA [match 0x0898D926, vtbl 0x05585C40] | EquipHide aob_resolver.hpp (MIT, 2.03.00)
cdt_childactor_vtbl_p3 | 45 31 ED 48 85 F6 74 ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? | dato rip@+0x15 -> vtable CCOIA [match 0x0898D91A, vtbl 0x05585C40] | EquipHide aob_resolver.hpp (MIT, 2.03.00)
cdt_playerstatic_p1 | 4C 89 3D ?? ?? ?? ?? 48 8B 8F F8 00 00 00 41 BC ?? ?? 00 00 48 85 C9 74 | dato rip@+0 -> slot PlayerStatic; +0x18 NwSession +0xA0 ServerUserActor +0xD0 ServerCCOIA [match 0x02771E8E, slot 0x06D69680] | LiveTransmog aob_resolver.hpp (MIT, 2.03.00)
cdt_playerstatic_p3 | 45 33 C9 45 33 C0 48 8D 54 24 64 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? | dato rip@+0xB -> slot PlayerStatic [match 0x00A0C817, slot 0x06D69680] | LiveTransmog aob_resolver.hpp (MIT, 2.03.00)
orx_position_primary | 49 3B F7 0F 8C ?? ?? ?? ?? 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00 48 8B BB F8 00 00 00 48 63 83 00 01 00 00 | MID @+0x16 movups [r13],xmm0: r13=float4 pos (x,y=alto,z,w) del controlado, xmm0=pos nueva, xmm6=delta, rbx=obj movimiento [match 0x04282EA6, hook 0x04282EBC] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_position_fallback | 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00 | MID @+0xD (mismo hook) [match 0x04282EAF, hook 0x04282EBC] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_position_ctx | 41 0F 58 45 00 41 0F 11 45 00 48 8B BB F8 00 00 00 48 63 83 00 01 00 00 48 8D 34 C7 48 3B FE 74 20 | MID @+5 (mismo hook) [match 0x04282EB7, hook 0x04282EBC] | Orcax position_research.md (sin licencia, v1.01.00)
orx_stats_getter | 48 8D ?? ?? 48 C1 E0 04 48 03 46 58 ?? 8B ?? 24 | MID @+0xC: rax=&StatEntry (stride 0x90; +0 tipo, +8 actual, +0x18 max), rsi=root ([rsi]==marker) [match 0x017ADD2F, hook 0x017ADD3B] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_damage_fn | 48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 49 8B C1 49 8B E8 0F B7 DA 48 8B F1 4D 85 C9 | fn @+0: rcx=root stats objetivo, dx=id stat, r9=delta int64, [rsp+0x28]=ctx origen (+0x68 actor atacante) [match 0x017AE080] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_stamina_ab00 | 0F B7 D7 49 8B CE E8 ?? ?? ?? ?? 48 8B F0 48 85 DB 74 ?? 33 C0 66 89 44 24 20 38 46 53 | MID @+0xB: rax=&StatEntry, rbx=delta, r14=root/dueno, di=id stat [match 0x017B3333, hook 0x017B333E] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_spirit_delta | 49 89 D8 48 89 FA 48 89 C1 48 89 C6 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B 5C 24 30 48 8B 74 24 40 48 83 C4 20 5F C3 | MID @+0xC (call) en .xtls DENUVO: rcx=&StatEntry, r8=delta; no hookear [match 0x0C7E087F, hook 0x0C7E088B] | Orcax scanner.cpp (sin licencia, v1.01.00)
orx_playerptr_v10003 | 0F B6 ?? ?? 8B ?? ?? 48 8B 58 40 48 8B 43 08 ?? 8D ?? 08 33 ?? ?? 85 ?? ?? 0F 44 | MID @+7: rax=[rcx+0x68]="actor" (tabla comp. del CCOIA), [rax+0x20]=marker; fn en match-0xA; exclusividad jugador SIN verificar [match 0x028FA3BA, hook 0x028FA3C1] | Orcax git 1689676 (sin licencia, v1.00.03)
orx_damage_slot_old | 49 8B 77 38 44 8B 24 88 48 8D 4C 24 ?? 4A 8B 1C E3 | MID legado (hook +4/+0x11 antes de 04-06), semantica no re-verificada; informativo [match 0x017ADB03] | Orcax git 5ceeac2/52de030 (sin licencia, v1.00.02-v1.01.00)
new_xtls_statwrite | 48 0F 4E C1 48 89 47 20 48 8B 44 24 ?? 48 FF 47 48 48 89 47 38 0F B7 84 24 ?? ?? ?? ?? 66 89 47 50 48 89 5F 08 | MID @+0x21 mov [rdi+8],rbx: rdi=&StatEntry, rbx=valor nuevo; .xtls DENUVO ofuscado, no hookear [match 0x0C7EFEF5, hook 0x0C7EFF16] | derivado de Orcax stat-write (esta investigacion)
new_xtls_playerptr | 48 8B 45 08 66 89 7C 24 20 4C 89 7C 24 28 4C 89 7C 24 30 89 5C 24 38 44 88 7C 24 3C 48 8B 40 68 48 8B 70 20 | MID @+0x24: rax="actor" (tabla comp.), rsi=marker; probable sucesor del player-pointer 1.01; .xtls DENUVO [match 0x0E364CDE, hook 0x0E364D02] | derivado de Orcax player-pointer (esta investigacion)
```
