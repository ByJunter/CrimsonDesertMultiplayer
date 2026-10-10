# Cheat tables, versiones y punteros para nuestra build

Fecha: 2026-10-03 · Build objetivo: `CrimsonDesert.exe` 1.0.0.2976 (ver §1).
Investigación de solo lectura: no se lanzó el juego ni se escribió en su carpeta, y no se
ejecutó ningún binario descargado ni Lua de los .CT. Aquí solo se recogen **hechos**
(patrones, offsets, significado de registros y layouts) con su fuente y su licencia.
No se ha copiado código de ninguna tabla ni de ningún mod.

> Regla del proyecto: un offset o una firma vale solo para la build donde se verificó.
> En las tablas, **"disco"** indica que el patrón se escaneó en el exe sin ejecutarlo, y
> **"en vivo"** que alguien lo validó con el juego en marcha. Ninguno de estos datos se ha
> probado todavía en vivo **con nuestro mod**.

---

## 1. Qué versión pública es nuestro exe

| Dato | Valor | Fuente |
|---|---|---|
| FileVersion | 1.0.0.2976 | Recurso de versión del exe |
| PE TimeDateStamp | `0x6AB28F00` = **2026-09-22 14:21:52 UTC** | Cabecera PE (calculado) |
| SHA-256 | `57DA440D72F4DB974F25FEF047CF84C4DADD999A88CB2A3C5AF4C9BD67FDE1E7` | Calculado aquí |
| Steam app / build ID | 3321460 / **25477059** (depot 3321461, manifest 8201393788450828800) | `D:\SteamLibrary\steamapps\appmanifest_3321460.acf` |
| Nombre en Steam | "Crimson Desert Enhanced" | mismo `.acf` |
| **Parche público** | **2.03.02** (hotfix para todas las plataformas) | ver abajo |

Por qué es la 2.03.02:

1. **Coincidencia exacta de hash.** `ZappendusterFX/CrimsonDesertTelemetry`, en
   `definitions/build-25477059.json`, declara Steam build 25477059, `executableVersion`
   1.0.0.2976 y el mismo SHA-256 que nuestro exe, y su README dice que apunta a
   "Crimson Desert patch 2.03.02".
2. **Fechas oficiales** (crimsondesert.pearlabyss.com, avisos `_boardNo=132/133`):
   la 2.03.01 se publicó el 2026-09-21 a las 14:00 UTC y la 2.03.02 el 2026-09-23 a las
   04:45 UTC. Nuestro exe se compiló el 2026-09-22 a las 14:21 UTC, justo entre las dos.
3. **Otra fuente independiente:** `shin2344234/flight-freedom` (`mod/src/game/signatures.h`)
   da esta correspondencia: 2.03.00 = exe 1.0.0.2944, 2.03.01 = 1.0.0.2949 y
   2.03.02 = 1.0.0.2976. Una de sus firmas (`StartAction`, RVA `0x21C0990` "on 2.03.02")
   aparece **exactamente en ese RVA** en nuestro exe.

Ojo: la FileVersion no sigue la numeración pública. Las 1.0.0.xxxx son números de build
internos.

### Historial de parches, para ubicar el resto de fuentes

1.01.00 (03-28) · 1.02 (04-04) · 1.03 (04-11) · 1.04 (04-23) · 1.05 (05-02) · 1.06 (05-11) ·
1.07 (05-15) · 1.08 (05-22) · 1.09 (05-29) · 1.10 (06-05) · 1.11 (06-12) · 1.12 (06-19) ·
1.13 (07-03) · 1.14 (07-16) · 1.15 (07-24) · 1.16 (08-01) · 1.17 (08-07) · 1.18 (08-15) ·
**2.00.00 "Enhanced" (08-25)** · 2.00.01 (08-26) · 2.00.02 (08-31) · 2.01.00 (09-04) ·
2.02.00 (09-10/11) · 2.03.00 (09-17/18) · 2.03.01 (09-21) · **2.03.02 (09-22/23)**. No hay
parches de octubre a fecha 2026-10-03; el DLC "Charting the Unknown" se ha aplazado al
29 de octubre.
Fuentes: los avisos oficiales de Pearl Abyss, consolepulse.com (tracker de parches) y
patchbot.io. SteamDB respondió con 403 y no se consultó.

Builds de Steam vistas en CrimsonDesertTelemetry: 24994088 (exe 1.0.0.2658), 25050808
(1.0.0.2692), 25116796 (1.0.0.2760), 25246367 (1.0.0.2850, validada el 2026-09-12, así
que ≈2.02.00) y **25477059 (1.0.0.2976 = 2.03.02)**.

---

## 2. Fuentes: qué es cada una y para qué build

| Fuente | Qué es | Build objetivo | Licencia | ¿Sirve para la nuestra? |
|---|---|---|---|---|
| **ZappendusterFX/CrimsonDesertTelemetry** (commit `3284601`, 2026-09-29) | Telemetría de jugador, cámara y luces (lector externo + ASI). Tiene una definición JSON por build | **Exactamente la nuestra** (25477059, mismo SHA-256), "locally-validated" en vivo el 2026-09-23 | MIT | **Sí, es la mejor fuente** |
| **tkhquang/CrimsonDesertTools** (CrimsonDesertCore, `a529ca1`, 2026-09-20) | Núcleo compartido de EquipHide/LiveTransmog: escalera de AOB para WorldSystem y resolución del personaje controlado | Actualizado para 2.0x. Las 3 firmas son únicas en nuestro exe | MIT | Sí |
| **shin2344234/** (flight-freedom `e5401c9`, stamina-master, bounty-teleportation, master-looter) | Mods ASI con fuentes. Incluyen RVAs y layouts anotados por parche | **2.03.02 (1.0.0.2976)** | MIT | Sí (transform y tablas estáticas) |
| **bbfox0703/Mydev-Cheat-Engine-Tables** `Crimson Desert/CrimsonDesert.CT` | La tabla CE de bbfox (≈1,2 MB, CE 7.6+). La "v29" que cita la referencia es de mayo | Último commit **2026-08-09** (≈ parche 1.17, **antes de Enhanced**). La tabla no indica versión de juego. Los volcados de las scripts son de builds de marzo a agosto | **GPL-3.0** (solo extraemos hechos) | **Parcialmente**: 11/35 AOB únicos en disco, y los de punteros base **no** encajan |
| **blizz3010/CrimsonDesertCoop** `game_structures.h` + `docs/RESEARCH_2026-04-18.md` | Recopilatorio de firmas de terceros (Orcax, EquipHide, Send, CD Companion…) | 1.00.0x–1.01.03 (marzo–mayo) | Sin licencia | **Casi nada**: casi todas sus firmas fallan en disco (§6) |
| **marvelmaster/CrimsonDesertModdingResearch** (último push 2026-04-13) | Apuntes de render: VAs absolutos de culling, gamma, distancia de dibujado… y un BMS de PAZ | ≈1.0x (abril) | Sin licencia | **No**: solo render, y las VAs absolutas son de otra build |
| xsip/CrimsonDesert-Developer-Console-Enabler (2026-09-20) | Habilitador externo de la consola de desarrollo; incluye `Patterns.h` y `CPosition.h` | ≈2.0x (no lo dice) | Sin licencia | Firmas únicas en disco, pero la semántica de la posición no está clara |
| Nexus mods/64 (bbfox), mods/3209 ("Enhanced Inventory Editor… V7"), OpenCheatTables t=1836, FearLess | Tablas CE | — | — | **403 / bloqueado por bot**: no se forzó el acceso |
| vgtimes "Crimson Desert Enhanced — Table" (TalesOfKnighty1815, 2026-07-27) | Tabla de inventario | Enhanced (no dice la build) | ? | Sin interés (solo inventario) |

---

## 3. Notas sobre la imagen PE (Denuvo)

Denuvo cambia los nombres de las secciones, así que hay que fiarse de los flags y no del
nombre:

| Nombre en el PE | Flags | RVA | Qué es en realidad |
|---|---|---|---|
| `.rsrc` | R-X | `0x1000`–`0x51EB000` | **El código del juego (.text original), en claro en disco** |
| `.xdata` | R-- | `0x51EB000`–`0x6893000` | rdata: vtables, constantes y RTTI COL |
| `.rdata` | RW- | `0x6893000`–`0x70E7000` (raw solo `0x35C800`) | .data/.bss. Los singletons que nos interesan están aquí (BSS, a cero en disco) |
| `.xtls` | RWX | `0x75C9000`, ≈249 MB | Región de Denuvo. Algunos AOB de items de bbfox apuntan aquí; hay copias o transformaciones de código |
| `.text` | R-- | `0x16F10000` | Stub o loader |

Como el código está en claro, **los RVA de disco coinciden con los RVA en vivo**. Lo
confirman la firma `StartAction` de shin, que cae en `0x21C0990` igual que dice su fuente,
y la referencia de cámara de Telemetry, que cae en `0x2D14367` y resuelve al global
`0x6C8CF30`, ambos idénticos a los de su JSON. Con la base fija `0x140000000`,
VA = `0x140000000 + RVA`.

---

## 4. Jugador local

### 4.1 Raíz: WorldSystem → ActorManager → actor del jugador

**WorldSystem global: RVA `0x6D69190` (VA `0x146D69190`).** Lo resuelven a la misma
dirección, en disco, cuatro patrones independientes:

| Patrón | Fuente | RVA del match | Cómo se resuelve |
|---|---|---|---|
| `48 8B 0D ?? ?? ?? ?? 48 8B 49 30 E8 ?? ?? ?? ?? 84 C0 75 23 49 8B 46 08 48 8B 48 68 C6 44 24 20 00` | Telemetry (build-25477059) | `0xB88500` | `mov rcx,[rip+d32]`, disp en +3, len 7 |
| `48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00 48 8B 41 20 83 78 28 00 [2-6] 33 C9 [2-5] 48 8B 40 20 48 8B 48 10 48 8B 01 FF 50 40` | tkhquang P1 (usa huecos acotados, no vale para nuestro probe) | `0x366CE2` | +3 / 7 |
| `80 B8 ?? ?? ?? ?? 00 [2-6] | 48 8B 05 …48 8B 88 D8 00 00 00` | tkhquang P2 | `0x366CE2` (resultado) | +3 / 7 |
| `48 8B 05 ?? ?? ?? ?? 48 8B 58 58 48 8D 54 24 ?? 48 8B CB` | tkhquang P3 | `0x3619E6` | +3 / 7 |

Las firmas antiguas de EquipHide que recopiló blizz3010 (`48 83 EC 28 48 8B 0D … 0F 94 C0 48 83 C4 28 C3`)
**no aparecen** en nuestra build.

**Cadena del jugador para la build 25477059** (Telemetry `playerRoot`, validada en vivo
el 2026-09-23: yaw y posición cambiaban al caminar del campamento al cobertizo):

```
[WS global 0x146D69190]                   = WorldSystem*
  +0x30  -> pa::ClientActorManager           (RTTI .?AVClientActorManager@pa@@)
  +0x50  -> pa::ClientChildOnlyInGameActor   (actor del jugador, "CCOIA")
  +0x68  -> tabla de componentes (densa, crece al cargar)
  +0x40  -> pa::ClientCharacterControlActorComponent   (¡slot variable! ver nota)
  +0x140 -> "owner"
  +0x2C0 -> objeto de física (raíz de física del jugador)
            +0x60 basis X (float3), +0x70 basis Y (float3, "up"), +0x80 basis Z (float3)
            +0x90 posición float3 (+0x9C w), en espacio LOCAL de chunk (ver 4.3)
```

- Evolución del salto `owner -> física`: `+0x298` (24994088), `+0x2B8` (25116796–25246367)
  y **`+0x2C0` (la nuestra)**. Es el offset que más cambia entre parches.
- El `+0x2C0` lo confirma también, en disco, el sitio de worldOffset de bbfox, que en
  nuestra build carga `mov rcx,[rbx+0x2C0]` justo antes de escribir `[rcx+0x90]` (§4.3).
- **Nota de tkhquang:** en `CCOIA+0x68` la tabla de componentes es densa y el slot de
  `ClientCharacterControlActorComponent` se desplaza: está en `+0x48` al principio de la
  carga (≈5 componentes) y en `+0x40` en régimen normal (≈22). En ese régimen también
  aparecen `+0x38` = `ClientEquipSlotActorComponent` y `+0x48` =
  `ClientVehicleActorComponent`. Recomendación: buscar el componente por RTTI, como hace
  tkhquang, y no por offset fijo. Telemetry usa `+0x40` con comprobación de RTTI.
- Ruta alternativa de tkhquang (MIT): `ActorManager +0x58 -> pa::ClientUserActor`. Desde
  ahí, `+0xD8` = CCOIA controlado; o bien `+0x08` = sub-manager, en el que `+0x30` = CCOIA
  de Kliff (siempre presente) y `+0x38` = CCOIA controlado. La identidad del personaje se
  lee en `CCOIA+0x68 -> +0x40 -> +0x40 -> +0x38` (std::string con la ruta `.app_xml`:
  `cd_phm_macduff`=Kliff, `cd_phw_damian`=Damiane, `cd_phm_oongka`=Oongka).
- La antigua cadena `ActorManager +0x28 -> UserActor` de EquipHide (versión de
  blizz3010) está **obsoleta**.

### 4.2 Rotación

En el objeto de física hay una **matriz de base**, no un cuaternión (layout `basis-v1`):
X en `+0x60`, Y (arriba) en `+0x70` y Z en `+0x80`. Las tres son ortonormales y dextrógiras.
Según Telemetry, el forward del personaje es `-Z` y el heading es `atan2(fwd.x, fwd.z)`.
Telemetry probó en la 25116796 un layout SQT con cuaternión en `owner+0xE08` y lo
**retiró**, porque esa zona estaba a cero. El "cuaternión en +0xA0" de blizz3010 no está
verificado en ninguna build moderna.

### 4.3 Posición: tres fuentes y una trampa

1. **Global estático (coordenadas de mundo).** Patrón de Telemetry, validado en vivo
   en la 25477059:
   `C5 FB 11 3D ?? ?? ?? ?? 44 89 3D ?? ?? ?? ??` (`vmovsd [rip+XY],xmm7 ; mov [rip+Z],r15d`),
   único en `0x942318`.
   - X y Y en **RVA `0x6D77858`** (dos floats: X en +0, Y en +4).
   - Z en **RVA `0x6D77860`** (float).
   - Convención: **Y es la altura** (el plano horizontal es X/Z). Telemetry descarta como
     "cargando" dos centinelas: (0,0,0) y Y≈1000,15 cerca del origen.
   - Ojo: en la 25116796 el patrón anterior apuntaba a la **posición de la cámara** y no
     a la del jugador (eran idénticas bit a bit a `camera.position`, a ≈6,5 u). Telemetry
     lo corrigió a esta forma `C5 FB 11 3D / 44 89 3D`. El patrón antiguo de CD Companion
     (`C5 FB 11 05 … 8B 44 24 28 89 05`) **no aparece** en nuestra build.
2. **Física (local de chunk):** `phys+0x90` = posición mundo − `worldOffset`, con un
   origen de chunk de 1000 u. Telemetry ha medido diferencias de mundo − física como
   (−10000, −0,05, −3000).
   - **`worldOffset` (Vec4) en RVA `0x6D56500`**. Lo resuelve el AOB `worldOffset` de bbfox
     `C5 ?? ?? ?? ?? ?? ?? ?? C5 ?? 11 ?? 90 00 00 00 E8 ?? ?? ?? ?? C5` (pos 4, len 8), único
     en `0x36A4046`. En nuestra build el código es
     `mov rcx,[rbx+0x2C0] … vsubps xmm0,xmm0,[worldOffset] ; vmovups [rcx+0x90],xmm0`.
     Por tanto: **mundo = phys+0x90 + worldOffset**.
3. **Componente transform (shin2344234, 2.03.02, no validado por nosotros):**
   `actor+0x68` (bloque de componentes) `+0x1A0` → transform. En el transform: `+0xB4`
   posición local, `+0xC8` id de la entidad padre, `+0xEC` posición del padre,
   `+0x29C/+0x324/+0x3D0/+0x51C` copias "world". `+0x324` es la que está poblada cuando
   el jugador va montado y en la propia montura. `transform+0x408` = id de lo que el
   jugador tiene bajo los pies (en la 2.02.00 era `+0x3F4`: el transform creció).

Otros sitios de posición, todos de builds antiguas:

- `0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00` (Orcax "position" / CD Companion
  "phys delta"): **sigue siendo único** en `0x4282EAF`, con la forma SSE antigua. En
  `+0x0D` hay un `movups [r13],xmm0`, mitad de función. No sabemos si en esta build r13
  sigue apuntando a la posición autoritativa: hay que verificarlo en vivo.
- xsip `POSITION_PTR_INSTR` `C5 FA 7F 05 ?? ?? ?? ?? 48 8D 0D …` es único en `0x2E4461`
  y resuelve a `0x6D4E2C0`. Ese sitio **pone a cero** el global (inicialización). Según
  xsip, la instancia `CPosition` tiene `+0x8 → contenedor, +0x90 = posición local`.
  Semántica sin verificar.
- `0x36ADB8C movups [r13],xmm0` (RVA de blizz3010), las firmas de waypoint, de velocidad
  `[+0x1B0]` y de entity base de CD Companion: **NOT FOUND** en disco.

### 4.4 Montura

**No hay ningún puntero de montura público y verificado para 2.03.02.** Lo más cercano:

- **Pista 1 (tkhquang):** `ClientVehicleActorComponent` está en la tabla de componentes
  del CCOIA (`+0x68`, en `+0x48` en régimen normal). Es el candidato natural a "en qué
  vehículo voy". Su layout interno es desconocido.
- **Pista 2 (shin):** el `transform+0xC8` del jinete guarda el id de la entidad padre,
  que debería ser la montura.
- bbfox (1.x): montura por cadenas desde `Play_Base2` o `Horse_Base3`
  (`…+0x420/+0x168…+0x58`), marcadas como "may not work". Las AOB de base **no encajan**
  en nuestra build. Orcax `MOUNT_PTR_CAPTURE` y bbfox `INJECT_GET_HORSE_HP`: NOT FOUND.

---

## 5. HP, Stamina y Spirit (StatEntry)

### 5.1 Lo que dice la tabla de bbfox (agosto, ≈1.17)

Base = "stats array" (el qword en `componente+0x58`):

| Campo | Offset desde la base | Tipo |
|---|---|---|
| HP actual | `+0x008` | int64 (valor mostrado × 1000) |
| HP regen ("Recovery speed") | `+0x010` | int64 |
| HP máx | `+0x018` | int64 |
| Stamina actual / regen / máx | `+0x6C8` / `+0x6D0` / `+0x6D8` | int64 |
| Spirit actual / regen / máx | `+0x758` / `+0x760` / `+0x768` | int64 |

Evolución de la Stamina actual: `+0x488` (v1.01, Orcax/Tuuuup) → `+0x518` (bbfox v29, mayo)
→ `+0x6C8` (agosto). En nuestra build puede haber cambiado otra vez, porque la 2.03.00
modificó 130 de 268 tablas estáticas (shin, stamina-master). Cadenas de bbfox (1.x; la de
"current char" es la más fiable de las tres):

```
[Play_Base_Cur] (rbx en el hook)  +0x08 (pa::WindowsRWLock) -> +0x158 (pa::NwVirtualAsyncSession)
  -> +0x90 (pa::ServerUserActor) -> +0xD0 / +0xD8 / +0xE0 = ServerChildOnlyInGameActor (char 1/2/3)
  -> +0x68 -> +0x20 (pa::ServerStatusActorComponent) -> +0x18 -> +0x58 -> stats array
Cliente (marcada "not working"): [Player_Base] +0x50 +0xA0 +0xD0 +0x140 +0x18 +0x58
Servidor: [Play_Base2] +0x18 +0x90 +0x68 +0x220(+0x420,+0x620 = char 2,3) +0x18 +0x58
```

Los stats viven en el actor **Server**Child… (el juego tiene una separación
cliente/servidor incluso en un jugador). Las AOB de `Player_Base`, `Play_Base2`,
`Horse_Base3` y `INJECT_GET_PLAYER_DATA_AOB2` **no aparecen** en nuestro exe. La de
`INJECT_GET_HP_1_3` tampoco, ni siquiera trozos de 11 bytes (la función se reescribió o
se movió).

Hipótesis (sin verificar): según Tuuuup, `childactor+0xA0 = server actor`. Si el
`Player_Base` de bbfox fuera el ActorManager, su cadena cliente
(`+0x50 → +0xA0 → +0xD0 → +0x140 → +0x18 → +0x58`) empezaría igual que la de Telemetry
(`AM+0x50` = CCOIA). Habría que comprobarlo en vivo desde el WorldSystem.

### 5.2 La función de lookup de stats en nuestra build (derivada aquí, en disco)

El sitio "HP step 2" de bbfox (`?? 8B ?? 08 ?? 0F B7 ?? ?? 8B ?? E8 ?? ?? ?? ?? ?? 85`) sigue
siendo **único** en `0x17AC2B9`. En nuestra build: `movzx edx,bx ; mov rcx,r15 ; call
0x17ADCC0 ; mov rsi,[rax+8] ; movzx edx,r14w ; mov rcx,r15 ; call 0x17ADCC0`. bbfox
capturaba `rax` (= entrada de HP, con `rax+8` = HP actual). El registro cambió de r13 a
r15, y por eso su patrón largo ya no encaja.

La función llamada, **`GetStatEntry` en RVA `0x17ADCC0` (entrada de función)**, se
decodificó así a mano:

```
rcx = componente de estado ("this"), dx = id del stat (u16, StaticInfoWrapper de statusinfo)
  ... resuelve la definición de statusinfo; su [def+0x14] = índice de fila (coincide con
      kOff_Status_Index=0x14 de stamina-master)
  slot  = dword [[tabla+0x58] + índice*4]        ; -1 => devuelve NULL
  entry = [this+0x58] + slot*0x30               ; lea rax,[rdx+rdx*2] ; shl rax,4 ; add rax,[rsi+58]
devuelve rax = StatEntry*
```

Así que **cada StatEntry ocupa 0x30 bytes**:

| Offset | Tipo | Significado | Confianza |
|---|---|---|---|
| `+0x00` | int32? | id o tipo del stat (Tuuuup/Orcax: 0=HP, 17=Stamina, 18=Spirit en 1.0x) | baja en 2.03.02 |
| `+0x08` | int64 | valor actual × 1000 | alta (bbfox + código) |
| `+0x10` | int64 | velocidad de regeneración | media (bbfox) |
| `+0x18` | int64 | máximo × 1000 | alta |
| `+0x20..+0x2F` | ? | desconocido | — |

Los "offsets" de bbfox son slot×0x30 + 8: Stamina `0x6C0`/0x30 = slot 36 y Spirit
`0x750`/0x30 = slot 39 (en 1.0x eran los slots 24 y 27). **Para nuestra build,** en vez de
fijar el slot, capturamos `rcx` y `dx` en la entrada de `0x17ADCC0`, o `rax` en el sitio
`0x17AC2B9+0x0B`, o validamos HP con `cur ≤ max` y `max > 0`. Claves de statusinfo
(shin, 2.03.02): HP = 1000000, Stamina = 1000026 (hash `0xC08A2354`). La API usa el índice
de fila u16, **no** la clave.

---

## 6. Cámara (para world-to-screen)

Fuente: CrimsonDesertTelemetry, layout `renderer-camera-direct-v1`. Lo validaron en vivo
en nuestra build exacta; también se ha probado con DLSS desactivado (lectura directa, sin
Streamline).

```
[camera global RVA 0x6C8CF30 / VA 0x146C8CF30]  -> objeto cámara del renderer
    [cam+0x00] == vtable RVA 0x5D20718 (VA 0x145D20718)   ; comprobación de tipo
    cam+0xC8 / +0xD0 : posición de cámara (XY como qword, Z dword) — es lo que lee la instrucción guía
    cam+0x2C8 (712)  : contador de frame (si no avanza en 1 s => no fiable)
    [cam+0x428]      -> bloque "SceneConstants" (0xB00 = 2816 bytes)
```

Referencia de código (guía): `48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00`,
única en `0x2D14367` (`mov rax,[cam_global] ; vmovsd xmm6,[rax+0xC8] ; mov ebx,[rax+0xD0]`).
Hay dos huellas de función de la vtable, únicas, que coinciden con los slots 1 y 2 de la
vtable leída en disco: slot 1 = `0x38EF0C0` y slot 2 = `0x3B4BBB0`.

Layout del bloque SceneConstants (medido en la 25116796 y reutilizado en la nuestra; no
es un contrato entre builds):

| Offset | Tipo | Contenido |
|---|---|---|
| `+0x20` | u32 | número de frame (autoritativo) |
| `+0x30` / `+0x34` | float | ancho / alto de pantalla (enteros exactos) |
| `+0x38` / `+0x3C` | float | 1/ancho, 1/alto |
| `+0x80` | float3 | **posición de cámara (mundo)** |
| `+0x90` | float3 | forward |
| `+0x3E0` | float4x4 | **matriz de vista (`_view`, mundo→cámara)**, column-major HLSL. right = (`+0x3E0,+0x3F0,+0x400`), up = (`+0x3E4,+0x3F4,+0x404`), fwd = (`+0x3E8,+0x3F8,+0x408`), traslación en `+0x410/+0x414/+0x418` = −(pos·right, pos·up, pos·fwd) |
| `+0x420` | float4x4 | `_viewRelative` (misma rotación, traslación 0) |
| `+0x4E0` | float4x4 | **proyección** (perspectiva simétrica): `P[0][0]` en `+0x4E0`, `P[1][1]` en `+0x4F4` y `+0x50C` = 1 |
| `+0x860` | float | near (se ha visto 0,2) |
| `+0x864` | float | campo "far" en bruto. **No es** la distancia far; vale 0 y Streamline recibe FLT_MAX |
| `+0xAC0` | float | `6360000.0` (radio terrestre): **firma del layout** |

- FOV vertical = `2·atan(1/P[1][1])`. Aspecto = `P[1][1]/P[0][0]` (se han visto 50° y
  1,775).
- Telemetry exige que right, up y forward sean unitarios, ortogonales y dextrógiros, y que
  la traslación de `_view` cuadre con la posición dentro de 0,05.
- Para lecturas coherentes, Telemetry lee el bloque dos veces más la cadena y compara.
  Hace 3 intentos y considera la cámara no disponible si el contador se para.
- La **ruta antigua** (layout `renderer-camera-v1`, build 24994088): `[main 0x62593B0] +0x28
  +0x18 +0xE0 = cámara`, con fuente en `cam+0x428`, right/up/fwd en el mismo sitio,
  proyección en `+0x4E0` y near en `+0x860`. En nuestra build el global "main root" se
  resuelve con `48 8B 0D ?? ?? ?? ?? 49 8B E8 8B F2 48 85 C9` (único en `0x35A23A`) a
  `0x6C8CC00`. Es el `WinAppLauncher` de xsip. Sirve como comprobación cruzada en vivo.
- El escaneo estructural antiguo, que buscaba copias del registro de cámara de
  Streamline (DLSS), está **obsoleto**: solo funciona con DLSS y las direcciones son de
  cada sesión.
- **Negativos:** el zoom/FOV de Send `F3 41 0F 11 84 24 D8 00 00 00` (`[r12+0xD8]`, v1.00.03),
  incluidas sus variantes VEX, y el heading de CD Companion
  `C4 C1 7A 11 97 A4 04 00 00 C5 78 2F CE` (`r15+0x4A4`): **NOT FOUND**. bbfox no tiene
  entradas de cámara.

---

## 7. Resultados del escaneo en disco

Herramienta: `tools/scripts/aob_scan_file.py` contra nuestro exe (sin ejecutar nada). "RVA"
es el inicio del match.

### 7.1 Firmas actuales (válidas para 2.03.02)

| Firma | Resultado | RVA | Destino RIP / nota |
|---|---|---|---|
| Telemetry world-system | UNIQUE | `0xB88500` | → `0x6D69190` |
| tkhquang WS P1 / P2 / P3 | UNIQUE ×3 | `0x366CE2` / `0x366CD9` / `0x3619E6` | → `0x6D69190` (las tres) |
| Telemetry static-pos | UNIQUE | `0x942318` | XY → `0x6D77858`, Z → `0x6D77860` |
| bbfox worldOffset (Lua) | UNIQUE | `0x36A4046` | → `0x6D56500` |
| `world_offset_sub` (derivada) | UNIQUE | `0x36A4031` | `mov rcx,[rbx+0x2C0]` + vsubps |
| Telemetry camera ref | UNIQUE | `0x2D14367` | → `0x6C8CF30` |
| Telemetry camera vtable slot 1 / 2 | UNIQUE ×2 | `0x38EF0C0` / `0x3B4BBB0` | = slots 1 y 2 de la vtable `0x5D20718` |
| main root / xsip WinAppLauncher | UNIQUE | `0x35A23A` | → `0x6C8CC00` |
| bbfox `INJECT_GET_HP_2` | UNIQUE | `0x17AC2B9` | mitad de función; rax = StatEntry |
| `get_stat_entry` (derivada) | UNIQUE | `0x17ADCC0` | **entrada de función** |
| `hp_entry_site` (derivada) | UNIQUE | `0x17AC2AE` | instrucción `mov rsi,[rax+8]` en +0x0B |
| `phys_delta` (Orcax / CD Companion) | UNIQUE | `0x4282EAF` | `movups [r13],xmm0` en +0x0D; semántica sin verificar |
| xsip position | UNIQUE | `0x2E4461` | → `0x6D4E2C0`; semántica dudosa |
| shin StartAction / call | UNIQUE ×2 | `0x21C0990` / `0x2075C0F` | coincide con el RVA documentado para 2.03.02 |

### 7.2 bbfox CT (35 AOB distintos): 11 UNIQUE, 5 MULTI, 19 NOT FOUND

- **UNIQUE:** FAST_FRIENDSHIP, INF_ARROW_N_OTHERS, GET_HP_2, KEEP_UNEMBED_ABYSS_GEAR_NONEQ_1,
  SET_ITEM_INF_DUR, SET_MIN_RESIS = SET_ATTACK_DEFENSE (mismo sitio `0x17AD75C`),
  SET_BAG_SIZE, **ENTCAP (falso positivo: en `0x39E19E0` es otra instrucción,
  `vbroadcastss [r15+0x88]`)** y worldOffset.
- **MULTI:** INJECT_3966 (3), KEEP_UNEQUIP_ABYSS_GEAR_1 (2), SET_ITEM_CNT_Des (2) y WPCAP (2).
- **NOT FOUND:** GET_HP_1_3, GET_HORSE_HP, GET_PLAYER_DATA_AOB2, la Lua de Player_Base,
  Play_Base2 y Horse_Base3, ITEM_REMOVE_NO_DEC, SET_ITEM_CNT, NO_DRAGON_CD,
  FAST_ENEMY_KILL*, NO_REP_DEC, SET_MIN_REP, BROWSE_KUKU*, ARCHERY, GREATER_GEAR_DUR,
  FAST_PET_FRIENDSHIP_2 y KEEP_*_2.
- Moraleja: un "UNIQUE" de un patrón corto lleno de comodines (estilo AOBMaker) **no
  garantiza** que sea el mismo sitio. Hay que comprobar los bytes de alrededor.

### 7.3 Firmas de blizz3010/CrimsonDesertCoop (`game_structures.h`)

- **NOT FOUND:** PLAYER_PTR_PRIMARY/FALLBACK, STAT_WRITE_FALLBACK, CAMERA_ZOOM_FOV,
  WORLD_SYSTEM_P1/P3 (formato antiguo), PLAYER_BASE_DISCOVERY, CD_COMPANION_STATIC_XYZ,
  CAMERA_HEADING, ENTITY_BASE, TELEPORT_WAYPOINT, velocidad, MOUNT_PTR_CAPTURE, DRAGON_TIMER
  y COMBAT_FLAG.
- **UNIQUE:** POSITION_FALLBACK y CD_COMPANION_PHYS_DELTA, que son el mismo sitio
  (`0x4282EAF`).
- **MULTI:** STATS_ACCESS_FALLBACK (4, uno de ellos dentro de `GetStatEntry`) y
  MAP_DEST_CAPTURE (5).
- Todos sus RVAs y estáticos (`+5CC7618`, `+5CE0928`, `+36ADB8C`, `+12D78BA`…) son de la
  1.0x y **no** valen para nuestra build.

---

## 8. Recomendación para el probe en vivo (orden de confianza)

1. **WorldSystem y cadena de Telemetry hasta `phys+0x90`**, comprobando RTTI en cada salto
   (ClientActorManager, ClientChildOnlyInGameActor y ClientCharacterControlActorComponent).
   El componente de control se busca por RTTI dentro de `CCOIA+0x68`.
2. **Global estático de posición** (`0x6D77858`/`0x6D77860`), para comparar: mundo ≈
   `phys+0x90 + worldOffset`.
3. **Cámara:** `[0x6C8CF30]`, comprobar la vtable `0x5D20718`, `[+0x428]`, y comprobar la
   firma `+0xAC0 == 6360000.0`; con eso ya tenemos view, proyección y tamaño de pantalla.
4. **Stats:** de momento, solo leerlos tras capturar el puntero en `GetStatEntry`
   (`0x17ADCC0`, entrada de función). Si no, con `safetyhook::create_mid` en
   `0x17AC2AE+0x0B` (mitad de función: rax = entrada). Nada de cadenas fijas hasta
   validarlas.
5. **Montura:** investigar `ClientVehicleActorComponent` y `transform+0xC8` en vivo.

Todas las lecturas, protegidas con `VirtualQuery`/SEH (regla del proyecto). Ninguna
escritura.

## 9. Preguntas abiertas

- ¿Cuáles son la ruta estática hasta el `ServerStatusActorComponent` del jugador y los
  slots de Stamina/Spirit en 2.03.02? Hay que verificar en vivo la hipótesis
  `AM+0x50 → +0xA0 → …`.
- ¿Qué apunta r13 en `0x4282EBC`: posición autoritativa o delta de física?
- ¿Cuál es el layout de `ClientVehicleActorComponent` (montura actual)?
- ¿Qué significa el global `0x6D4E2C0` de xsip?
- ¿Se mantiene `cam+0x428` y el bloque SceneConstants sin DLSS, con FSR/XeSS o en GPUs
  AMD/Intel? Telemetry solo lo ha validado en NVIDIA.

---

## 10. Firmas para el probe en vivo

Formato: `nombre | patrón | descripción | fuente`. Solo usan `AA` y `??`. Todas dan
UNIQUE en el disco de la build 1.0.0.2976 (2.03.02). "rip@+N/len L" significa que
`destino = match + (inicio de la instrucción) + L + disp32`, con disp32 en `match+N`.

```sigs
world_system_ref | 48 8B 0D ?? ?? ?? ?? 48 8B 49 30 E8 ?? ?? ?? ?? 84 C0 75 23 49 8B 46 08 48 8B 48 68 C6 44 24 20 00 | mov rcx,[rip] rip@+3/len7 -> WorldSystem global (RVA 0x6D69190); +0x30 ClientActorManager, +0x50 CCOIA jugador | CrimsonDesertTelemetry build-25477059 (MIT)
world_system_ref_p3 | 48 8B 05 ?? ?? ?? ?? 48 8B 58 58 48 8D 54 24 ?? 48 8B CB | alternativa: mov rax,[rip] rip@+3/len7 -> mismo WorldSystem global 0x6D69190 | tkhquang/CrimsonDesertTools CrimsonDesertCore P3 (MIT)
player_static_pos_write | C5 FB 11 3D ?? ?? ?? ?? 44 89 3D ?? ?? ?? ?? | vmovsd [rip+XY] rip@+4/len8 -> X,Y(altura) RVA 0x6D77858; mov [rip+Z] instr +8 rip@+11/len7 -> Z RVA 0x6D77860 (mundo) | CrimsonDesertTelemetry build-25477059 (MIT)
world_offset_sub | 48 8B 8B ?? ?? 00 00 C5 FB 10 44 24 ?? C4 E3 79 21 44 24 ?? 20 C5 F8 5C 05 ?? ?? ?? ?? C5 F8 11 81 90 00 00 00 | mov rcx,[rbx+0x2C0]=fisica; vsubps en +0x15 rip@+0x19/len8 -> worldOffset Vec4 RVA 0x6D56500; mundo = phys+0x90 + worldOffset | derivada aqui de bbfox CT worldOffset (GPL-3.0, solo hecho)
camera_global_ref | 48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00 | mov rax,[rip] rip@+3/len7 -> puntero camara RVA 0x6C8CF30; vtable 0x5D20718; [cam+0x428]=SceneConstants (view +0x3E0, proj +0x4E0, pos +0x80) | CrimsonDesertTelemetry build-25477059 (MIT)
camera_vtable_slot1 | 48 8B C4 48 89 58 10 48 89 70 18 41 54 41 56 41 57 48 81 EC 10 01 00 00 8B B1 A8 02 00 00 4D 8B ?? 4D 8B ?? 4C 8B F2 48 8B D9 85 F6 | entrada de funcion = slot 1 de la vtable de camara (RVA 0x38EF0C0), huella de tipo | CrimsonDesertTelemetry build-25477059 (MIT)
camera_vtable_slot2 | 40 53 48 83 EC 20 48 8B 01 48 8B DA FF 50 68 4C 8B C8 48 63 48 08 85 C9 75 08 32 C0 48 83 C4 20 5B C3 48 89 7C 24 30 41 B0 01 33 FF 4C 8B D1 C5 F8 57 C0 48 83 F9 04 | entrada de funcion = slot 2 de la vtable de camara (RVA 0x3B4BBB0), huella de tipo | CrimsonDesertTelemetry build-25477059 (MIT)
main_root_ref | 48 8B 0D ?? ?? ?? ?? 49 8B E8 8B F2 48 85 C9 | mov rcx,[rip] rip@+3/len7 -> main root RVA 0x6C8CC00 (ruta antigua +0x28 +0x18 +0xE0 = camara, verificar) | CrimsonDesertTelemetry / xsip WinAppLauncher
get_stat_entry | 48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B F1 0F B7 DA 48 8B 09 48 83 C1 30 E8 ?? ?? ?? ?? 48 8D 4C 24 38 0F B7 90 ?? ?? 00 00 | ENTRADA de funcion RVA 0x17ADCC0: rcx=StatusComponent dx=id stat u16 -> rax=StatEntry* ([rcx+0x58]+slot*0x30; +0x08 cur, +0x10 regen, +0x18 max, int64 x1000) | derivada aqui (desensamblado en disco) desde el sitio HP step2 de bbfox
hp_entry_site | 0F B7 D3 49 8B CF E8 ?? ?? ?? ?? 48 8B 70 08 41 0F B7 D6 49 8B CF E8 | MITAD de funcion (RVA 0x17AC2AE): en +0x0B mov rsi,[rax+8]; rax=StatEntry de HP, r15=StatusComponent, bx=id stat; usar create_mid | bbfox CT INJECT_GET_HP_2 adaptada (GPL-3.0, solo hecho)
phys_delta_r13 | 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00 48 8B BB F8 00 00 00 | MITAD de funcion (RVA 0x4282EAF): +0x0D movups [r13],xmm0; r13 = vector de posicion (semantica sin verificar en 2.03.02) | Orcax player-status-modifier / CD Companion via blizz3010 game_structures.h
xsip_position_global | C5 FA 7F 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 83 C4 ?? E9 ?? ?? ?? ?? CC CC CC CC CC CC CC 41 B9 | vmovdqu [rip],xmm0 rip@+4/len8 -> RVA 0x6D4E2C0 (instancia CPosition: +0x8 -> +0x90 pos local?); sitio de inicializacion, sin verificar | xsip/CrimsonDesert-Developer-Console-Enabler Patterns.h (sin licencia)
```
