# Fase 5.1: el invitado ve los enemigos del anfitrión dentro de 150 m, y los suyos fuera

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`) y
solo valen para esta build. Investigación de solo lectura (6 de octubre de 2026):
- capstone sobre el exe en disco (`cddis.py`, `lin.py`);
- **83 capturas**: `bin64\cdmp_packets_*`, `D:\cdmp-grabaciones` y, por primera vez, las **dos capturas
  del invitado real** (Shadow) (`20261005-202422`, `20261006-055013`,
  con el relevo de enemigos apagado: el invitado veía solo a sus propios enemigos y a la copia del
  anfitrión);
- los logs del mod de las pruebas de copias (`captures\cdmp_enemy_*.log`, `cdmp_sanitized_test.log`…);
- el código actual (`enemy_relay.cpp`, `troc_sniffer.cpp`, `remote_puppet.cpp`, `coop.cpp`, `keys.h`,
  protocolo 8).

No se ejecutó el juego ni se tocó `src/`. Este informe **parte de**
[pendiente-3g-suprimir-enemigos-invitado.md](pendiente-3g-suprimir-enemigos-invitado.md) (4 oct), que
sigue siendo válido, y añade:
- la regla concreta de diseño (150 m alrededor del anfitrión, y **que vuelvan** los enemigos propios
  al salir);
- datos de dos PC reales;
- la **vuelta** de los enemigos propios (reinyección de las cargas guardadas) con evidencia nueva;
- la evaluación con datos de las cuatro vías posibles;
- un plan de implementación sobre el código de hoy (roles estrictos, protocolo 8).

Marcas: **[verificado]** (desensamblado o datos estáticos), **[captura]** (medido; se da el script),
**(hipótesis)** (deducción; la §6 dice con qué prueba se confirma).

Notación: **H** anfitrión, **G** invitado, **nativo** = enemigo del propio mundo de G, **copia** =
copia en G de un enemigo de H (`0xB01F0000+n`), **zona** = disco de 150 m alrededor de H.

Scripts nuevos en `build/research/p_f5/` (§9); los del 4 oct siguen en `build/research/p_3g/`.

---

## 0. Resumen

**Recomendación: "descartar y guardar" en el cliente de G, y "reinyectar" al salir.**
1. Mientras G esté a ≤ 150 m de H (se apaga a > 170 m), en G:
   - las **cargas nuevas** (`ActorLoadDataAck` 2005) de nativos hostiles dentro de la zona **no se
     entregan** al cliente (el mid-hook del sniffer en `ClientNet::OnRecvPacket` `0x35CA00` salta la
     función con un `ret`) y **se guardan**;
   - los nativos hostiles **ya cargados** (y en reposo) se retiran con un `RemoveActorAck` (2516)
     inyectado, y también se guarda su carga.
   - H envía copias de **todos** los enemigos que cumplen el mismo predicado dentro de la misma zona.
2. Cuando la zona se apaga (G se aleja, se cae el enlace, termina el co-op), se **reinyectan** las
   cargas guardadas de los nativos que el servidor de G sigue teniendo vivos (grupo entero, en el
   orden en que llegaron, con la posición actual en los que se retiraron). Las copias de H se quitan.
3. **El servidor de G nunca se toca.** Ni se le envía nada ni se le borra nada: para él, esos nativos
   están vivos y quietos en su sitio. Por eso no hay botín, ni muertes, ni reapariciones, ni cambios
   en misiones o en el guardado.

Por qué esta vía y no las otras (§2):

| Vía | Veredicto | Motivo principal |
|---|---|---|
| (a) descartar / `RemoveActorAck` + reinyectar | **Sí** | Solo presentación en el cliente; las dos operaciones (saltar un paquete, inyectar carga o baja) ya se usan a diario |
| (a') que el servidor los reenvíe | No existe | No hay ninguna Req que pida reenviar un actor vivo: el servidor **nunca** reenvía una clave viva (0 de 2.605) y solo recarga con claves nuevas tras despawn a 200-225 m o pantalla de carga |
| (b) ocultar en memoria (alfa, IA, colisión) | No | Ocultos seguían dando y recibiendo golpes (`copias-grupo-invisibles.md` §2.4); escrituras en actores vivos = riesgo de cierre (que se reporta a Pearl Abyss) |
| (c) impedir que aparezcan (pools, Req de spawn) | No | Los enemigos los crea el servidor por horarios y etapas de facción, no por Req del cliente (solo 21 de 831 cargas de NPC llevan un id de pool); bloquear pools se lleva objetos con estado guardado |
| (d) matarlos o "deslogarlos" en el servidor | No | Irreversible: botín, contadores, reaparición a 30-120 s, telemetría de truco |

Datos nuevos que lo respaldan:
- **Dos PC reales** [captura, `c7_shadow_zone.py`]: G estuvo a ≤ 150 m de H el **98-99 %** del tiempo
  (1.549 de 1.583 s y 618 de 626 s), con solo 3 cambios de zona por sesión. Con la regla, G habría
  descartado 43 + 17 cargas y retirado 23 nativos ya cargados; ninguno peleaba con G en ese momento.
  De esos 83 nativos ocultos, el servidor retiró **56 por su cuenta** durante la zona (30 con motivo
  25, 24 con motivo 80, 2 con motivo 19); al apagarse la zona solo **6** seguían vivos y habría que
  devolverlos; los otros 21 seguían ocultos al terminar la captura.
- **Reinyectar cargas viejas funciona** [captura + logs, `c4_loadage.py`]: las 196 copias de las
  pruebas de eco se crearon con la carga original **de 0,4 a 198 s de antigüedad** (mediana 44 s) y,
  desde el saneado de grupos, **todas** terminaron el login (`+CF 1`; las 46 que no lo hicieron eran
  las de la prueba de los "invisibles", antes del saneado).
- **Retirar un enemigo en plena pelea tiene precedente** [captura, `c8`/`c9`]: el servidor retiró 46
  NPC vivos menos de 30 s después de que golpearan al jugador (41 con motivo 28, siempre tras
  `ClearAllyGroupDataAck`); el cliente solo responde con `ChangeAggroRelationValidTickReq`. Aun así,
  por defecto **no se retira a los que pelean con G** (§4.4).

Confianza:
- **descarte + barrido: alta** (mecanismos verificados; precedentes medidos);
- **vuelta por reinyección: media** (evidencia análoga fuerte, pero con la clave original y tras
  minutos sin control: prueba L2);
- **efecto en el servidor de un nativo sin controlador durante mucho tiempo: desconocido** (0
  precedentes: el cliente maneja el 99,9 % de las cargas de NPC en segundos; prueba L1). No se
  escribe nada, así que el peor caso esperado es que el servidor los retire o los mueva él.

---

## 1. Cómo llegan y se van los enemigos (lo que importa para suprimir y devolver)

### 1.1 Ciclo de vida [captura; `p_3g/s1`, `s3`, `s10`; `p_f5/c5`]

| Paso | Qué pasa | Dato |
|---|---|---|
| Alta | S>C `ActorLoadDataAck` de **un solo registro** (tipo 3 persona, 6 animal) | Cada alta trae **clave nueva**: 0 recargas de una clave viva en 2.605 |
| Control | El cliente corre la IA: lo primero que manda es `RegionInOutReq` (8 B) a los ~0,27 s, luego `MoveActorReq`, `StartActionReq`, `ChangeAIStateReq`… | **9.778 de 9.789** cargas de NPC (tipos 3 y 6, sin dueño) recibieron Req del cliente; de las 11 restantes, 8 eran nuestras copias y 3 llegaron en los últimos 3 s de la captura (`c5`, 5 capturas) |
| Baja | S>C `RemoveActorAck` `04 │ u32 0 │ u8 motivo │ n × u32 clave` | Motivos de NPC vivos: 25 (distancia, pico a **200-225 m**), 19 (pantalla de carga, todo), 28/80/42/30/36 (servidor, sin causa clara) |
| Después de la baja | El servidor sigue mandando `UnapplyBuffAck`, `UpdateElementalAck`… sobre la clave | El cliente los ignora: `0x8A67E0` no la encuentra y el handler la salta (`0xB62FD4`) [verificado] |

Consecuencias:
- Si G no entrega la carga, **el servidor de G no recibe nada** de ese NPC: ni regiones, ni IA, ni
  movimiento, ni golpes (`DoFrameEventReq` lo emite el cliente). No puede hacer daño a G. En el
  servidor queda vivo y quieto en su punto de carga (hipótesis sobre "quieto": la IA de los NPC
  cercanos corre en el cliente, `bdo-emuladores-monstruos.md` §4).
- **No hay precedente** de un NPC cargado que el cliente nunca maneje durante minutos. Lo que haga el
  servidor con él (temporizadores, reubicación) solo lo dirá la prueba L1 (pregunta P1).

### 1.2 Las funciones que se usan [verificado]

| RVA | Qué es | Por qué importa |
|---|---|---|
| `0x35CA00` | `ClientNet::OnRecvPacket(ClientNet*, NwSession*, ?, const u8* pkt, u16 size)`; el sniffer tiene un mid-hook en la entrada (`kClientOnRecvPacket`) | **Los 4 llamadores ignoran `rax`**: `0x35BB58` y `0x35C0F2` (informe 3g), `0x35C956` (sobrescribe `eax` y quita el paquete de su búfer `+0x8008` igual), `0x86E55AF` (no lee `rax`). Saltar la función con un `ret` en la entrada es limpio. Una llamada = un mensaje Troc |
| `0xB62C70` | Handler cliente de `RemoveActorAck` | Admite varias claves; salta las desconocidas; para cada actor `0x2068340` (baja local, sin red); retira también el enlazado `vf(0x160)` (montura) con motivo 6 |
| `0x2068340` → `.xtls 0xE0D9AF0` | Baja local | Si ya está saliendo (`+5E` `0x20`/`0x40`) no hace nada: una baja inyectada dos veces es inocua |
| `0x8A67E0` | Busca actor por clave en `[[0x6D69188]+0x38]` | La usa también `game::find_actor` |

### 1.3 Lo que hace el cliente cuando quitamos un NPC [captura, `c9_r28.py`]

En las 46 bajas naturales de NPC que **acababan de pelear** con el jugador:
- C>S en los 3 s siguientes: `ChangeAggroRelationValidTickReq` (46 de 46) y, a veces,
  `ChangeBattleStateReq` del propio NPC (14). El jugador no cambia de estado de combate en esos 3 s
  (45 de 46).
- Armas "on demand" (gimmicks de tipo 7 cuya carga nombra al NPC): 7 de 46 tenían una. Nunca se
  retiran en el mismo mensaje; en 3 se retiran en < 3 s (motivo 45 o 9), en 4 no.

En un barrido nuestro, el cliente manda lo mismo que en una baja natural (`RegionInOutReq` de salida y
`ChangeAggroRelationValidTickReq`) (hipótesis basada en `p_3g/s1`). El servidor no interpreta eso
como una baja del NPC: lo sigue teniendo.

---

## 2. Las cuatro vías, con evidencia

### 2.1 (a) Descartar / retirar en el cliente y devolver después — **recomendada**

**Qué cree el servidor de G:** que su cliente tiene esos NPC cargados. No recibe Req de ellos (los
descartados) o recibe la salida de región y la limpieza de aggro (los retirados). Sigue mandando
mensajes sobre sus claves, que el cliente ignora (§1.1).

**Misiones, guardado, reapariciones:** nada cambia en el servidor. No hay muertes, así que no arranca
ningún temporizador de reaparición (`p_3g/s10`: 20 de 117 muertes reaparecieron, casi siempre en
< 2 min) ni cambia el estado del campamento. Un guardado de G durante la zona guarda el mismo mundo
que sin el mod (hipótesis: los NPC de horario y de facción no se guardan uno a uno; lo que se guarda
es el estado de campamentos, nodos y misiones, que no cambia).

**Cómo vuelven.** Hay tres casos:

| Caso | Qué pasa | Fuente |
|---|---|---|
| El servidor ya los retiró (G se alejó > 200-225 m, pantalla de carga, motivos 28/80) | Al volver G, llegan **claves nuevas** y el filtro decide otra vez. No hay nada que devolver | [captura] En Shadow: de 83 nativos ocultos, 56 los retiró el propio servidor durante la zona |
| Siguen vivos en el servidor al apagarse la zona | **Reinyectar** su carga guardada con su clave original (§5.5) | (hipótesis, prueba L2) |
| La reinyección falla (no termina el login, error del juego) | Se retiran del cliente (`RemoveActorAck`) y vuelven en la siguiente recarga natural (alejarse ~250 m o una pantalla de carga) | Plan de reserva |

**¿Se le puede pedir al servidor que los reenvíe?** No se ha encontrado ninguna forma [captura,
`c1_opcount.py`, 83 capturas]:
- `ResyncSyncSceneObjectDataForReloadReq` (2104, 40 veces): es de objetos de escena (gimmicks), no de
  actores;
- el servidor solo crea altas nuevas: por horario o etapa de facción (`CreateNPCScheduleStageAck`,
  `CreateFactionSpawnStageAck`), al reentrar en rango, o tras una pantalla de carga;
- forzar eso (teletransportes, pantallas de carga) sería mucho más invasivo que reinyectar.

**Por qué la reinyección debería funcionar:**
- es **la misma entrega, tarde**: el paquete es el que mandó el servidor, con su clave, que el
  servidor sigue considerando cargada;
- un nativo descartado no ha hecho nada desde la carga (ni IA, ni movimiento, ni golpes), así que su
  estado en el servidor es casi el de la carga;
- crear un actor con una carga de hasta **198 s de antigüedad ya funciona**: así se hacen todas las
  copias de enemigos (`c4_loadage.py`: 196 casos, mediana 44 s, p90 125 s; login 100 % tras el
  saneado). Diferencias con un nativo: la copia va en modo 2 (por red) y con clave nueva; el nativo
  devuelto va en modo 1 (IA en el cliente) y con la clave que el servidor conoce.

**Riesgos de la reinyección** (hipótesis; prueba L2):
- cargas de varios minutos: campos de tiempo del registro (CC, buffs) caducados. Lo medido llega a
  198 s;
- mensajes de estado del servidor perdidos mientras estuvo oculto (§5.5 los guarda y los reentrega):
  en la simulación de Shadow, sobre los nativos "suprimidos" llegaron sobre todo `UpdateElementalAck`
  (periódico), `ApplyBuffAck`/`UnapplyBuffAck`, `MoveForceAck` + `ChangeMoveSessionIDAck` y
  `VaryHPAck` [captura, `c7b_shadow_about.py`]. Los dos de movimiento solo aparecen con empujones en
  combate, que un nativo oculto no tiene;
- grupos: un líder sin un hijo se queda en estado 8 para siempre y un miembro sin padre se destruye
  (`copias-grupo-invisibles.md` §2.2). Se devuelve **el grupo entero o nada**.

### 2.2 (b) Ocultarlos en el cliente (render, IA, colisión)

- **Ocultar:** `Status+0x390` bit 2 pone el modelo en alfa 0 (`copias-grupo-invisibles.md` §2.3). Pero
  los "originales invisibles" de las pruebas **seguían recibiendo y dando golpes** (§2.4 de ese
  informe): oculto no es intangible.
- **Sin IA:** el modo lo decide el usuario de control (`load+0x14` = `actor+0x90`; `0x4FA530`,
  `0x88E8E0`). Reescribirlo en la carga deja al NPC en modo 2 (por red, quieto), pero **visible y con
  colisión**.
- **Sin colisión:** habría que escribir en componentes de física de un actor vivo, sin dirección
  conocida.
- **Servidor:** igual que (a), pero los golpes de G al aire acabarían en el nativo y en su servidor.

Veredicto: no. Tres escrituras en actores vivos, cada una con riesgo de cierre, para un resultado peor
que no tener el actor.

### 2.3 (c) Impedir que aparezcan

[captura, `c1_opcount.py` (83 capturas), `c2`/`c3` (114648); `enemigos-del-anfitrion.md` §4.1]

| Req (C>S) | Veces | Formato | ¿Crea enemigos? |
|---|---|---|---|
| `ActivateSpawningPoolReq` 2424 | 29.491 | `04 │ n × 54 B` (id de escena de 16 B repetido, `FF×8`, ceros) | Casi siempre objetos de tipo 7: **21 de 831** cargas de NPC llevan un id de pool activado |
| `DeactivatePoolSpawnReq` 2119 | 32.909 | igual | Retiradas de objetos (motivos 57/67) |
| `AutoSpawnRoadReq` 2550 | 69.237 | `04 │ n × 24 B` | NPC y carros de camino |
| `SceneLevelOnDemandGimmickSpawnReq` 2928 | 28.572 | 25 B | Objetos |
| `MeshGroupAutoSpawnReq` 2809 / `TerrainRegionVoxelSpawnReq` 2327 | 2.847 / 1.591 | 71 / 38 B | Fauna |
| `RegisterSpawnDistanceByExternalReq` 2817 | **0** | `u8 │ u32 │ n × 16 B` (deserializador `0x29C90D0`) | Desconocido; solo se ve `Reset…` 3102 (694 veces, 16 B) |
| `LogoutAutoSpawnActorForDebugReq` 3031, `DestroyAllFieldCheatReq` 2540 | 0 | sin datos | De depuración, destructivas en el servidor |

- Los enemigos de campamento y de camino salen de horarios (`cd_mNN_mNN_monster_*`) y nodos de
  facción que **decide el servidor** (`p_3g/s8`: tras las cargas hostiles llegan
  `CreateFactionSpawnStageAck`/`CreateNPCScheduleStageAck` S>C, sin una Req que las cause).
- Bloquear las Req del cliente no quita enemigos y se lleva objetos con estado guardado
  (`SyncFieldGimmickSavedByGimmickStateAck`); las `Deactivate*` de pools nunca activadas pueden dar Nak.

Veredicto: no.

### 2.4 (d) Matarlos o retirarlos en el servidor

- `KillCheatReq` (2279, `u32 clave │ u32 opción`, deserializador `0x29BF730`) o `VaryStatCheatReq`
  (2703): el servidor los mata. Consecuencias medidas: botín en el suelo, buff 20001, contadores,
  campamento "limpio" y reaparición a los 30-120 s con el jugador a 50-100 m (`p_3g/s10`). La muerte por
  truco no da progreso y queda marcada (`pendiente-recompensas-progreso.md`).
- `AiControlChangeCheatReq` (2442): cambiaría quién controla la IA en el servidor; el NPC seguiría
  visible.
- Las Req de depuración que "deslogan" actores (3031, 2540) nunca se han visto en una captura: su
  efecto en el guardado es desconocido.

Veredicto: no como supresión. `KillCheatReq` sigue teniendo sentido **después**, como consecuencia de
una muerte real en H (fase 5.2: remate del gemelo oculto para que G tenga su botín).

---

## 3. Qué es un "enemigo hostil" de G

### 3.1 Lo que ya sigue `enemy_relay` [verificado en el código]

| Estructura | Qué guarda | Notas |
|---|---|---|
| `g_loads` | carga (payload) de cada NPC de tipo 3 o 6, un registro, clave de juego (`keys::is_game_world_key`), dueño 0 | Se vacía entera al pasar de 1.500 (junto con `g_actions`, `g_engaged`, `g_at_us`): no sirve como almacén para devolver cargas |
| `g_known` | clave → tipo de registro de **todo** lo cargado | Para los padres (tipo 5 montura, 7 torre o carro) |
| `g_engaged` | último momento "en combate": `ChangeBattleStateReq ≠ 0`, golpe al jugador o a la marioneta, golpe del jugador | 20 s |
| `g_at_us` | último `CurrentTargetChangeReq` o golpe hacia el jugador o la marioneta | 120 s |
| `g_last_hp`, `g_actions` | último `VaryHPAck`, últimas 4 `StartActionReq` | Para la copia |
| `g_refused` | jinetes y tiradores de torre | No se copian todavía |
| `enemy_hostile_in_combat(k)` | tipo 3, facción (`+0x1C`) ≠ 0 y apuntó a uno de nosotros en 120 s | Barrera de seguridad para lo que pide el otro jugador |

El relevo automático de hoy (`auto_tick`) copia solo los de tipo 3 **en combate** a ≤ 40 m de H
(máximo 8) y los quita a > 80 m. Para la fase 5 eso no basta (§4.2).

### 3.2 El predicado común (H releva y G suprime **exactamente lo mismo**)

Es el de `pendiente-3g` §2.3, con offsets del payload (`u16 len │ registro`):

```text
es_enemigo_de_zona(carga):
  len + 2 == tamaño del payload                  (un solo registro)
  p[2] == 3                                      (persona; nunca 4 nombrado, 5 montura, 6 animal, 7 objeto)
  u32[+7] == 0                                   (sin dueño: no mercenarios ni mascotas)
  keys::is_game_world_key(u32[+3])               (no nuestras copias ni la marioneta)
  u32[+0x1C] ∈ facciones_hostiles                (55 por tabla ∪ aprendidas en la sesión)
  u32[+0x18] ∉ variantes_excluidas               (957 con "Boss"/"MiddleBoss"/"Unique")
  origen(T0) ∈ {permitida, nodo de facción, ninguna}   (∪ "cautela" con interruptor; nunca prohibida)
  cola: padre (+0xAC) == 0, o padre ∈ suprimidos; nunca padre de tipo 5/7; sin hijos de Hierarchy
```

- **Facciones hostiles:** grupo de relación HostileCombat (`0x4245`) o Monster_Common (`0x4247`) en
  `faction.staticinfo`: 55 de 135 (`p_3g/tables_2976.txt`). De 121 NPC que atacaron al jugador en las
  capturas, 118 eran de esas facciones (`p_3g/s7`).
- **Aprendidas:** facción ≠ 0 de un NPC que apuntó a G o le golpeó (`g_at_us`); H comparte las suyas
  (§5.6). Se pueden guardar en `cdmp_learnt.txt` (`learnt_store`).
- **Origen:** firma de T0 `0C 00 │ 8×00 │ FF×4` (única en 5.332 cargas); horario en `T0+42`
  (hash) / `+46` (id), nodo de facción en `T0+390`, secuencia en `T0+1043` (como
  `strip_spawn_identity`). Lista prohibida: misiones, jefes, contenidos, minijuegos, "wanted",
  "memory".
- **Fuera siempre:** tipo 4 (personajes con nombre), tipo 6 (lobos y osos: H no los releva aún;
  `pendiente-jinetes-animales.md`), jinetes y torres, guardias de ciudad (facciones NPC_Common),
  civiles y animales (facción 0), mercenarios y mascotas (dueño ≠ 0), cadáveres (§4.4).
- **Grupos:** la decisión la toma el líder (se carga antes que sus miembros en 617 de 617,
  `p_3g/s9`): líder suprimido → sus hijos (`+0xB0`) también; líder entregado → hijos entregados.

**Regla de oro:** el predicado y el radio viven en **un solo sitio** del código y los usan los dos
lados. Si H releva menos de lo que G suprime, G ve campamentos vacíos; si releva más, G ve duplicados.

---

## 4. La regla de los 150 m

### 4.1 Medido con dos PC reales [captura, `c7_shadow_zone.py`]

Simulación sobre las capturas del invitado (posición de H = `MoveActorAck` inyectados de `0xA010F001`;
posición de G = sus `MoveActorReq`; nativos = predicado sin la parte de origen):

| | 5 oct (35 min) | 6 oct (14 min) |
|---|---|---|
| G a ≤ 150 m de H | 1.549 de 1.583 s (98 %) | 618 de 626 s (99 %) |
| Cambios de zona (150/170 m) | 3: ON a 20 m, OFF a 171 m (t = 1.774 s), ON a 143 m (t = 1.807 s) | 3: ON, OFF por viaje rápido (825 m), ON |
| Cargas hostiles descartadas | 43 | 17 |
| Nativos ya cargados retirados al encender | 23 (0 peleando con G) | 0 |
| Guardados que el servidor retiró solo | 15 (motivo 25) + 24 (motivo 80) | 15 (25) + 2 (19) |
| **Vivos al apagar (a devolver)** | **6** | 0 |

Lectura: los dos jugadores van casi siempre juntos; la zona cambia pocas veces; la devolución afecta a
pocos nativos y la mayoría se "devuelve sola" porque el servidor los recicla.

### 4.2 Definición

- **Zona:** disco de **R = 150 m** alrededor de la posición de H en el mundo de G
  (`puppet_position()`, la copia dibujada de H; en dos PC el desplazamiento es 0). Las dos partidas
  comparten coordenadas.
- **Encendido:** G a ≤ 150 m de H durante 2 s **y** enlace conectado, `g_ready`, G en el mundo
  (`local_in_world_`), la copia de H cargada, y H ha confirmado que releva (`EnemyZone` §5.6).
- **Apagado:** G a > 170 m de H durante 5 s, **o en el acto** si se cae el enlace, desaparece la copia
  de H (`puppet_position()` vacío > 3 s), cualquiera de los dos salta > 100 m en 1 s (viaje rápido,
  muerte), llega una baja masiva con motivo 19, o el usuario lo apaga.
- **Qué se suprime:** nativos con `es_enemigo_de_zona` a ≤ R de H (posición de carga para los
  nuevos; posición leída de memoria, `actor_world_position`, para los cargados).
- **Qué releva H:** lo mismo, a ≤ R de **su propio** jugador, hasta 24 copias (primero los que pelean
  con alguno de los dos, luego por cercanía a la marioneta de G); quita la copia a > R + 20 m. Hoy el
  relevo solo copia enemigos en combate a ≤ 40 m: hay que ampliarlo (la otra tarea, la del encendido
  automático, debe usar este mismo predicado).
- **Por nativo, durante la zona:** un nativo guardado que queda a > R + 40 m de H **no** se devuelve en
  la primera versión: H tampoco lo tiene copiado, y G casi siempre se aleja con H, así que el servidor
  lo recicla (motivo 25). Devolverlo por nativo es una mejora posible (v2), con histéresis 150/190 m y
  10 s.

### 4.3 Los momentos de cambio

**Encendido** (en este orden):
1. G pasa a "zona activa" y empieza a **descartar cargas nuevas** dentro del disco.
2. G manda `EnemyZone{on}` a H. H responde y empieza a mandar copias.
3. **Barrido** cada 1 s: nativos ya cargados que cumplen el predicado, dentro del disco, **en reposo**
   (§4.4) → un `RemoveActorAck` por grupo con motivo 25 (la salida natural por distancia). Antes se
   guarda su carga y su última posición.
4. Si G está peleando con sus nativos, el encendido espera hasta 60 s a que la pelea acabe (§4.4).

Hay un instante en que G puede ver a la vez nativos (aún no barridos) y copias recién cargadas: dura
como mucho un barrido (1 s) más la cola de carga de copias (8 de golpe, luego 2/s).

**Apagado:**
1. G deja de descartar.
2. G quita las copias de H **en local** (como `enemy_reset_partner` pero solo las copias) y manda
   `EnemyZone{off}`; H deja de relevar (y retira lo que haya enviado).
3. **Devolución:** se reinyectan los grupos guardados que el servidor no ha retirado, en el orden de
   llegada, a 10 cargas por segundo como mucho (§5.5).
4. Si el apagado es por pantalla de carga o viaje rápido no se devuelve nada: el servidor ya lo ha
   retirado todo (motivo 19) y lo recargará con claves nuevas.

### 4.4 Casos de borde

| Caso | Qué hacer | Por qué |
|---|---|---|
| G pelea con sus nativos cuando llega H | El encendido espera a que ningún nativo haya apuntado a G ni le haya golpeado en 10 s (`g_at_us`/`g_engaged`), hasta 60 s. Después se enciende y **esos** nativos siguen hasta que mueran o pasen 10 s sin pelear; entonces se barren | Que desaparezca un enemigo a mitad de espadazo es feo, y sus armas "on demand" pueden quedar sueltas (§1.3). Mientras tanto G puede ver a la vez su nativo y la copia de su gemelo (aceptado) |
| Nativo "armado" (tiene un gimmick de tipo 7 cuya carga lo nombra y no se ha retirado) | No se barre hasta que guarde el arma (llega su retirada, motivo 45) | Evita armas flotando; los descartados al cargar nunca piden armas |
| G pelea con copias y sale de la zona | Al apagar, las copias se van y vuelven los nativos (en reposo, en su sitio) | Cada mundo vuelve a ser el suyo |
| Jefes, élites, enemigos de misión, "wanted", eventos de nodo | Nunca se suprimen; H tampoco los releva | Lista prohibida y variantes excluidas (§3.2). Cada uno ve el suyo (`pendiente-jefes.md`) |
| Enemigos que G ya mató | Los cadáveres (vistos con `NotifyHandleDeadAck`) no se barren: G puede saquearlos. Si el servidor de G los hace reaparecer, la carga nueva se descarta dentro de la zona | El servidor de G sigue con sus reglas de reaparición |
| H ya limpió un campamento que en G sigue vivo | G lo ve vacío (sus nativos ocultos, H no tiene copias). Al salir de la zona, los nativos de G vuelven | Cada mundo tiene su estado |
| Misiones de "matar X" de G | No avanzan matando copias. Se acepta (como Genshin, `genshin-coop-referencia.md` §13); la fase 5.2 (remate del gemelo) lo arreglará en parte | |
| G guarda durante la zona | Sin efecto en el guardado (hipótesis, prueba L6) | El servidor no se ha tocado |
| G muere, viaja o hay pantalla de carga | Apagado inmediato sin devolución; el servidor manda motivo 19 para todo y se vacía lo guardado | |
| Se cae el enlace o se va H | Apagado con devolución | Que G no se quede sin sus enemigos |
| Clave de un nativo retirada por el servidor mientras está guardada | Se olvida (nada que devolver); el `RemoveActorAck` se entrega igual (el cliente la ignora) | 56 de 83 casos en Shadow |
| Una facción hostil por tabla que en el mundo de G no lo es (`UpdateFactionRelationDataAck` 2597, conquistas) | Registrar ese Ack. Si llega para una facción de la lista, excluirla mientras dure | Pregunta P5 |
| Rotación de spawns que no para (un gestor de spawn que pide sin fin lo que no ve) | **Disyuntor**: si una misma identidad (variante + origen) se descarta > 4 veces en 2 min, o > 60 descartes por minuto, se apaga la supresión, se devuelve todo y se avisa en el log y en pantalla | Pregunta P4 |

---

## 5. Plan de implementación

### 5.1 Módulo nuevo: `src/mod/net/native_suppress.{h,cpp}`

API propuesta:

```cpp
namespace cdmp::net {
enum class SuppressMode { Off, DryRun, On };
void suppress_configure(SuppressMode mode, float radius_m, float off_radius_m, bool restore, bool node_events);

// Game network thread, from troc_sniffer's on_recv_packet (not for our own injections):
// true = do not deliver this packet (it was held).
bool suppress_filter_s2c(const uint8_t* packet, uint16_t size);

// Sniffer writer thread (like enemy_observe_*): removals, HP, weapons, positions, aim.
void suppress_observe_s2c(const uint8_t* packet, size_t size);
void suppress_observe_c2s(const uint8_t* packet, size_t size);

// Co-op thread, every update (CoopLink::update), guest only.
struct ZoneInput {
    bool linked, in_world, host_relays;
    std::optional<proto::Vec3> host, me;
    uint64_t now_ms;
};
void suppress_tick(const ZoneInput& in);   // state machine, sweep (1 Hz), restore (10/s)
bool suppress_zone_on();                   // for EnemyZone and the HUD
bool suppress_is_hidden(uint32_t key);     // enemy_relay / guest_hits must skip these
void suppress_stop(bool restore);          // co-op off, partner gone, breaker
}
```

Estado (con un `SRWLOCK`; el hilo de red del juego solo usa `TryAcquire…` y, si no puede, entrega el
paquete):
- `Held { uint64_t seq; Bytes load; std::vector<uint32_t> children; uint32_t parent; bool swept;
  std::optional<Vec3> pos; Quat rot; std::deque<Bytes> state; uint64_t since_ms; }` por clave;
- `std::map<uint32_t, Bytes> natives`: cargas de nativos que cumplen el predicado salvo la posición,
  limitado a 512 y borrado por `RemoveActorAck`. Lo necesita el barrido, porque `enemy_relay::g_loads`
  se vacía a los 1.500;
- `armed`: nativo → gimmicks de tipo 7 vivos que lo nombran;
- `engaged`: nativo → último apunte o golpe a G;
- atómicos con la posición de H y el estado de la zona, para el hilo de red.

### 5.2 Gancho de entrada (`troc_sniffer.cpp`, `on_recv_packet`)

Hoy: `capture()` → (si inyección activa y no `t_injecting`) se inyectan hasta 64 paquetes nuestros
**antes** del real. Cambio:

```cpp
void on_recv_packet(safetyhook::Context& ctx) {
    capture(...);                                   // the capture still records it (diagnostics)
    if (!g_injection_enabled || t_injecting) return;
    ... injection loop as today ...
    uint16_t size = 0; read [rsp + kHookArgSizeStackOffset] guarded;
    if (suppress_filter_s2c(reinterpret_cast<const uint8_t*>(ctx.r9), size)) {
        ctx.rip = reinterpret_cast<uintptr_t>(g_ret_stub);   // VirtualAlloc'd 0xC3, as horse_diag
    }
}
```

- Las inyecciones nuestras (la devolución incluida) pasan con `t_injecting = true` y **nunca** se
  filtran.
- Si la inyección está desactivada (opcodes que no coinciden, una excepción), tampoco se filtra: sin
  inyección no se podría devolver nada.
- `suppress_filter_s2c` solo mira opcode 2005 de un solo registro: copia con SEH los primeros 0x40 B
  y, si es tipo 3 sin dueño de una facción candidata, la carga entera (6,6-11 KB). Luego evalúa el
  predicado sobre la copia. Coste: el de una copia de 8 KB, unas 1,4 veces por segundo (picos de
  270/s al cargar zona).
- En modo `DryRun` registra `WOULD DROP …` y devuelve `false`.
- **El sniffer sigue viendo los descartados** (`capture_observe_s2c`/`enemy_observe_s2c` corren
  después, en el hilo escritor): `enemy_relay` del invitado los tendrá en `g_loads`. No los copia
  (no hay posición en memoria y el invitado no releva con roles estrictos), pero
  `enemy_hostile_in_combat`, `guest_hits` y `player_hits` deben consultar `suppress_is_hidden`.
- Opcional: marcar `(held)` en la línea de texto de la captura para leerla mejor.

### 5.3 Paquetes (formatos exactos, payload sin la cabecera de 5 B)

| Mensaje | Dirección | Formato | Uso |
|---|---|---|---|
| `ActorLoadDataAck` 2005 | S>C | `u16 len │ u8 tipo │ u32 clave │ u32 dueño │ u8 idx │ FF×8 │ … +0x14 u32 usuario de control │ +0x18 u32 variante │ +0x1C u32 facción │ +0x20 u32 n + UTF-16 nombre │ … T0 … │ último Transform: pos (12 B) y quat (16 B) justo antes de la escala (1,1,1) + FF×8 │ … Hierarchy u16 m + m claves │ u32 padre (+0xAC) │ u16 n + n claves hijas (+0xB0)` | Predicado, guardado, devolución |
| `RemoveActorAck` 2516 | S>C (inyectado) | `04 │ u32 0 │ u8 25 │ n × u32 clave` | Barrido de un grupo (líder y miembros en un mensaje) |
| `MoveActorReq` | C>S (observado) | `u32 clave │ f32 x,y,z (+4) │ quat (+0x10) …` (514 B) | Última posición de un nativo barrido, para reescribir su carga |
| `VaryHPAck` 2147 | S>C | 92 B, clave en +0 | Último HP, reentregado tras la carga |
| `NotifyHandleDeadAck` 2530 | S>C | 19 B: `u32 muerto │ u32 asesino │ motivo` | Cadáveres: no se barren |
| `CurrentTargetChangeReq` 2984 | C>S | `u32 actor │ u32 objetivo │ u64 │ u8 │ u8` | Nativo que apunta a G |
| `DoFrameEventReq` 2380 | C>S | `04 │ u32 atacante │ … │ entradas de 665 B al final (u32 objetivo)` | Nativo que golpea a G |
| `ChangeBattleStateReq` 2257 | C>S | `u32 actor │ u8 estado` | En combate |
| `OnDemandEquipDockingGimmickSpawnReq` 2249 | C>S | `04 │ u32 NPC │ u16` | Va a llegar un arma de tipo 7 que nombra al NPC |
| `UpdateFactionRelationDataAck` 2597 | S>C | (sin decodificar) | Registrar; P5 |

### 5.4 Barrido (hilo de co-op, 1 Hz, solo con la zona encendida)

1. Para cada clave de `natives` que no esté guardada: `actor_world_position(clave)` (lectura
   protegida). Si está a ≤ R de H, toma su grupo: líder = la propia clave o su padre, más los hijos del
   líder.
2. Se salta el grupo si algún miembro está **en combate** con G (10 s), **armado**, **muerto**, o si no
   están todos en el cliente (`game::find_actor`).
3. Por cada miembro: guardar la carga, `swept = true`, posición y rotación actuales (memoria o último
   `MoveActorReq`).
4. Un `RemoveActorAck` con todas las claves del grupo (motivo 25) por la cola de inyección
   (`puppet_inject_s2c(kOpRemoveActorAck, …, 0)`).
5. Log: `Suppress: removed group 0x… (3 natives, "Bleed Bandit", 63 m from the host)`.

### 5.5 Devolución (hilo de co-op, al apagar la zona)

1. Lista de grupos guardados vivos (ningún `RemoveActorAck` del servidor para ninguna de sus claves),
   ordenada por `seq` (orden de llegada: el líder antes que sus miembros).
2. Para cada clave: la carga guardada. Si `swept`, se reescriben **todas** las apariciones de la
   posición del último Transform (el método de `spawn_horse`/`puppet_spawn_enemy`) y la rotación con
   las actuales. Si se descartó al llegar, va tal cual.
3. Inyectar `ActorLoadDataAck` y, detrás, los mensajes de estado guardados de esa clave, en orden:
   `VaryHPAck` (el último), `ApplyBuffAck`/`UnapplyBuffAck`, `ChangeMoveSessionIDAck` (el último),
   `UpdateElementalAck` (el último). Como mucho 32 por clave (hipótesis: mejora la fidelidad; L2 lo
   compara con y sin ellos).
4. Ritmo: un grupo por tick y como mucho 10 cargas por segundo. Solo con G en el mundo.
5. A los 2 s, un informe como `enemy_relay::report()`: existe, `+5E`, login `+CF`. Si a los 5 s no ha
   terminado el login: `RemoveActorAck` (motivo 25) del grupo, contador `restore_failed` y el aviso
   "tus enemigos volverán al alejarte". Revisar también `game_errors` (`eErrNoParentsNotExist`,
   errores de login).

### 5.6 Protocolo (`kVersion` 8 → 9)

| Nuevo | Dirección | Formato | Uso |
|---|---|---|---|
| `RelKind::EnemyZone = 8` | G → H | `u8 flags (bit0 on) │ u32 seq │ f32 R` | G decide la zona (es quien suprime). H **solo releva con la zona encendida** y la retira toda al apagarse |
| `RelKind::EnemyZone = 8` | H → G | `u8 flags (bit0 relays, bit1 cautela) │ u32 seq (eco) │ f32 R │ u16 n │ n × u32 facción aprendida` | H confirma y comparte las facciones aprendidas. **G no suprime nada sin esta confirmación** |

- `coop.cpp`: `accepts(RelKind::EnemyZone)` en los dos roles; validar tamaños (`n ≤ 256`, `R` de 50 a
  300 m).
- Identidad de cada copia (para enlazar copia ↔ gemelo en la fase 5.2): ampliar `EnemySpawn` con
  `u8 clase │ u32 hash/nodo │ u16 id/slot │ u32 variante │ f32 x,y,z` (`pendiente-3g` §3.6). No hace
  falta para suprimir.

### 5.7 Cambios en lo que ya existe

- **`enemy_relay` en H:** con la zona del compañero encendida, `auto_tick` usa `es_enemigo_de_zona` a
  ≤ R de H (no solo los que pelean), con un tope de 24 y prioridad (en combate > cerca de la marioneta
  de G); retira a > R + 20 m. Con la zona apagada, ninguna copia para G. El receptor admite 48.
  Coste: un campamento de 7 son unos 54 KB fiables; 20-30 enemigos, 200-300 KB de golpe y unos
  15-20 KB/s de posiciones.
- **`enemy_relay` en G:** no releva (ya lo garantizan los roles estrictos con Steam). Al apagar la
  zona, quitar las copias en local sin esperar a H.
- **Daño espejo** (`coop_damage`, hoy apagado por defecto): forzarlo apagado mientras la zona esté
  encendida.
- **`coop.cpp`:** construir `ZoneInput` en `update()` (después de `enemy_set_linked`); llamar a
  `suppress_stop(true)` en `forget_partner()` y en el destructor; HUD: "Enemigos del anfitrión (zona
  150 m)" o "Tus enemigos".
- **Tablas:** `p_3g/tables_2976.txt` → arrays `constexpr` ordenados (55 facciones, 957 variantes, 585
  hashes permitidos, 1.170 de cautela, prohibidos). Solo se usan con PE `0x6AB28F00`.
- **`dllmain`/`mod.cpp`:** nada en `DllMain`; el módulo se activa desde el hilo worker junto al
  relevo.

### 5.8 Configuración (`cdmp.ini`)

```text
[coop] suppress=dry            ; 0 | dry | 1   (por defecto dry hasta pasar L1-L2)
       suppress_radius_m=150
       suppress_off_m=170
       suppress_restore=1      ; devolver los nativos al apagar la zona
       suppress_wait_fight_s=60
       suppress_node_events=0  ; clase "cautela" (eventos diarios y de nodo)
       suppress_lab=0          ; laboratorio: zona manual centrada en ti, relevo apagado
```

Laboratorio: **Ctrl+Inicio** enciende o apaga una zona fija en tu posición actual (radio 60 m, o el de
`suppress_radius_m` si se pone `suppress_lab_radius_m`). F6-F12 ya están ocupadas: F8 sin
modificadores alterna el panel, y Ctrl+F7 ignora Mayús.

### 5.9 Registro

- Cambios de zona: `Suppress: zone ON (host 42 m away, 7 natives to hide)` /
  `zone OFF (host 175 m away | link lost | loading screen), restoring 6`.
- Cada carga descartada (las 50 primeras y luego 1 de cada 20): clave, nombre, facción, variante,
  origen (S/Q/F/-), distancia a H.
- Barridos, aplazamientos (`kept 0x… fighting you`), devoluciones con antigüedad (`held 312 s`) y el
  informe de login a los 2 s.
- Cada 10 s: `held N, dropped N, swept N, deferred N, restored N, restore_failed N, server removed
  while held N, breaker off`.
- Disyuntor y Ack de relación de facción, siempre.

---

## 6. Pruebas

Antes de cualquier prueba que escriba: copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`
(en los dos PC para las de dos jugadores). `[sniffer] record=1`. Campamento de referencia: Bleed
Bandits de `cd_m11_m04_monster_beacon_01_0003` (cerca de (−10.284, 640, −3.383)) o
`Node_Her_EmbernestBeacon` (−10.201, 623, −3.481).

### 6.1 Sin juego

- **T0:** pruebas unitarias del predicado con 10-20 cargas reales (bandidos, guardia, ciervo,
  mercenario, jinete, líder con hijos, jefe) sacadas de `p_3g/hostile_rows.tsv`/capturas a
  `tests/data/`.
- **T1:** prueba del protocolo de `EnemyZone` (codificar, decodificar y validar tamaños) en la batería
  de `build.ps1`.

### 6.2 Un PC

En eco (`cdmp_peer`), este PC es H y G a la vez: suprimir con el relevo encendido haría desaparecer lo
que se copia. Por eso: dry-run con eco, y laboratorio sin relevo.

| # | Prueba | Escribe | Se espera |
|---|---|---|---|
| **L0** | Eco, `suppress=dry`: `WOULD DROP`/`WOULD SWEEP` en el campamento y en una ciudad | No | Solo los bandidos; 0 en ciudad; ningún guardia, animal, civil, jinete ni mercenario |
| **L0b** | Eco con relevo en modo zona (H) y supresión en dry-run (G): en el log, el conjunto que H relevaría y el que G suprimiría | No | **Idénticos** (mismo mundo, misma posición salvo el desplazamiento del eco). Comprueba la "regla de oro" y `EnemyZone` |
| **L1** | Laboratorio: zona a > 250 m del campamento, caminar hasta él | Descarta | Campamento vacío, sin barras de vida ni ataques. `game_errors` sin `eErrNoParentsNotExist`. En la captura, cargas sin `RegionInOutReq` detrás. Quedarse **10 min** a 30-60 m: anotar si el servidor retira o mueve claves guardadas (P1) |
| **L1b** | Laboratorio junto a un campamento cargado y en reposo | Inyecta bajas | Desaparecen (fundido del motivo 25). C>S: salida de región y `ChangeAggroRelationValidTickReq` |
| **L1c** | Igual peleando con ellos | Inyecta | Los que pelean se quedan; los demás se van; al dejar de pelear 10 s, se barren. Sin armas flotando |
| **L2** | **Devolución:** zona 30 s, 5 min y 20 min con el campamento oculto; apagar a 60-80 m | Inyecta cargas | En su sitio, login `+CF 1`, atacan, se pueden matar y dan botín. Repetir con y sin los mensajes de estado guardados. Decide `suppress_restore` |
| **L2b** | Devolución de barridos (se movieron antes) | Inyecta | Aparecen donde estaban, no en su punto de carga |
| **L3** | Zona encendida, alejarse 300 m y volver | Descarta | Llegan bajas (motivo 25) de claves guardadas (ignoradas, se olvidan); al volver, claves nuevas descartadas otra vez. El disyuntor no salta |
| **L4** | Viaje rápido o muerte con la zona encendida | No | Apagado sin devolución; todo lo guardado se vacía con el motivo 19 |
| **L5** | Disyuntor: bajar los umbrales y forzarlo | Inyecta | Se apaga, devuelve y avisa |
| **L6** | Guardar con nativos ocultos, salir, cargar sin el mod | No | El campamento está normal |

### 6.3 Dos PC (Shadow invitado, este PC anfitrión, Steam)

Los logs y las capturas de Shadow se traen al PC del anfitrión para analizarlos.

| # | Prueba | Se espera |
|---|---|---|
| **G1** | Empezar juntos junto al campamento | G ve solo las copias de H, ningún nativo duplicado. Log de G: `zone ON`, descartes; log de H: `EnemyZone` y relevo de todo el campamento |
| **G2** | G se aleja 200 m y vuelve | A > 170 m: las copias se van y los nativos de G vuelven (`restored N`, login); al volver a ≤ 150 m se barren otra vez |
| **G3** | G pelea con sus nativos lejos de H; H se acerca | El encendido espera (hasta 60 s); luego desaparecen los nativos y quedan las copias |
| **G4** | H cierra el juego | G apaga la zona y recupera sus enemigos |
| **G5** | G guarda durante la zona y carga esa partida solo | Mundo normal |

Capturas de pantalla en los dos (`System.Drawing CopyFromScreen`, recortando la zona del campamento).

---

## 7. Riesgos, preguntas abiertas y confianza

| # | Riesgo o duda | Gravedad | Cómo se reduce o se decide |
|---|---|---|---|
| R1 | La devolución con cargas de varios minutos falla (CC, buffs) | Media | L2 a 30 s, 5 min y 20 min; si falla, `suppress_restore=0` y la vuelta natural (alejarse o pantalla de carga) |
| R2 / P1 | El servidor hace algo con NPC que nadie controla durante mucho tiempo | Desconocida (0 precedentes) | L1 de 10 min; el disyuntor y los contadores |
| R3 | H y G no usan el mismo predicado o radio (campamentos vacíos o duplicados) | Media | Una sola función compartida; L0b |
| R4 | Grupos incompletos (líder en estado 8, miembro destruido) | Media | Todo o nada por grupo, en el orden de llegada; `game_errors` |
| R5 | Retirar en combate (armas sueltas, estado de combate pegado) | Baja con la espera | Esperar a que no pelee; L1c |
| R6 / P4 | Bucle de spawns que no ve sus NPC | Baja | Disyuntor |
| R7 / P9 | Regiones fuera de Hernand con orígenes no clasificados | Baja (se ve un nativo de más, nunca uno de menos) | Dry-run en otras regiones |
| R8 / P5 | Relaciones de facción que cambian en partida | Baja | Registrar `UpdateFactionRelationDataAck` |
| R9 | Un cierre del juego (se reporta a Pearl Abyss) | Baja | Solo se salta una función entera en su entrada; lecturas con SEH; inyecciones por la cola de siempre; nada en `DllMain` |

**Confianza:** alta en que el descarte y el barrido ocultan sin romper nada; media en la devolución;
el efecto de muchos minutos sin controlador en el servidor de G está por medir, pero no se ha escrito
nada en él.

---

## 8. Orden de trabajo propuesto

1. **5.1-0** (sin escrituras): tablas, predicado compartido, `suppress=dry`, `EnemyZone` (protocolo 9).
   Pruebas T0, T1, L0, L0b.
2. **5.1-1** (laboratorio): descarte, barrido, disyuntor, contadores, devolución. Pruebas L1-L6.
3. **5.1-2**: zona real alrededor de H, relevo de H por zona (con la tarea del encendido automático) y
   daño espejo apagado. Pruebas G1-G5 con Shadow.
4. **5.2**: remate del gemelo oculto en G cuando muere el enemigo en H (necesita la identidad en
   `EnemySpawn`).

---

## 9. Apéndice: direcciones (build 2976) y scripts

| RVA | Qué es |
|---|---|
| `0x35CA00` | `ClientNet::OnRecvPacket`. Llamadores `0x35BB53`, `0x35C0ED`, `0x35C951`, `0x86E55AA`: ninguno usa `rax` después (`0x35BB58`, `0x35C0F2`, `0x35C956`, `0x86E55AF`) |
| `0xB62C70` | Handler cliente de `RemoveActorAck` (salta claves desconocidas en `0xB62FD4`; enlazado `vf(0x160)`, motivo 6) |
| `0x2068340` → `.xtls 0xE0D9AF0` | Baja local del actor, sin red |
| `0x8A67E0` | Busca actor por clave en `[[0x6D69188]+0x38]` |
| `0x29C90D0` | Deserializador servidor de `RegisterSpawnDistanceByExternalReq` 2817 (`u8 │ u32 │ n × 16 B`) |
| `0x29C3530` / `0x29C3A90` | Deserializadores de `ActivateSpawningPoolReq` 2424 / `DeactivatePoolSpawnReq` 2119 |
| `0x29BF730` / `0x29BF3C0` | Deserializadores de `KillCheatReq` 2279 / `VaryStatCheatReq` 2703 |
| `0x4FA530`, `0x88E8E0` | Usuario de control → modo de control (`load+0x14` = `actor+0x90`) |

Scripts nuevos en `build/research/p_f5/` (`build/research/venv/Scripts/python.exe`; usan
`pieza4/pk.py` y su caché):

| Script | Qué mide | Resultado citado |
|---|---|---|
| `c1_opcount.py` | Uso C>S/S>C de todas las Req de spawn, despawn, recarga y truco en las 83 capturas | §2.1, §2.3 |
| `c3_poolcorr.py <sid>` | Ids de pool en las cargas de NPC; bajas tras `DeactivatePoolSpawnReq` | 21 de 831 (114648) |
| `c4_loadage.py` | Antigüedad de la carga original cuando se inyectó su copia (logs del mod + captura del eco) y login | 196 casos, 0,4-198 s, mediana 44 s |
| `c5_uncontrolled.py <sid…>` | Cargas de NPC que el cliente nunca manejó | 11 de 9.789 (8 copias, 3 al final) |
| `c7_shadow_zone.py <bin> [R]` | Simulación de la zona de 150/170 m sobre una captura del invitado real | §4.1 |
| `c7b_shadow_about.py <bin>` | Mensajes S>C sobre nativos "suprimidos" mientras lo están | §2.1 |
| `c8_engaged_removal.py` | Bajas del servidor de NPC vivos < 30 s después de golpear al jugador | 57 (28: 41, 19: 11, 42: 3, 25: 2) |
| `c9_r28.py` | Qué rodea esas bajas: mensajes antes, armas, C>S después, estado de combate del jugador | §1.3 |

Nota: `pk.dfe_targets()` devuelve pares `(clave, offset)`; los scripts lo tienen en cuenta.
