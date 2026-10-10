# Revisión de seguridad — capa de red (cdmp.asi)

Alcance: `src/common/protocol.*`, `reliable.*`, `client_session.*`, `udp.*`, `byte_ring.h`,
`src/mod/net/steam_transport.*`, `src/mod/mod.cpp` (`run_network`), `tests/tests.cpp`, y el
primer salto a `coop.cpp`/`log.cpp`/`hud.cpp` donde lo que llega de la red se usa.
Modelo de amenaza: el otro jugador es un amigo de Steam con un mod modificado que envía bytes
arbitrarios. Revisión de solo lectura; no se ha tocado el repo.

Resumen: **no he encontrado corrupción de memoria ni ejecución de código en la capa de red.** El
decodificador es sólido (lector con límites, sin `memcpy` de structs, comprobación de `at_end`,
floats finitos). Los problemas son de **consumo de memoria sin límite (OOM del juego)**,
**amplificación de log/CPU**, y **ocupación del puesto de anfitrión**.

---

## ALTA

### A1. `ReliableChannel::ready_` sin límite: ~1,1 GB (o más) de memoria en el proceso del juego
- **Dónde:** `src/common/reliable.cpp:94` (único límite: `kMaxAhead = 4096`), `:122-127`
  (los mensajes completos pasan a `ready_` y liberan su hueco en `incoming_`),
  `src/common/reliable.h:35-36`; `src/common/protocol.cpp:292-295` (permite `count` hasta 239).
- **Ataque:** tras el handshake, el par manda `RelData` con su stream `S` y **nunca** manda el
  mensaje `msg_id = S` (el que espera `rx_next_`). Para cada `msg_id` de `S+1` a `S+4095` envía
  239 fragmentos de 1100 bytes (`count=239`, `index=0..238`). Cada mensaje se completa
  (`p.have == p.count`), sale de `incoming_` (así `kMaxPartials = 64` nunca frena) y se queda en
  `ready_` esperando a `rx_next_`, que nunca llega. 4095 × 262 900 B ≈ **1,08 GB** de datos, más
  la holgura de capacidad del `insert` en `m.bytes` (crecimiento geométrico, hasta ~1,5×), más
  ~17 MB de parciales. Todo con acuse (`RelAck`), así que el emisor malicioso sabe que se quedó.
  El juego ya usa mucha RAM: el resultado probable es un fallo de asignación en un hilo del
  juego → **crash reportado a Pearl Abyss**; o un `std::bad_alloc` en el worker (capturado en
  `mod.cpp:748`, que apaga la red). Coste para el atacante: ~1 GB por Steam (minutos a pocos MB/s;
  el atacante controla su `SendRateMax`). Variante: al mandar por fin `msg_id = S`, se entregan
  4096 mensajes en una sola llamada (pico de CPU en `decode_spawn`/manejadores).
- **Además:** `decode` acepta `count*kRelChunk ≤ kMaxRelMessage + kRelChunk`, o sea mensajes de
  hasta 262 900 B > `kMaxRelMessage` (262 144). Inofensivo, pero el límite no es el declarado.
- **Arreglo:**
  1. Presupuesto de bytes para el receptor: `held_bytes_` = suma de `chunk.size()` en
     `incoming_` + `bytes.size()` en `ready_` (actualizar al insertar/borrar). Si un fragmento
     nuevo excede p. ej. `kMaxHeldBytes = 8 MB` (≈30 spawns completos) o
     `incoming_.size() + ready_.size() >= kMaxHeld` (p. ej. 256), **rechazar sin acusar**
     (`++refused_; return out;`), igual que hoy con `kMaxPartials`.
  2. Eximir de ese presupuesto y de `kMaxPartials` al mensaje `msg_id == rx_next_` (así el
     receptor nunca se bloquea: el siguiente mensaje debido siempre cabe).
  3. En `decode`: `if (static_cast<size_t>(b.count) * kRelChunk > kMaxRelMessage + kRelChunk - 1)`
     o, mejor, `count <= (kMaxRelMessage + kRelChunk - 1) / kRelChunk` y, en `on_data` al
     completar, descartar si `m.bytes.size() > kMaxRelMessage`.
  4. Test: 4095 mensajes completos adelantados → `ready_` no pasa del presupuesto y el mensaje
     `rx_next_` sigue entrando.

---

## MEDIA

### M1. Amplificación de log y de trabajo por paquete: log sin tope, `FlushFileBuffers` por línea
- **Dónde:** `src/mod/log.cpp:53-69` (cada línea hace `WriteFile` + `FlushFileBuffers`, sin
  límite de tamaño); fuentes disparables por el par:
  `src/common/client_session.cpp:107-127` (cada `Hello` con **nonce nuevo** → `check_build`
  (`:92`, aviso si `mod_build` difiere) + "peer reconnected" + `reliable_.reset()` + `++epoch_`),
  `:140-148` (cada `Welcome` con **nombre distinto** estando conectado → "peer is now" + reset +
  `++epoch_`), `src/mod/net/coop.cpp:365` y `:368` (un aviso por cada mensaje `Spawn`
  malformado/recibido), `coop.cpp:103-119` (cambio de epoch → `forget_partner` →
  `puppet_remove()` en el juego y reenvío de nuestro Spawn).
- **Ataque:** el par alterna `Hello{nonce=i++, mod_build=random}` (≈40 B) a todo lo que dé Steam.
  Por cada uno: 2-3 líneas de log con `FlushFileBuffers` (cada flush es una escritura síncrona al
  disco, ~ms), reset del canal fiable y nuevo epoch. Una vez por iteración del bucle de red,
  CoopLink quita el personaje copiado (`puppet_remove`, que actúa sobre el juego), y vuelve a
  codificar y encolar nuestro Spawn (~40-100 KB, hasta 128 fragmentos × 1,2 KB salen al instante
  → amplificación ~1000-3500× hacia el atacante, limitada por Steam). Resultado: `cdmp.log`
  crece sin límite (GB/hora; el disco del jugador se llena), el hilo worker se queda atascado en
  flushes (co-op muerto), y el motor recibe un bucle de quitar/cargar la copia (estrés de código
  del juego que ya se ha visto frágil). Lo mismo con un `Spawn` malformado por fragmento
  (`coop.cpp:365`). Sólo el aviso de *otra versión* está limitado (cada 10 s,
  `client_session.cpp:76`).
- **Arreglo:**
  1. `ClientSession`: aceptar un **nonce nuevo** como reinicio como mucho una vez cada N s
     (p. ej. 3 s) — si llega antes, contestar `Welcome` pero no resetear ni subir epoch; contar
     en `stats_.rejected`.
  2. Rol `Responder`: ignorar `Welcome` (ver B2). Rol `Initiator`: el cambio de nombre por
     `Welcome` sólo tiene sentido con el relé `cdmp_peer` (UDP); con Steam, ignorarlo.
  3. `log`: helper con límite de frecuencia por clave (p. ej. `log::info_ratelimited(key, …)`
     ≤1/s con contador de suprimidos) para todo lo que dependa de datos del par; **tope de
     tamaño** del fichero (p. ej. 64 MB → escribir una última línea "log truncado" y dejar de
     escribir); `FlushFileBuffers` sólo en `Warn/Error` o cada ~250 ms (el crash‑safety se
     mantiene con flush periódico y en `Error`).

### M2. Puesto de anfitrión bloqueado para siempre por un `Hello` de otra versión; y "el primero que llegue"
- **Dónde:** `src/mod/mod.cpp:295-304` (se fija `peer` con cualquier `Hello` decodificable,
  **incluida otra versión de protocolo**: `decode` la devuelve como `Hello`,
  `protocol.cpp:225-232`) y `:310-313` (sólo se suelta `peer` si `was_connected` era cierto).
- **Ataque 1 (permanente):** con `peer_steamid` sin configurar (`accept_from_ = 0`), cualquier
  amigo que esté jugando envía **un solo** datagrama `Hello` con `version = 5`. El anfitrión fija
  `peer` = ese amigo; la sesión lo rechaza (otra versión) y se queda en `Connecting`;
  `was_connected` nunca pasa a true → `peer` no se suelta jamás. El amigo legítimo queda fuera
  hasta reiniciar el juego. Ocurre también sin malicia (un amigo con una versión vieja del mod).
- **Ataque 2 (carrera):** el atacante manda `Hello` en bucle; en cuanto el invitado legítimo se
  cae 5 s (timeout, `client_session.cpp:31`) el anfitrión suelta `peer` y el siguiente `Hello`
  del atacante se queda con el puesto. Después de fijado, **no** hay secuestro: `from == *peer`
  (`mod.cpp:304`) descarta a cualquier otro SteamID — bien hecho.
- **Arreglo:** fijar `peer` de forma provisional y soltarlo si la sesión no llega a `Connected` en
  p. ej. 5 s (guardar `peer_since_ms`; `if (host && peer && !connected && now - peer_since > 5000) peer.reset()`),
  y no fijar con un `Hello` cuya `packet->version != kVersion` (sólo registrar el aviso). Para
  partidas reales, recomendar en las instrucciones rellenar `[steam] peer_steamid` en el
  anfitrión (entonces Steam sólo acepta a ese SteamID).

---

## BAJA

### B1. El transporte Steam acepta y encola tráfico de **todos** los amigos que juegan, incluso con par fijado
- **Dónde:** `src/mod/net/steam_transport.cpp:263-275` (cada 500 ms `AcceptSessionWithUser` para
  todos los amigos jugando, también tras fijar `peer_`), `:356-373` (`drain_incoming` encola sin
  mirar quién envía; `inbox_` con tope 1024 que **descarta los más antiguos**).
- **Ataque:** un tercer amigo jugando (no el par) inunda con datagramas pequeños. `run_network`
  los descarta (`from != *peer`), pero antes ocupan `inbox_` y el presupuesto de 512/iteración;
  con la cola llena, los paquetes del par legítimo se descartan ("drop oldest") → pérdida de
  estados/eventos, posible timeout de 5 s → M2 ataque 2. Coste de CPU moderado.
- **Arreglo:** cuando hay par (`peer_ != 0`), en `drain_incoming` descartar antes de copiar todo
  mensaje cuyo `m->peer.steam_id64 != peer_` (y en el host sin par, sólo aceptar `Hello`), dejar
  de llamar a `AcceptSessionWithUser` para otros, y `CloseSessionWithUser` a quien envíe sin ser
  el par. Opcional: tope por remitente en `inbox_`.

### B2. Un `Responder` (anfitrión) acepta `Welcome` y se conecta sin `Hello`
- **Dónde:** `src/common/client_session.cpp:131-151`.
- **Efecto:** el anfitrión pasa a `Connected` con un `Welcome` (si su `peer` ya está fijado, p.ej.
  por el caso M2), y estando conectado cualquier `Welcome` con otro nombre resetea el canal y sube
  el epoch (vector de M1). No da más poder que el que ya tiene el par, pero amplía superficie.
- **Arreglo:** `if (role_ == Role::Responder) { ++stats_.rejected; return; }` al recibir `Welcome`.

### B3. `PlayerState`: floats finitos pero sin rango
- **Dónde:** `src/common/protocol.cpp:261-269`; usos: `coop.cpp:174,182,197` (posición del par
  copiada a la posición de **reaparición** de la copia), `coop.cpp:248` (pin del mapa),
  `hud.cpp:50,135-149` (`health/health_max`, distancia y proyección).
- **Ataque:** `pos = (3e38, 3e38, 3e38)`, `rot` no normalizado, `health = -1e30`,
  `health_max = 1e-38`. En la capa de red no rompe nada (el HUD hace `clamp`; `%.0f` con 1e38 es
  seguro), pero la posición extrema llega a `puppet_spawn` y a `puppet_map_pin` del juego, y
  `length(sub(...))` da `inf`. Riesgo real depende del código de juego (fuera de este alcance).
- **Arreglo:** validar en `decode` o al recibir: `|x|,|y|,|z| ≤ 1e6`, cuaternión con norma en
  [0,9; 1,1] (y normalizarlo), `0 ≤ health ≤ health_max ≤ 1e7`. Rechazar el datagrama si no.

### B4. `old_streams_` expulsa el id **más pequeño**, no el más antiguo
- **Dónde:** `src/common/reliable.cpp:85` (`std::set` ordenado por valor).
- **Efecto:** con >16 reinicios (fácil si el par provoca M1), un stream reciente puede salir del
  conjunto; un rezagado suyo se toma como "stream nuevo", se vacía `incoming_/ready_` y el stream
  actual pasa a `old_streams_` → sus fragmentos siguientes se **acusan y se tiran**: pérdida
  silenciosa de mensajes fiables (Spawn, muertes). Es robustez más que seguridad.
- **Arreglo:** `std::deque<uint32_t>` FIFO (o set + deque) para expulsar el más antiguo.

### B5. (Robustez, relacionado) Tras un timeout asimétrico se pierden los mensajes fiables del otro lado
- **Dónde:** `client_session.cpp:109-111` + `:206-214` + `reliable.cpp:136-144`.
- **Efecto:** si sólo el invitado da timeout, al reconectar con el **mismo nonce** el anfitrión lo
  trata como duplicado (no resetea ni sube epoch), pero el invitado ya metió el stream del
  anfitrión en `old_streams_`: lo que el anfitrión tuviera en vuelo (p. ej. su Spawn) se acusa y
  se descarta, y el anfitrión no lo reenvía porque su epoch no cambió. No es explotable más allá
  de lo que el par ya puede hacer, pero conviene saberlo (copia invisible tras cortes).
- **Arreglo:** generar un nonce nuevo en `reset_to_connecting` (o enviar un campo "reset" en
  `Hello`), de modo que el otro lado también resetee.

---

## INFORMATIVO

- **I1. Cadenas de formato: correcto.** Grep de todo `src/`: el nombre del par sólo aparece como
  argumento (`ImGui::Text("…'%s'…", d.peer_name.c_str())`, `hud.cpp:118`; `"%s"` en `:125`;
  `AddText` no formatea, `hud.cpp:30-31`), y `log::*` usa `std::format_string` (formato fijo en
  compilación, `log.h:18-29`). No hay `printf(nombre)` ni `ImGui::Text(nombre)`.
- **I2. Inyección en el log: limitada.** `Reader::str` (`protocol.cpp:67-80`) limita a 32 bytes y
  sustituye todo lo que no es ASCII imprimible (sin CR/LF) → no se pueden falsificar líneas. Sí se
  puede poner texto engañoso dentro de la línea (p. ej. nombre `x' reconnected`), y comillas.
- **I3. Suplantación trivial de "cdmp_peer".** Un par que se llame `cdmp_peer…` cambia el
  desplazamiento de la copia (`coop.cpp:115`) y con `mod_build = 0` evita el aviso de build
  distinta (`client_session.cpp:84`). Sin impacto de seguridad; en Steam podría ignorarse el caso 0.
- **I4. RTT falsificable** (`client_session.cpp:160-162`): el par elige `origin_ms` del `Pong`;
  sólo afecta a lo mostrado en el HUD/log.
- **I5. Repetición (replay):** no hay anti‑replay (`seq`/`send_ms` no se usan), pero en Steam no
  hace falta: ISteamNetworkingMessages cifra y autentica (identidad de Steam no falsificable, se
  comprueba `type == SteamID` en `steam_transport.cpp:364`). El propio par puede repetir lo que
  quiera, pero eso no le da nada que no tenga ya. El nonce no es un desafío criptográfico (no
  necesita serlo).
- **I6. IP del jugador:** el comentario `mod.cpp:212-213` dice que Steam "oculta las IP". Entre
  **amigos**, Steam puede usar ICE/P2P directo (según la opción de Steam del usuario "Permitir
  conexiones P2P" y `k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable`), lo que revela la IP
  pública al amigo. Si se quiere garantizar sólo relé: fijar `P2P_Transport_ICE_Enable = 0`
  (`SetConfigValue`; cuidado con hacerlo global si el juego usa Steam networking) o documentarlo.
  Verificar antes de prometerlo en el README.
- **I7. Datos propios enviados al par:** el Spawn manda las cargas `ActorLoadDataAck` del
  usuario/cuerpo/caballo **tal cual las da el servidor del juego** (`coop.cpp:211`). Conviene
  revisar si contienen identificadores de cuenta o datos que no deberían salir del PC.
- **I8. UDP:** sólo loopback en ambos lados (`mod.cpp:216-225`, `udp.cpp:90`); el anfitrión UDP
  acepta el primer `Hello` de cualquier proceso local — aceptable para pruebas. `recvfrom` con
  datagrama mayor que el búfer devuelve error (no trunca en silencio).
- **I9. `ByteRing`** (`byte_ring.h`) no recibe datos del par (sólo el sniffer local); sin hallazgos
  de red. (Un registro con `len` 1-3 no bloquearía el bucle de `drain`.)
- **I10. Sentido de envío:** si el par nunca acusa, nuestro canal queda acotado (`kWindow` 128 en
  vuelo, `kMaxQueued` 2048 en cola, ≤64 reenvíos/tick, cada fragmento como mucho cada 250 ms →
  ~0,6 MB/s de salida máximo). Correcto.

---

## Lo que ya está bien
- `Reader` con comprobación de límites en cada lectura (`has()` sin desbordamiento: `pos_ ≤
  size`), sin `memcpy` de structs, `at_end()` obligatorio, floats no finitos rechazados, tamaño
  de datagrama acotado (16..1200) antes de decodificar, tipos desconocidos rechazados, `kind = 0`
  rechazado.
- Longitudes acotadas antes de reservar: `GameEvent` ≤ 1024, chunk ≤ 1100, `index < count`,
  `count ≥ 1`, `count*1100` acotado; `decode_spawn` acota cada blob a 0xFFFF y comprueba que
  queden bytes antes de copiar (no hay reservas por tamaño declarado).
- `ReliableChannel`: `index` revalidado contra el parcial, `count`/`kind` coherentes entre
  fragmentos, comparación de ids con aritmética circular, límite de parciales (64) y de adelanto
  (4096), refusar sin acusar cuando no hay sitio, `old_streams_` acotado.
- `ClientSession`: nada salvo `Hello`/`Welcome` antes del handshake; kinds desconocidos se
  descartan antes de llegar al juego; aviso de otra versión limitado a 1/10 s.
- Steam: identidad autenticada por Steam y filtrada por tipo; tamaño comprobado (`> 0`, `≤ 1200`,
  `data != null`) antes de copiar; mensajes liberados siempre; `inbox_` acotado; todas las
  llamadas a `steam_api64` dentro de `__try/__except` que desactiva el transporte en vez de
  tumbar el juego; el host sólo acepta amigos que juegan o un SteamID fijo; tras fijar el par,
  ningún otro SteamID llega a la sesión (`mod.cpp:304`).
- Nombres: ≤32 bytes, ASCII imprimible; nunca usados como formato.
- Bucle de recepción acotado (512/iteración); `CoopLink` limita sus propias colas
  (`ordered_held_` 256, `held_enemy_spawns_` 64, outbox 4096).

## Huecos en `tests/tests.cpp`
Ya probado: magia/versión/tipo malos, truncado, basura al final, tamaño > 1200, NaN/Inf, nombre
largo/no imprimible, `index >= count`, `kind = 0` y kinds nuevos, Spawn truncado, tope de
`kMaxPartials`, rezagados de stream viejo, duplicados de `Hello`.
Falta: (1) A1 — muchos mensajes completos adelantados (memoria de `ready_`); (2) `msg_id` ≥
`rx_next_ + kMaxAhead`; (3) fragmento con `count`/`kind` distinto al del parcial; (4) 17+ streams
(B4); (5) `decode_spawn` con longitud declarada > bytes restantes y con > 0xFFFF; (6) `RelData`
con `count = 0` y `count` en el límite (239/240); (7) `Welcome` a un `Responder` (B2);
(8) ráfaga de `Hello` con nonces distintos (M1, con el limitador); (9) un fuzzer simple
(bytes aleatorios / mutaciones de paquetes válidos contra `decode`, `decode_spawn` y
`ClientSession::on_datagram`) — fácil de añadir y barato.

## Punto de entrega a la lógica del juego (fuera de este alcance; para la otra revisión)
Lo que la red garantiza al llamar a los manejadores (`coop.cpp:316-414`): `GameEvent.kind` en
1..13 y `payload` de **0 a 1024 bytes sin más validación**; mensajes fiables de kind 1..7 de
**0 a 262 900 bytes**; `SpawnInfo` con `user/body ≥ 256` B, `horse` vacío o ≥ 256, cada uno
≤ 65 535, `equip` 0..65 535, y claves/posiciones arbitrarias (finitas). Destinos a revisar:
`puppet_event`, `puppet_spawn`, `enemy_on_spawn/remove/state/event`, `hits_on_event`,
`guest_hits_on_event`, `time_sync_on_event`, `apply_remote_damage`, `merge_equip`
(`EquipState::valid`). `EnemyRemove` admite hasta ~65 000 claves por mensaje (bucle con
`erase_if` sobre ≤64 retenidos, `coop.cpp:397-401`, más lo que haga `enemy_on_remove`).
