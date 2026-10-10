# El otro jugador en el minimapa y en el mapa del mundo

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`). Análisis de solo lectura con capstone sobre el exe en disco y con las capturas del
sniffer. El juego no se ejecutó.

Cada afirmación va marcada así:
- **[E]** verificado estáticamente (desensamblado);
- **[C]** visto en captura;
- **[I]** inferido; hay que confirmarlo en vivo.

Scripts de trabajo (no forman parte del repo):
- `cap.py`/`scan.py`/`show.py`: buscan opcodes en todas las `cdmp_packets_*.bin`.
- `evmap.py`: tabla "offset del objeto de eventos de UI → nombre del evento".
- `rng.py`, `mm160.py`, `flag26.py`, `invt.py`: búsquedas de instrucciones y vtables.

Se reutilizan `cddis.py`, `callers.py`, `opscan.py` y `horse2/xtrace.py` de `build/research/`.

---

## 0. Resumen

1. **Hay dos mecanismos nativos que valen. Se pueden usar a la vez.**

   **A. Pin del mapa inyectado como Ack S>C.**
   - Es solo inyección de paquetes, sin hooks.
   - **Ya está probado a medias:** en una prueba se vio el pin (rombo amarillo) en el minimapa cuando
     estaba cerca **[C]**.
   - El servidor interno no se entera, así que **no llega a la partida guardada** **[E]+[I]**.
   - Aspecto limitado: 14 formas por 15 colores, sin texto ni rotación.

   **B. Icono de actor del minimapa para la marioneta**, con la flecha del jugador o cualquier
   otra `UIMapIconKey`.
   - El minimapa ya dibuja actores mediante el componente `ClientMinimapActorComponent`
     (`[actor+0x68]+0x160`). Ese componente calcula el icono de cada actor en
     `exe+0x9E89C0`/`0x9E8D10` **[E]**.
   - **Para la marioneta ese cálculo da "sin icono" con casi total seguridad** **[I]**: es un
     actor de tipo 1 que no es el jugador local ni un mercenario propio.
   - Un hook inline al **inicio** de `exe+0x9E89C0` que devuelva un icono solo para
     `0xA010F001` hace que el propio juego cree el icono y lo mueva y gire cada frame desde el
     `TransformSync` de la marioneta. También lo quita al despawnear **[E]**.
   - La flecha del jugador local es justamente uno de esos iconos: `cfg+0x126` **[E]**.
   - Otro color exige **otra `UIMapIconKey`**: el color va en los datos del icono, no en un
     parámetro **[I]**. Hay que probar las claves candidatas en vivo (§5.4).

2. **Party y mercenario no dan icono a la marioneta.**
   - La decisión del icono no lee nunca el componente Party (`+0x108`) **[E]**.
   - Para actores de tipo 1 solo mira el clan de mercenarios local (`+0x110`), buscando por
     `CharacterKey` **[E]**.
   - Meter a la marioneta en el clan exigiría `RefreshMercenaryDataAck`/`MercenaryDataListAck`:
     escriben datos de mercenario del jugador local, con muchos efectos laterales.
     **Descartado.**
3. Alternativa C, el **destino propio** (`UpdateSelfDestinationAck` 2386):
   - 16 B; el minimapa muestra distancia y una flecha de borde **[I]**;
   - pisa el waypoint del usuario. Queda como modo opcional "seguir al compañero".

---

## 1. Pines: `CreateOrChangePinMarkerAck` / `RemovePinMarkerAck`

### 1.1 Formatos (deserializadores del cliente, confirmados con la captura `…-211630`)

**`CreateOrChangePinMarkerAck` (2824), handler `vf2 0xB9D9D0`** **[E]**:

```
+0x00 u8   ?           (el servidor envía 04; el cliente lo lee y no lo usa)          [E][C]
+0x01 u32  actorKey    (tiene que ser el jugador LOCAL, ver 1.3)                     [E][C]
+0x05 u8   grupo       0 = pin normal, 1 = pin "trace"                               [E][C]
+0x06 N × 22 B, hasta el final del payload (N = (len-6)/22):
      u64  id
      f32  x, y, z     (posición de mundo absoluta, la misma escala que actor_world_position)
      u8   c           (forma o icono)                                               [I]
      u8   d           (color)                                                       [I]
```

- Ejemplo real **[C]**:
  ```
  04 | 01 00 10 A0 | 00 | 00×8 (id 0) | 0E 5B 2A C6 | 00 00 00 00 | 38 90 89 C5 | 04 | 0E   (28 B)
  ```
- Una lectura anterior ponía el id justo tras la clave. En realidad entre los dos va el **u8 de
  grupo** (`00`). Lo confirman el segundo pin (`… A0 | 00 | 01 00…`) y el orden de lectura
  `1,4,1,{8,12,1,1}…` del handler **[E][C]**.
- El constructor del servidor (`0x29B2700`) escribe el mismo orden y parte los paquetes que
  pasan de `0x7FC1` B **[E]**.

**`RemovePinMarkerAck` (2992), handler `0xB9DF60`** **[E][C]**:

```
+0x00 u32 actorKey | +0x04 u64 id | +0x0C u8 grupo        (13 B)
```

**Req del cliente** (constructor `0xB84320`; la llama la UI del mapa en `0xD91EF0`) **[E][C]**:

```
CreateOrChangePinMarkerReq (2575, 23 B): u64 id (-1 = nuevo) | f32 x,y,z | u8 c | u8 d | u8 grupo
RemovePinMarkerReq        (2317,  9 B): u64 id | u8 grupo
```

- La UI toma `c`, `d` y el grupo de la "brocha" actual del componente:
  `ContentsMisc+0x260`/`+0x261`/`+0x262` **[E]**.
- La `y` sale a 0 al hacer clic en el mapa **[C]** y aun así el pin se ve.

### 1.2 Qué hace el cliente

`0x495460(ContentsMisc, out, grupo, lista)` **[E]**:
- Lista del grupo `g` = vector en `ContentsMisc + 0xC8 + 16·g`: `{ptr, u32 count, u32 cap}`,
  con entradas de 24 B `{u64 id, vec3, u8 c, u8 d, pad}`.
- Para cada pin:
  - si el id **no está**, lo añade;
  - si **ya está**, solo actualiza `c` y `d`: **la posición guardada en la lista no cambia**.
- Después lanza el evento de UI `CreateOrUpdatePinMarkerIcon(u64 id, float3 pos, u8 c, u8 d,
  bool grupo)`, en el offset `+0x618` del objeto de UI `[[exe+0x6D69190]+0x80]`, **con la
  posición nueva** **[E]**.
- `0x50ACF0(actor)` reemite **todos** los pines desde las listas, con su posición guardada.
  Lo llama `0xE13C40`, la (re)inicialización de la UI del HUD y el mapa **[E]**.
  **Consecuencia:** si solo se reenvía Create con el mismo id, el icono se mueve, pero tras
  recrear la UI vuelve a la posición vieja. → **Para mover hay que mandar Remove + Create**
  (§1.6).
- Remove (`0x495700`) quita la entrada y lanza `RemovePinData(UIMapIconKey, id)` (`+0x650`).
  La clave de icono es `cfg+0xEA` en el grupo 0 y `cfg+0x16C` en el grupo 1 **[E]**.

### 1.3 Puertas y condiciones de crash

| Comprobación (cliente) | Efecto |
|---|---|
| `p->size-5 == len`, y el deserializador consume exactamente `len` | Si no, el Ack se descarta en silencio **[E]** |
| El actor de `+0x01` existe | Si no, no hace nada **[E]** |
| `0x8B53C0`: actor "aún no listo" | Reintento (cola `ClientNet+0xF0`) **[E]** |
| `byte[[actor+0x88]+1] == 1` (tipo jugador) **y** `byte[actor+0x96] != 0` | Si no, no hace nada **[E]**. `+0x96 = 1` solo lo pone `0x46B271`, al crear el personaje propio: es la marca "personaje local" **[E]+[I]** |
| **grupo**: sin comprobación de rango | `0xC8 + 16·g` con `g ≥ 2` lee y escribe **fuera** de las dos listas, es decir, corrompe memoria. **Usar solo 0 o 1** **[E]** |
| id | Sin comprobación de rango; solo se compara por igualdad. Cualquier u64 es seguro **[E]** |
| `c`, `d` | El cliente no los valida **[E]**. El servidor exige `c < 14` (grupo 0), `c == 0` (grupo 1) y `d < 15` (`0x28C36B0`). Fuera de ese rango la UI podría indexar mal: **respetar esos rangos** **[I]** |

- **No uses la clave de la marioneta**: sería ignorada por `+0x96 = 0`.
- **Usa siempre `0xA0100001`**, el jugador local.

### 1.4 Ids, límites y significado de `04 0E`

- Límites en el servidor (`0x28C36B0`) **[E]**, según un flag global `byte [exe+0x6CE0868]`
  que solo se lee (no se ve quién lo escribe):

  | Flag | Grupo 0 | Grupo 1 (trace) | id |
  |---|---|---|---|
  | ≠ 0 | 500 pines (`0x1F4`) | 20 pines (`0x14`) | índice del primer bit libre (bitsets `+0x278` y `+0x2B7`) |
  | = 0 | 15 pines | no se permite | `(byte<<32) \| índice` |

  - La captura da ids 0, 1 y luego 0 otra vez: es el más bajo libre, coherente con flag ≠ 0
    **[C]**.
  - Al llegar al máximo, el servidor **desaloja el más antiguo** **[E]**.
- El id de la Req se ignora: el servidor **siempre** asigna uno nuevo **[E]**.
- `04` = `c` y `0E` = `d` **[C]**. Por los rangos (`c < 14`, y solo 0 en trace; `d < 15` en
  los dos grupos), lo más probable es que **`c` sea la forma o el icono (14)** y **`d` el color
  (15)** **[I]**. Esto se confirma con la captura de §6, prueba P0.
- El primer byte del Ack (`04`) no lo usa el cliente **[E]**; mandar `04` como el servidor.
- El cliente no tiene límite de cantidad **[E]**. Un pin falso puede sumar en el contador de
  la UI del mapa (`UI_Worldmap_PinMarkCount`) y bloquear "poner pin" si el usuario ya está en
  el máximo **[I]**.

### 1.5 ¿Va a la partida guardada?

**No, si solo se inyecta el Ack** **[E]+[I]**:
- El componente del cliente (`ClientSelfContentsMiscActorComponent`, vtable `0x5582268`) tiene
  stubs en los slots de carga y guardado.
- El del servidor (`ServerContentsMiscActorComponent`, vtable `0x5B1D318`) los implementa:
  `slot10 0x28B7360` lee y escribe `+0xC8`/`+0xD8` con reflexión y SQL
  (`PinMarkerIconSaveData` dentro de `ContentsMiscSaveData`) **[E]**.
- Al cargar, el login (`0x28F31F0`) reenvía solo los pines del servidor (dos llamadas a
  `0x29B2700`). El pin falso desaparece al recargar la partida **[E]**.
- Coincide con lo que dice Glint-Spotter: "por la Req son persistentes"
  (`troc-bdo-y-runtime.md`).
- **Nunca inyectar `CreateOrChangePinMarkerReq`**: crearía un pin real y persistente.

**Riesgos residuales** **[I]**:
- Si el usuario borra el pin falso en el mapa, el cliente manda `RemovePinMarkerReq(id
  falso)`. El servidor no lo encuentra (`0x28C3B80`), no responde con Ack y el pin sigue.
  Es inofensivo.
- Si "edita" el pin, sale una Req con `id` −1: crea un pin **real** nuevo. Es lo mismo que
  poner un pin a mano.

### 1.6 Receta del pin del compañero

- **id propio**, fuera del rango del servidor: `0x7FDD00000000F001` (nunca −1).
- **Grupo 0** (pin normal, ya visto en el minimapa). El grupo 1 ("trace", `c = 0`) solo como
  prueba P1.
- **Crear o mover**, a 1–2 Hz y solo si se movió más de 3 m. Los dos Ack van juntos y en ese
  orden, en la cola de inyección S>C actual de `OnRecvPacket`:
  ```
  2992 RemovePinMarkerAck  (13 B): 01 00 10 A0 | id | 00
  2824 CreateOrChangePinMarkerAck (28 B): 04 | 01 00 10 A0 | 00 | id | x y z | c | d
  ```
  - La posición es la del compañero (`actor_world_position` o la del último Move recibido).
  - Usa una `y` real para que el icono de altura sea correcto.
  - La primera vez basta con Create.
- **Quitar**: `RemovePinMarkerAck` con el mismo id y grupo.
  - Al desconectar, al despawnear la marioneta y en el apagado del mod. En el apagado, antes de
    quitar los hooks de red, mientras la cola aún se procesa.
  - Si no da tiempo, se pierde solo hasta recargar: no hay persistencia.

**Antes de inyectar, el mod debe comprobar**:
1. que el actor `0xA0100001` existe (gestor `[[exe+0x6D69188]+0x38]`), con `byte[[a+0x88]+1]
   == 1` y `byte[a+0x96] != 0`;
2. que `p = [[a+0x68]+0x168]` no es nulo y `[p] == exe+0x5582268`;
3. que el grupo es 0 o 1, `c < 14` (o 0 en el grupo 1) y `d < 15`;
4. que la longitud del payload es exactamente `6 + 22·N` (Create) o 13 (Remove).

Todas las lecturas, con `VirtualQuery`/SEH.

---

## 2. Cómo decide el juego qué actores salen en el minimapa

### 2.1 Piezas

| Pieza | Dónde | Qué hace |
|---|---|---|
| `ClientMinimapActorComponent` | vtable `0x55BEC90`, slot `0x160` (índice `0x2C`) | Lo crea **para todo actor in-game** el constructor común de componentes `0x8A9090` (`0x8A9A8C`, sin condición) **[E]** |
| `+0x18 u16` | | Icono actual (`0xFFFF` = ninguno) **[E]** |
| `+0x34`, `+0x35` | | Bits: `+0x34&2` = tiene icono (lo pone la creación, `0x9EAF77`); `+0x35&4` = activado por la UI (`0x50A310`); `+0x35&8` = sucio **[E]** |
| `+0x38..+0x3D`, `+0x40` | | Último resultado y marca de tiempo **[E]** |
| `0x9E8930` → `.xtls 0x9C61DD0` | "Refresh" | Llama a `0x9E89C0` y luego a `0x9EA6F0`. Lo invocan el slot 24 (`0x9EBA20`) y `0x50A1B0(actor)` (14 llamadores, por ejemplo al desmontar) **[E]** |
| `0x9E7C60` (slot 31) | Tick | Si tiene icono, cada frame: `UpdateMinimapActor(icono, clave, pos, yaw)`. La pos sale de `TransformSync 0x4F8220` con el sector ×`0x5D736F4`, y el yaw de `0x17E9170`. Además **recalcula el icono** mientras `+0x34&6` **[E]** |
| `0x9E89C0(comp, u16* icono, u16* subicono, u8* a, u8* b)` | Cálculo | Obtiene el jugador y el usuario locales (`0x8B44E0`/`0x8B4570`) y llama a `0x9E8D10` **[E]** |
| `0x9EA6F0(comp, icono, sub, a, b)` | Aplicar | Con icono nuevo y sin icono previo: pone `+0x34|=2`, registra el componente para el tick (`0x4FF1F0`) y lanza `CreateMinimapActor(icono, clave, nombre, esYo, …)` (`+0x620`). `esYo = clave == [[[exe+0x6D69190]+0x58]+0x20]`. Si cambia el icono: `ChangeMinimapActor`. Si pasa a `0xFFFF`: `RemoveMinimapActorByActorKey` (`+0x630`) **[E]** |
| slot 36 → `.xtls 0x9C5E5E0` | Despawn | `0x9EA6F0(comp, 0xFFFF, 0xFFFF, …)`, es decir, quita el icono **[E]** |
| `UIGamePlayControlRootMiniMap::vf171` = `0xE09FC0` | UI | Crea el widget según `MapIconType` (`info+0x2C`). Para algunos tipos mete la clave en sus conjuntos (`+0x1B8`/`+0x1D8`) y activa el componente del actor con `0xE12CD0` → `.xtls 0xABF15E0` → `0x50A310` **[E]** |

Tabla de eventos de UI (`[[exe+0x6D69190]+0x80]+off`, sacada de `0x7C7240`) **[E]**:

| Offset | Evento |
|---|---|
| `+0x150` | `SetSelfDestinationMark(float3)` |
| `+0x610` | `CreatePinIcon` |
| `+0x618` | `CreateOrUpdatePinMarkerIcon` |
| `+0x620` | `CreateMinimapActor` |
| `+0x628` | `HideFlagMinimapActor(ActorKey)` |
| `+0x630` | `RemoveMinimapActorByActorKey` |
| `+0x638` | `ChangeMinimapActor` |
| `+0x640` | `UpdateMinimapActor` |
| `+0x648` | `UpdateMinimapActorAdditionalIcon` |
| `+0x650` | `RemovePinData` |
| `+0x658` | `UpdateMinimapActorIconOpacity` |
| `+0x670` | `UpdateMinimapActorDuplcateTargetIcon` |
| `+0x6E0` | `LevelGimmickMapIconUpdated` |

Son colas con lock (`exe+0x6D76338`) que la UI HTML consume por nombre.

### 2.2 El árbol de decisión (`0x9E8D10`)

`cfg = [[[[exe+0x6D69528]]+8]+0x270]` es una tabla de `UIMapIconKey` u16. Con el flag TLS
`+0x1EC`, el código usa `exe+0x6D69530` en vez de `0x6D69528` **[E]**.

1. **Visibilidad** **[E]**:
   - Si `actor+0xA0 == [[[exe+0x6D69190]+0x30]+0x58]` (dueño = usuario local) y
     `byte[actor+0x94]`, es visible.
   - Si no:
     - tipo 4 → visible;
     - tipos 1, 3, 5, 6, 8 y 9 → exige el bit `0x4000000` en
       `[[[[actor+0x68]+0x58]+0x60]+0x38]` (componente IA), y luego una comparación de facción
       si `Status+0x4A0&1`;
     - tipo 7 → visible.
   - Después oculta si `Sequencer(+0xB0)+0x3DC`, `Status+0x390|0x394 ≥ 0x80` o
     `Status+0x341`. El camino de "oculto" lanza `HideFlagMinimapActor`.
   - Ese bit solo se usa aquí en todo el exe (`flag26.py`): viene de datos.
2. **Icono** (`switch` por tipo, tabla `0x9EA07C`) **[E]**:
   - **actor == personaje local → `cfg+0x126` (la flecha del jugador).**
   - Tipo 1 (otro jugador) y 4 (mercenario):
     - busca `CharacterInfo.key` en el **clan de mercenarios del jugador local**
       (`[[local+0x68]+0x110]`, `0x214E7A0`);
     - si lo encuentra: `cfg+0x100` o `cfg+0x14E` según el estado (`0x214D130`);
     - si no, por lo general `0xFFFF`.
   - Tipo 5 (montura): con jinete sigue la ruta general. Sin jinete y si el dueño coincide con
     el del jugador local (`actor+0xA0`) → `(0x17EB150(actor))+0x92`, el **icono del
     caballo**, que sale de los datos de la montura.
   - Tipo 3 (NPC): `cfg+0x110`, `+0x112` y `+0x114`, o `CharacterInfo+0x1E6` (icono propio del
     NPC) según tienda, facción y estado.
   - Otras claves vistas: `+0xFC` y `+0xFE` (los dos con lista de objetivos), `+0x104`, `+0x108`,
     `+0x10A`/`+0x10C` (`Status+0x217 == 4`), `+0x10E`, `+0xEA` (pin), `+0x16C` (pin trace).

**La marioneta** (`0xA010F001`):
- tipo 1;
- dueño `0x9010F001`, que no es el usuario local;
- no es el personaje local;
- su `CharacterKey` (Kliff, 1) no está en el clan local.

→ **Ningún icono, o directamente oculta** **[I]**, a confirmar con la prueba P2 en modo
diagnóstico.

**La flecha propia**:
- Es un icono de actor más (`cfg+0x126`) con `esYo = 1`.
- La posición y la rotación llegan cada frame con `UpdateMinimapActor` **[E]**.
- Si el centro del minimapa se dibuja aparte, la UI lo decide con `esYo` **[I]**.

### 2.3 Party y mercenarios

**Party** **[E]+[C]**:
- `OpenPartyAck` (3013, 25 B): `líder | líder | u32 0 | u64 nº de grupo | u32 100 | u32 1 |
  u8 1`.
- `JoinPartyAck` (2425): `u8 4 | u32 miembro | u32 miembro | u32 líder | u64 nº | u32 100 |
  u32 | u8 | u32 miembros…`. Se envía uno por cada compañero `0xB0100002…4` al cargar
  **[C]**. Los NPC también forman grupos (líder `0xB0100864`, tipo `0x22`).
- El handler `0xBEF6E0` aplica sobre el componente Party (`+0x108`) con `0xA071D0`.
- La decisión del minimapa no lee `+0x108` → **ser miembro del grupo no da icono** **[E]**.

**`UpdateMercenaryPositionAck` (2164)**:
- Formato: `u32 dueño (tipo 1) | u64 nº de mercenario | vec3 | f32 | u32` **[E]**.
- Solo escribe la posición guardada de una entrada **existente** del clan local
  (`entrada+0xC8`/`+0xD4`/`+0xD8`). Sin entrada, no hace nada.
- No hay ninguna en las capturas **[C]**.
- Usarlo exigiría crear un mercenario falso (`RefreshMercenaryDataAck` con 11,6 KB de datos):
  saldría en los menús de mercenarios y llamadas, y tocaría la lógica que crasheó en
  `exe+0x1F63FE6` (`caballo-coop.md`). **No recomendado.**

### 2.4 `LevelGimmickMapIconUpdatedAck` (2979)

- Handler `0xB9CA10`: `u32 jugador | u32 | vec3 | u32 | u32 | u8` → `0x497C00(ContentsMisc, …)`
  (mutada), con comprobación de nulo.
- Es el icono de un gimmick de nivel identificado por clave estática; no sigue a un actor. Sin
  capturas. **No sirve** para un actor que se mueve **[E]+[I]**.

### 2.5 `UpdateSelfDestinationAck` (2386, 16 B) **[E][C]**

- Formato: `u32 actorKey | vec3`.
- Escribe `ContentsMisc+0x1E8` y lanza `SetSelfDestinationMark(float3)`.
- **No comprueba nulo** en `[[actor+0x68]+0x168]`: con una clave de actor sin ese componente,
  **crash**. Solo `0xA0100001`.
- Se recibe uno al cargar con el destino actual del usuario, por ejemplo
  `01 00 10 A0 | 9B 93 26 C6 97 04 3A 44 F2 F1 1D C5` **[C]**. El mod debe guardarlo para
  restaurarlo.
- Persistencia: igual que los pines, el servidor no se entera **[I]**.

---

## 3. Mecanismo A (pin): estrategia y riesgos

| Aspecto | Detalle |
|---|---|
| Hilo | La cola de inyección S>C actual; el handler corre en el hilo principal del cliente como un Ack más **[E]** |
| Frecuencia | Remove + Create a 1 Hz (2 Hz como máximo) y solo si se movió más de 3 m. Cada par lanza dos eventos de UI. El parpadeo es posible, pero no hay coste de red |
| Aspecto | `c` (0–13) y `d` (0–14); no hay texto. Elegir un color distinto del que usa el usuario |
| Fin | `RemovePinMarkerAck` al despawn, al desconectar y en el apagado. Si no se envía, desaparece al recargar |
| Crash | Solo con grupo ≥ 2 (corrupción). Con otro `actorKey` no hace nada |
| Visibilidad | En el minimapa cuando está cerca **[C]**. Mapa del mundo, siempre **[I]** (prueba P1). Si sale en el borde o la brújula cuando está lejos, está por confirmar; puede que solo el grupo 1 ("trace") |

---

## 4. Mecanismo B (icono de actor): receta

### 4.1 Hook

- Hook inline (safetyhook, **inicio de función**) en `exe+0x9E89C0`.
  - Está en `.rsrc` y no está mutado.
  - Prólogo `48 89 5C 24 08 | 48 89 74 24 10 | 48 89 7C 24 18 | 55`. Hay que comprobar estos
    13 bytes antes de instalarlo.
- Firma: `void __fastcall(ClientMinimapActorComponent* comp, u16* icon, u16* subIcon, u8* a,
  u8* b)`.
  - `b` va en `[rsp+0x28]` en la entrada, como 5.º argumento.
  - Los llamadores inicializan `*icon = *subIcon = 0xFFFF` y `*a = *b = 0` **[E]**:
    `0x9C61DD0` y `0x9E7C60`.

```
detour(comp, icon, sub, a, b):
    key = SEH{ actor = *(u64*)(comp+8); *(u32*)(actor+0x60) }   // 0 si falla
    if key == 0xA010F001 && g_minimap_override_key != 0xFFFF:
        *icon = g_minimap_override_key; *sub = 0xFFFF; *a = 0; *b = 0
        return                    // NO llamar al original: así no se lanza HideFlagMinimapActor
    return original(comp, icon, sub, a, b)
```

- `g_minimap_override_key`: al arrancar, leer `cfg+0x126` (la flecha del jugador). Ruta:
  `[[[[exe+0x6D69528]]+8]+0x270]+0x126`, todo con SEH.
- **Modo diagnóstico** (`[coop] minimap_icon=-1`): llamar al original para la marioneta y
  registrar `icon`, `sub`, `a` y `b`. Sirve para saber qué decide el juego hoy.

### 4.2 Qué hace el juego solo (todo [E])

- **Al spawnear** (refresh del slot 24 → `0x9E8930`): calcula el icono con el hook, y
  `0x9EA6F0` lanza `CreateMinimapActor(icono, 0xA010F001, …, esYo = 0)`, pone `+0x34|=2` y
  registra el tick.
- **Cada frame** (`0x9E7C60`): `UpdateMinimapActor(icono, clave, pos de mundo, yaw)` desde el
  `TransformSync` de la marioneta. **No hay que mover nada a mano.**
- **Mientras tenga icono**, se recalcula. Cambiar `g_minimap_override_key` en caliente aplica
  `ChangeMinimapActor` en el frame siguiente: así se pueden ciclar claves.
- **Al hacer `RemoveActorAck`**: el slot 36 quita el icono.
- **Ordenar el arranque así**: instalar el hook **antes** de cargar la marioneta. El hook se
  instala diferido como los demás: tras `WorldSystem` y antes de que se procese el
  `SpawnInfo`.
  - Para una marioneta que ya existe, hay que forzar un refresh: `0x50A1B0(actor)` desde el
    hilo del juego, o despawn y respawn.
  - Fuera del hilo del juego no conviene llamarlo.

### 4.3 Riesgos

- **La UI recibe un segundo icono con la clave de la flecha y `esYo = 0`** **[I]**. Si el JS
  asume que la flecha es única, puede dibujarlo mal; no debería colgar el juego.
  → Probar antes con una clave de compañero (`cfg+0x100`).
- **El hook no se ejecuta para la marioneta** si el slot 24 no se llama para actores
  remotos **[I]**. Se ve en el modo diagnóstico (no sale ninguna línea de log). En ese caso,
  forzar el refresh.
- **Llamar al original y luego sobrescribir** dejaría pasar el `HideFlagMinimapActor`
  (`0x9E9FE7`), que se lanza dentro. Por eso hay que **saltarse** el original.
- **Sin comprobar la clave**, todos los actores pasarían por el hook: el filtro por clave es
  obligatorio y barato.
- **El mapa del mundo** puede no usar estos eventos **[I]**: §6, P2. Si no aparece, el pin (A)
  cubre el mapa del mundo.

---

## 5. Qué mira el mod antes de cada cosa (resumen)

1. **Build**: PE `0x6AB28F00` y los bytes de prólogo de `0x9E89C0`; ya se hace con otras firmas.
2. **Pin**: las comprobaciones de §1.6.
3. **Icono**: SEH en `comp+8` y `actor+0x60`. `cfg` legible y clave ≠ `0xFFFF`. El hook solo
   actúa con la marioneta presente.
4. **SelfDestination** (si se usa): solo `0xA0100001`, componente `+0x168` no nulo y con la
   vtable `0x5582268`. Guardar el valor del login para restaurarlo.

### 5.4 Color distinto

El color va con la `UIMapIconKey`. Para tener la flecha en otro color hace falta otra clave
cuyo icono sea una flecha de ese color, si la tabla de datos la tiene **[I]**:
- Ciclar con una tecla las claves de `cfg+0xFC…+0x16C` (36 u16) y mirar el minimapa.
- Alternativa: **subicono** (`*sub`), un adorno extra que se manda con
  `UpdateMinimapActorAdditionalIcon` (`+0x648`). Por ejemplo, la flecha más el icono de
  compañero `cfg+0x100` como distintivo **[I]**.

---

## 6. Pruebas en vivo mínimas

**Copia de `save` antes de P1, P2 y P3**: escriben en la memoria del juego.

| # | Prueba | Cómo | Qué mirar |
|---|---|---|---|
| P0 | Tabla `c`/`d` (solo captura) | Con el sniffer, poner 3 pines cambiando **solo la forma** y otros 3 cambiando **solo el color** en el selector del mapa. Probar también un pin "trace" si la UI lo permite | En `CreateOrChangePinMarkerReq` los bytes `+0x14` (c), `+0x15` (d) y `+0x16` (grupo). Si `c` cambia con la forma y `d` con el color, se confirma §1.4. Si aparece grupo 1, el flag está activo |
| P1 | Pin inyectado | Tecla: Create (grupo 0, `c=4`, `d` distinto) en la pos del compañero y Remove + Create cada 1 s. Abrir el mapa (M), alejarse más de 300 m, recargar la partida | En el sniffer, **ninguna** Req de pin saliente. Ver si sale en el minimapa (cerca o lejos), la brújula y el mapa. Tras recargar, **el pin no está**. Repetir con grupo 1, `c=0` |
| P2 | Diagnóstico del icono | Hook en modo diagnóstico y spawn de la marioneta | En el log: `icon`/`sub`/`a`/`b` para `0xA010F001`. Lo esperado: `0xFFFF` o `a=1`. Si no sale ninguna línea, hay que forzar el refresh |
| P3 | Flecha | Override con `cfg+0x100` y luego `cfg+0x126`. La marioneta camina y gira (espejo con `mirror_offset_m=5`) | Captura con `CopyFromScreen` del minimapa (2560×1440, recortar la esquina): el icono sigue y rota. Abrir M para ver si sale en el mapa. Despawn con F10: el icono desaparece |
| P4 | Colores | Tecla que cicla `cfg+0xFC…0x16C` | Anotar qué clave da una flecha o un marcador útil y de qué color |

---

## 7. Referencias rápidas (build 2976)

| Qué | RVA |
|---|---|
| `CreateOrChangePinMarkerAck` vf2 / aplicar / evento | `0xB9D9D0` / `0x495460` / `0x4A47E0` (`+0x618`) |
| `RemovePinMarkerAck` vf2 / aplicar | `0xB9DF60` / `0x495700` |
| Reemitir pines a la UI | `0x50ACF0` (lo llama `0xE13C40`) |
| Servidor: crear pin / quitar pin / Ack / login | `0x28C36B0` / `0x28C3B80` / `0x29B2700` / `0x28F31F0` |
| Req del cliente: pin / quitar pin / destino | `0xB84320` / `0xB84550` / `0xB85340` |
| `UpdateSelfDestinationAck` vf2 | `0xB9EDE0` (`+0x1E8`, evento `+0x150`) |
| `ClientMinimapActorComponent` | vtable `0x55BEC90`, slot `+0x160` |
| Calcular icono / árbol / aplicar | `0x9E89C0` / `0x9E8D10` / `0x9EA6F0` |
| Refresh / tick / despawn | `0x9E8930` (envoltura `0x50A1B0(actor)`) / `0x9E7C60` / `.xtls 0x9C5E5E0` |
| Tabla de iconos `cfg` | `[[[[exe+0x6D69528]]+8]+0x270]`; flecha propia `+0x126` |
| Objeto de eventos de UI | `[[exe+0x6D69190]+0x80]`; jugador local `[[exe+0x6D69190]+0x58]+0x20` |
| UI del minimapa | `UIGamePlayControlRootMiniMap` vtable `0x5674E00`, vf171 `0xE09FC0` |
