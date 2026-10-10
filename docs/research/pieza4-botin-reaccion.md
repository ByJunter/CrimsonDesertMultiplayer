# Pieza 4: botín, daño con atribución y reacción al golpe

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco y los `staticinfo` ya
  extraídos en `build/research/ids/out/`);
- re-análisis de las 49 capturas: las 15 de `bin64\cdmp_packets_20261004-*` (incluida la de los
  experimentos, `111538`) y las de `D:\cdmp-grabaciones`;
- el log del mod `captures/cdmp_piece4_experiments.log`.

El juego no se ejecutó. Continúa [pieza4-dano-servidor.md](pieza4-dano-servidor.md) (la tubería
de daño) y [pieza4-capturas.md](pieza4-capturas.md) (tablas de daño y aggro).

Marcas:
- **[verificado]**: leído en el código;
- **[captura]**: medido en capturas;
- **[inferido]**: deducción pendiente de prueba en vivo.

Notación: **H** anfitrión, **G** invitado, **E** el enemigo, jugador `0xA0100001`,
marioneta (la copia del otro jugador) `0xA010F001`.

Scripts y volcados nuevos en `build/research/pieza4/loot/` (§6).

---

## 0. Resumen

1. **El botín no lo rompió nada: los enemigos de X2 y X4 eran de variantes que nunca sueltan
   botín al morir.** [verificado + captura]
   - `LootDropCharacterRewardListAck` en el lote de la muerte depende **solo de la variante**:
     su `CharacterInfo` tiene una lista de botín y hace falta una entrada con la máscara `2`
     ("al morir").
   - No dependen de eso:
     - el asesino (también sueltan botín los muertos por otro NPC);
     - el tipo de golpe (cuerpo a cuerpo, proyectil o truco);
     - el motivo (`8` o `0x10`);
     - el daño previo por `VaryStatByAccInputReq`. No hay umbral de contribución.
   - En las capturas, **72 de 72** muertes de variantes con máscara 2 soltaron botín, y **0 de
     142** sin ella. Coincide en las 63 variantes vistas.
   - Las víctimas de X2/X4 eran `…OneHandSword_54117`, `…_54109` y `…OneHandAxe_54140`, que no
     tienen máscara 2. Antes, a mano, esas variantes tampoco soltaron nada: 0 de 26 en la 54109,
     0 de 8 en la 54117 y 0 de 7 en la 54140.
   - Las dos muertes normales con botín eran `…OneHandAxe_54139` y `…OneHandBow_54121`, que
     siempre lo sueltan.
2. **Sí hay una vía de daño con atribución y cantidad libre: `VaryStatCheatReq` (2703), modo 0.**
   [verificado]
   - Hace lo mismo que `KillCheatReq`, que ya funcionó en X4:
     1. abre el registro de daño;
     2. llama a `0x17AE080(HP, −valor×1000, atacante = actor de la sesión, motivo 0x10, tipo 0xA)`;
     3. cierra el registro.
   - Resultado:
     - `VaryHPAck` con fuente = jugador de H;
     - muerte nativa si el HP llega a 0, con asesino = jugador de H y motivo `0x10`;
     - botín según la variante;
     - el buff de recompensa del asesino (20001).
   - Formato (15 B): `u32 objetivo | u32 0x000F4240 | u8 0 | i32 −daño | u8 0 | u8 0`.
   - El permiso es el mismo componente que el de KillCheat. Pasó en X4, y su `vf40` es
     `mov al,1; ret` [inferido: que ese componente sea el de `+0x130`].
   - **Ninguna otra Req** lleva una cantidad libre y un atacante ajeno al propio actor (§2.3).
3. **La reacción no salió porque la tambaleada no la produce `AttackTargetAck`, sino
   `ApplyActionRestrictionAck`.** [verificado + captura]
   - La plantilla de X3 era un golpe `41275A91` (`F104F0B7/4`). En las capturas, ese golpe
     siempre llega con AAR.
   - En los golpes de "aturdimiento":
     - con AAR, el enemigo reacciona el **100 %** de las veces (364 de 364);
     - sin AAR, casi nunca.
   - Las acciones de reacción con AAR y sin AAR son conjuntos casi disjuntos.
   - `0x45C8A0` no llega a mirar el accessor ni la acción en curso de la marioneta: con el byte
     del cable `+0x82 = 1` se salta ese bloque, y vale 1 en el 100 % de los golpes reales.
   - El atacante sí se resolvió. Hubo `ChangeAIStateReq` 28 ms después de una inyección, y la IA
     cambió de objetivo a la marioneta 7,7 s después (motivo `0x1D`).
4. **Receta final (§5):**
   - **daño y remate:** `VaryStatCheatReq` modo 0 con el daño estimado. Sustituye a
     `VaryStatByAcc` y a la colisión, y un solo mensaje sirve para todo;
   - **reacción:** `AttackTargetAck` + `ApplyActionRestrictionAck` inyectados, con plantillas del
     mismo hash de ataque;
   - **botín:** sale solo, en el mundo de H, para el jugador de H;
   - **aggro:** la IA ya lo recibe por el evento; si hace falta, se fuerza como en
     `pieza4-capturas.md` §5.2.

---

## 1. Botín

### 1.1 Qué pasó en los experimentos [captura]

`l_batch.py 20261004-111538` → `deaths_111538.txt`.

| t (s) | Muerto (variante) | Cómo | Asesino / motivo | Lote de la muerte | Botín |
|---|---|---|---|---|---|
| 170,271 | `B01002B2` (`…OneHandSword_54117`) | X2: a 1 HP y colisión, atacante jugador | `A0100001` / 8 | `AttackTargetAck` (211 B, `F7`), `VaryHPAck` 0 (fuente jugador), `OnPostBuffDamaged` **7 HP**, `ApplyBuffAck@P` 20001 ×2, `UnapplyBuff`×8, `ClearAllyGroupData`, `ElementalMaterialFuel`, `AttackerAck` | No |
| 179,123 | `B01002FC` (`…_54109`) | X2 | `A0100001` / 8 | Igual | No |
| 203,813 | `B01002C8` (`…OneHandAxe_54140`) | X2, atacante 0 | `B010006C` (gimmick) / 8 | Igual, pero **sin** `ApplyBuffAck@P` 20001 ni `AttackerAck`; colisión de **1 HP** | No |
| 241,453 | `B01002FA` (`…OneHandAxe_54139`) | Agarre y lanzamiento del jugador (`ReleaseCatchResultAck`, flags `E3`) | `A0100001` / 8 | Igual que X2, más `LootDrop…` (153 B) | **Sí** |
| 328,645 | `B0100311` (`…OneHandBow_54121`) | Igual | `A0100001` / 8 | Igual, más `LootDrop…` (42 B) | **Sí** |
| 336,617 | `B01002FB` (`…_54117`) | X4: `KillCheatReq` | `A0100001` / `0x10` | `VaryHPAck` 0 (fuente jugador), `ApplyBuffAck@P` 20001 ×2, `UnapplyBuff`×3, `ClearAllyGroupData`, `ElementalMaterialFuel` (sin `AttackTargetAck` ni `AttackerAck`) | No |

- Salvo por el `LootDrop`, los lotes de X2/X4 y los de las muertes normales son iguales.
- La colisión `0xF2EE2678` con el jugador como dueño quita **7 HP**. Con el gimmick como dueño
  quita 1.
- El buff 20001 (`0x4E21`) va al asesino **solo si es un jugador**. Es la única diferencia
  visible de la atribución en el lote de la muerte.

### 1.2 Dónde decide el servidor [verificado]

```
cierre del golpe → Status vf43 0x2941B60 → .xtls 0x10433BD0
   NotifyHandleDeadAck (escritor 0x29927E0): muerto, asesino, motivo (= r9b de vf43)
   → 0x1797850 → .xtls 0xC76CF30 (post-muerte)
        → Status vf97 0x2942350(status, asesino, ahora, motivo, …)
             Status+0x270 = motivo, Status+0x271 = 5.º argumento
             → 0x2942730
                  tipo del actor ([actor+0x88]+1) ∈ {1, 3, 4, 5, 6, 8, 9}   (el 7 no)
                  y Status+0x270 (motivo) != 0
                  y byte global 0x6D06E98 != 0
                  → 0x2AE6210 (componente [actor+0x68]+0x38)
                       [actor+0xA0] == 0 (si no, no suelta nada)
                       suelta el equipo (UnequipItemOnlyAck / CreateProjectileAck del arma)
                       → si existe [actor+0x68]+0xC8 (LootDrop): 0x2B85E40(comp, …, &motivo)
                            modo = 3 si motivo == 3; si no, 2
                            → 0x2B83960(comp, out, &modo, …)
                                 lista CharacterInfo+0x280 (n.º en +0x288), entradas de 12 B {clave, máscara, …}
                                 si ninguna entrada cumple (máscara & modo) → sin botín
                                 tira el botín (0x1F3E020, 0x2B85500, 0x2B84650; destinatario = usuario del field)
                                 → 0x2B84E20 → escritor 0x29F2CD0 = LootDropCharacterRewardListAck (ToNear)
```

- El asesino solo entra en `NotifyHandleDeadAck` y en los efectos sobre él (buff 20001). **La
  rama de botín no lo lee.**
- Tampoco lee el registro de daño. No hay un "mínimo de contribución" del jugador.
- El motivo solo importa si vale 0 (sin botín) o 3 (modo 3 = máscaras 1|2). 8 y `0x10` dan el
  mismo modo 2.
- Las demás llamadas al escritor `0x29F2CD0` son otras fuentes de botín:
  - `LootingDropFromDeadBodyReq` / `ProcessLootingDeadDropOnceTimer`: registrar el cadáver;
  - `ServerFrameEventDrop`, `…StealItem…`, `MiniGameResultReq`, `RewardAfterRelationalDialogReq`.

  [verificado: árbol de `0x29F2CD0` con `uptree.py`]

### 1.3 La lista de botín de cada variante [verificado + captura]

En el `staticinfo` serializado de `characterinfo`, la lista viene así:
`u32 n.º | n × {u32 clave, u32 máscara, u32 1}`, justo tras un `FF FF FF FF`. Lo lee
`l_mask2.py`.

| Familia | Lista | Máscara 2 |
|---|---|---|
| `NHM_Bleed_Bandits_OneHandSword_54111/54127`, `OneHandAxe_54139`, `OneHandBow_54107/54113/54121/54122/54143/54145` | `0x27167`/**2**, `0x2ABA8`/1, `0x2AD8D`/1, `0x2ADDF`/1, `0x2ABAA`/`0x400` | Sí |
| `NGM_Wolf_Pursuer_OneHandMusket_1/2/3` | `…`, `0x27168`/**2**, … | Sí |
| `NHM_Bleed_Bandits_OneHandSword_54104/54109/54117`, `OneHandAxe_54138/54140`, `…TowerShield…`, `…Dagger…`, `…Bomb…` | `0x2ABA8`/1, `0x2AD8D`/1, `0x2ADDF`/1, `0x2ABAA`/`0x400` | No |
| `NHM_Bandit_*` | `0x2AC59`/1, `0x2AD8D`/1 | No |
| `NGM_Wolf_Pursuer_OneHandAxe_*`, mercenarios errantes, animales | Sin entrada con máscara 2 | No |

Comprobación sobre todas las capturas (`l_deaths.py` → `l_var.py` → `l_mask2.py`; muertes de
enemigos reales, sin copias ni marioneta):

| Variante | Asesino | Muertes | Con `LootDrop` en el lote |
|---|---|---|---|
| Con máscara 2 | Jugador (motivo 8) | 68 | **68** |
| Con máscara 2 | Otro NPC (motivo 8) | 4 | **4** |
| Sin máscara 2 | Jugador (motivo 8 / `0x10`) | 98 / 1 | 0 / 0 |
| Sin máscara 2 | NPC (motivo 8 / 7) | 28 / 15 | 0 / 0 |
| Desconocida (`105149`, truncada) | — | 29 | 1 |

- **¿Cuánto falta el botín en muertes normales?** En el 62 % de las muertes a manos del jugador
  (115 de 184). Todas son variantes sin máscara 2. **0 excepciones.**
- **¿Rompe algo el daño espejo (`VaryStatByAccInputReq`)?** No.
  - De las 73 muertes con botín, en 33 la mayor parte del daño vino de `VaryStatByAcc`
    (fuente 0).
  - En 6, *todo* el daño visible vino de ahí, y aun así soltaron botín. Por ejemplo:
    `064320` `B0100298` (54111), `183530` `B0102ABE` (54121).
- La máscara 1 es, con toda probabilidad, el botín al **registrar el cadáver**. En 105149 y 090931,
  un aliado (`B0100004`) hizo `InteractionDoStepReq` sobre el cadáver. Llegaron
  `LootDropCharacterRewardListAck` (5 o 153 B) y `RemoveLootableAck(muerto, A0100001)`,
  10-40 s después de la muerte [captura; el significado de la máscara es inferido].

### 1.4 Receta para que una muerte "en nombre de G" dé botín

- **Basta con que el enemigo muera de forma nativa** (motivo ≠ 0, ≠ 3) **y sea de una variante
  con máscara 2.** Cualquiera de estas vías vale [verificado]:
  - `VaryStatCheatReq` modo 0 (§2);
  - `KillCheatReq`;
  - la colisión de X2;
  - un golpe nativo.
- Para comprobarlo (**prueba L1**), repetir X4 o X2 sobre una variante con máscara 2. En el campamento de
  111538 estaban `…OneHandAxe_54139` y `…OneHandBow_54121`. Hay que ver `LootDrop…` en el mismo
  lote. Usar como control una `54117`, que no debe soltar nada.
- **Quién se lo queda:**
  - el `LootDrop` va ToNear desde el muerto;
  - los objetos aparecen en el mundo de H, con destinatario el usuario del field (el jugador
    de H);
  - el cadáver queda "registrable" para el jugador de H (`RemoveLootableAck(…, A0100001)`).

  **Para la pieza 4 es el jugador de H** [verificado + captura].
- **¿Podría quedárselo G?** No por el servidor de H: G no tiene actor allí. Vías posibles
  [inferido]:
  - **R6, "muerte espejo" en G:** cuando muere E en H, G manda `KillCheatReq` (o
    `VaryStatCheatReq`) **a su propio servidor** contra el gemelo nativo de E.
    - El servidor de G daría a G el botín de la variante, el buff 20001 y, si lo hubiera, el
      crédito, todo de forma nativa.
    - Requiere que el gemelo exista en el servidor de G, es decir, que la supresión 3g "oculte y
      congele" en vez de no crearlo (lo mismo que R2 en `pieza4-dano-servidor.md` §3).
    - Hay que emparejar por variante y punto de aparición, nunca por dirección.
    - Efecto secundario: H también recibe botín. Cada jugador tendría el suyo, como en un coop
      con botín por jugador.
  - Reenviar a G la lista del `LootDrop` no sirve para nada: su servidor no tiene esos objetos.

---

## 2. Un camino de daño con atribución

### 2.1 `VaryStatCheatReq` (2703) [verificado]

Deserializador `0x29BF3C0`, 15 B, en este orden:

| Campo | Tamaño | Uso en `0x2C3FBF0` |
|---|---|---|
| f0 | u32 | Clave del objetivo. Si es 0, Nak `0x3F5`. Se busca con `0x2A827B0` sobre `[exe+0x6D69698]`; no comprueba el dueño |
| f1 | u32 | Clave de stat. Misma tabla hash `[exe+0x6D69AE8]` que `VaryStatByAccInputReq`, así que el HP es `0x000F4240`. Si no la encuentra, prueba tres nombres por defecto |
| f2 | u8 | **Modo:** 0 = sumar (§2.2); 1 = fijar (`0x17AED80`, y con f5 también `0x17AE080`); 2 = `0x17AFB10` (otra variante con atacante) |
| f3 | i32 | Valor en unidades. Internamente ×1000 (`imul rsi, rax, 0x3E8`); recortado a 0..100 para el modo porcentaje |
| f4 | u8 | ≠0: f3 es un porcentaje |
| f5 | u8 | Solo para el modo 1 |

Permiso, antes de nada: `[[actor+0xA0]+0x68]+0x130 → vf(0x140)(3)`. Es el mismo objeto que en
`KillCheatReq` (tipo 2), que pasó en X4. `ServerContentsMiscActorComponent::vf40` es
`mov al,1; ret` (`exe+0x3600C0`), así que ignora el tipo. [verificado el código;
inferido que el componente de `+0x130` sea ese]

**Modo 0, sin porcentaje** (`0x2C3FF32`–`0x2C40819`):
1. `Status vf52` del objetivo (`call [rax+0x1A0]`): **abre el registro de daño**.
2. `delta = min(f3×1000, cota del stat)`. Un valor negativo pasa tal cual.
3. `0x17AE080(stats, HP, ahora, delta, atacante = r12, 0, 0, motivo 0x10, tipo 0xA, 0, &out)`. Son
   los mismos argumentos que `KillCheatReq` (`0x2C40FA0`).
   - `r12 = [ctx]` es el actor de la sesión: el **jugador de H**.
   - En X4, `VaryHPAck` y `NotifyHandleDeadAck` salieron con `A0100001` [captura].
4. `Status vf54` (`call [rax+0x1B0]`): **cierra el registro**. Si murió, salen
   `NotifyHandleDeadAck`, el post-muerte y el botín (§1.2).

Con eso:
- la fuente de `VaryHPAck` es el jugador de H;
- el registro de daño recibe el asesino y el motivo `0x10`;
- **no hace falta remate aparte**: el mensaje que deja el HP a 0 ya mata;
- no hay zombis a 0 HP como con `VaryStatByAcc` (`pieza4-dano-servidor.md` §2.d).

No genera `OnPostBuffDamagedAck` ni `AttackTargetAck`. Eso es cosmético o se inyecta aparte (§3).

### 2.2 Paquete [verificado formato; inferido respuesta]

```
op 2703, slot 0, len 15:
  u32 E | u32 0x000F4240 | u8 0 | i32 −daño_HP | u8 0 | u8 0
```

Se manda por el mismo canal que `KillCheatReq` en X4 (`sniffer_inject_c2s`). Respuesta esperada:
- `VaryHPAck(E, hp−daño, fuente A0100001)`;
- si llega a 0: `NotifyHandleDeadAck(E, A0100001, 0x10)` + post-muerte + botín según la variante.

Si no responde, o responde Nak `0x3F5`: el permiso de tipo 3 no pasa. En ese caso:
- `VaryStatByAcc` hasta 1 HP;
- `KillCheatReq` como remate (comprobado en X4).

### 2.3 Otras entradas que acaban en `0x17AE080` [verificado: árbol `up_17ae080.txt`]

| Req | Cantidad | Atacante | Motivo | ¿Sirve? |
|---|---|---|---|---|
| `VaryStatCheatReq` (2703) | **Libre** | Actor de la sesión | `0x10` | **Sí** |
| `KillCheatReq` (2279) | Todo el HP | Actor de la sesión | `0x10` | Como remate (X4) |
| `KillJammedTargetReq` (2392) | Todo el HP | **El propio objetivo** (`0x28E54CA`) | `0x12` | No. Además solo vale para el "objetivo atascado" del jugador (`[[comp+0x168]…+0xA0]+0xD8`, con la clave comprobada) |
| `DieByRunOutFatalReq` (2321) | — | Él mismo | — | No |
| `ProcessFallReq` (2095) | La calcula el servidor (`0x2BCFBB0`) | Ninguno | — | No |
| `CreateProjectileFrom*Req`, `OnBoundProjectileReq`, `OnDeflectionProjectileReq`, `CreateTornadoThrowingProjectileReq`, `AttackByCollisionReq` | La del proyectil (stats del dueño) | Dueño del proyectil | 8 | Solo como remate (X2: 7 HP) |
| `RecoveryMercenaryReq`, `LoadMercenaryStatReq`, `RecoveryActorForMiniGameReq`, `JoinMiniGameReq`, `EnableSpecialModeReq`, `CallHyosi…`, `CallMercenary…` | Curas y costes propios | — | — | No |
| `ApplySkillFromCollideObjectReq` (2806) | La del buff (stats del objetivo) | El propio actor | `0x13` | No (`pieza4-dano-servidor.md` §2.e) |

**Atribuir a G es imposible en el servidor de H** (no tiene actor). Lo mejor posible es atribuir al
jugador de H, y eso es lo que hace `VaryStatCheatReq`.

---

## 3. La reacción al golpe

### 3.1 Qué hace el cliente con `AttackTargetAck` [verificado]

**`0xB59280`**:
- lee `u32 objetivo`;
- deserializa el resto con `0x24431B0` → `0xDE45F50`;
- busca el objetivo y llama a `0x451020(ClientAttack del objetivo, struct)`.

Equivalencia cable → struct (`struct = cable − 4`, salvo el bloque inicial):

| Cable | Struct | Qué es |
|---|---|---|
| `+0x04` | `+0x00` | Atacante |
| `+0x08..+0x23` | `+0x04..+0x1F` | Bloque: flags `+0x08`, u32 `+0x10` (7/3/4/6/1), `+0x20` (35/39/…) |
| `+0x24` | `+0x20` | Hash (clave de `AttackInfo`) |
| `+0x2D`, `+0x41`, `+0x49`, `+0x51` | `+0x30`, `+0x48`, `+0x50`, `+0x58` | u64 (ids o ticks) |
| `+0x35` / `+0x39` / `+0x3D` | `+0x38` / `+0x3C` / `+0x40` | f32 6,0 / 1,0 / 2,05 (empuje) |
| `+0x5D` / `+0x5E` / `+0x5F` / `+0x64` | `+0x68` / `+0x69` / `+0x6A` / `+0x70` | Niveles del golpe |
| `+0x69` / `+0x75` / `+0x83` / `+0x8F` | `+0x78` / `+0x84` / `+0x94` / `+0xA0` | Posiciones |
| `+0x81` / `+0x82` | `+0x90` / `+0x91` | Bytes |
| `+0x9B` | `+0xAC` | Cuaternión |
| Final | `+0xD0` | Accessor |

**`0x451020`**:
1. Busca al **atacante** en `[exe+0x6D69190]+0x30`. Si no está, no hace **nada más**. La marioneta
   sí está: la IA reaccionó en X3 n.º 3 (`ChangeAIStateReq` +28 ms) [captura].
2. Si el objetivo es de tipo 3-6, 8 o 9, llama a `0x45C8A0`.
3. Si `struct+0x90` (cable `+0x81`) **== 0**, llama a `0x45B700` (efectos visuales y de sonido
   desde el `AttackInfo` y las posiciones). En los golpes reales siempre vale 1, así que nunca
   pasa.
4. `0x45D3E0`.
5. **Evento de juego a la IA**: `0x2083D90` sobre el GameEventHandler (`+0x58`), con el atacante,
   el bloque de flags, los bytes `+0x5D`/`+0x5E`/`+0x64` y el hash.
   - Se descarta si `struct+0xC` (cable `+0x10`) == 1.
   - Dentro compara el nivel del golpe con un nivel del objetivo (`info+0xA5`, que salta si es
     `0x0B`).
6. `0x45D800` → `.xtls 0x89715B0` (cambio de objetivo al atacante).

**`0x45C8A0`**:
- Si flags `& 0x10` y `+0x2C`: impulso (`0x9FCD40`).
- Si flags `& 0x4`: **empuje físico** (`0x503E80`) con el vector `+0x30..+0x40` y
  `AttackInfo+0x68`.
- Después, **`if (struct+0x91 != 0) return`** (`0x45CCF3`). Solo con `+0x91 == 0` y flags `& 0x20`
  resuelve el frame event del atacante (`0x1F78A20` sobre su acción en curso) para la sensación
  de golpe (sacudida de cámara, `0x1417EB830`…).
- En el 100 % de los 1.042 golpes reales a enemigos `+0x82 = 1` [captura]. Así que **el accessor y la
  acción en curso de la marioneta no influyen**.

Ninguna de estas funciones lanza la animación de tambaleo.

### 3.2 La tambaleada sale de `ApplyActionRestrictionAck` [verificado + captura]

`0xB5AEE0` (handler de AAR):
- objetivo de tipo 3-6, 8 o 9 → CharacterControl `[+0x68]+0x40` → componente `+0x118` →
  **`0x48B400`** (aplica la restricción de acción);
- busca la fuente (`+0x49`) en el gestor del cliente, **pero es opcional**;
- si el objetivo está muerto (`+0x273`), no hace nada.

La restricción la ejecuta el cliente de H, que controla al enemigo: lanza la acción de tambaleo y
manda su `StartActionReq`.

En las capturas (`l_react.py`, `l_react2.py`; 1.042 golpes del jugador a enemigos vivos):

| Golpe | Golpes | `StartActionReq` del enemigo en ≤100 ms |
|---|---|---|
| `F7` con AAR | 364 | **100 %** (también los 24 sin ningún buff de golpe) |
| `F7` sin AAR | 82 | 79 % (sobre todo agarres y lanzamientos y golpes sin aturdimiento) |
| `84` (bloqueado) / `87` | 98 / 59 | 100 % (bloqueo o esquiva) |
| `E3` | 103 | 17 % (100 % sin el buff 34; 14 % con el buff 34, ¿súper armadura?) |
| `00` | 274 | 0 % |

- **Hashes "de aturdimiento"**: reacción ⇔ AAR. Por ejemplo:
  - `C7D0779C`: 16 golpes, 13 con AAR, 13 reacciones;
  - `1B0CD1E5`: 10, 8 y 8.
  - Los golpes sin AAR de esos hashes no hicieron reaccionar al enemigo.
- **Acciones de reacción**: con AAR, `90A9C337`, `3C4A7F1D`, `F966B53B`, `4A641121`…; sin AAR,
  `7208BF4D`, `3CBA4E0F`, `04BE5488`…
  - Solo 5 acciones coinciden, de 44 con AAR y 38 sin AAR.
  - Son reacciones distintas: tambaleo frente a guardia o agarre.
- **El golpe que sirvió de plantilla** (261,439, `41275A91` sobre `B010030A`):
  - AAR a los 2 ms;
  - `StartActionReq 9B68944F → 3C4A7F1D` a los 18,6 ms.
- **Las cuatro inyecciones X3** (264,248, 272,217, 293,244 y 305,965):
  - ningún `StartActionReq` de tambaleo;
  - en la tercera, `ChangeAIState` y `StartActionReq → 4FD6A2FF` a los 29 ms (la acción normal
    de perseguir; también aparece sin golpes, por ejemplo en 170,100).
- **Liberación:** no hay `ReleaseActionRestrictionAck` ni `UnapplyTimedState…` en 111538 (0 de
  22 AAR). La restricción caduca en el cliente, así que una AAR inyectada no deja nada pendiente
  en el servidor [captura].
- **El aggro de X3:**
  - `B010030A` ya apuntaba al jugador (CTC a 250,759).
  - Cambió a la marioneta a 271,970, 7,7 s después de la primera inyección, con motivo `0x1D`
    (por amenaza, no el inmediato `0x18`).
  - `B0100311` cambió a la marioneta (303,285) **antes** de su inyección (305,965).
  - Así que "el aggro funciona" no está demostrado: la marioneta ya es objetivo válido por sí
    misma.
  - Lo coherente con el código es que el evento `0x2083D90` alimente la amenaza [inferido].

### 3.3 Formato de `ApplyActionRestrictionAck` (2162, 77 B) [captura + verificado parcial]

Ejemplos: 111538, t = 257,612 / 258,308 / 261,441.

| Cable | Tipo | Valor típico | Qué es |
|---|---|---|---|
| `+0x00` | u32 | Enemigo | Objetivo (lo lee el handler) |
| `+0x04` | f32×4 | `(0; 0,872; 0; 0,489)` | Cuaternión. **Igual a `AttackTargetAck +0x9B`** |
| `+0x14` | u64 | 0 | Retardo o inicio (`0x48B400`: -1 = sin tiempo; si no, ahora + valor) |
| `+0x1C` | u64 | 1 | Pasa a `0x8B4790` |
| `+0x24` | u32 | `0x800000NN` | **Id de instancia**. Es el mismo contador por actor que `ApplyBuffAck +0x14`; el AAR comparte id con el buff de golpe 1104/1105 del mismo lote |
| `+0x28` | f32 | −0,629 | Igual a `AttackTargetAck +0x60` |
| `+0x2C`, `+0x30` | u32 | 0 | — |
| `+0x34` | u32 | `0x79156D1B` | Clave de la restricción (el mismo valor en las tres muestras) |
| `+0x38` | u32 | `0x41275A91` | Hash de ataque (= `AttackTargetAck +0x24`) |
| `+0x3C`, `+0x40` | u32 | 0 | — |
| `+0x44` | u8 | 0 | — |
| `+0x45` / `+0x46` | u8 | 01/01, 01/04, 02/06 | = `AttackTargetAck +0x5D` / `+0x5E` |
| `+0x47` / `+0x48` | u8 | 0 / 1 | — |
| `+0x49` | u32 | Atacante | Fuente (opcional en el cliente) |

**Receta segura** (por golpe de G con P(AAR) ≥ 0,8 para su clave, `pieza4-capturas.md` §2.3):
- **plantilla:** el último AAR **real** de H con el **mismo hash** (`+0x38`). Hay que guardarlo
  como ya se guarda el `AttackTargetAck` (`piece4_observe_s2c` solo guarda el ATA);
- **reescribir:**
  - `+0x00` = E;
  - `+0x49` = marioneta;
  - `+0x24` = id de un rango privado. Propuesta: `0x8000F000 + n` por enemigo. El contador del
    servidor va por `0x80000017` tras minutos de pelea [captura], así que no choca;
  - `+0x04` = cuaternión de la orientación de G hacia E. El de la plantilla vale para empezar;
  - `+0x28` = el mismo valor que en el ATA;
- **no inyectar** si E está muerto o si el golpe es letal: el `VaryStatCheatReq` lo mata y el
  cliente lanza la muerte;
- **orden:** `AttackTargetAck` → `ApplyActionRestrictionAck` (es el orden real, 0-2 ms entre
  ambos), y a la vez el `VaryStatCheatReq` C→S;
- **opcional:** `ApplyBuffAck` 1104/1105 con el mismo id. No hace falta para la reacción: hubo
  24 de 24 reacciones con AAR y sin buffs de golpe.

### 3.4 Qué tiene que llevar el `AttackTargetAck` inyectado [verificado + inferido]

- `+0x00` E; `+0x04` marioneta (debe existir en el cliente).
- `+0x08` flags de la plantilla (`F7`). El bit `0x4` da el empuje físico con `+0x35`/`+0x3D`.
- `+0x10` ≠ 1: la plantilla trae 7. Con 1 no se envía el evento a la IA.
- `+0x24` un hash válido de `AttackInfo` (empuje y evento). `+0x5D`/`+0x5E`/`+0x64` de la
  plantilla (nivel del golpe frente al del objetivo).
- `+0x81 = +0x82 = 1`, como los reales.
  - Con `+0x82 = 1` el accessor del final y los ids u64 no se usan en la reacción.
  - **`+0x81 = 0`** activaría `0x45B700` (efecto y sonido del impacto con las posiciones `+0x69`/`+0x83`).
    Puede servir para que H vea el impacto de G, porque la marioneta (modo 2) no ejecuta frame
    events. Probarlo aparte [inferido].
- Posiciones `+0x69`/`+0x83`/`+0x8F`: las de E en H y el punto de impacto de G.

---

## 4. Respuestas directas

1. **Botín:**
   - Lo decide el post-muerte (`vf97 0x2942350 → 0x2942730 → 0x2AE6210 → 0x2B85E40 → 0x2B83960`)
     a partir de la lista de botín de la `CharacterInfo` del muerto (máscara 2).
   - No dependen de ello el asesino, el tipo de daño (cuerpo a cuerpo, proyectil o truco), el
     motivo (salvo 0 o 3) ni la parte de daño del jugador.
   - X2/X4 no dieron botín porque sus víctimas eran 54117/54109/54140.
   - El daño `VaryStatByAcc` previo no rompe nada.
   - Receta: muerte nativa con atribución. El botín cae en el mundo de H para el jugador de H.
     Para G, la "muerte espejo" en su servidor (R6).
2. **Daño con atribución:** `VaryStatCheatReq` modo 0. Cantidad libre, atacante = jugador de H,
   motivo `0x10`, con registro abierto y cerrado. No hay otra Req útil.
3. **Reacción:**
   - El ATA inyectado se procesó: se encontró al atacante y llegó el evento a la IA.
   - Pero el tambaleo de los golpes "de aturdimiento" lo produce `ApplyActionRestrictionAck`, y
     X3 no lo mandó.
   - El accessor y la acción de la marioneta no importan con `+0x82 = 1`.
4. **Receta final:** §5.

---

## 5. Receta de la pieza 4 en H, por golpe de G

Entrada, desde G: el evento `GuestHit` (`pieza4-capturas.md` §6.1):
- enemigo;
- acción y frame;
- hash de ataque, si se conoce por la tabla acción+frame → hash;
- daño estimado `d` (base de G × defensa de H);
- posiciones.

**H, con E vivo y su HP `hp` (último `VaryHPAck`):**

| # | Qué | Cómo | Cuándo |
|---|---|---|---|
| 1 | **Daño y remate** | C→S `VaryStatCheatReq(E, 0x000F4240, 0, −d, 0, 0)`. Si `d ≥ hp`, mandar `−hp` (o más): mata de forma nativa | Siempre que `d > 0` y la clave no sea "sin daño" (`E3` fijo) |
| 2 | **Reacción** | S→C `AttackTargetAck` (plantilla del mismo hash, §3.4) | Siempre (también en `E3`) |
| 3 | **Tambaleo** | S→C `ApplyActionRestrictionAck` (plantilla del mismo hash, §3.3) | Si P(AAR) ≥ 0,8 para la clave y el golpe **no** es letal |
| 4 | Número de daño | S→C `OnPostBuffDamagedAck` (`+0x00` E, `+0x04`/`+0x08` marioneta, `+0x0C` d×1000) | Cosmético |
| 5 | **Botín** | Nada: lo emite el servidor de H tras la muerte del paso 1 si la variante tiene máscara 2 | — |
| 6 | **Aggro** | El evento de IA del paso 2 (amenaza). Si no basta, `CurrentTargetChangedAck` forzado con la política de `pieza4-capturas.md` §5.2 | — |

- **Mismo cambio para el daño espejo** (`local_capture.cpp`) **y la pieza 2** (`player_hits.cpp`):
  - con `VaryStatCheatReq` en vez de `VaryStatByAccInputReq` no quedan enemigos a 0 HP;
  - la muerte es nativa y atribuida al jugador de H, y hay botín.
  - En la pieza 2 el objetivo es el jugador. Con `VaryStatCheatReq` su muerte sería nativa
    (motivo `0x10`, asesino = el propio jugador) [inferido; probar con cuidado, ver
    `pieza4-dano-servidor.md` §8].
- **Alternativa si el permiso de tipo 3 falla:** `VaryStatByAcc` hasta 1 HP y `KillCheatReq` como
  remate (X4 ya lo validó). La colisión de X2 también vale, pero quita 7 HP y no es exacta.
- **Riesgos** [inferido]:
  - `0x10` es el motivo "truco". El buff de recompensa (20001) sí llega, pero no se sabe si las
    misiones de "eliminar N" lo cuentan (prueba X5);
  - no se conoce telemetría del lado del servidor local para los trucos;
  - los ids de instancia inyectados (`+0x24`) podrían chocar si el rango no se separa bien.

### Pruebas (un PC, `cdmp_peer` en eco; copia de la partida en `backups/` antes)

| # | Prueba | Esperado | Decide |
|---|---|---|---|
| **L1** | X4 (`KillCheatReq`) sobre una variante **con** máscara 2 (`…OneHandAxe_54139`, `…OneHandBow_54121`/`54107`/`54122`, `…OneHandSword_54111`, `Wolf_Pursuer_OneHandMusket_*`) y, como control, sobre una `54117` | `LootDrop…` en el mismo lote que `NotifyHandleDeadAck` (motivo `0x10`) en la primera; nada en la segunda | §1 (motivo `0x10` y botín) |
| **V1** | `VaryStatCheatReq(E, HP, 0, −5, 0, 0)` sobre un enemigo vivo | `VaryHPAck` hp−5, fuente `A0100001`; sin Nak `0x3F5` | El permiso de tipo 3 y el formato de 15 B |
| **V2** | `VaryStatCheatReq(E, HP, 0, −999, 0, 0)` | `NotifyHandleDeadAck(E, A0100001, 0x10)`, `ApplyBuffAck@P` 20001 y el botín si la variante lo tiene | Remate en un solo mensaje |
| **R1** | X3b: ATA + AAR (plantillas del mismo hash, `+0x49` = marioneta, `+0x24 = 0x8000F000+n`) sobre un enemigo vivo que no esté ya tambaleándose | `StartActionReq` del enemigo en ≤30 ms con una acción de tambaleo (por ejemplo `3C4A7F1D`/`90A9C337`) y la animación visible | §3.2 |
| R2 | Controles: AAR solo, ATA solo, AAR con `+0x49 = 0` | La reacción con AAR solo y con fuente 0; ninguna con ATA solo para `41275A91` | Qué es imprescindible |
| R3 | ATA con `+0x81 = 0` | Efecto y sonido del impacto en E (captura visual con `CopyFromScreen`) | Impacto visible de G en H |
| X5 | Misión "eliminar N" rematando con V2 | Si el contador sube con el motivo `0x10` | Crédito de misión |
| R6 | En G, `KillCheatReq` al gemelo nativo cuando muere su original en H (en eco, sobre el original) | Botín y buff 20001 en el mundo de G | Botín para G (depende de 3g) |

---

## 6. Scripts y volcados (`build/research/pieza4/loot/`)

Ejecutar con `build/research/venv/Scripts/python.exe`. Usan `../pk.py` y su caché.

| Script / fichero | Qué hace |
|---|---|
| `l_batch.py <sesión> [antes] [después]` → `deaths_111538.txt` | Cada muerte de una sesión con los paquetes que la rodean |
| `l_deaths.py` → `deaths_all.csv` | Una fila por muerte: asesino, motivo, botín, daño por fuente (`VaryHPAck`), inyecciones `VaryStatByAcc`, agarres |
| `l_var.py` → `deaths_var.csv` | Añade la variante (`ActorLoadDataAck`), el botín diferido y `RemoveLootableAck` |
| `l_mask2.py` | Lista de botín de cada `CharacterInfo` (máscara 2) frente a lo observado (63 de 63) |
| `l_loot_t.py` | Retardo de cada `LootDrop` respecto a la muerte (inmediato / diferido) |
| `l_react.py` → `react_all.csv`, `l_react2.py` → `react2.csv` | Golpe real → AAR, buffs de golpe, `StartActionReq` del enemigo (reacción) |
| `l_ctx.py`, `l_dump.py` | Ventana de paquetes por clave; volcado hexadecimal por tipo |
| `d_0x2942350.asm` … `d_0x2b84e20.asm` | Cadena post-muerte → botín |
| `d_28e5250_killjammed.asm`, `up_17ae080.txt` | `KillJammedTarget`; árbol de llamadores de `0x17AE080` |
| `c_0x24431B0.asm`, `c_0x451020.asm`, `c_0x45C8A0.asm`, `c_2083d90_onattacked.asm`, `c_b5aee0_aar.asm`, `c_48b400_applyrestr.asm` | Cliente: ATA → reacción; evento de IA; AAR → restricción |

### Direcciones nuevas (build 2976)

| RVA | Qué es |
|---|---|
| `0x2942350` | `ServerStatusActorComponent` vf97: post-muerte (`+0x270` = motivo) |
| `0x2942730` | Condiciones de soltar objetos por tipo de actor |
| `0x2AE6210` | Suelta el equipo y llama al componente LootDrop (`[actor+0x68]+0xC8`) |
| `0x2B85E40` / `0x2B83960` | Botín al morir: modo 2/3, lista `CharacterInfo+0x280` |
| `0x2B84E20` / `0x29F2CD0` | Envía / escribe `LootDropCharacterRewardListAck` |
| `0x6D06E98` | Byte global "soltar al morir" |
| `0x29BF3C0` → `0x2C3FBF0` | `VaryStatCheatReq`: deserializador → núcleo (modo 0 → `0x17AE080`, motivo `0x10`) |
| `0x3600C0` | `ServerContentsMiscActorComponent::vf40` = `mov al,1; ret` |
| `0x29D0380` → `0x28E5250` | `KillJammedTargetReq` (motivo `0x12`, atacante = objetivo) |
| `0xDE45F50` | Bloque `+0x08..+0x23` de `AttackTargetAck` (cliente) |
| `0x2083D90` | Cliente: evento de IA "golpeado" (lo llama `0x451020`) |
| `0x45CCF3` | `0x45C8A0` sale si `struct+0x91` (cable `+0x82`) ≠ 0 |
| `0xB5AEE0` → `0x48B400` | Cliente: `ApplyActionRestrictionAck` → aplica la restricción (CC `+0x118`) |

---

## 8. Resultados en vivo: V1, V2, R1, L1 (captura `20261004-114648`)

Log del mod: hora del log = hora de la captura + 11:47:05,557. Scripts: `build/research/pieza4/p4r2*.py`.

| Prueba | Resultado | Detalle |
|---|---|---|
| V1 (`VaryStatCheatReq` −5) | Funciona, pero cayó sobre **nuestro jugador** | El objetivo fue `0xA0100001` porque `capture_last_attack_target()` devolvió al propio jugador. `VaryHPAck` con fuente `A0100001`. Corregido en `pick_target` |
| V2 (`VaryStatCheatReq` −999) | **Daño atribuido, pero sin muerte inmediata** | `VaryHPAck` hp 0, fuente = jugador → **`NotifyHandleFatalAck`** (2899, "por" = jugador) → el enemigo queda aturdido → **5,0 s** después el cliente de H manda **`DieByRunOutFatalReq`** (2321, 4 B = clave) → `NotifyHandleDeadAck` (asesino = jugador, **motivo 15**) + botín (54121 y 54143 lo soltaron) |
| L1 (`KillCheatReq`) | **Muerte inmediata, atribuida, con botín** | Motivo 16; botín en `B01001A7` (54122). Sobre un muerto no hace nada |
| R1 (ATA + AAR desde la copia) | **Sin tambaleo** | El enemigo pasó en ~25 ms a la acción `A73D6EC4`, que en el resto de la captura solo aparece **al terminar** un tambaleo real (después de `3C4A7F1D`, `4A641121`, `F966B53B`, `90A9C337`, `496AABC1`). La restricción llegó e interrumpió la acción, pero el tambaleo se saltó. En investigación (`pieza4-tambaleo.md`) |

- El estado "fatal" (HP 0 sin muerte) **no aparece nunca** en las otras 48 capturas: solo lo produce
  `VaryStatCheatReq`. Una muerte normal del jugador va directa a `NotifyHandleDeadAck` (motivo 8).
- Receta adoptada en `guest_hits.cpp`:
  - golpe no letal: `VaryStatCheatReq` (atribuido);
  - golpe letal con HP conocido: `KillCheatReq`;
  - si aun así llega `NotifyHandleFatalAck` de un enemigo al que acabamos de dañar (HP no conocido),
    se manda `DieByRunOutFatalReq` en el acto, como haría el cliente 5 s después.
