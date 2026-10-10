# Hallazgos en vivo con cdmp_probe

Build: CrimsonDesert.exe 1.0.0.2976 (PE timestamp `0x6AB28F00`, base fija `0x140000000`).
Fecha: 2026-10-03. Todo se leyó en el juego en marcha con `cdmp_probe`, en **solo
lectura**, con el juego en la pantalla de título (sin partida cargada).

## RTTI disponible
El exe conserva el RTTI de MSVC: **14.637 clases** en el namespace `pa`, más Havok
(`hknp*`). Con `cdmp_probe classes <texto>` y `vtables <nombre>` se localiza
cualquier clase, y con `findptr <vtable> --heap` sus instancias. Es la herramienta
principal para entender las estructuras sin depurador.

## Arquitectura cliente/servidor interna (herencia de BDO)
Aunque el juego es para un jugador, el motor separa un **cliente** y un **servidor**
dentro del mismo proceso:

| Cliente | Servidor |
|---|---|
| `ClientActorManager` | `ServerActorManager` |
| `ClientUserActor` | `ServerUserActor` |
| `ClientActor` | (`ServerActor…`) |

Se comunican con **1.116 tipos de mensaje** `pa::TrocTr*`: 486 `…Req` y 525 `…Ack`
(por ejemplo `TrocTrMoveFieldAck`, `TrocTrTeleportRevivePointAck`,
`TrocTrPostProcessAttackResult`, `TrocTrUseItemByItemInfoReq`).

**Contexto histórico:** Crimson Desert se anunció
originalmente como MMORPG (precuela de Black Desert). Pearl Abyss lo reconvirtió en
un juego para un jugador para no competir con su propio MMO. Eso explica la
separación cliente/servidor, y sugiere que puede quedar en el exe código de red real
de la época MMO (serialización de mensajes, sesiones, transporte).

**Implicación para el multijugador:** en vez de sincronizar memoria a mano (lo que
hacía el repo de referencia), podríamos interceptar ese canal de mensajes. El primer paso es encontrar dónde se envían y despachan los `Troc*`.

## Instancias y punteros estáticos (pantalla de título)

| Qué | Dirección / cadena | Notas |
|---|---|---|
| Puntero global a WorldSystem | `exe+0x6D69190` | WorldSystem no tiene vtable/RTTI. Firma `cdt_world_system` (1 match, `exe+0xB88500`) |
| ClientActorManager | `[exe+0x6D69190]+0x30` | También referenciado de forma estática en `exe+0x6D69A18` y `exe+0x6DDA800` |
| ClientUserActor (jugador) | `[ClientActorManager]+0x58` | Existe ya en la pantalla de título |
| CameraManager | `[ClientActorManager]+0xE8` | |
| ServerActorManager | estático en `exe+0x6DA67D8` | |
| Posición estática del jugador | `exe+0x6D77858` (X, Y) y `exe+0x6D77860` (Z) | Firma `cdt_static_pos`. Vale `(0,0,0)` en la pantalla de título. **Falta verificarla con una partida cargada** |
| Cadena a la física (Telemetry) | `[[[[[[exe+0x6D69190]+30]+50]+68]+40]+140]+2C0`, posición en `+0x90` | En la pantalla de título `+0x50` es null |

| Cámara (objeto) | `[exe+0x6C8CF30]` | Firma `cdt_camera_ref` (1 match) |
| Bloque de datos de cámara | `[[exe+0x6C8CF30]+0x428]` | Posición en `+0x80`, forward en `+0x90`. En la pantalla de título: posición (0,0,0) y forward (0,0,1) |
| Matriz de proyección | bloque `+0x4E0` (4×4 float) | Leída: filas `(1.545,0,0,0) (0,2.747,0,0) (0,0,0,1) (0,0,0.2,0)` → reversed-Z infinita, near = 0.2, FOV vertical = 2·atan(1/2.747) ≈ **40°**, aspecto 2.747/1.545 ≈ **16:9**. Basta para proyectar mundo → pantalla en el hito 3 |

Nuestro exe coincide por SHA-256 con el perfil `build-25477059` de
ZappendusterFX/CrimsonDesertTelemetry (MIT), probablemente el parche 2.03.02.

vtables: `ClientActorManager` `exe+0x55B62C8`, `ServerActorManager` `exe+0x5B25988`,
`ClientUserActor` `exe+0x55B59F0`, `ServerUserActor` `exe+0x5B257A8`,
`CameraManager` `exe+0x55CA3B8`.

## La cadena del repo de referencia no sirve en esta build
`actor → +0x40 → +0x08 → +0x248 → +0x90` falla: en `ClientUserActor+0x40` hay
`0xFFFFFFFF`, no un puntero. Confirma que sus offsets eran de otra build.

## Firmas de cd-companion-justskip.md en memoria viva
9 de 15 coinciden exactamente una vez (`cdmp_probe sigs docs/research/cd-companion-justskip.md`).
Fallan las de cd-companion "legacy" (XYZ estático antiguo, heading de cámara y entity
base) y la de JustSkip.

## Firmas de orcax-equiphide.md en memoria viva
**21 de 21** coinciden exactamente una vez (`cdmp_probe sigs docs/research/orcax-equiphide.md`).
Entre ellas están las 3 de WorldSystem, las 2 del slot de ActorManager, las 3 de la
vtable de ChildActor, el mid-hook de posición de Orcax (`exe+0x4282EB7`), el getter
de stats, la función de daño, stamina y los sucesores en `.xtls`. Se confirmó
el offset `+0x58` para UserActor; `game_structures.h` del repo de referencia dice
`+0x28`, que es incorrecto.

`UserActor+0xD8` (el actor del personaje controlado, `ClientChildOnlyInGameActor`
según CrimsonDesertTools) es **null en la pantalla de título**. Se comprueba con la
partida cargada.

## Firmas de cheat-tables-versions.md en memoria viva
**12 de 12** coinciden exactamente una vez. Nuestro exe es el **parche público 2.03.02**
(Steam build 25477059). Comprobaciones de la cámara:
- La vtable del objeto cámara es `exe+0x5D20718`, la esperada.
- El bloque `+0xAC0` vale `6360000.0`, que es el valor de control del bloque correcto.
- `+0x30`/`+0x34` son floats con el tamaño de pantalla: **2560 × 1440**.
- La matriz de vista está en `+0x3E0` y la proyección en `+0x4E0`.

El desplazamiento global del trozo de mapa (`exe+0x6D56500`) vale 0 en la pantalla de título.

Total en vivo: **42 de 48** firmas de las tres investigaciones coinciden exactamente
una vez. Las 6 que fallan son de cd-companion "legacy" y JustSkip, y ya estaban
marcadas como obsoletas.

## ✅ Verificado con partida cargada (2026-10-03, jugador corriendo)

Notación: `[x]` = leer puntero en x. Todo parte de la base fija `0x140000000`.

### Posición
- **Global de mundo:** `exe+0x6D77858` X, `+0x6D7785C` Y (altura), `+0x6D77860` Z
  (float). Mientras el jugador corría, X pasó de -10806 a -10828 en ~4 s (≈5,5 m/s)
  y la altura de 727,8 a 730,1.
- **Física:** `[[[[[[exe+0x6D69190]+0x30]+0x50]+0x68]+0x40]+0x140]+0x2C0` → objeto de
  física (vtable `exe+0x5D00F88`). Posición **local** en `+0x90`, matriz de rotación
  en `+0x60/+0x70/+0x80` (ortonormal, verificada).
- **Mundo = física `+0x90` + desplazamiento del trozo** (`exe+0x6D56500`, vec3; valía
  `(-10000, 0, -2000)`). Comprobado: `(-805.6, 728.2, -598.8) + (-10000, 0, -2000)`
  = `(-10805.6, 728.2, -2598.8)` frente al global `(-10805.4, 727.7, -2599.1)`.
- Orientación: según Telemetry, el jugador mira hacia −basisZ (fila `+0x80`).

### Actores
- `[[exe+0x6D69190]+0x30]` = `ClientActorManager`.
- `ClientActorManager+0x50` = actor del personaje (`ClientChildOnlyInGameActor`),
  el mismo que `[ClientActorManager+0x58 (ClientUserActor)]+0xD8`.
- `actor+0x68` = **tabla de componentes**:

| +off | Componente | +off | Componente |
|---|---|---|---|
| 0x08 | UserLogin | 0x58 | Ai |
| 0x18 | ChildContainer | 0x60 | Effect |
| **0x20** | **Status** | 0x68 | FrameEvent |
| 0x28 | SubLevel | 0x80 | Attack |
| 0x38 | EquipSlot | 0x90 | Target |
| **0x40** | **CharacterControl** | 0x98 | Interaction |
| **0x48** | **Vehicle (montura)** | 0xB8 | Inventory |
| 0x50 | Detect | 0xF0 | Skill |

### Stats (HP)
- Por desensamblado de `exe+0x17ADCC0` (getter): `entrada = [contenedor+0x58] + índice × 0x90`.
  El índice sale de una tabla de mapeo id → índice.
- Contenedor: `[Status+0x18]`. El número de entradas está en `contenedor+0x60` (u32, **20**).
- Entrada (0x90 bytes): `+0x00` id del stat, `+0x08` actual, `+0x10` regeneración,
  `+0x18` máximo. Todo int64 ×1000.
- **HP = id 0**: cadena `[[[[[[[exe+0x6D69190]+30]+50]+68]+20]+18]+58]+8`. Leído
  248 → 254 → 264 → 274 de 300 mientras se regeneraba.
- Las demás entradas válidas (id: actual/máx): 18: 90,5/100 · 17: 58/100 · 19: 2,5/100 ·
  20: 96,2/100 · 52: 84,4/400 · 81: 0/10 · 79: 0/100 · 32: 0/100 · 33: 100/100 ·
  22: 120/120 · 23: 30/30 (regen 0,1) · 24: 100/100 · 83: 100/100 · 78: 0/100 ·
  53: 0/400 · 80: 0/100 · 34: 0/100.
- **Falta** identificar stamina y espíritu: muestrear mientras el jugador esprinta,
  escala o usa una habilidad. No cambiaron en 3 s de carrera normal. Por el id y no
  por posición, porque el orden cambia entre parches.

### Stamina y otros stats (muestreo mientras el jugador esprintaba)
- **Stamina = id 22** (máx 120): bajó de 120 a 45 en 7 s de sprint y se rellena al
  máximo al parar.
- **id 52** (máx 400): baja despacio mientras se esprinta (78 → 60). Puede ser
  espíritu o fatiga; sin confirmar.

### Carga de partida
Mientras carga una partida, el juego coloca al actor en **(0, 1000, 0)** durante
unos 30 s, y luego lo pone en su sitio. El global del juego dice lo mismo. El mod lo
filtra y no lo envía. Secuencia en el log: `WorldSystem not created` → `no controlled
actor` → `control inner missing` → `in world`.

### Cámara (verificada con el overlay)
- La posición de la cámara (bloque `+0x80`) está en **coordenadas de mundo**, no
  locales al trozo. Con la cámara normal está a 2,4–6,8 m del jugador.
- La matriz de vista (`+0x3E0`) contiene right/up/forward y su tercer eje coincide con
  el forward del bloque.
- FOV vertical en torno a 42°. En algunos momentos (sprint o transiciones de cámara)
  sube hasta 70°, así que hay que leerlo en cada frame.
- La proyección `x = (d·right)·P00/(d·fwd)` e `y = (d·up)·P11/(d·fwd)` coloca la cruz
  sobre el personaje.

### Render / Streamline
- Una vez arrancado el juego, `CreateDXGIFactory1` devuelve objetos de Streamline:
  el Present enganchado vive en `sl.interposer.dll`. Eso permite dibujar sobre el frame
  del juego **antes** de la generación de frames.
- `IDXGISwapChain::GetDevice` no devuelve la cola de comandos; se usa la última cola
  DIRECT que ejecutó en el hilo de present.

## Pendiente
1. Espíritu: confirmar si es el id 52.
2. Montura: explorar el componente Vehicle (`tabla+0x48`) estando montado.
3. Comprobar que las cadenas sobreviven a una pantalla de carga y a un viaje rápido.
