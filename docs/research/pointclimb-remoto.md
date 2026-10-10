# Por qué el clon (modo 2) no se mueve en PointClimb (método 0x12)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA.
Análisis estático de solo lectura (capstone sobre el exe). No se ejecutó el juego.
Desensamblados usados: `build/research/pointclimb/*.asm`.

## 0. Resumen

**Causa:** `PointClimbMovementMethod::Enter` (vf7, `0x691400`) **devuelve false en el clon**, y el
controlador guarda ese resultado en el byte **`method+0x44`** ("método activo"). Con
`method+0x44 == 0`, dos puertas del paso por frame **se saltan todo**:

| Dónde | Código | Efecto |
|---|---|---|
| `0x88F4E0` (paso de movimiento) | `0x890A78: cmp byte [method+0x44],0 / je 0x891103` | No se llama a `0x87B760` → no se ejecuta `vf2 = 0xAB0540` (no se interpola) |
| `0x891180` (aplicar) | `0x8913B6: cmp byte [method+0x44],0 / je 0x891E5C` | Sale antes de `SetPosition 0x9FCA60` → `TS+0x5A0` queda en `0xF` |

Encaja con todas las medidas en vivo: el búfer `mc+0x3D0` sigue recibiendo muestras (la
inserción no mira el método), `TS+0x534` sube, pero `TS+0xB4`, la física y `TS+0x5A0 = 0xF`
quedan congelados. Al salir del método 18 el método nuevo tiene `+0x44 = 1`, `vf2` interpola
a `now − 200 ms` y el clon salta ~10 m (pop).

**Por qué falla `Enter` solo en el clon:** PointClimb toma el "punto de escalada" de los
huecos del objeto de travesía/escalada `D = [CC+0xD8]` (`D+0x19C8`, `+0x1B48`, `+0x1BC8`,
`+0x1C48`), que **solo rellena el sensor del personaje controlado localmente**
(`0x7087B0` → `0x7089B0`). Para un actor remoto esos huecos están vacíos, el handle de punto
en `method+0x50` no es válido y `0x696EF0` devuelve false.

**Arreglo mínimo:** poner `byte [method_PointClimb_del_clon + 0x44] = 1` (o forzar el
retorno de `Enter` a 1 en el clon). `vf2 = 0xAB0540` es genérico: solo lee los búferes de red
del `mc` y no usa ningún estado interno de PointClimb, así que el clon seguirá las
posiciones de red igual que en Normal o WallUp.

---

## 1. Cadena verificada (paso por frame)

```
0x88EB70(mc, dt)
  ├ 0x87BA80(mc) → true si [[actor+0x68]+0x70]+0x40 != 0 o byte [[CC+0x100]+8] ∈ {2,4..8}
  │      (anclado / montado). En ese caso NO se llama a 0x891180.   [no es el caso: mc+0x70=0]
  ├ ctx = 0xAB0870(); ctx+0x70 = byte mc+0x70
  ├ 0x88F4E0(mc, dt, &ctx, &out)
  │     modo = 0x88E8E0(mc)                      (2 = remoto)
  │     switch byte ctx+0x70: 0/1 → 0x890A58; 2 → 0x890BC5; 3 → 0x890AB7 (no interpolan)
  │     0x890A58: m = [mc + 0xF8 + 8·byte[mc+0xF0]]
  │               if (!m || byte[m+0x44]==0) → fin           ◄── PUERTA 1
  │               modo 2 → 0x87B760 → jump table 0x87B7F4 (índice 0x12 → 0x87B797 = vf2)
  │                         → 0xAB0540 → 0x88E6A0 → updateInterpolatePositionData 0x11B2420
  │                         → tail-jmp 0x888190 (solo actúa si mc+0x550 != 0: no aplica)
  └ 0x891180(mc, &ctx, &out)
        switch byte ctx+0x70: 0/1 → 0x891396; 2/3 → tipo 7
        0x891396: m = método actual; if (!m || byte[m+0x44]==0) → fin   ◄── PUERTA 2
        ctx+0x70 == 0 → 0x8917FB: método 0xD → tipo 1; out+0x10A → tipo 4; out+0x10D → tipo 7;
                        si no, tipo 0xC
        0x891A2E: si pos(out) ≈ pos actual y velocidad ≈ 0 → no hay SetPosition
        0x891C6F / 0x891C85: SetPosition 0x9FCA60(TS, &out.pos, tipo, …)
```

Notas:
- La jump table de `0x87B760` **no** trata el 0x12 de forma especial (va a `vf2` como el 0–16, 18
  y 21). Solo 17 (RopeSwing → `vf10`), 19 (AiJump → `vf10`) y 20 (Snowboard → nada) difieren.
- `0xAB0540` tiene una salida temprana `actor->vf(0x188)(2)` (`0x4FA620`: `byte[actor+0xC8] == 2`).
  Es por actor, no por método; como andar funciona, no es la causa.
- `cmp byte [rsp+0xF0],1` (`0xAB070C`) es solo el flag "búfer de rotación vacío"; no corta nada.
- `0x888190` no mira el método; con `mc+0x550 == 0` sale sin tocar nada.
- En `0x891180` no hay ninguna comparación con 0x12. La de `0x8917FB` es con 0xD (Sequencer).

## 2. Quién escribe `method+0x44`

- Constructores (`0x88C530`, PointClimb `0x915D780`): `+0x44 = 0`.
- **Único escritor en tiempo de juego: `0x87D00A`** en `0x87CBC0` (`X::Update` del controlador),
  dentro del bloque de cambio de acción (solo se ejecuta si el 3.er argumento `r8b` = "acción
  nueva", es decir, al procesar cada `StartAction`):

```
0x87CF21  r14 = byte [[[accion]+0x18]+0x38]+0x40      índice de método de la acción
0x87CFB8  si cambia: viejo->vf8 (+0x40)  (Exit)
0x87CFE4  byte mc+0xF0 = r14
0x87CFEB  method+0x28..+0x43 = parámetros de la acción ([..+0x38]+0x24..+0x3F)
0x87CFFE  dl = (viejo == nuevo)
0x87D007  al = method->vf7 (+0x38)(dl)              Enter
0x87D00A  byte [method+0x44] = al
```

`Enter` se vuelve a llamar con `dl = 1` en **cada** acción nueva del mismo método (cada
"paso" de la escalada), y cada vez sobrescribe `+0x44`.

Lecturas de `+0x44`: solo `0x890A78` y `0x8913B6` en `0x870000–0x8A0000`; ninguna en el código
de los métodos (`0x680000–0x6B2000`).

## 3. Por qué `PointClimb::Enter` (`0x691400`) devuelve false en el clon

Vtable de PointClimb `exe+0x5597028`: vf0 `0x691370`, vf1 `0x691DB0`, vf2 `0xAB0540`,
vf3 `0x692370`, vf4 `0x692380`, vf6 `0x692730`, **vf7 `0x691400` Enter**, **vf8 `0x691810` Exit**,
vf9 nop. (La entrada "vf10 = `0x5DBC2B8`" no es una función: la vtable de PointClimb tiene 10
entradas y `0x5597078` es el puntero COL de la vtable siguiente, RopeSwing.)

`Enter(this, bool mismoMetodo)`:
- `+0x238 = (byte this+0x34 != 0) ? 1 : (u32 [[[CC+0x88+8·idx]+0x18]+4] != 0)`; la global
  `byte exe+0x6C9D5A8` lo fuerza a 1.
- **Reentrada** (`dl = 1`) → `0x692A30`: exige `byte this+0x48 != 0` (solo se pone a 1 en una
  entrada nueva con éxito, `0x691709`) y un handle de punto válido en `this+0x50`; luego
  `0x696EF0` (según la subacción `this+0x2C` = 1, 3 o 4) y `0x693F90`. Si algo falla → 0.
- **Entrada nueva** (`dl = 0`) → `0x696EF0` y `0x693F90`; si alguna falla → 0.

`0x696EF0` (búsqueda del punto de escalada) rellena `this+0x50` (handle de 0x80 B) desde:
- `byte this+0x35 != 0` → sondeo alrededor de la posición actual (`TS+0x98`) con
  `0x713DE0([CC+0xD8], …)`;
- subacción 4 → `D+0x1B48`; 5 → `D+0x1BC8`;
- en otro caso, `0x699310(this, this+0x28, …)`, y si falla, `D+0x1C48 + 128·[this+0x8C]` o
  `D+0x19C8 + 128·[this+0x88]`.

Después lo valida: `[[h+8]+8]` vivo (`+0x15 == 0`) y de tipo `+0x30 ≥ 5`, `[[h]+8]` vivo,
`[h+0x20]` con `+0x3C` y `+0x3D` no nulos. Si no, devuelve 0 (`0x69725A`).

**`D = [CC+0xD8]` solo se rellena en el personaje controlado aquí.** `D::Update 0x7087B0`
(desde `0x85FBA0`) llama al sensor completo `0x7089B0` (el que escribe `D+0x19C8…`, ver
`0x708F74`) **solo si** `([M2+0x58] == [actor+0xA0] && byte actor+0x94)` **o**
`0x17F2A50(actor)` (= control local, condición del modo 1). Al clon (modo 2) le toca la rama
ligera y sus huecos de punto de escalada se quedan vacíos → handle inválido → `Enter = 0` →
`+0x44 = 0`.

(Hipótesis menor, sin efecto en la conclusión: en la primera entrada el sondeo `+0x35` podría
llegar a acertar porque el clon está justo en el punto de agarre; pero en la siguiente acción
la reentrada vuelve a fallar porque el clon sigue congelado ahí y `D` está vacío.)

## 4. Mensajes de red relacionados

En `troc_messages_2976.csv` **no existe** ningún mensaje de PointClimb, punto de escalada,
agarre o colgarse. Los únicos de escalada o travesía son:
- `UpdateWallUpDataReq 2851` / `UpdateWallUpDataAck 2893` (cliente `0xB64460`): escriben los
  campos de WallUp (`D+0xEE0…+0x1200`), no los huecos de PointClimb (`D+0x19C8…+0x1C48`).
- `PreemptActionPointAck 2167` (`0xB9BB50`) y sus Req: son puntos de acción o interacción, no
  de escalada.

Los parámetros que decide `Enter` (`this+0x28…+0x43`: subacción `+0x2C`, sondeo `+0x35`…) salen
de la **tabla estática de la acción**, no del paquete. `StartActionAck` y `MoveActorAck` no
llevan ningún campo que llegue a `D` ni a `method+0x50`.

## 5. Intervención propuesta (de menos a más invasiva)

### (a) Solo paquetes: no hay una solución limpia
- Ningún mensaje rellena `D+0x19C8…` ni el handle del método.
- La única vía es **no meter al clon en el método 18**: sustituir los `StartActionAck` de
  PointClimb por una acción cuyo método tenga un `Enter` trivial (Normal o AnimationAll). El
  clon seguiría la posición, pero con una animación incorrecta. Es un parche de último recurso.

### (b) Un byte en la memoria del clon (recomendado para probar ya)
```
CC = [[clon+0x68]+0x40];  mc = [CC+0xC8]
si byte[mc+0xF0] == 0x12:
    m = [mc + 0xF8 + 0x12*8]          (= [mc+0x188])
    comprobar [m] == exe+0x5597028, [m+0x18] == CC, [m+0x20] == clon
    si byte[m+0x44] == 0:  byte[m+0x44] = 1
```
- Hay que **reaplicarlo**: `Enter` lo vuelve a poner a 0 en cada `StartAction` del clon
  (cada paso de la escalada). Basta con sondear desde el hilo worker cada 1–5 ms, o justo
  después de inyectar cada `StartActionAck` (que se procesa en el hilo principal un poco más
  tarde, así que mejor un sondeo corto).
- Una ventana de 1–2 frames con `+0x44 = 0` solo congela esos frames: la interpolación es
  absoluta en el tiempo (`now − 200 ms`), así que se recupera sin pop.
- Por qué es seguro: con `+0x44 = 1`, en modo 2 solo se ejecutan `0xAB0540` (genérica: lee
  `mc+0x3D0`/`+0x488` y TS) y `0x891180` (tipo `0xC`, como al andar). Las funciones propias de
  PointClimb con lógica local (vf1 `0x691DB0` y demás) solo se llaman en el modo 3
  (`0x879C50`). Exit (`0x691810`) ya se llama hoy con `+0x44 = 0` y no lee ese byte. Proteger
  con SEH y hacer copia de `save/` antes de probar.

### (c) Hook (elimina la carrera)
- Opción 1: **hook inline en la entrada de `0x691400`** (inicio de función, permitido). Llamar
  a la original y, si `this+0x20` es un actor remoto (`u32[actor+0x90] != 0` o
  `0x88E8E0([this+0x18]+0xC8) == 2`), devolver 1.
- Opción 2: `safetyhook::create_mid` en `0x87D00A` (justo antes de `mov [r15+0x44], al`): si
  `[r15] == exe+0x5597028` y `[r15+0x20]` es el clon, poner `al = 1`.

Prefiero la opción 1 por estar en el inicio de una función. Cualquiera de las dos cubre la
entrada y todas las reentradas.

## 6. Comprobación en vivo (solo lectura)

Durante una escalada larga del clon:
- `byte [[mc+0x188]+0x44]` → se espera **0** (y `+0x48` = 0).
- En el jugador local, el mismo campo → 1.
- Opcional: los huecos `[CC_clon+0xD8]+0x19C8` / `+0x1C48` del clon, vacíos frente a los del
  jugador.

Si al escribir `+0x44 = 1` el `TS+0x5A0` del clon pasa a `0xC` en los frames siguientes y
`TS+0xB4` sube con `TS+0x534`, queda confirmado.
