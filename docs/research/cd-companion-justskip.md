# Investigación: posición, cámara, marcador, teletransporte y combate (cd-companion, JustSkip, CrimsonDesertTelemetry)

Fecha: 2026-10-03 · Build analizada (la única que importa): `CrimsonDesert.exe`
FileVersion **1.0.0.2976**, PE TimeDateStamp `0x6AB28F00` (2026-09-22 14:21 UTC),
ImageBase `0x140000000`, SizeOfImage `0x173AB000`, SHA-256
`57DA440D72F4DB974F25FEF047CF84C4DADD999A88CB2A3C5AF4C9BD67FDE1E7`,
Steam build `25477059`. `meta/0.paver` contiene `02 00 03 00 02 00`: el parche
probablemente sea **2.03.02**. El 2.03.00 salió el 2026-09-18 y la propia
CrimsonDesertTelemetry llama 2.03.02 a esta build.

Todas las RVA de este documento valen **solo para esta build**. Los clones están en
`reference/` (en `.gitignore`). No se ha copiado código de ningún repo: aquí solo hay
hechos (bytes, offsets, semántica de registros) con su atribución.

## 1. Fuentes

| Fuente | Qué es | Licencia | Versión del juego a la que apunta |
|---|---|---|---|
| [leandrodiogenes/cd-companion](https://github.com/leandrodiogenes/cd-companion) (v0.11.04, último commit `cdf69d0` del 2026-06-27) | Overlay de MapGenie que muestra la posición en vivo y permite teletransportarse | **Sin archivo LICENSE**. El README solo dice "provided for educational purposes". Solo se puede usar como referencia. | AOBs de abril de 2026 ("2026-04 patch", primer commit `85905b2` del 2026-04-29). La firma de la cámara se corrigió para el parche **1.09.00** (commit `c006993`, 2026-06-02). Es **anterior** a nuestra build. |
| [wealdly/JustSkip](https://github.com/wealdly/JustSkip) (v2.72, `fec155c`, 2026-04-05) | ASI para acelerar el juego: hookea `QueryPerformanceCounter`, no código del juego | **GPL-3.0** | Época del lanzamiento (abril de 2026). No depende de la versión porque no usa firmas del juego. |
| [ZappendusterFX/CrimsonDesertTelemetry](https://github.com/ZappendusterFX/CrimsonDesertTelemetry) (v2.2.4, `3284601`, 2026-09-29) | Telemetría de luces, jugador y cámara: API HTTP/WebSocket, HUD y radar | **MIT** | **Exactamente nuestra build**: `definitions/build-25477059.json` fija el mismo SHA-256, FileVersion 1.0.0.2976 y build de Steam 25477059. |
| `reference/CrimsonDesertCoop/include/cdcoop/core/game_structures.h` | Entradas `CD_COMPANION_*` y `COMBAT_FLAG_*` | Sin licencia | Copia de cd-companion de mayo de 2026, antes de la corrección para 1.09.00. **Nunca se probó en el juego.** |

### Arquitectura (precedentes para nuestra herramienta de sondeo externa)

- **cd-companion**: proceso **externo** en Python (`pymem`) que se eleva con UAC. Lee
  el módulo entero con `ReadProcessMemory` y hace el AOB scan sobre la **memoria
  viva**, no sobre el archivo. Con `VirtualAllocEx` reserva un bloque RWX y escribe
  *code caves* y parches `JMP rel32` de 7 o 9 bytes en el código del juego. **No es
  de solo lectura**: la posición sí la lee de globales estáticos sin hooks, pero el
  marcador, la cámara, el teletransporte y la invulnerabilidad parchean código.
  Guarda las RVA en caché en `%LOCALAPPDATA%\CD_Teleport\cd_hook_offsets.json` y la
  invalida cuando cambia `SizeOfImage`. Se basa en CDTT (dencoexe). Detecta si otro
  mod ya hookeó el mismo punto (OpenFlight, o Freedom Flyer con safetyhook de
  5 bytes) y encadena con él.
- **JustSkip**: ASI inyectado con MinHook. Solo usa APIs del sistema (QPC y XInput).
- **CrimsonDesertTelemetry**: tiene dos partes. Un **ASI** (MinHook, hooks de
  render D3D12, overlay ImGui) y un **host .NET 8 externo** que lee la memoria en
  **solo lectura** (`ReadOnlyProcess`). Ese host lee la posición, la cámara y la
  orientación del jugador **sin hooks**: resuelve los globales RIP-relative
  escaneando el **archivo exe en disco** (exige un único match) y valida el SHA-256
  del exe, las vtables y la plausibilidad de los datos. **Es el mejor precedente
  para nuestro probe externo.**

## 2. Verificación en disco: método

Cuando empecé, `tools/scripts/aob_scan_file.py` no existía. Usé un escáner propio
de solo lectura (analiza las cabeceras PE con `struct`, admite `??`
y resuelve los RIP-relative) y desensamblé fragmentos con `dumpbin /disasm` sobre un
`.obj` COFF temporal. Después volví a pasar el bloque `sigs` de la sección 5 por
`tools/scripts/aob_scan_file.py` y dio los mismos resultados: 7 UNIQUE, 1 MULTI(5)
y 5 NOT FOUND. El juego no se ha ejecutado ni se ha escrito nada en su carpeta.

Mapa de secciones (Denuvo renombra las secciones: fíate de los flags, no del nombre):

| Sección | VA | Tamaño virtual | Flags | Observación |
|---|---|---|---|---|
| `.rsrc` | `0x1000` | `0x51EA000` | R-X (code) | **El código real del juego** (entropía ~6.4, 618k `CC CC CC`). Todas las firmas válidas caen aquí. |
| `.xdata` | `0x51EB000` | `0x16A8000` | R | Datos de solo lectura: vtables (p. ej. la de la cámara, `0x5D20718`) |
| `.rdata` | `0x6893000` | `0x854000` (raw `0x35C800`) | RW | Datos RW. Lo que está más allá del tamaño raw es BSS: **los globales que buscamos están ahí**. |
| `.xtls` | `0x75C9000` | `0xF9466E2` | RWX | Región de Denuvo: código mezclado y bloques de entropía 8.0 (cifrados) |
| `.text` | `0x16F10000` | `0x1C3986` | R-X | Stub (entropía baja) |

**Leyenda de la verificación:** **1 match** = verificado (objetivo). **0** = no está en
disco: el código cambió en esta build o Denuvo lo oculta. Hay que tratarla como no
verificada, no necesariamente como incorrecta. **>1** = hace falta más contexto.

## 3. Tabla de firmas, offsets y resultados

### 3.1 Posición del jugador (globales estáticos XYZ)

| Firma | Origen | Matches en disco | Detalle |
|---|---|---|---|
| `C5 FB 11 05 ?? ?? ?? ?? 8B 44 24 28 89 05 ?? ?? ?? ??` (`vmovsd [rip+d],xmm0 ; mov eax,[rsp+28h] ; mov [rip+d],eax`) | cd-companion `AOB_XYZ_PREFIX/MID`. También en `CD_COMPANION_STATIC_XYZ` del coop. | **0** | Variantes con `[rsp+??]` o con otro registro: también 0. **La firma está obsoleta en esta build.** |
| `C5 FB 11 3D ?? ?? ?? ?? 44 89 3D ?? ?? ?? ??` (`vmovsd [rip+d],xmm7 ; mov [rip+d],r15d`) | CrimsonDesertTelemetry, `static-position-xy/z` ("locally-validated" en vivo el 2026-09-23 en esta build) | **1**: RVA `0x942318` | **XY** = rip(+4, fin +8) → **RVA `0x6D77858`** (X en `+0`, Y en `+4`). **Z** = rip(+11, fin +15) → **RVA `0x6D77860`**. El sitio es el epílogo de una función (mid-function), pero **no hace falta hook**: basta con leer los globales. |

Semántica (de CrimsonDesertTelemetry): **Y es la altura**. La distancia horizontal
se calcula con X y Z. En pantallas de carga aparecen dos centinelas: (0, 0, 0) y
(~0, 1000.15, ~0) con distancia horizontal menor de 100, que significan "todavía no
inicializado". cd-companion trata los mismos globales como coordenadas absolutas
(su `teleport_to_abs` calcula `target - static`). Su "world offset"
(`0F 5C 1D ?? ?? ?? ?? 0F 11 99 90 00 00 00`) da **0** matches en disco, así que en
esta build no se usa ningún offset adicional.

Candidato descartado, se anota solo como pista: `C5 FB 11 05 ?? ?? ?? ?? 41 8B 46 08 41 B0 01 89 05 ?? ?? ?? ??`
(1 match, RVA `0x2D17FED`). Copia un vec3 desde `[r14]` a los globales `0x6D696F0`
(XY) y `0x6D696F8` (Z). Está en la misma función grande que la referencia a la
cámara (`0x2D14367`). No coincide con el global validado y su significado no se
conoce.

### 3.2 Cadena del jugador y orientación (CrimsonDesertTelemetry, `playerRoot` de esta build)

| Paso | Valor |
|---|---|
| Firma de WorldSystem | `48 8B 0D ?? ?? ?? ?? 48 8B 49 30 E8 ?? ?? ?? ?? 84 C0 75 23 49 8B 46 08 48 8B 48 68 C6 44 24 20 00`: **1 match**, RVA `0xB88500`. rip(+3, fin +7) → **global `0x6D69190`** (puntero a WorldSystem). |
| Cadena | `*[global] → +0x30 (pa::ClientActorManager) → +0x50 (pa::ClientChildOnlyInGameActor = actor del jugador) → +0x68 → +0x40 (pa::ClientCharacterControlActorComponent) → +0x140 → +0x2C0 = objeto de física` |
| Objeto de física | Base ortonormal en `+0x60` (X), `+0x70` (Y = arriba), `+0x80` (Z), filas de 16 bytes. **Posición en `+0x90`**, local al chunk. **forward = −basisZ**. Rumbo = `atan2(fwd.x, fwd.z)` en grados. |
| Coherencia | `física+0x90` difiere del global estático en múltiplos de 1000 en X y en Z (origen del chunk). Telemetry acepta una tolerancia de 2 unidades. |

La firma `WORLD_SYSTEM_P1` del coop (`48 83 EC 28 48 8B 0D ...`) da **0** matches.
Hay que usar la de Telemetry.

### 3.3 Teletransporte por delta de física (cd-companion `hook_e`, técnica de OpenFlight)

| Firma | Matches | Detalle |
|---|---|---|
| `0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00` (hook + confirmación +8) | **1**: RVA `0x4282EAF` | La firma corta de 8 bytes `0F 28 C6 F3 45 0F 5C C8` también es única. |

- **Mid-function**: la entrada de la función está en `0x42820E0`. El sitio de
  inyección `0x4282EAF` es `movaps xmm0,xmm6 ; subss xmm9,xmm8`: 8 bytes
  reubicables y sin RIP-relative, así que valen para `safetyhook::create_mid`. Le
  siguen `addps xmm0,[r13] ; movups [r13],xmm0`.
- Registros en el punto de inyección (comprobados desensamblando la función):
  **`rbx` = objeto de física** (`mov rbx,rcx` en el prólogo, es el `this`).
  **`r13` = `rbx+0x90`** (`lea r13,[rbx+90h]` en `0x4282625`), es decir, el vec4
  de posición local al chunk, el mismo `+0x90` de la sección 3.2. **`xmm6` = delta
  de movimiento del paso** que se va a sumar a la posición.
- Cómo lo usa cd-companion: con flag 1 sustituye `xmm0` por
  `target_static − current_static`, y con flag 2 por un delta directo. Además guarda
  `rbx` para leer el forward en `rbx+0x80`/`rbx+0x88`, que coincide con basisZ.x/.z
  de Telemetry. **Escribir aquí mueve al jugador**: queda fuera del alcance del probe
  de solo lectura. Para nosotros es una pista para el teletransporte opcional.

### 3.4 Marcador del mapa (cd-companion `hook_d`)

| Firma | Matches | Detalle |
|---|---|---|
| `C5 FB 10 07 C5 FB 11 02 8B 47 08 89 42 08` (`vmovsd xmm0,[rdi] ; vmovsd [rdx],xmm0 ; mov eax,[rdi+8] ; mov [rdx+8],eax`). Hook en **+4**. | **5**: `0xAA4F15`, `0x179A7BD`, `0x17FA8F3`, `0x1EEE607`, `0x1EEE71A` | Los cinco son copias inline de un vec3 dentro de bucles de `push_back` (`rdi` = origen, `rdx` = hueco destino, `rbx`/`rbp` = array con el puntero en `+0` y la cuenta en `+8`). Las zancadas del origen son 0xC, 0x10, 0x10, 0x14 y 0x14. |
| Variante única del primer match: `C5 FB 10 07 C5 FB 11 02 8B 47 08 89 42 08 FF 43 08 48 83 C7 0C` | **1**: `0xAA4F15` (inyección en `0xAA4F19`) | cd-companion usa `data.find`, que devuelve **el primer match del módulo**, así que en esta build elegiría este. Su propio commit `ee0b844` reconoce que hay varias ocurrencias. |

- Mid-function, dentro de un bucle. En `+4`, `xmm0` = XY del vec3 y `rdi` apunta al
  vec3 de origen (Z en `[rdi+8]`). cd-companion guarda el **último** vec3 copiado
  (X, Y, Z) y un flag.
- **Semántica no verificada en esta build.** No se puede saber desde el disco cuál
  de las 5 funciones copia el destino del marcador. Hay que confirmarlo en vivo
  (poner un marcador y ver qué sitio se ejecuta con valores parecidos a la posición
  del mapa). La firma corta de 14 bytes, tal como está en el coop, es ambigua.

Variantes únicas de los demás candidatos, para probarlas en vivo:
`0x179A7BD`: `... 89 42 08 8B`; `0x17FA8F3`: `... FF 43 08 48 83 C7 10`;
`0x1EEE607`: `... FF 45 08 48`; `0x1EEE71A`: `... FF 45 08 4C`.

### 3.5 Rumbo de la cámara (cd-companion `hook_cam`)

| Firma | Matches | Detalle |
|---|---|---|
| `C4 C1 7A 11 97 A4 04 00 00 C5 78 2F CE` (`vmovss [r15+4A4h],xmm2 ; vcomiss xmm9,xmm6`), la que tiene el coop | **0** | Anterior a 1.09.00. Ojo: el coop comenta `vcomiss xmm9,xmm14`, pero los bytes `C5 78 2F CE` son `vcomiss xmm9,xmm6`. |
| `C4 C1 7A 11 97 CC 04 00 00 C5 78 2F CE` (`[r15+4CCh]`, versión 1.09.00 de cd-companion) | **0** | |
| `?? 04 00 00 C5 78 2F CE` y `C4 C1 7A 11 97 ?? ?? 00 00 C5 78 2F CE` | **0** | Hay 10 `vmovss [r15+disp],xmm2` en el código y ninguno tiene disp `0x4xx`. |

Era mid-function. En el punto de inyección `xmm2` = rumbo de la cámara en grados con
signo (−180..180) y `r15` = struct de la cámara. Conclusión: **no está en disco en
esta build**. CrimsonDesertTelemetry (`docs/PROVENANCE.md`) llegó a lo mismo de forma
independiente: comparó las dos firmas, la actual y la histórica, con este exe y no
encontró ninguna. **No hace falta**: el rumbo de la cámara se puede calcular con el
forward de la cámara del renderer (sección 3.6).

### 3.6 Cámara del renderer: posición, base, FOV y proyección (para world-to-screen)

Fuente: CrimsonDesertTelemetry (`engineCamera` de `build-25477059.json`,
`EngineCameraReader.cs`, `docs/ENGINE_CAMERA_RESEARCH.md`). La cámara se lee
directamente del motor, sin Streamline ni DLSS.

| Elemento | Valor en esta build | Verificación en disco |
|---|---|---|
| Referencia a la cámara | `48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00` (`mov rax,[rip+d] ; vmovsd xmm6,[rax+C8h] ; mov ebx,[rax+D0h]`) | **1**: RVA `0x2D14367`. rip(+3, fin +7) → **global `0x6C8CF30`** = `cameraGlobalRva`, coincide. |
| Objeto cámara | `camera = *[EXE+0x6C8CF30]`. Su vtable debe ser **EXE+`0x5D20718`** (en `.xdata`). | Las entradas 1 y 2 de la vtable en disco son `0x38EF0C0` y `0x3B4BBB0`, que coinciden con las dos huellas de abajo (1 match cada una). |
| Huella de la vtable, entrada 1 | `48 8B C4 48 89 58 10 48 89 70 18 41 54 41 56 41 57 48 81 EC 10 01 00 00 8B B1 A8 02 00 00 4D 8B ?? 4D 8B ?? 4C 8B F2 48 8B D9 85 F6` | **1**: `0x38EF0C0` (entrada de función) |
| Huella de la vtable, entrada 2 | `40 53 48 83 EC 20 48 8B 01 48 8B DA FF 50 68 4C 8B C8 48 63 48 08 85 C9 75 08 32 C0 48 83 C4 20 5B C3 48 89 7C 24 30 41 B0 01 33 FF 4C 8B D1 C5 F8 57 C0 48 83 F9 04` | **1**: `0x3B4BBB0` (entrada de función) |
| Posición de la cámara (en el objeto) | `camera+0xC8` (XY), `camera+0xD0` (Z) | El propio sitio `0x2D14367` lee esos campos |
| Contador de frames | `camera+0x2C8` (uint32). Si no cambia durante 1 s, la cámara se considera no disponible. | (dato de Telemetry) |
| Bloque fuente | `src = *[camera+0x428]` (0x868 bytes) | (dato de Telemetry) |
| Posición | `src+0x80` float3 | |
| Forward | `src+0x90` float3 | |
| Right | `src+0x3E0`, `+0x3F0`, `+0x400` (con zancada) | |
| Up | `src+0x3E4`, `+0x3F4`, `+0x404` (con zancada) | |
| **Proyección 4×4** | `src+0x4E0` (64 bytes). `sx=[+0x4E0]`, `sy=[+0x4F4]`, `[+0x50C]=1`, y los términos 1,2,3,4,6,7,8,9,12,13,15 valen ≈0. Es una perspectiva simétrica de estilo D3D (vector fila; el 1 está en m[2][3]). | |
| FOV vertical y aspecto | `fov = 2·atan(1/sy)`, `aspect = sy/sx`. Valores observados: 50° y 1.775. | |
| Near | `src+0x860`. El campo `+0x864` no es un far validado: el far real es infinito. | |

**Hipótesis (sin verificar):** la zancada de right/up apunta a que en `src+0x3E0`
hay una matriz de vista 4×4 en filas (`0x3E0`, `0x3F0`, `0x400`, `0x410`) con columnas
right/up/forward. Hay que comprobarlo en vivo antes de usarla. Si no se confirma,
la vista se construye a mano con pos, right, up y forward, y se proyecta con la
matriz de `+0x4E0`. Ref. adicional: el coop menciona zoom/FOV `movss [r12+0xD8],xmm0`
(`F3 41 0F 11 84 24 D8 00 00 00`), que da **0** matches en disco.

Otra ruta antigua, de la build 24994088: `*[main]+0x28 → +0x18 → +0xE0 = camera`. Su
firma de main root, `48 8B 0D ?? ?? ?? ?? 49 8B E8 8B F2 48 85 C9`, sigue dando
**1** match (`0x35A23A`, rip → `0x6C8CC00`), pero el perfil de esta build usa la
ruta directa (`mainRootGlobalRva = 0`). Que esa cadena siga valiendo no está verificado.

### 3.7 Flag de combate (JustSkip)

| Firma | Matches | Detalle |
|---|---|---|
| `48 8B 05 ?? ?? ?? ?? 80 78 1A 01` | **0** (y `80 78 1A 01` sola, también 0) | **No es un hallazgo: es un ejemplo inventado.** En el README y en el INI de JustSkip aparece como "Example" bajo la frase *"If the community discovers the game's in-memory combat flag"*, y por defecto `SignaturePattern=` está vacío. El coop lo presenta como "community-sourced", y eso es incorrecto. |

El resolver de JustSkip (`ScanSignature`) solo recorre las secciones con
`IMAGE_SCN_MEM_EXECUTE`, toma el primer match y calcula
`addr = match + offset + 4 + rel32`. Esa fórmula solo es correcta si el disp32 es lo
último de la instrucción. Además, después lee **un byte directamente en `addr`**, sin
desreferenciar. Con el ejemplo `mov rax,[rip+d] ; cmp byte [rax+1Ah],1`, el flag
estaría en `*(u64*)addr + 0x1A`, así que JustSkip leería el byte bajo de un puntero.
JustSkip detecta el combate de verdad con heurísticas: vibración de XInput fuerte,
LT + ataque y pulsaciones rápidas. **En esta build no tenemos un flag de combate
verificado.** Hay una pista sin verificar en el coop: `AnimationEvaluator +0x6A`
(CDAnimCancel). Su AOB `0F 28 CE 48 89 4C 24 20 48 8B CB E8` también da **0**.

### 3.8 Otras firmas de cd-companion (para completar)

| Firma | Matches | Comentario |
|---|---|---|
| `48 83 EC 50 48 8B F9 48 8B 91 30 11 00 00` (entity base, hook en +7) | **0** | `48 8B 91 30 11 00 00` sola también da 0. Obsoleta. |
| `0F 11 99 90 00 00 00` (`movups [rcx+90h],xmm3`, "position write") | **5** | **Peligrosa.** Los 5 matches son normalizaciones de las filas de una matriz (`sqrtps/divps` y luego `[rcx+50h]`, `[rcx+80h]`, `[rcx+90h]`), no la escritura de la posición. cd-companion, que toma el primero, hookearía la función equivocada. |
| `48 8B 46 08 48 89 F1` (invulnerabilidad) | 1, en `.xtls` (`0xC7E7A02`) | Es la codificación no canónica de `mov rcx,rsi`, en la región de Denuvo. Casi seguro que es una coincidencia. No usar. |

## 4. Conclusiones

1. **Posición**: está verificada para esta build. Basta con leer los floats en
   `EXE+0x6D77858` (X), `+0x6D7785C` (Y = altura) y `+0x6D77860` (Z), sin hooks. Las
   direcciones se resuelven con la firma de Telemetry, que tiene 1 match.
2. **Cámara para el overlay**: verificada en disco. Global `0x6C8CF30` → objeto
   cámara (comprobar la vtable `0x5D20718`) → `+0x428` → posición, base y proyección
   4×4 en `+0x4E0`. Con eso se puede hacer world-to-screen sin hooks.
3. **Rumbo de la cámara de cd-companion** (`r15+0x4A4/0x4CC`): no está en disco, y
   Telemetry coincide en que tampoco está en esta build. Se puede sustituir por
   `atan2(fwd.x, fwd.z)` usando el forward de la cámara.
4. **Delta de física**: verificado (`0x4282EAF`, mid-hook, rbx/r13/xmm6). Es útil
   para el teletransporte opcional, pero escribe en memoria.
5. **Marcador del mapa**: 5 candidatos. Hay que distinguirlos en vivo.
6. **Flag de combate**: no existe ninguno publicado. Lo de JustSkip es un ejemplo.

Preguntas abiertas: cuál de los 5 candidatos es el marcador de verdad; si
`src+0x3E0` es una matriz de vista completa; el convenio de profundidad de la
proyección (¿Z invertida?) para el clipping; la estabilidad de la cadena del jugador
tras pantallas de carga (Telemetry la vuelve a resolver en cada muestra), y encontrar
un flag de combate real.

## 5. Firmas para el probe (memoria viva)

Convención: el offset de inyección o de RIP va en la descripción. "rip+N/fin+M"
significa `target = match + M + int32(match+N)`.

```sigs
cdt_static_pos | C5 FB 11 3D ?? ?? ?? ?? 44 89 3D ?? ?? ?? ?? | player world pos: XY=rip+4/fin+8 -> RVA 0x6D77858 (X,Y), Z=rip+11/fin+15 -> 0x6D77860; Y is height | CrimsonDesertTelemetry (MIT) build-25477059
cdt_world_system | 48 8B 0D ?? ?? ?? ?? 48 8B 49 30 E8 ?? ?? ?? ?? 84 C0 75 23 49 8B 46 08 48 8B 48 68 C6 44 24 20 00 | WorldSystem ptr global rip+3/fin+7 -> 0x6D69190; chain +30 +50 +68 +40 +140 +2C0 = physics (basis +60/+70/+80, pos +90) | CrimsonDesertTelemetry (MIT) build-25477059
cdt_camera_ref | 48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00 | camera ptr global rip+3/fin+7 -> 0x6C8CF30; cam+C8 pos, cam+2C8 frame ctr, cam+428 src (pos +80, fwd +90, proj4x4 +4E0) | CrimsonDesertTelemetry (MIT) build-25477059
cdt_camera_vt_slot1 | 48 8B C4 48 89 58 10 48 89 70 18 41 54 41 56 41 57 48 81 EC 10 01 00 00 8B B1 A8 02 00 00 4D 8B ?? 4D 8B ?? 4C 8B F2 48 8B D9 85 F6 | camera vtable (RVA 0x5D20718) slot 1 function entry, expected RVA 0x38EF0C0 | CrimsonDesertTelemetry (MIT) build-25477059
cdt_camera_vt_slot2 | 40 53 48 83 EC 20 48 8B 01 48 8B DA FF 50 68 4C 8B C8 48 63 48 08 85 C9 75 08 32 C0 48 83 C4 20 5B C3 48 89 7C 24 30 41 B0 01 33 FF 4C 8B D1 C5 F8 57 C0 48 83 F9 04 | camera vtable slot 2 function entry, expected RVA 0x3B4BBB0 | CrimsonDesertTelemetry (MIT) build-25477059
cdt_main_root_ref | 48 8B 0D ?? ?? ?? ?? 49 8B E8 8B F2 48 85 C9 | legacy main root global rip+3/fin+7 -> 0x6C8CC00; old chain +28 +18 +E0 = camera (unverified on this build) | CrimsonDesertTelemetry (MIT) build-24994088 notes
cdc_phys_delta | 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00 | mid-hook at +0 (RVA 0x4282EAF, 8 relocatable bytes); rbx=physics obj, r13=rbx+0x90 local pos vec4, xmm6=step delta | leandrodiogenes/cd-companion (no license)
cdc_map_dest_first | C5 FB 10 07 C5 FB 11 02 8B 47 08 89 42 08 FF 43 08 48 83 C7 0C | map marker candidate cd-companion picks (first of 5), RVA 0xAA4F15, hook +4; rdi=src vec3, rdx=dst; semantics UNVERIFIED | leandrodiogenes/cd-companion (no license)
cdc_map_dest_generic | C5 FB 10 07 C5 FB 11 02 8B 47 08 89 42 08 | generic vec3 push_back copy, 5 matches on disk (AA4F15 179A7BD 17FA8F3 1EEE607 1EEE71A); hook +4 | leandrodiogenes/cd-companion (no license)
cdc_static_xyz_legacy | C5 FB 11 05 ?? ?? ?? ?? 8B 44 24 28 89 05 ?? ?? ?? ?? | legacy static XYZ write, XY rip+4/fin+8 Z rip+14/fin+18; 0 matches on disk | leandrodiogenes/cd-companion (no license)
cdc_cam_heading_4CC | C4 C1 7A 11 97 CC 04 00 00 C5 78 2F CE | camera heading mid-hook xmm2=signed deg r15=cam struct (game 1.09.00); 0 matches on disk | leandrodiogenes/cd-companion (no license)
cdc_cam_heading_4A4 | C4 C1 7A 11 97 A4 04 00 00 C5 78 2F CE | camera heading pre-1.09.00 (as in coop header); 0 matches on disk | leandrodiogenes/cd-companion (no license)
cdc_entity_base | 48 83 EC 50 48 8B F9 48 8B 91 30 11 00 00 | entity base capture hook +7 rcx=entity; 0 matches on disk | leandrodiogenes/cd-companion (no license)
js_combat_example | 48 8B 05 ?? ?? ?? ?? 80 78 1A 01 | INI EXAMPLE only, not a real finding; flag would be *(rip+3/fin+7)+0x1A; 0 matches on disk | wealdly/JustSkip (GPL-3.0)
research_vec3_r14 | C5 FB 11 05 ?? ?? ?? ?? 41 8B 46 08 41 B0 01 89 05 ?? ?? ?? ?? | unknown vec3 global copied from [r14], XY rip+4/fin+8 -> 0x6D696F0, Z rip+17/fin+21 -> 0x6D696F8; 1 match RVA 0x2D17FED | own disk analysis
```
