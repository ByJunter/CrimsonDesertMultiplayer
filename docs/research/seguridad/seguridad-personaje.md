# Revisión de seguridad: camino del personaje del otro jugador

Revisión de solo lectura (no se ha tocado el repo). Código en `main` @ `fa89068`. Build del juego
1.0.0.2976. Los números de línea son de ese commit.

**Modelo de amenaza:** el otro jugador es un amigo de Steam con un mod modificado que envía
mensajes del protocolo bien formados pero con el contenido que quiera. Todo lo que llega a un
handler del juego (que confía en su servidor) es superficie de ataque.

**Alcance:**
- `coop.{h,cpp}`, `remote_puppet.{h,cpp}`, `equip_state.h`, `time_sync.{h,cpp}`;
- `puppet_minimap.cpp`, `hud.cpp`, `dx12_hook.cpp`;
- `offsets.h` (`kRelayedAcks`) y los documentos de `docs/research` citados.

Del transporte y de `protocol.cpp` solo se ha mirado lo necesario para saber qué llega validado.

## Contexto: qué llega validado y en qué hilo corre cada cosa

- **`proto::decode` (`protocol.cpp`):**
  - todo `f32` del protocolo propio tiene que ser finito: `PlayerState`, `SpawnInfo.pos/rot/horse_pos`;
  - nombre de 32 bytes como máximo, saneado a ASCII imprimible;
  - payload de `GameEvent` de 1024 bytes como máximo;
  - mensaje fiable de 256 KB como máximo.
- **Lo que no se valida:** los **floats y campos de dentro de los payloads del juego** (Move,
  StartAction, Rotate, WallUp, Docking, Equip, Relay y las plantillas `ActorLoadData`) son bytes
  crudos sin comprobar.
- **`decode_spawn`:** `user`/`body`/`equip`/`horse` ≤ 0xFFFF bytes; `user` y `body` ≥ 256 bytes;
  `horse` ≥ 256 bytes o vacío.
- **Hilos:**
  - `CoopLink`, `puppet_event`, `puppet_spawn`, `merge_equip` y `time_sync_on_event` corren en el
    **hilo de red del mod**.
  - Lo encolado se entrega en el **NetworkThread del juego**, dentro del hook de
    `ClientNet::OnRecvPacket` (`troc_sniffer.cpp:213-247`).
  - De ahí los datos pasan a los hilos de animación, física y render del juego. Un fallo en
    cualquiera de ellos cierra el juego y el informe va a Pearl Abyss.
- **`guarded_inject` (`troc_sniffer.cpp:204-211`):** captura con SEH las excepciones
  *síncronas* del handler, pero no las diferidas a otros hilos (precedente: el crash del caballo
  en `exe+0x1F63FE6`, en otro hilo).
- **Sin límite de ritmo:** `ClientSession::on_datagram` (`client_session.cpp:166-172`) no limita
  cuántos `GameEvent` por segundo acepta.

---

## Hallazgos (de mayor a menor gravedad)

### H1 · CRÍTICA. Plantillas `ActorLoadData` del otro jugador inyectadas casi sin validar (contenido y claves)
- **Dónde:**
  - `remote_puppet.cpp:737-811` (`puppet_spawn`): `replace_u32` en 745-751, posición en 778-786,
    registros del cuerpo en 792-808 y encolado en 810-811;
  - `remote_puppet.cpp:678-735` (`spawn_horse`; `horse=1` viene **activado** en los perfiles
    públicos `kit/public/cdmp.ini.*`).
- **Qué se comprueba hoy:**
  - el tamaño (≥ 256 bytes, ≤ 0xFFFF);
  - que se pueda escribir la posición a `size-154` y la rotación a `size-142`;
  - que el cuerpo sea una cadena `[u16 len][registro]`; si no lo es, **se envía entero igual**
    (línea 806);
  - se sustituyen **solo las tres claves que el otro jugador declara** en `SpawnInfo`
    (`user_key`, `player_key`, `body_key`, `horse_key`).
  - No se valida nada más del contenido: ~31 KB que el deserializador de `ActorLoadDataAck` del
    juego trata como datos del servidor.
- **Ataque A (claves):**
  - En cada partida las claves son serials del mundo, así que muy probablemente coinciden entre
    los dos PCs. Ejemplos: usuario `0x90100000`, jugador `0xA0100001`, caballo `0xB0100003`
    ("el caballo siempre es `0xB0100003`", `caballo-coop.md` §2).
  - El atacante declara `player_key`/`body_key`/`user_key` falsos (p. ej. `0x12345678`) y deja
    en la plantilla las claves reales. Nuestro mod no reescribe nada e inyecta un
    `ActorLoadDataAck` con **nuestras propias claves**:
    - registros con dueño = nuestro usuario local `0x90100000`;
    - un registro de cuerpo (tipo 7, dueño 0) con la clave de **nuestro** cuerpo o caballo.
  - Según `caballo-coop.md` §8.3:
    - con dueño = usuario local el cliente comprueba si el actor ya existe;
    - con **dueño 0** la fábrica `0x4631C0` "solo prohíbe el tipo 1" y **no consta que
      compruebe duplicados**;
    - con dueño ≠ 0 el actor se mete en `slot[índice]` del contenedor de hijos **de ese usuario**.
  - Un actor ajeno quedaría colgado de nuestro usuario (como mercenario o montura nuestra), o
    habría claves duplicadas en el gestor de actores. Resultado: estado corrupto del cliente,
    peticiones C>S sobre esos actores (`OnDemandEquipDockingGimmickSpawnReq`, `RegionInOutReq`,
    `UpdateGimmickReq`, `ClientCacheReq`; `apariencia-estado-coop.md` §5.1) y cierres.
- **Ataque B (contenido):**
  - Cualquier campo de los componentes viaja tal cual: tipo de actor (`+0x0D`), índice en el
    contenedor (`+0x16`), personalización, lista de equipo con `itemNo` iguales a los de
    **nuestro inventario** (§5.3.2), u32 de longitud internos, etc.
  - Es la mayor superficie de memory corruption: un deserializador complejo, escrito para datos de
    confianza, recibe el blob completo controlado por el otro jugador.
- **Arreglo:**
  1. **Parsear y validar la estructura** de las cargas de jugador y caballo, no solo la del
     cuerpo.
     - `user`, `body` y `horse` tienen que ser una cadena exacta `[u16 len][registro]` que
       consuma todo el buffer. Si no, **rechazar** el spawn en vez de "sent whole".
     - Cabeceras exigidas:
       - registro 0 = tipo 0 con clave `kPuppetUser`;
       - jugador = tipo 1, clave `kPuppetPlayer`, dueño `kPuppetUser`;
       - cuerpo = tipo 7, clave `kPuppetBody`, dueño 0, índice `0xFF`;
       - caballo = tipo 5, clave `kPuppetHorse`, dueño `kPuppetUser`.
     - Comprobarlo **después** de reescribir las claves.
  2. **Validar las claves declaradas:** `keys::is_class(user_key, kClassUser)`,
     `is_class(player_key, kClassPlayer)`, `is_class(body_key, kClassWorld)`, y que cada una
     aparezca en su cabecera *antes* de reescribirla.
  3. **Barrera de claves como la de los enemigos** (`enemy_relay.cpp:1053-1068`): tras reescribir,
     recorrer el blob y **rechazar** si queda cualquier `keys::is_actor_key(k)` que no sea de la
     marioneta y que `game::find_actor(k)` encuentre en nuestro mundo. Esa barrera ya existe para
     las copias de enemigos, pero **no** para el personaje ni para el caballo.
  4. Reescribir o anular los `itemNo` de la lista de equipo de la plantilla (§5.3.2), o al menos
     comprobar que no coinciden con los de nuestro inventario.
  5. Si se puede, comparar el blob con una plantilla local de la misma build (longitudes fijas por
     componente) y rechazar desviaciones de tamaño.

### H2 · ALTA. Si el número de personaje no está donde se espera, se carga igual con el del otro jugador (corrompe nuestro equipo local)
- **Dónde:** `remote_puppet.cpp:768-771` (`set_character_no`: si falla, solo hay un `log::warn` y
  la carga sigue), `remote_puppet.cpp:58-84`.
- **Ataque:**
  - El parche solo se aplica si los dos campos valen `0x7FFFFFFFFFFFFFFF` (el valor que, según
    `apariencia-estado-coop.md` §5.3, comparte el jugador local).
  - El atacante deja `+0x17 = 0x7FFF…` y cambia `+0x43`: el parche no se aplica y la copia entra
    con el **mismo characterNo que nuestro jugador**.
  - Efecto documentado en la prueba en vivo 1: la caché global `EquipSlotListCache` se mezcla, el
    cliente local deja de aplicar sus propios `EquipItemAck`/`UnequipItemAck` y aparece
    `FailEquipItemAck`. Es una desincronización de nuestro inventario y equipo con nuestro
    servidor, y la marioneta lanza un `ClientCacheReq` con ese número.
- **Arreglo:** si `set_character_no` falla, **no cargar** (devolver `false` y descartar ese
  `SpawnInfo`). Comprobar además que no queda ningún u64 `0x7FFFFFFFFFFFFFFF` en las posiciones
  conocidas de usuario y cuerpo (`+0x1839`/`+0x1865`).

### H3 · ALTA. Floats de los payloads del juego sin validar (NaN, Inf, valores enormes) hacia física y animación
- **Dónde:**
  - `move_ack` (`remote_puppet.cpp:296-307`), `start_action_ack` (311-334), `rotate_ack`
    (337-347) y `wallup_ack` (351-359);
  - `ride_horse` (1125-1148), que solo comprueba `isfinite(x)` y `x,z != 0`;
  - `fill_gap_before` (438-534);
  - spawn: `pos` es finito pero puede ser 3e38 (778).
- **Ataque:**
  - `MoveActorReq` con posición, quaternion, escala (`+0x24` Transform, `+0x68`), velocidades
    (`+0x74`, `+0x80`), Transform de mundo (`+0x94`) y cola (`+0xDE`) a NaN, ±Inf, 1e38 o
    escala 0.
  - `shift_x` respeta los no finitos, pero nada los rechaza: el cliente los escribe en
    `[[actor+0x68]+0x1A0]+0x534/+0x540` e interpola (`mirror-ack-formats.md` §1).
  - Los NaN en broadphase o física y los quaternions no normalizados son una causa clásica de
    *assert*/AV **en hilos del juego** que `guarded_inject` no cubre. Esos cierres van a Pearl
    Abyss.
- **Arreglo:** función `sane_move(...)` aplicada **antes** de construir el Ack (y en
  StartAction, Rotate, WallUp y en el caballo):
  - todos los floats finitos;
  - posiciones dentro de los límites del mundo (p. ej. `|x|,|z| ≤ 20 000`,
    `-2 000 ≤ y ≤ 5 000`; ajustar al mapa);
  - quaternions con `|q|≈1` (normalizar o rechazar);
  - escala ≈ (1, 1, 1);
  - velocidades `≤ ~100 m/s`;
  - salto respecto al Move anterior acotado (≤ 15 m, como `kGapMaxJump`, salvo teleport
    señalizado).
  - Aplicar lo mismo a `SpawnInfo.pos/horse_pos` y a la posición de `remote_state` que se reutiliza
    para recargar (`coop.cpp:174, 182, 197`).

### H4 · ALTA. Cola de inyección sin límite y vaciado completo en el NetworkThread del juego (congelación y memoria)
- **Dónde:**
  - `enqueue_at` (`remote_puppet.cpp:216-222`): `std::deque` sin tope e inserción ordenada O(n);
  - `troc_sniffer.cpp:221` (`while (puppet_take_injection(pkt))`): entrega **todo** lo que ha
    vencido en una sola llamada a `OnRecvPacket`;
  - no hay límite de ritmo por tipo de evento (`client_session.cpp:166-172`, `coop.cpp:316-359`).
- **Ataque:**
  - Inundar con `Equip` (`(1024-9)/6 = 169` objetos cada uno; **no** es movimiento, así que nunca
    se descarta por viejo), `Relay` y `Docking`.
  - Usar `Move` con huecos falsos (`ts += 3000`, saltos de 10 m): `fill_gap_before` amplifica cada
    Move ×59 (511-528).
  - Mandar `Spawn` repetidos.
  - La cola crece sin límite y, al vencer, el NetworkThread del juego ejecuta miles de handlers
    seguidos. Por ejemplo, cada `OtherEquipItemAck` **crea objetos de ítem nuevos**
    (`apariencia-estado-coop.md` §5.2), y `next_equip_serial` garantiza que todos pasen la puerta
    de serie.
  - Resultado: el juego se congela, la memoria se agota y cualquier cierre en esa situación es
    reportable.
- **Arreglo:**
  1. Tope de cola (p. ej. 4096 paquetes o 4 MB). Al superarlo, descartar lo nuevo que no sea
     carga ni retirada.
  2. Máximo de inyecciones por llamada a `OnRecvPacket` (p. ej. 32): el resto sale en la
     siguiente.
  3. Límite de ritmo por tipo en `CoopLink::on_event`/`puppet_event`:
     - Move ≤ 60/s, StartAction ≤ 30/s, Rotate ≤ 60/s;
     - Equip ≤ 4/s y ≤ 32 objetos por mensaje;
     - Relay ≤ 20/s, Docking ≤ 10/s.
  4. `fill_gap_before`: como mucho un relleno cada `kGapMaxMs` y como mucho N muestras por
     segundo.
  5. Spawn: aceptar un `SpawnInfo` nuevo como mucho cada 10 s (`coop.cpp:361-373`).

### H5 · ALTA. Equipo: `itemKey`, ranura y marcador sin validar; crecimiento cuadrático; longitudes truncadas a u16
- **Dónde:**
  - `puppet_event` Equip (`remote_puppet.cpp:1333-1354`);
  - `merge_equip` (`coop.cpp:34-40, 303-310`);
  - `EquipState::apply_items` (`equip_state.h:51-57`, bucle O(n²));
  - spawn (`remote_puppet.cpp:838-859`);
  - `make_packet` (`remote_puppet.cpp:131-139`) y `troc_sniffer.cpp:225` (`static_cast<uint16_t>`).
- **Ataques:**
  1. **Contenido:** `itemKey` arbitrarios (inexistentes) y ranuras u16 arbitrarias.
     - `0x99F500` llama a `0x38AAC0(itemKey)` y crea el ítem a partir de los datos estáticos. Con
       una clave inexistente es probable que acabe en una desreferencia nula o en un índice fuera
       de tabla en el NetworkThread o en el render del arma.
     - El byte 0 (`0x04`) del payload tampoco se comprueba en `puppet_event`.
  2. **Crecimiento:** cada `Equip` fiable se fusiona en `last_spawn_`/`remote_spawn_`. Con ranuras
     distintas (hay 65 536) el snapshot crece hasta ~131 000 entradas (~786 KB).
     - `apply_items` recorre la lista completa por cada ranura nueva: ~10⁹–10¹⁰ operaciones por
       mensaje, lo que bloquea el hilo de red del mod y hace caer el enlace.
     - En la siguiente recarga, `puppet_spawn` encola **hasta 65 536 `OtherUnequipItemAck` con el
       mismo `due`**, que se inyectan en una sola pasada (H4).
     - También encola un `OtherEquipItemAck` de hasta ~393 KB cuyo `u16 len` y cuyo `size` pasado
       a `OnRecvPacket` se truncan los dos módulo 65 536. Siguen cuadrando entre sí
       (`size-5 == len`), así que el juego parsea un payload recortado.
  3. **Serie:** `next_equip_serial(0xFFFFFFFF)` deja `g_equip_serial_out = 0xFFFFFFFF`; la
     siguiente vuelve a 0 y la puerta (comparación con signo) rechaza todo lo posterior. Solo
     afecta a la apariencia de la copia, pero es trivial.
- **Arreglo:**
  - Exigir `payload[0] == 0x04`.
  - Ranura dentro del rango real de ranuras de equipo (p. ej. `< 64`; sacarlo de la tabla del
    juego).
  - `itemKey` validado contra la tabla de ítems: llamar a `0x38AAC0` en solo lectura con SEH, o
    usar una lista blanca creada al cargar.
  - ≤ 32 objetos por mensaje y ≤ 64 ranuras en `EquipState`; rechazar y no fusionar el resto.
  - `apply_items` en O(n) (precalcular un `set` de ranuras con retirada).
  - Limitar la serie a `sender_serial ≤ last + 1000` y evitar el desbordamiento.
  - `make_packet` tiene que **rechazar** payloads de más de `0xFFFF - 5` bytes; no truncar nunca
    a u16.

### H6 · ALTA. Claves de actores reales en campos que no se limpian (confusión de claves en Move y StartAction)
- **Dónde:** `move_ack` (`remote_puppet.cpp:296-307`), `start_action_ack` (311-334),
  `rotate_ack`, `wallup_ack`, Docking (1387-1393).
- **Qué hay:** `mirror-ack-formats.md` §1-§2 documenta claves de actor en:
  - Move `+0x20` ("a veces una clave `0xB01…`");
  - StartAction Ack `+0xAB` (el `+0x1C` de MoveData) y Ack `+0x169` (el primer u32 tras MoveData).
- **Qué se limpia hoy:** solo el padre (`+0x8C`, `+0x117`, `+0x54`) y el objetivo (`+0x16D`).
  `has_foreign_key` solo se aplica a los Relay.
- **Ataque:** poner en esos campos la clave de **nuestro** jugador, de nuestro caballo o de un NPC
  real. Además:
  - `clear_parent` deja pasar `kPuppetHorse` aunque el caballo esté desactivado;
  - `puppet_event` solo comprueba la clave de `+0`.
  - El cliente vincula la copia a un actor real nuestro (plataforma, objetivo, montura), con un
    efecto desconocido sobre nuestro personaje: lo arrastra o lo ancla, o el estado cambia.
- **Arreglo:**
  - Limpiar o anular `+0x20` (Move) y `+0xAB`/`+0x169` (StartAction) si contienen una clave que no
    sea de la marioneta.
  - Mejor aún: aplicar `has_foreign_key` a **todos** los Ack construidos (Move, StartAction,
    Rotate, WallUp, Docking, Equip) después de la traducción, igual que a los Relay, y descartar.
  - Ampliar `has_foreign_key` a las claves locales del cliente (bit 30) y a generaciones de más de
    64 de las clases 1-3, o contrastar con `game::find_actor`.
  - `clear_parent` solo debe conservar `kPuppetHorse` si `g_horse_spawned`.

### H7 · MEDIA. Relays de tamaño libre y contenido arbitrario hacia handlers del juego
- **Dónde:** `offsets.h:173-189`. `NotifyHandleFatalAck` (2899), `NotifyHandleReviveAck` (3053) y
  `ReviveAck` (2475) tienen `size = 0`, así que se acepta cualquier tamaño hasta 1022 bytes
  (`remote_puppet.cpp:1366`).
- **Ataque:**
  - Cuerpos de longitud arbitraria a esos deserializadores.
  - En el resto de opcodes el tamaño es fijo, pero los valores no se validan: id de buff y nivel
    (`ApplyBuffAck`), datos de `CharacterCustomizationAck` (279 bytes, índices de apariencia),
    elementos.
  - Los índices fuera de tabla en apariencia o buffs se consumen en los hilos de
    render/animación, lejos de la red y del SEH.
- **Arreglo:**
  - Medir los tamaños reales en las capturas y fijarlos (o fijar un máximo estricto).
  - Valorar quitar `CharacterCustomizationAck` de la lista hasta poder validar sus rangos.
  - Para `ApplyBuff`/`UnapplyBuff`, comprobar que el id de buff existe en la tabla del juego.

### H8 · MEDIA. Pin del mapa con la posición del otro jugador sin acotar
- **Dónde:** `coop.cpp:237-280` y `remote_puppet.cpp:1088-1110`.
- **Ataque:**
  - `PlayerState.pos` es finito pero puede valer ±3e38, e incluso `y` sin acotar.
  - Se convierte en `CreateOrChangePinMarkerAck` con el **dueño = nuestro jugador**.
  - La interfaz del mapa convierte las coordenadas a celdas o texturas. Con valores enormes puede
    haber conversiones float→int fuera de rango y un índice OOB en la interfaz.
  - El documento dice que estos pins no llegan al servidor ni al guardado, lo cual está bien.
- **Arreglo:** limitar `where` a los límites del mundo (los mismos de H3) y no crear el pin si está
  fuera.

### H9 · MEDIA. Peticiones C>S provocadas indirectamente por datos del otro jugador
- **Dónde:** efecto de H1, H2 y H5.
  - Cargar o equipar la copia hace que **nuestro cliente** envíe a **nuestro servidor**:
    `ClientCacheReq("cacheEquipSlotList", characterNo)`, `OnDemandEquipDockingGimmickSpawnReq`
    (ranuras de la copia), `RegionInOutReq` y `UpdateGimmickReq` (`apariencia-estado-coop.md`
    §5.1, §1.4).
  - La propia documentación avisa: "No guardar la partida con la marioneta presente".
- **Ataque:** con plantillas o equipo manipulados, el otro jugador decide parte del contenido de
  esas peticiones a nuestro servidor, que es el que guarda.
- **Arreglo:**
  - Los arreglos de H1, H2 y H5 cierran la mayor parte.
  - Además, valorar filtrar en `on_post_packet` (`troc_sniffer.cpp:157`) las peticiones C>S que
    llevan claves de la marioneta (`0x9010F001`, `0xA010F001`, `0xB010F00x`) para que nunca lleguen
    al servidor.

### H10 · BAJA. Sincronía de hora: el otro jugador mueve nuestra hora y se guarda
- **Dónde:** `time_sync.cpp:146-170`. El perfil `invitado` trae `time_sync=2`.
- **Qué está bien:** `nearest_raw_with_time_of` limita el cambio a ±½ día (el día nunca salta),
  la escala y el anchor son nuestros y hay como mucho un `SetFieldTimeReq` cada 15 s.
- **Ataque:** alternar día y noche cada 15 s indefinidamente; el servidor lo guarda con el campo.
  No hay corrupción, pero sí molestia y una hora arbitraria en el guardado.
- **Arreglo:** aplicar solo si el valor del otro es coherente con su `seq` (que avance como un
  reloj, ±tolerancia entre mensajes); como mucho un ajuste cada 60 s; ignorar los saltos hacia
  atrás no explicados.

### H11 · BAJA. Inundación del log (disco y E/S en el hilo de red)
- **Dónde:** se escribe una línea por evento sin limitar en:
  - `remote_puppet.cpp:1407` (tamaño inesperado), 1343 y 1353 (equipo);
  - `coop.cpp:365` y `368-370` (cada Spawn);
  - `time_sync.cpp:167`.
- `log.cpp` no rota el archivo ni tiene tamaño máximo.
- **Ataque:** miles de eventos con tamaño erróneo por segundo hacen crecer `bin64\cdmp.log`
  (GB/hora).
- **Arreglo:** contador y log 1 de cada N (como `g_relays_dropped`), y un tope de tamaño de
  `cdmp.log`.

### H12 · BAJA. `ClockMapper` O(n) por muestra con ritmo libre
- **Dónde:** `clock_mapper.h:47-50`.
- **Ataque:** una inundación de Moves hace que la ventana de 4 s tenga cientos de miles de
  muestras, y cada `add` recorre todas. Gasta CPU en el hilo de red del mod.
- **Arreglo:** tope de muestras (p. ej. 512) o un deque monótono para el mínimo; se arregla
  también con el límite de ritmo de H4.

### H13 · BAJA. Desbordamiento con signo (UB) en las marcas de tiempo
- **Dónde:** `rebase_timestamp` (`remote_puppet.cpp:285-291`), `action_hold_for` (412) y
  `fill_gap_before` (477-480).
- **Ataque:** `ts = 0x7FFF…` hace que `int64 + shift` desborde: UB en C++ (en MSVC da la vuelta).
  `InjectClock::stamp` acota luego Move, Rotate y StartAction a [now-500, now+450], así que hoy
  no hay efecto práctico.
- **Arreglo:** descartar los eventos cuya marca diste más de, p. ej., 1 h de la última, o hacer la
  aritmética en `uint64` con saturación.

### H14 · INFO. La cola ordenada de Equip/Relay se puede bloquear
- **Dónde:** `coop.cpp:375-388`.
- **Qué pasa:** está acotada a 256 entradas de ≤ 1 KB, lo cual está bien. Pero el otro jugador
  puede dejarla atascada: manda `seq = ordered_in_+1…+256` y nunca `ordered_in_`. A partir de ahí
  no se aplica ningún relay ni equipo más.
- **Impacto:** solo pierde funciones él mismo, sin daño. Opcional: si hay un hueco de más de 5 s,
  saltarlo.

### H15 · INFO. Overlay (`hud.cpp`, `dx12_hook.cpp`): sin riesgo de cadena de formato
- El nombre del otro jugador llega saneado (ASCII imprimible, ≤ 32 bytes; `protocol.cpp:67-80`) y
  se pinta con `"%s"` (`hud.cpp:118, 125`) o `AddText` (48).
- HP y distancia pasan por `%.0f` y `snprintf` con tamaño (54). `std::clamp` de la fracción (38)
  cubre `inf`.
- No se pueden recibir NaN (`Reader::f32`). Una posición de 1e38 solo produce coordenadas de
  pantalla absurdas.
- `dx12_hook.cpp` no pinta ningún dato del otro jugador.
- Mejora opcional: no dibujar el marcador si `remote->pos` está fuera de los límites del mundo.

---

## Lo que ya está bien hecho
- **Protocolo propio robusto** (`protocol.cpp`):
  - lectura campo a campo con comprobación de límites y `at_end()`;
  - floats no finitos rechazados;
  - nombre saneado;
  - topes de payload (1024), de mensaje fiable (256 KB) y de fragmentos;
  - `decode_spawn` limita los blobs a ≤ 0xFFFF y exige ≥ 256 bytes, así que los offsets
    `kLoadPositionFromEnd = 154` y `kLoadRotationFromEnd = 142` nunca se salen.
- **Tamaños exactos** en Move (514/0xEA), StartAction (725), Rotate (176), WallUp (132) y
  Docking (16), y tamaños fijos para la mayoría de relays. Todas las lecturas de payload están
  dentro de los límites que se comprueban; no hay `memcpy` con offset o longitud controlados por el
  otro jugador.
- **La clave de `+0` siempre se fuerza** a la de la marioneta: no se puede dirigir un
  Move/Action/Equip/Relay a nuestro jugador cambiando la clave principal (`puppet_event`, 1220-1224).
- **Lista blanca de opcodes de relay** (`kRelayedAcks`), con campos de clave secundaria que se
  reescriben y `has_foreign_key`, que descarta el mensaje si queda cualquier otra clave.
- **Validaciones concretas:**
  - el cuaternión del spawn se usa solo si `|q|≈1`;
  - `shift_x` respeta los no finitos;
  - `clear_parent` y la limpieza del objetivo de StartAction;
  - `InjectClock` acota las marcas de tiempo de movimiento.
- **Inyección en el hilo correcto,** con `TryAcquireSRWLockExclusive` (nunca bloquea el
  NetworkThread), SEH en `guarded_inject` y **desactivación de la inyección tras la primera
  excepción**.
- **Hooks sobre el juego con SEH y sin escrituras arriesgadas:** `puppet_minimap`,
  `owner_of_method`, guardas de capas.
- **Colas acotadas:** `outbox_` (4096), `ordered_held_` (256), `held_enemy_spawns_` (64) y el
  canal fiable (`kMaxPartials`, `kMaxAhead`).
- **Pins del mapa** solo del lado del cliente (no llegan al guardado), con un grupo fijo
  (`kPinGroupNormal`), lo que evita la escritura fuera de las dos listas documentada en
  `offsets.h:117-119`.
- **Sincronía de hora:** desactivada por defecto en el código, limitada a ±½ día y a una petición
  cada 15 s.
- **Copias de enemigos:** su camino ya tiene una barrera de claves contra nuestro mundo
  (`enemy_relay.cpp:1053-1068`). Es el modelo que hay que copiar en H1.

## Orden recomendado de arreglos
1. **H1 + H2:** validar la estructura y las claves de las plantillas (cabeceras de registro,
   clases de clave, barrera `find_actor`) y **rechazar** el spawn si `set_character_no` falla.
2. **H3:** `sane_move`/`sane_transform` en todos los Ack de movimiento y en las posiciones de spawn
   y recarga.
3. **H4 + H5:** límites de ritmo por tipo, tope de cola, máximo de inyecciones por pasada, y
   equipo con ranuras e ítems validados, ≤ 32 objetos y `apply_items` O(n). `make_packet` debe
   rechazar más de 0xFFFA bytes.
4. **H6:** `has_foreign_key` en todos los Ack, y limpiar Move `+0x20` y StartAction `+0xAB`/`+0x169`.
5. **H7 a H9:** tamaños fijos para los relays de tamaño libre, límites del mundo para el pin y
   filtrado de las C>S que lleven claves de la marioneta.
6. **H10 a H13:** robustez menor (hora, log, `ClockMapper`, UB).

Tests sugeridos (`tests/tests.cpp`):
- `decode_spawn` + validador con plantillas manipuladas: claves no declaradas, dueño = usuario
  local, cadena de registros rota, characterNo distinto;
- `EquipState` con 10 000 ranuras (que tarde poco);
- `move_ack` con NaN o 1e38 (rechazado);
- `make_packet` con más de 65 530 bytes (rechazado).
