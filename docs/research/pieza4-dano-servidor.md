# Pieza 4: el golpe del invitado en el servidor del anfitrión

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA
(`exe+…`) y valen solo para esta build.

Método:
- análisis estático de solo lectura (capstone sobre el exe en disco);
- re-análisis de las capturas ya grabadas:
  - `captures/cdmp_packets_20261003-*.bin`;
  - `D:\cdmp-grabaciones\*.bin`;
  - las 14 de hoy en `bin64\cdmp_packets_20261004-*.bin`, que llevan el daño espejo inyectado
    con `VaryStatByAccInputReq`.

El juego no se ejecutó.

Complementa a [pieza4-capturas.md](pieza4-capturas.md), que mide en las capturas los golpes a
copias, las tablas de daño y el aggro. Este informe mira **dentro del servidor**: qué calcula, con
qué datos y qué puertas de entrada hay.

Marcas:
- **[verificado]**: leído en el código;
- **[captura]**: medido en capturas;
- **[inferido]**: deducción pendiente de prueba en vivo.

Notación:
- **H**: anfitrión;
- **G**: invitado;
- copia: `0xB01F0000+n`;
- jugador: `0xA0100001`;
- marioneta: `0xA010F001`.

Scripts y volcados en `build/research/pieza4/` (§10).

---

## 0. Resumen

1. **El daño no lo calcula el frame event de ataque, sino un buff de daño.** [verificado]
   - El ataque resuelve su `AttackInfo` (clave = `[frame event+8]`).
   - Aplica al objetivo los buffs de esa `AttackInfo`.
   - El buff de daño (`ServerDamageBuffProcessor`) llama a una función **común** (cliente y
     servidor), `0x209A680`, que calcula el daño con:
     - el ataque del atacante (stat `+0xA6`, del arma o de sus stats);
     - las defensas del objetivo;
     - los datos del buff (valor por nivel, tipo de daño).
   - El HP se aplica con `0x17AE080` → `0x17AC040`, que conoce **al atacante y el motivo de
     muerte**.
2. **La muerte la decide `0x17AC040` y solo esa función.** [verificado]
   - Si el HP llega a ≤0, marca al actor muerto (`Status+0x273`).
   - Guarda en un registro de daño por hilo el asesino y el motivo.
   - Al cerrar el golpe salen `NotifyHandleDeadAck` (asesino, motivo 8) y el botín
     (`LootDropCharacterRewardListAck`, en el mismo lote).
3. **Hallazgo clave: `VaryStatByAccInputReq` no mata.** [verificado + captura]
   - Entra por la ruta genérica de stats `0x17AE4B0`, sin atacante ni motivo.
   - Al llegar a 0 solo lanza el evento de juego `OnBattle_HandleKnockOut`.
   - En las capturas de hoy, **53 enemigos** (55 inyecciones) bajaron a 0 HP por esta vía:
     - **ninguno murió** sin más;
     - 50 murieron con el siguiente golpe nativo (asesino = quien dio ese golpe, motivo 8,
       botín en el mismo lote cuando lo hubo);
     - 3 siguieron vivos y peleando a 0 HP ("zombis").
   - Esto afecta ya al daño espejo y a la pieza 2 (§8).
4. **Ninguna Req de H calcula el daño con las stats de G**, porque el servidor de H no tiene un
   actor con esas stats. Las opciones nativas usan siempre las stats de un actor de H:
   - (a) atacante suplente: el jugador de H;
   - (c) `AttackByCollisionReq`: el dueño del proyectil.
5. **`AttackByCollisionReq` no lleva un hash de ataque, sino una clave de `ProjectileInfo`.**
   [verificado + captura]
   - Crea un proyectil del atacante en el punto de contacto.
   - El cliente lo simula y el golpe vuelve como `DoFrameEventReq` de tipo 1.
   - Así produce todo nativo: `AttackTargetAck`, `VaryHPAck` con fuente, `OnPostBuffDamagedAck`,
     muerte y botín.
   - Con la clave de un `AttackTargetAck` (prueba T3, `0x81048142`) no responde nada.
   - Con `0xF2EE2678` (un gimmick) quita 1 HP.
   - **Sirve como "remate" nativo** con asesino = jugador de H. No sirve para entregar el daño de G.
6. **La reacción y el aggro sí pueden ser nativos en H.** [verificado + captura]
   - El handler de `AttackTargetAck` del cliente lanza la reacción del objetivo.
   - Por la ruta `0x451020 → 0x45D800 → 0x89715B0 → SetCurrentTarget 0x2217290` puede
     **cambiar su objetivo al atacante**.
   - En captura, un enemigo golpeado por otro NPC cambió de objetivo a él 16 ms después (motivo
     `0x1B`).
   - Inyectar `AttackTargetAck` en H con atacante = marioneta es la palanca.

**Recomendación** (§6):
- **R1, ya:**
  - daño estimado en G con las tablas de `pieza4-capturas.md`;
  - en H, `VaryStatByAccInputReq` sin bajar nunca de 1 HP;
  - **remate nativo** con `AttackByCollisionReq` (atacante = jugador de H) cuando el golpe de G
    sea letal;
  - `AttackTargetAck`/`ApplyActionRestrictionAck` inyectados en H con atacante = marioneta para la
    reacción y el aggro.
- **R2, números exactos:** "golpe sombra" en el servidor de G contra su gemelo
  nativo. Depende de cómo se haga la supresión 3g.
- Descartadas: (a) atacante suplente y los trucos de GM.

---

## 1. La tubería de daño del servidor

### 1.1 Cadena de llamadas [verificado salvo lo marcado]

```
DoFrameEventReq::vf2 0x296D130                      deserializa, valida dueño, orden de objetivos…
 └─ 0x2A896E0 (accessor tipo 0)                     valida la acción en curso (§1.2), mueve objetivos propios (0x2BC5420)
     └─ sistema de frame events [[realm+0x48]]->vf(0x20)   (virtual; fábrica 0x28F0210, tipo 0 → Attack)
         └─ ServerFrameEventAttack::vf1 0x27D8550 → .xtls 0x101A3F00
             ├─ ServerAttackActorComponent ([actor+0x68]+0x80, vt 0x5B1D0C0) vf40 0x28AF220 → .xtls 0x10321260
             │    ├─ contexto tipo 3 (objetivos que manda el cliente) → 0x28AF2B0
             │    │    ├─ AttackInfo = 0x1EA94D0([realm+0x60], clave = [frameEvent+8])  (tipos de FE 0-3)
             │    │    ├─ copia MoveData, lista de objetivos (ctx+0x360) y datos del ataque al struct de resultados
             │    │    └─ 0x1564440 (común: volumen y alcance del ataque, resultados por objetivo)
             │    └─ contexto tipo 0 → 0x1565970 (el servidor barre él mismo)  [inferido]
             └─ 0x17F3E20 → .xtls 0xC89F080  "aplicar resultados" por objetivo:
                  AttackComp vf44 0x28AF5D0 (comprobación)
                  → 0x1566F10 (aplica el ataque: buffs del AttackInfo al objetivo)            [ruta virtual]
                  → vf72 0x28B0F30  PostProcessAttackResult_AttackTargetAck (ToNear)
                  → vf71 0x28B0150  PostProcessAttackResult_AttackerAck (ToNear)
```

**El daño** (buff de daño aplicado al objetivo):

```
ServerDamageBuffProcessor (vt 0x5B2EBA0) vf7 0x2ACC840 → .xtls 0x105EC040
 ├─ 0x209A220 (CommonDamageBuffProcessor, común)
 │    ├─ si el objetivo ya está muerto (+0x273): evento "OnFreezedDeadBodyBreak" y fuera
 │    ├─ 0x209CB30: ¿inmune o invulnerable?
 │    └─ 0x209BDC0 (aplicar daño)
 │         ├─ 0x209A680  FÓRMULA (1.400 instrucciones, sin virtualizar) → daño×1000
 │         ├─ empuje físico y alcance (TransformSync +0x1A0)
 │         └─ 0x17AE080(stats objetivo, HP, ahora, −daño, ATACANTE, …, motivo, tipo)  → 0x17AC040 (§1.4)
 ├─ Status (objetivo) vf51 0x2944120 → .xtls 0x1043CEB0 → escritor OnPostBuffDamagedAck 0x2989490
 └─ 0x209A090 (post-proceso)
```

Componentes del actor que usa la cadena (`[actor+0x68]+off`) [verificado]:

| Offset | Componente |
|---|---|
| `+0x20` | Status (stats en `+0x18`; muerto `+0x273`; hora de muerte `+0x268`) |
| `+0x38` | EquipSlot (arma) |
| `+0x40` | CharacterControl (capas de acción en `+0x88+slot·8`) |
| `+0x58` | GameEventHandler |
| `+0x80` | Attack |
| `+0x88` | Projectile |
| `+0x90` | Target |
| `+0xF0` | Skill (buffs; vf47 `0x2BEE940` aplica un buff y envía `ApplyBuffAck`) |
| `+0x1A0` | TransformSync |

### 1.2 Validaciones de `DoFrameEventReq` (las de `combate-coop.md` §1.4 y las nuevas) [verificado]

Dentro de `0x2A896E0`:

| Comprobación | Código | Error (`exe+…`) |
|---|---|---|
| La capa `slot` tiene acción en curso `[[CC+0x88+slot·8]+0x18]` | `0x2A897A2` | `0x6CF6C60` |
| Hash de la acción `[[x+0x18]+0x18]` == hash del accessor | `0x2A8986C` | silencio |
| u16 del accessor == `[[x+0x38]+0x5E]`. Es la **variante** de la acción: el cliente la indexa como `chart+0x238[acción+0x54 + u16]` (`0x1F78ABE`) | `0x2A8987A` | silencio |
| Índice < nº de frame events. Si `CC+0xC0` > 0 (ecos de sesión pendientes), un índice fuera de rango se acepta sin error | `0x2A8989E` | `0x6CF73D4` |
| **Nuevo:** índice ≥ "siguiente índice permitido" de la capa (`capa+0x44`), que después pasa a índice+1. Cada frame event se procesa **una vez y en orden**, salvo los de tipo ≤4 con ventana de tiempo (golpes múltiples) | `0x2A89BF2`–`0x2A89C0B` | `0x6CF6C9C` |
| **Nuevo:** atacante vivo: `Status+0x273 == 0`, o muerto hace menos de 1 s (`+0x268 + 1000`) | `0x2A89D47` | `0x6CF6C68` |

- Algunos frame events tienen alternativas con peso. El servidor elige una con un LCG
  (`0x343FD`/`0x269EC3`) sembrado con `[[[CC+0x38]]+8]+0xE8` del **atacante** (`0x2A898BA`–`0x2A89AB4`).
- Si los objetivos tienen el mismo dueño que el atacante, sus MoveData pasan al TransformSync
  (`0x2BC5420`) antes de calcular nada.

### 1.3 Qué entra en la fórmula `0x209A680`

Firma: `(res*, atacante, objetivo, ctx*, …, i64* daño_out [7.º], …)` [verificado].

Las stats se leen con `0x17AD670(stats, índice, ahora)`. Los índices salen de la tabla global
`[[exe+0x6D69528 (cliente) / 0x6D69530 (servidor)]+8]+0x10` (u16 por stat):

| Entrada | Dónde se lee | Qué es |
|---|---|---|
| Datos del buff de daño | `ctx+0x10` | `BuffInfo` [verificado]; significados [inferido]. Campos: `+0x120` categoría de daño (0-2, elige la defensa `+0xCC`/`+0xCE`); `+0x115` daño fijo (no suma el ataque del atacante); `+0xB8`/`+0xD8` tablas valor-por-nivel (`0x2404690(tabla, &out, nivel)`); `+0x121` motivo de muerte; `+0x116`/`+0x117`/`+0x118`/`+0x14A`/`+0x14D` banderas |
| Nivel del buff o habilidad | `ctx+0x20` | [inferido] |
| **Ataque del atacante** | stat de índice `+0xA6` | Del **arma** si `ctx+0x30` (objeto) existe: `0x20C5190` sobre EquipSlot `+0x38`. Si no, de las stats del atacante (`0x209A848`–`0x209A8CF`) [verificado] |
| Defensas del objetivo | stats `+0xCC`/`+0xCE` → su `StatusInfo +0x1C`/`+0x1A`; `+0xA8`, `+0xAE`, `+0xA4`, `+0xB4`/`+0xB6` | [verificado] lectura; [inferido] significado |
| HP | `+0xA0` HP, `+0xA2` HP máx | [verificado] |
| Aritmética | Punto fijo: valores ×1000, tasas ×10⁶ (`0xF4240`), MulDiv `0x910E90`, mínimo 1 HP (`max(·, 1000)`) | [verificado] |
| Críticos y aleatorio | No hay LCG dentro de `0x209A680`; los ayudantes `0x209D0F0`, `0x17B06F0`, `0x17B46B0`, `0x17FC3D0`, `0x15A1BD0` están sin abrir | En captura: ×2 exacto en ~4 % [captura, `pieza4-capturas.md` §2.2] |

Con las capturas cuadra:
- el daño por golpe limpio es `base[acción+frame] × defensa[variante]`, con dos clases de defensa;
- es estable entre sesiones con el mismo equipo (`pieza4-capturas.md` §2.2).

### 1.4 HP ≤ 0 → muerte, botín y crédito [verificado + captura]

`0x17AE080(stats, stat, ahora, delta, atacante, b1, b2, motivo, tipo, b3, out)`:
- si `stat == HP` (`+0xA0`) → `0x17AC040`;
- si no → `0x17AE4B0`.

`0x17AC040` (ruta de HP):
1. Lee HP (`+0xA0`) y HP máx (`+0xA2`) y aplica el cambio (`0x17B3230`).
2. Si el HP nuevo es ≤0 con delta negativo:
   - si no está muerto (`+0x273 != 1`):
     - `Status+0x268 = ahora`;
     - `vf(0x150)`;
     - `word [+0x272] = 0x100`, es decir, **`+0x273 = 1` (muerto)**;
     - si el HP máx coincide, `+0x274 = 1`;
   - variante "fatal": con `[registro+0x4F]` o según el estado, `+0x272 = 1` y `vf(0x268)(0x13)`
     (`NotifyHandleFatalAck`) [inferido].
3. Envía `VaryHPAck` (Status vf100 `0x29418B0` → `.xtls 0x10431D40` → escritor `0x2991440`, ToNear)
   con **fuente = `atacante+0x60`**.
4. Rellena el **registro de daño por hilo**:
   - lo devuelve Status vf53 `0x2953C60` → `.xtls 0x1046DA60` = `[[gs:0x58]+0x250]+0x2E0`;
   - si `+0x4C` (activo): `+0x08` = clave del asesino, `+0x4D`/`+0x4E` motivo y tipo,
     `+0x18` hora, `+0x52` murió, `+0x53` fatal.
5. Eventos de juego al componente `+0x58` y facción y habilidades (`0x1ECEC70` sobre `[+0xF0]+0x20`).

El cierre del golpe consume el registro:
- Status vf43 `0x2941B60` → `.xtls 0x10433BD0` → **`NotifyHandleDeadAck`** (escritor `0x29927E0`,
  ToNear: muerto, asesino, motivo);
- y después `0x1797850` → `.xtls 0xC76CF30` (post-muerte; mutado).

En la captura, el botín sale **en el mismo lote** que la muerte:
`NotifyHandleDeadAck` → `UnapplyBuff`×n → `LootDropCharacterRewardListAck` → `ClearAllyGroupDataAck`.
Ejemplo: `112627`, t = 284,664, enemigo `B0100170`, asesino `A0100001`, motivo 8.

El patrón completo "abrir registro → variar HP con atacante → cerrar" aparece tal cual en
`KillCheatReq` (§2.e): Status vf52 `0x29544F0` (`+0x1A0`) → `0x17AE080` → vf54 `0x2954810` (`+0x1B0`).

Motivos vistos en `NotifyHandleDeadAck +0x08` [captura]:
- 8: matado por otro;
- 7: por sí mismo;
- 6: por sí mismo, variante.

El crédito de misión (`KillCount`, `KilledByAtOnce`…) cuelga, con toda probabilidad, del asesino
registrado [inferido; no localizado: el post-muerte está mutado].

### 1.5 Tablas de datos

| Tabla | Paquete | Uso |
|---|---|---|
| `AttackInfo` | `0010` (`attackinfo`). Carga: `0xA4B3F0` ("Load AttackInfo"). Gestor: `0x1EA96E0`/`0x1EA94D0` | Clave = `[frame event+8]` = `AttackTargetAck +0x24`. `[info+0x48]` → tipo (`+4`) y datos (`+8`): buffs o habilidad del ataque [verificado] |
| `ProjectileInfo` | `0010` (`projectileinfo`), mismo cargador | Clave de `AttackByCollisionReq +0x24` (§2.c) |
| `BuffInfo` (`BuffInfoManager`) | `0008` (`buffinfo.staticinfo`) | El buff de daño: valor por nivel, tipo y banderas (§1.3) |
| `SkillInfo` | `0008` (`skillinfo.staticinfo`) | Niveles → listas de buffs (`ApplySkillFromCollideObject` lee `[info+0x18]` por nivel, máx `+0xF8`) |
| `StatusInfo` | `0008` | Definición de cada stat; enlaza las defensas (`+0x1A`/`+0x1C`) |
| `CharacterInfo`, `ItemInfo`/`EquipInfo`, `TribeInfo` | `0008` | Stats base, arma, bonificaciones por tribu |
| Chart de acciones | — | Acción → variantes (`chart+0x238`) → frame events `{tipo u16, …, +8 clave AttackInfo}` |

---

## 2. ¿Puede el servidor de H calcular el golpe de G?

### 2.a Atacante suplente (c1): `DoFrameEventReq` con el jugador de H y el accessor de G

Validaciones que fallan si no se prepara nada [verificado]:
- la acción en curso del jugador de H en esa capa no es la de G: hash distinto, descarte en
  silencio.

Para que pase hay que inyectar antes un `StartActionReq` del jugador de H con la acción de G, más
su `ProcessAfterBeginFrameEventReq`. Aun así:
- **u16 de variante:** debe coincidir con la que eligió el servidor para H. Si la acción tiene
  varias variantes, puede no coincidir (descarte en silencio) [inferido].
- **Orden de frame events:** la capa guarda el siguiente índice permitido (`+0x44`). Los índices
  ya usados se rechazan (`0x6CF6C9C`), salvo frames de golpe con ventana [verificado].
- **Atacante vivo** (`+0x273`) [verificado].

Efectos secundarios [inferido, por la captura de un golpe normal]:
- el servidor cambia la acción en curso del jugador de H;
- el siguiente `DoFrameEventReq` real del cliente de H se descarta en silencio hasta su próximo
  `StartActionReq`, así que **H pierde golpes**;
- costes de estamina y buffs de la acción en H (`ApplyBuffAck`×3 y `VaryDataDefinedStaticStat`
  tras cada `StartActionReq`);
- `AttackerAck` y `VarySharpnessAck` para H, y el filo de H baja;
- las alternativas aleatorias salen con la semilla de H.

**El daño usa el ataque y el arma de H** (`0x209A848`: atacante = actor del `DoFrameEventReq`).

Veredicto: no. Frágil, invasivo y con números ajenos.

### 2.c `AttackByCollisionReq` (2815) [verificado + captura]

Formato (44 B):
`u32 víctima | u32 atacante | u32 0 | vec3 punto | vec3 dirección | u32 clave | u32 0`.

```
0x2A3D430 (síncrona) → 0x2C3D7D0
   la sesión debe tener personaje ([actor+0x88]+1 != 0)
   víctima: existe, dueño (+0x90) == dueño de la sesión, mismo field
   atacante: opcional, mismo field
 → 0x2BDA2F0 (ProjectileComp de la víctima)
   si clave == 0 y último u32 == 0 → nada
   atacante = el del paquete; si no existe, se usa otro actor de respaldo ([víctima+0xA0]) [inferido]
 → 0x2BE11A0 (ProjectileComp del ATACANTE)
   definición = 0x21E3EF0(comp, clave) → 0x23161C0 (gestor de datos, .xtls)
     → tipo [def]+0x1B (0-10); si no existe → error 0x6CF744C, sin Nak
   → 0x21E0AC0/0x21E0BD0 → 0x21DC0C0 (crea el proyectil) → 0x2BD7710 = ProjectileComp vf42 → CreateProjectileAck
```

En la captura (105149, t = 495,835) [captura]:
1. `AttackByCollisionReq(B01005D1, 0, …, 0xF2EE2678)`.
2. `CreateProjectileAck` (dueño `B010006C`, un gimmick).
3. El cliente simula el proyectil y manda `DoFrameEventReq` de tipo 1 con accessor de proyectil.
4. Respuesta nativa: `AttackTargetAck` (`B01005D1` ← `B010006C`, flags `E7`), `VaryHPAck` (−1 HP,
   fuente `B010006C`), `OnPostBuffDamagedAck` (1000), `ApplyBuffAck` (`0x3FF`).
5. Reacción del enemigo, 25 ms después.

Ocurrió 6 de 6 veces, siempre −1 HP.

Prueba T3 del mod (173354): `AttackByCollisionReq(B0100157, A0100001, …, 0x81048142)` ×10 →
**sin respuesta** [captura, `pieza4-capturas.md` §7]. `0x81048142` es una clave de `AttackInfo`
(`26AA9809/8`), no de `ProjectileInfo`: falla en `0x21E3EF0`.

Conclusión:
- La clave es de `ProjectileInfo`.
- El daño lo pone ese proyectil con las stats de su dueño.
- Produce de forma nativa `AttackTargetAck`, reacción, `VaryHPAck` con fuente, muerte con
  **asesino = atacante del paquete** y botín.
- **No transporta el daño de G.** Sí sirve de **remate nativo con atribución** (§6, R1): enemigo a
  1 HP + colisión de 1 HP con atacante = jugador de H. Falta confirmar que `0xF2EE2678` funciona
  con el jugador como dueño y cuánto quita entonces (prueba X2).

### 2.d `VaryStatByAccInputReq` (2181) sobre el HP

`0x2972720` → `0x17AE4B0(stats, índice, ahora, delta, fuente = 0)`. Es la ruta **genérica**: no
pasa por `0x17AC040`.

Con la definición del HP [verificado]:
1. `0x17B3230` (aplica y recorta al mínimo y al máximo).
2. Status vf103 `0x1798BF0` (umbrales): si el stat cruza su mínimo y es el HP → evento de juego
   **`OnBattle_HandleKnockOut`** (global `0x6A6C658`) al componente `+0x58` (`0x207AA10`). En otro
   caso, `OnStatLimitReached` (`0x6A6C668`).
3. vf100 → `VaryHPAck` con **fuente 0** y secuencia `+0x40` incrementada.
4. `0x1798740`.

No toca `+0x273`, no abre registro de daño y no tiene atacante.

**Resultado en las capturas de hoy** (`vsba_zero.py`, 14 sesiones) [captura]:

| Tras `VaryStatByAccInputReq` que deja el HP en 0 (53 enemigos, 55 inyecciones) | Enemigos |
|---|---|
| Muere sin más | **0** |
| Muere con el siguiente golpe nativo dañino (asesino = ese atacante, motivo 8, botín en el mismo lote cuando lo hay) | 50 (48 del jugador, 2 de NPC aliados) |
| Sigue vivo ≥60 s a 0 HP, atacando y lanzando proyectiles | 3 |

- Ejemplo: `095647`, `B0100206`.
  - t = 183,957: `VaryStatByAcc −28` → `VaryHPAck hp 0, fuente 0`.
  - t = 185,86: un golpe sin daño no lo mata.
  - El enemigo dispara proyectiles a los 185,9 s y sigue interactuando a los 191 s.
- El "verificado: HP 56→0 y `NotifyHandleDeadAck`" del commit `00d32e1` corresponde a un golpe
  nativo posterior.

Respuestas a las preguntas:
- **muerte:** no;
- **botín:** no, hasta que un golpe nativo lo remata;
- **crédito de misión:** no;
- **`RemoveActorAck`:** solo tras una muerte nativa.

`OnBattle_HandleKnockOut` no mata a los enemigos humanoides de estas capturas. Para el jugador no
está probado (§8).

### 2.e Otras entradas candidatas (`troc_messages_2976.csv`)

| Req (id) | Handler → núcleo | Qué hace | ¿Sirve? |
|---|---|---|---|
| `OnHitProjectileReq` (2999) | `0x2A3A1C0` → `0x2BDFEC0`, `0x21E1010` | Impacto de un proyectil **que ya existe en el servidor** y es del dueño de la sesión. Valida dueño de proyectil y objetivo [verificado] | No: las flechas de G no existen en H |
| `AttackByCollisionReq` (2815) | §2.c | Proyectil de colisión por clave de `ProjectileInfo` | Remate o atribución |
| `ApplySkillFromCollideObjectReq` (2806) | `0x296E6A0` → `0x2BF59C0` (Skill) | `u32×5 + u8`. Aplica los buffs del nivel N de una `SkillInfo` a un actor vía Skill vf47 con motivo `0x13`. Fuente = **el propio actor** (`[comp+8]`) [verificado] | No: el daño se calcularía con las stats del enemigo, y sin asesino |
| `ChangeSkillFromCollideObjectReq` (2330) / `Unapply…` (2260) | `0x296F230` → `0x2BF5D80` | Variantes de lo anterior | No |
| `ForceApplySkillForDebugReq` (2531) | `0x2A647B0` | **Vacía**: solo deserializa [verificado] | No |
| `KillCheatReq` (2279, diferida) | `0x29BF730` → `0x2C40C00` | `u32 víctima, u32 opción`. **Permiso** `[[actor+0xA0]+0x68]+0x130 → vf(0x140)(2)`; si falla, Nak `0x3F5`. Si pasa: abrir registro (Status vf52), `0x17AE080(HP, −HP, atacante = actor de la sesión, motivo 0x10, tipo 0xA)`, cerrar (vf54) [verificado] | Quizá: muerte nativa con asesino = jugador de H. Depende del permiso (prueba X4) |
| `VaryStatCheatReq` (2703) | `0x29BF3C0` → `0x2C3FBF0` | Mismo permiso, con nivel 3 [verificado] | Igual que arriba |
| `SpawnCharacterCheatReq` (2115), `SpawnCharacterForActionTestReq` (3058) | `0x2C3ED50` / `0x2C3F4D0` | Generan un personaje; mismo permiso (`0x2C3EF46`, `0x2C3F65C`) [verificado] | Para R2, si hubiera permiso |
| `CharacterPresetSpawnNPCReq` (2440) | `0x298F490` | Vacía | No |
| `ClearKillerActorDataReq` (2448) | `0x298CEC0` → `0x17933A0` | Borra `Status+0x254/+0x258/+0x260/+0x275` ("datos del asesino") [verificado] | Solo informativa |
| `KillJammedTargetReq` (2392), `SetIgnoreDamageByObserverReq` (2798), `AttackResultBackgroundOnlyReq` (2577) | `0x29D0380`, `0x29D1E70`, `0x2973030` | Sin abrir a fondo | — |

Los 14 usos del permiso de trucos (`vf(0x140)` sobre el componente `+0x130`) son todos handlers de
trucos (tipos 0, 1, 2, 3, 6, 9…). El componente `+0x130` no está identificado:
`ServerContentsMiscActorComponent` tiene `vf40 = mov al, 1; ret`, pero no se ha comprobado que esté
en `+0x130` [inferido]. La prueba X4 lo decide.

---

## 3. Magnitud del daño cuando no puede ser nativo

**¿Lo puede calcular el lado de G?**
- **Su cliente no.** `ClientDamageBuffProcessor` vf7 es el stub `0x35EA90`: el cliente nunca
  calcula daño [verificado].
- **Su servidor solo contra un actor de servidor real.** La copia no lo es.

| Vía | Exactitud | Requisito | Riesgo |
|---|---|---|---|
| **M1. Tablas aprendidas** (`pieza4-capturas.md` §2, §6): `base[acción+frame]` en G (persistente, sembrada) × `defensa[variante]` en H | ~83 % de golpes a ±1 HP con la tabla sembrada; sin críticos (4 %) ni bloqueos (11 %) [captura] | Ninguno | Nulo |
| **M2. Golpe sombra en el servidor de G** | **Exacta**: stats, arma, habilidades, críticos, filo, buffs de G | Un enemigo nativo de G de la misma variante (su gemelo), vivo y en el servidor de G | Medio (§6, R2) |
| M3. Fórmula offline (cadena de tablas de §1.5) | Alta, en teoría | Descodificar PAZ, `AttackInfo`/`BuffInfo`/`StatusInfo`/charts y reproducir 1.400 instrucciones; además, leer en vivo las stats efectivas del atacante | Esfuerzo muy alto |
| M4. Llamar a `0x209A680` desde el mod en el cliente de G | — | Construir el contexto de buff a mano en un realm que no lo usa | **Descartada**: si falla, crash, y el informe de crash va a Pearl Abyss |

**M2, receta** [inferido; se apoya en §1.2, verificado]:
- En el hook de `PostPacket` de G, si un `DoFrameEventReq` del jugador nombra una copia:
  1. sustituir la clave por la de su **gemelo nativo**;
  2. emparejar el gemelo por variante y punto de aparición, como ya hace el daño espejo;
  3. reescribir el MoveData de la entrada;
  4. **reordenar** las entradas (claves crecientes).
- El servidor de G mueve el gemelo a esa posición (mismo dueño, `0x2BC5420`) y calcula el golpe de
  forma nativa.
- El hook S→C de G captura `OnPostBuffDamagedAck +0x0C`, los flags y el hash de `AttackTargetAck`,
  y `ApplyActionRestrictionAck`. Los manda a H y **descarta** esos Ack para que el cliente de G
  no haga reaccionar al gemelo.
- Coste:
  - el gemelo pierde HP en el servidor de G y puede morir allí, con botín y crédito en el mundo de
    G;
  - hay que tenerlo oculto y quieto;
  - hay que curarlo (`VaryStatByAccInputReq +daño`) si debe seguir vivo.
- **Choca con la supresión 3g** si esta borra las spawns de G. Solo sirve si 3g "oculta y congela"
  en vez de "no crea".

---

## 4. Reacción y efectos en H

### 4.1 Qué hace el cliente con un `AttackTargetAck` [verificado + captura]

1. `0xB59280`:
   - deserializa con `0x24431B0`: el struct copia `+0x00` y luego `0x1F77B50` lee un bloque.
     `+0x24` del cable acaba en `struct+0x20`, la **clave de `AttackInfo`**;
   - busca el objetivo.
2. `0x451020` (ClientAttack del objetivo `[+0x68]+0x80`):
   - busca al **atacante** en el gestor del cliente (`[exe+0x6D69190]+0x30`, `0x8AE940`);
   - la marioneta existe en H.
3. `0x45C8A0`: la reacción.
   - Resuelve el frame event del atacante con el accessor del final (`0x1F78A20`, sobre la acción
     **que el atacante está ejecutando**).
   - Busca el `AttackInfo` (`0x1EA94D0`).
4. `0x45D800` → `.xtls 0x89715B0` → **`SetCurrentTarget 0x2217290`**, si se cumple todo esto:
   - el atacante cumple `vf(0x178)`;
   - objetivo ≠ atacante;
   - no es un gimmick;
   - `StatusInfo +0xB8 != 2`;
   - condiciones del `AttackInfo`.

El cliente de H lanza la reacción de un enemigo que controla y la comunica con Req reales
(`Echo`, `ChangeAIState`, `StartActionReq`). Así llega también a la copia de G por el relevo 3b
[captura, `pieza4-capturas.md` §4.2].

El eco `EchoActionSessionIDReq` que sigue es inofensivo [verificado]:
- `0x296E1A0` solo decrementa `CC+0xC0` si la sesión coincide;
- si no, no hace nada y no envía Nak.

### 4.2 Receta mínima S→C en H (por golpe de G que acierta)

| Orden | Mensaje | Campos |
|---|---|---|
| 1 | `VaryStatByAccInputReq` C→S (2181, 16 B) | `u32 enemigo | u32 0x000F4240 | i64 −min(daño, HP−1)×1000` (§6: nunca a 0) |
| 2 | `PostProcessAttackResult_AttackTargetAck` S→C (2922, 199 B) | Plantilla: el último real de H con el **mismo hash de ataque** (acción+frame → hash, `pieza4-capturas.md` §2.3). Reescribir: `+0x00` enemigo, `+0x04` **`0xA010F001`**, posiciones `+0x69`/`+0x83` (las del enemigo en H y el impacto que manda G), ids de ataque nuevos (`+0x2D`/`+0x41`) y el **accessor de G** (12 B finales, de su `DoFrameEventReq +0x1F7`). Los flags y `+0x10` de la plantilla (`F7`/7) |
| 3 | `ApplyActionRestrictionAck` (2162, 77 B) | Si P(AAR) ≥ 0,8 para esa clave. Plantilla del mismo hash; `+0x00` enemigo, `+0x49` marioneta |
| 4 | `OnPostBuffDamagedAck` (2681, 53 B) | Cosmético: `+0x00` enemigo, `+0x04`/`+0x08` marioneta, `+0x0C` daño×1000, `+0x2C` id de ataque |

- No hace falta `AttackerAck`: es para el atacante.
- No hay que inyectar `NotifyHandleDeadAck` ni botín: los emite el servidor de H con el remate
  (§6).
- Riesgo [inferido]: `0x45C8A0` resuelve el accessor sobre la acción en curso de la marioneta. Si
  el `StartActionAck` de G aún no ha llegado, la reacción puede degradarse o no salir. El
  remedio es aplicar el golpe después del `StartAction` relevado de esa acción (la cola del
  espejo ya ordena por tiempo).

---

## 5. Aggro

- **Nativo** [verificado + captura]: el `AttackTargetAck` inyectado con atacante = marioneta pasa
  por `0x89715B0 → SetCurrentTarget`. Si las condiciones se cumplen, el enemigo cambia a la
  marioneta y el cliente de H emite un `CurrentTargetChangeReq(enemigo → 0xA010F001)` real.
  - En `112627`, t = 269,551: `B0100170`, golpeado por un proyectil de `B0100003`, cambió a
    `B0100003` 16 ms después (motivo `0x1B`).
  - El 95 % de los enemigos ya apunta al jugador antes del golpe, y entonces no cambian
    (`pieza4-capturas.md` §5.1).
- **Condición sin confirmar:** `vf(0x178)` de la marioneta (en el servidor, "controlado por
  usuario"). La marioneta es un actor de tipo 1 de otro usuario. Probablemente cumple, pero hay
  que probarlo (X3) [inferido].
- **Forzado** (lo que ya existe): `CurrentTargetChangedAck` (3040) con `force = 1`,
  `seq = [comp+0x170]+1` y motivo `0x18` (`companeros-mercenarios.md` §4, `aggro_test.cpp`).
  - Política de amenaza con histéresis de ≥5 s: `pieza4-capturas.md` §5.2.
  - Requisito: la marioneta tiene que ser un objetivo válido en H (`enemies_target_copy`).
- **Lista de aggro del servidor:** la mantiene el cliente (`AggroRelationListReq`) y la reenvía
  solo después del cambio. No hay que tocarla.

---

## 6. Recomendación ordenada

| # | Opción | Fidelidad | Riesgo | Esfuerzo |
|---|---|---|---|---|
| **R1** | Tablas (M1) + `VaryStatByAcc` hasta 1 HP + **remate nativo** + reacción y aggro inyectados | Media-alta: HP y muerte reales con asesino; reacción nativa; ~83 % del daño a ±1 HP; sin críticos ni bloqueos | Bajo (todo son mensajes del juego; sin llamadas internas) | 2-4 días |
| **R2** | R1 con el daño de M2 (golpe sombra en G) | Alta: números exactos de G | Medio (toca el mundo de G; depende de 3g) | +3-5 días |
| R3 | R1 con remate por `KillCheatReq` en vez de colisión | Igual que R1 | Bajo, si hay permiso | Horas (prueba X4) |
| R4 | Hook de "VaryStat con atacante": entrada de `0x2972720`; con un id de stat marcado, ejecutar la secuencia de KillCheat (vf52 → `0x17AE080(HP, −daño, jugador de H, motivo 8, tipo 0xA)` → vf54) | R1 sin remate aparte | **Medio-alto**: llamadas internas desde el hook; si fallan, crash del juego | 2 días + pruebas |
| ✗ | (a) atacante suplente | Baja (stats de H) | Alto (rompe los golpes de H) | — |

### Receta R1 (por golpe de G)

**En G:**
- `DoFrameEventReq` C→S del jugador cuya lista nombra copias → evento `GuestHit` por copia (formato
  en `pieza4-capturas.md` §6.1).
- Añadir:
  - el accessor completo (12 B);
  - el **flag de letal estimado**.
- Sube `kVersion`.

**En H**, con HP actual del enemigo `hp` (del último `VaryHPAck`) y daño estimado `d`:
1. Si `d` ≤ 0 o el frame es de "sin daño" (`E3`): solo la reacción (pasos 3-4).
2. Si `d < hp`: `VaryStatByAccInputReq(E, HP, −d×1000)`.
3. Si `d ≥ hp`:
   1. `VaryStatByAccInputReq(E, HP, −(hp−1)×1000)`: E a 1 HP;
   2. **remate:** `AttackByCollisionReq(E, 0xA0100001, 0, pos(E), dir, K, 0)`, con `K` una clave de
      `ProjectileInfo` validada en X2 (candidata `0xF2EE2678`).
   - Resultado nativo: `CreateProjectileAck` → `DoFrameEventReq` tipo 1 de H → `AttackTargetAck`,
     `VaryHPAck`, `NotifyHandleDeadAck(asesino = jugador de H, motivo 8)`, botín y crédito.
   - Si X2 falla: `KillCheatReq` (R3), o dejar E a 1 HP hasta el próximo golpe nativo y avisar.
     Nunca dejarlo a 0.
4. `AttackTargetAck` S→C (atacante = marioneta) y, según la tabla, `ApplyActionRestrictionAck` (§4.2).
5. Aggro (§5).

Garantías:
- nunca hay enemigos a 0 HP vivos;
- toda muerte es nativa y con asesino;
- el botín y `RemoveActorAck` salen del servidor de H y llegan a G por el relevo 3c.

Límites [inferido]:
- el crédito de la muerte va al **jugador de H**;
- G no recibe crédito de misión en su mundo (necesitaría R2 o un relevo de misiones);
- G pierde los efectos de atacante que da su servidor: filo, buff 1081 (`pieza4-capturas.md`
  §4.3).

---

## 7. Experimentos en un PC

El montaje habitual:
- `cdmp_peer` en eco;
- H = G = el mismo juego;
- **copia de la partida en `backups/` antes de cualquier inyección C→S** (las reglas del proyecto, CONTRIBUTING.md).

| # | Prueba | Cómo | Decide |
|---|---|---|---|
| X1 | ¿`VaryStatByAcc` mata? | **Hecho sobre las capturas** (§2.d, `vsba_zero.py`): 0 de 53 | — |
| X2 | Remate por colisión | `Ctrl+F6` (T2) hasta dejar un enemigo aislado a ≤1 HP. Después, T3 con **`K = 0xF2EE2678`**, atacante `0xA0100001`, punto = posición del enemigo, dirección unitaria. Repetir con atacante 0. Leer: `CreateProjectileAck` (¿dueño?), `DoFrameEventReq` tipo 1 de H, `AttackTargetAck`, `OnPostBuffDamagedAck +0x0C`, `NotifyHandleDeadAck +0x04/+0x08`, `LootDrop…` | Si R1 puede rematar con asesino = jugador. El daño de la colisión con el jugador como dueño. Si `K` vale para cualquier dueño |
| X3 | Reacción y aggro por `AttackTargetAck` inyectado | Enemigo que **no** apunte al jugador (pelea con otro NPC). Inyectar S→C la plantilla del último `AttackTargetAck` del jugador, con `+0x04 = 0xA010F001` y el accessor de un golpe reciente de la marioneta. Buscar en ≤100 ms: `StartActionReq`/`ChangeAIStateReq` del enemigo y `CurrentTargetChangeReq(→A010F001)` | Si la reacción y el aggro salen nativos con la marioneta como atacante (`vf(0x178)`) |
| X4 | Permiso de trucos | `KillCheatReq` (2279, 8 B: `u32 enemigo, u32 0`), canal de las Req diferidas. ¿Nak `0x3F5` o `NotifyHandleDeadAck` (asesino jugador, motivo 0x10)? | R3 |
| X5 | Crédito de misión | Matar un enemigo de una misión de "eliminar N" con el remate de X2, y comparar los Ack de misión con una muerte normal | Si la atribución basta para las misiones de H |
| X6 | El golpe sombra (R2) | En eco, con `mirror_offset_m` ≥ 10: reescribir el `DoFrameEventReq` de un golpe a una copia para que apunte a su original (claves crecientes). Leer `OnPostBuffDamagedAck`. Debe coincidir con el daño de un golpe directo con la misma acción | Viabilidad de M2 (en eco, el "gemelo" es el original) |
| X7 | Solo log | Hook de lectura en la entrada de `0x2A896E0` (T5 de `combate-coop.md`) | Solo si alguien quiere retomar (a) |

---

## 8. Avisos para lo que ya existe

1. **Daño espejo** (`local_capture.cpp`, `apply_remote_damage`) y **pieza 2** (`player_hits.cpp`):
   - aplican el daño con `VaryStatByAccInputReq`. En las capturas de hoy dejaron **53 enemigos a
     0 HP**; 3 siguieron peleando "muertos" [captura].
   - Arreglo inmediato: **no bajar nunca de 1 HP**, y rematar como en R1.
   - En el jugador (pieza 2), un golpe que lo deje a 0 dispara `OnBattle_HandleKnockOut` en vez de
     la muerte nativa. Qué hace eso con un jugador está **sin probar**: puede quedar "vivo a 0".
     Mientras no se pruebe, recortar a 1 HP [inferido].
2. El relevo 3c envía a G los `VaryHPAck` a 0 de esos enemigos "zombis": la copia muestra la barra
   vacía y sigue actuando.
3. La prueba T3 actual (`mod.cpp`, `capture_last_attack_hash()`) manda una clave de `AttackInfo`,
   así que el servidor la ignora en silencio (§2.c). Para X2 hay que pasarle una clave de
   `ProjectileInfo`.

---

## 9. Direcciones (build 2976)

| RVA | Qué es |
|---|---|
| `0x27D8550` → `.xtls 0x101A3F00` | `ServerFrameEventAttack::vf1` |
| `0x5B1D0C0` | vtable `ServerAttackActorComponent`: vf40 `0x28AF220` (→ `.xtls 0x10321260`), vf44 `0x28AF5D0`, vf71 `0x28B0150` (AttackerAck), vf72 `0x28B0F30` (AttackTargetAck) |
| `0x28AF2B0` | Prepara el ataque con objetivos del cliente (contexto tipo 3); busca el `AttackInfo` |
| `0x1EA94D0` / `0x1EA96E0` | Gestor de `AttackInfo` (en `[realm+0x60]`): buscar / cargar |
| `0x1564440` | Proceso común del ataque (volumen y resultados) |
| `0x17F3E20` → `.xtls 0xC89F080` | Aplica los resultados por objetivo y envía AttackTarget/Attacker |
| `0x1566F10` | Aplica el ataque al objetivo |
| `0x5B2EBA0` | vtable `ServerDamageBuffProcessor`: vf7 `0x2ACC840` → `.xtls 0x105EC040` |
| `0x209A220` / `0x209BDC0` / `0x209A680` | Procesador de daño común / aplicar daño / **fórmula** |
| `0x17AE080` | Variar stat con atacante (HP → `0x17AC040`; resto → `0x17AE4B0`) |
| `0x17AC040` | Variación de HP: muerte (`Status+0x273`), registro de daño, `VaryHPAck` con fuente |
| `0x17AE4B0` | Variación genérica de stat (la que usa `VaryStatByAccInputReq`) |
| `0x1798BF0` | Status vf103: umbrales → `OnBattle_HandleKnockOut` / `OnStatLimitReached` |
| `0x5B203F0` | vtable `ServerStatusActorComponent`: vf43 `0x2941B60` (muerte → `NotifyHandleDeadAck`), vf46 `0x2946F00` (revivir) [inferido], vf51 `0x2944120` (`OnPostBuffDamagedAck`), vf52 `0x29544F0` / vf54 `0x2954810` (abrir / cerrar registro) [inferido], vf53 `0x2953C60` (registro por hilo), vf100 `0x29418B0` (`VaryHPAck`), vf101 `0x29419E0` (otros stats) |
| `0x6A6C658` / `0x6A6C668` / `0x6A6C618` / `0x6A6B758` | Ids (en tiempo de ejecución) de `OnBattle_HandleKnockOut` / `OnStatLimitReached` / `OnBattle_MinHpRateActivated` / `OnFreezedDeadBodyBreak` |
| `0x2A3D430` → `0x2C3D7D0` → `0x2BDA2F0` → `0x2BE11A0` | `AttackByCollisionReq` → proyectil de colisión |
| `0x21E3EF0` / `0x23161C0` | Definición de proyectil por clave |
| `0x5B2D468` | vtable `ServerProjectileActorComponent` (vf42 `0x2BD7710` = crear + `CreateProjectileAck`) |
| `0x29BF730` → `0x2C40C00` | `KillCheatReq` (permiso `+0x130` vf40, tipo 2) |
| `0x29BF3C0` → `0x2C3FBF0` | `VaryStatCheatReq` (permiso tipo 3) |
| `0x296E6A0` → `0x2BF59C0` | `ApplySkillFromCollideObjectReq` |
| `0x298CEC0` → `0x17933A0` | `ClearKillerActorDataReq` |
| `0x296E1A0` | `EchoActionSessionIDReq` (inofensivo si la sesión no coincide) |
| `0xB59280` → `0x451020` → `0x45C8A0` / `0x45D800` → `.xtls 0x89715B0` → `0x2217290` | Cliente: `AttackTargetAck` → reacción / cambio de objetivo al atacante |
| `0x24431B0` | Deserializador de `AttackTargetAck` (cliente) |
| `0x1F78A20` | Cliente: accessor → frame event (u16 = variante: `chart+0x238[acción+0x54+u16]`) |
| `0x6CF6C60`, `0x6CF73D4`, `0x6CF6C9C`, `0x6CF6C68`, `0x6CF744C` | Errores: sin acción / índice fuera de rango / frame ya usado / atacante muerto / proyectil desconocido |

---

## 10. Scripts (`build/research/pieza4/`)

Los míos; los `q*_*.py` son de `pieza4-capturas.md`.

| Script | Qué hace |
|---|---|
| `uptree.py <rva>[,…] [prof]` | Árbol de llamadores: xrefs directas, stubs `.rsrc`→`.xtls` y ranuras de vtable |
| `path.py <desde> <hasta>[,…] [prof]` | Busca un camino de llamadas directas (sigue stubs) |
| `vcall.py <ranura> <comp>` | Llamadas virtuales `call [reg+ranura]` sobre `[[x+0x68]+comp]` |
| `vtl.py <vtable>` | Lista una vtable con el destino de cada stub |
| `scan2181.py`, `vsba_effects.py`, `vsba_zero.py` → `vsba_zero_out.txt`, `vsba_effects_095647.txt` | Efectos de los `VaryStatByAccInputReq` inyectados (§2.d) |
| `deadscan.py`, `deathtl.py`, `keytl.py`, `scanop.py` | Muertes (asesino y motivo), líneas de tiempo por clave, búsqueda por opcode |
| `s_*.asm` (`.rsrc`), `x_*.asm` (`.xtls`), `c_*.asm` (cliente), `f_*.asm` | Volcados citados (fórmula `s_209a680_damageformula.asm`, HP `s_17ac040_varyhp.asm`, colisión `s_2bda2f0_collision.asm`/`s_2be11a0.asm`, KillCheat `s_2c40c00_killcheat.asm`…) |
