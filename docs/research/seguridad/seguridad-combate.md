# Revisión de seguridad: enemigos compartidos y combate (build 2976, commit fa89068)

Alcance: `enemy_relay`, `guest_hits`, `player_hits`, `aggro_test`, `local_capture`, `coop.cpp`
(y lo que llaman en `remote_puppet.cpp` / `troc_sniffer.cpp`). Solo lectura; no se tocó el repo.

Modelo de amenaza: el otro jugador (amigo de Steam) usa un mod modificado que envía mensajes del
protocolo bien formados con contenido arbitrario. Todo lo que sigue se refiere a lo que **el
receptor** hace con esos bytes.

## Observaciones transversales (afectan a casi todos los hallazgos)

- **Los mensajes no se filtran por rol.** `coop.cpp:316-342` y `:389-404` aceptan `Damage`,
  `PlayerHit`, `GuestHit`, `EnemySpawn/Act/Pos/State` venga quien venga. El anfitrión acepta copias
  de enemigos del invitado (`mod.cpp:625`: `enemy_set_receive_enabled(guarded)` en los dos PC) y el
  invitado aplica `PlayerHit` aunque no tenga nada activado. Cada hallazgo vale en los dos sentidos.
- **No hay límites de ritmo** en ningún evento de combate recibido. Los únicos topes son de cola:
  `g_delayed` 128 (`guest_hits.cpp:607`), cola C>S 16 (`troc_sniffer.cpp:488`) y retenidos 64
  (`coop.cpp:392`). La cola S>C `g_queue` no tiene tope (`remote_puppet.cpp:216-222`).
- **Lo inyectado S>C vuelve a pasar por nuestros propios observadores.** `on_recv_packet` captura
  también los paquetes inyectados (`troc_sniffer.cpp:214` y `:221-226`). Así, una carga que manda
  el par la "aprenden" `capture_observe_s2c` (plantillas del jugador propio), `enemy_observe_s2c`
  (`g_known`) y los aprendices de golpes. Ver H3.
- **Las guardas SEH no hacen seguro el código del juego.** `guarded_inject`
  (`troc_sniffer.cpp:204-211`) evita que el proceso caiga *en ese momento*. Pero un handler del
  juego que falla a medias puede dejar locks tomados u objetos a medio construir, y el crash llega
  después en otro hilo (y se informa a Pearl Abyss). Hay que validar antes de inyectar, no confiar
  en el `__except`.

---

## Hallazgos por gravedad

### H1. ALTA. `Damage` (daño espejo) está activo por defecto y deja a cualquier NPC humano cercano a 1 HP, o a 0 HP si no se conoce su vida
- **Dónde:** `local_capture.cpp:856-944` (`apply_remote_damage`); activo por defecto en
  `config.h:66` (`coop_damage = true`) y `mod.cpp:612` (solo se apaga con `guest_hits=1`). Lo
  despacha `coop.cpp:319-322` sin comprobar el rol.
- **Ataque:** el par envía `Damage{variant, pos, delta}` con un `delta` enorme y negativo. El
  receptor escoge el actor de tipo 3 (`g_enemies`) de esa variante que esté vivo, se haya movido en
  los últimos 10 s y tenga el punto de aparición a menos de 25 m (`:878-892`). En `g_enemies` entran
  **todos** los registros de tipo 3 (`:591-610`): enemigos, pero también aldeanos, mercaderes,
  NPC de misión y aliados en batalla. Las variantes son datos estáticos del juego, iguales en los dos
  PC, y el atacante ve los mismos NPC en su mundo.
  - Si se conoce el HP (`best_hp > 0`), lo baja a 1 HP (`:925-926`).
  - Si el HP no se conoce (`hp == -1`: el NPC aún no ha recibido ningún `VaryHPAck`), no hay recorte
    y el NPC queda a **0 HP**, "noqueado/zombi" (`pieza4-dano-servidor.md` §2.d). El siguiente golpe
    nativo de cualquiera, incluso de un NPC aliado, lo mata con una muerte normal.
  - Sin límite de ritmo: se puede repetir sobre todos los NPC cercanos.
- **Efecto:** muerte de NPC de misión, mercaderes o aliados en el servidor en proceso, lo que
  **puede persistir en la partida** (estado de misión). El `VaryStatByAccInputReq` va al hilo del
  servidor. El precedente de crash fue un actor sin stat de HP (`interacciones-mundo.md` "Lo que NO
  hay que hacer"). Con `hp == -1` no hay prueba de que el actor tenga ese stat.
- **Arreglo:**
  1. Desactivar `coop_damage` por defecto (lo sustituye `guest_hits`), o quitarlo.
  2. Si se mantiene, aceptar solo objetivos de una lista blanca:
     - HP conocido (`e.hp > 0` desde un `VaryHPAck` de stat HP, lo que prueba que tiene ese stat);
     - hostil: clave de facción `+0x1C` distinta de 0, y apuntó (`CurrentTargetChangeReq`) o golpeó a
       nuestro jugador o a la copia en los últimos 20 s (como `g_engaged`);
     - la copia del otro jugador (`0xA010F001`) a menos de unos 10 m del objetivo (unos 80 m para
       flechas).
  3. Recortar `|delta|` a `min(hp - 1000, 150 HP, 25 % del máximo)`.
  4. Limitar a unos 5 golpes por segundo por objetivo y unos 10 por segundo en total.
  5. Aceptarlo solo en el rol que corresponde.

### H2. ALTA. `EnemySpawn`: las acciones que acompañan a la carga pueden nombrar CUALQUIER actor, incluido nuestro propio jugador
- **Dónde:** `enemy_relay.cpp:1047-1048` solo comprueba que el tamaño sea múltiplo de 725. La
  barrera de claves (`:1060-1068`) recorre **solo `load`**, no `action`. `remote_puppet.cpp:958-964`
  convierte cada Req en `StartActionAck` y la inyecta.
- **Ataque:** el anfitrión incluye N `StartActionReq` cuyo `+0` es `kSelfMarker` (que
  `untranslate` convierte en nuestra clave de jugador, `:124-129`) o cualquier clave de nuestro
  mundo. Las claves son secuenciales y el anfitrión conoce las nuestras (player, body, horse) por el
  `SpawnInfo`. N solo está limitado por los 256 KB del mensaje (`protocol.h:103`): unas 360 acciones.
- **Efecto:** el cliente recibe `StartActionAck` arbitrarios (hash de acción, posiciones, padre en
  `0x117`) para nuestro jugador, nuestro caballo o un NPC o gimmick. Puede forzar animaciones o
  estados y tiene un riesgo real de crash en el hilo de red del cliente: una acción sobre un gimmick,
  una capa de acción inexistente o el precedente del caballo de `INFORME-CABALLO-CRASH`.
- **Arreglo:** en `enemy_on_spawn`:
  - rechazar el mensaje si una acción tiene `u32_at(+0) != copy`;
  - aceptar como mucho `kKeptActions` (4) acciones;
  - aplicar a cada acción la misma barrera de claves que a la carga (sin la excepción del jugador
    propio, ver H3) y poner a "sin padre" `+0x116` y a 0 el objetivo `+0x16C`.

### H3. ALTA. `EnemySpawn`: la cabecera del registro de carga no se valida, así que se pueden cargar actores arbitrarios y secuestrar las plantillas del jugador propio
- **Dónde:** `enemy_relay.cpp:1040-1068` y `remote_puppet.cpp:954`. Solo se comprueban
  `is_enemy_copy_key(copy)` del encabezado de red y `n >= 0x40`. **No** se comprueban:
  - el `u16` de longitud en `load[0]` (que sea igual a `n - 2`, es decir, un solo registro);
  - el tipo `load[2]` (que sea 3);
  - la clave del registro `load[3..7]` (que sea igual a `copy`);
  - el dueño `load[7..11]` (que sea 0);
  - un tamaño máximo (`make_packet` trunca el tamaño a `u16`, `remote_puppet.cpp:133`).

  Además, la barrera **deja pasar nuestra propia clave de jugador en cualquier sitio** (`:1063`) y
  solo rechaza claves que *existen ahora* en el cliente (`find_actor`). Las claves de usuario
  (clase 1), las de actores que el servidor tiene pero el cliente no ha cargado y las futuras pasan.
- **Ataques:**
  1. **Varios registros** en un `ActorLoadDataAck`, o un registro con clave del rango del juego
     y otro tipo (usuario, montura, gimmick): el cliente carga actores con claves que el servidor en
     proceso tiene o tendrá. Si el registro deja el control en local (dueño o usuario de control =
     nuestro usuario), el cliente ejecuta su IA y envía C>S (`MoveActorReq`, `StartActionReq`,
     `ChangeBattleStateReq`) con esa clave. El servidor se fía del cliente para el movimiento de los
     NPC, así que podría mover o alterar un actor real del servidor. **Esto puede persistir en la
     partida** [inferido, no verificado].
  2. **Secuestro de plantillas:** el paquete inyectado vuelve a `capture_observe_s2c`. Un registro
     de clase usuario con un actor de clase jugador en `+0x0E` (cualquier clave distinta de
     `0xA010F001`, posición distinta de (0, 1000, 0)) sobrescribe `g_user`, `g_player_key` y
     `g_equip_key` (`local_capture.cpp:626-646`). El filtro de `:614-617` solo excluye las claves de
     la marioneta. Desde ese momento `capture_own_player_key()` devuelve la clave que eligió el
     atacante:
     - los `VaryStatByAccInputReq` y `AttackByCollisionReq` de `player_hits` van a esa clave
       (`player_hits.cpp:441`, `:389`);
     - se rompe toda la lógica de "propio" (equipo, golpes, cooperativo).

     Con `kLoadKindMount` también se sustituye `g_horse` (`:620-624`).
  3. **Bloque de grupo** (`ClientPartyActorComponent`) que nombra a nuestro jugador (permitido por
     `:1063`) como líder o miembro. Es el mecanismo que dejó invisibles "para siempre" a los
     originales (`copias-grupo-invisibles.md` §6).
- **Arreglo:** en el receptor, antes de inyectar:
  - exigir `u16 len + 2 == n`, `load[2] == 3`, `u32(load+3) == copy` y `u32(load+7) == 0`, y que
    `n <= 16 KB` (las cargas reales son de pocos KB);
  - exigir que `+0x18` (variante) sea distinto de 0 y que el usuario de control sea la copia de
    usuario del emisor (`0x9010F001`);
  - pasar `find_party` y exigir el bloque de grupo "solo" (`kSoloParty`);
  - no aceptar nuestra clave de jugador en la carga, salvo en campos concretos (si hiciera falta,
    solo `Target+0x198` / la lista de aggro);
  - rechazar cualquier valor con forma de clave de actor que no sea `copy` ni una clave de red, no
    solo las que `find_actor` encuentra hoy.

  En `capture_observe_s2c` y `enemy_observe_s2c`, **ignorar los paquetes que inyectamos
  nosotros**: pasar un indicador desde `on_recv_packet` cuando `t_injecting` sea true.

### H4. ALTA (con valores por defecto) / MEDIA. `PlayerHit` se aplica siempre, sin opción de configuración y sin límite de ritmo; con el HP sin conocer no hay recorte
- **Dónde:** `player_hits.cpp:401-451` y `coop.cpp:323-326`. `hits_set_enabled` solo controla el
  envío y el aprendizaje, no `hits_on_event`. Con los valores por defecto (`player_hit_react=0`,
  `lethal=0`) el daño se aplica igual.
- **Ataques:**
  - Un `PlayerHit` con `damage = INT64_MAX`:
    - si ya se conocen el HP y el máximo, queda en `max/2` y nunca baja de 1 HP (`:425`, `:432`), así
      que bastan 2 eventos para dejar al jugador a 1 HP, repetible sin límite;
    - si **todavía no** se ha visto un `VaryHPAck` propio (`g_own_hp_max == 0`, `g_own_hp == -1`,
      justo tras cargar), no hay recorte: `VaryStatByAccInputReq` con un delta de unos -9,2e18 deja
      al jugador a 0 HP por la ruta genérica (`OnBattle_HandleKnockOut`). Es un estado **sin probar**
      en el jugador (`pieza4-dano-servidor.md` §8: "puede quedar vivo a 0"), con riesgo de bloqueo y
      de que el HP 0 se guarde.
  - Con `player_hit_lethal=1`, matar al invitado cuando se quiera: 2-3 eventos y luego
    `die_by_collision` (`:431`, `:445`).
- **Arreglo:**
  - aplicar `PlayerHit` solo si `hits_set_enabled`/`enemies_target_copy` está activo **y** el emisor
    es el anfitrión;
  - descartar el evento si `max <= 0 || hp < 0`;
  - exigir que `copy` sea una copia de enemigo cargada aquí (`g_copies`) y que esté a menos de unos
    15 m del jugador (unos 100 m para proyectiles);
  - limitar a unos 4 golpes por segundo y al 30 % del máximo por segundo;
  - para un golpe letal, exigir golpes previos en la misma ventana, nunca uno solo.

### H5. MEDIA. `PlayerHit` inyecta en nuestro cliente plantillas ATA/AAR/buff que fabrica el par
- **Dónde:** `player_hits.cpp:331-376`. Los códigos de operación son fijos (2922, 2162, 2506; bien
  hecho) y se sobrescriben el objetivo, la instancia y la fuente. Pero:
  - (a) `copy` lo elige el par y solo se exige que exista (`:342`). Puede ser cualquier actor nuestro
    (caballo, NPC, gimmick, nuestro propio jugador), que pasa a "atacante" o "fuente".
  - (b) el ATA se acepta con cualquier tamaño `>= 0x30` (`:338`) y contenido arbitrario.
  - (c) el resto del buff (id de buff `+4`, duración y niveles) y de la restricción (hash `+0x38`)
    es del par: puede aplicarnos cualquier buff o restricción en el cliente (aturdimiento permanente,
    bloqueo de acciones), y tras `replace_u32(kSelfMarker → own)` cualquier campo puede nombrarnos.
  - Todo se ejecuta en el hilo de red del cliente; con un atacante gimmick, riesgo de crash.
- **Arreglo:** que el par mande solo índices y nunca bytes, como ya hace `guest_hits` en el
  anfitrión:
  - el `PlayerHit` lleva `(variante, hash, se_tambalea)` y el receptor usa **sus propias**
    plantillas aprendidas (`g_reactions`, que existen en los dos PC);
  - si se mantienen las plantillas del par, aplicar una lista blanca de ids de buff (la familia
    CC_Push observada) y de hashes de restricción, exigir un tamaño de ATA en el rango real
    (`0xA0..0x400`, como `valid_size`), poner a 0 todo campo con forma de clave que no sea uno de los
    que fijamos, y exigir `is_enemy_copy_key(copy)`.

### H6. ALTA si se activa `guest_hits` (por defecto apagado). Un invitado mata cualquier enemigo copiado, incluidos jefes y NPC no hostiles, desde cualquier distancia y a cualquier ritmo
- **Dónde:** `guest_hits.cpp:453-602` (`apply_hit`) y `:420-447` (rematar).
- **Lo bueno:** el objetivo sale de `enemy_original_of(copy)` (`:463`), es decir, solo enemigos que
  el anfitrión decidió copiar (`g_sent`): registros de tipo 3 con clave de mundo y dueño 0.
  - **No** se pueden alcanzar el jugador del anfitrión (su cuerpo es de tipo 7), su caballo ni sus
    mercenarios (registros de clase usuario), ni gimmicks ni claves arbitrarias.
  - Las plantillas inyectadas son del anfitrión.
- **Problemas:**
  - (a) `their_base` no tiene límite (`:510-511`): `base = 2^31/1000` (unos 2,1 millones de HP) hace
    letal cualquier golpe. Luego `VaryStatCheatReq` a 1 HP, colisión y, si no muere,
    **`KillCheatReq`** a los 600 ms (`:443-444`). Muerte segura de cualquier enemigo copiado, jefes
    incluidos; es probable que ignore la invulnerabilidad, porque es la ruta de trucos.
  - (b) Sin comprobar distancia: la copia del invitado no tiene por qué estar cerca del enemigo.
  - (c) Sin límite de ritmo (solo `g_delayed < 128` en vuelo).
  - (d) `g_sent` puede contener NPC no hostiles:
    - Ctrl+F6 (`pick_manual`, `enemy_relay.cpp:238-249`) coge el tipo 3 más cercano aunque no esté
      peleando;
    - "en combate" (`:817-821`, `:751-755`) incluye a los aliados en `ChangeBattleState != 0` y a
      cualquier NPC que nuestro jugador golpee sin querer.

    Matar a un NPC de misión o a un aliado con `KillCheatReq` en el servidor del anfitrión **persiste
    en su partida**.
- **Arreglo:**
  - **ignorar `their_base`**, o limitarlo a `<= 2 × max(kSeedBase)` (unos 120 HP);
  - `damage <= 25 %` del máximo del enemigo por golpe;
  - exigir `kPuppetPlayer` a 8 m o menos del enemigo (80 m o menos si el accesorio es de tipo 1,
    flecha);
  - limitar a un golpe cada 150 ms por enemigo y unos 8 por segundo en total;
  - exigir que el enemigo esté en combate (`g_engaged` en los últimos 20 s) **y** sea hostil:
    facción distinta de 0 y que apuntara o golpeara al jugador o a la copia;
  - no usar `KillCheatReq` como remate: dejar a 1 HP y que lo remate un golpe nativo;
  - en el emisor, que `pick_manual` y `auto_tick` solo copien enemigos hostiles en combate.

### H7. MEDIA. Rango de claves de copia demasiado amplio (incluye los centinelas del cliente)
- **Dónde:** `enemy_relay.h:21`. `is_enemy_copy_key` acepta `0xB01F0000-0xB01FFFFF`, es decir, los
  índices `0xF0000-0xFFFFF`. El emisor se limita a `1..0xEFFF` precisamente para no llegar a los
  centinelas `0xFFFFE` y `0xFFFFF` del cliente (`enemy_relay.cpp:80`), pero el receptor no lo exige.
- **Ataque:** `EnemySpawn` con `copy = 0xB01FFFFF` o `0xB01FFFFE` (o `0xB01F0000`): el cliente carga
  un actor con un índice centinela. Riesgo de crash en el hilo de red o en el de juego.
- **Arreglo:** en el receptor, `net_id = copy - kEnemyCopyBase` con `1 <= net_id <= 0xEFFF`.

### H8. MEDIA. Copias sin tope de número, sin ritmo y sin cuarentena al reutilizar una clave
- **Dónde:** `enemy_relay.cpp:1069-1073`. `g_copies` crece sin límite, y si llega la misma clave se
  hace `puppet_remove_actor` y se carga otra vez en el mismo tick. Además, `coop.cpp:87-93` carga de
  golpe hasta 64 retenidas.
- **Ataque:** cientos de `EnemySpawn` (claves distintas), o la misma clave en bucle (retirar y cargar
  sin la cuarentena de 10 s que el emisor respeta "porque la retirada puede ir de camino"). Agota
  memoria y CPU en el hilo de red del cliente, con riesgo de crash del juego.
- **Arreglo:**
  - tope de copias a la vez (16, el doble de `kAutoMax`);
  - como mucho unas 2 cargas por segundo;
  - si llega una clave ya cargada, ignorarla o exigir que pasen 10 s desde su retirada;
  - al volver al mundo, sacar las retenidas de una en una (como mucho 2 por tick).

### H9. MEDIA. `EnemyPos` y la transformada de la carga aceptan NaN, infinitos y coordenadas absurdas
- **Dónde:** `enemy_relay.cpp:1008-1016`, luego `remote_puppet.cpp:1001-1025`, sin
  `std::isfinite`. La posición de la carga (`remote_puppet.cpp:936-952`) tampoco se valida.
- **Ataque:** posiciones NaN o 1e30, o un cuaternión no normalizado, para una copia: llegan a la
  física y la animación del cliente. Riesgo de crash en un hilo del juego y de lecturas basura en
  `game::actor_world_position`.
- **Arreglo:**
  - exigir valores finitos y `|x|, |y|, |z| < 1e5`;
  - exigir que estén a menos de unos 200 m de nuestro jugador y que el salto respecto a la muestra
    anterior sea de `30 m/s × dt` como mucho;
  - normalizar el cuaternión y rechazarlo si su norma no está entre 0,9 y 1,1;
  - validar igual la transformada de la carga.

### H10. BAJA-MEDIA. `EnemyAct`/`EnemyState`: los campos de clave que no son el objetivo quedan libres
- **Dónde:** `enemy_relay.cpp:1017-1027` y `:1088-1099`. Se comprueban el código de operación, el
  tamaño y que `+0` sea una copia (bien), pero no:
  - el atacante de `AttackTargetAck` (`+4`, `+0xBC`, `+0xCE`);
  - la fuente de la restricción (`+0x49`);
  - la fuente de `VaryHPAck` (`+0x54`);
  - el asesino de `NotifyHandleDeadAck` (`+4`).

  Tras `untranslate` pueden nombrar a nuestro jugador o a cualquier actor nuestro. Además,
  `clear_parent` respeta `kPuppetHorse` (`remote_puppet.cpp:249`), así que una acción de enemigo
  puede "montar" la copia en el caballo copiado.
- **Efecto:** "nuestro jugador mató a X" o "golpeó a X" en el cliente (posibles efectos de UI o
  reacciones locales); emparejamientos de montura imposibles, con riesgo de crash (precedente del
  caballo).
- **Arreglo:**
  - en el receptor, pasar estos campos por el mismo mapeo que `map_message_locked`: solo se permiten
    `copy`, otra copia de `g_copies`, nuestro jugador (vía marcador) y `0xA010F001`, y todo lo demás
    se pone a 0 o a FF;
  - para copias de enemigo, quitar siempre el padre, incluido `kPuppetHorse`.

### H11. BAJA. Llenado del log y del disco
- **Dónde:** `troc_sniffer.cpp:190` escribe una línea por cada C>S inyectado, sin límite; también
  `local_capture.cpp:939-941` y otros, aunque esos tienen contador.
- **Ataque:** una avalancha de `PlayerHit` o `Damage` produce miles de líneas por minuto en
  `cdmp.log`, que además puede estar en una carpeta sincronizada en la nube.
- **Arreglo:** contador o ritmo por código de operación (como los `g_*_log++ < 50` de otros sitios).

### H12. BAJA. Un hook en hilos del juego toma un `std::mutex`
- **Dónde:**
  - `aggro_test.cpp:217` (`is_targetable`, hook inline sobre IsTargetable, hilo de IA) y `:280`
    (`on_set_target`, hook medio) llaman a `capture_own_player_key()`;
  - esa función toma `local_capture`'s `g_mutex` (`local_capture.cpp:830-833`), cuyo comentario dice
    "never taken on game threads" (`:72`);
  - el hilo escritor sostiene ese mutex mientras escribe en el log (`:623`, `:645`, `:655`).
- **Efecto:** bloqueos del hilo de IA (tirones). No es un crash, pero contradice la regla y la
  contención crece con el tráfico del par.
- **Arreglo:** guardar la clave propia en un `std::atomic<uint32_t>` (ya existe `g_equip_key`) y
  leerla así desde los hooks.

### H13. INFO. Datos que salen del PC (`local_capture.cpp`)
- **Se envían:**
  - las cargas completas del usuario, del cuerpo y del caballo propios (`capture_spawn_info`,
    `:781-807`): apariencia, equipo y el nombre del personaje ("Kliff", fijo);
  - cambios de equipo, posiciones, acciones y el trazado de escalada;
  - los Acks sobre el jugador propio de `kRelayedAcks` (HP, buffs, muerte, revivir, personalización);
  - `Damage` (variante y punto de aparición);
  - las cargas de los enemigos del anfitrión.
- **No se envían** rutas, nombres de Windows ni identificadores de Steam por estas vías: el nombre del
  par lo pone la sesión.
- **Recomendación:** verificar una vez que la carga de usuario de unos 12,8 KB no lleva identificadores
  de cuenta o plataforma (número de usuario, nombre de Steam). Recordar que los logs que se
  comparten entre PC contienen claves y posiciones, y en el futuro el SteamID
  (`pendiente-robustez.md` §527).

---

## Riesgo de corrupción de la partida (resumen)

| Vía | ¿Persiste? | Hallazgo |
|---|---|---|
| `Damage` deja NPC de misión, mercaderes o aliados a 0 o 1 HP y luego los mata un golpe nativo | **Probable**: estado del mundo y de las misiones en el servidor en proceso | H1 |
| `GuestHit` + `KillCheatReq` sobre NPC no hostiles de `g_sent` | **Probable** (partida del anfitrión) | H6 |
| `PlayerHit` con HP sin conocer: jugador "vivo a 0 HP" | Posible: el HP del jugador se guarda; estado sin probar | H4 |
| Carga inyectada con clave real del servidor y control local, cuyos C>S alteran un actor real | Posible [inferido] | H3 |
| Buffs o restricciones inyectados S>C | Improbable: solo en el cliente, y la partida la escribe el servidor | H5 |

## Código que se ejecuta en hilos del juego

- **Hilo de red del cliente (`OnRecvPacket`):** todo lo inyectado S>C (cargas, acciones, ATA, AAR,
  buffs, `VaryHPAck`, `NotifyHandleDeadAck`, movimiento) lo procesan los handlers del juego con bytes
  del par. Afecta a H2, H3, H5, H7, H8, H9 y H10.
- **Hilo del servidor en proceso:** los C>S inyectados (`VaryStatCheatReq`, `KillCheatReq`,
  `DieByRunOutFatalReq`, `AttackByCollisionReq`, `VaryStatByAccInputReq`). Hoy sus claves salen de
  listas locales (jugador propio, `g_sent`, `g_enemies`), no del par (bien), salvo la cadena de H3.
- **Hilos de IA:** hooks de `aggro_test` (SEH en todas las lecturas, bien; mutex en H12).

## Lo que ya está bien hecho

- **Listas blancas de opcode y tamaño** en lo que se acepta del otro lado: `valid_size`
  (`enemy_relay.cpp:133-143`) con tamaños exactos y solo 6 códigos; `EnemyState` solo
  `VaryHPAck`/`NotifyHandleDeadAck`.
- **Objetivo restringido a copias cargadas:** `EnemyAct`, `EnemyPos` y `EnemyState` exigen que
  `+0` esté en `g_copies`.
- **Las copias solo se aceptan con las guardas de crash instaladas** (`mod.cpp:617-625`).
- **Saneado en el emisor** (`sanitize_load`): quita padre, hijos y grupo, corrige `Target+0x198`,
  hace una comprobación de claves residuales y rechaza jinetes y torres. El receptor repite una
  barrera propia (padre, hijos, jerarquía y claves existentes).
- **`guest_hits` no acepta claves del par:** el objetivo sale de `g_sent`, el ataque del par solo
  elige plantillas **locales** del anfitrión y nunca se inyectan bytes del invitado en el anfitrión.
  Tiene comprobaciones de muerto y de 0 HP y está apagado por defecto.
- **`player_hits`:**
  - códigos de operación fijos;
  - objetivo forzado a nosotros, instancias en un rango privado;
  - tope de `max/2` por golpe y nunca por debajo de 1 HP salvo `lethal` (apagado por defecto);
  - el daño negativo no cura;
  - la muerte es nativa (motivo 8), no "DevCheat".
- **Las Req de stats nunca van a gimmicks:** `g_enemies` y `g_sent` solo tienen registros de tipo 3.
- **Topes de colas:** outbox 4096, `g_out` 64, `g_out_events` 512, `g_delayed` 128, C>S 16, retenidos
  64, parciales fiables 64.
- **Claves:** rango propio `>= 0x8000`, alarma si el juego llega a él, cuarentena de 10 s del net id
  en el emisor, nunca direcciones de memoria.
- **Lecturas de memoria:** todas con SEH o `safe_read` en `aggro_test` y `enemy_relay`. Inyección
  envuelta en SEH que se desactiva tras la primera excepción.
- **Limpieza al cambiar de compañero** (`enemy_reset_partner`).
- **Red:** solo Steam entre jugadores (commit fa89068).

## Orden de arreglo sugerido

1. **H1:** apagar `coop_damage` por defecto o poner la lista blanca y los recortes.
2. **H4:** controlar `PlayerHit` por configuración y rol, y descartarlo con HP desconocido.
3. **H2 + H3 + H7 + H8:** validación completa de `EnemySpawn` en el receptor (cabecera, un registro,
   clave y rango, acciones con `key == copy`, sin el jugador propio, topes) y no observar lo inyectado.
4. **H6:** antes de activar `guest_hits` en pruebas con dos jugadores, limitar `their_base`, la
   distancia, el ritmo, la hostilidad y quitar `KillCheatReq`.
5. **H5, H9, H10, H11, H12.**
