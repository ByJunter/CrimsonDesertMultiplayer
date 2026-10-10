# El cooperativo de Genshin Impact como referencia para enemigos compartidos

Investigación de fuentes públicas para la **opción (a), autoridad del host** de
`combate-coop.md` §4.3: el host simula los enemigos y el invitado ve copias movidas por red y
le envía sus golpes.

Marcas: **[fuente]** con enlace, **[inferido]** cuando es deducción nuestra (de nombres de
campos, del código o de cómo encaja con nuestro caso).

## Fuentes y alcance

- **Grasscutter** (reimplementación del servidor, AGPL-3.0), commit
  [`f373827`](https://github.com/Grasscutters/Grasscutter/tree/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9)
  (abril de 2025). Lo he leído como referencia, sin copiar código. Su propio README dice que el
  cooperativo funciona **solo en parte**. Más abajo, `GC/...` abrevia
  `https://github.com/Grasscutters/Grasscutter/blob/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9/src/main/java/emu/grasscutter/...`.
- **Esquema del protocolo**: las clases protobuf generadas en Grasscutter
  (`src/generated/.../net/proto/*OuterClass.java`). Los nombres de los campos dicen qué
  datos existen, pero no cómo los usa el servidor oficial.
- **Reglas oficiales visibles para el jugador**: la
  [guía de cooperativo de Game8](https://game8.co/games/Genshin-Impact/archives/297418) y la
  [página de escalado de enemigos de la wiki](https://genshin-impact.fandom.com/wiki/Enemy/Level_Scaling)
  (la consulté por el extracto del buscador, porque la wiki bloquea la descarga directa).
- **Descartado**: el servidor oficial filtrado en 2022 y sus derivados (por ejemplo
  "hk4e-emu"). Es código propietario filtrado: no lo he consultado.
- **Límite importante**: **Grasscutter nunca cambia la autoridad** (siempre es el host) y deja
  vacíos varios handlers de sincronización de IA. Lo que se diga aquí sobre cómo pasa la
  autoridad entre jugadores en el juego oficial es **[inferido]** del esquema. No encontré
  ninguna charla ni artículo público de miHoYo/HoYoverse sobre su red.

---

## 0. Resumen

1. **Genshin tiene un solo servidor por mundo (el del host) y los clientes simulan.** El
   movimiento, la IA, las habilidades y la detección de golpes salen del cliente que tiene la
   **autoridad** sobre cada entidad. El servidor guarda HP, muerte, botín, scripts y misiones,
   y reenvía todo lo demás. [fuente: `GC/server/packet/recv/HandlerCombatInvocationsNotify.java`]
2. **"Host" y "autoridad" son conceptos distintos.**
   - El *host* es el dueño del mundo: sus datos, su nivel de mundo, sus misiones y sus cofres.
   - El *authority peer* es el cliente que simula cada entidad. Cada monstruo lleva un
     `authority_peer_id`.

   El protocolo permite traspasar la autoridad (`EntityAuthorityChangeNotify`) junto con el
   estado de la IA. [fuente: protos `SceneMonsterInfo`, `AuthorityChange`,
   `EntityAuthorityInfo`]
3. **El daño lo calcula el cliente del atacante y el servidor se lo cree.** El servidor resta
   HP y emite el nuevo HP a todos (`EntityFightPropUpdateNotify`). Si el monstruo llega a 0,
   el servidor emite la muerte y el botín. [fuente: `GC/game/world/Scene.java#L476`,
   `GC/game/entity/GameEntity.java#L174`]
4. **Cuando el invitado entra, abandona su propio mundo.** Su mundo deja de simularse. Solo
   el host avanza la historia, abre cofres e inicia dominios. [fuente: `MultiplayerSystem`,
   Game8]
5. **Los monstruos aparecen y desaparecen por mensajes del servidor que llevan un motivo**
   (`MEET`, `BORN`, `DIE`, `REMOVE`, `REPLACE`…) y una instantánea completa de la entidad.
   [fuente: protos `SceneEntityAppearNotify`, `VisionType`]
6. **Para nosotros:** la opción (a) encaja con la "autoridad = host" de Grasscutter, que
   funciona aunque sea en parte. Hay tres diferencias que Genshin no tiene que resolver y
   nosotros sí:
   - el servidor del invitado sigue vivo y crea sus propios enemigos;
   - el servidor del host solo hace aparecer enemigos cerca **del host**;
   - la IA del host no conoce al clon del invitado.

   La §5 propone cómo tratar cada una.

---

## 1. Autoridad sobre las entidades

### 1.1 Lo que dice el protocolo

- **Identidad de jugador en el mundo.** Cada jugador recibe un `peer_id` al entrar: 1 para el
  host y 2, 3… para los invitados. `EnterScenePeerNotify` le dice a cada cliente su `peer_id`
  y el `host_peer_id`.
  [fuente: [`World.java`](https://github.com/Grasscutters/Grasscutter/blob/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9/src/main/java/emu/grasscutter/game/world/World.java#L72),
  `PacketEnterScenePeerNotify.java`]
- **Autoridad por entidad.**
  - `SceneMonsterInfo.authority_peer_id`, en la instantánea de cada monstruo.
  - `EntityAuthorityInfo`, en cada `SceneEntityInfo`, con:
    - `ai_info`: `is_ai_open`, `born_pos`, `skill_cd_map`, `skill_group_cd_map`,
      `ai_threat_map`, `cur_tactic` y `servant_info` (amo y ranura de invocación);
    - además `ability_info`, `renderer_changed_info`, `pose_para_list` y
      `client_extra_info`.

  [fuente: protos `SceneMonsterInfo`, `EntityAuthorityInfo`, `SceneEntityAiInfo`]
- **Traspaso.** `EntityAuthorityChangeNotify` lleva una lista de
  `AuthorityChange {entity_id, authority_peer_id, entity_authority_info}`. Es decir, al
  cambiar de autoridad se envía **todo el estado de la IA** que el nuevo cliente necesita para
  continuar: enfriamientos, tabla de amenaza, táctica actual y posición de nacimiento.
  [fuente: protos `AuthorityChange`, `EntityAuthorityChangeNotify`] Lo que se puede deducir
  de ello: el traspaso funciona como "migración de estado" y no como "reinicio de IA".
  [inferido]
- **Información que los clientes mandan al servidor sobre la IA:**
  - `EntityAiSyncNotify {local_avatar_alerted_monster_list, info_list: AiSyncInfo {entity_id,
    is_self_killing, has_path_to_target}}`;
  - `MonsterAlertChangeNotify {avatar_entity_id, is_alert, monster_entity_list}`;
  - `EvtAiSyncCombatThreatInfoNotify {combat_threat_info_map}`;
  - `EvtAiSyncSkillCdNotify`;
  - `EntityAiKillSelfNotify {entity_id}`.

  [fuente: protos] Cada cliente informa de qué monstruos **ha alertado su propio
  personaje** y de si un monstruo **tiene camino hasta su objetivo**. Son justo los datos que
  un servidor usaría para decidir a qué cliente dar la autoridad. Por ejemplo, al jugador con
  el que el monstruo está peleando o al que está cerca cuando el host está lejos. La regla
  exacta no es pública. [inferido]
- **Enrutado.** Cada entrada de combate o habilidad lleva un `ForwardType` elegido por el
  **cliente**: `TO_ALL`, `TO_ALL_EXCEPT_CUR`, `TO_HOST`, `TO_PEER`/`TO_PEERS` (con
  `forward_peer`), `TO_ALL_GUEST` y `ONLY_SERVER`. `AbilityInvokeEntry` lleva además
  `is_ignore_auth`, lo que indica que el servidor comprueba la autoridad del emisor y que hay
  excepciones explícitas. [fuente: protos `ForwardType`, `CombatInvokeEntry`,
  `AbilityInvokeEntry`; inferido para la comprobación]

### 1.2 Lo que hace Grasscutter

- **Monstruos, objetos del mundo y objetos tirados: autoridad = host, siempre**
  (`setAuthorityPeerId(world.getHostPeerId())`). Lo creado por un cliente (gadgets de
  habilidades) y los vehículos tienen autoridad = su dueño.
  [fuente: [`EntityMonster.java#L453`](https://github.com/Grasscutters/Grasscutter/blob/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9/src/main/java/emu/grasscutter/game/entity/EntityMonster.java#L453),
  `EntityClientGadget.java#L144`, `EntityVehicle.java#L90`]
- **Nunca envía `EntityAuthorityChangeNotify`.**
- **Sincronización de la IA:**
  - `EntityAiSyncNotify`: lo reenvía a la escena si trae monstruos alertados.
  - `MonsterAlertChangeNotify`: dispara una sola vez el evento de script "monstruo entra en
    combate" y anota qué jugadores pelean con él (`playerOnBattle`).
  - Los handlers de amenaza y de enfriamientos están **vacíos** ("Auto template").

  [fuente: `GC/server/packet/recv/HandlerEntityAiSyncNotify.java`,
  `HandlerMonsterAlertChangeNotify.java`, `HandlerEvtAiSyncCombatThreatInfoNotify.java`]
- Con todo eso, el cooperativo de Grasscutter funciona "en parte": la IA vive solo en el
  cliente del host y el invitado la ve por reenvío. Es exactamente nuestra opción (a).

### 1.3 Paralelismo con Crimson Desert

El MMO de Crimson Desert ya funcionaba con **autoridad del controlador**: cada NPC tiene un
dueño (`actor+0x90`) y existe `OverriedOwnershipStateTypeyAck` (2168) (ver
`combate-coop.md` §2.1). Equivalencias:
- `authority_peer_id` de Genshin ≈ nuestro dueño `+0x90`;
- `EntityAuthorityChangeNotify` ≈ posiblemente 2168. [inferido; conviene mirar su formato
  antes de inventar uno propio]

---

## 2. Qué se sincroniza y con qué frecuencia

| Dato | Quién lo produce | Mensaje (campos clave) | Qué hace el servidor (Grasscutter) |
|---|---|---|---|
| Movimiento | Cliente con autoridad | `CombatInvocationsNotify` → `ENTITY_MOVE`: `EntityMoveInfo {entity_id, motion_info, scene_time, reliable_seq, is_reliable}` | Guarda posición, rotación, `scene_time` y `reliable_seq`, y lo reenvía según `forward_type`. **Lo ignora mientras el emisor está cargando.** No reenvía los estados `MOTION_STATE_NOTIFY`/`FIGHT`, porque cortan la animación en los demás clientes [fuente: `HandlerCombatInvocationsNotify.java#L49-L104`] |
| Corrección de posición | Cliente o servidor | `SYNC_ENTITY_POSITION`, `COMPENSATE_POS_DIFF`, `FORCE_SET_POS_INFO` (argumentos de combate); `EntityForceSyncReq/Rsp {motion_info / fail_motion}`; `SceneEntityMoveNotify` | El servidor puede rechazar una posición y devolver la buena (`fail_motion`) [fuente: protos `CombatTypeArgument`, `EntityForceSyncRsp`] |
| Animación y orientación | Cliente con autoridad | `ANIMATOR_STATE_CHANGED`, `ANIMATOR_PARAMETER_CHANGED` (`is_server_cache`), `FACE_TO_DIR`, `SET_ATTACK_TARGET`, `MONSTER_DO_BLINK` | Solo reenvío (borra `is_server_cache` antes de reenviar) [fuente: ídem] |
| Habilidades (buffs, efectos, gadgets) | Cliente dueño de la entidad | `AbilityInvocationsNotify` (`entity_id`, `forward_type`, `forward_peer`, `is_ignore_auth`) | Procesa algunas y reenvía todas [fuente: `HandlerAbilityInvocationsNotify.java`] |
| Golpe y daño | **Cliente del atacante** | `EVT_BEING_HIT`: `EvtBeingHitInfo {peer_id, frame_num, attack_result}`; `AttackResult {attacker_id, defense_id, damage, element_type, ability_identifier, hit_eff_result, hit_collision, resolved_dir, anim_event_id…}`. Hay una variante agrupada, `EvtBeingHitsCombineNotify` | **Se fía del `damage`** del cliente. Lo encola y lo aplica **después** de procesar y reenviar todo el lote del paquete [fuente: `HandlerCombatInvocationsNotify.java#L35-L47`, `HandlerUnionCmdNotify.java#L33-L40`] |
| HP | Servidor | `EntityFightPropUpdateNotify {entity_id, fight_prop_map}` | Lo emite a toda la escena tras cada golpe [fuente: `GameEntity.java#L174-L221`] |
| Alerta y combate | Cada cliente sobre su personaje | `MonsterAlertChangeNotify`, `EntityAiSyncNotify` | Scripts y lista de jugadores en combate (ver §1.2) |
| Muerte | Servidor | `LifeStateChangeNotify {entity_id, life_state, source_entity_id, attack_tag, die_type, move_reliable_seq}` + `SceneEntityDisappearNotify(VISION_TYPE_DIE)` | `killEntity`: códice, botín, retirada de la escena, scripts `EVENT_ANY_MONSTER_DIE` y eventos de misión para **todos** los jugadores de la escena. Mata también a las invocaciones del monstruo y apunta el spawn como muerto para que no reaparezca [fuente: `Scene.java#L500-L558`, `EntityMonster.java#L270-L345`] |
| Botín | Servidor | Entidades de objeto (`EntityItem`) o ingreso directo en el inventario | Si el botín cae al suelo, lo crea a nombre del jugador 0 y "compartido"; si no, lo mete en el inventario de **cada** jugador. Los cofres son solo del host [fuente: `DropSystem.java#L102-L108`, `GadgetChest.java#L69`] |

**Frecuencia:**
- El cliente agrupa sus mensajes en `UnionCmdNotify`. Grasscutter procesa cada lote, vacía las
  colas de reenvío (`InvokeHandler.update`) una vez por lote y después aplica los golpes.
  [fuente: `HandlerUnionCmdNotify.java`, `GC/game/player/InvokeHandler.java`]
- El bucle del servidor va a **1 Hz**: aparición y retirada de grupos y spawns, scripts y
  tiempo de escena. El movimiento no espera a ese tic, porque se reenvía al llegar.
  [fuente: `GameServer.java` (`scheduleAtFixedRate(..., 1000L)`), `Scene.onTick`]
- La cadencia exacta de movimiento del cliente oficial no es pública. Que existan
  `reliable_seq`/`is_reliable` indica dos clases de mensaje: los periódicos, que se pueden
  perder, y los fiables (cambios de estado). [inferido]

---

## 3. El mundo y las misiones del invitado

**Reglas oficiales:**
- solo el host abre cofres, avanza la historia, habla con la mayoría de NPC e inicia dominios
  y desafíos;
- el invitado no puede recoger especialidades ni minerales que reaparecen con el tiempo;
- sí puede atacar, recoger botín y resolver puzles con el host;
- el nivel y el botín de los enemigos siguen el nivel de mundo del host;
- solo se entra en mundos de nivel igual o inferior al propio.

[fuente: [Game8](https://game8.co/games/Genshin-Impact/archives/297418)]

**Escalado:** el HP de los enemigos se multiplica por 1,5 con 2 jugadores, 2,0 con 3 y 2,5
con 4 (por 1,25, 1,5 y 1,75 en algunos jefes semanales).
[fuente: [wiki, Enemy/Level Scaling](https://genshin-impact.fandom.com/wiki/Enemy/Level_Scaling)]
Grasscutter no lo implementa en el mundo abierto.

**Cómo lo modela Grasscutter:**
- **El invitado sale de su mundo.** `World.addPlayer` lo saca de su `World` anterior, y la
  escena vacía se da de baja (`deregisterScene`): **el mundo del invitado deja de existir
  mientras visita.** Recibe un `peer_id` nuevo y su equipo se copia a un "equipo multijugador".
  [fuente: [`MultiplayerSystem.java`](https://github.com/Grasscutters/Grasscutter/blob/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9/src/main/java/emu/grasscutter/game/systems/MultiplayerSystem.java),
  `World.java#L174-L230`, `Scene.java#L251-L281`]
- **El host también recarga.** Al pasar de un jugador a cooperativo, el host vuelve a entrar
  en su escena (`EnterReason.HostFromSingleToMp`). El invitado entra con
  `ENTER_TYPE_OTHER / TeamJoin` en la posición del host. [fuente: ídem]
- **Al salir o ser expulsado**, el jugador vuelve a un `World` propio nuevo (`TeamBack`,
  `TeamKick`). **Si el host se va, el mundo se disuelve** y todos vuelven al suyo. Para salir
  hace falta que **todos** hayan terminado de cargar. [fuente: `World.java#L303`,
  `MultiplayerSystem.leaveCoop`]
- **Misiones:** Grasscutter encola los eventos de "monstruo muerto" en el gestor de misiones
  de todos los jugadores de la escena. El juego oficial congela la historia del invitado.
  Que sus contadores de "matar X" avancen igualmente no está documentado. [inferido]

---

## 4. Aparición y desaparición de monstruos para el invitado

- **Visibilidad = unión de los jugadores.** En cada tic, `checkGroups()`/`checkSpawns()` juntan
  los grupos y celdas cercanos a **cualquier** jugador de la escena:
  - carga lo nuevo y lo anuncia a toda la escena con `VISION_TYPE_BORN`;
  - descarga lo que ya no está cerca de nadie con `VISION_TYPE_REMOVE`.

  Grasscutter no filtra por jugador: todos reciben todas las entidades de la escena.
  [fuente: [`Scene.java#L792-L963`](https://github.com/Grasscutters/Grasscutter/blob/f373827a83d3e688fd3c6afb1e1c3759ca1d61c9/src/main/java/emu/grasscutter/game/world/Scene.java#L792)]
- **El motivo viaja con el evento** (`VisionType`), y así el cliente sabe qué animación
  reproducir:
  - `MEET`: ya existía; el jugador acaba de verlo;
  - `BORN`: aparece ahora;
  - `REBORN` y `WAYPOINT_REBORN`;
  - `DIE`: muerte, con su animación;
  - `REMOVE`: retirada silenciosa;
  - `MISS`;
  - `REPLACE`: con `param` = id antiguo;
  - `TRANSPORT`, `PICKUP`, `GATHER_ESCAPE`…

  [fuente: proto `VisionType`, `Scene.java#L434-L461`]
- **Entrada tardía.** El invitado solo recibe las entidades existentes cuando **termina de
  cargar** (`EnterSceneDoneReq` → `showOtherEntities` con `VISION_TYPE_MEET`). Hasta entonces
  el servidor ignora sus movimientos.
  [fuente: `HandlerEnterSceneDoneReq.java`, `HandlerCombatInvocationsNotify.java#L52`]
- **Instantánea completa en la aparición** (`SceneEntityInfo`):
  - `entity_id`, tipo, `motion_info`, `life_state`;
  - `fight_prop_list` (HP actual y máximo) y `prop_list` (nivel);
  - `last_move_scene_time_ms` y `last_move_reliable_seq`;
  - `entity_authority_info`;
  - `monster {monster_id, group_id, config_id, authority_peer_id, pose_id, block_id,
    owner_entity_id, summon_tag_map, affix_list…}`.

  [fuente: protos `SceneEntityInfo`, `SceneMonsterInfo`; `EntityMonster.toProto`]
- **Identificadores.** Los asigna el mundo del host: `(tipo << 24) + contador`. Los gadgets
  que crea un cliente usan el id que propone ese cliente (el servidor rechaza duplicados), y
  existe `EvtLocalGadgetOwnerLeaveSceneNotify` para limpiarlos cuando su dueño se va.
  [fuente: `World.java#L170`, `HandlerEvtCreateGadgetNotify.java`, lista de protos]
- **Muertos persistentes.** Los spawns muertos se guardan (`deadSpawnedEntities`) y no
  reaparecen al recargar la zona. [fuente: `Scene.checkSpawns`, `EntityMonster.onDeath`]

---

## 5. Lecciones para nuestra opción (a)

En nuestro caso no hay un servidor, sino **dos**: dos servidores locales, dos clientes y
nuestro relevo entre ellos. En la opción (a):
- el servidor del host hace de "servidor del mundo";
- el **cliente del host es el authority peer de todos los monstruos** (como en Grasscutter);
- el cliente del invitado solo los ve;
- el servidor del invitado **sobra** para los enemigos, y hay que neutralizarlo (Genshin lo
  resuelve sacando al invitado de su mundo; nosotros no podemos).

### 5.1 Decisiones de diseño

1. **Separar "host" de "autoridad" en nuestro protocolo desde el principio.** Cada entidad
   relevada lleva `net_id`, `authority_peer` (de momento siempre el host) y `owner_peer`
   (quién da botín y misiones). Así un traspaso futuro no rompe el formato (subir `kVersion`
   al introducirlo). [inferido de `authority_peer_id` y `EntityAuthorityChangeNotify`]
2. **Ids de red asignados por el host, nunca claves locales ni direcciones.** El host asigna
   `net_id`, y cada PC guarda su tabla `net_id ↔ clave local` (`0xB01…` en el host, la clave
   traducida en el invitado). Lo que crea el invitado (flechas) va en un rango de ids
   propio del invitado, como los gadgets de cliente de Genshin. Al recargar, el servidor del
   host reutiliza claves (§4.1): la tabla se invalida con cada `REMOVE`/`DIE`.
   [fuente: `World.getNextEntityId`, `HandlerEvtCreateGadgetNotify`; inferido para nuestro
   caso]
3. **Aparición = instantánea completa + motivo.** Nuestro evento de aparición debería llevar
   tipo o personaje, posición y rotación, HP actual y máximo, estado de vida, nivel,
   `authority_peer`, el último número de secuencia de movimiento y un **motivo**:
   - `BORN`: animación de aparición;
   - `MEET`: aparece directamente;
   - `DIE`: animación de muerte;
   - `REMOVE`: borrado silencioso.

   En nuestro caso, la instantánea es el `ActorLoadDataAck` del host traducido (§4.3 (a)).
   [fuente: `SceneEntityInfo`, `VisionType`]
4. **Entrada tardía con barrera de carga.** Cuando el invitado termina de cargar (o se conecta
   a mitad de partida), el host le manda un `MEET` de todos los enemigos vivos cerca. Antes
   de eso no le mandamos movimientos. [fuente: `HandlerEnterSceneDoneReq`]
5. **Movimiento con secuencia y tiempo por entidad.**
   - Cada movimiento relevado lleva `seq` y `t_escena` del host, y el invitado descarta los
     que son más antiguos que el último aplicado.
   - La muerte lleva el último `seq` (como `LifeStateChangeNotify.move_reliable_seq`) para
     descartar los movimientos que lleguen tarde de un enemigo ya muerto.
   - Hacen falta dos clases de mensaje: periódicos que se pueden perder (posición) y fiables
     (cambio de estado o acción, muerte).

   [fuente: `EntityMoveInfo`, `LifeStateChangeNotify`]
6. **Corrección periódica de posición.** Genshin tiene mensajes específicos para
   compensar desviaciones (`COMPENSATE_POS_DIFF`, `FORCE_SET_POS_INFO`, `EntityForceSyncRsp`).
   Nosotros: cada N segundos o en cada cambio de acción, una posición absoluta del host que
   el invitado aplica si se desvía más de X. [fuente: protos; umbral inferido]
7. **El HP tiene una sola fuente: el servidor del host.** El invitado solo muestra el HP
   relevado (`VaryHPAck` traducido) y nunca el suyo. Es lo que hace Genshin con
   `EntityFightPropUpdateNotify` tras cada golpe, y además evita el problema de §4.1: un
   `VaryHPAck` inyectado que la regeneración local pisa.
   [fuente: `GameEntity.damage`]
8. **Golpes del invitado = informe de golpe que el host aplica.**
   - Genshin envía `AttackResult {atacante, defensor, daño, elemento, habilidad, dirección,
     punto}` y el servidor lo aplica **sin recalcular**.
   - Nosotros: el invitado envía `{net_id del objetivo, daño estimado, hash del ataque,
     punto de impacto, dirección}`, y el host lo aplica en su servidor con (c2)
     `VaryStatByAccInputReq` o (c3) `AttackByCollisionReq` y fuerza la reacción con (c4).
   - Para un mod entre amigos basta con fiarse del invitado, como hace Grasscutter, pero
     conviene limitar el daño por golpe para que un error no mate a un jefe de un toque.

   [fuente: `HandlerCombatInvocationsNotify`, `Scene.handleAttack`; aplicación a CD
   inferida]
9. **Aplicar los golpes después del movimiento del mismo lote.** Grasscutter primero reenvía
   el lote y después aplica los golpes. Nosotros, en el host, deberíamos procesar los
   movimientos del clon que llegan en el mismo paquete antes de su golpe, para no validar
   el golpe contra una posición vieja. [fuente: `HandlerUnionCmdNotify.java#L33-L40`]
10. **Una muerte, un evento, emitido por el host.** El invitado nunca decide que un enemigo
    del host ha muerto: espera el `NotifyHandleDeadAck` relevado y entonces reproduce la
    muerte y retira el clon del enemigo. Cuando muere un monstruo que invocó a otros, sus
    invocaciones mueren también en el host, y eso se releva igual que cualquier otra muerte.
    [fuente: `Scene.killEntity`, `EntityMonster.onDeath`]
11. **El enrutado lo decide el emisor y lleva el origen.** Cada mensaje relevado lleva
    `origen` y `destino` (el equivalente de `ForwardType`). En el receptor, lo inyectado
    se marca para que nuestro sniffer no lo vuelva a relevar (bucle de eco). El
    `t_injecting` actual lo cubre en el hilo que inyecta, pero no lo que el servidor emite
    **como consecuencia** de lo inyectado (por ejemplo, el `VaryHPAck` que genera el (c2)
    del invitado en el host). Hay que distinguir "Ack causado por un golpe remoto" de "Ack
    propio" mediante el `net_id` y el autor del golpe.
    [inferido de `ForwardType`/`forward_peer`]
12. **Neutralizar el mundo del invitado es nuestro equivalente de "salir de su mundo".**
    Genshin recarga la escena del invitado (y la del host). Nosotros:
    - al entrar en cooperativo, suprimir las peticiones de spawn del invitado (T7) y retirar
      o esconder los enemigos locales que ya existan;
    - al salir, deshacerlo todo: retirar todos los clones del host con un `REMOVE` en bloque
      y volver a permitir los spawns locales.

    Si el host se desconecta, el invitado tiene que volver solo a su mundo, igual que la
    disolución del mundo cuando el host se va en Genshin.
    [fuente: `World.removePlayer`, `MultiplayerSystem`; aplicación inferida]
13. **Misiones, botín y cofres: copiar las reglas de Genshin y aceptar la limitación.** En
    Genshin la historia, los cofres y los recursos que reaparecen son del host, y el
    invitado está "de visita". En nuestro caso el servidor del invitado no conoce a los
    enemigos del host, así que el invitado no recibe botín ni experiencia por ellos. Es
    aceptable para una primera versión, y Genshin sirve de precedente para decirlo en la UI.
    Más adelante: "botín para cada uno" (Genshin da su botín a cada jugador) solo si
    encontramos una Req de botín segura.
    [fuente: Game8, `DropSystem`; aplicación inferida]
14. **Escalado opcional del HP.** Genshin multiplica el HP por 1,5 con dos jugadores. Si el
    combate resulta demasiado fácil, se puede subir el HP máximo de los enemigos del host
    al aparecer. Es un ajuste de diseño, no un requisito. [fuente: wiki]

### 5.2 Problemas que tuvieron (o que el modelo crea) y cómo nos afectan

| Problema | Origen | Qué hacer en la opción (a) |
|---|---|---|
| **Los enemigos solo existen donde el servidor los carga.** El servidor de Genshin carga por la **unión** de las posiciones de los jugadores. El nuestro (host) solo hace aparecer cerca del **host**, porque los spawns los pide su cliente (`ActivateSpawningPoolReq`…) | [fuente: `Scene.checkGroups`; `combate-coop.md` §4.3] / [inferido] | Limitar el cooperativo a un radio alrededor del host. O bien, modo híbrido: si el invitado se aleja más de R, levantar la supresión y que vuelva a tener sus enemigos locales (opción (b) lejos, (a) cerca). Probar qué pasa en el borde |
| **La IA del host no ve al invitado.** En Genshin el cliente con autoridad tiene a los personajes de los demás como entidades reales, con tabla de amenaza (`ai_threat_map`). Nuestro clon no existe en el servidor del host | [fuente: `SceneEntityAiInfo`] / [inferido] | Es la prueba T6. Si los enemigos ignoran al clon, el invitado no recibe aggro. No hay arreglo barato: el clon no es un `ServerActor` del servidor del host. En la opción (a) no se puede traspasar la autoridad al invitado: su servidor no conoce al monstruo |
| **Quién decide que un enemigo golpeó a un jugador.** En Genshin, el golpe lo informa un cliente con `peer_id` y `frame_num` | [fuente: `EvtBeingHitInfo`] | Para nosotros, el ataque del enemigo ocurre en el PC del host. Si conecta con el clon, el host avisa al invitado, que se aplica el daño a sí mismo con `VaryStatByAccInputReq` (ya previsto en §4.3 (a)). Si no conecta con el clon, que el invitado lo detecte en su clon del enemigo (menos fiel) [inferido] |
| **Reenviar ciertos estados de movimiento corta animaciones** (`MOTION_STATE_NOTIFY`/`FIGHT`) | [fuente: `HandlerCombatInvocationsNotify.java#L96-L102`] | Filtrar en el relevo los `MoveActor`/`StartAction` que el clon no debe aplicar. Hay que identificarlos en las capturas: probablemente los que solo confirman un estado [inferido] |
| **Mensajes de un mismo suceso que llegan separados** (Grasscutter guarda la velocidad de aterrizaje y espera 200 ms al "tocar suelo") | [fuente: `HandlerCombatInvocationsNotify.handleFallOnGround`] | Agrupar en el emisor las secuencias que forman un suceso (golpe = `DoFrameEvent` + `AttackTarget` + `VaryHP` + `Dead`) y enviarlas como un solo evento de red, en vez de confiar en el orden de llegada |
| **Sin sincronizar enfriamientos ni amenaza, el traspaso de autoridad sería un reinicio** (Grasscutter deja esos handlers vacíos) | [fuente: handlers vacíos] | No intentar traspasos en la opción (a). Si algún día se hacen, el mensaje de traspaso debe llevar el estado de la IA, como `EntityAuthorityInfo` |
| **"Monstruos invisibles"** (un TODO en Grasscutter) y entidades que el cliente cree vivas y el servidor no | [fuente: `HandlerMonsterAlertChangeNotify.java`] | Reconciliar de vez en cuando: el host envía la lista de `net_id` vivos cerca del invitado, y el invitado borra los que sobran y pide la instantánea de los que le faltan |
| **Salir o entrar a mitad de carga** (Grasscutter exige que todos hayan cargado para salir) | [fuente: `MultiplayerSystem.leaveCoop`] | No empezar ni terminar el relevo de enemigos mientras alguno de los dos está en pantalla de carga (teletransporte, muerte) |
| **Botín a nombre del "jugador 0"** | [fuente: `DropSystem.java#L102-L104`] | Recordatorio de que el dueño del botín debe ser explícito en nuestro protocolo (`owner_peer`) |

### 5.3 Orden recomendado (encaja con B2 de `combate-coop.md` §5)

1. Protocolo: `net_id` asignado por el host, `authority_peer`, motivo de aparición, `seq`
   por entidad y evento de muerte con el último `seq` (subir `kVersion`).
2. Instantánea `MEET` al conectar y `BORN`/`REMOVE`/`DIE` en vivo, solo con enemigos dentro
   del radio R del host.
3. Supresión de spawns en el invitado (T7) y limpieza simétrica al salir o al desconectarse.
4. Relevo de movimiento y acción con descarte por `seq` y corrección periódica.
5. HP y muerte solo desde el host.
6. Golpes del invitado aplicados en el host como "informe de golpe" ((c2)/(c3) + (c4)), con
   límite de daño por golpe.
7. Medir T6 (¿la IA del host ataca al clon?). Si no lo hace, documentar que la opción (a)
   es "pegar juntos sin aggro hacia el invitado" y dejar el aggro como limitación conocida.
