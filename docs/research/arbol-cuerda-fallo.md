# Por qué falla el enganche de la copia al árbol (`RemoteCatchResultAck` inyectado)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas
solo para esta build. Análisis estático de solo lectura más tres capturas del 6 de octubre:

- Shadow (invitado): `cdmp_packets_20261006-225658.bin` + `cdmp.log` del invitado.
- Anfitrión, prueba con dos PC: `bin64/cdmp_packets_20261006-205626.bin`.
- Un PC (copia por `cdmp_peer`): `bin64/cdmp_packets_20261006-201209.bin`.

Lo marcado **(hipótesis)** no está comprobado. Los volcados nuevos están en
`build/research/interactions/` (§9). Script nuevo: `interactions/rc_scan.py <captura.bin>`, que lista
el tráfico de remote catch con el id de escena y el id de pieza.

---

## 0. Resumen

1. `0x594AD0` (inicio del remote catch en el cliente) solo tiene **dos salidas con `false`** para un
   enganche de árbol (tipo 0, clase de objetivo 2, actor objetivo 0):
   - **F1 `0x594B4A`:** la ranura ya está ocupada (`comp + ranura*0x58 + 0x20 != 6`, en `0x1808100`).
   - **F2 `0x594DDC`:** no se puede calcular la transformación del objetivo
     (`0x5A40E0` → `0x5A5370`). Con id de pieza `!= -1`, la pide a
     `[[0x6D69190]+0xE8]->vf(0x3B0)(pieza, &transform)`, y esa llamada falla si la pieza no existe
     en este cliente.
2. **El id de pieza (`+0x1E` del Ack, u64) es local de cada cliente.** No sale del nivel ni del
   servidor: lo genera el cliente que lanza la cuerda (lo pone en su `DoFrameEventReq`) y el
   servidor lo devuelve sin cambios. Prueba directa en el mismo árbol
   (`id escena a51dbc005d003daf 05…`):
   - anfitrión: `0x800000000D0013C7`;
   - Shadow: `0x800000000E000857`.

   El mismo árbol también cambia de id en otra sesión del mismo PC (`956e832a…`: `…01000954` a las
   20:12 y `…010009FB` a las 20:39). Tiene forma de id de cuerpo físico: secuencia en el byte alto
   y un índice de 24 bits **(hipótesis)**.
3. **(a) Primer enganche de la copia con dos PC → F2.** En Shadow, la ranura de la copia estaba
   vacía: era el primer `RemoteCatchResultAck` de la sesión y no había ninguno antes. El árbol
   estaba cargado (`SyncSceneObjectAck` y `UpdateOnDemandGimmickAck` cargado = 1, 3,7 s antes). El
   id de pieza era el del anfitrión. **El jugador de Shadow no había enganchado ese árbol antes:**
   lo enganchó 12,4 s **después** (t 1007,278, con éxito y con su propio id). En cualquier caso, el
   enganche de otro jugador no influye: la ranura es de cada actor y, con actor objetivo 0, el
   árbol no guarda quién lo agarra.
4. **(b) Segundo enganche sin soltar el primero → F1.** En la prueba con un PC (`201209`), el id
   de pieza era válido porque era el mismo mundo, así que el primero funcionó. Los cuatro siguientes
   (t 82,9 / 95,8 / 104,9 / 109,9) llegaron sin ninguna `ReleaseRemoteCatchResultAck` entre medias.
   F2 además deja la ranura ocupada: `0x1808100` ya la había rellenado y nadie la limpia.
5. **La suelta (`0x5A7A10` → `0x5A7BC0`) siempre termina en `0x1807C80`**, que vacía la ranura
   (`+0x20 = 0x00010106`: tipo 6, pieza `-1`…). Así que sí limpia lo que comprueba F1. El propio
   juego, en su ruta local de agarre de actor (`0x596BF0`), llama a `0x1807C80` justo antes de
   `0x594AD0`.
6. **Arreglo recomendado (receptor, al inyectar 2763 de la copia):**
   - poner `+0x1E..+0x25 = FF×8` (pieza `-1`). Con pieza `-1`, `0x5A5370` y `0x5A4850` buscan el
     objeto de escena **por el id de escena de 16 B** (`0x1851010`, igual en los dos PCs) y componen
     el punto local con su transformación;
   - inyectar siempre antes `ReleaseRemoteCatchResultAck {copia, ranura, 2}` si la copia tenía un
     enganche anterior (o si no se sabe).

   Opcional: si el receptor conoce **su propio** id de pieza para ese id de escena (de su propio
   2763 con catcher = jugador local, desde la última carga del árbol), usar ese en vez de `-1`.
7. No hacen falta ni la herramienta de la copia (`0xB01000AB`), ni `GimmickCatcherUpdateAck`, ni
   `UpdateOnDemandGimmickAck`, ni otro opcode: no aparecen en ninguna ruta de fallo. Tampoco hay
   comprobaciones de "controlado en local / por la red", de distancia o de cooldown.

---

## 1. Del lector a `0x594AD0`

### 1.1 Formato real de `RemoteCatchResultAck` (2763, 97 B con cadenas vacías)

El lector `0xB5B710` y el deserializador del objetivo `0x1806940` corrigen la tabla de
`interacciones-mundo.md` §2.1 a partir de `+0x32`:

| Offset | Tipo | Destino | Contenido |
|---|---|---|---|
| `+0x00` | u32 | `rbp-0x58` | clave de quien agarra (busca el actor con `0x8A67E0`) |
| `+0x04` | u8 | `rbp+0xC8` → r8 de `0x592340` | **ranura** (0; hay 2, de 0 a 1) |
| `+0x05` | u8 | `rbp+0xC0` → r9 de `0x592340` | **tipo de remote catch** (0–5; 6 = vacío). Árbol = **0** |
| `+0x06..+0x08` | 3× u8 | `rbp-0x80` | `01 01 00`, se copian a la ranura `+0x21..+0x23` |
| `+0x09` | u8 | objetivo `+0x00` | clase de objetivo: 0/1 actor, **2 objeto de escena**, 3 ninguno |
| `+0x0A` | u32 | objetivo `+0x04` | clave del actor objetivo (0 en el árbol) |
| `+0x0E` | 16 B | objetivo `+0x08` | **id de escena** (igual en los dos PCs) |
| `+0x1E` | u64 | objetivo `+0x18` | **id de pieza (local de cada cliente)**; `-1` = ninguna |
| `+0x26` | vec3 | objetivo `+0x24` | punto del gancho, en el sistema de la pieza o del objeto |
| `+0x32` | vec3 | objetivo `+0x30` | (ceros en todas las capturas) |
| `+0x3E` | quat | objetivo `+0x3C` | orientación (`0x141FA90`) |
| `+0x4E` | cadena | hash → objetivo `+0x20` | u32 longitud + bytes + NUL (vacía: `00 00 00 00 00`) |
| `+0x53` | u32 | `rbp-0x40` | `0x85FB13C7` en todas **(hipótesis: hash de efecto o configuración)** |
| `+0x57` | f32 | `rbp-0x44` | `45.0` |
| `+0x5B` | u8 | `rbp-0x48` | 0 |
| `+0x5C` | cadena | `rbp-0x28` → hash `0x12BECB0` | vacía (`00 00 00 00` + NUL `+0x60`) |

Las dos cadenas tienen longitud variable: a partir de `+0x4E`, los offsets solo valen si la primera
cadena está vacía. Los campos de `+0x00` a `+0x4D` sí son fijos.

### 1.2 `0x592340(comp, &resultado, &ranura, &tipo, &3bytes, &objetivo, &u32, &f32, …, &cadena)`

- `0x5923EB`: `switch (tipo)` con la tabla en `0x59265C` = `{0x592499, 0x59240B ×3, 0x592541,
  0x592633}`.
  - **Tipo 0 va directo a `0x592499`, sin ninguna búsqueda.**
  - Tipos 1–3: busca un actor con `0x8ADFE0` y, si no lo encuentra, da el error `0x55898A0`.
  - Tipo 4: usa el padre de `comp`.
  - Tipo 5: error `0x5589B30`.
- `0x5924EB`: `call 0x594AD0` con:
  - `rcx` = comp;
  - `dl` = ranura;
  - `r8` = copia local `S` (`rbp-0x39`): `S+0` tipo, `S+1..3` 3 bytes, `S+8..` objetivo
    (`S+8` clase, `S+0xC` actor, `S+0x10` escena, `S+0x20` pieza, `S+0x28` hash, `S+0x2C` pos,
    `S+0x38` vec3, `S+0x44` quat);
  - `r9` = &u32 `+0x53`;
  - pila: `f32 +0x57`, `0`, `&local`, `&hash de cadena`.
- Si devuelve `false`, el error `0x5589B80` es "모종의 이유로 클라에서 RemoteCatch에 실패했습니다…"
  (lo registra `0x35D8D0` desde `0x592536`). Devuelve el código `[0x6CF70A4]` y **no limpia
  nada**. El lector registra entonces "RemoteCatchResultAck처리에 실패했습니다." en `0xB5BBC4`.

---

## 2. `0x594AD0`: todas las salidas con `false`

`0x594AD0(comp, ranura, S, &u32, f32, u32, &x, &hash)`. Es `vf 42` (`+0x150`) de
`ClientRemoteCatchActorComponent` (vtable `0x55890D8`). El código de servidor `0x2902DC0` comparte
con él `0x1807A40`.

| # | Dirección | Condición | Tipos afectados |
|---|---|---|---|
| **F1** | `0x594B43` → `0x594B4A` | `0x1807A40` devuelve `false`, es decir, `0x1808100`: `byte [comp + ranura*0x58 + 0x20] != 6` (**ranura ocupada**) | todos |
| — | `0x594B5F` | tipo > 5 | — |
| **F2** | `0x594DD5` → `0x594DDC` | `0x5A40E0(comp, ranura, &transform)` devuelve `false` (§3) | todos |
| F3 | `0x595134`/`0x595147` | tipos 1 y 2: `0x1806C00` no encuentra el actor objetivo (`S+0xC`) | solo 1, 2 |
| F4 | `0x59537C` → `0x59513D` | tipo 3: no encuentra el actor objetivo | solo 3 |

Para el tipo 0 (árbol) solo pueden darse **F1 y F2**. Lo demás (`0x594C79`…`0x594F76`, efecto en
`comp+0xCC0`, `0x4FF1F0`, `0x740900`) no es una condición: con éxito, escribe `slot+0x480 = 0`,
`slot+0x168 = 0`, `comp+0xCA8[ranura] = 1` y la marca de tiempo `comp+0xCB0[ranura]`. **No hay
comprobaciones** de distancia, de cooldown, de herramienta, de que el actor esté controlado en local
ni del estado del gimmick del árbol. Con actor objetivo 0, `0x1807A40` no toca ningún componente
del objetivo: la parte de `0x1807A92`–`0x1807B47`, que escribe quién agarra en
`[[objetivo+0x68]+0x78]+0x18`, solo corre si hay actor objetivo.

### 2.1 `0x1807A40` y `0x1808100` (F1)

- `0x1807A40`:
  1. resuelve el actor objetivo con `0x1806C00` (`S+0xC`; con 0 no hay actor);
  2. si lo hay, ejecuta los efectos de §2;
  3. `return 0x1808100(comp, ranura, S)`.
- `0x1808100`:
  - `entrada = comp + ranura*0x58`;
  - **si `byte [entrada+0x20] != 6`, devuelve `false`**;
  - si no, copia `S` en la entrada (`+0x20` tipo, `+0x21..23`, `+0x28` clase, `+0x2C` actor,
    `+0x30` escena, `+0x40` pieza, `+0x48` hash, `+0x4C` pos, `+0x58` vec3, `+0x64` quat) y
    devuelve `true`.
- **Consecuencia:** si después falla F2, la entrada queda **ocupada** con el tipo 0, y los
  siguientes enganches fallan en F1 hasta que haya una suelta.
- Valor inicial: `0x1806B20` escribe `0x10106` (`0x1806BA1`), el valor "vacío". El getter
  `0x18082A0` y `0x5A40E0` tratan el 6 como "sin enganche".

---

## 3. `0x5A40E0` → `0x5A5370` (F2)

- `0x5A40E0(comp, ranura, &out)` es `vf 45` (`+0x168`), "transformación del objetivo del remote
  catch". Lo llaman 19 sitios, entre ellos la actualización por fotograma (`0x5A1530`).
  - Si la entrada vale 6, devuelve `false`.
  - Si no, aplica `switch(tipo)` con la tabla en `0x5A4744` = `{0x5A4266, 0x5A430C ×2, 0x5A45C4,
    0x5A4288, false}`.
  - Tipo 0 → **`0x5A5370(&entrada+0x28, comp + ranura*0x3E0 + 0x1C0, out)`**.
- `0x5A5370(T, …, out)` con `T` = objetivo de la entrada:
  - clase 3 o > 2 → `false`;
  - clase 0 → actor por clave (`0x8AE940`), y `false` si no está;
  - clase 1 → actor, con su esqueleto o hueso;
  - **clase 2 (objeto de escena):**
    1. `out.pos = T+0x24` (punto local) y `out.rot = T+0x3C`.
    2. **Si la pieza `T+0x18 != -1`** (`0x5A5439`): llama a
       `[[0x6D69190]+0xE8]->vf(0x3B0)(pieza, &xf)` (`0x5A5495`). **Si devuelve `false`, devuelve
       `false` (F2).** Si no, `out = xf ∘ out`.
    3. **Si la pieza vale `-1`** (`0x5A5575`):
       - si los 12 primeros bytes del id de escena son 0, devuelve `true` con `out` tal cual (el
         punto se toma como coordenadas de mundo);
       - si no, busca el objeto de escena por **id de 16 B**: `0x1851010` sobre
         `[[[0x6D69190]+0xC8]]+0x80`, con el attacher `SyncSceneObjectDataAttacher`. Si lo
         encuentra, `out = obj->vf(0x38)() ∘ out` y devuelve `true`; si no, `false`.
- `0x5A4850` (transformación y velocidad, que usan `0x5A1530`/`0x5A4760`) tiene la misma
  bifurcación: pieza `!= -1` → `vf(0x3B0+8 = 0x3B8)`; `-1` → `0x1851010` por id de escena
  (`0x5A4A1C`–`0x5A4B45`). **Con la pieza `-1`, todas las rutas por fotograma funcionan con el id de
  escena.**
- El objeto de `[0x6D69190]+0xE8` no tiene RTTI que lo identifique **(hipótesis: es el mundo
  físico)**. En la ruta del tipo 3, el id viene de `0x500D70(actor)->[+0x108]->vf(0x70)`, el id del
  cuerpo físico del actor. Por eso el id de pieza es un id de cuerpo físico del cliente.

### 3.1 Prueba de que el id de pieza no se comparte entre PCs

`interactions/rc_scan.py`:

| PC | t | Mensaje | Id de escena | Pieza | Punto |
|---|---|---|---|---|---|
| Anfitrión | 1015,322 / ,331 | su DoFrameEventReq / su 2763 | `a51dbc005d003daf 05…` | `0x800000000D0013C7` | (0,05, 7,03, −0,37) |
| Shadow | 994,870 | 2763 inyectado para `0xA010F001` | ídem | `0x800000000D0013C7` (del anfitrión) | ídem → **falla** (log 23:14:02.537) |
| Shadow | 1005,257 | 2471 inyectado `{0xA010F001, 0, 2}` | | | |
| Shadow | 1007,278 / ,280 | su DoFrameEventReq / su 2763 | ídem | **`0x800000000E000857`** | (−0,12, 12,19, −0,25) → funciona |

- El id de pieza solo aparece en el `DoFrameEventReq` del cliente y en el Ack. No está en ninguna
  carga (`ActorLoadDataAck`) ni en `SyncSceneObjectAck`, como ya se veía en la captura
  `20261003-200328`: `findbytes.py 601d0002` → 2 resultados.
- En Shadow, el árbol estaba cargado antes del Ack inyectado: `SyncSceneObjectAck` (2665) con ese id
  de escena en t 991,126 y `UpdateOnDemandGimmickAck` cargado = 1 en 991,158.

---

## 4. Diagnóstico por caso

**(a) Primer enganche de la copia en un árbol que el receptor tiene cargado (dos PC): F2**
(`0x5A5495`, `vf 0x3B0` con la pieza del otro PC). Confianza alta:
- la ranura estaba vacía (primer 2763 de la sesión en Shadow);
- el tipo 0 no tiene más condiciones;
- los ids de pieza son distintos en los dos PCs para el mismo árbol.

La idea de que el receptor lo había enganchado 14 s antes no se sostiene. En Shadow, su propio
enganche es posterior (+12,4 s). Aunque fuera anterior, la ranura es del actor que agarra (la
copia) y con actor objetivo 0 el árbol no guarda quién lo tiene. En la prueba con un PC, el
jugador y la copia engancharon el mismo árbol a la vez y la copia funcionó.

Si se reutiliza un índice de cuerpo con la misma secuencia, `vf 0x3B0` podría devolver `true` con
**otro objeto**, y el gancho aparecería en un sitio absurdo **(hipótesis, poco probable)**.

**(b) Segundo enganche sin soltar el primero: F1** (`0x180810A`, `+0x20 != 6`). En la captura
`201209` llegaron cuatro 2763 seguidos para `0xA010F001` sin ningún 2471 entre medias. Lo mismo
pasa con cualquier enganche **después de un F2**, porque F2 deja la ranura ocupada.

---

## 5. Suelta: `0x5A7A10` → `0x5A7BC0` → `0x1807C80`

- `0x5A7A10(comp, ranura, motivo, r9b, a5)`:
  - en el hilo del juego (TLS `+0x1B4 == [0x6A652D4]`) escribe `comp+0xC90+ranura*4 = {0,0,1,0}` y
    llama a `0x5A7BC0` en el momento;
  - en otro hilo deja la petición en `comp+0xC90+ranura*4 = {motivo, r9, a5, 1}` para el update.
- `0x5A7BC0`:
  - lee el estado de fase `byte [comp + ranura*0x1A0 + 0x9BC]` (0 = nada; 1 a 4; 7);
  - deshace lo que haga falta: efecto `comp+0xCC0` en `0x5A8470`, componente `0xCA0`,
    `0x589820(comp + ranura*0x3E0 + 0x168)`, `0x5AE410`, y `comp+0xCA8[ranura] = 2` con marca de
    tiempo;
  - pone la fase a 6 si estaba en 0/1 (`0x5A867D`);
  - **siempre** termina en **`0x1807C80(comp, ranura, …)`** (`0x5A86DF`).
- `0x1807C80`:
  - si había actor objetivo, borra `[[objetivo+0x68]+0x78]+0x18` (quien lo agarra) y `+0xE8`;
  - **escribe `dword [comp + ranura*0x58 + 0x20] = 0x00010106`** (tipo 6, `+0x21 = 1`, `+0x22 = 1`,
    `+0x23 = 0`), clase 3, actor 0, escena 0, **pieza `-1`**, y escala, rotación y posición por
    defecto.
- Conclusión: la suelta limpia exactamente lo que comprueba F1. Con la ranura vacía también es
  inofensiva: la fase 0 salta a `0x5A8507` y luego llega a `0x1807C80` **(hipótesis de que no tiene
  efectos secundarios; no se ha seguido `0x5AE410` en profundidad)**. El juego hace lo mismo en
  `0x596BF0` (`vf 47`, agarre local de un gimmick de tipo 7): llama a `0x1807C80` y, sin más, a
  `0x594AD0`.

---

## 6. Servidor y otros Ack

- El servidor (`0x2902DC0`) usa el mismo `0x1807A40` sobre **su** componente: si la ranura está
  ocupada, falla en `0x2902E3D` y no manda nada. Si no, manda el Ack (`0x2968330`, ToNear) con los
  bytes que le llegaron: el id de pieza va tal cual, sin traducir. `0x29035E2`/`0x290361D`
  (`0x1807C80` + `0x29688C0` `ReleaseRemoteCatchResultAck`) es una rama de cancelación: en las
  sueltas normales nunca aparece 2471 (ya visto en §2.1 de `interacciones-mundo.md`).
- `GimmickCatcherUpdateAck` (2356, `0xBB0960`) es del flujo de coger o lanzar objetos (16 B:
  `u32 gimmick, u32 gimmick, u32 catcher, u32 0x000F426C`). Ninguna ruta de `0x594AD0`/`0x5A40E0`
  lo consulta, así que no hace falta.
- `UpdateOnDemandGimmickAck` (2145) solo indica si el árbol está cargado en el receptor. Con la
  pieza `-1`, lo que hace falta es el `SyncSceneObjectData` del id de escena, que llega con
  `SyncSceneObjectAck` (ToField) al acercarse. Sirve como señal para el mod (§7.3).
- La herramienta `0xB01000AB` no participa en ninguna comprobación: el primer enganche con un PC
  funcionó sin ella. Puede que se necesite para **dibujar** la cuerda **(hipótesis)**.

---

## 7. Arreglo recomendado (solo en el receptor, S→C)

### 7.1 Reescribir el 2763 de la copia antes de inyectarlo

```
+0x00 u32  = 0xA010F001            (ya se hace)
+0x04 u8   = ranura (0)            sin cambios
+0x05 u8   = 0                     sin cambios (tipo; si != 0, no inyectar)
+0x09 u8   = 2                     sin cambios (si != 2, no inyectar o traducir el actor)
+0x0A u32  = 0                     sin cambios
+0x0E 16 B = id de escena          sin cambios
+0x1E u64  = 0xFFFFFFFFFFFFFFFF    <-- NUEVO (o la pieza propia del receptor, §7.3)
resto      = sin cambios
```

Con eso, `0x5A5370`/`0x5A4850` buscan el árbol por id de escena (`0x1851010`) y componen
`T+0x24`/`T+0x3C` con su transformación.

- **(Hipótesis)** El punto local está en el sistema de la pieza. Para el tronco debería coincidir
  con el del objeto de escena en reposo: los puntos rondan los 7–8 m de altura, sin desplazamiento
  lateral. Si no coinciden, el gancho sale desplazado (solo se ve mal, no se cuelga).
- **(Hipótesis)** Con el id de escena, el receptor no sigue la flexión física del cuerpo: el
  extremo de la cuerda se queda en el punto del árbol sin doblar.
- **Caso "coordenadas de mundo".** Algunos objetivos de Shadow (ids de escena
  `d613cf1c…`/`8a27ce1c…`, piezas `…09000D2B`/`…010025FE`) llevan en `+0x26` coordenadas de mundo
  (−10731, 605, −4007). Su pieza tiene transformación identidad (cuerpo estático). Si
  `|vec3 +0x26| > 1000`:
  - poner también el id de escena a 0 (16 B);
  - con pieza `-1` y escena 0, `0x5A5370` devuelve `true` con el punto tal cual.

  Si no, la transformación del objeto de escena se aplicaría dos veces **(hipótesis)**.

### 7.2 Vaciar la ranura antes de cada enganche

Antes de cada 2763 de la copia, si la copia tuvo un 2763 anterior y no se ha inyectado su 2471
(o si no se sabe), inyectar:

```
ReleaseRemoteCatchResultAck (2471), 6 B: u32 0xA010F001 | u8 ranura | u8 2
```

y luego el 2763 (en el mismo tick o en el siguiente). Esto cubre F1, incluido el F1 que deja un F2
anterior. Lo más sencillo es hacerlo **siempre**: el propio juego vacía la ranura sin comprobar
nada en `0x596F15` **(hipótesis de que es inofensivo con la ranura vacía, §5)**.

### 7.3 Opcional: id de pieza propio del receptor (mejor fidelidad)

- Guardar `id escena → pieza` de los 2763 S→C del receptor con `+0 == jugador local`, o de su
  `DoFrameEventReq` C→S de 636 B: id de escena en `+0x20B`, pieza en `+0x21B`.
- Invalidar la entrada cuando llegue `UpdateOnDemandGimmickAck` (`04 | u32 jugador | u8 cargado |
  id escena`) con ese id de escena, tanto si es 0 (descargado) como si es 1 (cargado otra vez): los
  ids cambian en cada carga, como ya se veía en la captura `20261003`.
- Si hay una entrada válida, poner esa pieza en `+0x1E`; si no, `-1`. Una pieza obsoleta vuelve a
  dar F2 y deja la ranura ocupada, así que mantener también §7.2.

### 7.4 Pruebas

| # | Prueba | Qué decide |
|---|---|---|
| A1 | Un PC: 2763 de la copia con pieza `-1` | que la marca azul aparece por el id de escena, y dónde |
| A2 | Un PC: dos enganches seguidos con 2471 automático antes del segundo | que F1 desaparece |
| A3 | Dos PC: Shadow con §7.1 y §7.2 | que no aparece el error de `0x592536` |
| A4 | Comparar la posición de la marca con la pieza propia (§7.3) y con `-1` | si el punto local depende de la pieza |

---

## 8. Confianza

| Afirmación | Confianza |
|---|---|
| Para el tipo 0 / clase 2 solo existen F1 y F2 | alta (desensamblado completo) |
| (a) es F2 por la pieza ajena | alta (ids distintos en el mismo árbol, ranura vacía, árbol cargado) |
| (b) es F1 | alta |
| La suelta vacía la ranura | alta (`0x1807C80`, incondicional al final de `0x5A7BC0`) |
| Pieza `-1` → éxito por id de escena | media-alta (código claro; falta comprobar en el juego que `0x1851010` encuentra el árbol) |
| Posición visual correcta con `-1` | media (hipótesis) |

---

## 9. Volcados nuevos (`build/research/interactions/`)

| Archivo | Función |
|---|---|
| `c_1807a40_remotecatch_canstart.asm` | `0x1807A40` (compartida con el servidor) |
| `c_1808100_remotecatch_canstart2.asm` | `0x1808100` (F1) y sus vecinas `0x18082A0` (getter) y `0x1808330` |
| `c_1806c00_catchtarget_resolve.asm` | `0x1806C00` (actor objetivo por `+0xC`) |
| `c_1806940_catchtarget_deserialize.asm` | `0x1806940` (orden de los campos del objetivo) |
| `c_5a40e0_remotecatch_check.asm` | `0x5A40E0` (F2, `switch` de tipos) |
| `c_5a5370_remotecatch_worldpoint_type0.asm` | `0x5A5370` (pieza → `vf 0x3B0`; `-1` → escena) |
| `c_5a4850_remotecatch_pull_piece.asm` | `0x5A4850` (variante por fotograma, `vf 0x3B8`) |
| `c_1851010_syncsceneobject_find.asm` | `0x1851010` (busca un objeto de escena por id de 16 B) |
| `c_599e50_remotecatch_update_piece.asm` | `0x599E50` (otro uso de `vf 0x3B0`) |
| `c_596bf0_remotecatch_localstart.asm` | `0x596BF0` (`vf 47`: `0x1807C80` y luego `0x594AD0`) |
| `c_5a7bc0_release_do.asm` | `0x5A7BC0` (suelta real) |
| `c_1807c80_remotecatch_clearslot.asm` | `0x1807C80` (vacía la ranura: `0x10106`, pieza `-1`) |
| `rc_scan.py` | script: tráfico de remote catch de una captura `.bin` |
