# Compañeros ("mercenarios") y objetivos de los enemigos: ¿puede el invitado ser un blanco nativo?

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Direcciones en RVA (`exe+…`).
Análisis de solo lectura (capstone sobre el exe en disco) más las capturas de
`D:\cdmp-grabaciones\cdmp_packets_*`. El juego no se ejecutó y no se tocó nada de `D:\SteamLibrary`.

Marcas:
- **[verificado]**: leído en el desensamblado;
- **[captura]**: visto en las capturas;
- **[inferido]**: deducción; hay que confirmarlo en vivo.

Scripts y volcados en `build/research/merc/`, descritos en el §9.

---

## 0. Resumen

1. **La IA de los enemigos corre en el cliente del host, que elige el objetivo y se lo comunica al
   servidor** [verificado + captura].
   - La cadena es `ClientTargetActorComponent` (componente `[[actor+0x68]+0x90]`) →
     `SetCurrentTarget` común `0x2217290` → slot 53 de la vtable (`0xA28270` → `.xtls 0x9DAA0F0`)
     → `CurrentTargetChangeReq` (2984) por `PostPacket`.
   - El emisor solo filtra dos cosas: que el enemigo sea del usuario local
     (`actor+0x90 == usuario local`) y que la clave del objetivo tenga el bit 30 a 0.
     **La marioneta `0xA010F001` pasa los dos filtros.** Si en ninguna captura aparece como
     objetivo es porque **la selección de la IA nunca la escoge**: el emisor no la descarta.
2. **No hace falta encontrar y parchear el filtro de selección para el primer experimento.**
   El juego trae un mensaje nativo para imponer un objetivo:
   - `CurrentTargetChangedAck` (3040, S→C, 18 B, handler `0xB68520`) llama al mismo
     `SetCurrentTarget` `0x2217290` sobre el enemigo;
   - **no pasa por `IsTargetable`** y busca el objetivo **en el gestor de actores del cliente**,
     donde la marioneta sí existe [verificado].
   - Con el byte `+0x10` = 1 funciona también con enemigos de IA local [verificado].
   - Es la palanca mínima: un paquete inyectado como los que el mod ya inyecta. Que la IA luego
     persiga y golpee a ese objetivo, o lo cambie en el siguiente ciclo, hay que verlo en vivo
     [inferido].
3. **El filtro de candidatos aún no está localizado del todo.**
   - Sí está la validación común `CommonTargetActorComponent::IsTargetable` `0x22141e0` (57
     llamadores), con todas sus comprobaciones listadas en el §3.3.
   - Ninguna de ellas descarta por sí sola a la marioneta de forma evidente. Lo más probable es
     que la marioneta **no llegue a la lista de candidatos** (aggro o detección) [inferido].
   - Una prueba de solo lectura con un hook en `0x22141e0` lo decide en minutos (§7, P1).
4. **Los golpes de un enemigo al jugador los informa el cliente del host** con
   `DoFrameEventReq` [captura].
   - Formato: atacante = enemigo; entrada de objetivo con la clave del jugador, su MoveData, el
     punto de impacto y el hueso.
   - El servidor responde con `AttackTargetAck`/`VaryHPAck`/`OnPostBuffDamagedAck` del jugador.
   - Un golpe a la marioneta saldría igual, con `0xA010F001` como objetivo. Se puede interceptar
     en el mid-hook de `PostPacket` y reenviar al invitado (§5).
5. **Compañeros:**
   - Los "mercenarios" del jugador son **actores hijos del usuario en el servidor**
     (`ServerChildOnlyInGameActor`, una ranura por hijo) y se cargan en el cliente como registros
     con dueño = usuario e índice de ranura [captura + verificado].
   - En las capturas solo hay llamadas al **caballo**. No hay ninguna llamada a un compañero de
     combate, ningún cambio de personaje y ningún `ChangeMainMercenaryAck`. Hay que capturarlas
     (§8).
   - "Habitar" un compañero es viable en teoría: el host es su dueño, así que **puede mandar Req
     nativas en su nombre**. Pero es bastante más caro y arriesgado que el objetivo forzado
     (§6).

**Recomendación:**
1. Primero, P1 + P2 del §7 (diagnóstico de solo lectura y `CurrentTargetChangedAck` forzado
   sobre un enemigo).
2. Si la IA acepta el objetivo y ataca, seguir con el relevo de golpes (§5).
3. La vía del compañero queda como plan B y necesita antes las capturas del §8.

---

## 1. Invocar un compañero, de principio a fin

### 1.1 Qué hay en las capturas

Recuento en las 38 capturas (`grep -E "Mercenary|Summon|Accompany|Playerble|Party"`)
[captura]:

- Al cargar, en todas: `MercenaryEquipmentListAck` ×36, `MercenaryDataListAck` ×21,
  `RefreshMercenaryDataAck` ×3, `JoinPartyAck` ×3, `OpenPartyAck`, `ChangeMercenaryFactionInfoAck`,
  `UpdateMercenaryExpandedSlotListAck`, y del cliente `LoadMercenaryStatReq` (0 B).
- Al llamar al caballo: `FrameEventCallMercenaryReq` (2166) → `CallVehicleMercenaryAck` (2955).
  Aparece en 105149, 112627, 160139, 183530, 205914, 211630 y 225741.
- `GimmickExecuteSummonCharacterFrameEventReq` (2603) y
  `ChangeSummoneeToIndependentByInitTriggerReq` (2228): invocaciones de gimmicks o misiones
  (183530, 200328), no compañeros.
- **No aparecen nunca:** `SelectMercenarySpawnReq`, `CallHyosiByQuickSlotReq`,
  `RequestSwitchMercenarySummonStateReq`, `AccompanyMercenaryAck`,
  `SummonMercenaryAfterRegistAck`, `ChangeMainMercenaryAck`, `ChangePlayerbleCharacterReq/Ack`,
  `UpdateMercenaryPositionAck` ni `MercenaryDeadAck`.

### 1.2 Actores del usuario al cargar (ejemplo de la captura 112627) [captura]

`horse3/recs.py` muestra, tras el jugador, tres registros con **dueño = usuario `0x90100000`** e
índice de ranura del contenedor de hijos:

| Ranura | Tipo de registro | Clave | N.º de mercenario (`RefreshMercenaryDataAck +8`) | Comportamiento en la captura |
|---|---|---|---|---|
| 0 | 1 (jugador) | `0xA0100001` | — | |
| 1 | **4 (mercenario)** | `0xB0100002` | 223 | `StartActionReq`, `SetQuestDialogReq`, `ChangeAIStateReq`: quieto o en diálogo. No combate |
| 2 | 5 (montura) | `0xB0100003` | 12 | El caballo |
| 3 | **6** | `0xB0100004` | 650 | Sigue al jugador (`UpdateAIDestinationReq`, `MoveActorReq`, `AITeleportReq`). Fija como objetivo a NPC `0xB01…` y les lanza `DoFrameEventReq`. Probablemente la **mascota** [inferido] |

El tipo 4 es el que el minimapa trata como "mercenario" (`mapa-minimapa.md` §2.2). La interfaz
distingue tres categorías: `UI_MercenaryTypeName_Mercenary`, `_Vehicle` y `_Pet`
(cadenas del exe).

### 1.3 ¿Hay `ServerActor`? Sí [verificado + captura]

- `SAM::CreateUser` (`0x2A83310`) crea un `ServerChildOnlyInGameActor`
  (vtable `0x5B1E2B0`, 0xD8 B) **por cada ranura de hijo** del usuario y lo cuelga de su
  `ChildContainer`.
- El servidor acepta las Req del cliente para `0xB0100002`/`0xB0100004`: `MoveForceAck` y
  `ChangeMoveSessionIDAck` en respuesta a `AITeleportReq` de `0xB0100004` (`keyrefs.py`).
- Antes de cada carga manda el "preámbulo" (`VaryHP`, buffs, `RefreshMercenaryDataAck`,
  `JoinPartyAck`…; `caballo-coop.md` §9.3).

### 1.4 La llamada (caballo; los compañeros, por analogía) [verificado + captura]

```
jugador: acción de "llamar" (silbido)
  └─ frame event de cliente "CallMercenary" → 0x7FCC70 / 0x7FDD30 → emisor 0xBE3190
       C→S FrameEventCallMercenaryReq 2166, 29 B:
          +0x00 FrameEventAccessor 12 B (u8 tipo, u8 slot, u32 hash de la acción, u16, u32 índice)
          +0x0C vec3 posición de llamada
          +0x18 u32 (4 en todas las muestras)
          +0x1C u8  (1)
       servidor 0x2A32440: deserializa accessor (0x1F78F20) + 12 + 4 + 1 B,
                 valida el accessor contra la acción en curso (0x17EE020) → 0x2BA9950
  S→C CallVehicleMercenaryAck 2955, 100 B: +0 clave del caballo, +4 u32, +9 jugador, +0x15 vec3…
  C→S (la IA LOCAL del caballo, en el cliente del host):
       CurrentTargetChangeReq(caballo → jugador), UpdateAIDestinationReq,
       ChangeAIStateReq ×2, StartActionReq
```

- **La llamada no crea el actor**: el caballo ya estaba cargado desde el login y solo se le ordena
  venir (`caballo-coop.md` §0).
- Para un compañero que no esté en el mundo, el servidor tendría que mandar su `ActorLoadDataAck`
  (registro de tipo 4, dueño = usuario, índice = su ranura) tras el preámbulo. Lo sugieren
  `ProcessCallMercenaryOnceTimer` (2029, `0x2C33230`), `UpdateCallMercenarySpawnDurationAck` y
  `ProcessUnspawnMercenaryOnceTimer` [inferido; falta captura].
- Otros emisores del cliente, por si la llamada no es por frame event [verificado]:
  - `SelectMercenarySpawnReq` 3069: emisor `0xBE21B0`, llamado desde `0x9E1510`, junto al código
    del clan;
  - `CallHyosiByQuickSlotReq` 2919: emisor `0xBE2820`, llamado desde la UI `0xEFE0C0`.
  - "Hyosi" (효시, flecha de señal) es la llamada de mercenarios registrados desde la barra
    rápida (`UI_NpcStore_UnregistMercenaryToHyosi`, `UI_MercenaryQuickSlot_Call`).

---

## 2. Cambiar de personaje y "mercenario principal"

### 2.1 `ChangePlayerbleCharacterReq` 2369 / `ChangePlayerbleCharacterAck` 2473 [verificado]

- **Req, 12 B:** tres `u32` (`0xBF67C0`, llamado desde `0x5CDDE0`).
  - Se manda con `PostPacket(ClientNet, &res, 4, pkt, len, canal 0, flags 0x10)`, con el slot del
    actor emisor (`actor->vf(0x168)`).
  - El primer `u32` sale de un `u16` comparado con `[[actor+0x68]+0x20]+0x30`, la clave de
    personaje (`CharacterKey`) [inferido].
- **Servidor:** `0x2A4A180` → `0x29181B0` (717 instrucciones, sin analizar a fondo). Si falla,
  responde con op `0x3F5` (1013, el Nak genérico).
- **Ack, cliente `0xBFDFB0`, 4 B (`u32` clave de actor):**
  - busca el actor;
  - `TransformSync->vf(0x150)(0)`;
  - si el attacher de field está activo, `vf(0x40)` y `byte [[exe+0x6D69190]+0x60]+0x15D = 1`.
- El cambio en sí (qué actor pasa a ser "el local", `actor+0x96`, la marca de control) **no está
  en este handler**. Lo harán otros Ack que lleguen a la vez (carga del nuevo personaje,
  `MainPlayerData*`…). **Hay que capturarlo (§8, C3).**
- Lo que se sabe de las marcas (otros documentos):
  - `actor+0x96` = "personaje local". Lo leen 370 sitios, entre ellos `IsTargetable` (§3.3);
  - el modo de control `0x88E8E0` depende de `[[CC+0x60]+0x10]` y de `actor+0x90`
    (`movimiento-remoto-cliente.md` §1.1).

### 2.2 `ChangeMainMercenaryAck` 3062 no es "cambiar de personaje" [verificado]

- Formato: `u32 clave del jugador | u64 | u64 | u32 | u8`.
- Handler `0xBE8060`:
  - exige que el actor sea de tipo 1 (`[actor+0x88]+1 == 1`);
  - llama a `0x9E38B0(clan = [[actor+0x68]+0x110], &res, u64, u64, u32, u8)`.
- Cambia el **mercenario principal de una categoría** del clan (caballo, mascota o mercenario
  "por defecto") [inferido por el nombre y por `ChangedMainMercenary` y `cpp-main-mercenary`].
- No toca el control.

---

## 3. Cómo eligen objetivo los enemigos

### 3.1 La IA es del cliente [verificado + captura]

- Todos los actores del mundo tienen `actor+0x90` = usuario local y su IA corre en el cliente
  del host (`caballo-coop.md` §9.2).
- `CurrentTargetChangeReq`, `AggroRelationListReq` y `ChangeAggroRelationValidTickReq` son
  **C→S**: el cliente informa y el servidor guarda.
- Recuento de objetivos en las 38 capturas [captura]:

| Objetivo | Veces |
|---|---|
| `0xA0100001` (jugador) | 2980 |
| ninguno (0) | 576 |
| `0xA0101740` (jugador tras un relogin en 183530) | 456 |
| caballo `0xB0100003` | 49 |
| otros NPC `0xB01…` | pocos cada uno |
| **marioneta `0xA010F001`** | **0** |

  - Los NPC sí fijan a otros NPC (`0xB010322F → 0xB010313D`) y al caballo (`0xB01… → 0xB0100003`,
    motivos `0x13`, `0x0C`, `0x04`).
  - La mascota `0xB0100004` fija a muchos NPC.
  - Ningún enemigo fija a `0xB0100002` ni a `0xB0100004`: no hay combate con compañeros en las
    capturas.

### 3.2 La cadena hasta el servidor [verificado]

```
ClientTargetActorComponent   (vtable 0x55C4F28, en [[actor+0x68]+0x90]; misma posición en el servidor)
  rutinas de selección y mantenimiento: 0xA2BC60, 0xA2E8F0, 0xA2F770, 0xA31D30, 0xA36D00 (slot 48)
     └─ CommonTargetActorComponent::SetCurrentTarget 0x2217290(comp, int* res, u32 objetivo,
              u64* secuencia, bool forzar, u8 a, u8* motivo)
           • busca el objetivo en el gestor del realm ([exe+0x6D694D8] cliente / +0x6D694E0 servidor,
             ctx+0x10 → vf 0x28); si no lo encuentra, sigue con posición (0,0,0)
           • si !forzar y *secuencia < comp+0x170 → rechaza (0x6CF6B34)
           • si objetivo == comp+0x178 → nada
           • casos especiales: motivo 0x15; vehículo ([actor+0x68]+0x48 +0xEB/+0xC8)
           • comp+0x170 = *secuencia; comp+0x178 = objetivo; comp+0x100 = ahora
           • …→ vf slot 51 (+0x198) y slot 53 (+0x1A8)
  slot 53 = 0xA28270 → .xtls 0x9DAA0F0:
           si byte [[exe+0x6D69190]+0xD0]+0xCAB != 0 → nada
           si objetivo & 0x40000000 → nada
           si [comp.actor]+0x90 != [[[exe+0x6D69190]+0x30]+0x58]+0x60 (usuario local) → nada
           → thunk 0xB657D0 → .xtls 0xA3A4D30: CurrentTargetChangeReq (op 0xBA8) por PostPacket 0x35C670
```

- Formato de la Req (= el de la Ack 3040): `+0 u32 actor | +4 u32 objetivo | +8 u64 secuencia |
  +0x10 u8 | +0x11 u8 motivo`.
  - La secuencia crece por actor [captura]: valores 3, 7, 0x16, 0x1D… de `0xB0100004`.
- Servidor (`0x2980740`):
  - comprueba que el usuario de la sesión sea el dueño del actor (`vf170 == actor+0x90`);
  - llama al mismo `0x2217290` sobre `[[actor+0x68]+0x90]` del servidor;
  - **con un objetivo que no conoce no falla**: guarda la clave sin posición.

**Conclusión:** la marioneta pasaría el filtro del emisor (es una clave `0xA0…`, con el bit 30 a
0, y el dueño del enemigo es el usuario local). Que salga 0 veces significa que **ninguna rutina de
selección llama a `SetCurrentTarget` con ella**.

### 3.3 Validación común `IsTargetable` `0x22141e0(comp, objetivo, modo)` [verificado]

La llaman 57 sitios. Las rutinas del componente de objetivo del cliente la usan con modo 3
(p. ej. `0xA2C4DB`). Rechaza (`false`) si:

1. `(Status+0x390 | Status+0x394)` tiene el bit 22 (`Status = [[obj+0x68]+0x20]`);
2. la clave del objetivo tiene el bit 30 (`[obj+0x60] & 0x40000000`);
3. `byte Status+0x340 != 0`;
4. **`ClientTransformSync::vf(0x160)` = `0xA02B30`: hace falta `byte [yo+0xC2] != 0` y
   `byte [obj+0xC2] != 0`**;
5. `byte [TS propio +0x3DC] != 0`;
6. tipo 7 (gimmicks): banderas `[[obj+0x68]+0x30]+0x28D`;
7. modos 2, 4 y 5: `[[Status+0x3F8]+0x19] == 0`;
8. **tipo 1 con `byte [obj+0x96] != 0`** (el personaje local): rechaza si su acción en curso tiene
   cierta marca (`0x17EE020` → `[..+0x178]+0x30` → `0x4A2890` → `+0x40`). La marioneta, con
   `+0x96 = 0`, **se salta esta rama**;
9. `byte Status+0x276 != 0`, o `byte [TS obj +0x3DC] != 0`;
10. modo ≠ 1 y ≠ 3: `byte Status+0x398 != 0`;
11. `(Status+0x390 | +0x394) >= 0x80` (el mismo umbral que el minimapa usa para ocultar).

Después:
- modo 1 → acepta;
- si `0x1F5AE40(Sequencer [obj+0x68]+0xB0)` → acepta;
- **modo 3 → `0x1800B90(obj, 0x15, &mi_clave)`**, mutado en `.xtls 0xC955C20`:
  - **rechaza si `obj->vf(0x178)()`** (en el servidor esa vfunc es "controlado por usuario")
    [inferido para el cliente];
  - si no, `0x22156E0(0x15)`, y luego busca un actor relacionado (`0x17EF2E0`) y consulta su
    componente de objetivo (`0x22227B0(comp, &mi_clave)`).

Ninguna regla menciona `actor+0x90` ni el usuario dueño. **Las candidatas a fallar con la
marioneta son la 4 (`+0xC2`), la 11 (`Status+0x390/0x394`) y la `vf(0x178)` del modo 3**, pero el
jugador real tendría el mismo problema con esta última, así que el modo 3 no es la vía con la que
se le elige [inferido].

### 3.4 Lo que no está localizado

- **De dónde salen los candidatos.** Las rutinas grandes del componente de objetivo
  (`0xA31D30`, 0x159C B; `0xA2F770`, 0xDB0 B; `0xA36D00`) llaman a `IsTargetable` y a
  `SetCurrentTarget`.
  - La lista de aggro se sincroniza con `AggroRelationListReq`. Entradas de 16 B:
    `{u32 hash, u32, u32, u32 clave}`, p. ej. `DA AF FB B0 | 40 42 0F 00 | 40 42 0F 00 | 01 00 10 A0`.
  - La detección es `CommonDetectActorComponent` (`0x20B3F40`, `0x20B00A0`): consulta espacial
    con descriptor propio.
  - Ninguna de las dos se ha seguido hasta el punto de inserción.
- **Hipótesis principal [inferido]:** la marioneta nunca entra en la lista de aggro ni en la de
  detección de un enemigo. Cualquiera de estas causas basta:
  - el objeto de detección no la registra;
  - le falta el flag `+0xC2`;
  - su relación de facción, que el juego calcula con la tabla del **jugador local**, no le asigna
    "hostil": `0xA35B90`, slot 52, usa `[[jugador_local+0x68]+0x188]+0x18` cuando el usuario
    coincide.
- P1 (§7) lo decide sin escribir nada.

### 3.5 Por qué un compañero sí sería objetivo [inferido]

- Es un actor del usuario local (`actor+0x90 = 0x90100000`), con `ServerActor`, componentes
  completos (IA, facción, `Status`) y su acción e IA en marcha en el cliente del host.
- Para la detección y la facción es un aliado del jugador, y los enemigos lo agregan igual que a
  los NPC.
- La marioneta es un actor de tipo 1 de **otro usuario** sin IA ni estado de servidor. Nada la
  mete en las listas.

---

## 4. Palanca mínima: imponer el objetivo con `CurrentTargetChangedAck` (3040)

Handler del cliente `0xB68520` [verificado]:

```
lee: +0 u32 actor | +4 u32 objetivo | +8 u64 secuencia | +0x10 u8 forzar | +0x11 u8 motivo   (18 B)
actor = buscar(+0) ; comp = [[actor+0x68]+0x90]   (si es nulo, registra error y sale)
f = byte [[[actor+0x68]+0x40]+0x60]+0x10          (las banderas del modo de control del CC)
si (f & 2) y forzar == 0 → no hace nada            (bit 1 = "IA local / modo 3": los enemigos lo tienen [inferido])
si no → SetCurrentTarget 0x2217290(comp, &res, objetivo, &secuencia, false, forzar, &motivo)
```

- `SetCurrentTarget` **no llama a `IsTargetable`**. Busca la clave en el gestor del cliente, donde
  la marioneta existe, y guarda la clave y su posición.
- Después llama al slot 53 → sale un `CurrentTargetChangeReq(enemigo → 0xA010F001)` real hacia
  el servidor, que lo guarda sin quejarse (§3.2).

**Receta (para P2):**
- `secuencia` = `u64 [comp+0x170] + 1`, leído justo antes. Si se pasa un valor enorme, el enemigo
  rechazaría para siempre sus propios cambios.
- `forzar` = 1.
- `motivo`: copiar el de un `CurrentTargetChangeReq` reciente del mismo enemigo hacia el jugador.
  Nunca usar 0x15, que tiene comprobaciones extra.
- Inyectarlo por la misma vía S→C que ya usa el espejo.

**Riesgos:**
- La IA puede volver a elegir al jugador en el siguiente ciclo de selección: se vería otro
  `CurrentTargetChangeReq` con `0xA0100001` a los pocos cientos de ms.
- La IA puede no tener ruta de ataque para un objetivo sin `ServerActor`.
- Si la puntería funciona pero no aguanta, las alternativas son:
  - repetir la inyección mientras la marioneta esté a menos de X m y "provoque";
  - un **mid-hook de solo filtro** en la entrada de `SetCurrentTarget` `0x2217290`: para los
    enemigos marcados, cambiar el objetivo "jugador" por la marioneta cuando toque (según
    distancia o reparto).

  Es un hook al inicio de una función, permitido por las reglas del proyecto (`CONTRIBUTING.md`).

---

## 5. Del golpe del enemigo a la marioneta al invitado

### 5.1 Cómo se ve un golpe de enemigo hoy [captura]

Captura 112627: `B01006DF → A0100001` (t = 506,8 s) y `B0100691` (t = 487,6 s), con
`build/research/merc/enemyhits.py`.

```
C>S DoFrameEventReq 2380 (1307 B): +0x01 MoveData del ENEMIGO; accessor +0x1F7 (p. ej. 00 00 0864696c 0000 02000000);
                                    1 entrada de objetivo (665 B): +0 0xA0100001, MoveData del jugador, punto de impacto, hueso
S>C PostProcessAttackResult_AttackTargetAck (A0100001), ApplyActionRestrictionAck (A0100001),
    VaryHPAck (A0100001), OnPostBuffDamagedAck (A0100001, daño×1000), AttackerAck (B01006DF)
```

- Lo emite el cliente del host porque la IA del enemigo es suya.
- Hay otra variante con el accessor de tipo 1 (`01 77 01 10 B0 …`). Una muestra no tuvo respuesta:
  golpe bloqueado o esquivado [inferido].
- `AttackByCollisionReq` (2815): `+0 víctima | +4 atacante | +8 u32 | +0xC vec3 | +0x18 vec3 |
  +0x24 hash | +0x28 u32`. Solo aparece 6 veces en 105149 (colisiones físicas).

### 5.2 Con la marioneta como objetivo [inferido]

- La entrada de objetivo llevaría `0xA010F001` y el MoveData que el cliente del host tiene de la
  marioneta.
- El servidor pasa las comprobaciones de forma (clave ≠ 0, bit 30 a 0, en orden creciente), no
  encuentra el actor y no genera daño. Es inofensivo, pero conviene comprobarlo.
- **Interceptarlo en el mid-hook de `PostPacket`** (`troc_sniffer.cpp`): si una entrada de objetivo
  es la clave de la marioneta, mandar al invitado
  `{tipo de enemigo / clave traducida, accessor (hash de acción + índice), punto de impacto,
  hueso, dirección}`.
- Se puede tirar la entrada o reenviar el paquete sin cambios. Primero, reenviarlo y observar.
- Si el golpe incluye al jugador y a la marioneta a la vez, hay **dos** entradas ordenadas. No
  reordenar.

### 5.3 Aplicarlo en el invitado (su servidor no conoce al enemigo del host)

| Opción | Qué hace | Estado |
|---|---|---|
| Daño | `VaryStatByAccInputReq` (HP `0x000F4240`, −daño×1000) sobre su propio jugador. El daño sale de una tabla `hash de acción del accessor → daño` aprendida de los `OnPostBuffDamagedAck` del host cuando ese ataque le da al host | Mecánicamente seguro (`combate-coop.md` §4.3, c2). Muerte, clamp y reacción, por probar |
| Reacción | Inyectar en su cliente `AttackTargetAck` / `ApplyActionRestrictionAck` (objetivo = su jugador, atacante = la copia del enemigo si existe) | Solo visual (c4) |
| Nativo con hash | `AttackByCollisionReq` (víctima = su jugador, atacante 0, hash) | T3 de `combate-coop.md`. Daño "por colisión", no el del ataque |

---

## 6. Evaluación de la idea original: "habitar" un compañero

### 6.1 (a) Invocarlo de forma nativa [inferido]

- Hay que reproducir la acción de llamada (el accessor de `FrameEventCallMercenaryReq` exige que
  la acción en curso del jugador del host sea la de "llamar"), o bien `SelectMercenarySpawnReq` o
  `CallHyosiByQuickSlotReq`, cuyos formatos faltan.
- Inyectar la Req con `PostPacket` sin la acción da un rechazo silencioso o un Nak.
- **Lo barato es que el jugador lo llame a mano** y el mod lo detecte por su carga (tipo 4,
  dueño = usuario).

### 6.2 (b) Pararle la IA y moverlo con los datos del invitado

- **Lo bueno:** el host es su dueño, así que su cliente puede mandar en su nombre `MoveActorReq`,
  `StartActionReq` y `DoFrameEventReq`. El servidor los valida de forma nativa (dueño ==
  `actor+0x90`) y el daño sale de las estadísticas del compañero.
  - Los enemigos lo fijan por la vía normal: es aliado del jugador local.
- **Pasos:**
  1. Quitar el bit 1 de `[[CC+0x60]+0x10]`. El modo pasa de 3 a 2 por la regla 3 de
     `movimiento-remoto-cliente.md` §1.1 (`actor+0x90 != 0`), igual que la marioneta.
  2. Mover el compañero en el cliente con Move/Rotate/StartAction Ack inyectados.
  3. **Además**, mandar al servidor `MoveActorReq` y `StartActionReq` con su clave, sacados de los
     del invitado, para que el servidor sepa dónde está y qué acción tiene. Sin esto, los golpes
     (`DoFrameEventReq`) se descartan, porque el servidor exige la acción en curso y el hash del
     accessor (`combate-coop.md` §1.4).
  4. **Suprimir** en `PostPacket` las Req que su IA siga generando (`ChangeAIStateReq`,
     `UpdateAIDestinationReq`, `CurrentTargetChangeReq` del compañero). El componente de IA
     sigue vivo mientras `actor+0x90` sea el usuario local (`0xB681F0`, `0x9DAA0F0`).
- **Lo caro:** traducir las acciones del jugador invitado a acciones válidas para **ese personaje**.
  - Un compañero tiene otro gráfico de acciones. Los hashes de acción de Kliff no existen en el
    de, por ejemplo, Oongka.
  - `StartActionReq` con un hash que el actor no tiene se rechaza, y el cliente podría caer en
    "capa con gráfico sin acción", el cierre del caballo (`INFORME-CABALLO-CRASH.md` §11).
  - Solo es viable si el compañero es otro "Kliff", lo cual no existe, o si se acepta que el
    cuerpo de servidor haga sus propias acciones.

### 6.3 (c) Que parezca el invitado

- Cambiar la clave de personaje o la apariencia de un actor ya cargado no tiene vía conocida.
  `UpdateMercenaryCharacterCustomizationAck` y `ChangeMercenaryDecoEquipmentAck` cambian
  personalización y equipo dentro de su propio modelo.
- **Alternativa [inferido]:** compañero invisible como "cuerpo de servidor" y la marioneta
  dibujada en su posición.
  - Hay que ocultar su malla (sin función localizada) y mantener los dos actores alineados.

### 6.4 Veredicto

**Plan B.** Resuelve "los enemigos lo atacan" y "el daño es nativo", pero:
- obliga a traducir acciones entre personajes distintos;
- obliga a ocultar un actor;
- toca la persistencia del clan (§6.5).

El objetivo forzado (§4) más el relevo de golpes (§5) consigue lo primero con un solo mensaje
nativo y sin tocar el guardado.

### 6.5 Riesgos

- **Guardado:** el clan (`ClientMercenaryClanActorComponent`, `[[jugador+0x68]+0x110]`) es
  persistente. `UpdateMercenaryClanRepeatTimer` (2014) es un temporizador del servidor que lo
  actualiza.
  - Si el compañero muere: `MercenaryDeadAck`, enfriamientos (`UpdateCallMercenaryCoolTimeAck`),
    reanimaciones (`QuickReviveMercenaryReq`).
  - Si está fuera de sitio cuando se guarda: posición guardada (`UpdateMercenaryPositionAck`
    escribe `entrada+0xC8…`).
  - Todo eso **acaba en la partida del host**. Copia de `save` en `backups/` antes de cada prueba
    que escriba.
- **Misiones:** algunas usan compañeros (`ChangeSummoneeToIndependentByInitTriggerReq`, actores de
  stage). No probar con misiones de compañero activas.
- **Cierres:** cambiar el modo de control de un actor con IA puede dejar la capa principal sin
  acción. Mantener la contención de `INFORME-CABALLO-CRASH.md` §11 (saltar `0x85FBA0`/`0x860D30`
  mientras la capa 0 tenga gráfico sin acción) para cualquier actor cuyo modo se cambie.
- **Objetivo forzado (§4):** el riesgo es bajo; es un Ack nativo con comprobaciones de nulo en el
  handler. Lo peor previsible es que el enemigo se quede "mirando" a un objetivo sin `ServerActor`,
  o una desincronización de la secuencia (`comp+0x170`) si se inyecta mal.

---

## 7. Plan de experimentos (de lo más barato y seguro a lo más caro)

| # | Qué | Escribe | Cómo se mide | Decide |
|---|---|---|---|---|
| **P0** | Volcado de campos de la marioneta frente al jugador real, cerca de un enemigo: `+0x60`, `+0x88→+1`, `+0x90`, `+0x94`, `+0x96`, **`+0xC2`**, `+0xCF`, `+0x5E`; `Status=[[a+0x68]+0x20]`: `+0x276`, `+0x340`, `+0x390`, `+0x394`, `+0x398`; `TS=[[a+0x68]+0x1A0]+0x3DC`; `[[a+0x68]+0x90]` (componente de objetivo) y sus `+0x170`/`+0x178`; byte de banderas `[[[a+0x68]+0x40]+0x60]+0x10` | No | Log del probe | Qué reglas del §3.3 fallarían |
| **P1** | Hook inline **al inicio** de `IsTargetable` `0x22141e0` (safetyhook). Registrar `(clave de comp.actor, clave de rdx, r8b, retorno)` solo cuando `rdx` sea la marioneta o el jugador, con límite por segundo. Opcional: lo mismo en `SetCurrentTarget` `0x2217290` (`r8d` = objetivo) | No | `cdmp.log` | Si la marioneta **nunca** aparece → no es candidata (aggro o detección). Si aparece con `false` → qué regla falla (con P0) |
| **P2** | Con copia de `save`: marioneta sola junto a **un** enemigo y el host lejos. Inyectar `CurrentTargetChangedAck {enemigo, 0xA010F001, [comp+0x170]+1, 1, motivo copiado}` | Sí (un Ack) | Sniffer: `CurrentTargetChangeReq(enemigo→A010F001)`; si después vuelve a `A0100001` y cuándo; `StartActionReq` del enemigo hacia la marioneta; **`DoFrameEventReq` con objetivo `0xA010F001`**; ningún Nak | Si la IA acepta el objetivo y ataca |
| P3 | Si P2 se pierde en el siguiente ciclo: repetirlo cada 0,5 s, o el mid-hook de §4 en `SetCurrentTarget` | Sí | Ídem | Si la puntería se puede mantener |
| P4 | Relevo: en el hook de `PostPacket`, copiar las entradas de objetivo de la marioneta y mandarlas al invitado; él aplica `VaryStatByAccInputReq` con la tabla de daño | Sí (en el invitado) | HP del invitado; sin desincronización con la regeneración | Combate básico enemigo → invitado |
| P5 | Llevar el resultado de P1 a la causa (añadir la marioneta al aggro o arreglar `+0xC2`/facción) para que el enemigo la elija solo | Sí | Como P2, sin inyectar | Solución "nativa" sin forzar |
| P6 | (Plan B) Compañero: con un compañero llamado a mano, quitar el bit 1 de `[[CC+0x60]+0x10]` y moverlo con Ack, con la contención del caballo activa | Sí | Sin cierre; se queda quieto; su IA deja de mandar Req | Si la vía del §6.2 es viable |

---

## 8. Qué hay que capturar en vivo

Con el sniffer activo y `cdmp_peer`, un solo PC. Anotar la hora de cada paso.

- **C1. Llamar a un compañero de combate (mercenario de tipo 4)** desde la barra rápida o el
  silbido, quieto y lejos de enemigos. Esperar 10 s. Da la Req de llamada (¿frame event,
  `SelectMercenarySpawn` o `CallHyosiByQuickSlot`?), el `ActorLoadDataAck` (¿tipo 4, dueño,
  índice?) y el preámbulo.
- **C2. Pelear con él al lado** contra 2 o 3 enemigos durante 30 s. Da
  `CurrentTargetChangeReq(enemigo → compañero)`, `AggroRelationListReq` con su clave y
  `DoFrameEventReq` enemigo → compañero y compañero → enemigo.
- **C3. Cambiar de personaje** (si el juego lo permite en esa partida), esperar 5 s y volver. Da
  `ChangePlayerbleCharacterReq/Ack` con todo lo que llega alrededor (qué actor pasa a ser el
  local).
- **C4. Despedir al compañero** (`UnSetMainMercenaryAndUnSpawn`, `RemoveActorAck`…).
- **C5. Lo mismo con la mascota** (`isSummonPet`, `UI_Pet_Summon`) para confirmar que
  `0xB0100004` es la mascota.
- **C6. Cambiar de mercenario principal** en el menú, para ver `ChangeMainMercenaryAck`.
- **C7. Probe P0 durante C2**: el componente de objetivo de un enemigo cuando apunta al compañero
  (`+0x178` = clave del compañero) frente a cuando apunta al jugador.

---

## 9. Direcciones y scripts (build 2976)

| RVA | Qué es |
|---|---|
| `0x22141E0` | `CommonTargetActorComponent::IsTargetable(comp, objetivo, modo)` (§3.3) |
| `0x2217290` | `CommonTargetActorComponent::SetCurrentTarget(comp, res*, objetivo, u64* sec, bool, u8, u8* motivo)`; `comp+0x170` secuencia, `+0x178` objetivo |
| `0x55C4F28` | vtable de `ClientTargetActorComponent`: slot 44 `0xA27C00` (aggro → `AggroRelationListReq` y `ChangeAggroRelationValidTickReq`), 52 `0xA35B90` (relación con la tabla del jugador local), 53 `0xA28270` (informar del objetivo) |
| `0x9DAA0F0` (`.xtls`) | emisor lógico de `CurrentTargetChangeReq` (filtros: usuario local, bit 30) → `0xB657D0` → `.xtls 0xA3A4D30` |
| `0xA2BC60`, `0xA2E8F0`, `0xA2F770`, `0xA31D30`, `0xA36D00` | rutinas de selección y validación del objetivo (cliente) |
| `0xB68520` | `CurrentTargetChangedAck` (3040), handler del cliente (§4) |
| `0x2980740` | `CurrentTargetChangeReq`, handler del servidor |
| `0x1800B90` → `.xtls 0xC955C20` | comprobación de relación del modo 3 (`vf178`, `0x22156E0`, `0x22227B0`) |
| `0xA02B30` | `ClientTransformSync::vf(0x160)`: `[a+0xC2] && [b+0xC2]` |
| `0x55BC3E8` / `0x5910000` | vtables de `ClientDetectActorComponent` / `CommonDetectActorComponent` (`0x20B3F40` recogida, `0x20B00A0` descriptor) |
| `0x2A32440` → `0x2BA9950` | `FrameEventCallMercenaryReq`, servidor |
| `0xBE3190` | emisor de `FrameEventCallMercenaryReq` (llamado desde `0x7FCC70` y `0x7FDD30`) |
| `0xBE21B0` / `0xBE2820` | emisores de `SelectMercenarySpawnReq` / `CallHyosiByQuickSlotReq` |
| `0xBF67C0` ← `0x5CDDE0` | emisor de `ChangePlayerbleCharacterReq` |
| `0x2A4A180` → `0x29181B0` | `ChangePlayerbleCharacterReq`, servidor |
| `0xBFDFB0` | `ChangePlayerbleCharacterAck`, cliente |
| `0xBE8060` → `0x9E38B0` | `ChangeMainMercenaryAck` → clan |
| `0xB681F0` | `ChangeAiControlAck` (cambia el gráfico de IA solo si `actor+0x90` = usuario local) |
| `0x8AB190` | instalador de componentes del actor de **usuario** (clan, `SelfFaction`, `SelfFriendly`…) |

Scripts nuevos en `build/research/merc/` (se ejecutan con `build/research/venv`):

| Script | Qué hace |
|---|---|
| `vtdump.py <vt>` | lista las ranuras de una vtable |
| `qref.py <rva>` | busca punteros absolutos a una función en las vtables |
| `scanfx.py <lo> <hi> <regex>` | busca instrucciones en las funciones de `.pdata` |
| `ctxwin.py` | contexto antes de cada sitio de llamada |
| `strva.py` | RVA de una cadena |
| `enemyhits.py <captura.bin>` | `DoFrameEventReq` de enemigos con objetivo jugador y sus Ack |

Los `*.asm` son los volcados citados.
