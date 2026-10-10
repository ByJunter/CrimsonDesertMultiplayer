# Árbol con cuerda (5): la cuerda de la copia sale corta y apunta a otro sitio

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`), válidas
solo para esta build. Análisis estático de solo lectura más los logs y capturas del 7 de octubre por
la noche; el juego no se ejecutó. Continúa `arbol-cuerda-4-cuerda-visible.md`. Lo marcado
**(hipótesis)** no está comprobado.

Build del mod probada: `228287A2 + local changes` (R1 y R2 de `arbol-cuerda-4`), en los dos PC.

Volcados y scripts nuevos en `build/research/interactions/rope5/` (§10).

Notación: `comp = [[actor+0x68]+0x78]` (`ClientRemoteCatchActorComponent`). Ranura 0:
`S = comp+0x928` (bloque de la cuerda), `E = comp+0x168` (estado de control). Un "asa" `h` apunta a
un objeto `o = [h+8]`, vivo si `byte o+0x15 == 0`; el nodo de escena es `o − 0x28`.

---

## 0. Resumen

1. **Hay una referencia limpia en los logs.** En la sesión 2 (20:34–20:40) el anfitrión y la
   copia del invitado engancharon **el mismo árbol** (`C6C50042719F53CC`, la misma pieza
   `0x8000000002001170`) **desde el mismo sitio** (a 1,3 m uno del otro, en
   `(-10116, 555,5, -4616)`), con puntos de enganche casi iguales en el tronco (6,5–9 m de altura,
   a menos de 0,4 m del eje). La cuerda del jugador local sale bien y la de la copia no. **Los datos
   del 2763 no son el problema** (§1.3, §4).
2. **La sonda del jugador local enganchado (20:37:50) desmiente la base de R1.** El jugador local
   está en la fase 3 con `+AC0 0 +ABC 0.00 +AB8 0.00`: el efecto está **apagado**. La copia, con R1,
   tiene `1 / 1.00 / 1.00`. R1 hace que la copia se diferencie del local. Además, solo cambia
   parámetros del shader, no la geometría, así que no puede explicar que la cuerda apunte a otro
   lado (§1.2, §4.5).
3. **`+A98 = 0` y estela `n 0` en los dos.** La ruta `0x59CAF0` (A + estela + B), que
   `arbol-cuerda-4` §1.2 daba como la que dibuja la cuerda, **no corre con los árboles** ni en el
   jugador local ni en la copia. Los puntos de la malla los pone **`0x5AEAA0`** en cada fotograma
   (bloque `0x5AEDCC`–`0x5AF128`, con el global `[0x6C99968]`): **`[pos(A), puntos de
   comp+0xCE0 en orden inverso, pos(B)]`**, con A = nodo `comp+0x950` y B = nodo `comp+0x958`. La
   lista `comp+0xCE0` (cuenta en `+0xCE8`) la rellena solo el script de movimiento
   `updateMovementInternal_RopeSwing` (`0x11AA7D0` → `0x11A92D0` → `0x11A5400`) (§2).
4. **Nadie mueve los nodos A y B en la copia.** Solo se crean una vez en `0x5AB7A0` (desde
   `0x5ABBC0`), colgados de un hueso (`cfg+0xB0` y `cfg+0xB4`) de una pieza visual del personaje
   que se busca por hash (`cfg+0xAC`) en el mapa de `[[[[CC+0x130]]+8]+0x78]`. **Si esa pieza no
   existe**, se cuelgan del nodo raíz del actor (`0x500D70`) con ese hueso. Ninguna función que corre
   en la copia (`0x59D470`, `0x59E150`, `0x598F80`, `0x59F5F0`/`0x5A34A0`, `0x5A1530`, `0x5AEAA0`)
   llama a `0x26D4B40` (fijar la transformación de mundo) sobre A o B: lo hacen sobre la punta
   `S+0x00`, `S+0x08`, la cabeza `S+0x10`, `S+0x58`, `S+0x68`, `+0x970`, `+0x988` y `+0xA78`, pero
   nunca sobre `S+0x28`/`S+0x30` (§3).
5. **Causa raíz (confianza media-alta):** el extremo B de la cuerda va pegado a un hueso que, en el
   jugador local, se lleva hasta el árbol con código que solo corre en él. El candidato es
   `0x17A6210` (al final de `0x5956A0`, tras la puerta `0x86A557`): copia la transformación del
   objetivo de la ranura 0 en los objetos acoplados del jugador (la herramienta "Game Mechanic" con
   `RemoteCatchPivot`). La copia, además, **no tiene herramienta**: su
   `OnDemandEquipDockingGimmickSpawnReq` (t 99,091) no recibe respuesta, mientras que la del
   anfitrión (t 39,787) recibe 7 cargas `0xB0100066`–`0xB010006C`. Así que en la copia B se queda en
   el hueso de reposo, junto al cuerpo. La malla va de A (mano) a B (junto al cuerpo): **una cuerda
   corta al principio del recorrido, orientada como el hueso y no hacia el árbol.** La punta
   (`S+0x00`), en cambio, sí está en el árbol porque `0x5AEAA0` la coloca en cada fotograma.
6. **Arreglo propuesto (R5, solo visual):** un `safetyhook::create_mid` en **`exe+0x59D89A`**
   (dentro del tick por ranura `0x59D470`, justo antes de `call 0x5AEAA0`, con `rdi = comp` y
   `sil = ranura`). Para la copia, en la ranura 0, tipo 0 y fase 3, pone en cada fotograma el nodo
   B en la transformación del objetivo, igual que `0x5AEAA0` hace con la punta:
   `0x5A40E0(comp, 0, &xf)` → `0x50EA00(&xf)` → `0x26D4B40(nodoB, &xf, 0, 1)`. Además, **apagar R1**
   en la fase 3 (o del todo). Antes, o a la vez, ampliar la sonda (R0', §6.1) para medir
   `|B − objetivo|` en la copia y en el jugador local (§6).

---

## 1. Qué dicen los logs

### 1.1 Sesiones y archivos

| Sesión | Anfitrión | Invitado (Shadow) | Enganches con cuerda |
|---|---|---|---|
| 1 (20:18–20:29) | `bin64/cdmp.1.log`, `cdmp_packets_20261007-201826` | log del invitado `cdmp.log` (19:16–19:30, reloj −1 h) | **ninguno** en los dos logs (solo "rope … installed") |
| 2 (20:34–20:40) | `bin64/cdmp.log`, `cdmp_packets_20261007-203428` | no hay logs | anfitrión: 1 (20:37:48); copia del invitado: 2 (20:39:30 y 20:39:54) |

Los dos PC llevan `build 228287A2 + local changes, protocol 9`. En la captura `203428`, `t = 0`
equivale a unas **20:34:41,8** del anfitrión: el 2763 inyectado sale en t 288,532 y el log lo
anuncia a las 20:39:30,096, más los 200 ms de retraso.

**Ojo:** según los logs, el árbol se dobla **en la pantalla del anfitrión**, mientras la copia del
invitado tira (`bends 62 → 395` entre 20:39:34 y 20:39:46). Del enganche del anfitrión visto en
Shadow (20:37:48) no hay log. El síntoma observado encaja con la copia que se ve en el
anfitrión, y el mecanismo de §3 es el mismo en los dos sentidos.

### 1.2 Sondas (`Puppet: rope probe`), ranura 0

| Campo | Jugador local, 20:37:50 (1,8 s tras su 2763) | Copia, 20:39:31,8 / 34,8 / 37,9 / 40,9 / 43,9 y 20:39:56,4 |
|---|---|---|
| tipo / pieza `+0x40` | 0 / **`0x8000000004000BDC`** | 0 / `0x8000000002001170` |
| fase `+9BC` (`+9F9/+9FA`) | 3 (3/2) | 3 (3/2) |
| vuelo `+A90/+A94` | 0,35/0,35 | 0,34/0,32 · 0,32/0,30 |
| `rope +928 +930` | ≠ 0 | ≠ 0 |
| **fx `+AC0` `+ABC` `+AB8`** | **0 / 0,00 / 0,00** | **1 / 1,00 / 1,00** (R1) |
| spline `+A98`, estela `n`, `d` | **0**, 0, 0,00 | **0**, 0, 0,00 |
| `+CA8` / `+D00` | 1 / 0 | 1 / 0 |
| acción `+F6` | 0 | 0 → 1 mientras tira (`pulling`) → 0 |
| flexión `+4F4`, `f`, `v` | 0, 0, 0 | 1, 0,24 → 0,70 → 0,80, 0,15 mientras tira (R2); 0 al dejar de tirar |
| malla `+9A0` (`+15`/`+30`), A, B | ok (0/5), ok, ok | ok (0/5), ok, ok |

- Solo hay **una** sonda del jugador local enganchado. La sonda propia se programa una sola vez al
  empezar (`puppet_tick`, `own_on && !own_was`) y luego solo se reprograma si `g_copy_hooked`.
- Todo lo que la sonda mide es igual en los dos, salvo el estado del efecto (por R1) y la pieza.
  Las posiciones, que es donde está la diferencia, **no se miden** (§6.1).

### 1.3 Los tres `RemoteCatchResultAck` (2763) de la sesión 2

`rope5/dump2763.py` sobre `cdmp_packets_20261007-203428.bin`:

| t | Quién | Escena | Pieza `+0x1E` | Punto `+0x26` (local a la pieza) | Quat `+0x3E` (giro en y) |
|---|---|---|---|---|---|
| 186,545 | anfitrión `0xA0100001` (su propio Ack) | `…06 C6C50042719F53CC` | `0x8000000002001170` | (−0,121, **8,970**, −0,082) | (−0,009, −0,441, −0,004, 0,898) ≈ −52° |
| 288,532 | copia `0xA010F001` (inyectado) | igual | `0x8000000002001170` (la nuestra, por `g_own_pieces`) | (−0,225, **6,517**, −0,285) | (0,001, −0,789, 0,002, 0,615) ≈ −104° |
| 313,008 | copia (inyectado) | igual | igual | (−0,199, **7,017**, −0,357) | (0,002, −0,814, 0,003, 0,581) ≈ −109° |

- Clase de objetivo 2, actor objetivo 0, `+0x53 = 0x85FB13C7`, `+0x57 = 45,0` en los tres.
- La pieza del anfitrión sale de su `DoFrameEventReq` (t 186,542, `+0x21B`). Entre t 163,7 (la
  última `UpdateOnDemandGimmickAck` de ese árbol) y t 313 no se descarga ni se recarga el árbol, así
  que la pieza que pone el mod (la del propio enganche del anfitrión) es válida.
- Tras cada 2763 de la copia, el cliente del anfitrión envía `IsRemoteCatchEffectStartStateReq` 2108
  `00 01` (+24 ms) y `00 00` (+0,34 s): el mismo par que tras el suyo. Es la cuerda de la copia
  hablando con el servidor del anfitrión (`arbol-cuerda-2` F4), y prueba que la copia hace el vuelo y
  engancha (`0x5A12A0` y `0x5A1530`).

### 1.4 Posiciones

Hay `Local: pos` y `last remote` cada 5 s:
- anfitrión al enganchar: `(-10115,8, 555,7, -4617,4)`;
- copia en 20:39:28–43: `(-10116,4, 555,5, -4615,8)`, quieta;
- duración del vuelo `+A94` · 45 m/s: anfitrión 15,8 m, copia 14,4 y 13,5 m.

Mismo árbol, mismo sitio y la misma distancia.

---

## 2. Cómo se dibuja de verdad la cuerda de un árbol (corrige `arbol-cuerda-4` §1.2)

### 2.1 `0x59CAF0` no se usa con los árboles

En `0x59D7F9`, el tick por ranura solo llama a `0x59C660` (estela) y a `0x59CAF0` (puntos) si
`byte comp + r·0x1A0 + 0xA98 != 0`. La sonda da **`+A98 0` en el jugador local y en la copia**, con
la estela vacía (`n 0`, `d 0`). Esa ruta, la de los portales (`portalgate0/1`) y la estela de la
fase 5, no participa. Tampoco hace falta mirar la estela `+0xAA8`.

### 2.2 `0x5AEAA0` construye los puntos en cada fotograma

`0x5AEAA0(comp, dt, ranura)` corre siempre al final del tick por ranura (`0x59D8A5`), también en la
copia (la sonda lo confirma: `+9F9/+9FA = 3/2` los escribe ese tick). Hace, en orden:

1. `ok = 0x5A40E0(comp, r, &T)` (`0x5AEB5B`): `T` es la transformación del objetivo. Formato
   `xf` (0x2C B): `+0x00` vec3 escala, `+0x0C` quat, `+0x1C` vec3 posición, `+0x28` dos `int16` de
   sector (posición de mundo = `+0x1C` + sector·1000; `0x5D736F4 = 1000.0`). `0x5A40E0` la devuelve
   con sector 0 y posición absoluta.
2. `0x500730(actor, cfg+0x90, &M)` (`0x5AEC6C`): el socket de salida en la mano. Si falla, sale.
3. **Lee la posición de mundo de los nodos `S+0x18`, `S+0x20`, `S+0x28` (A) y `S+0x30` (B)** con
   `0x26D3840` (`0x5AECAF`…`0x5AED7A`).
4. **Si `byte [0x6C99968] != 0` y la malla `S+0x78` es válida** (`0x41F6C0`, `+0x5C`,
   `[+0x250]+0x88 & 0x400 == 0`), monta la lista de puntos (`0x5AEE46`…`0x5AF0C8`):
   - `pos(A)`;
   - los `n = [comp+0xCE8]` puntos de `[comp+0xCE0]` (24 B cada uno, se copian los 12 primeros),
     **del último al primero**;
   - `pos(B)`.

   Y se la pasa a la malla con `0x589750` (`0x5AF0F1`).
5. Coloca la **punta** `S+0x00` en `T` (`0x5AF4B1`), si `ok`.
6. Escala `S+0x08` según la velocidad (`0x5AF2A1`…`0x5AF4B1`, árboles y `[0x6C9A3F8]`), coloca
   `S+0x68` entre las dos manos (`0x5AF522`…`0x5AF7C4`) y `S+0x58` (`0x5AF7C9`…`0x5AF8BF`).

`[0x6C99968]` está en la zona sin inicializar del ejecutable (la rellena el arranque), así que no
se puede leer en estático. Como `+A98 = 0` y la cuerda del jugador local sí se dibuja, **se deduce
que vale ≠ 0** y que esta es la ruta que dibuja (hipótesis fuerte; la sonda R0' lo comprueba).

### 2.3 La lista `comp+0xCE0`/`+0xCE8`

- **La escribe solo `0x11A5400`** (`lea rcx, [rbx+0xCE0]` → `0x5B24F0`; `add rbx, 0xCE0` →
  `0x5853D0`), que hace pruebas de choque a lo largo de la cuerda (`0x3A19000`/`0x3A18E00`) para
  que rodee obstáculos.
- `0x11A5400` la llaman solo `0x11A92D0`, y a esta solo **`0x11AA7D0`**, que se registra en
  `0xC2E570` con el nombre `"::pa::gameClientScript::updateMovementInternal_RopeSwing"`
  (`0x5579020`). Es movimiento del jugador controlado en local **(hipótesis: en la copia no
  corre; su movimiento lo dan los `Move` de la red)**.
- La suelta (`0x5A7BC0`, `0x5A7CF7`) la vacía (`+0xCE8 = 0`), y `0x5A60D0` la lee.
- Con un árbol quieto y sin obstáculos, la lista está vacía o casi, y la cuerda es la recta A → B.

**Conclusión:** la geometría de la cuerda depende **solo de dónde estén A y B**. Los parámetros del
shader (`vf16` `0x11930A0`, que es lo que toca R1) no mueven los extremos.

---

## 3. Dónde están A y B

### 3.1 Creación: `0x5AB7A0(comp, &hashPieza, &hashHueso, &asa)`

`0x5ABBC0` (inicio de la cuerda, fase 1) la llama 4 veces con la configuración
`cfg = 0x2456860([[0x6D69190]+0xB0], 0x85FB13C7)` (`0x5AC491`…`0x5AC4E2`):

| Asa | Pieza | Hueso |
|---|---|---|
| `S+0x18` | `cfg+0xA0` | `cfg+0xA4` |
| `S+0x20` | `cfg+0xA0` | `cfg+0xA8` |
| **`S+0x28` (A)** | **`cfg+0xAC`** | **`cfg+0xB0`** |
| **`S+0x30` (B)** | **`cfg+0xAC`** | **`cfg+0xB4`** |

Cuerpo de `0x5AB7A0` (`rope5/c_5ab7a0_full.asm`):

```
pieza = null
si [hashPieza] != 0:
    CC = [[comp+8]+0x68]+0x40                      // ClientCharacterControl
    m  = [[[[CC+0x130]]+8]+0x78]                   // escena del personaje
    e  = 0x466740(m, hashPieza)                    // búsqueda binaria: claves u32 en [m+0x58], n en [m+0x60], 16 B por entrada
    si e: pieza = [[e+0x38]+8] − 0x28              // nodo de la pieza
          (y si [[e+0x28]+8] vive: 0x4D6050(...))
si [hashHueso] != 0:
    nodo = 0x3B57AE0([[0x6D69190]+0xE0]+0xF38, …)  // nodo nuevo en la escena
    desc = {flags 0x01010101, escala 1, quat identidad, pos 0, hueso = [hashHueso]}
    si pieza:
        si 0x26D2F10([pieza+0x110], hashHueso) encuentra el hueso:
            0x26DAB60(pieza, nodo, &desc, 0)       // colgado del hueso de la pieza
    si no:
        0x26DAB60(0x500D70(actor), nodo, &desc, 0) // colgado del nodo del actor, con ese hueso
    *asa = nodo
```

- `0x466740` busca en un mapa ordenado de `u32 → objeto` (`rope5/c_466740_scene_map_find.asm`). El
  objeto `[CC+0x130]` es el mismo que recibe los eventos de efecto (`0x2D2C900([CC+0x130], 0x25,
  cfg+0x94)`, `arbol-cuerda-3` §3.1): es la parte visual del personaje.
- `0x26DAB60(padre, hijo, desc, 0)` es "enganchar el nodo hijo al padre". El mismo patrón se usa en
  `0x5AC1C2` para `S+0x08` sobre el nodo del actor.

### 3.2 Nadie más los mueve

Llamadores de `0x26D4B40` (fijar la transformación de mundo de un nodo; dentro llama a
`0x26D5080`, que la convierte a local respecto al padre) en el código de la cuerda: `0x599039`
(cabeza `S+0x10`, en `0x598F80`), `0x5A3F41` (`+0xA78`, en `0x5A34A0` ← `0x59F5F0`) y los cuatro de
`0x5AEAA0` (`S+0x08`, `S+0x00`, `S+0x68`, `S+0x58`). `0x5A1530` usa `0x26B01C0`/`0x26D55E0` sobre
`+0x970`/`+0x988` y engancha la punta a una restricción física (`0x338C940` → `0x26D5E60`). **A
(`S+0x28`) y B (`S+0x30`) solo se leen** (`0x5AEAA0`, y `0x59CAF0`/`0x59C660`, que no corren).

Así que la posición de B es la del hueso `cfg+0xB4` del que cuelga, sin más.

### 3.3 Qué lleva ese hueso al árbol en el jugador local

En el código que solo corre para el jugador local (`0x5956A0`, puerta `0x86A557`) hay una única
escritura de transformaciones: **`0x17A6210([[[actor+0x68]+0x20]+0x20], &T')`** (`0x595F37`), con
`T'` construida a partir de `0x5A40E0(comp, 0, …)` (`0x595E97`) y `0x50EA00`. `0x17A6210` recorre los
objetos acoplados del actor y, para los que tienen la regla de tipo 1 y `byte +0x132 & 0x20`, copia
`T'` en `+0x80`…`+0xF7` del objeto (`0x17A62C8`…`0x17A6301`). Es decir, **pone un objeto acoplado
en el punto de enganche.**

- **(Hipótesis, confianza media)** Ese objeto es la herramienta de la cuerda (gimmick acoplado
  "Game Mechanic", con `RemoteCatchPivot`, `InteractionPivot` y `Bip01 L HandTwist` en su carga,
  `arbol-cuerda-2` §3.2), y es la pieza `cfg+0xAC` de §3.1. A cuelga de su base (`cfg+0xB0`) y B de
  su pivote (`cfg+0xB4`). La cuerda va de la mano al pivote, y el pivote está en el árbol.
- Lo que sí es un hecho: **la copia no tiene herramienta.** En `203428`, la
  `OnDemandEquipDockingGimmickSpawnReq` del anfitrión (t 39,787, `04 01 00 10 A0 00 | 0,1,2,0xF,
  0x10,0x14,0x17`) recibe al instante 7 `SyncSceneObjectAck` + `ActorLoadDataAck` de
  `0xB0100066`…`0xB010006C`. La de la copia (t 99,091, `04 01 F0 10 A0 00 | 0,1,2,0xF,0x14,0x17`)
  no recibe nada: el servidor no conoce `0xA010F001`. Pasa igual en Shadow (`arbol-cuerda-2`
  §3.2).
- Aunque la herramienta existiera, `0x17A6210` no corre para la copia (puerta `0x86A557`).

Con cualquiera de las dos lecturas, en la copia B se queda en un hueso del cuerpo o en la raíz del
actor: **la cuerda empieza bien en la mano, pero acaba a poca distancia y con la dirección del
hueso.** Encaja con lo observado en la prueba (la cuerda solo cubre el principio del recorrido y
apunta a otro lado) y con lo de antes (se quedaba en el brazo; solo se veía una parte).

---

## 4. Comprobaciones, una a una

### 4.1 ¿El punto `+0x26` del 2763 está en coordenadas del emisor o le falta un desplazamiento? — **No**

Es un punto **local a la pieza** (o al objeto de escena), y no de mundo. Los tres valores tienen
`x`, `z` < 0,4 m y `y` de 6,5–9 m: un punto del tronco. Como el árbol es el mismo objeto de escena
en los dos PC, el punto vale igual en los dos. El sector de mundo (`+0x28` del `xf`) no participa
porque `0x5A5370` lo compone con la transformación de la pieza (`vf 0x3B0`) o del objeto de escena
(`vf 0x38`). La regla del mod para puntos de mundo (`|x|` o `|z|` > 1000 → escena a cero) no se
activa aquí. Prueba práctica: con estos datos, `0x5A4760` (en `bend_tree`) da un punto que dobla el
árbol hacia el lado correcto.

### 4.2 ¿Pieza o escena equivocadas? — **No para la cuerda**

- La escena es el id de 16 B, igual en los dos PC (`arbol-cuerda-fallo` §3.1).
- La pieza es la del propio enganche del anfitrión a ese árbol, 102 s antes y sin recargas entre
  medias. La copia llega a la fase 3 (`0x5A40E0` resuelve en cada fotograma; si no, `0x59D67E`
  soltaría) y `bend_tree` encuentra el árbol (`p+0x18` puesto, 395 flexiones).
- **Dato raro, sin efecto en el síntoma:** el jugador local tiene en su entrada `+0x40` la pieza
  `0x8000000004000BDC`, mientras que su `DoFrameEventReq` y su 2763 llevan `0x8000000002001170`
  (este segundo número no aparece en ningún paquete de la sesión). El cliente local cambia la pieza
  de su ranura después del enganche, o la toma de otra fuente (`0x593B50`, `0x5947AD`…`0x594826`
  sustituye la pieza del objetivo por la del cuerpo principal del objeto de escena para la
  petición). **(Hipótesis)** El local se queda con el cuerpo que tocó el rayo (otra parte del
  tronco) y el resto del mundo recibe el principal. Si los dos cuerpos tuvieran orígenes distintos,
  el punto de la copia quedaría más arriba o más abajo en el tronco, pero seguiría en el árbol.
  Añadir `+0x40` y la posición del objetivo de los dos a la sonda (§6.1).

### 4.3 ¿La estela no se actualiza? — **No aplica**

`+A98 = 0` en los dos, así que `0x59C660`/`0x59CAF0` no corren para nadie (§2.1). La estela vacía
es normal.

### 4.4 ¿Los nodos A/B, la estela `+0xAA8` y el punto `0x59CAF0` fijan los extremos y la dirección?

- La dirección y los extremos los dan **solo** las posiciones de mundo de A (`comp+0x950`) y B
  (`comp+0x958`), más los puntos intermedios de `comp+0xCE0`, y los monta `0x5AEAA0` (§2.2).
- `0x59CAF0` y `+0xAA8` no intervienen con árboles.
- La punta (`comp+0x928`) es un efecto aparte. Se coloca en el objetivo en cada fotograma, también
  en la copia, pero no es un extremo de la malla.

### 4.5 ¿R1 lo empeoró?

- **R1 no puede hacer que la cuerda apunte a otro lado.** Solo cambia `x` (`+AB8`) y, con ella,
  `_targetColorLerpRatio`, `_darkhandBeginOffset` y `_init`/`_endScaleFromIndexRatio`.
  `_sourcePosition` es la posición de A, y no depende de R1.
- **R1 deja la copia distinta del jugador local**, que en la fase 3 tiene `+AC0 0`, `+ABC 0`,
  `+AB8 0`. **(Hipótesis, baja-media)** Con `x = 1`, los parámetros de escala por índice pueden
  acortar la parte visible de la malla, lo que empeoraría que solo se vea el principio.
- **Recomendación:** apagar R1 en la fase 3. **(Hipótesis)** El estado encendido puede ser propio
  del vuelo (fase 2). Si R0' lo confirma, dejarlo solo en la fase 2.

---

## 5. Causa raíz

| # | Afirmación | Tipo |
|---|---|---|
| 1 | La forma de la cuerda de un árbol es `[A, comp+0xCE0 al revés, B]`, montada por `0x5AEAA0` en cada fotograma | hecho (código); que `[0x6C99968] ≠ 0` es una deducción |
| 2 | A y B son nodos colgados de huesos (`cfg+0xB0`/`+0xB4`) de una pieza visual (`cfg+0xAC`) o de la raíz del actor, y ningún código que corre en la copia los mueve | hecho (código) |
| 3 | En el jugador local B acaba en el árbol porque `0x17A6210` (solo local) mueve el objeto acoplado del que cuelga | **hipótesis, media** |
| 4 | La copia no tiene herramienta (su 2249 no tiene respuesta) | hecho (captura) |
| 5 | Los datos del 2763 (punto, pieza, escena) son correctos para la copia | hecho (mismo árbol y mismo sitio que el enganche local; fase 3; flexión correcta) |
| 6 | R1 hace que el efecto de la copia (1/1/1) difiera del local (0/0/0) en la fase 3 | hecho (sondas) |

**Conclusión:** la cuerda de la copia va de la mano a un hueso junto al cuerpo, porque su extremo B
no se lleva al punto de enganche. No es un fallo de coordenadas ni de la pieza.

---

## 6. Arreglo

### 6.1 R0' — sonda de posiciones (primero, o en la misma build que R5)

Un `create_mid` en el sitio de R5 (§6.2), en el hilo del tick, con `__try`. Guarda los valores en
variables atómicas y `puppet_tick` los imprime. Lo hace para la copia y **también para el jugador
local** (`[comp+8] == g_own_actor`), y para el local cada 1 s mientras esté en la fase 2 o 3 (hoy
solo hay una muestra).

| Dato | Cómo |
|---|---|
| objetivo `T` | `0x5A40E0(comp, 0, &T)` (el mismo que llama `0x5AEAA0` justo después) |
| pos(A), pos(B), punta `S+0x00`, cabeza `S+0x10`, manos `S+0x68` | `0x26D3940(nodo, &xf)` (devuelve el `xf` con la posición de mundo ya sumada al sector), `nodo = [[comp+off]+8] − 0x28` |
| **`|B − T|`**, `|A − S+0x68|` | calculado |
| padre de A y B | `[[nodo+0x80]+8]` (asa del padre, `0x26D3840`), en hex |
| `n = u32 [comp+0xCE8]` | lectura |
| `u8 [exe+0x6C99968]` | lectura |
| pieza `u64 [comp+0x40]` | lectura |
| escena del personaje | `m = [[[[CC+0x130]]+8]+0x78]`: `u32 [m+0x60]` (n) y las claves `u32 [[m+0x58] + i·16]`. Diferencia entre copia y local = piezas que le faltan a la copia |

Formato sugerido:
`rope ends | copy: T (x,y,z) A (…) B (…) |B-T| 12.8 |A-hands| 0.1 parentB 0x… n 0 g68 1 piece 0x… scene 23 keys | ours: …`

Esperado si la hipótesis es buena: local `|B−T| ≈ 0`; copia `|B−T|` ≈ 13–15 m (B junto al cuerpo);
el local tiene en la escena del personaje una o más claves que la copia no tiene.

### 6.2 R5 — llevar B al punto de enganche en la copia (el arreglo)

**Dónde:** `safetyhook::create_mid` en **`exe+0x59D89A`**, dentro de `0x59D470` (tick por ranura).

- Bytes: `44 0F B6 C6` (`movzx r8d, sil`) `C5 F8 28 CE` (`vmovaps xmm1, xmm6`) `48 8B CF`
  (`mov rcx, rdi`) `E8 F6 11 01 00` (`call 0x5AEAA0`). Comprobarlos antes de instalar, como se hace
  en `puppet_install_rope_bend`.
- Es un límite de instrucción y un destino de salto (`0x59D802`, `0x59D813`), pero ningún salto cae
  dentro de los bytes que se sustituyen (`0x59D89B`…`0x59D8A1`), así que vale para un mid-hook.
- Registros: `rdi = comp`, `sil = ranura`, `xmm6 = dt`.
- Corre en el mismo trabajo que coloca la punta (`0x5AEAA0`), justo **antes** de que esta lea A y
  B. Es mejor sitio que `0x86A576`, que solo registra el tick y puede ir en otro momento o hilo.

**Qué:**

```
on_slot_post(ctx):
  si !g_rope_end: salir
  comp = ctx.rdi; r = u8(ctx.rsi)
  si r != 0: salir                                   // árbol = ranura 0 (+4 del 2763)
  __try {
    si [comp+8] != g_copy_actor || !g_copy_hooked: salir
    si u8 [comp+0x20] != 0 || u8 [comp+0x9BC] != 3: salir
    h = [comp+0x958]; o = h ? [h+8] : 0
    si !o || u8 [o+0x15] != 0: salir                 // nodo B vivo
    alignas(16) xf[0x30] = { 1,1,1, 0,0,0,1, 0,0,0, u32 0 }   // como 0x5D76280
    si !0x5A40E0(comp, 0, xf): salir                 // la misma llamada que hace 0x5AEAA0
    u32 xf+0x28 = 0
    0x50EA00(xf)                                     // reparte la posición en sector + resto
    0x26D4B40(o − 0x28, xf, 0, 1)                    // igual que con la punta en 0x5AF51D
  } __except { g_rope_end = false; log una vez }
```

- Es lo mismo que hace `0x5AEAA0` con la punta, en el mismo hilo y con las mismas funciones. Como
  B está colgado de un padre, `0x26D4B40` → `0x26D5080` calcula la transformación local respecto a
  ese padre. Hay que hacerlo **en cada fotograma**, porque el padre (un hueso o la raíz) se mueve
  con la animación.
- Dejar de escribir al salir de la fase 3 o al soltar. Los nodos los destruye la suelta y se crean
  nuevos en el siguiente `0x5ABBC0`, así que se leen siempre desde `comp+0x958`.
- Tras un fallo, apagarlo para toda la sesión, como `bend_tree`.
- Con una opción `[dev]` propia (por ejemplo `rope_end = 1`) para poder comparar.

**Opcional R5b (si R0' dice `|A − manos|` > 0,5 m en la copia y no en el local):** poner A en la
transformación del nodo `S+0x68` (`comp+0x990`, que `0x5AEAA0` coloca entre las dos manos;
valor del fotograma anterior), con `0x26D3940(nodo68, &xf)` → `0x26D4B40(nodoA, &xf, 0, 1)` en el
mismo hook.

**Opcional R5c, fase 2 (vuelo):** con B en la cabeza `S+0x10` (`comp+0x938`, que `0x598F80`
interpola en el vuelo), la cuerda crece con el vuelo en vez de aparecer de golpe al enganchar.
Hacerlo solo si R0' enseña que el local hace algo parecido.

### 6.3 R6 — R1 apagado en la fase 3

En `rope_effect`, cambiar `on` de "fase 2 o 3" a "fase 2", o desactivar R1 por defecto
(`g_rope_effect = false`) hasta que R0' mida el local durante el vuelo. Motivo: el local tiene
`+AC0 0 +ABC 0 +AB8 0` en la fase 3 (§1.2). R2 (flexión) se queda como está: no hay muestra del
local tirando con la que compararlo.

### 6.4 Lo que no hay que hacer

| Opción | Por qué no |
|---|---|
| Cambiar el punto `+0x26` o el sector | Es local a la pieza y ya es correcto (§4.1) |
| Volver a la pieza `-1` | Funciona igual de bien para el extremo, pero pierde la flexión física |
| Llamar a `0x17A6210` para la copia | No tiene objetos acoplados con esa regla (sin herramienta): no haría nada |
| Pedir la herramienta para la copia | El servidor no conoce la clave de la copia (2249 sin respuesta), y `0x17A6210` seguiría siendo solo local |
| Volver a enganchar B a la punta (`0x26DAB60`) | Habría que desengancharlo antes, y no se ha analizado cómo destruye los nodos la suelta (`0x5AE410`). El arreglo por fotograma es más seguro |
| Rellenar `comp+0xCE0` | Son rodeos de obstáculos; no hacen falta |

---

## 7. Riesgos de R5

- **Hilo:** el mismo trabajo y el mismo momento en que el juego coloca la punta y otros tres nodos
  de esa cuerda con `0x26D4B40`. Sin carreras nuevas.
- **Excepción:** todo dentro de `__try`. `0x5A40E0` es una consulta que `0x5AEAA0` repite enseguida.
  `0x50EA00` es aritmética sobre el búfer. `0x26D4B40` toca solo el nodo B de la copia.
- **Coste:** una comprobación de puntero por ranura y actor. Solo hace trabajo para la copia
  enganchada.
- **Visual:** si la hipótesis de §3.3 fallara (B ya estuviera bien y el error fuera A), R5 no
  empeora nada: B iría donde ya debería estar. R0' lo decide.
- **Crash = informe a Pearl Abyss:** mantener la comprobación de los bytes del sitio del hook y el
  apagado tras el primer fallo.

---

## 8. Pruebas

| # | Prueba | Decide |
|---|---|---|
| T0 | R0' en un PC (`cdmp_peer`): engancharse con el jugador local y, después, enganche de la copia (mismo árbol si se puede) | `|B−T|` local ≈ 0 y copia ≫ 0; claves de la escena del personaje que faltan en la copia; `[0x6C99968]`; `n` de `+0xCE8` |
| T1 | R5 + R6 en un PC, con captura `CopyFromScreen` de la cuerda de la copia | que va de la mano al tronco |
| T2 | Dos PC (Shadow invitado), los dos tirando por turnos del mismo árbol | lo mismo en los dos sentidos; el árbol se sigue doblando |
| T3 | Enganchar, soltar (`d20295ad` y `1f4ef471`) y volver a enganchar 3 veces | ninguna escritura fuera de la fase 3; sin excepciones; nodos nuevos en cada enganche |

---

## 9. Confianza

| Afirmación | Confianza |
|---|---|
| Los datos del 2763 de la copia son correctos (punto local, pieza y escena) | alta |
| `0x59CAF0`/estela no intervienen con árboles (`+A98 = 0` en los dos) | alta (sonda) |
| La geometría es `[A, CE0 al revés, B]` de `0x5AEAA0` | alta (código); `[0x6C99968] ≠ 0` deducido |
| A/B solo se crean en `0x5AB7A0`; nada que corra en la copia los mueve | alta (todos los llamadores de `0x26D4B40`/`0x26D5E60`/`0x26DAB60` en el código de la cuerda) |
| B llega al árbol en el local por `0x17A6210` y la herramienta | **media** (único escritor de transformaciones solo local; la copia no tiene herramienta) |
| R5 arregla el síntoma | media-alta (no depende de qué mueve B en el local) |
| R1 contribuye a la cuerda "a medias" | baja-media |
| Pieza local `0x…4000BDC` ≠ pieza del Ack | hecho; su efecto, sin aclarar |

---

## 10. Archivos nuevos (`build/research/interactions/rope5/`)

| Archivo | Contenido |
|---|---|
| `cap5.py` | lector de capturas `.bin` (`CDPK`) con caché `.pkl` en el directorio actual |
| `dump2763.py <captura.bin>` | todos los mensajes `*Catch*` con el 2763 decodificado (clave, ranura, clase, escena, pieza, punto, quat) |
| `findpiece.py <captura.bin> <hex>…` | busca secuencias de bytes (ids de pieza o de escena) en todos los paquetes |
| `c_5ab7a0_full.asm` | creación de los nodos A/B/`S+0x18`/`S+0x20` y su enganche a la pieza o al actor |
| `c_466740_scene_map_find.asm` | búsqueda por hash en el mapa de la escena del personaje |
| `c_5a60d0_wrap_getter.asm` | lector de la lista `comp+0xCE0` |
| `c_11a5400_ropeswing_wrap.asm` | escritor de `comp+0xCE0` (RopeSwing, pruebas de choque) |
| `c_26d4b40_node_set_world.asm` | fijar la transformación de mundo de un nodo |
| `c_50ea00_xf_sectors.asm` | reparto de la posición en sectores de 1000 m |

Ya volcados y usados aquí: `rope2/c_5aeaa0_slot_post.asm`, `rope2/c_59d470.asm`,
`rope2/c_5abbc0_rope_start.asm`, `rope2/c_5a1530.asm`, `rope2/c_593b50.asm`,
`rope3/c_5956a0.asm`, `rope4/c_17a6210_pivot_matrix.asm`, `rope4/c_26d3840_node_world_xf.asm` y
`interactions/c_599e50_remotecatch_update_piece.asm`.

---

## 11. Resumen corto

1. Mismo árbol, misma pieza y mismo sitio que el enganche del anfitrión (20:37:48): los datos del
   2763 de la copia son correctos. El punto `+0x26` es local a la pieza.
2. `+A98 = 0` y estela vacía en el jugador local y en la copia: `0x59CAF0` y `+0xAA8` no
   intervienen con árboles.
3. La malla la monta `0x5AEAA0` en cada fotograma: `[pos A, comp+0xCE0 al revés, pos B]`.
4. A (`+0x950`) y B (`+0x958`) cuelgan de huesos `cfg+0xB0`/`+0xB4` de una pieza visual
   `cfg+0xAC` (o de la raíz del actor), y nada que corra en la copia los mueve.
5. En el jugador local, B llega al árbol por código solo local (`0x17A6210`, que mueve la
   herramienta acoplada). La copia no tiene herramienta: su 2249 queda sin respuesta.
6. Por eso la cuerda de la copia va de la mano a un hueso junto al cuerpo: corta y mal orientada.
7. R1 no tiene que ver con la dirección, y además el local está a `+AC0 0 / +AB8 0` en la fase 3
   mientras la copia está a 1: apagarlo en la fase 3.
8. Arreglo R5: mid-hook en `0x59D89A` (antes de `call 0x5AEAA0`; `rdi = comp`, `sil = ranura`).
9. Para la copia, ranura 0, tipo 0 y fase 3: `0x5A40E0(comp, 0, &xf)` → `0x50EA00` →
   `0x26D4B40(nodoB, &xf, 0, 1)` en cada fotograma, con SEH y apagado al primer fallo.
10. R0': sonda de `|B − T|`, `|A − manos|`, padres, `+0xCE8`, `[0x6C99968]` y claves de la escena
    del personaje, para la copia y para el local cada 1 s (hoy solo hay una muestra del local).
11. Pendiente: por qué la pieza de la ranura local (`0x…4000BDC`) no es la del Ack
    (`0x…2001170`).
