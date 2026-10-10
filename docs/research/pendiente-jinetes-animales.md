# Pendiente: copias de jinetes, monturas, animales y enemigos de varios actores

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`)
y solo valen para esta build. Investigación de solo lectura:
- capstone sobre el exe en disco (`.xtls` con `horse2/xtrace.py`);
- **las 50 capturas** que hay (`bin64\cdmp_packets_*.bin` de hoy y `D:\cdmp-grabaciones`): 5,69 h de juego;
- los logs del mod en `captures/`;
- `characterinfo` extraído en `build/research/ids/`.

El juego no se ejecutó y no se tocó el código del mod. Scripts en `build/research/p_riders/` (§9).

Marcas:
- **[verificado]**: leído en el desensamblado o en el código del mod (se da la dirección o el archivo);
- **[captura]**: medido en las capturas o en los logs (se da el script);
- **[inferido]**: deducción; la §7 dice con qué prueba se confirma.

"Único" = un actor por clave y por captura (primera carga). Se excluyen las claves de red nuestras
(`0xB01F…`, `0xB010F00x`, `0xA010F001`, `0x9010F001`).

---

## 0. Resumen

1. **Lo que hay no es lo que temíamos** [captura, `classify.py`, `cats.py`]:
   - en 5,69 h, **143 actores pelearon** con el jugador o la marioneta. 135 eran personas de tipo 3,
     3 eran de tipo 4 y 5 eran objetos de tipo 7;
   - **ningún animal peleó**. Hay 26.654 animales únicos (tipos 3, 5 y 6): 6 recibieron golpes y
     ninguno atacó;
   - **ningún jinete peleó**. Hay 237 jinetes, y solo 4 son de facción hostil;
   - **1 tirador en torre peleó**. Hay 72 tiradores en torre, 54 de ellos de facción hostil.

   Los jinetes y animales hostiles (lobos, osos, bandidos montados en lobo, cañones) existen en
   `characterinfo`, pero **no aparecen en ninguna captura** (zona de Hernand).
2. **Cómo se enlaza un jinete con su montura** [verificado + captura]:
   - **En la carga**, los dos registros se nombran mutuamente en cinco sitios:
     - la cola del actor: el jinete tiene `+0xAC` = montura; la montura tiene `+0xB0` = [jinete];
     - **el bloque de `ClientVehicleActorComponent`** (`vf11` `.xtls 0xE6E3810`, 167 B). En el
       jinete, `L+0x10` = montura y `L+0x14` = asiento. En la montura, `V+0x48[asiento]` = jinete
       y `V+0xC8` = número de jinetes;
     - `Target+0x198`;
     - un campo de cabecera de la montura;
     - y, en los grupos de campamento, la Party.

     **El vínculo de monta viaja en la carga.** Lo que le faltaba a nuestro caballo copia, aquí lo
     trae el propio registro.
   - **Orden:** la montura llega antes que el jinete en el 100 % de los casos (237/237), en
     paquetes separados de la misma ráfaga (< 3 ms).
   - **En ejecución:**
     - el jinete manda sus `StartActionReq`/`MoveActorReq` con el padre = montura (`+0x116`/`+0x8C`);
     - la montura manda su propio tráfico (en 194 de 231, ≥ 5 acciones y ≥ 5 Move).
   - **Al morir el jinete**, el servidor manda **`RideOffByServerAck`**
     (`montura | jinete | asiento | 0 | 8`) 33 ms después de `NotifyHandleDeadAck`.
   - **Al desmontar por IA**, no hay ningún mensaje: solo cambia el padre de sus acciones a `FF…`.
3. **Las "jinetes de carro" de `copias-grupo-invisibles.md` §6.4 no son carros** [captura]:
   - son **tiradores (arco o mosquete) montados en torres**: gimmicks de tipo 7 con
     `TowerDetectRange`, `RideLimitTrigger` y `ProjectilePivot_*`;
   - suben con `InteractionPreemptionStep`/`DoStep`;
   - el nombre de la torre sigue en 6-7 campos de la carga del tirador, además de los conocidos.
4. **Los animales**:
   - El tipo de registro es **fijo por variante** (983 de 983) [captura]:
     - **tipo 6**: fauna ambiental (pájaros, insectos, peces, domésticos), 135 variantes. Va en
       bandadas padre/hijos;
     - **tipo 3**: animales "con cuerpo": ciervos, jabalí, cucús, serpientes, arañas, cuervo hambriento;
     - **tipo 5**: monturables (toros salvajes, animales de carga y silla, caballos, carros).
   - **Ya se han copiado 26 animales de tipo 3 en las pruebas, y los 26 hicieron login** (`+CF 1`),
     con 1-2 saltos de contención [captura, `copylog.py`].
   - **Se copiaban por error.** El modo automático exige "apuntó al jugador", y **4.138 animales
     apuntan al jugador para huir**.
   - **La señal buena es `ChangeBattleStateReq` con estado ≠ 0**: la mandan 133 de las 135 personas
     hostiles y **1 de 26.654 animales** [captura, `battle.py`].
5. **Recomendación:**
   - **Ya:** cambiar la selección automática a "estado de batalla ≠ 0, o golpe real". Seguir
     rechazando jinetes y tiradores en torre, pero con un log que diga la categoría.
   - **Después**, si hace falta:
     1. **pareja jinete+montura "visual"**, cada uno solo y sin vínculo. Es la receta del caballo de
        la fase 1, que ya funciona;
     2. **como experimento**, la **pareja enlazada nativa**: las dos claves traducidas en ambos
        registros, con aparición atómica y `RideOffByServerAck` relevado.
   - **Torres:** no hasta tener mapeados sus campos.
   - **Animales hostiles:** se copian con la receta de tipo 3 actual, cuando aparezcan.

---

## 1. Qué hay en las capturas

### 1.1 Categorías [captura, `cats.py`, 50 capturas, 5,69 h]

| Categoría | Únicos | Facción hostil¹ | Pelearon² | Solo víctima | Apuntaron sin pelear |
|---|---|---|---|---|---|
| Animal tipo 6 | 23.957 | 0 | **0** | 3 | 3.499 |
| Persona sola (3) | 3.719 | 652 | 60 | 54 | 142 |
| Animal tipo 3 | 1.958 | 0 | **0** | 3 | 428 |
| Persona miembro de grupo (3 hijo de 3) | 1.444 | 500 | 40 | 39 | 98 |
| Persona líder de grupo (3 padre de 3) | 872 | 255 | 34 | 16 | 48 |
| Montura o animal tipo 5 sin jinete | 566 | 0 | 0 | 0 | 214 |
| **Jinete (3 sobre 5)** | **237** | **4** | **0** | 0 | 7 |
| Montura con jinete (5 padre de 3) | 231 | 0 | 0 | 0 | 5 |
| Animal de tiro (5 hijo de un carro 5) | 94 | 0 | 0 | 0 | 7 |
| **Tirador en torre (3 sobre 7)** | **72** | **54** | **1** | 0 | 12 |
| Carro (5) sin jinete de tipo 3 | 39 | 0 | 0 | 0 | 0 |
| "MON" tipo 3 (flores explosivas) | 18 | 0 | 0 | 0 | 0 |

¹ `rec+0x1A` ∈ {BleedBlade `1000016`, Bandit `1000128`, WolfPursuer `1000018`, Antumbra `1000024`}.

² "Pelearon":
- `VaryHPAck` del jugador o la marioneta con fuente X;
- `AttackTargetAck` con atacante X sobre ellos;
- o `DoFrameEventReq` de X con ellos como objetivo (`fights.py`).

**Los 4 jinetes hostiles** son Bleed Bandits montados (Lumif o Stefero de silla, Lumiph) dentro de
campamentos [captura]:
- la cadena es **líder (3) → montura (5) → jinete (3)**: la montura es hija del líder;
- el jinete está en la Party del campamento.

### 1.2 Qué jinetes y monturas hay [captura, `links.py`]

| Monturas (tipo 5) | Jinetes |
|---|---|
| `Animal_Lumif/Stefero/Stefano_Domestic_Saddle`, `Riding_Horse_Lumiph/Strapero` | Mercenarios errantes (≈ 150), guardias de Hernand (12), Bleed Bandits (4) |
| `Animal_*_Bagpack` (de carga, a veces montadas) | Mercenarios y mercaderes |
| `Riding_Wagon_*` (carros) | Cocheros y mineros. Hijos: animales de tiro `*_WagonConnecter` (5) y carga (7) |

**En `characterinfo`, sin capturar** [verificado en `ids/characterinfo_keys.tsv`]:
- monturas de combate: `Riding_Wolf` (9), `Riding_SandfangBandits_Wolf`, `Riding_Bear` (3),
  `Animal_Ride_Battle_Boar` (8), `Riding_BlackBear_Cannon`, `Riding_Machine_Bear`, `Riding_Wyvern`,
  `Riding_Dragon`, `Riding_Elephant`;
- asedio: `Riding_Cannon`, `Riding_ballista`, `Riding_FixedCrossBow`, `Riding_WarMachine_Unique`,
  `Riding_Wagon_Cannon/Ballista`;
- depredadores: `Animal_Wolf_Wild` (18), `Animal_Black_Bear_Wild`, `Animal_Tiger_Wild`, `Animal_Lion_Wild`,
  `Animal_Crocodile_Wild`, `Animal_Scorpion_Wild`, arañas `*_LandSpider_*`;
- invocados: `Mon_*_Summon`, `Animal_Wolf_Summon`, `MON_MachineTank_Summon`.

### 1.3 Ninguna señal "apunta al jugador" sirve para animales [captura, `battle.py`, `bstate.py`]

| Grupo | n | `ChangeBattleStateReq` estado ≠ 0 | `DoFrameEventReq` propio |
|---|---|---|---|
| Personas tipo 3 **hostiles** | 135 | **133** | 135 |
| Personas tipo 3 que apuntaron sin pelear | 307 | 188 | 101 |
| Animales tipo 3 que apuntaron | 428 | **1** | 3 |
| Animales tipo 6 que apuntaron | 3.499 | **0** | 49 |
| Animales tipo 5 que apuntaron | 211 | 0 | 0 |

- `ChangeBattleStateReq` = `u32 actor | u8 estado` (5 B). Los hostiles usan los estados 1 y 0.
- **El primer estado 1 llega antes** que el primer `CurrentTargetChangeReq` hacia el jugador:
  mediana 18 s, 20 de 20.
- El modo automático de hoy (`auto_tick`, `c.aimed`) solo pide "apuntó al jugador en 20 s", sobre
  cualquier tipo 3. **Por eso copió 13 "Female Deer", 5 "Baby Deer", 2 "Mole"…** [captura,
  `copylog.py`; verificado en `enemy_relay.cpp:641-646`].

---

## 2. Cómo se enlaza un jinete con su montura

### 2.1 En la carga [verificado + captura]

**Orden** [captura, script de §1 en `links.py` + inline]:
- padre antes que hijo en el **100 %**: 237/237 jinetes, 72/72 torres y 94/94 animales de tiro;
- siempre en `ActorLoadDataAck` separados, con Δt máximo de 3 ms;
- el hijo está siempre en la lista `+0xB0` del padre.

**Bloque de `ClientVehicleActorComponent`**:
- vtable `0x55B6108`, slot de componente `0x48`;
- `vf11` = `0x2227280` → `jmp .xtls 0xE6E3810`;
- tamaños desofuscados con las constantes globales en `.xtls` (`0xD04218A`, `0xA9CB85C`,
  `0x113F947C`, `0x14697A44`, `0x1412A914`, `0x105FECBC`) [verificado, `x_e6e3810.txt`].

| Bytes | Destino | En el jinete | En la montura |
|---|---|---|---|
| u32 | `L+0x10` (`L = [V+0x18]`): **vehículo que monto** | **clave de la montura** | 0 |
| u8 | `L+0x14`: asiento | 0… (`FF` = ninguno) | `FF` |
| u32 | `L+0x18` | 0 | 0 |
| 16 × {u32, u32} | `V+0x88[i]`, **`V+0x48[i]` = jinete del asiento i** | 0 | **clave del jinete** en su asiento |
| u8 | `V+0xC8`: nº de jinetes | 0 | 1 |
| u32, u32 | `V+0xCC`, `V+0xD0` | 0 | en carros, `V+0xCC` = el animal de tiro; en sillas, 0 |
| u32, u8, u8, u8, u8, u8, 12 B | `V+0x40`, `+0x44`, `+0xE8`, `+0x20`, `+0xE9`, `+0x21`, `+0x24` | — | — |

Comprobado en las capturas [`vehblk.py`]:
- `L+0x10` del jinete = montura en **237/237**;
- la montura sienta al jinete en el mismo asiento, con `V+0xC8 = 1`, en **226/237**.

Es la misma estructura que escribe `CommonVehicleActorComponent::vf53` `0x2228260`
(`caballo-coop.md` §3.2). **El cliente recibe el vínculo ya hecho en la carga**, sin interacción.

**Todas las apariciones de la clave del compañero** [captura, `xref_pair.py`, 237 parejas]:

| En el registro del… | Campo | Veces |
|---|---|---|
| jinete | cola `+0xAC` | 237 |
| jinete | Vehicle `L+0x10` | 237 |
| jinete | `Target+0x198` (el "líder" del jinete es la montura) | ≈ 226 |
| jinete | `Path.parent` de una Transform (`end-352`) | 4 |
| montura | cola `+0xB0` | 237 |
| montura | Vehicle `V+0x48[asiento]` | 226 |
| montura | `Target+0x198` | 222 |
| montura | cabecera, `fin del nombre + 172` (componente sin identificar) | 226 |
| ambos | Party (jinetes en grupo) | 4 |

Notas:
- La posición del bloque Vehicle **no es fija**: depende del tipo de actor y de la longitud del
  nombre. No hay un ancla común, porque el `u32` que lo precede cambia por actor [`anchor.py`]. Se
  localiza por contenido (`vehblk.py`).
- En el Bleed Bandit montado `0xB010019D` (120000), el localizador de Party de `enemy_relay.cpp`
  **falla**:
  - el índice propio `Party+0x28` (3) no coincide con su posición en la lista (4);
  - la lista incluye a la montura;
  - el validador final lo rechazaría igual, por las claves de los otros bandidos [captura,
    `pair_san.py`].

### 2.2 En ejecución [captura, `runtime.py`, `tl.py`]

Ejemplo, 120000, montura `0xB010019C` (Lumif de silla) y jinete `0xB010019D`:

```text
105.563  ActorLoadDataAck montura (tipo 5) ; ActorLoadDataAck jinete (tipo 3)
105.772  StartActionReq montura padre FF ; StartActionReq jinete padre FF   (primera acción, +0,21 s)
105.804  StartActionReq jinete padre = montura
107.037  MoveActorReq montura (-10235,4 618,7 -3494,6) padre FF
107.039  MoveActorReq jinete  (-10235,4 620,4 -3494,7) padre = montura    (+1,7 m en Y, misma ms)
```

- El jinete lleva el padre = montura en sus Req; la primera, de "aparición", va con `FF`.
- **Las posiciones del jinete son las de mundo**, sobre la silla, y salen a la vez que las de la
  montura. **La montura tiene IA propia y su propio tráfico.**
- **En combate a caballo** (065037, un mercenario contra Bleed Bandits):
  - el jinete ataca con el padre = montura y su objetivo en `+0x16C`;
  - **la montura también recibe golpes**: `AttackerAck` y `OnPostBuffDamagedAck` sobre la clave de
    la montura.

### 2.3 Muerte, desmonte y retirada [captura]

**Muerte del jinete** (065037, el mercenario `0xB010025E` sobre el Lumiph `0xB010025D`):

```text
144.617  S>C VaryHPAck jinete hp 0 ; NotifyHandleDeadAck jinete (asesino 0xB0100198, motivo 8)
144.650  S>C ClearAllyGroupDataAck montura
144.650  S>C RideOffByServerAck   5d0210b0 5e0210b0 00 00 08   (vehículo | jinete | asiento 0 | 0 | 8)
144.673  C>S StartActionReq jinete padre FF (cae) ; la montura sigue con su IA
148.912  S>C RemoveActorAck montura (motivo 42)                ← la montura se va antes
164.085  S>C RemoveActorAck jinete (motivo 25, cadáver)
```

Es el **único** `RideOffByServerAck` de las 50 capturas. Su manejador `0xB59C20` llama a RideOff
`0x2228E40` si `V+0xEB` y el asiento cuadran (`0x2227FA0`, mutada) [verificado en
`caballo-coop.md` §3.3].

**Desmonte por IA** (112627, un minero de carro que se encara con el jugador):
- **ningún Ack**; el padre pasa de la montura a `FF` en el siguiente Move;
- la IA del cliente ejecuta el desmonte, que es **autoritativo del cliente**;
- 7 casos en total, ninguno de enemigos.

**Retirada** [`runtime.py`]:

| Caso | Veces |
|---|---|
| Jinete y montura en el **mismo** `RemoveActorAck` (motivos 25, 30, 36, 19, 80, 82, 28) | 172 |
| Montura antes que el jinete (muerte del jinete o motivo 42) | 4 |
| Jinete antes | 1 |
| Solo uno de los dos | 2 |
| Siguen al acabar la captura | 58 |

### 2.4 Tiradores en torre (tipo 3 sobre 7) [captura]

- **La torre:**
  - es un gimmick genérico `Object_Gimmick_Base_70001` con `TowerDetectRange`, `RideLimitTrigger`,
    `ProjectilePivot_*` e `InteractionPivot_*`;
  - **no manda acciones** (0 `StartActionReq`, 1 Move);
  - nombra al tirador en una lista `03 04 FF FF FF FF | u32 n | n × clave` (en `end-505`/`end-767`).
    Es el mismo patrón que la lista de objetos acoplados, como el casco de un mercenario.
- **Los tiradores:**
  - suben con `InteractionPreemptionStepReq`/`DoStepReq`: 28 y 9 veces con la torre, Wolf
    Pursuers con mosquete;
  - luego mandan sus Req con el padre = torre.
- **La carga del tirador nombra a la torre** en la cola, en `L+0x10` y en la Transform. Además,
  en **6-7 campos más** (`+220/+232/+240` de cabecera, `end-210/212`, `end-559/561`, `end-929`…).
  - `tower_san.py` intenta dejarlo "solo": `Path.parent` → `FF×8` copiando la Transform de mundo en
    la local, Vehicle a 0/`FF`, `Target` y Party;
  - aun así **quedan restos en 72 de 72**. Sin mapear esos campos, la torre no se puede quitar con
    seguridad.

---

## 3. Animales

### 3.1 Tipos y grupos [captura, `animals.txt`]

- **Tipo 6** (23.957):
  - cuervo, cotorra, loro, paloma (70 %), insectos, peces, ganado y mascotas;
  - **bandadas padre/hijos (6→6)**: 9.575 hijos, de 2 a 28 por grupo; ovejas y cabras hijas de
    pastores (6→3).
- **Tipo 3** (1.958):
  - `KukuBird` (en bandadas con un `Animal_Boss_Cucubird` líder, 3→3), ciervos, luciérnagas,
    cuervo hambriento, sapo, garza, jabalí (`Animal_Wild_Boar_Wild`, 32), serpientes
    (`Bad_Rattlesnake`, 15), arañas (`Bismuth_LandSpider`, padre de un gimmick 7);
  - `StoneLampExplosion_Spider`; burros y toros de carga con mochila (padres de gimmicks 7).
- **Tipo 5 sin jinete** (566): toros salvajes (`Female/Baby/Bull_Wild`: 299), animales de carga y
  de silla sueltos.

**Inferido:** los depredadores que no hemos visto (lobo, oso, tigre) serán de tipo 3, como el
jabalí y el ciervo. Los monturables de combate (`Riding_Wolf`, `Animal_Ride_Battle_Boar`) serán de
tipo 5, como `Animal_Bull_Wild` y `Riding_Horse_*`. La regla "tipo fijo por variante" es
[captura]. No se encontró el campo de `characterinfo` que da el tipo: no hay ningún byte en offset
fijo que lo prediga.

### 3.2 Animales ya copiados en vivo [captura, `copylog.py` sobre `captures/*.log`]

| Nombre (todos tipo 3) | Copias | Resultado |
|---|---|---|
| Female Deer, Baby Deer, Deer | 19 | 19 × `+5E 0x10 login +CF 1` |
| Mole, Iguana, Heloderma Lizard | 5 | 5 × login |
| Hungry Crow (`Animal_Crow_Wild_30121`) | 1 | login, `Status+0x390 = 0x1000` |
| Stone Lantern Spider | 1 | login |

- Todas pasaron por 1-2 saltos de contención (`HorseDiag … skipped`) y ninguna cerró el juego.
- **Los animales de tipo 3 se copian ya con la receta actual.**
- Los de tipo 6 y 5 no se han probado nunca, porque `candidates()` solo admite el tipo 3.

---

## 4. Otros enemigos de varios actores

| Caso | Qué se ve [captura] | Efecto en la copia hoy |
|---|---|---|
| **Armas y escudos "on demand"** (escudos de torre incluidos: `Spine2_B_Shield_ChildSocket`, `LHand_Socket`) | Cargas de tipo 7 tras `OnDemandEquipDockingGimmickSpawnReq` (124 de 189 cargas que nombran a un hostil). **Nombran al enemigo** (`+234`/`+260`), pero el enemigo no las nombra | No bloquean la copia. En el invitado, la copia pide sus armas a su propio servidor: **38 `OnDemandEquipDockingGimmickSpawnReq` y 5 `OnDemandDockingGimmickSpawnReq` con claves `0xB01F…`**, y llegan 26 `RegisterLoadGimmickFromActorImpulseNak` con claves de copia [captura, capturas de hoy] |
| **Objetos acoplados en la carga** (casco `Bip01 T Head`, mochilas, `CatchGimmick`) | Lista `03 04 FF…FF n × clave` en la carga del portador; hijos 7 de personas (227) | **El validador los rechaza**: "actor 0xB0100174 still named", un mercenario con casco, y "0xB01001CF", un toro con mochila (log `cdmp_sanitized_test.log`) |
| **Invocados** | Solo `GimmickExecuteSummonCharacterFrameEventReq` y `ChangeSummoneeToIndependentByInitTriggerReq` **entre gimmicks** (tipo 7 → 7). Ningún enemigo invocado | — |
| **Asedio** (`Riding_Cannon`, `ballista`, `FixedCrossBow`) | Ninguno | Serían "jinetes" de un vehículo de tipo 5 [inferido] |
| **Torres de tiradores** | 72 (§2.4) | Rechazados ("rides a mount or cart") |

---

## 5. Enfoques

| # | Enfoque | Qué hace | Riesgos | Esfuerzo |
|---|---|---|---|---|
| **A** | **Rechazar y registrar** (casi como hoy) | Jinetes, monturas con jinete y torres no se copian. Log con la categoría | El invitado no ve al enemigo montado, aunque le pegue (2 PC) | Mínimo |
| **B** | **Pareja visual sin vínculo** (como el caballo, fase 1) | Copia de la montura **sola** (asientos a 0, `V+0xC8 = 0`, cola sin hijos, Party sola) y del jinete **solo** (`L+0x10 = 0`, asiento `FF`, cola sin padre, `Target+0x198` propio). En los mensajes relevados, el padre del jinete se mapea a la copia de la montura (ya lo hace `map_field_locked`). Posiciones de los dos desde la memoria del anfitrión, en el mismo instante | Sin IK de silla; cápsulas que chocan (caballo-coop §6). Cada copia hace su login por separado. Sin asientos, la montura no entra en la rama de vehículo de `0x925CE0` [inferido]. `RideOn::Enter` ya se fuerza para copias (`owner == 0x9010F001`, `remote_puppet.cpp`) | Bajo-medio: `pair_san.py` (receta B) deja limpios 164/178 jinetes de silla y 59/59 de carro |
| **C** | **Pareja enlazada nativa** | Las dos cargas con **todas** las apariciones de jinete → `RC` y montura → `MC` (`replace_all`), Party sola, la cola de la montura sin padre (cadena de campamento), hijos = [`RC`]. Aparición **atómica**: montura y luego jinete en la misma tanda. Relevar `RideOffByServerAck` y retirar las dos con un solo `RemoveActorAck` | Grupo de login de dos copias (`copias-grupo-invisibles.md` §2.2): si el jinete falta o se rechaza, **la montura queda en estado 8 para siempre**. Montura de tipo 5 con asiento ocupado = rama de vehículo (el cierre `0x925F0E` del caballo); la contención lo cubre [inferido]. Riesgo de pelea entre el búfer de red y el asiento (caballo-coop §5, fase 2) | Medio-alto: mensaje de grupo (`kVersion++`), receptor con validación por grupo. `pair_san.py` (receta A): 163/178 parejas de silla limpias; 11 con restos (casco acoplado, Party) y 4 de campamento |
| **D** | **Tirador de torre solo** | Quitar la torre de la carga del tirador (Transform local := mundo, padre `FF×8`, Vehicle, Target, Party) y dejarlo de pie en la torre del invitado | **6-7 campos sin identificar** siguen nombrando la torre (72/72). Ponerlos a 0 a ciegas es una apuesta | Medio, pero con investigación previa |

**Lo que no hay que hacer:**
- vincular la copia a la torre o a la montura **reales** del invitado (`copias-grupo-invisibles.md`
  §6.1);
- copiar carros enteros: carga de tipo 7 con estado guardado y animales de tiro.

---

## 6. Plan recomendado

1. **Selección (ya)**, en `enemy_relay.cpp`:
   - "comprometido" = `ChangeBattleStateReq` con estado ≠ 0 en los últimos 20 s, `DoFrameEventReq`
     con el jugador o la marioneta como objetivo, o golpe recibido o dado;
   - **"apuntó" solo ya no basta**;
   - admitir tipos 3, 5 y 6 por esa señal. Con los datos, solo pasarán personas, y algún animal si
     de verdad ataca;
   - **efecto previsto: 0 ciervos copiados en el modo automático.**
2. **Logs de categoría (ya)**: en el rechazo, decir "jinete sobre `<variante>`", "torre",
   "acoplado `<clave>` tipo 7" o "grupo de bandada 6→6". Sirve para medir cuántos se pierden en
   partidas reales.
3. **Pareja visual B** (cuando aparezcan enemigos montados de verdad, o como prueba con
   mercenarios):
   - `send_enemy` detecta "padre de tipo 5" y manda primero la montura (sola) y luego el jinete
     (solo);
   - `g_sent` guarda las dos;
   - `enemy_host_tick` muestrea las dos en el mismo tick.
4. **Experimento C** detrás de un interruptor (`[coop] enemy_rider_link=1`):
   - nuevo `RelKind::EnemySpawnGroup` con `n` cargas y sus acciones;
   - el receptor valida **todas** antes de inyectar ninguna y admite padre o hijos **solo** si son
     claves del mismo grupo;
   - relevar `RideOffByServerAck` (2938) por `EnemyState`, con las dos claves traducidas;
   - en el anfitrión, si el padre del jinete pasa de la montura a `FF` sin `RideOffByServerAck`,
     sintetizar uno (`MC | RC | asiento | 0 | 0`) **solo si la prueba C2 muestra que el desmonte
     no desengancha por sí solo**.
5. **Torres:** mapear los campos de §2.4 (vf11 de los componentes que los leen), y luego decidir D.
6. **Animales de tipo 5 y 6:** nada hasta ver uno hostil. Si aparece:
   - **tipo 6**: la receta sola actual ya corta la bandada;
   - **tipo 5**: contención activa, rama de vehículo sin asientos.

---

## 7. Pruebas en vivo (un PC, `cdmp_peer --mode relay`)

**Preparación:**
- copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`;
- `enemy_offset_m=4`;
- sitios donde aparecen:
  - mercenarios montados en los caminos de Hernand (≈ 3 por captura);
  - Bleed Bandits montados en el campamento de Embernest, cerca de (−10.235, 618, −3.494) (120000);
  - tiradores en torre de bandidos cerca de (−10.333, 601, −3.550) (112627), y Wolf Trackers con
    mosquete (183530).

| # | Prueba | Qué mirar | Predicción |
|---|---|---|---|
| P1 | Solo lectura: log por carga de "jinete X sobre Y, asiento s" con el parser Vehicle | `cdmp.log` | ≈ 1 de cada 35 tipo 3 es jinete. `L+0x10`/`V+0x48` cuadran en ≥ 95 % [captura: 226/237] |
| P2 | Selección nueva, 10 min por el campo y una pelea de campamento con el modo automático | Copias por nombre | 0 animales copiados. Los bandidos que pelean se copian como antes |
| B1 | Pareja visual con un mercenario montado (Ctrl+F6 forzado sobre él) | `exists … +5E 0x10 login +CF 1` en las dos; `HorseDiag … ENEMY COPY` con 1-2 saltos en la montura; captura de pantalla | Las dos hacen login. El jinete va en postura de montar sobre la copia del caballo, a < 0,3 m de la silla. Puede haber vaivén por las cápsulas. **Sin cierres** |
| B2 | Matar al jinete original | Log de `NotifyHandleDead` relevado | La copia del jinete muere y cae; la de la montura sigue moviéndose y se retira con el motivo 42 antes que el cadáver |
| C1 | Pareja enlazada (interruptor) | Lectura protegida a los 2 s: jinete `[[V+0x18]+0x10]` = `MC`, `+0x14` = asiento; montura `V+0x48[s]` = `RC`, `V+0xC8` = 1; ambos `+5E 0x10` | La montura espera en estado 8 hasta que llega el jinete (≤ 1 frame) y luego hacen login juntos. Piernas en la silla, sin choque de cápsulas |
| C2 | Desmonte por IA del original (un minero al que se le provoca, o un mercenario) | Padre del jinete y vínculo en el invitado | La acción de desmontar se reproduce. Si `L+0x10` sigue con `MC` al acabar, hace falta el `RideOffByServerAck` sintético (§6.4) |
| C3 | Muerte del jinete con C | `RideOffByServerAck` inyectado → `0x2228E40` | El vínculo se borra (`V+0xC8 = 0`) y el cadáver cae al suelo |
| C4 | Rechazo atómico: corromper a propósito la carga del jinete en el receptor (modo de prueba) | Que **ninguna** de las dos se inyecte | Log "group refused", sin montura en estado 8 |

---

## 8. Preguntas abiertas

1. ¿Qué componente lee el campo de cabecera "fin del nombre + 172" de la montura? Mismo
   interrogante para los de la torre: `+220…`, `end-210`, `end-559`, `end-929`. Los lee algún
   `vf11` del principio del registro. Hay que identificarlos por el orden de componentes del actor
   (`actor+0x78`).
2. ¿El desmonte por IA, reproducido como `StartActionAck` en modo 2, desengancha el vínculo
   (RideOff `vf8` `0x683820`, mutado)? Lo decide C2.
3. ¿La copia de una montura de tipo 5 **con asiento ocupado** pasa la rama de vehículo de
   `0x925CE0` sin cierre una vez tiene acción? La contención solo cubre el tiempo sin acción.
4. ¿Qué tipo de registro tienen los lobos, osos y monturas de combate? Hace falta una captura en
   una zona con ellos: Pailune/desierto, o los Sandfang Bandits.
5. Las peticiones de armas "on demand" que manda la copia en el invitado (§4): ¿crean gimmicks en
   su servidor o dan Nak? Hay que verlo en la pieza de accesorios.
6. ¿Montan los Bleed Bandits en combate, o desmontan al ver al jugador? No hay ninguna pelea
   montada en las capturas.

---

## 9. Apéndice

### 9.1 Direcciones (build 2976)

| RVA | Qué es |
|---|---|
| `0x55B6108` | vtable de `ClientVehicleActorComponent` (componente `[actor+0x68]+0x48`) |
| `0x2227280` → `.xtls 0xE6E3810` | Vehicle `vf11`: bloque de 167 B (tabla de §2.1) |
| `0xD04218A`, `0xA9CB85C`, `0x113F947C`, `0x14697A44`, `0x1412A914`, `0x105FECBC` (`.xtls`) | Constantes que desofuscan tamaños y offsets de ese `vf11` (4, 4, `+0x18`, 1, `+0xE8`, 1) |
| `0x2228260` / `0x222AFD0` | `CommonVehicle vf53` "RideOn": escribe el asiento y `L+0x10` (`caballo-coop.md` §3.2) |
| `0xB59C20` → `0x2228E40` (`0x2227FA0`, comprobación de asiento) | Manejador de `RideOffByServerAck` (2938) → RideOff |
| `0x681C80` / `0x682C20` / `0x683820` | `RideOn::Enter` (forzado para claves de usuario `0x9010F001`), `RideOff::Enter`, `RideOff` `vf8` (mutado) |
| `0x925CE0` (desde `0x85FBA0` en `0x860154`) | Rama de vehículo: cierre 3 del caballo |
| `0x17EA310` → `.xtls 0xC867CC0` | Unpack del actor (`+0xAC` padre, `+0xB0` hijos) |
| `0x8B7B50` | Bucle de pendientes: un hijo con padre existente espera a su grupo (`copias-grupo-invisibles.md` §2.2) |

**Mensajes:**
- `RideOffByServerAck` 2938 = `u32 vehículo | u32 jinete | u8 asiento | u8 0 | u8 bandera` (8 al
  morir el jinete);
- `ChangeBattleStateReq` 2257 = `u32 actor | u8 estado`;
- `StartActionReq` `+0x116` = padre (u32 + 4 B), `+0x16C` = objetivo; `MoveActorReq` `+0x8C` = padre.

### 9.2 Scripts (`build/research/p_riders/`)

Se ejecutan con `..\venv\Scripts\python.exe <script>` desde esa carpeta. Las cachés quedan en
`cache/` y `out/`; `pk.py` reutiliza las cachés de `pieza4/cache` si existen.

| Script | Qué hace |
|---|---|
| `pk.py`, `rec.py` | Lector de capturas (copia de `pieza4/pk.py`); registro de una carga y cadenas |
| `survey.py` | Todos los registros de `ActorLoadDataAck` de las 50 capturas → `out/loads_*.pkl` (tipo, variante, facción, cola, Party) |
| `fights.py`, `classify.py`, `cats.py` | Quién peleó con el jugador o la marioneta; categorías (§1.1) |
| `links.py`, `animals.txt` (salida) | Pares padre/hijo por tipo y nombre; inventario de animales |
| `runtime.py`, `tl.py <sid> <claves…>` | Padre de las Req de los jinetes, transiciones, retiradas e interacciones; línea de tiempo |
| `vehblk.py`, `anchor.py`, `xref_pair.py`, `keypos.py` | Bloque Vehicle y clasificación de cada aparición de la clave del compañero |
| `pair_san.py`, `tower_san.py` | Prototipos de las recetas B/C y D, con cuántas cargas quedan limpias |
| `battle.py`, `bstate.py` | Señales de combate por categoría; adelanto del estado de batalla |
| `accessories.py` | Cargas que nombran a un hostil: armas y escudos "on demand" y acoplados |
| `copylog.py` | Resultado de cada copia en los logs del mod, por nombre |
| `d_2227280.asm`, `x_e6e3810.txt` | Desensamblado del `vf11` de Vehicle |
