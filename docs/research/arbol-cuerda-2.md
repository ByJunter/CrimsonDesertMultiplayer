# Árbol con cuerda (2): la suelta que falta y la cuerda que no llega

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas
solo para esta build. Análisis estático de solo lectura más capturas; el juego no se ejecutó.
Continúa `arbol-cuerda-fallo.md` y `interacciones-mundo.md` §8.

Capturas usadas:

| Captura | Qué es |
|---|---|
| `bin64/cdmp_packets_20261006-223211.bin` | anfitrión (emisor), enganches 1 y 2 de la prueba con Shadow |
| `bin64/cdmp_packets_20261006-223810.bin` | anfitrión tras reiniciar el juego (el log `cdmp.1.log` acaba en `"Terminate 시작."` a las 22:37:41), enganche 3 |
| `build/research/shadow-20261006b/cdmp_packets_20261006-213346.bin` | Shadow (receptor), con los paquetes inyectados a la copia |
| `bin64/cdmp_packets_20261006-065017.bin`, `…-201209.bin` | capturas anteriores (2 y 5 enganches) para confirmar el patrón |

Lo marcado **(hipótesis)** no está comprobado. Scripts y volcados nuevos en
`build/research/interactions/rope2/` (§6).

---

## 0. Resumen

1. **Suelta.** El mod solo suelta la copia con la acción `1f4ef471`. El emisor termina un enganche
   de **dos** formas: `1f4ef471` (suelta tras tirar, con lanzamiento) o **`d20295ad`** (suelta o
   cancela sin lanzamiento, seguida de `8feda3fd`). Los enganches 2 y 3 de la prueba acabaron con
   `d20295ad`, y por eso no hubo suelta. En las 12 capturas de enganche analizadas, el fin es
   siempre la **primera `StartAction` de la capa principal (`+8 != 0`) cuyo `+8` no está en el
   conjunto de la cuerda** `R = {9a368460, 2821f20a, a747bf2f, 7cc69d61, d9c333c4, 76d8c2d8,
   6b4ee3ca, 563271fe}`. Hay además una señal del emisor que no falla nunca: su propio
   `UpdateGimmickReq` de la herramienta con `+4 = 0x550D1ED6`, `+8 = 0x85FB13C7` y
   `+0x14 = jugador`.
2. **Por qué la copia no suelta sola.** En el cliente, la suelta local sale de
   `ClientFrameEventReleaseRemoteCatch` (`0x7F8F40` → `0x5A7A10`) y de `vf40` del componente
   (`0x58B380`, cambio de acción en la fase 4). Ninguna de las dos funciona en la copia: los frame
   events de la marioneta no se ejecutan (mismo caso que en `pendiente-proyectiles.md`) y la copia
   nunca llega a la fase 4. Así que la suelta tiene que inyectarla el mod.
3. **Cuerda corta.** Con pieza `-1`, la copia pasa todas las comprobaciones. El extremo se coloca
   con la transformación del objeto de escena (`0x1851010` → `vf 0x38`), así que el fallo no es
   silencioso. Shadow mandó `IsRemoteCatchEffectStartStateReq 00 01` 24 ms después del 2763
   inyectado y `00 00` 0,43 s después, el mismo patrón y el mismo tiempo que el anfitrión (vuelo de
   unos 19 m a 45 m/s). Eso indica que el componente de la copia recorre el inicio y el vuelo
   **(hipótesis: también el enganche, fase 3)**. Lo que falta tiene que ver con la física: con
   pieza `-1` no hay cuerpo físico del tronco al que atar la cuerda ni que doblar.
4. **El receptor puede calcular su propio id de pieza a partir del id de escena** con la misma
   secuencia que usa el juego en `0x5947C7`–`0x594826`: `0x1851010(id de escena)` →
   `obj` → `[[obj+0x58]+0xE0]->vf(0x70)()`. Ese es el id de pieza que el cliente pone en su
   `DoFrameEventReq`.
5. **La herramienta (`0xB01000xx`, "Game Mechanic" con `RemoteCatchPivot`) no la lee ninguna ruta
   del componente.** El origen de la cuerda sale del esqueleto del actor: `0x500730`, y los huesos
   `"Bip01 L Hand"`/`"Bip01 R Hand"` en `0x5AF584`. Sin la herramienta solo faltan sus propios
   efectos **(hipótesis)**.

---

## 1. Cómo termina cada enganche del emisor

`rope2/rope_end.py` y `rope2/rope_tl.py`. En `StartActionReq`, `+4` es la acción anterior y `+8`
la nueva (capa principal); `+0x49`/`+0x4D` son la capa superior, y en esos paquetes `+4 = +8 = 0`.

### 1.1 Prueba del 6 de octubre (anfitrión = emisor)

| Enganche | t (captura) | Cadena principal (`+8`, s desde el 2763) | Fin | Herramienta (`UpdateGimmickReq`/GEFE, `+8 = 85fb13c7`) |
|---|---|---|---|---|
| 1 (`223211`) | 236,47 | `2821f20a`@0,07 `a747bf2f`@0,48 `7cc69d61`@0,78 `d9c333c4`@4,01 `76d8c2d8` `6b4ee3ca` `563271fe`@4,94 ×8 | **`1f4ef471`@16,06** → `be0b7eed`, `eb340761` (vuelo) | `da667cc9`@0,05, `a55a59ff`@0,48, **`550d1ed6`@16,11**, `edfd0ebb`@16,37 |
| 2 (`223211`) | 268,93 | `2821f20a`, `a747bf2f`, `7cc69d61`@0,77 repetida cada ~1,36 s durante 38 s (no tira) | **`d20295ad`@39,13** → `8feda3fd` | `da667cc9`, `a55a59ff`, **`550d1ed6`@39,29**, `edfd0ebb`@39,80 |
| 3 (`223810`) | 91,64 | `2821f20a`, `a747bf2f`, `7cc69d61` ×3 | **`d20295ad`@4,43** → `8feda3fd` | `da667cc9`, `a55a59ff`, **`550d1ed6`@4,59**, `edfd0ebb`@5,11 |

Otros mensajes del emisor:
- `DoFrameEventReq`: del enganche `9a368460` fe 14–19 (var. 7, 517 B) y fe 20 (var. 6, 636 B, el
  objetivo). Para soltar, `1f4ef471` fe 5 (var. 3, 521 B, el `ServerFrameEventReleaseRemoteCatch`
  que envía `0x7F927E`) o `d20295ad` fe 3 (var. 7, 517 B).
- `IsRemoteCatchEffectStartStateReq`: `00 01` +0,03 s y `00 00` +0,45 s después del 2763, en todos
  los enganches.
- Ninguno lleva `ReleaseRemoteCatchResultAck`: el servidor no lo emite (`interacciones-mundo.md`
  §2.1).

### 1.2 Capturas anteriores (mismo patrón)

| Captura | Enganche | Fin |
|---|---|---|
| `065017` | 1 | `563271fe` → **`d20295ad`**@12,21 (suelta sujetando, sin lanzamiento) |
| `065017` | 2 | `563271fe` → **`1f4ef471`**@15,97 |
| `201209` | 1 / 5 | **`1f4ef471`** (en el 5 vuelve a tirar: `563271fe` → `7cc69d61` → `d9c333c4` …) |
| `201209` | 2, 3, 4 | `7cc69d61` → **`d20295ad`** |

**Las 12 acaban en `1f4ef471` o en `d20295ad`**, y en las 12 aparece justo después el
`UpdateGimmickReq` de la herramienta `550d1ed6 | 85fb13c7`. `85fb13c7` es el mismo u32 que va en
`+0x53` del 2763: la clave de configuración de la cuerda (§3.2).

### 1.3 Lo que vio Shadow

Los 2763 inyectados para `0xA010F001` llegaron en t 133,49, 165,93 y 346,04. Los 2471 llegaron en
149,71 (la suelta de `1f4ef471`) y en 346,04 (la suelta preventiva antes del enganche 3; la copia
se había recargado en 326,3 al reiniciar el anfitrión). Las `StartActionAck` de la copia
reproducen la cadena completa, `7cc69d61 → d20295ad → 8feda3fd` incluida (en 350,61 y 351,27),
pero `d20295ad` no estaba en la regla del mod. Las horas de Shadow van **~1 h 0 min 14–16 s** por
detrás de las del anfitrión.

---

## 2. Código de suelta en el cliente (llamadores de `0x5A7A10`)

| Llamador | Qué es | Cuándo suelta | ¿Funciona en la copia? |
|---|---|---|---|
| `0x7F8F40` | `ClientFrameEventReleaseRemoteCatch::vf1` (vtable `0x5585490`) | frame event de suelta de la animación (datos `+8` ranura, `+9` motivo, `+0xA`); antes manda `ServerFrameEvent…` (`0x4EC430`) | **no**: los frame events de la marioneta no corren (precedente: proyectiles) |
| `0x58B380` | `ClientRemoteCatchActorComponent` vf40, **al cambiar de acción** | por ranura, si la fase `[comp+r*0x1A0+0x9BC] == 4` y la nueva acción no tiene `byte [datos+9] & 4` ("mantener enganche"). También llama a `0x595FE0(comp, 0)` (efecto apagado) | no: la fase 4 la pone `vf50` `0x5A0770`, que solo llega desde el frame event (`0x593B50` con `+0x57`) |
| `0x59D470` (tick de cada ranura) | `0x59D614` / `0x59D67E` | `0x59D614`: hay un pendiente `comp+0xD00+r*16` y vence el tiempo `[0x6C99B98]`. `0x59D67E`: en la fase 3, `0x5A40E0` deja de encontrar el objetivo | sí, pero no salta: la copia no tiene pendiente y el árbol sigue ahí |
| `0x59F760` | actualización de la fase 4 | condiciones del tirón | no (no hay fase 4) |
| `0x117F0C0` / `0x117F7E0` | `RemoteCatchFunctions@gameClientScript` vf1/vf2 (vtable `0x56FFB48`) | condición de script `0x1180EA0` | **(hipótesis)** depende del estado de tirón |
| `0x87F750` (← `0x87EA10`) | reinicio de estado del actor | suelta las dos ranuras si `0x17FE800(actor)` | posible (muerte, montura…) |
| `0xB5BD60` | lector de `ReleaseRemoteCatchResultAck` | lo que inyecta el mod | **sí** |

El tick se registra para todos los actores. `0x8A0FC0` salta el bloque principal si
`[actor+0x88]+1 == 1` y `byte [actor+0x96] == 0` (jugador no local), pero registra igualmente
`0x8A5D40` → `0x8A1640` → `0x59C0D0` → `0x59D470`.

### 2.1 Regla robusta (receptor)

Mientras la copia tiene un enganche (desde el 2763 inyectado):
1. Para cada `StartAction` del emisor que no sea de caballo, con `+8 != 0` (capa principal):
   **si `+8 ∉ R`, inyectar el 2471 con el mismo `due` que esa acción** y marcar la copia como
   suelta. Esto incluye `1f4ef471`, `d20295ad` y cualquier interrupción (golpe, caída, muerte),
   igual que hace `vf40` del juego al cambiar de acción.
   `R = {9a368460, 2821f20a, a747bf2f, 7cc69d61, d9c333c4, 76d8c2d8, 6b4ee3ca, 563271fe}`.
2. Vigilancia: si pasan más de **5 s** sin ninguna `StartAction` con `+8 ∈ R`, soltar. El emisor
   repite `7cc69d61` cada ~1,36 s y `563271fe` cada ~1,5 s mientras sujeta.
3. Al recargar la copia (nuevo `ActorLoadDataAck` del jugador copia) o al desconectarse el otro
   jugador: poner `g_copy_hooked = false` (componente nuevo; una suelta de más es inofensiva).
4. Mantener la suelta preventiva antes de cada 2763 nuevo (§7.2 del documento anterior). Funcionó
   en 346,04.

Alternativa más directa (emisor): cuando el emisor ve **su propio** `UpdateGimmickReq` (2758, C→S)
con `+4 == 0x550D1ED6`, `+8 == 0x85FB13C7` y `+0x14 == jugador`, reenviar un 2471 sintético
`{jugador, 0, 2}` por el mismo relevo que el 2763 (el receptor cambia `+0` por la copia). Va 12/12
en las capturas y no depende de nombres de acción. Solo el `+0` de la herramienta cambia en cada
carga (`0xB01000FA`, `0xB01000F1`, `0xB01000A2`, `0xB01000D5`).

---

## 3. La cuerda en el cliente (fases)

Componente `comp = [[actor+0x68]+0x78]` (`ClientRemoteCatchActorComponent`, vtable `0x55890D8`).
Por ranura `r` (0–1):

| Campo | Contenido |
|---|---|
| `comp + r*0x58 + 0x20` | entrada del objetivo (tipo; 6 = vacía), `+0x28` clase, `+0x2C` actor, `+0x30` escena (16 B), `+0x40` pieza |
| `comp + r*0x1A0 + 0x928` | bloque de la cuerda: `+0x928` cuerpo de la cuerda, `+0x930` punta, `+0x968`, `+0x970`, `+0x988` (punteros con asa, `+8` objeto, `+0x15` muerto) |
| `comp + r*0x1A0 + 0x9B8` | u32 clave de configuración (`0x85FB13C7`) |
| `comp + r*0x1A0 + 0x9BC` | **fase**: 0 nada, 1 lanzando, 2 en vuelo, 3 enganchada, 4 tirando, 5, 6 soltando, 7 |
| `comp + r*0x1A0 + 0x9F8/9F9/9FA` | indicador de cambio de fase, fase actual y anterior (los pone el tick) |
| `comp + r*0x1A0 + 0x9A8` | vec3 de salida del vuelo |
| `comp + r*0x1A0 + 0xA90 / 0xA94` | f32 tiempo de vuelo transcurrido / duración (= distancia / velocidad) |
| `comp + r*0x1A0 + 0xAC0` | efecto "remote catch" activo (`0x595FE0`) |
| `comp + 0xCA8 + r` | 1 al enganchar, 2 al soltar; `comp + 0xCB0 + r*8` marca de tiempo |
| `comp + 0xD00 + r*16` | pendiente del servidor (solo en el lanzamiento local; `+8` marca de tiempo) |

Flujo con el 2763 (sea el del jugador local o el inyectado a la copia):
1. `0x594AD0` → (tras F2) **`0x5ABBC0`** si el u32 `+0x53 != 0`. Busca la configuración
   `0x85FB13C7` en `[[0x6D69190]+0xB0]` (`0x2456860`) y el socket de salida en el esqueleto del
   actor (`0x500730`: tipo 1 → nodo de escena del esqueleto `0x26D32C0`; **no usa la
   herramienta**). Crea la cuerda física en el mundo `[[0x6D69190]+0xE0]+0xF38` (`0x3B57AE0`) y los
   efectos de la configuración con `RemoteCatchEffect_UserData` (vtable `0x5589290`;
   `+0x50` clave del actor, `+0x58` ranura, `+0x5C` velocidad, `+0x60` indicador). **Pone la fase
   a 1** (`0x5AC58C`).
2. **Fase 2**: `0x5A12A0` (duración = distancia / `f32 +0x57` del Ack = 45). Lo llama directamente
   `0x5ABBC0` (`0x5AD069`) o la llamada de vuelta del efecto `0x5B2E30` → `0x58A1C0`, que busca al
   actor por clave con `0x8AF170` (exige `[actor+0x5E] & 0x18`; la copia tiene `0x10`).
3. Tick `0x59D470` → `0x59E150`: `+0xA90 += dt`. Con progreso ≥ 1 y sin pendiente →
   **`0x5A1530`**: `0x5A40E0` y `0x5A4760`/`0x5A4850` resuelven el punto, crean la restricción
   física (`0x338C940`, `0x26D5E60`) y **ponen la fase a 3** (`0x5A24DB`).
4. En la fase 3, el tick resuelve el objetivo en cada fotograma (si falla, suelta) y hace
   `0x598F80` + `0x59F5F0`. `0x5AEAA0` coloca la cuerda entre la mano (`"Bip01 L Hand"`/
   `"Bip01 R Hand"`, `0x5AF584`) y el objetivo.
5. La fase 4 (tirón) y el tirón real (`vf51` `0x5A68D0`, desde `ClientFrameEventRemoteCatchPull`
   `0x807C80`) salen de **frame events**, igual que el efecto (`ClientFrameEventRemoteCatchEffectState`
   `0x6D2090` → `0x595FE0`).

### 3.1 Qué pasa con la pieza `-1`

- `0x5A5370` y `0x5A4850` con pieza `-1` y escena `≠ 0`: `0x1851010` sobre
  `[[[0x6D69190]+0xC8]]+0x80` → `obj->vf(0x38)` ∘ punto local. **Devuelve `true`** si el árbol
  está cargado. No falla en silencio: si fallara, `0x594AD0` habría dado el error de `0x592536`, y
  esta vez no hubo errores.
- Esa transformación es la del objeto de escena (estática). Con pieza válida se usa la del cuerpo
  físico (`vf 0x3B0`/`0x3B8` sobre `[[0x6D69190]+0xE8]`), que sí sigue la flexión.
- **(Hipótesis, media)** La flexión del árbol es física local: la cuerda se ata al cuerpo del
  tronco (la pieza) con la restricción de `0x5A1530`, y el tirón (`0x5A68D0`) actúa sobre ella. Con
  `-1` no hay cuerpo: el extremo queda en el punto de reposo y el árbol no se dobla. La flexión no
  tiene mensaje de red (`interacciones-mundo.md` §2.1), así que en el receptor solo puede salir de
  esa física local.
- **(Hipótesis, media-baja)** La cuerda corta se ve porque la copia no pasa de la fase 1/2, o
  porque la cuerda física no tiene a qué atarse. Hay un dato en contra de que se quede en la fase
  1: Shadow envió `IsRemoteCatchEffectStartStateReq 00 01`/`00 00` con el mismo intervalo de
  0,43 s que el anfitrión, y eso no lo provocó ninguna acción de la copia (su `9a368460` llegó
  64 ms **después** del Ack). Para decidirlo hace falta la sonda de §4 F3.

### 3.2 Herramienta `0xB010xxxx` ("Game Mechanic")

- El servidor la carga con el resto del equipo acoplable del jugador (7 gimmicks de registro 7 tras
  `OnDemandEquipDockingGimmickSpawnReq`). Su carga contiene `"RemoteCatchPivot"`,
  `"Bip01 L HandTwist"`, `"InteractionPivot"`, `"Gimmick_OBB_Trigger"` y `"InteractionUI"`.
- Su estado durante el enganche (`UpdateGimmickReq`/GEFE): `003ecc59`/`92786a19` con
  `6ba22087`/`080bfa8f`/`76b28076`, y `da667cc9` → `a55a59ff` → `550d1ed6` → `edfd0ebb` con
  `85fb13c7`. Es un `UpdateGimmickReq` que el servidor no reenvía a nadie.
- Ninguna función de la cadena de §3 lee la herramienta: los sockets salen de `0x500730` y
  `0x5AF584` sobre el propio actor. **(Hipótesis)** sin ella solo faltan los efectos de la
  herramienta. La `OnDemandEquipDockingGimmickSpawnReq` de la copia (Shadow 49,6 y 326,7) sigue
  sin respuesta.

### 3.3 Id de pieza del receptor a partir del id de escena

`0x593B50`, `0x5947AD`–`0x594859` (el propio cliente al preparar un objetivo por id de escena):

```
pieza = -1
si id_escena (16 B) != 0:
    mgr = [[[exe+0x6D69190]+0xC8]] + 0x80          ; qword, qword, +0x80 (qword)
    att = {}                                        ; 0x20 B: vt, +8 obj, +0x10 u8 hallado
    0x1851010(mgr, &att, &id_escena)                ; toma el cerrojo de mgr+0x10
    obj = att.+8
    h = [obj+0x58]
    si h && byte[h+0x15] != 1:                      ; (si no: mira byte[obj+0xA4]; si es 0, nada)
        b = [h+0xE0]
        si b: pieza = b->vtbl[0x70/8](b)            ; u64, p. ej. 0x800000000E000857
    liberar att: att.vt = exe+0x5725E48 (ScopeAttacherBase); 0x1434880(&att)
el juego exige pieza != -1 para su propio enganche (0x594854)
```

---

## 4. Arreglos, por confianza

| # | Confianza | Arreglo | Detalle y riesgos |
|---|---|---|---|
| **F1** | **alta** | Soltar con la regla de §2.1 | En `remote_puppet.cpp`, sustituir la condición `== kActionRopeRelease` por "con enganche y `+8 != 0` y `+8 ∉ R`". Añadir la vigilancia de 5 s y la puesta a cero al recargar la copia. Mismo `due` que la acción. Riesgo: una acción nueva de la cuerda que no esté en `R` soltaría antes de tiempo, pero las 12 capturas cuadran. |
| F1b | alta | Señal del emisor (`UpdateGimmickReq 550d1ed6/85fb13c7`, `+0x14 = jugador`) → relevo de un 2471 sintético | Se puede combinar con F1; el receptor ignora una segunda suelta si ya está suelta. Cambia lo que se relevaría; si cambia el formato del protocolo, subir `kVersion`. |
| **F3** | alta (diagnóstico) | Sonda de solo lectura de la copia durante un enganche | `comp = [[copia+0x68]+0x78]`. Cada 100 ms, para r = 0: `byte comp+0x20` (tipo), `u64 comp+0x40` (pieza), `byte comp+0x9BC` (fase), `f32 comp+0xA90/0xA94`, `qword comp+0x928/0x930/0x968/0x970` (≠ 0), `byte comp+0xAC0`, `byte comp+0xCA8`, `qword comp+0xD00`. Hacer lo mismo con el jugador local para comparar. Todo con `VirtualQuery`/SEH. Decide §3.1: fase 1, 2 o 3. |
| **F2** | media | Pieza propia del receptor por id de escena (§3.3) en vez de `-1` | Llamar a `0x1851010` y a `vf(0x70)` bajo SEH, mejor desde el hilo del juego que inyecta (o justo antes de inyectar). Si devuelve `-1`, quedarse con `-1`. Riesgos: llamar a código del juego fuera de su hilo (el cerrojo de `mgr+0x10` lo protege; `vf(0x70)` parece un *getter*, sin comprobar). Una pieza equivocada da F2 y deja la ranura ocupada, así que mantener la suelta preventiva. Debería permitir que la cuerda siga el tronco y lo doble **(hipótesis)**. |
| F4 | baja | Filtrar el `IsRemoteCatchEffectStartStateReq` (2108, C→S) que provoca la copia | Shadow lo envió en 133,52/133,95, 165,96/166,41 y 346,06/346,47; su servidor lo aplica a su propio jugador (`0x2971BD0`). Parece inofensivo. Solo filtrarlo en ~1 s tras un 2763 inyectado y si el jugador local no tiene enganche propio. |
| F5 | baja | Cargar la herramienta en la copia | No hace falta según §3.2. |

### 4.1 Pruebas

| # | Prueba | Decide |
|---|---|---|
| T1 | Dos PC, F1: enganchar y soltar sin tirar (`d20295ad`) y tirando (`1f4ef471`) | Que la cuerda y la marca desaparecen en los dos casos |
| T2 | Un PC (`cdmp_peer`): el receptor tiene la pieza válida (`g_own_pieces`, porque el jugador engancha el mismo árbol) con F3 activa | Cómo se ve la cuerda con pieza válida |
| T3 | Dos PC con F3 (pieza `-1`) | En qué fase se queda la copia; si llega a la 3, el problema es la física o el dibujo |
| T4 | Dos PC con F2 | Si con la pieza propia la cuerda llega y el árbol se dobla |

---

## 5. Confianza

| Afirmación | Confianza |
|---|---|
| Los enganches 2 y 3 acabaron con `d20295ad` y el mod no lo trata | alta (capturas) |
| Regla "primera acción principal fuera de `R`" | alta (12/12) |
| `UpdateGimmickReq 550d1ed6/85fb13c7` marca el final | alta (12/12) |
| La copia no suelta sola (frame events, fase 4) | media-alta |
| Pieza `-1` → punto correcto en reposo, sin error | alta (código + sin errores) |
| La flexión necesita la pieza (cuerpo físico) del receptor | media (hipótesis) |
| Causa exacta de la cuerda corta | baja (hace falta F3) |
| Secuencia id de escena → pieza (§3.3) | alta (es el código del propio juego) |
| La herramienta no hace falta para la cuerda | media |

---

## 6. Archivos nuevos (`build/research/interactions/rope2/`)

| Archivo | Contenido |
|---|---|
| `capl.py` | cargador genérico de capturas con caché |
| `rope_tl.py <cap> [jugador]` | línea de tiempo del enganche (SA, DFE, herramienta, 2108, 2763/2471, Move) |
| `rope_end.py <cap> [jugador]` | resumen por enganche: cadena principal y fin |
| `offscan.py <lo> <hi> <regex>` | búsqueda de instrucciones por expresión regular |
| `tl_223211.txt`, `tl_223810.txt`, `tl_065017.txt`, `tl_201209.txt` | líneas de tiempo |
| `c_7f8f40.asm` | `ClientFrameEventReleaseRemoteCatch::vf1` |
| `c_58b380.asm` | `vf40`, suelta al cambiar de acción en la fase 4 |
| `c_59d470.asm`, `c_59c0d0`, `c_8a0fc0_update_register.asm` | tick de cada ranura y su registro |
| `c_59e150.asm`, `c_5a12a0.asm`, `c_5a1530.asm`, `c_5a4760.asm` | vuelo, fase 2, enganche (fase 3) |
| `c_5abbc0_rope_start.asm`, `c_58a1c0_phase1to2.asm` | creación de la cuerda (fase 1) y llamada de vuelta del efecto |
| `c_5aeaa0_slot_post.asm`, `c_500730_socket_transform.asm` | colocación de la cuerda por fotograma y sockets |
| `c_593b50.asm` | lanzamiento local (incluye id de escena → pieza en `0x5947AD`) |
| `c_5970c0_vf52_pullforce.asm`, `c_5a68d0_vf51_pull.asm`, `c_5b1800.asm`, `c_1190fd0.asm`, `c_117f0c0.asm`, `c_117f7e0.asm`, `c_11930a0.asm`, `c_1180ea0.asm` | tirón, script del cliente y sus sueltas |
| `c_87f750.asm`, `c_59f760.asm`, `c_5a0770.asm`, `c_58b8f0.asm`, `c_6d2090.asm`, `c_807c80.asm`, `c_7f88b0.asm` | resto de llamadores y frame events |
