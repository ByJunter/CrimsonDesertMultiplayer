# Muerte realista de los enemigos que remata el invitado

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`)
y valen solo para esta build.

Método:
- re-análisis de **448 muertes de enemigos** en todas las capturas (`bin64\cdmp_packets_*.bin`,
  incluidas las de la prueba con dos PC del 6 de octubre, y `D:\cdmp-grabaciones\*.bin`);
- el código actual de `guest_hits.cpp` (remate letal) y `enemy_relay.cpp` (relevo de la muerte);
- análisis estático de solo lectura cuando hizo falta.

El juego no se ejecutó y no se tocó `src/`. Continúa
[pendiente-recompensas-progreso.md](pendiente-recompensas-progreso.md) §4-5,
[pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) §3 y
[pendiente-proyectiles.md](pendiente-proyectiles.md) §1.3.

Marcas:
- **[verificado]**: leído en el código;
- **[captura]**: medido en capturas (se da el script);
- **[inferido]**: deducción pendiente de prueba en vivo (§6).

Notación: **H** anfitrión, **G** invitado, **E** enemigo de H, jugador `0xA0100001`, marioneta
`0xA010F001`, ATA = `PostProcessAttackResult_AttackTargetAck` (2922), AAR =
`ApplyActionRestrictionAck` (2162).

Scripts en `build/research/p_botin_muerte/` (§7).

---

## 0. Resumen

1. **La animación de muerte la elige el cliente de H a partir del golpe letal, no del tipo de muerte.**
   [captura]
   - `NotifyHandleDeadAck` es **idéntico** en una muerte por colisión y en una muerte normal: motivo 8,
     `u8 0`, sesión 1, `u8 0x0A` (380 normales y 25 por colisión, `nd.py`).
   - Lo único que cambia en el lote de la muerte es el **ATA letal**: la colisión `0xF2EE2678` llega
     siempre con el hash de ataque **`8710E690`**, niveles `+0x5D/+0x5E/+0x64 = 3/5/5` y **sin AAR**.
   - Con ese ATA, el cliente de H lanza una muerte "en volandas" de tres fases:
     `18136804 → 03A4E5C8 → 4F69D6A0` (11 de 24), `4BA26366 → DA8361EC → FF5894F8`,
     `70C5348E → 7B588FF7 → DC4E5F5E`, `738DB695`, `B3D1AAA6`. **Es el "choque" que ve el usuario.**
   - Desde la misma acción previa (`9B68944F`, en guardia), la flecha normal del jugador da
     `5B636249` y `KillCheatReq` da `8D31F4BC`/`8E1DC009`; la colisión da `18136804`.
   - El invitado ve lo mismo: su copia repite el `StartActionReq` de muerte que manda el cliente de H
     (`enemy_relay`). **Arreglarlo en H lo arregla en los dos PC.**
2. **La muerte normal más frecuente es `5B636249`**: una sola acción, sin fases (93 muertes). La dan
   la flecha normal del jugador (`10481FDD`, 26 de 32) y los golpes de espada `C7D0779C` (12 de 15),
   `4CE4CF87`, `64F3FBC0` y `1B0CD1E5`, siempre **con AAR** en el lote.
3. **Receta recomendada (B1):** rematar con `AttackByCollisionReq` usando la **flecha normal del
   jugador, `ProjectileInfo 0xF653A168`**, en vez de `0xF2EE2678`.
   - Es una muerte nativa con atribución, como hoy: motivo 8, asesino = jugador de H, experiencia,
     `KillActor`, botín y barra de campamento.
   - Su ATA es `10481FDD` (niveles 2/8/3), que en las capturas lleva AAR y acaba en `5B636249`.
   - Cambia una constante y el punto de salida del proyectil [inferido; prueba M1].
4. **Alternativa (B2), si la flecha no impacta:** dejar la colisión y **reescribir en el cliente de H
   el ATA letal** (`8710E690`) con la plantilla del ataque real del invitado, en el mismo gancho de
   `ClientNet::OnRecvPacket` (`0x35CA00`) que ya usa el sniffer.
   - Es más fiel (espada → muerte de espada), pero hace falta modificar un paquete recibido, cosa que
     hoy el mod no hace.
5. **Descartado:** `KillCheatReq` o la muerte "fatal" (sin experiencia ni crédito), y las variantes de
   `VaryStatCheatReq` (no llevan datos de ataque: no hay `KillActor`). Inyectar un ATA o un AAR
   "normal" antes del remate no sirve: el ATA de la colisión llega después y es el que manda.

---

## 1. Qué cambia entre una muerte normal y una por colisión [captura]

`da.py` recorre cada `NotifyHandleDeadAck` de un enemigo real (`0xB01…`, sin copias):
- el último ATA sobre él (≤ 300 ms antes; en la práctica, en el mismo lote);
- las inyecciones C→S del mod (≤ 1,5 s antes: `AttackByCollisionReq` y su `ProjectileInfo`,
  `KillCheatReq`, `VaryStatCheatReq`, `DieByRunOutFatalReq`);
- los tres primeros `StartActionReq` C→S del muerto después de la muerte (la animación que elige el
  cliente de H, que controla al enemigo).

Salida: `da_all.csv` (448 filas). Resumen (`da2.py`, `da3.py`):

| Cómo murió (asesino = jugador) | Muertes | Acción de muerte (primera) |
|---|---|---|
| Golpe nativo, motivo 8 | 171 | `A2192D1F` 24, `B090371B` 14, `5B636249` 11+5+5+4…, `DC4E5F5E` 5 |
| Agarre y lanzamiento (`ReleaseCatchResultAck`) | 90 | La de cada lanzamiento (`2F241A95`, `CF2A22C1`, `C4386831`…) |
| **Remate del mod: `AttackByCollisionReq` `0xF2EE2678`** | **23** | **`18136804` 10, `4BA26366` 4, `738DB695` 2, `B3D1AAA6` 2, `70C5348E` 2, `8DCAD131`, `EE27F594`** |
| `KillCheatReq` (motivo 16) | 10 | `8D31F4BC`, `8E1DC009`, `5B636249`, `B456159F` |
| Fatal → `DieByRunOutFatalReq` (motivo 15) | 5 | `C2DF23E1` (la salida del aturdimiento) |

Las 23 muertes por colisión son de `111538`, `193440`, `201913`, `205431`, `181505` (6 oct) y
`205626` (prueba con dos PC del 6 oct): por ejemplo `205626` t = 700,031, 722,850, 726,277,
738,589, 820,912, 833,066, 843,325 y 852,444.

### 1.1 La muerte depende del ataque letal [captura]

Acción de muerte según el hash del ATA letal y si el lote trae AAR (`db.py`):

| Hash del ATA letal | Niveles `+5D/+5E/+64` | AAR en el lote | Muertes | Acción de muerte |
|---|---|---|---|---|
| `10481FDD` (flecha normal del jugador, `F653A168`) | 2/8/3 | Sí | 32 | **`5B636249` 26**, `57BB3258` 3, otras 3 |
| `C7D0779C` (espada) | 2/1/3 | Sí | 15 | **`5B636249` 12**, `AF91164D` 2 |
| `4CE4CF87` / `64F3FBC0` / `1B0CD1E5` | 3/4/5, 3/2/5, — | Sí | 7 / 5 / 3 | **`5B636249`** en todas |
| `9F117782` | 2/5/3 | Sí | 25 | `A2192D1F` 25 (desde el tambaleo `90A9C337`) |
| `C139B667` | 4/2/8 | No | 17 | `B090371B` 16 |
| `9683E1EB` (otra flecha, `E912512D` 3/1) | 2/5/3 | Sí | 14 | variada (`8D31F4BC`, `9ACCBB2D`, `AB10E82B`…) |
| **`8710E690` (colisión `F2EE2678`)** | **3/5/5** | **No** | **24** | **`18136804` 11, `4BA26366` 4, `738DB695`, `B3D1AAA6`, `70C5348E`** |

- Cada hash da casi siempre la misma familia de muerte. La acción previa también cuenta (la muerte
  `A2192D1F` sale del tambaleo `90A9C337`), pero desde la misma acción previa el hash decide:

  | Acción previa `9B68944F` (en guardia) | Muerte |
  |---|---|
  | Colisión `8710E690` | `18136804` ×4, `4BA26366`, `70C5348E`, `8DCAD131` |
  | Flecha `10481FDD` | `5B636249` |
  | `KillCheatReq` (sin ATA) | `8D31F4BC`, `8E1DC009` |
  | Espada `C139B667` / `9B7F6675` | `B090371B` |

- **Forma de las muertes** (`StartActionReq` siguientes, ms desde la muerte):
  - `5B636249`: una sola acción, sin más en 1,5 s (caer al suelo);
  - colisión: tres fases, `18136804` (~20 ms) → `03A4E5C8` (~310 ms) → `4F69D6A0` (~500 ms), o
    `4BA26366 → DA8361EC → FF5894F8`, o `70C5348E → 7B588FF7 → DC4E5F5E` (~1,3 s). Es el patrón de
    salir despedido, volar y quedar tendido [inferido por la forma; la animación no se vio].
- **El resto del lote** (`db.py`, 20 ms antes de la muerte):
  - normal (324): ATA 278, `VaryHPAck` 242, `OnPostBuffDamagedAck` 171, **AAR 134**, buff de golpe
    1105 ×84, 1120 ×25, 1106 ×9…;
  - colisión (23): ATA 23, `OnPostBuffDamagedAck` 23 (7 HP), `VaryHPAck` 20, **sin AAR ni buff de
    golpe**;
  - `KillCheatReq` (10): `VaryHPAck`, y ATA solo si hubo un golpe anterior.

### 1.2 `NotifyHandleDeadAck` no distingue [captura]

`nd.py`, `u32 muerto | u32 asesino | u8 motivo | u8 | u64 sesión | u8`:

| Clase | (motivo, u8, sesión, u8 final) | Muertes |
|---|---|---|
| Normal | (8, 0, 1, 10) | 380 |
| **Colisión** | **(8, 0, 1, 10)** | **25** |
| `KillCheatReq` | (16, 0, 1, 10) | 10 |
| Fatal | (15, 0, 1, 10) | 5 |
| Caída | (7, 0, 1, 10) | 15 |

La muerte por colisión es, para el servidor y para la parte común del cliente, una muerte normal.
La diferencia visual sale del ATA letal y del gráfico de acciones del cliente de H [inferido el
mecanismo; ver §2].

---

## 2. Dónde se decide [verificado + inferido]

- **Servidor:** la cadena de la muerte (`Status vf43 0x2941B60 → .xtls 0x10433BD0`, escritor de
  `NotifyHandleDeadAck` `0x29927E0`) escribe solo muerto, asesino y motivo
  (`pendiente-recompensas-progreso.md` §2.1). No hay ningún campo de "tipo de muerte".
- **Cliente, golpe** (`0xB59280 → 0x451020`, `pieza4-botin-reaccion.md` §3.1): el ATA guarda el golpe
  en el `ClientAttack` del objetivo, hace el empuje físico (`0x45C8A0`, flags `& 4`, vector
  `+0x35..+0x3D` y `AttackInfo+0x68` del hash) y manda el evento "golpeado" a la IA (`0x2083D90`,
  con los niveles `+0x5D/+0x5E/+0x64` y el hash).
- **Cliente, muerte** (`0xB764A0 → 0x634420 → .xtls 0x902F790 → 0x1797850 → .xtls 0xC76CF30`): marca
  muerto (`+0x272 = 0x100`) y llama a los `vf(0x150)/(0x300)/(0x308)` del actor. Lo demás está en
  `.xtls` (virtualizado).
- **El gráfico de acciones** tiene condiciones de muerte: existen `AICondition_CheckDead`,
  `AICondition_CheckDeadReason`, `AICondition_CheckDeadByFatal`, `AICondition_CheckDeadOrBreak` y
  `AICondition_IsDeathByDrowning` (RTTI). La rama de muerte se elige, con toda probabilidad, con el
  último golpe (su `AttackInfo` y niveles) y la acción en curso [inferido; encaja con §1.1].
- **Por qué la colisión "choca":** `0xF2EE2678` es el `ProjectileInfo` de una colisión física (un
  objeto que golpea), con `AttackInfo` `8710E690` de nivel alto (3/5/5) y sin tambaleo. Para el
  cliente es "le ha arrollado algo" y elige la muerte de salir despedido.

---

## 3. Opciones

Requisito: que la muerte siga siendo **normal para el servidor de H** (motivo 8, datos de ataque,
asesino = jugador de H). Si no, se pierden la experiencia y el `KillActor`
(`pendiente-recompensas-progreso.md` §2.2).

| | Qué | Muerte esperada | ¿Muerte normal (XP, `KillActor`, botín)? | Riesgo | Esfuerzo |
|---|---|---|---|---|---|
| **B1** | `AttackByCollisionReq` con **`ProjectileInfo 0xF653A168`** (la flecha normal del jugador) | `5B636249` (81 % con su ATA y AAR) | **Sí**: es un proyectil del jugador, como X2 | Que la flecha creada así no impacte o que gaste una flecha del inventario (M1). Puede verse la flecha un instante | **Muy bajo** |
| **B2** | Colisión como hoy + **reescribir en el cliente de H el ATA letal** (`8710E690`) con la plantilla del ataque del invitado (y, si tambalea, inyectar su AAR y buff justo antes) | La del ataque real (espada → `5B636249`/`B090371B`…; flecha → `5B636249`) | **Sí**: el servidor no se entera | Primer paquete recibido que el mod **modifica**; si la elección de la muerte no lee el ATA, no cambia nada (M2) | Medio |
| B3 | B1 con otra flecha del jugador (`E912512D`, hash `9683E1EB`) | Variada (`8D31F4BC`, `9ACCBB2D`…) | Sí | Igual que B1; menos uniforme | Muy bajo |
| B4 | `KillCheatReq` | `8D31F4BC`/`8E1DC009`/`5B636249` | **No** (motivo 16) | Sin XP para H | — (ya existe como reserva) |
| B5 | `VaryStatCheatReq` modos 0/1/2 hasta 0 | Fatal → `C2DF23E1` | **No** (sin datos de ataque; motivo 16 o 15) | Aturdimiento de 5 s | — |
| B6 | Inyectar un ATA o AAR "normal" en el cliente de H antes de la colisión | Ninguna: el ATA de la colisión llega después, en el lote de la muerte | Sí | No funciona por orden | — |
| B7 | `DoFrameEventReq` falso en el servidor de H con el jugador de H como atacante y el hash del ataque del invitado | Nativa | Sí | El servidor valida la acción en curso y el frame del atacante (`pieza4-dano-servidor.md` §1.2); el jugador de H no está haciendo ese ataque | Alto |

### 3.1 B1 en detalle (recomendada)

Cambio en `guest_hits.cpp` (el mod; aquí solo se describe):
- `kCollisionProjectile = 0xF653A168` (hoy `0xF2EE2678`).
- **Punto de salida:** hoy es el centro del enemigo + 1 m, que vale para una colisión de volumen pero
  no para una flecha (nacería dentro de la cápsula y podría no detectar el impacto). Proponer:
  `punto = pos(E) − dir × 1,5 m + (0, 1,2, 0)` y `dir` = de ese punto al pecho de E (con componente
  vertical 0). Así la flecha recorre 1,5 m y entra en E [inferido].
- El resto igual: `VaryStatCheatReq` hasta 1 HP, y `KillCheatReq` si no muere en 600 ms.

Paquete (44 B, `pieza4-dano-servidor.md` §2.c):
```
op 2815, slot 0, len 44:
  u32 E | u32 0xA0100001 | u32 0 | f32×3 punto | f32×3 dir | u32 0xF653A168 | u32 0
```

Respuesta esperada [captura de flechas reales, `pendiente-proyectiles.md` §1.3; inferido para la
vía por colisión]:
1. `CreateProjectileAck` (dueño `A0100001`, 289 B);
2. el cliente de H simula la flecha y manda `DoFrameEventReq` de tipo 1 con E en la lista;
3. lote: ATA `10481FDD` (flags `F7`, 2/8/3), AAR + buff de golpe 1105, `VaryHPAck` 0 (fuente
   `A0100001`), `NotifyHandleDeadAck(E, A0100001, 8)`, `AddSubLevelExperienceAck`, buff 20001 y
   `LootDrop…` si la variante tiene máscara 2;
4. `StartActionReq(E) → 5B636249` en ~20 ms.

Riesgos concretos:
- **Gasto de flechas:** el servidor crea el proyectil por `0x2BE11A0` sin pasar por el disparo del
  arma (`CreateProjectileFromFrameEventReq`). No se ha visto ningún descuento de flechas en la vía por
  colisión, pero con `F2EE2678` no había nada que descontar [inferido; mirar `RemoveItemToInventoryAck`
  y el contador del carcaj en M1].
- **Desafíos de maestría:** un remate con flecha puede contar para desafíos "de arco" del jugador de H
  (`Challenge_SealedArtifact_Mastery_*`, `KillActor` con condiciones) [inferido; menor].
- **Daño:** la flecha quita 28 HP (base, defensa 1,0) en vez de 7. Da igual con E a 1 HP. Si el
  `VaryStatCheatReq` previo no llegara, el remate quitaría 28 HP sin matar; la reserva de 600 ms
  (`KillCheatReq`) sigue cubriendo ese caso.

### 3.2 B2 en detalle (alternativa)

Cuándo: si en M1 la flecha no impacta, o si se quiere que cada arma tenga su muerte.

1. En `apply_hit`, al mandar el remate letal, guardar `g_finishing[E] = {ms, act}` con el ataque real
   del invitado (`act`, ya se recibe en el `GuestHit`).
2. En el gancho de entrada de `ClientNet::OnRecvPacket` (`0x35CA00`, `troc_sniffer.cpp`
   `on_recv_packet`, puntero del paquete en `r9`), si llega un ATA con:
   - `+0x00` = E de `g_finishing` (≤ 600 ms),
   - `+0x04` = nuestro jugador,
   - `+0x24` = `8710E690`,

   sobrescribir **en el búfer** (bajo SEH) con los campos de la plantilla `g_attacks[act].ata`:

   | Cable | Campo | Origen |
   |---|---|---|
   | `+0x08..+0x23` | flags y bloque del golpe | plantilla |
   | `+0x24` | hash de ataque | plantilla |
   | `+0x35/+0x39/+0x3D` | empuje | plantilla |
   | `+0x5D/+0x5E/+0x5F/+0x64` | niveles | plantilla |
   | `+0x00`, `+0x04`, ids u64 `+0x2D/+0x41/+0x49/+0x51`, posiciones `+0x69..+0x9A`, `+0x81/+0x82`, cuaternión `+0x9B` | — | **sin tocar** |

   Sin plantilla de ese ataque: la del último golpe de flecha `10481FDD` de H (siempre la hay si H ha
   disparado) o la de cualquier golpe `F7` con AAR.
3. Si la plantilla tambalea (P(AAR) ≥ 0,8, como ya calcula `apply_hit`), inyectar su AAR y su buff
   (id `0x8000F000 + n`, como en `guest_hits`) **antes** de dejar pasar el ATA, con el mismo
   `guarded_inject` del sniffer: en los lotes normales de muerte con tambaleo llegan AAR y buff junto
   al ATA (134 de 324).

Riesgos: es la primera vez que el mod cambia un paquete del servidor. Hay que hacerlo solo con el
tamaño exacto (211 B) y con los tres filtros de arriba. El daño, la muerte y el crédito no cambian
porque ya están decididos en el servidor.

---

## 4. Lo que no cambia

- **El crédito (experiencia, `KillActor`) depende de los datos de ataque**, no del hash: cualquier
  proyectil cuyo dueño sea el jugador de H vale (`pendiente-recompensas-progreso.md` §2.2).
- **El botín** depende solo de la variante y del motivo ≠ 0/3 (`pieza4-botin-reaccion.md` §1.2): no
  cambia con B1 ni con B2.
- **La copia del invitado** repite la acción de muerte que elige H. `enemy_relay` no necesita cambios.

---

## 5. Seguridad

- B1 no añade superficie: es la misma `AttackByCollisionReq` de hoy, con los mismos límites de
  `apply_hit` (enemigo hostil en combate, distancia, ritmo, H6). La clave de `ProjectileInfo` es
  **fija** en el código de H; nunca sale del paquete del invitado.
- B2 solo toca ATA de enemigos que el propio H acaba de rematar; la plantilla es de H. El invitado no
  puede elegir qué se escribe.

---

## 6. Pruebas en vivo

Un PC, `cdmp_peer` en eco (el eco ya produce `GuestHit` sobre las copias). Antes, copia de la partida
en `backups/`. Verificar con la captura y con `CopyFromScreen` (recortar el enemigo).

| # | Prueba | Predicción | Decide |
|---|---|---|---|
| **M1** | B1: remate con `0xF653A168` y el punto 1,5 m antes de E sobre un bandido `Tier_2` | `CreateProjectileAck` → `DoFrameEventReq` tipo 1 → ATA `10481FDD` + AAR → `NotifyHandleDeadAck(E, A0100001, 8)` + `AddSubLevelExperienceAck(521, 15)`; `StartActionReq(E) → 5B636249`. Sin `RemoveItemToInventoryAck` de flechas | B1 |
| M1b | Si M1 no impacta: el mismo con el punto en el centro de E (como hoy) | Igual que M1 | Punto de salida |
| **M2** | B2: colisión `F2EE2678` + ATA reescrito con una plantilla de espada `C7D0779C` | `StartActionReq(E) → 5B636249` (o la muerte de esa espada) en vez de `18136804` | Que la muerte la decide el ATA en el cliente |
| M3 | Control: la colisión de hoy sobre la misma familia | `18136804 → 03A4E5C8 → 4F69D6A0` | Línea base |
| M4 | Con dos PC: un remate de G con B1 | En H y en G, caída normal (captura de pantalla en los dos); H recibe la XP | Resultado final |

---

## 7. Scripts (`build/research/p_botin_muerte/`)

Ejecutar con `build/research/venv/Scripts/python.exe`. Usan `build/research/pieza4/pk.py` y su caché.

| Script | Qué hace |
|---|---|
| `da.py` → `da_all.csv` | Por cada muerte: ATA letal (hash, flags, niveles), inyecciones del mod antes, y los 3 `StartActionReq` del muerto después |
| `da2.py` | Acción de muerte por clase de muerte, y por acción de muerte → (clase, hash, flags) |
| `da3.py` | Acción de muerte por niveles del ATA, por hash y por acción previa |
| `db.py` | Paquetes S→C sobre el muerto en los 20 ms antes de la muerte (AAR, buffs) y muerte por (hash, AAR) |
| `nd.py` | Campos de `NotifyHandleDeadAck` por clase de muerte |

### Direcciones y claves (build 2976)

| RVA / clave | Qué es |
|---|---|
| `0xF2EE2678` | `ProjectileInfo` de la colisión del remate actual → ATA `8710E690` (3/5/5, sin AAR) |
| `0xF653A168` | `ProjectileInfo` de la flecha normal del jugador → ATA `10481FDD` (2/8/3, con AAR) |
| `5B636249` | Muerte normal más frecuente (93 muertes; una sola fase) |
| `18136804 → 03A4E5C8 → 4F69D6A0` | Muerte "despedida" de la colisión (también `4BA26366`, `70C5348E`, `738DB695`, `B3D1AAA6`) |
| `0x2A3D430 → 0x2C3D7D0 → 0x2BDA2F0 → 0x2BE11A0` | Servidor: `AttackByCollisionReq` → crea el proyectil del atacante |
| `0xB59280 → 0x451020` | Cliente: ATA → golpe, empuje, evento de IA |
| `0xB764A0 → 0x634420 → .xtls 0x902F790 → 0x1797850` | Cliente: `NotifyHandleDeadAck` |
| `0x35CA00` | `ClientNet::OnRecvPacket` (gancho del sniffer; lugar de B2) |
