# Prueba del 7 oct: copias que faltan, pose A antes de pelear y copias sin punto en el minimapa

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA y solo
valen para esta build. Investigación de solo lectura (7 oct 2026). No se ejecutó el juego ni se tocó
`src/`.

Datos:
- anfitrión (H): `bin64\cdmp.log` (MARK 1 05:07:47.631, MARK 2 05:16:08.633) y
  `bin64\cdmp_packets_20261007-050207.bin`;
- invitado (G, Shadow, fase 5 con `suppress=1`): `build\research\shadow-20261007\cdmp.log` y
  `cdmp_packets_20261007-040239.bin`.

Reloj: hora de G + 1 h ≈ hora de H − 0,6 s (ajustado con las inyecciones `ActorLoadDataAck` de las
copias y con el golpe de `0xB0100BF9` de §1.2). Las horas de captura se pasaron a reloj de pared con
dos anclas: en H la carga del jugador inyectada (`delivered opcode 2005 (12673 bytes)` 05:04:25.638),
en G la de la copia `0xB01F0001` (04:05:28.458).

Scripts: `build\research\p_copias7oct\` (`capx.py` lee y cachea las dos capturas; `cargas_cerca.py`,
`cargas_hostiles.py`, `lista_gimmicks.py`, `acciones.py`, `reposo_por_origen.py`, `keyh.py`).

Marcas: **[verificado]** (log, captura o desensamblado), **(hipótesis)**.

---

## 0. Resumen

| # | Síntoma | Causa | Confianza |
|---|---|---|---|
| A | 2 enemigos del grupo de la MARK 2 no se ven en G | `0xB0100BF9` (Blinding Arrow 52666) y `0xB0100C1E` (52669) **no se copiaron nunca**: el saneador los rechaza porque su carga nombra un **gimmick acoplado a la mano** (`Game Mechanic`, tipo 7). Los dos pelearon, y el segundo fue el último que mató H | **alta** [verificado] |
| A' | Un enemigo que no estaba en la misma posición en la pelea 1 | No falta ni sobra ningún actor en esa pelea: los 4 bandidos se copiaron, G ocultó su propio grupo y las posiciones coinciden. Lo más probable es la **postura**: las copias estaban de pie en pose A, y los reales en su pose del schedule | media (hipótesis) |
| B | Pose A hasta que empieza la pelea | Las copias **sí** reciben la acción de reposo del real. Pero los enemigos de **schedule** (bandidos, Blinding Arrow) reposan con una acción **propia de cada elemento del schedule**, y a la copia le quitamos el schedule (`strip_spawn_identity`). Los de campamento o sin origen reposan con `wait_default` | media-alta (hipótesis con datos) |
| C | Las copias no tienen punto rojo en el minimapa | El icono de un NPC vivo de tipo 3 sale de condiciones de batalla, aggro y relación con el jugador local (`0x9E8D10`). Una copia controlada por red nunca entra en batalla ni tiene aggro (`copia-objetivo-sin-ataques.md` §7.2) | media (hipótesis; el arreglo no depende de ella) |

---

## 1. A: copias que faltan

### 1.1 Pelea 1: los bandidos (H 05:10:53–05:11:32, G 04:11:01–04:11:32)

Enemigos reales de H (cargados a las 05:09:47.06, schedule `0x347E8569`):

| Real | Variante | Qué es | Copia | Enviada | En G | Final |
|---|---|---|---|---|---|---|
| `0xB0100BE5` | 50002 | líder, hijos BE6–BE9 | `0xB01F0023` | 05:09:47.259, "NO action" | encolada 04:09:47.051 | muere 04:11:24.980 |
| `0xB0100BE6` | 50007 | miembro | `0xB01F0024` | ídem | 04:09:47.100 | muere 04:11:27.283 |
| `0xB0100BE7` | 50006 | miembro | `0xB01F0021` | ídem | 04:09:47.003 | muere 04:11:26.047 |
| `0xB0100BE8` | 50038 | miembro | `0xB01F0022` | ídem | 04:09:47.028 | muere 04:11:32.448 |
| `0xB0100BE9` | 2107 | **"Basic Wagon"**, tipo 5 (no es enemigo) | — | — | — | — |

- Los cuatro bandidos se copiaron. El quinto "hijo" del líder es el carro **[verificado]**.
- G retiró su propio grupo en ese sitio: `0xB0100B77` y 4 miembros, a 26 m de H, retirados a las
  04:10:10.490. También retiró `0xB0100B54` (bandidos y civiles, a 133 m) **[verificado]**.
- **Posición**:
  - las copias aparecen en la posición de la carga real (por ejemplo `0x23` en (-9650.4, 576.1,
    -4910.9) en los dos lados);
  - durante la pelea se mueven con las muestras de memoria de H;
  - en la prueba se vio que las que aparecen en los dos lados se mueven igual.
- **Gemelos de botín** (`0xB0100D76`, `D7B`, `D8E`): se ven en la captura de G, pero `loot_twin` no
  entrega su carga al cliente, así que no se ven **[verificado en `loot_twin.h`]**.
- Otros hostiles cerca: el campamento Blinding Arrow, a ~40 m al oeste (§1.2):
  - estaba en reposo: `BF9` no apunta a nadie hasta las 05:13:33;
  - de los 4 enemigos, en G solo había 2 copias (`0x25` y `0x28`), cada una a ~1 m de un real sin copia.

**Conclusión de la pelea 1.** No hay hueco del relevo. El enemigo que no estaba en la misma
posición puede ser:
- **(hipótesis principal)** la postura. Las copias estaban de pie en pose A (§2), mientras que en H
  los bandidos estaban en su pose del schedule (sentados, apoyados…);
- **(hipótesis)** el campamento de al lado. Allí G veía una copia donde H veía dos enemigos pegados:
  `BF8`/`BF9` a 0,6 m y `C1E`/`C1F` a 0,9 m.

En la próxima prueba, una captura de pantalla de cada lado (`CopyFromScreen`) antes de pelear lo
decide.

### 1.2 Pelea 2: el campamento Blinding Arrow (H 05:13:33–05:16:16, MARK 2 a las 05:16:08)

Campamento de schedule `0xAA29991F`, en (-9690, 572.9, -4904):

| Real | Variante | Copia | Qué hizo en H | En G |
|---|---|---|---|---|
| `0xB0100BF8` | 52665 (líder, hijo BF9) | `0xB01F0025` | pelea con G | G la mata (04:13:57.416); H recibe la muerte a las 05:13:57.713 |
| `0xB0100BF9` | 52666 (miembro) | **ninguna**: `05:09:47.259 enemy 0xB0100BF9 NOT copied: actor 0xB0100BFA still named at +7074` | **Pelea con G**: `CurrentTargetChangeReq` desde 05:13:33.470; frames de ataque a la copia de G a las 05:13:39.559, 05:13:51.714, 05:14:01.699, 05:14:12.928 y 05:14:25.395; 12 proyectiles; **golpe** a las 05:14:13.762 (`our enemy 0xB0100BF9 hit the other player's copy (attack 0x719C2056, 26.0 hp)`) | **invisible**. G descarta su golpe: `04:14:13.486 Co-op: the other player's enemy 0x00000000 hit us (attack 0x719C2056, -1.0 m): ignored, its attacker is not a copy of their enemies here` |
| `0xB0100C1E` | 52669 (líder, hijo C1F) | **ninguna**: `05:09:48.791 enemy 0xB0100C1E NOT copied: actor 0xB0100C20 still named at +7335` | **pelea con H** (H aprende sus ataques 05:14:41–05:15:00); muere a las **05:15:05.747** (`NotifyHandleDeadAck`) | invisible |
| `0xB0100C1F` | 52671 (miembro) | `0xB01F0028` | pelea con los dos | G la mata (04:15:36.643) |

- **El último enemigo que mató H** es `0xB0100BF9`: `NotifyHandleDeadAck` a las **05:16:16.575**,
  8 s después de la MARK 2 **[verificado]**.
- Los dos que faltan en G son `BF9` y `C1E`. Son exactamente los "1 o 2 enemigos" del informe.
- En el segundo campamento (schedule `0xD35B348B`, 90 m al sur) pasa lo mismo:
  - `0xB0100D9D` (52669, rechazado, `D9F` en `+7475`);
  - `0xB0100DFE` (52666, rechazado, `DFF` en `+7214`);
  - sus compañeros 52671 y 52665 sí se copiaron (`0x2E`, `0x30`).

### 1.3 Causa: la lista de gimmicks acoplados de la carga [verificado]

En la carga de un NPC hay una lista de longitud variable, unos 1000 B antes del final:

```
03 04 FF FF FF FF | u32 n | n × u32 clave de gimmick | 24 B a cero | FF FF 00 00 …
```

Ejemplos:
- `BF8` (n = 0): `03 04 FF FF FF FF 00 00 00 00` y luego 24 ceros;
- `BF9` (n = 1): `03 04 FF FF FF FF 01 00 00 00 FA 0B 10 B0` y luego 24 ceros. El resto de la
  carga es igual, desplazado 4 B.

Los cuatro rechazos de la sesión son esta lista. Son también **los únicos 4 de 96 cargas hostiles de
tipo 3** con `n > 0` (`lista_gimmicks.py`):

| Enemigo | Variante | Gimmick | Offset |
|---|---|---|---|
| `0xB0100BF9` | 52666 | `0xB0100BFA` | +7074 |
| `0xB0100C1E` | 52669 | `0xB0100C20` | +7335 |
| `0xB0100D9D` | 52669 | `0xB0100D9F` | +7475 |
| `0xB0100DFE` | 52666 | `0xB0100DFF` | +7214 |

El gimmick:
- es una carga propia de tipo 7, variante 70001, "Game Mechanic", de 6515 B, sin padre;
- llega en el mismo instante que su dueño y en su misma posición;
- en su carga se leen `"Gimmick_Hand_L_00_Socket"`, `"InteractionPivot"`, `"RemoteCatchPivot"`,
  `"Gimmick_OBB_Trigger"` e `"InteractionUI"`: es un **objeto en la mano izquierda**;
- se mueve con su dueño: 373 `MoveActorReq` de `BFA`, los mismos que nombran a `BF9`
  (`keyh.py`).

En G pasa lo mismo: sus 52666 y 52669 nativos (`0xB0100C53` → `C54`, `0xB0100C39` → `C3B`, …) llevan
su propio gimmick.

En el código, `sanitize_load()` (`enemy_relay.cpp:426`):
- quita el padre, los hijos, el grupo y `Target+0x198`;
- después `residual_key_locked()` encuentra `0xB0100BFA` en `g_known` (tipo 7) y devuelve
  `still named` → `g_refused` (línea 554);
- el enemigo **no se reintenta nunca**: `g_refused` solo se vacía con su `RemoveActorAck`.

Mientras tanto, G retira sus propios nativos equivalentes, porque son de zona (regla de oro):
- el líder 52669 `0xB0100C39` y el 52665 `0xB0100C52` (04:10:23–24);
- sus miembros, entre ellos el 52666 `0xB0100C53`.

Resultado: dos enemigos **atacan desde la nada**.

### 1.4 Descartado [verificado]

| Posible causa | Por qué no |
|---|---|
| Tope de 24 copias (`kZoneMax`) | Solo se llegó a 24 a las 05:07:41 (0x18–0x1A), antes de la MARK 1, y se vació a las 05:08:04 al viajar H. En la zona 2 hubo como mucho ~13 a la vez |
| Ritmo de carga en G (8 de golpe, luego 2/s) | Las tandas de 5 de las 05:09:47 cargaron en 150 ms |
| Cuarentena de claves | Ningún `refused`. Los ids `0x26`, `0x27`, `0x2D` y `0x2F` no faltan por cuarentena: los gastaron los rechazos, porque `alloc_copy_key_locked()` se llama antes de `sanitize_load()`. Es inofensivo |
| Muertos o retirados antes de tiempo | `BF9`/`C1E` vivos hasta 05:16:16 y 05:15:05. Las copias de los demás duran hasta su muerte o retirada en H |
| Nativos de G sin ocultar | En la zona 2 todos los hostiles de tipo 3 cercanos se retiraron (`B77`, `B54`, `B91`, `B97`, `C39`, `C52`, `103B`, `116C`, `11D4`). Los guardias de torre `A02`/`A05` (padre gimmick) estaban a 200 m y se fueron a las 04:10:05 |
| `0xB0100A0D` (Hernandian Guard) sin copia mientras su líder y su compañero sí | Variante 500069, en `kExcludedVariants`. G tampoco oculta el suyo (regla de oro intacta) |

### 1.5 Hallazgo lateral: G oculta miembros que H nunca copia [verificado en código y log]

`native_suppress.cpp` `decide()` (línea 229):
- un registro cuyo padre está retenido se retira **sin evaluar el predicado del propio miembro**;
- en cambio, H solo copia los miembros que dan `FollowParent`, es decir, de tipo 3 y facción hostil
  (`is_zone_enemy_locked`).

Ejemplo: `04:10:08.673 dropped 0xB0100B58 "Pailune Refugee"` y tres `"Stranger"`, miembros del
bandido `0xB0100B54` (facción 0).

En G desaparecen los civiles o rehenes de un grupo hostil, y H no los manda. No es lo que se reportó,
pero rompe la regla de oro. Hay que decidir:
- si se envían como copias de civiles;
- o si se acepta que desaparezcan. El líder retenido no puede tener hijos cargados sin él
  (`copias-grupo-invisibles.md`).

### 1.6 Arreglos para A

1. **(alta) Quitar la lista de gimmicks en `sanitize_load()`**, entre el paso 3 y el 4, antes de
   reescribir la longitud:
   - buscar `03 04 FF FF FF FF` seguido de un `u32 n` con 1 ≤ n ≤ 8;
   - comprobar que las n claves siguientes son actores conocidos de tipo 7 (`g_known[k] == 7`);
   - poner `n = 0` y borrar los 4·n bytes de claves (los 24 B de ceros que siguen se quedan);
   - registrar "N docked gimmicks removed" en la línea `sanitized:`.

   `check_spawn()` en G no necesita cambios. Efecto esperado: `BF9`/`C1E`/`D9D`/`DFE` se copian, sin
   el objeto de la mano. Un detalle: al entrar en combate, el cliente de G pide los gimmicks de las
   copias a **su** servidor (`OnDemandEquipDockingGimmickSpawnReq` con `0xB01F0024`/`21`/`23` a las
   04:10:54–56 y `0x25`/`0x28` a las 04:13:36–37 [captura]). No recibe respuesta y la copia queda sin
   el objeto: solo es cosmético.
2. (media) **Retirar `g_refused` para los fallos de saneado que se pueden corregir** y reintentar
   cuando el enemigo pase a `g_engaged`. Registrar con `warn`, no `info`, el rechazo de un enemigo de
   zona: es un enemigo invisible para G.
3. (baja) Asignar la clave de copia después de sanear, o devolver el id si se rechaza: así no se
   gastan ids.
4. (más adelante, si importa el objeto) Relevar también la carga de tipo 7 del gimmick con su propia
   clave de copia y su referencia traducida. Hace falta que `check_spawn` acepte un segundo registro
   de tipo 7 hijo de la copia, y mover el gimmick con su dueño.

---

## 2. B: pose A hasta que empieza la pelea

### 2.1 Qué recibieron las copias [verificado]

- Las copias de zona se mandan unos 200 ms después de que llegue la carga real, antes de que la IA
  de H haga su primera acción:
  - con "NO action": `0x17`, `0x1C`–`0x25`, `0x2E`, `0x30`;
  - con 1 acción: `0x14`–`0x16`, `0x18`–`0x1B`, `0x28`–`0x2C`, `0x31`.
- Las acciones de reposo **sí llegan en vivo** (`acciones.py`):

  | Real (H) | Primera `StartActionReq` | Copia (G) | `StartActionAck` inyectado |
  |---|---|---|---|
  | `BF8` | 05:09:47.275, acción `0xF35F9FEE` | `0x25` | 04:09:47.376, la misma |
  | `BE5` | 05:09:47.730, `0x6A703B9F` | `0x23` | 04:09:47.677, la misma |
  | `BE7` | 05:09:47.730, `0x83332911` | `0x21` | 04:09:47.677, la misma |
  | `BE6` | 05:09:47.730, `0x0FEF2E31` | `0x24` | 04:09:47.677, la misma |
  | `BE8` | 05:09:47.730, `0xC45DA4D3` | `0x22` | 04:09:47.694, la misma |

  Después, el real repite la misma acción (`+4 == +8`) cada 2–20 s, y la copia también la recibe
  (por ejemplo `BE7` a las 05:09:50.309 → `0x21` a las 04:09:50.370).
- **La contención de `horse_diag` no es la causa.** El contador de actualizaciones saltadas sube 35
  justo después de las cargas (52 → 87, 04:09:47–49) y luego queda **plano** durante el minuto previo
  a la pelea (87 → 89 a las 04:11:26). La copia no está saltándose el update.

### 2.2 El reposo de un enemigo de schedule no es `wait_default` [captura]

`reposo_por_origen.py` mira la primera acción de cada NPC hostil de tipo 3 de la sesión de H, antes
de entrar en batalla, según su origen:

| Origen de la carga | Primera acción `wait_default` (`0x26107D3C`) | Otra |
|---|---|---|
| Schedule (S) | 4 | **48** |
| Nodo de facción, campamento (F) | 32 | 12 |
| Sin origen (–) | 12 | 0 |

En los de schedule, la acción de reposo depende **del elemento del schedule, no de la variante**:
- la variante 53221 usa `0xBA97CAC7` en un schedule (`0x79AF6B66`) y `0xAA8595D3` en otro
  (`0x57360156`);
- los 4 bandidos de un mismo grupo usan 4 acciones distintas (tabla de §2.1).

Son las poses del secuenciador (sentado, apoyado, de guardia…; `ids-enemigos-verificacion.md`
§3: `actionpointinfo`, "NPC de schedule sentados o apoyados"). Las copias de esta prueba que se vieron
en pose A (bandidos y Blinding Arrow) eran todas **de schedule**, y `strip_spawn_identity()`
(`enemy_relay.cpp:276`) les quita el hash del schedule y el id del elemento (`T0+42`, `T0+46`).

**Hipótesis B1 (principal).** La acción de pose del elemento solo tiene animación con el contexto
del schedule (o de su punto de acción), y a la copia se lo quitamos. Por eso se reproduce sin pose:
la copia queda en la pose de referencia. Al empezar la pelea llegan acciones del gráfico normal
(`0xC2B2D85B`…, 05:10:54) y se anima bien.

**Hipótesis B2.** Aunque tuviera el schedule, una copia en modo de control 0 no lo ejecuta (su
secuenciador no corre). En ese caso tampoco bastaría con dejar el schedule.

### 2.3 Algo más que la copia no recibe [captura]

- El cliente de G manda **`SetQuestDialogReq` (2857) por cada copia cada ~535 ms, sin parar**: 11.818
  peticiones en la sesión, contra 594 `SetQuestDialogAck`. Ejemplos:
  - `0xB01F0021`: 683 veces, de 04:09:47 a 04:16:06;
  - `0xB01F0028`: 735 veces.
- En H el real lo manda **una vez**, unos 1,3 s después de la carga, y su servidor contesta en
  15 ms: `Req f8 0b 10 b0` → `Ack 04 01 00 10 a0 f8 0b 10 b0` (`04 | jugador | clave`).
- El servidor de G no conoce la clave y nunca contesta. **(hipótesis)** El NPC se queda en un estado
  de "esperando diálogo" que puede afectar a su inicialización.
- Además, la clave de la copia sale hacia el servidor de G unas 2 veces por segundo por copia. Con
  la clave también salen `UpdateGimmickReq` (513), `GimmickExecuteFrameEventReq` (428) y
  `GamePlayTriggerInOutReq` (546) que nombran copias. Ver `pendiente-robustez.md`.

### 2.4 Arreglos y pruebas para B

1. **Prueba barata (decide B1).** En la próxima sesión, mirar en G antes de pelear:
   - una copia de **campamento** (Hernandian Guard, origen F, reposo `wait_default`): ¿se anima?;
   - una copia de **schedule** (bandido): ¿pose A?

   Si la primera se anima y la segunda no, se confirma B1.
2. **Arreglo A (anfitrión, recomendado).** Para un enemigo cuyo origen era un schedule
   (`strip_spawn_identity` devolvió 1), mientras no esté en `g_engaged`:
   - recordar su **acción de pose**: la de su primera `StartActionReq` con `+0x04 == 0`;
   - en las acciones guardadas (`g_actions`) y en las que se relevan, cambiar esa clave por
     `wait_default` `0x26107D3C`, tanto en `+0x04` como en `+0x08`;
   - si no tiene ninguna acción al enviarla, añadir una `StartActionReq` sintética: la plantilla es
     la última del mismo enemigo, o una `wait_default` de cualquier NPC de la sesión con la clave y la
     posición reescritas, y `+0x04 = 0`, `+0x08 = 0x26107D3C`.

   Con eso la copia queda de pie y en reposo, sin la pose sentada, en lugar de en pose A.
3. **Prueba B (fidelidad).** Un interruptor de desarrollo para **no** quitar el schedule
   (`strip_spawn_identity` solo pone a cero el serial `T0+50`), con una copia manual (Ctrl+F6) de un
   enemigo de schedule en reposo:
   - si la pose real aparece, B1 se confirma y B2 queda descartada;
   - riesgos: el secuenciador de G puede reclamar el elemento, y el nativo de G para ese mismo
     elemento está retirado. Hacerlo primero con `cdmp_peer` en un solo PC.
4. **Contestar `SetQuestDialogReq` en local.** En el gancho de envío de G:
   - si la clave es `is_enemy_copy_key`, descartar el Req;
   - inyectar una vez `SetQuestDialogAck` 2948 = `04 | jugador propio | copia` (9 B, plantilla de la
     §2.3).

   Así se corta la fuga y se descarta la hipótesis del diálogo pendiente.
5. (baja) En la zona, no enviar una copia hasta que su real tenga una primera acción, o hasta 1 s
   después de su carga. Así nunca llega "NO action".

---

## 3. C: el punto rojo del minimapa

### 3.1 Cómo decide el juego el icono [verificado, desensamblado de `0x9E8D10`]

`0x9E89C0(comp, u16* icono, u16* sub, u8* a, u8* b)`:
- es el gancho que ya usa `puppet_minimap.cpp`;
- obtiene el jugador y el usuario locales y llama a `0x9E8D10(comp, jugadorLocal, arg3, icono, …)`.

Para un actor **vivo** de tipo `[[actor+0x88]+1] = 3`:

1. **Visibilidad** (`0x9E8E07`):
   - exige el bit `0x4000000` de `[[[comps+0x58]+0x60]+0x38]` (datos de la IA);
   - si `Status+0x4A0 & 1`, añade una comparación de facción;
   - después oculta si `[comps+0xB0]+0x3DC` (congelado), si `Status+0x390|0x394 ≥ 0x80` o si
     `Status+0x341`.
2. `*arg8 = 0x2215750(npc, jugadorLocal, 0)` (`0x9E8D97`), una relación entre los dos actores.
3. Si `Status+0x273` (muerto), sigue la rama `0x9E9406`: `cfg+0xFC`/`+0xFE` según una consulta del
   componente `[comps+0xC8]` (`0x2145830` → `.xtls 0xE4A7EE0`, que recorre listas de
   `CharacterInfo`). **(hipótesis)** son los iconos de cadáver con botín.
4. Si está vivo, `switch` por tipo (tabla `0x9EA07C`); el tipo 3 va a `0x9E9754`:
   - `cfg+0x112` (`0x9E97DB`) si `*arg8 == 0`, el tipo de orden de la IA
     `[[[comps+0x58]+0x50]+0x40] != 3` y `CharacterInfo(npc)+0x1E5 ∈ {1, 2}`;
   - si no, `cfg+0x110` (`0x9E9842`) si `[[arg3.comps+0x120]+0x18] == 1` **y**
     `u32 [[npc.comps+0x128]+0x1C] != 0`;
   - si no, `cfg+0x114` (`0x9E988E`) o el icono propio `CharacterInfo+0x1E6` (tiendas, PNJ con
     conocimiento…);
   - si no, `0xFFFF`: sin icono.

### 3.2 Por qué la copia no lo tiene (hipótesis)

La copia es de **otro usuario** (`+0x90 = 0x9010F001`), así que su estado de control `byte
[A+0xC8]` es 0. Según `copia-objetivo-sin-ataques.md` §7.2 [verificado allí]:
- `IsInBattle(copia)` va por `C+0x164 = 0`;
- `AddAggro` exige `vf188(5)`, así que no tiene aggro;
- `0x22218F0` (inicio o fin de batalla) **no se dispara nunca**;
- además, la sonda midió que el tipo de orden de la IA de una copia vale 2, y no 0/1 como en un real.

Las ramas de "enemigo" del §3.1 dependen de ese estado: batalla o aggro en `comps+0x128`/`+0x120`,
la relación de `0x2215750` y el tipo de orden. **Lo más probable** es que el real de H reciba
`cfg+0x110` o `+0x112` por estar en batalla o ser hostil al jugador, y que la copia caiga en `0xFFFF`.
Para saber qué rama exactamente hace falta el diagnóstico de §3.4; el arreglo no depende de él.

Relevar `CurrentTargetChangeReq` como `CurrentTargetChangedAck` (receta en
`enemigos-del-anfitrion.md` §3.3) daría objetivo a la copia, pero **no** aggro ni batalla. Lo más
probable es que no baste.

### 3.3 Cómo funciona hoy el icono del compañero

- Gancho en línea en `exe+0x9E89C0` (`kMinimapIconDecide`).
- Solo para `0xA010F001` contesta sin llamar al original: `*icono = cfg[+0x124]`, `*sub = 0xFFFF`,
  `a = b = 0`. Así evita además el `HideFlagMinimapActor` del original.
- El juego crea el icono al refrescar (slot 24) y lo mueve cada frame (`0x9E7C60`, recalcula
  mientras `+0x34 & 6`). Con `RemoveActorAck` (slot 36) lo quita.

### 3.4 Arreglo

1. **Diagnóstico (solo lectura), en los dos lados.** Ampliar `minimap_icon()`:
   - para cada actor de tipo 3 hostil, llamar al original y registrar, cuando cambie, la clave, el
     icono/sub/a/b devueltos, `Status+0x273`, `Status+0x340`, `[[[comps+0x58]+0x50]+0x40]`,
     `u32 [[comps+0x128]+0x1C]` y el byte del bit `0x4000000`;
   - en H, para los reales en reposo y en pelea, da la `UIMapIconKey` del punto rojo y su offset
     (`+0x110`/`+0x112`);
   - en G, para las copias: se espera `0xFFFF`.
2. **Arreglo.** En `minimap_icon()`:
   - si `is_enemy_copy_key(clave)` y el offset aprendido no es 0, contestar sin el original, como con
     el compañero;
   - si la copia está viva (`Status+0x340 == 0`, `+0x273 == 0`) y no está oculta
     (`Status+0x390|0x394 < 0x80`), devolver `icono = cfg[offset del enemigo]`, `sub = 0xFFFF`;
   - si no, `0xFFFF`. Al morir, el recálculo quita el punto.
   - Configuración `[coop] enemy_minimap_icon` (offset; `-1` = diagnóstico).
   - Opcional: mostrarlo solo si el real está peleando, con un bit "engaged" que H ya calcula
     (`g_engaged`) y mandaría en `EnemyState`. Así imitaría a H si allí el punto solo sale en batalla.

---

## 4. Orden recomendado

| Paso | Qué | Confianza | Coste |
|---|---|---|---|
| 1 | Quitar la lista de gimmicks acoplados en `sanitize_load()` (§1.6.1) | alta | bajo |
| 2 | `warn` y reintento al pelear para los rechazos de zona (§1.6.2) | alta | bajo |
| 3 | Icono de enemigo para las copias en el gancho del minimapa, con diagnóstico (§3.4) | alta (el arreglo) / media (la causa) | bajo |
| 4 | Reposo `wait_default` para las copias de schedule (§2.4.2), tras la prueba barata §2.4.1 | media-alta | medio |
| 5 | `SetQuestDialogReq` de copias: descartar y contestar en local (§2.4.4) | media | bajo |
| 6 | Prueba de mantener el schedule (§2.4.3) | por confirmar | medio |
| 7 | Miembros no hostiles retirados en G y no copiados en H (§1.5) | — | decisión de diseño |
