# Pieza 4: por qué R1 no hizo tambalear al enemigo

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco, siguiendo los saltos a `.xtls`);
- los `staticinfo` ya extraídos (`build/research/ids/out/gd/`) y los `actionchart/*.paac` del juego,
  leídos de los PAZ sin modificarlos;
- todas las capturas (`bin64\cdmp_packets_*` y `D:\cdmp-grabaciones`), en especial la de R1,
  `20261004-114648`.

El juego no se ejecutó. Continúa [pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) §3.

Marcas:
- **[verificado]**: leído en el código o en los datos estáticos;
- **[captura]**: medido en capturas;
- **[inferido]**: deducción pendiente de prueba en vivo.

Notación: **E** el enemigo, jugador `0xA0100001`, marioneta `0xA010F001`. "AAR" =
`ApplyActionRestrictionAck` (2162), "ATA" = `PostProcessAttackResult_AttackTargetAck` (2922),
"buff" = `ApplyBuffAck` (2506), "SAR" = `StartActionReq`.

Scripts y volcados en `build/research/pieza4/react/` (§6).

---

## 0. Resumen

1. **A R1 le faltó un paquete: el `ApplyBuffAck` del buff de golpe con el mismo id de instancia
   que el AAR.** [captura + verificado; que sea la causa, inferido fuerte]
   - En las capturas, **610 de 610** AAR reales sobre enemigos llevan detrás, en el mismo lote y casi
     siempre como paquete siguiente (604 de 610), un `ApplyBuffAck` con el mismo objetivo y el mismo id
     (`buff +0x14` = `AAR +0x24`). Contando todos los objetivos, 808 de 808.
   - Ese buff es un buff de control: `CC_PushLight` (1104), `CC_PushNormal` (1105), `CC_PushHard`
     (1106), `CC_KnockDown` (1107), `CC_Pull` (1120)… Su clave de restricción coincide 1:1 con la del AAR:
     1104 ↔ `79156D1B` y 1105 ↔ `67AE8750`.
   - Las dos inyecciones de R1 fueron las **únicas** AAR sin ese buff. Las dos acabaron en `A73D6EC4`.
   - Por lo demás, el ATA y el AAR inyectados son **idénticos byte a byte** a un par real que 12 s y
     1,7 s antes hizo tambalear **al mismo enemigo**. Solo cambian el atacante o fuente (marioneta) y
     el id (§3.1).
2. **La hipótesis del sello de tiempo caducado no se sostiene para el AAR.** [verificado]
   - `+0x14` no es un tick del servidor. Es un **retardo relativo**: `fin = ahora_cliente + valor`, o
     `-1` para "sin fin".
   - Vale 0 en el 100 % de los AAR reales. Copiarlo de una plantilla vieja no cambia nada.
   - El ATA sí lleva ticks del servidor (`+0x2D`/`+0x41`/`+0x49`/`+0x51`). Pero solo se leen si
     `+0x82 == 0`, y en los golpes reales vale 1.
3. **El AAR por sí solo hace tres cosas.** [verificado]
   - Escribe la restricción en el componente de E (`[actor+0xB0]`).
   - Lanza con prioridad 6 la acción "global" `0xE69AB342`. La misma para todas las claves vistas.
   - No da duración: la restricción empieza y "acaba" en el mismo instante.
4. **El buff hace, en el cliente, lo que el AAR no hace.**
   - Vuelve a aplicar la restricción con su propio id y la duración de su `BuffData`.
   - Manda a la IA los eventos de juego `0x63` (al objetivo) y `0x64` (a la fuente).
   - Si hace falta, relanza la acción, con prioridad 4.
   - Ejecuta el resto de procesadores del buff.

   [verificado]
5. **`A73D6EC4` es la acción genérica de "fin de reacción".** [captura]
   - Sale tras 131 de 165 reacciones con AAR y también tras reacciones sin AAR.
   - En R1 el enemigo entró en el estado de golpe: interrumpió su acción y hubo
     `EchoActionSessionIDReq`. Pero no se eligió ninguna reacción concreta y el gráfico de acciones
     saltó directo a su salida.
6. **Receta (§5):** inyectar el trío real del mismo lote, en el mismo orden y en la misma pasada del
   hook:
   1. `ATA`;
   2. `AAR`;
   3. `ApplyBuffAck` del buff de golpe, con el **mismo id** que el AAR, fuente = marioneta y
      portador = E.

   **Predicción:** en ≤30 ms, `SAR prev=<actual> new=<reacción>` (por ejemplo `3C4A7F1D` para
   `41275A91`, `90A9C337` para `9683E1EB`). `A73D6EC4` no debe salir hasta 250-600 ms después.

---

## 1. El AAR en el cliente, campo a campo [verificado]

### 1.1 Deserialización

`0xB5AEE0`:
- lee `u32` E;
- llama a `0x2443CA0` → `.xtls 0xF5D9D30`;
- lee `u32` fuente (`+0x49`) aparte.

La estructura es **cable − 4**:

| Cable | Struct | Tipo | Valor en el 100 % de los reales | Qué hace `0x48B400` con él |
|---|---|---|---|---|
| `+0x04` | `+0x00` | f32×4 | Cuaternión | **Nada**: no se lee |
| `+0x14` | `+0x10` | u64 | **0** | Retardo: `fin = ahora + valor` (`-1` = sin fin) |
| `+0x1C` | `+0x18` | u64 | **1** | Va a `0x8B4790` junto a la clave del jugador local. Si es > 0 llama a `0xB55290` sobre `[mgr+0x50]`. Parece una respuesta para el jugador local, como una pausa de golpe [inferido] |
| `+0x24` | `+0x20` | u32 | `0x800000NN` | Id de instancia. Solo se **guarda** en la ranura de la restricción |
| `+0x28` | `+0x24` | f32 | = ATA `+0x60` | No se lee |
| `+0x2C` | `+0x28` | u32 | 0 | No se lee |
| `+0x30` | `+0x2C` | u32 | 0 | "Param" de la ranura (`+0x24`) |
| `+0x34` | `+0x30` | u32 | Clave | Busca `ActionRestrictionOrderInfo` (§1.3) |
| `+0x38` | `+0x34` | u32 | Hash del ataque | Petición de acción `R+0x58` |
| `+0x3C` / `+0x40` | `+0x38` / `+0x3C` | u32 | 0 / 0 | `R+0x50` / `R+0x54` |
| `+0x44` | `+0x40` | u8 | **0** | Tipo de ranura: 0 = fija; 1 = lista (`+0xC8`) |
| `+0x45` / `+0x46` | `+0x41` / `+0x42` | u8 | Nivel del golpe | `R+0x75` / `R+0x74` |
| `+0x47` | `+0x43` | u8 | 0/1 | Bit 1 de las banderas `R+0xA8` |
| `+0x48` | `+0x44` | u8 | **1** | Con 0 solo se pone la restricción; no se lanza ninguna acción |
| `+0x49` | aparte | u32 | Atacante o 0 | `R+0x18` (fuente) |

### 1.2 `0x48B400` (componente `CharacterControl+0x118`)

1. Si existe el actor del jugador local (`0x8B4700`), llama a `0x8B4790` con `+0x1C`.
2. Calcula `fin`:
   - `ahora = 0x1416B80() + [0x6D69438]`, el reloj del cliente. El hilo con la bandera TLS `+0x1EC`
     usa la base 0;
   - `fin = (+0x14 == -1) ? -1 : ahora + (+0x14)`.
3. Si E está muerto (`[[actor+0x68]+0x20]+0x273`), termina.
4. Llama a `0x182CDA0([actor+0xB0], tipo=+0x44, clave=+0x34, param=+0x30, fin, &id, 0xFFFF, 0)` →
   `.xtls 0xCA8FD40`. Con tipo 0 escribe la ranura fija:
   - `+0x08` = inicio (`ahora`);
   - `+0x10` = `fin`;
   - `+0x18` = orden (`registro+0x1C`);
   - `+0x1C` = id;
   - `+0x20` = clave;
   - `+0x24` = param.

   Con tipo 1, añade una entrada a la lista `+0xC8` y elige con `0x182CB00`.
5. Comprueba que el `vf 0x188(5)` del actor es cierto y que `+0x48 != 0`.
6. Arma la petición de acción `R` con `0x17B54D0`:
   - `{0xFFFF, registro+0x14, 0x206C220(registro+0x14)}`;
   - la fuente, el hash, los niveles y `registro+0x38`.
7. Llama a `0x207AA10([[actor'+0x68]+0x58], …, R, prioridad 6)`, donde `actor'` es E o su jinete
   (`0x17EF7D0`).
8. Si `registro+0x1E == 1` y E va montado, propaga la acción a la montura.

**Con `+0x14 = 0`, la restricción del AAR nace caducada (`fin == inicio`).** Ni el AAR ni la ranura
llevan duración.

### 1.3 Los datos estáticos de la clave (`actionrestrictionorderinfo`)

Deserializador `0x48D110` → `0x14F5E10`. Campos: `+0x00` clave, `+0x08` nombre, `+0x10` u8, `+0x14`
acción, `+0x18` acción al liberar, `+0x1C` u16 orden, `+0x1E` u8 ranura.

| Clave | `+0x14` (acción) | `+0x18` (al liberar) | `+0x1C` (orden) |
|---|---|---|---|
| `79156D1B` | `0xE69AB342` | 0 | 10 |
| `67AE8750` | `0xE69AB342` | 0 | 10 |
| `F931B81A` | `0xE69AB342` | 0 | 10 |
| `58B15AA0` | `0xE69AB342` | 0 | 2 |

- `E69AB342` no aparece en ningún `.paac`. Es una clave "global":
  - `0x206C220` la busca en la tabla ordenada `0x6F40488`, que `0x206BC20` construye con hashes de
    nombres;
  - cada personaje la resuelve con **su** gráfico de acciones.
- Por eso las reacciones varían con la variante, la dirección y el nivel. Por ejemplo, `67AE8750`
  da más de 20 acciones distintas (§4.2).
- `actionchart/common_hitaction.paac` contiene:
  - `A73D6EC4` (11 veces), `3C4A7F1D` (2), `90A9C337` (4) y `5B636249` (2);
  - las claves de restricción en su tabla de claves.

  Es el gráfico de reacciones de los humanoides. No se ha descodificado su formato.

---

## 2. Lo que hace el buff de golpe en el cliente [verificado]

### 2.1 Del cable a la instancia

- Handler `0xC13D70` → deserializador `0x244C170` → `.xtls 0xF609BB0` (cable − 4) → `0xA16F60`.
- `0xA16F60` construye la instancia con `0x21E8F10` → `.xtls 0xE6154F0`:

| Cable | Instancia | Qué es |
|---|---|---|
| `+0x04` | `+0xE8` (índice) | Buff (clave de skill/buff: 1104…) |
| `+0x0C` | `+0x144` | Nivel (1) |
| `+0x14` | **`+0x130`** | **Id de instancia** |
| `+0x18` / `+0x1C` | `+0x138` / `+0x13C` | Fuente / portador |
| `+0x29` | `+0x20` | vec3: **posición del atacante** en el golpe (coincide con el `MoveActorReq` del jugador) [captura] |
| `+0x35` / `+0x36` / `+0x37` | `+0xB5` / `+0xB4` / `+0xB7` | = AAR `+0x46` / `+0x45` / `+0x47` (610 de 610) |
| `+0x38` | `+0x148` | 1 |
| `+0x39` / `+0x41` | `+0x118` / `+0x120` | u64: tiempo de juego del servidor en ms. Iguales en el 100 % de los buffs de golpe |
| `+0x49` | `+0x140` | Buff (repetido) |

- `0xA170B0` busca en la lista de buffs (`comp+0x58`, entradas de `0x150` B) uno con el mismo id
  `+0x130`. Si existe, lo actualiza en vez de crear otro.
- Si `BuffData+0x15 != 0`, guarda el buff en la lista (`0x1ED2000`). Después llama a `0xA15420`, que
  ejecuta los procesadores.

### 2.2 El procesador `CommonDamagedActionRestrictionBuffProcessor` (vtable `0x55B8BA8`)

Los buffs 1104/1105/1106/1107/1109/1112/1120 (y 10159, 12031, 15033, 7) son
`DamagedActionRestrictionBuffData`:
- `+0x9C` = clave de restricción;
- `+0x90` = probabilidad ×10⁻⁶. En la skill 1104 vale `1 000 000` (100 %); `0x209E100` tira el dado.

| Skill/buff | Nombre | `+0x9C` |
|---|---|---|
| 1104 | `CC_PushLight` | `79156D1B` |
| 1105 | `CC_PushNormal` | `67AE8750` |
| 1106 | `CC_PushHard` | `F931B81A` |
| 1120 | `CC_Pull` | `FFCFA077` |
| 10159 | `Skill_Lunge_II` | `67AE8750` |
| 1021 / 1023 | `Damage_Percent(_IgnoreFatal)` | — (el buff de daño del mismo lote: **no** inyectar) |

`vf2` → `.xtls 0xE1943D0`:
1. Libera la restricción anterior por id: `0x182CFF0`, ranura `[actor CC+0xB0]`, clave
   `BuffData+0x9C`.
2. Llama a `0x209E760`:
   - **`0x182C4A0`** (`.xtls 0xCA852D0`) decide si hay que relanzar la acción. Devuelve 0 si la
     ranura ya tiene la misma clave, que es justo lo que dejó el AAR un paquete antes. En el flujo
     real, el buff **no** relanza la acción.
   - **`0x182CBF0`** (`.xtls 0xCA8A420`) vuelve a aplicar la restricción con el id del buff y su
     duración:
     - `BuffData+0x15 == 1` → `+0x18` ms;
     - 2 o 4 → sin fin;
     - otro valor → 0.

     Si la ranura sigue viva con la misma clave y el mismo orden, se queda con el fin más tardío.
   - **Eventos de juego a la IA** (`0x1804360`, `GameEventHandlerParameter`): `0x63` al objetivo y
     `0x64` a la fuente, con la clave y el tipo de restricción.
   - Si `0x182C4A0` lo pide, `0x207AA10` con la acción `E69AB342` y prioridad 4.
   - `CharacterControl vf 0x248` (descriptor del golpe) es `ret` en el cliente (`0x359B50`). Solo
     actúa en el servidor (`0x2A7C670`).
3. La liberación (`0x182CFF0`):
   - vacía la ranura si coincide el id;
   - vuelve a elegir con `0x182CB00`;
   - si `registro+0x18 != 0`, lanza la acción de liberación (prioridad 4) y el `vf 0x250` del actor.

   Para estas claves `+0x18 = 0`.

### 2.3 Duración [captura + inferido]

- Los buffs de golpe nunca reciben `UnapplyBuffAck`: 0 de 610. Además llevan `+0x39 == +0x41`.
  [captura]
- En el servidor son instantáneos. Es probable que en el cliente también (`BuffData+0x15` no está
  descodificado para 1104) [inferido].
- La duración del tambaleo **la da la animación**, no un campo [captura, §4.3].

---

## 3. R1: qué llegó y qué faltó

### 3.1 Bytes inyectados frente a la plantilla [captura, `r_diff.py`]

| Inyección | Plantilla (mismo hash, de `A0100001`) | Diferencias |
|---|---|---|
| 353,820, ATA `41275A91` | 341,540, **el mismo E** `B01001AC` | Solo `+0x04` (atacante) |
| 353,820, AAR | 341,541 | Solo `+0x24` (`0x8000000F` → `0x8000F000`) y `+0x49` (fuente) |
| 389,898, ATA `9683E1EB` | 388,178, **el mismo E** `B01001AA` | Solo `+0x04` |
| 389,898, AAR | 388,178 | Solo `+0x24` (`0x80000021` → `0x8000F001`) y `+0x49` |

Lo que siguió a las plantillas, y faltó en R1:

```
341.541 S>C ApplyBuffAck  ac0110b0 50040000 00000000 01000000 00000000 0f000080 010010a0 ac0110b0
                          27 0000000000000000 6c4e1fc6 436f1144 5f0c66c5 01 01 00 01
                          0ca0040000000000 0ca0040000000000 50040000          (1104, id 0x8000000F)
388.178 S>C ApplyBuffAck  … 51040000 … 21000080 010010a0 aa0110b0 27 … 05 02 00 01 … 51040000  (1105)
```

### 3.2 Lo que respondió el cliente de E [captura, `r_after.py`, `r_after2.py`]

| Caso | C>S de E en los 40 ms siguientes |
|---|---|
| Real (AAR + buff), el patrón más frecuente (238) | `EchoActionSessionID` → **`StartAIFunction`(`A17A8F3E`, `E69AB342`, `0x1C`, `E69AB342`, fuente)** → `ChangeAIState` → `StartAction` → **reacción** |
| Real sin `StartAIFunction` (85 de 610) | `EchoActionSessionID` → `ChangeAIState` → `StartAction` → **reacción** |
| R1 (2 de 2) | `EchoActionSessionID` → `ChangeAIState`×3 → `StartAction` → **`A73D6EC4`** |

- `EchoActionSessionIDReq` sale en todos los casos: el AAR **sí** cambió la acción de E.
- `StartAIFunctionReq` lleva `E69AB342` y la clave de la fuente. Sale en 525 de 610 golpes reales y en
  0 de 2 de R1. Encaja con que lo provoca el evento `0x63` del buff [inferido].

### 3.3 Las demás diferencias, descartadas

| Hipótesis | Veredicto |
|---|---|
| `+0x14` caducado, copiado de una plantilla vieja | No: es un retardo relativo y vale 0 en todos [verificado §1.2] |
| Ticks viejos en el ATA | No se leen con `+0x82 = 1` (`0x45C9B7`) [verificado] |
| Fuente = marioneta | Hay 11 AAR reales con fuente **0** (explosiones y otros NPC) y los 11 reaccionaron [captura]. La fuente no es necesaria. Que sea dañina no está descartado: control en R1c |
| Id `0x8000F000` | Solo se guarda en la ranura. No se ha visto ninguna búsqueda por id salvo la liberación desde un buff [verificado] |
| Falta el buff | Es lo único que distingue a R1 de 610 de 610 casos reales [captura] |

### 3.4 Por qué `A73D6EC4` [captura + inferido]

- **Qué es `A73D6EC4`** (165 casos en todas las capturas) [captura]:
  - 131 veces sigue a una reacción con AAR;
  - 16 veces sigue a reacciones sin AAR (`7208BF4D`, `22F21991`, `A9CD10F5`);
  - el resto, tras otras acciones o R1.
  - Dura unos 500 ms y luego E vuelve a su IA. Es la **salida genérica del estado de golpe**.
- **Qué pasó en R1** [inferido]:
  - El AAR lanzó `E69AB342` con una restricción que nace caducada.
  - Sin el buff no hubo eventos `0x63`/`0x64` ni el resto del procesado.
  - El gráfico no eligió ninguna rama de reacción y salió por su transición final.
- **Qué falta por saber**, porque está en los `.paac` o en los gráficos de IA sin descodificar: cuál de
  las tres cosas del buff es la imprescindible.
  - El evento `0x63` a la IA. La IA del cliente controla a E y ya recibió el evento del ATA
    (`ChangeAIState`×3).
  - El buff vivo en el componente.
  - Otro de sus procesadores.

  La receta las cubre las tres.

---

## 4. Estadística (todas las capturas) [captura]

### 4.1 Emparejamiento AAR ↔ buff (`r_all.py`, `r_batch.py`, `r_pair.py`)

- **Emparejamiento:** 808 de 808 AAR reales tienen un `ApplyBuffAck` con el mismo (objetivo, id) en
  ±50 ms. Sobre enemigos:
  - 610 de 610, siempre **después** del AAR;
  - 604 a distancia de 1 paquete y 6 a distancia de 2 o 3.
- **Campos fijos del buff emparejado:**
  - `+0x08 = 0`;
  - `+0x0C = 1` (5 en el buff 7);
  - `+0x10 = 0` (1 en 10159, 12031 y 15033);
  - `+0x20 = 0x27` y `+0x21 = 0`;
  - `+0x1C =` objetivo;
  - `+0x49 = +0x04`;
  - `+0x35..+0x38 = (AAR+0x46, AAR+0x45, AAR+0x47, 1)`;
  - `+0x39 == +0x41`;
  - `+0x18 =` AAR `+0x49` en 603 de 610.
- **Clave ↔ buff:**

  | Clave | Buffs (casos) |
  |---|---|
  | `79156D1B` | 1104 (216) |
  | `67AE8750` | 1105 (259), 10159 (29), 12031 (3) |
  | `F931B81A` | 1106 (38), 7 (2) |
  | `FFCFA077` | 1120 (28) |
  | `58B15AA0` | 1109 (18) |
  | `E55D252F` | 1107 (13) |
  | `5A6EBE95` | 1112 (3) |
  | `38121DC3` | 15033 (1) |
- **Los campos del AAR en los 921 AAR de 77 B:**
  - `+0x14 = 0`, `+0x1C = 1`, `+0x44 = 0` y `+0x48 = 1` en todos;
  - los ids son siempre `0x800000NN`.
- **Ticks del buff:** `+0x39` es el tiempo de juego del servidor en ms.
  - En `064320` avanza a 0,99 ms/ms, con un error de ±0,3 s.
  - En `114648` se para a ratos, quizá con el juego en pausa o en un menú. Es un reloj de juego.

### 4.2 Qué decide la reacción concreta

- La reacción no depende solo de la clave ni solo del buff. Para la misma tupla (buff, hash, niveles)
  hay varias reacciones distintas: varían según la variante, la dirección y el estado de E.
  - Ejemplo: 1105 + `10481FDD` + 2/8 → `4A641121` ×27, `5B636249` ×20, `A27C0039` ×5…
- Sí hay familias por buff:
  - 1104 → `3C4A7F1D`, `5A457397`, `898229AC` y `C07D6843` (solo con 1104);
  - 1120 → `A2192D1F` (28 de 28);
  - 1109 → `55F2E5B1` (18 de 18);
  - 10159 → `90A9C337` (29 de 29).

### 4.3 Duración del tambaleo (de la reacción a la acción siguiente; `r_dur.py`)

| Reacción | Buff | n | Mediana | Después |
|---|---|---|---|---|
| `3C4A7F1D` | 1104 | 44 | 325 ms | `A73D6EC4` |
| `4A641121` | 1105 | 31 | 286 ms | `A73D6EC4` |
| `90A9C337` | 1105 / 10159 | 17 / 29 | 301 / 434 ms | `A73D6EC4` / `A2192D1F` |
| `C07D6843` | 1104 | 13 | 576 ms | `A73D6EC4` |
| `5B636249` | 1105 | 42 | 3.860 ms (caída) | `DC4E5F5E` |
| `A73D6EC4` | — | — | ≈500 ms | Vuelta a la IA |

- La duración depende de la acción, no del buff ni de un campo del cable. Viene de la animación o del
  gráfico.

---

## 5. Receta y prueba

### 5.1 Qué inyectar por golpe de G (en H, con E vivo y el golpe no letal)

**Plantillas.** Guardar por hash de ataque el **trío del mismo lote real**:
- el ATA;
- el AAR;
- el `ApplyBuffAck` que va justo detrás del AAR, con el mismo objetivo y el mismo id.

Hoy `piece4_observe_s2c` solo guarda el ATA y el AAR. **No** guardar el buff `Damage_Percent`
(1021/1023): el daño va por `VaryStatCheatReq`.

**Orden:** en la misma pasada del hook `OnRecvPacket` (mismo fotograma), como en el servidor:
1. `ATA`;
2. `AAR`;
3. `ApplyBuffAck`.

Con ese orden, el buff no relanza la acción: `0x182C4A0` ve la clave ya puesta.

| Paquete | Campo | Valor |
|---|---|---|
| ATA | `+0x00` / `+0x04` | E / marioneta. El resto, de la plantilla (`+0x81 = +0x82 = 1`) |
| AAR | `+0x00` | E |
| | `+0x24` | `id = 0x8000F000 + n` (rango propio) |
| | `+0x49` | Marioneta |
| | `+0x14 = 0`, `+0x1C = 1`, `+0x44 = 0`, `+0x48 = 1` | Sin tocar (los de la plantilla) |
| `ApplyBuffAck` (77 B) | `+0x00` | E |
| | `+0x04`, `+0x49` | Buff de la plantilla (1104/1105/…) |
| | `+0x08` / `+0x0C` / `+0x10` / `+0x20` / `+0x21` | De la plantilla (`0` / `1` / `0` / `0x27` / `0`) |
| | **`+0x14`** | **El mismo `id` que el AAR** |
| | `+0x18` | Marioneta (= AAR `+0x49`) |
| | `+0x1C` | E |
| | `+0x29` | f32×3: posición de la marioneta en el mundo de H (la del atacante) |
| | `+0x35` / `+0x36` / `+0x37` / `+0x38` | AAR `+0x46` / `+0x45` / `+0x47` / `1` (ya coherentes en la plantilla) |
| | `+0x39` y `+0x41` | Iguales: el último tick de servidor visto en un `ApplyBuffAck` real (`+0x39`) más el tiempo transcurrido. La ruta de la restricción no lo lee [verificado], pero mejor que sea reciente |

Notas:
- El cliente busca buffs por id (`0xA170B0`). No reutilizar un id mientras pueda seguir vivo. Con
  `0x8000F000 + (n & 0xFFF)` no choca con los del servidor (`0x800000NN`).
- Los buffs de golpe nunca reciben `UnapplyBuffAck` del servidor, así que no queda nada pendiente.

### 5.2 Prueba R1b (un PC, `cdmp_peer` en eco; copia de la partida antes)

| # | Qué | Esperado en ≤30 ms (C>S de E) | Si falla |
|---|---|---|---|
| **R1b** | Trío ATA + AAR + buff (§5.1) sobre un enemigo que no esté ya tambaleándose | `EchoActionSessionID`. Probablemente `StartAIFunctionReq(A17A8F3E, E69AB342, 0x1C, E69AB342, A010F001)`. `ChangeAIState`. **`StartActionReq prev=<actual> new=<reacción>`**: con `41275A91`/1104, `3C4A7F1D` (lo que hizo `B01001AC` a 341,541) u otra del grupo 1104; con `9683E1EB`/1105, `90A9C337` (lo de `B01001AA` a 388,178). Después, a los 250-600 ms, `prev=<reacción> new=A73D6EC4` | Si vuelve a salir `A73D6EC4` directamente, el buff no basta: pasar a R1c |
| R1c | Igual, con AAR `+0x49 = 0` y buff `+0x18 = 0` (hay precedente real con fuente 0) | Como R1b | Si R1c funciona y R1b no, el problema es la marioneta como fuente |
| R1d | Control: repetir sin cambios un trío real reciente sobre el mismo E (fuente `A0100001`, id nuevo) | Reacción | Si tampoco funciona, el problema es la vía de inyección (orden o fotograma), no los campos |

**Señal de que el buff entró:** el `StartAIFunctionReq` con la fuente `A010F001`.

**Alternativa si la inyección no bastara:** no conviene llamar a funciones del cliente
(`0x48B400`, `0x209E760`) desde nuestro hilo:
- habría que hacerlo en el hilo de red, dentro del hook, con objetos del juego construidos a mano;
- la inyección ya va por ese mismo camino, con el mismo hilo y el mismo orden que los paquetes
  reales.

Antes habría que descodificar el gráfico (`common_hitaction.paac`) o la IA (el evento `0x63`).

---

## 6. Scripts y volcados (`build/research/pieza4/react/`)

Ejecutar con `build/research/venv/Scripts/python.exe`. Usan `../pk.py`.

| Script / fichero | Qué hace |
|---|---|
| `r_all.py` → `r_all.csv` | Todos los AAR de 77 B: campos, buff con el mismo id y 3 `StartActionReq` siguientes |
| `r_list.py <sesión>` | AAR de una sesión con las acciones siguientes del objetivo |
| `r_win.py <sesión> t0 t1 [clave…]` | Paquetes de una ventana que contienen una clave |
| `r_diff.py` | R1: bytes inyectados frente a la plantilla y el buff que faltó |
| `r_batch.py`, `r_pair.py` | Emparejamiento AAR ↔ buff (orden, distancia, campos fijos) |
| `r_buffs.py` | Buffs de golpe con y sin AAR del mismo id |
| `r_after.py`, `r_after2.py` | Respuesta C>S del enemigo en 40 ms (`StartAIFunction` y demás) |
| `r_dur.py` | Duración de cada reacción y acción siguiente |
| `r_a73d.py` | Contextos de `A73D6EC4` |
| `r_unapply.py` | `UnapplyBuffAck` (ninguno) y ticks del buff |
| `r_fields.py` | Campos fijos del AAR por clave |
| `r_paac.py` → `paac/` (100 MB, solo lectura del PAZ) | Extrae `actionchart/*.paac` y cuenta hashes |
| `c_*.asm` (`.rsrc`), `x_*.asm` / `f_*.asm` (`.xtls` o funciones relacionadas) | Volcados de las funciones citadas |

### Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x2443CA0` → `.xtls 0xF5D9D30` | Deserializador del AAR (cable − 4) |
| `0x182CDA0` → `.xtls 0xCA8FD40` | Escribe una restricción (ranura fija o lista `+0xC8`) en `[actor+0xB0]` |
| `0x182CBF0` → `.xtls 0xCA8A420` | Aplica una restricción desde un buff (duración y conservación del fin) |
| `0x182CFF0` | Libera una restricción por id; acción de liberación `registro+0x18` |
| `0x182C4A0` → `.xtls 0xCA852D0` | ¿Relanzar la acción? (0 si la ranura ya tiene la clave) |
| `0x182CB00` | Elige la restricción activa de la lista |
| `0x182D8D0` | Quita una entrada de la lista por id |
| `0x48CA70` / `0x48D110` | `ActionRestrictionOrderInfo` (tabla `0x6D6D758`) |
| `0x206C220` / `0x206BC20` | Clave global de acción → índice (tabla `0x6F40488`) |
| `0x207AA10` | Lanza una acción (petición `R`, prioridad) |
| `0x17B54D0` | Construye la petición de acción `R` |
| `0x17EF7D0` | Actor o jinete que ejecuta la acción |
| `0x8B4700` / `0x8B4790` | Actor del jugador local y uso de AAR `+0x1C` |
| `0x1416B80` (+ `[0x6D69438]`) | Reloj del cliente |
| `0xC13D70` | Handler de `ApplyBuffAck` |
| `0x244C170` → `.xtls 0xF609BB0` | Deserializador del buff (cable − 4) |
| `0xA16F60` | Aplica el buff: busca por id en `comp+0x58`; guarda si `BuffData+0x15 != 0` |
| `0x21E8F10` → `.xtls 0xE6154F0` | Instancia del buff desde el cable (`+0x130` = id) |
| `0x55B8BA8` | vtable de `CommonDamagedActionRestrictionBuffProcessor` |
| `0x209E100` | Su vf1: tirada de probabilidad (`BuffData+0x90`) |
| `.xtls 0xE1943D0` | Su vf2: libera la anterior y aplica la nueva |
| `0x209E760` | Restricción, eventos de IA `0x63`/`0x64` y acción (prioridad 4) desde el buff |
| `0x58FDD90` | vtable de `DamagedActionRestrictionBuffData` (`+0x9C` clave, `+0x90` probabilidad) |
| `0x359B50` | `ClientCharacterControlActorComponent vf 0x248` = `ret` |

---

## 7. Resultado en vivo de R1b (prueba de `guest_hits`, captura `20261004-173434`)

Hora del log = hora de la captura + 17:34:50,93. Script: `build/research/pieza4/p4r3*.py`.

- Daño, muerte y botín: bien. 10 golpes a copias → 10 `VaryStatCheatReq`/`KillCheatReq` con la fuente
  = jugador; dos enemigos muertos (motivo 16), botín en ambos.
- **Tambaleo: no.** Solo 1 de los 10 golpes tenía el trío aprendido (`F104F0B7/4` → `41275A91`,
  buff 1104); los demás eran ataques que el jugador no había usado antes contra enemigos reales.
  Ese trío llegó completo y en un mismo lote (t = 186,330: ATA, AAR y `ApplyBuffAck` 1104 con
  `inst 8000F000`, fuente `A010F001`, `+0x29` = posición de la marioneta, ticks frescos).
  - Respuesta del enemigo `B010016B`: `EchoActionSessionIDReq` (+21 ms) y
    **`StartActionReq CBD9B48B → A73D6EC4`** (+24 ms). Es lo mismo que en R1 sin el buff.
  - **No hubo `StartAIFunctionReq`**, que según §5.2 era la señal de que el buff entró.
  - Byte a byte, los tres paquetes coinciden con los reales salvo los campos reescritos.
- La copia (en el lado del "invitado") recibió el ATA y el AAR relevados y luego las acciones
  relevadas `A73D6EC4` y `FF8F3228`. En pantalla parecía tambalearse, pero eso es la animación de
  fin de reacción o un golpe local, no el tambaleo del original.
- Siguiente: las pruebas R1c (fuente 0) y R1d (lote real sin tocar, fuente = jugador) de §5.2, ahora
  en `piece4_test.cpp` (Ctrl+Shift+F10 / Ctrl+F11; Ctrl+F10 = R1b a mano). Si R1d tampoco hace
  tambalear, la inyección no basta y la reacción depende de algo que hace el cliente del atacante
  en su propio golpe (detección local del `DoFrameEventReq`); el hecho de que el golpe local del
  "invitado" sobre una copia en modo 2 parezca hacerla reaccionar apunta en esa dirección
  [inferido].

---

## 8. R1b, R1c y R1d en vivo (log del 4 de octubre, 17:53-17:55)

`piece4_test.cpp` inyecta el último lote real de tambaleo (ATA + AAR + buff) cambiando solo el
objetivo, el id de instancia y la fuente. El log apunta las acciones siguientes del enemigo.

| Prueba | Fuente (ATA `+0x04`, AAR `+0x49`, buff `+0x18`) | Intentos | Respuesta del enemigo | Lo que se vio |
|---|---|---|---|---|
| R1d | Nuestro jugador `A0100001` (lote real sin tocar) | 5 | `→ A73D6EC4` en 31-63 ms, siempre | Lo empuja <1 m, sin reacción clara |
| R1b | La marioneta `A010F001` | 12 | `→ A73D6EC4` en 16-94 ms, siempre | Igual |
| **R1c** | **0 (nadie)** | 2 | **`→ 75ACAB01`** (buff 1104) y **`→ 47E122BD`** (buff 1106, derribo) en 32-47 ms; `A73D6EC4` llega después, a los ~600 ms | **Reacción correcta; uno cayó al suelo** |

- La inyección en sí funciona: con fuente 0 el enemigo hace la reacción real y después el fin de
  reacción, como en un golpe nativo.
- Lo que la estropea es que haya **una fuente válida**. Incluso con nuestro propio jugador y el lote
  real intacto (R1d).
- R1c cambió a la vez el atacante del ATA y la fuente del AAR y el buff. Para saber cuál de los dos
  estropea la reacción quedan R1e (ATA desde la marioneta, AAR y buff sin fuente) y R1f (sin ATA, AAR y
  buff desde la marioneta).
- Hipótesis [inferido]: con una fuente válida el cliente aplica una comprobación que mira al
  atacante (distancia, orientación o su estado de ataque). En las pruebas la fuente estaba a 6-20 m
  del enemigo, y en un golpe real a 1-3 m.
- **Cambio en `guest_hits.cpp`:** el AAR y el buff van sin fuente; el ATA sigue desde la marioneta
  (empuje y evento de IA). Para ataques sin lote propio que siempre tambalean (tabla §2.3 de
  `pieza4-capturas.md`), se usa el último lote completo de cualquier ataque.

---

## 9. R1e y R1f en vivo (18:29-18:30): el culpable es el `AttackTargetAck`

| Prueba | ATA | AAR y buff, fuente | Respuesta | Lo que se vio |
|---|---|---|---|---|
| R1e | Desde la marioneta | 0 | Sin tambaleo: `→ A73D6EC4` o `→ 0AFD6433` (¿guardia?), 6 de 6 | Solo empuje |
| **R1f** | **Ninguno** | **La marioneta** | **`→ 55F2E5B1`** (1109) y **`→ 47E122BD`** (1106, derribo), 2 de 2 | **Funciona** |
| R1c | Atacante 0 | 0 | `→ 47E122BD`, 3 de 3 | Funciona (otra animación) |

- **Un `AttackTargetAck` con un atacante real estropea el tambaleo**, aunque sea un lote real intacto
  (R1d). Con atacante 0 el cliente no hace nada con el ATA (§3.1: `0x451020` sale si no encuentra al
  atacante), y por eso R1c funcionó.
- La fuente del AAR y del buff **no** importa: la marioneta como fuente funciona (R1f).
- Por qué el ATA con atacante lo estropea queda abierto [inferido]: el empuje físico (flags `& 0x4`,
  `0x45C8A0`) o el evento de IA `0x2083D90`, que empiezan otra reacción y la restricción cae
  después sobre ella. En el flujo real el cliente del atacante también hace su propio golpe.
- La prueba de `guest_hits` con la receta de §8 (ATA desde la marioneta + AAR y buff sin fuente,
  6 golpes con tambaleo) tampoco hizo reaccionar al original, como R1e.
- **Receta final en `guest_hits.cpp`:** golpe que tambalea → solo AAR + buff, con la marioneta como
  fuente (sin ATA); el resto de golpes → ATA desde la marioneta (empuje y evento de IA). Los ataques
  sin al menos 5 golpes propios usan la tabla medida para decidir si tambalean.
