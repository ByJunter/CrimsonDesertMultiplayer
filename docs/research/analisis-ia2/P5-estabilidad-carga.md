# P5 — Estabilidad durante las pantallas de carga de Shadow

HEAD auditado: `57bec36bdad634a502eb587f8f899c090ca162c5`. Juego: Crimson Desert 1.0.0.2976, PE `0x6AB28F00`, base `0x140000000`. Análisis de solo lectura: no se ejecutó el juego ni se aplicaron cambios. Solo se crearon este informe y `build/research/ia2/p5_capture.py`.

## Resumen (9 líneas)

1. **Hecho:** hay un cierre confirmado a G 17:46:35.215; `last_crash` registra `2026-10-08T00:46:35.517463Z`. No hay dump, pila ni RIP.
2. El segundo cierre observado en la prueba no puede reconstruirse independientemente con los archivos presentes.
3. **Hecho:** antes del cierre entraron doce cargas de enemigos sin acciones iniciales, entre 17:46:33.017 y 17:46:35.206. Es correlación, no prueba de la instrucción causante.
4. Las nuevas guardias de enemigos mejoran ese caso, pero no protegen toda la cola ni invalidan paquetes al cambiar de mundo.
5. **Hecho en HEAD:** durante una carga de noche2, t=152.938573–161.719350, entró una acción del caballo remoto en t=156.289 (G 20:37:39.811).
6. La posición local ya era válida antes de LoadingCompleteAck; el detector de pausa tampoco equivale a detectar carga.
7. **Propuesta:** compuerta central del mundo local con epoch, cierre inmediato por 2437/retirada19, y reapertura por 2762 más estado local coherente; comprobarla al consumir ambas colas.
8. EnemyPos/EnemyAct están marcados `movement=false`, por lo que eluden también la guarda de pausa actual.
9. El crash histórico sigue sin causa demostrada; sí está demostrado el agujero vigente de inyección durante carga.

## 1. Fuentes y límites

- Log del invitado `.../20261007-tarde/cdmp.1.log`: PID11788, build6B865217, protocolo9, desde 17:43:16 hasta 17:46:35.
- `.../20261007-tarde/cdmp.log`: reinicio PID12268, desde 17:46:53.696. No es la sesión que cerró a 17:46:35.
- `.../20261007-tarde/cdmp_packets_20261007-174707.{bin,log}`: captura del reinicio. No contiene los paquetes anteriores al crash.
- `.../20261007-tarde/DumpCache/DumpCache/`: last_crash 27B, metadata 16B, settings.dat 40B, lock 0B, dos breadcrumbs 0B, __sentry-event 1410B y session.json 295B. No hay archivo .dmp ni stacktrace.
- `.../20261007-noche2/cdmp.log` y captura203438: evidencia de la build actual.
- Código: `coop.cpp`, `remote_puppet.cpp`, `troc_sniffer.cpp`, `enemy_relay.cpp`, `native_suppress.cpp`, `local_capture.cpp`, `time_sync.cpp`, `loot_twin.cpp`, `player_reader.cpp`, `inject_clock.h`.
- Estudios previos: `pendiente-robustez.md:73`, `enemigos-faltan-invitado-7oct.md`, `zona-tras-muerte-anfitrion.md`, `orcax-equiphide.md`.

La idea de world_epoch ya figuraba en pendiente-robustez; en HEAD no se encuentra un manejo de 2437/2445/2762 ni world_epoch en src. Este informe verifica qué falta y aporta evidencia actual.

El script `p5_capture.py` lee registros en streaming. Sus contadores IN_LOADING incluyen ambos sentidos y mensajes nativos que nombran copias: no son todos inyecciones. Las pruebas citadas aquí usan cargas de claves reservadas y acciones S>C corroboradas por el log de entrega.

## 2. Línea de tiempo del cierre conocido

G es Shadow. H≈G+1h00m00,6s es una conversión orientativa, no una observación independiente de la excepción en H.

| G | H aproximada | Observación |
|---|---|---|
|17:44:12.304|18:44:12.904|Jugador aparcado en (0,1000,0), carga inicial.|
|17:46:31.336|18:46:31.936|StartActionAck del caballo remoto entregado, 730B totales.|
|17:46:31.400|18:46:32.000|Local en (-10021.60,715.63,-2446.77).|
|17:46:32.935|18:46:33.535|B01F0001 encolada, 8867B, NO action.|
|17:46:33.017|18:46:33.617|Cargas de B01F0001/0002 entregadas, 8872/8142B totales.|
|17:46:33.143|18:46:33.743|RESTORE de ambas copias: capas sin current/playing, pendientes0.|
|17:46:33.216|18:46:33.816|Guardia de update: tres saltos por falta de acción.|
|17:46:33.383–34.705|18:46:33.983–35.305|Varias copias ya tienen acción; otras todavía no.|
|17:46:34.970|18:46:35.570|B01F0001 existe, estado10/login1, a 10545,4m del local.|
|17:46:35.114|18:46:35.714|B01F0004 a 10543,8m del local.|
|17:46:35.206|18:46:35.806|Carga de B01F000C entregada, 8643B totales.|
|17:46:35.215|18:46:35.815|Game error «[Crash] ...», emitido desde exe+1320E76. Es el notificador, no el RIP del fallo.|
|17:46:35.234|18:46:35.834|Guardia contabiliza 19 updates saltados.|
|17:46:35.422–35.495|18:46:36.022–36.095|El worker sigue registrando brevemente después de la notificación.|

Constan doce entregas2005 (claves1..C), todas procedentes de entradas NO action. La distancia de unos10,5km es compatible con que el jugador local estuviera aparcado, pero no se registró aquí su posición exacta. No afirmar que todas las copias seguían sin acción al producirse el crash: los logs muestran que varias se recuperaron.

`last_crash=00:46:35.517Z` corresponde a 17:46:35.517 con UTC−7 y concuerda a unos302ms de la notificación. `session.json` empieza a 00:46:54.092Z y dice status=ok/errors=0: pertenece al proceso nuevo y no desmiente el cierre anterior. `__sentry-event` marca fatal y release Windows_Steam@2.03.02, pero contiene metadata, sin excepción ni pila. Los identificadores personales de Sentry no aportan al diagnóstico y se omiten.

Clasificación causal:

- **Hecho, confianza alta:** crash y creación de copias sin acciones coinciden temporalmente.
- **Inferencia, media-alta:** la carga masiva durante una transición expuso más actores con capas todavía no inicializadas.
- **Hipótesis, media:** una ruta de actualización no cubierta por las guardias accedió a una capa vacía. Los contadores prueban que otras rutas sí se evitaron, no que ésta causara el fallo.
- **No demostrado:** RIP causante, corrupción por caballo, culpabilidad de SetFieldTimeReq o causa del segundo cierre.

## 3. Paquetes que entran antes de terminar la carga

### 3.1 Reinicio de tarde: build6B865217

`t` es relativo al primer paquete de174707. Esta sesión sobrevive: demuestra una barrera ausente, no la causa del crash anterior.

| t(s) | Mensaje | Datos |
|---|---|---|
|44.975399|2437 GameLoadingStartReq C>S|Payload0; abre carga.|
|50.554265–56.480858|2005 ActorLoadDataAck S>C|18 copias B01F0019..0029 y002C. Primeros tamaños8261/8371/8247B.|
|51.072002|2005 S>C|Usuario9010F001 y jugador remoto,13013B. Inicio `09 00 00 01 F0 10 90...`.|
|51.072011|2005 S>C|CuerpoB010F001,6178B. Inicio `20 18 07 01 F0 10 B0...`.|
|62.164698|2005 S>C|Caballo con registro de usuario9010F001,8927B. El log17:48:33.804 confirma enqueue con primera acción.|
|62.577871|2445 LoadingCompleteReq C>S|1B: `01`.|
|62.584200|2762 LoadingCompleteAck S>C|13B: `01 00 10 A0 01 EE 85 01 00 00 00 00 00`.|
|74.980460–83.987223|Otra carga2437→2762|En medio siguen acciones de copias existentes.|
|87.679604|2125 SetFieldTimeReq C>S|36B, después de ambas cargas.|

El mod anuncia back in the world a 17:48:22.395 y prepara jugador/caballo a 17:48:22.747, unos11s antes de LoadingCompleteAck. Una posición legible no implica carga terminada. El reloj confiable valida una fuente temporal; no valida el ciclo de carga.

### 3.2 Noche2: agujero confirmado en HEAD

| t(s) | Mensaje |
|---|---|
|152.938573|C>S2437, payload0.|
|**156.289**|**S>C2991 StartActionAck,725B, claveB010F002.**|
|161.704715|C>S2445,1B: `00`.|
|161.719350|S>C2762,13B: `01 00 10 A0 00 00 80 02 00 00 00 00 00`.|

Captura textual203438, línea68469:

```text
156.289 S>C StartActionAck 2991 slot -1 len 725 |
02 F0 10 B0 E9 18 FE 27 E9 18 FE 27 07 00 00 00 ...
```

Log G20:37:39.811: `Inject: delivered StartActionAck of the horse copy (730 bytes)`. H≈21:37:40.411. Usando ese paquete como ancla, la carga va aproximadamente de G20:37:36.461 a G20:37:45.241.

A G20:37:40.042 el local ya está en (-10021.60,715.70,-2446.77), dentro del intervalo. El centinela(0,1000,0) no lo detecta. También avanzan reloj y animaciones durante el túnel: pausa y carga son estados distintos.

En noche2 no se observaron nuevas cargas de copias ni SetFieldTimeReq dentro de las ventanas2437→2762 examinadas. La acción del caballo sí está probada; las demás rutas conservan riesgos estáticos descritos abajo.

## 4. Auditoría de las guardias de HEAD

| Ruta y líneas | Guarda existente | Qué falta |
|---|---|---|
|Jugador remoto: `coop.cpp:243`→`puppet_spawn():1012`|local_pos presente y espera tras despawn.|Finalización de carga. g_spawned se marca antes de entregar las cargas.|
|Caballo: `remote_puppet.cpp:937`, `horse_event():1472`|Primera acción, copia del jugador presente y not-before.|Carga local. Carga y primera acción se encolan como movement=false.|
|Jugador, acciones y movimiento: `coop.cpp:461–476`|puppet_spawned y offset de reloj.|No consulta local_in_world ni carga; pausa solo limita movement=true al entregar.|
|Equipo/relays fiables: `coop.cpp:381–391`|puppet_spawned, conserva plantilla.|No gate de carga. Equipo va como movement=false, incluso con retraso1s (`remote_puppet:1154`).|
|Nuevos enemigos: `enemy_relay.cpp:1713`|Reloj decidido/nozero, no aparcado,300m si hay posición, acción, cuarentena y ritmo.|Protege al productor; no a paquetes ya en cola ni copias existentes. Decided admite −1: lector apagado con estimación, no solo trusted.|
|EnemyPos/EnemyAct: `coop.cpp:453`, `remote_puppet.cpp:1345/1369`|Offset, clave, tamaño, posición.|movement=false elude pausa/TTL de movimiento. No exige local_in_world.|
|EnemyState: `coop.cpp:537`|Validación específica.|No gate local de carga.|
|Hora: `coop.cpp:445`, `time_sync.cpp:66–102,150–190`|local_in_world con500ms de gracia; no paused; relojtrusted, anchor y límites.|No lifecycle. C>S puede esperar hasta3s sin epoch. No ocurrió durante las ventanas medidas.|
|Daño: `coop.cpp:436–450`|local_in_world y reglas por ruta.|Comprobación anterior al consumo; no invalida pendientes al comenzar una carga.|
|Botín: `loot_twin.cpp:207`, `loot_tick():268`|Hit reciente, posición/distancia y límites.|Spawn/kill/rescate C>S sin lifecycle general; loot_tick sigue ejecutándose sin mundo local listo.|
|Restauración de nativos/pines|Reglas de zona/pin.|Usan la cola común; la señal g_loading_screen de Suppress no gobierna todas las inyecciones.|

### Punto común que permite el agujero

`remote_puppet.cpp:243` define Pending con due_ms, movement y packet, sin epoch. `puppet_take_injection():2344` descarta únicamente movimiento atrasado2s, y durante pausa salta solo movement=true. Cargas, estados, equipo y los movimientos de enemigos marcadosfalse siguen entrando.

`troc_sniffer.cpp:227–268` consume hasta64 paquetes propios **antes del paquete real** que disparó OnRecvPacket. Suppress observa la retirada nativa después (`:274–279`): si ese paquete anuncia retirada19 por carga, las inyecciones pueden adelantarse a su detección.

`on_post_packet():159–201` también vacía nuestros C>S antes del Req real; QueuedC2S (`:127`) no tiene epoch. Si ese Req es2437, una petición de hora, spawn o daño pendiente puede publicarse justo al iniciar carga. TTL3s limita antigüedad, no pertenencia al mundo actual.

SEH en `guarded_post():138` y `guarded_inject():212` cubre una excepción síncrona dentro de la llamada, pero no un crash posterior de un job de animación/recursos. Tampoco deshace un estado parcialmente escrito. Un retorno exitoso del wrapper no certifica seguridad en los frames posteriores.

## 5. Señal de mundo preparado

Señal primaria observada: 2437 GameLoadingStartReq abre la carga; 2445 LoadingCompleteReq y2762 LoadingCompleteAck la terminan. La tabla troc2976 asocia handlers2437=29C59D0,2445=29C61A0,2762=B8BB00. Los tamaños0/1/13B están medidos. Se verificaron prólogos y validaciones de tamaño en disco; no se atribuye significado a todos los campos del Ack.

Máquina propuesta: `Unknown → Loading → AwaitLocalReady → Ready`, separada de Paused.

- Cerrar inmediatamente en2437, desconexión/relogin y, como respaldo, retirada19 o pérdida estructural del actor local. Incrementar epoch una vez por transición; deduplicar retirada19 de una carga ya abierta.
- Una señal explícita de carga domina los500ms de gracia. El debounce sigue siendo útil para fallos breves de lectura física fuera de carga.
- 2445 no abre.2762 del jugador local marca fin observado para la epoch actual. No abrir dentro del hook de entrada antes de que el juego haya procesado ese Ack.
- Reabrir tras fin observado y evidencia local fresca: raíces/vtables verificadas, clave correcta, componentes válidos, estado/login listos, posición física/global coherente y finita, fuera del aparcamiento, reloj válido para esa generación.
- Dos muestras coherentes separadas, por ejemplo100ms, añaden histeresis operativa. Ese plazo requiere prueba; no es garantía formal de finalización.
- Marcar «Ack procesado» desde un punto posterior seguro del handler/ciclo de juego. Si solo se observa su entrada, permanecer en AwaitLocalReady hasta una actualización de mundo posterior. No adivinar offsets de epílogo ni sustituir hooks sin verificar firma.
- PlayerReader usa `World+30→manager+50→ControlledActor` y valida vtables. El `UserActor+D8` de EquipHide está expresamente sin confirmar para esta build en `orcax-equiphide.md:64`; no usarlo como única garantía.
- Un menú normal no incrementa epoch. Existencia o posición real del actor tampoco bastan: noche2 lo demuestra.

## 6. Arreglo propuesto, NO aplicado

### 6.1 Barrera central al consumir

Añadir estado local compartido entre sniffer, CoopLink y ambas colas. Los hooks publican flags/contadores atómicos; toda lectura de punteros del juego usa SEH/VirtualQuery. No introducir trabajo complejo del motor en el hook.

```diff
 // troc_sniffer.cpp:on_post_packet, antes de línea163 y antes de drenar C>S
 capture(...);
 if (t_posting) return;
+guard.observe_native_c2s(op, bounded_payload);
+// 2437 len0: comenzar carga.2362 puede dar aviso temprano, tras validarlo.
 ...
 for (auto& q : mine) {
+  if (q.world_epoch != guard.epoch() || !guard.ready()) {
+    count_drop(q, "world-not-ready-or-old-epoch"); continue;
+  }
   guarded_post(...);
 }

 // troc_sniffer.cpp:on_recv_packet, antes de línea238
 capture(...);
 if (!g_injection_enabled || t_injecting) return;
+guard.observe_native_s2c(op, bounded_payload);
+// Remove2516 reason19 cierra ANTES de consumir.2762 marca fin observado,
+// no Ready en esta misma entrada. El paquete nativo siempre continúa.
 for (...) {
   if (!puppet_take_injection(pkt)) break;
+  if (!guard.ready() || pkt_epoch != guard.epoch()) discard;
   guarded_inject(...);
 }
```

La comprobación al consumir es necesaria: el productor pudo ver Ready antes de que comenzara la carga. Publicar epoch/estado de forma coherente y revalidar inmediatamente antes de la llamada. Documentar cómo se serializa la transición frente al consumidor; un bool leído un segundo antes no basta.

```diff
 // remote_puppet.cpp:Pending, línea243
 struct Pending {
   uint64_t due_ms;
+  uint64_t world_epoch;
+  InjectionClass kind; // LoadBundle, StateSnapshot, TransientAction, Motion, Removal, UI
   bool movement;
   Bytes packet;
 };
 // enqueue_at:278 etiqueta epoch; rechaza transitorios cuando !Ready.
 // puppet_take_injection:2344 elimina epoch vieja y comprueba Ready.
 // QueuedC2S:127 añade epoch; sniffer_inject_c2s:520 también etiqueta.
```

No guardar miles de acciones para soltarlas al finalizar. Invalidar paquetes de epoch anterior; conservar plantillas/últimos estados declarativos fuera de la cola para reconstruir. `purge_movement():292` no basta: deja cargas, equipo y EnemyPos/Act marcadosfalse.

### 6.2 Productores, acciones iniciales y recuperación

- `CoopLink::update:83`, `local_in_world_:89`: una carga explícita domina la gracia; separar Ready de posición temporalmente disponible.
- `coop.cpp:243`: nuevos spawns exigen Ready. Mantener el SpawnInfo más reciente para recrear después; no perderlo al resetear remote_spawn si la entrega se canceló.
- `on_event:453–476` y `deliver_ordered:381`: no inyectar transformaciones, acciones o equipo durante carga. Sí se puede actualizar el último estado declarativo que se aplicará al recrear.
- `enemy_relay::load_waiting:1713`: añadir la compuerta conservando acción inicial,300m,cuarentena,ritmo y validaciones.
- `horse_event:1472`: carga y primera acción forman un bundle de igual epoch, aceptado completo o cancelado completo. Si cambia epoch entre encolados no debe sobrevivir solo la carga.
- `puppet_spawn_enemy:1266–1283`: convertir/validar acciones antes de encolar la carga. Actualmente la carga entra primero; una conversión fallida o cola llena podría separarla de su acción. Este riesgo estático no se ha demostrado como causa de la captura.
- `puppet_forget:1219` no es una purga completa: borra solo movement y retorna pronto si !g_spawned. La invalidación de mundo debe ser independiente de g_spawned.
- Durante Loading no inyectar remociones para un mundo que se está destruyendo. Retener claves de posibles supervivientes y reconciliarlas ya en Ready: resolver de nuevo, retirar lo necesario y recrear una sola vez. Algunas marionetas sobrevivieron viajes en capturas anteriores; olvidar el flag y recargar ciegamente generaría duplicados.

### 6.3 Resync y protocolo

Vaciar la cola sin reconciliar g_copies/g_sent pierde enemigos: el host ya recibió el ACK fiable de Spawn. Cerrar EnemyZone al cargar y solicitar reconstrucción tras Ready, coordinada con P2.

Puede reutilizarse EnemyZone OFF/ON si se garantiza una nueva seq y reenvío de todos los enemigos vivos. Si se introduce EnemyResync o epoch remoto en el protocolo, subir kVersion y actualizar ambos PC. Una epoch puramente local en las colas no cambia el protocolo.

### 6.4 Movimiento de enemigos

No cambiar simplemente false→true en `remote_puppet.cpp:1345/1369`: purge_movement actual elimina movimiento globalmente y podría borrar estados de enemigos al recargar solo al compañero. Separar:

- pertenencia: clave, epoch y bundle;
- semántica: posición efímera, acción o estado;
- purga: por actor/epoch;
- pausa: caducidad propia;
- carga: toda inyección modificadora espera o se cancela.

La primera posición del spawn pertenece al bundle inicial. Las posiciones continuas viejas no se reproducen al terminar el túnel.

### Riesgos y límites del arreglo

1. Filtrar tráfico nativo impediría terminar la carga. La barrera solo afecta a nuestras colas;2437,2445,2762 y el resto del tráfico real deben continuar.
2. Gate sin resync deja copias ausentes permanentemente; diseñar recuperación antes de activarlo.
3. Perder relays fiables sin conservar vida/equipo declarativos puede recrear una copia viva cuando el remoto está muerto.
4. Fallos de lectura física de50–120ms no deben crear una epoch por frame.
5. La barrera no arregla por sí sola un actor falso ya existente que el motor toque durante unload. Sin dump, esa causa queda abierta.
6. No alterar Status, alfa, IA o parámetros de spawn a ciegas. Hooks en mitades de función: safetyhook::create_mid, validación de bytes y build.

## 7. Sondas exactas

Registrar transiciones y contadores; usar anillo fijo dentro de hooks y escribir texto fuera del hilo del motor.

```text
WorldGate epoch=12 old=Ready new=Loading reason=GameLoadingStartReq
 nativeOp=2437 dir=C>S thread=... monotonic=... gameMs=... localKey=...
WorldGate epoch=12 phase=CompleteSeen op=2762 key=A0100001 bytes=13
WorldGate epoch=12 old=AwaitLocalReady new=Ready
 actorState=10 login=1 physicsValid=1 parked=0 clockSource=trusted readySamples=2
InjectAudit epochQueued=11 epochNow=12 op=2991 actor=B010F002
 class=TransientAction action=DROP reason=old-epoch ageMs=...
SpawnBundle id=... epoch=12 actor=B01F.... loadBytes=... actions=... move=1
 result=delivered|cancelled reason=... nativeOpAtDrain=...
```

Contador `injectedWhileLoading` por clase/sentido: debe ser0, incluyendo hora, pines, equipo, botín, acciones y movimientos. Separar estados del bundle: accepted,delivered,cancelled. No confundir g_spawned con entrega confirmada.

Conservar los últimos128 eventos de lifecycle/inyección para diagnóstico. No añadir format, locks, allocations o I/O complejo en un handler de crash. Si se necesita cambiar configuración de dumps en Shadow, hacerlo aparte: este análisis no la configura ni descarga herramientas.

## 8. Prueba en juego

1. Aplicar los cambios. Tests sin juego útiles: enqueueReady→Loading→consume descarta; tráfico nativo pasa; bundle no se parte; pausa no cambia epoch; purga no depende de g_spawned.
2. Respaldar partida conforme a las reglas del proyecto (CONTRIBUTING.md) antes de la futura build de prueba. Host cerca de enemigos y con caballo; guest conectado.
3. Guest viaja mientras host camina, monta/desmonta y combate.2437 debe cerrar inmediatamente; no debe haber ninguna entrega propia dentro de la ventana de carga.
4. Reproducir el caso difícil: local con posición real o reloj avanzando durante el túnel. La barrera sigue cerrada hasta lifecycle completo y Ready coherente.
5. Al volver: una copia de jugador/caballo, enemigos correctos, sin acciones antiguas ni golpes acumulados. Nueva epoch, resync y bundles con acción inicial válida.
6. Repetir muerte/reaparición del guest, menú→cargar partida y carga inicial larga, con host activo y pausado. Guardar logs/capturas de ambos PC antes de que roten.
7. Menú sin carga: no cambia epoch; se conservan equipo/vida y no aparece una ráfaga de movimiento viejo.
8. Si vuelve a cerrar, conservar el dump antes de reiniciar. Sin RIP/pila no certificar la causa histórica. El éxito medible del arreglo es cero inyecciones durante Loading y recuperación correcta, además de ausencia de cierres en las repeticiones.

## 9. Preguntas abiertas

- RIP/pila del cierre17:46:35 y evidencia independiente del otro cierre.
- Punto posterior seguro al procesamiento de2762 para abrir Ready sin coincidir con un job de unload.
- Necesidad de teardown especial de marionetas sobrevivientes o suficiencia de reconciliación al volver.
- Histeresis adecuada para no reaccionar a lecturas físicas transitorias.
- Prueba específica de hora y botín durante carga: existen caminos en HEAD, pero SetFieldTime no ocurrió dentro de las ventanas examinadas.
