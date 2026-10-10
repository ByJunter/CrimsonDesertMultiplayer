# P2 — Pelea tras muerte y reaparición del anfitrión

**Corte:** 7 de octubre de 2026, noche 2. Código `57bec36bdad634a502eb587f8f899c090ca162c5`. Juego 1.0.0.2976, PE 0x6AB28F00, base 0x140000000. Solo análisis; propuestas sin aplicar.

## Resumen (8 líneas)

1. **Hecho:** después de reaparecer H, G produce 21 impactos contra dos copias; 15 bajan la vida en H **y en G**, y 6 se rechazan por el intervalo de 50 ms.
2. **Hecho:** los primeros 10 impactos aceptados ocurren **antes** de que esos enemigos adquieran a H. El permiso de zona para iniciar combate funciona.
3. **Hecho:** esa sesión no contiene ningún AttackTargetAck de P contra un enemigo; el mod no obtiene ninguna plantilla de reacción. El daño se calcula con estadísticas persistidas, pero las reacciones no.
4. **Hecho:** `enemy_note_guest_blow` solo actualiza tablas del mod; no cambia el objetivo de la IA. Los dos enemigos no eligen a G' y solo adquieren a P a las 21:48:16.577/17.268.
5. **Hecho:** el filtro de 50 ms elimina impactos con distinto fotograma; 6/21 = 28,6 % en la segunda pelea. Un lote también comprime en H impactos separados 137 ms en G.
6. **Hecho:** las copias 21/22 reciben alrededor de un 50 % de movimientos tarde para su búfer; sus movimientos cambian cada ~229 ms de mediana frente a ~134 ms en el control previo (P3).
7. **Hecho:** la zona de G permanece ON hasta la salida final; no hay devoluciones de nativos ni una oscilación OFF/ON durante la reaparición.
8. **Conclusión:** hay daño, falta una reacción autónoma y adquisición de objetivo, y existe un problema separado de temporización de enemigos. No es una regresión demostrada del permiso de daño al morir H.

## 1. Fuentes y precisión temporal

- H log: `D:/SteamLibrary/steamapps/common/Crimson Desert/bin64/cdmp.log`.
- H captura: `cdmp_packets_20261007-213534.bin`, mismo directorio.
- G log/captura: log del invitado `cdmp.log` (7 oct, sesión noche2) y `cdmp_packets_20261007-203438.bin`.
- Extracción reproducible: `build/research/ia2/p2_capture_audit.py`; salida acotada `p2_events.json` (solo operaciones relevantes y contadores por segundo; no copia íntegra de captura).
- Cadencia/lead: extracción P3 `p3_noche2_enemies.csv` y método de [P3-retraso.md](P3-retraso.md).
- Leídos: README.md, ROADMAP.md, `copias-sin-dano-7oct.md`, `zona-tras-muerte-anfitrion.md`, `copia-objetivo-sin-ataques.md`, `pendiente-aggro-y-respuesta-golpes.md`, código actual.

**Convención:** P = jugador H 0xA0100001; G' = copia de G en H 0xA010F001. Copias de enemigos en G = 0xB01Fxxxx. Las claves nativas son locales a H, no identificadores persistentes.

**Anclajes medidos:**

| Captura | Conversión a hora local de su PC | Evidencia |
| --- | --- | --- |
| H | 21:35:50.545386 + t | Primer 2703 capturado emparejado con `Inject C>S ... posted` a 21:42:32.485 |
| G | 20:35:03.525015 + t | Primer DoFrameEventReq contra copia emparejado con `we hit the copy` |

Las horas cruzadas usan H ≈ G + 3600,6 s, según la alineación estimada de relojes; no tiene precisión de microsegundos. Los t de captura sí son los registrados. La tabla conserva ambas horas sin ocultar ese error de sincronización. Latencias DFE → HP en el **mismo G** no dependen de la diferencia entre relojes.

**Límite:** CDPK captura Troc; no captura el datagrama externo `GuestHit` ni `EnemyPos`. “Impacto de G” abajo significa el DFE que genera GuestHit en `guest_hits.cpp:520–553`. Para los 21 de la segunda pelea se cierra la contabilidad con efectos y rechazos de H; no equivale a una captura de cada datagrama Steam.

## 2. Cronología desde las muertes hasta el fin

| H | G observado / equivalente H−3600,6 s | t H / t G | Evidencia y efecto |
| --- | --- | --- | --- |
| 21:43:43.822 eq. | **20:43:43.222** | — /519.697148 | G muere primero, 2530, víctima A0100001 |
| 21:43:53.755 | 20:43:52.639 revive G | 483.209 /529.114 | H recibe 3053 para **A010F001**; G había enviado ReviveReq y recibido 3053/2475. No asumir que la muerte posterior de H es la primera muerte de la sesión |
| **21:44:03.445** | 20:44:02.845 eq. | **492.899604** /— | H muere: 2530, asesino B0100316 |
| 21:44:04.306 /04.453 | DFE **20:44:03.149 /03.380** | 493.761 /493.908; G 539.623 /539.855 | G golpea copia 02 del asesino: 26AA9809/6 y /8; H publica −13 y −14 HP. Morir no bloquea el daño durante la animación |
| **21:44:08.593–15.801** | 20:44:07.993–15.201 eq. | aprox 498.048–505.256 | Primera pausa de H. Entre 21:44:09 y 14 hay 0 C>S y 4 S>C en segundos completos de captura |
| 21:44:14.898 /14.912 | 20:44:14.298 /14.312 eq. | 504.352 /504.367 | ReviveLoadingReq 2362 (2 B `02 00`) y ReviveLoadingAck 2993 |
| **21:44:15.041** | **20:44:14.479** | **504.495 /550.954** | El mundo H retira 25 nativos que tenían copia; G recibe 25 RemoveActorAck de sus copias |
| **21:44:17.419** | **20:44:17.016** (NotifyHandleReviveAck de P') | 506.873 /553.491 | H recibe NotifyHandleReviveAck 3053 y ReviveAck 2475; H está de nuevo vivo. G mantiene zona ON |
| 21:44:40.056 /44.742 | 20:44:39.456 /44.142 eq. | — | Nuevas claves nativas 05EB/05EC/05FA → copias 1C/1D/1E; no se reutilizan los enemigos de la pelea anterior |
| 21:44:50.600–21:45:00.179 | 20:44:50.000–20:44:59.579 eq. | — | Otra pausa H; no confundir todo 21:44:08–21:45:29 con una única pausa |
| 21:45:02.849–10.315 | 20:45:02.249–09.715 eq. | — | Otra pausa H |
| 21:45:13.604–29.473 | 20:45:13.004–28.873 eq. | — | Otra pausa H |
| 21:45:43.796–21:46:54.138 eq. | **20:45:43.196–20:46:53.538** | — | G también abre pausa: `Puppet: this game is paused/running again`. Importa para colas y movimiento, independientemente de H |
| 21:46:35.028 | 20:46:35.568 removidas | 644.482 /692.043 | H retira 05EB/05EC → G retira 1C/1D |
| 21:46:35.560 | 20:46:34.983 carga | — | Nuevo 0695 → 1F |
| **21:46:51.081** | **20:46:50.499 /50.547** | — | H anuncia 06BD → 21 y 06BE → 22, Black Bear, variantes 54018 y 54022. Son los dos enemigos del problema observado después |
| 21:47:36.218/36.251 | 20:47:35.636 | 705.672 /752.111 | 06BA/copia 20 retirada; no es 21/22 |
| 21:47:41.464–45.777 | 20:47:40.896–45.230 cargas | — | Nuevas copias 27–33, aumento de carga de enemigos |
| **21:47:52.986–21:48:06.326** | 20:47:51.933–20:48:06.021 | — | 12 impactos: 10 aceptados/HP confirmados y 2 rechazados 50 ms, mientras 21/22 todavía no tienen a P ni G' como objetivo |
| **21:48:16.577/17.268** | 20:48:15.977/16.668 eq. | **746.031975 /746.723049** | 06BD/06BE eligen **P**, motivo 18. Primer CTC observado de ambos; no eligen G' |
| 21:48:21.351–22.973 | 20:48:20.356–22.539 | — | 9 impactos: 5 cambios de HP confirmados y 4 rechazos 50 ms |
| 21:48:23.578 | 20:48:23.024 | 753.033 /799.499 | Retira 07BC/07BD/07BE → copias 41/42/47, no 21/22 |
| **21:48:28.060** | 20:48:27.460 eq. | **757.514614** | Segunda muerte H, asesino 06BD; G ya no genera más impactos contra copias |
| 21:48:28.084 → 29.713 | 20:48:27.484 → 29.113 eq. | 757.538 → 759.168 | 06BD/BE limpian objetivo y pasan al NPC B010054D. No pasan a G' |
| 21:48:33.479–21:49:03.105 | 20:48:32.879–20:49:02.505 eq. | — | Pausa H; entre 21:48:35–21:49:00: 0 C>S y 17 S>C en segundos completos |
| 21:48:57.215 eq. | **20:48:56.615** | — | G sale a carga: `38 copies ... removed (zone off)`; `zone OFF (loading screen), nothing to give back (0 held forgotten)` |
| 21:49:02.361 | G ya dejó zona | 791.816 | H retira 38 nativos copiados, durante nueva reaparición |
| 21:49:04.739 | — | 794.194 | H recibe 3053/2475 otra vez |
| 21:49:10.887 | — | — | Última pausa H registrada. No hay otra pelea G en estos datos |

**Retirada de las 25 copias de la primera pelea:** 15,13,0B,07,14,16,18,17,0A,08,0C,10,1B,1A,02,09,03,05,04,06,0D,0E,0F,11,12 (sufijos hex de B01F). No son resends con la misma clave: las nuevas 1C–47 corresponden a claves nativas posteriores.

### 2.1 Zona, epochs y duplicados

**Hechos:** G log: ON 20:40:45.519 (línea 706) hasta OFF 20:48:56.615 (2204). No `st.paused` de gracia, no `resumed`, no OFF intermedio. `restored 0`, `swept 0`, `deferred 0` durante toda la zona. A 20:44:14.860: held 25, dropped 56, removed-while-held 31; a 20:44:24.869: held 18, dropped 56, removed 38; a 20:48:25.061: held 53, dropped 91, removed 38.

**Inferencia fuerte:** no es el antiguo ciclo de devolver nativos al reaparecer H. H reaparece en (−10172.66,674.37,−2156.90); G está en (−10200.15,675.64,−2259.70): ~106 m horizontales, dentro de off 250 m. `enemy_zone.h:101–109` conserva zona sin activar gracia si el salto queda cerca.

**Código:** las retiradas nativas llegan por `enemy_relay.cpp:1153–1167`, borran loads/actions/state y `g_sent` mediante `remove_locked:728–735`. `g_partner_epoch` solo aumenta en `enemy_reset_partner:1927–1944`, invocado al reiniciar socio en `coop.cpp:378`; no al morir. No hay log de cambio/reconexión del socio durante la reaparición. **No existe contador de epoch en el log**, por lo que la ausencia de cambio se infiere de esa ruta y la continuidad, no se midió una variable.

No hay líneas de cambio de radio `zone radius`, ni de salto de muestra `read at ... skipped`. No prueba el radio real por memoria, pero no hay evidencia de una reducción dinámica. No hay evidencia de duplicados causados por restauración. No excluye cualquier nativo fuera de los criterios de supresión: haría falta una sonda por clave/variante/posición para afirmarlo globalmente.

## 3. Los impactos posteriores, uno por uno

**Hecho:** copy 21 ↔ B01006BD variante 54018; copy 22 ↔ B01006BE variante 54022. Las 15 operaciones 2703 tienen 15 respuestas 2147 en H y 15 copias 2147 en G con la misma vida. Respuesta servidor H: 1,5–26,9 ms después del 2703. La vida no se perdió en la red.

Horas locales de cada PC; “rechazo” cita hora de log H. t G entre paréntesis. Las filas de igual acción/varios objetivos se separan para contar impactos, como hace `guest_hits_observe_c2s`.

| # | G DFE (t G) | Copia | Acción/fotograma | H 2703 o rechazo | HP final H=G / G recibe |
| --- | --- | --- | --- | --- | --- |
| 1 | 20:47:51.933 (768.408) | 21 | 73FA3F0A/10 | 21:47:53.002 −4 | 116 /20:47:52.705 |
| 2 | 20:47:51.975 (768.450) | 21 | 73FA3F0A/11 | 21:47:52.995 rechazo 50 ms | — |
| 3 | 20:47:52.851 (769.326) | 21 | F104F0B7/4 | 21:47:53.912 −3 | 113 /20:47:53.493 |
| 4 | 20:47:55.514 (771.989) | 22 | 7C16DAAF/13 | 21:47:56.548 −13 | 107 /20:47:56.256 |
| 5 | 20:47:55.515 (771.990) | 22 | 7C16DAAF/14 | 21:47:56.544 rechazo 50 ms | — |
| 6 | 20:47:56.341 (772.816) | 22 | F104F0B7/4 | 21:47:57.433 −14 | 93 /20:47:57.117 |
| 7 | 20:47:56.710 (773.185) | 22 | 0F357AC7/14 | 21:47:57.815 −7 | 86 /20:47:57.522 |
| 8 | 20:47:56.888 (773.363) | 22 | 0F357AC7/24 | 21:47:57.961 −7 | 79 /20:47:57.669 |
| 9 | 20:47:57.170 (773.645) | 22 | 0F357AC7/39 | 21:47:58.227 −6 | 73 /20:47:57.927 |
| 10 | 20:47:57.365 (773.840) | 22 | 0F357AC7/49 | 21:47:58.462 −14 | 59 /20:47:58.154 |
| 11 | 20:48:05.077 (781.552) | 21 | F0CB3CF9/6 | 21:48:06.154 −3 | 110 /20:48:05.889 |
| 12 | 20:48:05.150 (781.625) | 21 | F0CB3CF9/8 | 21:48:06.310 −3 | 107 /20:48:06.021 |
| 13 | 20:48:20.356 (796.831) | 21 | F0CB3CF9/6 | 21:48:21.351 −3 | 104 /20:48:20.913 |
| 14 | 20:48:20.434 (796.909) | 21 | F0CB3CF9/8 | 21:48:21.583 −3 | 101 /20:48:21.150 |
| 15 | 20:48:21.226 (797.701) | 21 | 5A2EA805/4 | 21:48:22.266 −3 | 98 /20:48:21.828 |
| 16 | 20:48:21.793 (798.268) | 22 | 267BE8EF/11 | 21:48:22.929 −7 | 52 /20:48:22.537 |
| 17 | 20:48:21.793 (798.268) | 21 | 267BE8EF/11 | 21:48:22.960 −2 | 96 /20:48:22.539 |
| 18 | 20:48:21.930 (798.405) | 22 | 267BE8EF/13 | rechazo del lote final | — |
| 19 | 20:48:21.930 (798.405) | 21 | 267BE8EF/13 | rechazo del lote final | — |
| 20 | 20:48:21.951 (798.426) | 21 | F653A168/0 | rechazo del lote final | — |
| 21 | 20:48:21.952 (798.427) | 22 | F653A168/0 | rechazo del lote final | — |

**Precisión del emparejamiento:** filas 1–15 se contrastan además con log de acción/daño H cuando existe. En 16–21, el contador de log de golpes aceptados ya se agotó. El lote contiene 2 cambios de HP (−7 y −2) y 4 rechazos, por las mismas copias, a 21:48:22.940/22.949/22.959/22.972. Asignar los dos aceptados a /11 se infiere de sus daños calculados y orden; **no puede recuperarse del log el orden exacto entre /13 y F653 rechazados**. No se presenta una correspondencia individual de esos cuatro como hecho.

La latencia G DFE → G VaryHP de los 15 aceptados, con ese emparejamiento, es **557–871 ms, mediana 757 ms**. Es actualización de vida, no la latencia del movimiento P3 ni prueba de cuándo se dibujó la barra.

### 3.1 Bytes y offsets que permiten repetirlo

- 2703 VaryStatCheatReq, payload 15 B: `u32 víctima | u32 stat | u8 mode | i32 delta | u8 | u8`.
- Primero segunda pelea, t H 722.456…: `bd 06 10 b0 40 42 0f 00 00 fc ff ff ff 00 00`: BD, stat 1000000, mode 0, delta −4.
- 2147 VaryHPAck, 92 B: víctima+0; HP actual **i64+0x08**, fuente u32+0x54. H primera respuesta t 722.484: HP 116000, fuente A0100001. G t 769.180: misma HP, víctima B01F0021, fuente A010F001.
- 2530 muerte H1, 19 B, t 492.899604: `01 00 10 a0 16 03 10 b0 08 00 01 00 00 00 00 00 00 00 0a`.
- CTC 2984, 18 B, t 746.031975: `bd 06 10 b0 01 00 10 a0 04 00 00 00 00 00 00 00 00 18`.
- DFE 2380: cabecera de fragmento 04; atacante u32+1; accessor+0x1F7; tipo byte+0, hash u32+2, frame u32+8 para melee; objetivos en entradas 665 B desde el final. Ver `guest_hits.cpp:59–61,237–250`.

### 3.2 Qué rechaza realmente el filtro

`guest_hits.cpp:658` compara **hora de procesamiento H por enemigo**, no acción/fotograma ni tiempo original. Rechaza distinto frame aunque sea un impacto nativo válido. Ejemplos reales: /10 → /11 separados ~41 ms en G; /13 → /14 casi simultáneos. En el lote final /11 → /13 están separados 137 ms en G pero llegan/procesan agrupados en H.

**Hecho:** 20 rechazos visibles totales en H son todos 50 ms; 14 antes de la primera muerte y 6 después. En la segunda pelea se cierra 21 = 15 + 6, así que no falta espacio para un rechazo adicional por pausa/zona/clave. No extrapolar esa igualdad a toda la sesión: hay golpes letales, colisiones y retornos silenciosos para cadáveres.

**Hecho:** no hay golpes contra copias durante los intervalos de pausa H posteriores a la primera muerte, aparte de los dos en la animación previa. Por ello no se puede culpar al filtro `our game is paused` de esta segunda pelea.

## 4. La reacción que falta y la IA

### 4.1 Daño y reacción son rutas independientes — causa demostrada

1. Estadísticas de 94 ataques y 17 defensas se cargan al arrancar (H log 44).
2. `guest_hits_learnt_put:487–495` restaura votos y contadores; **no** `ata/aar/buff`.
3. `on_attack_target:298–310` rellena `ata` solo si el atacante es P, objetivo enemigo real, y encuentra DFE reciente. `on_restriction:315–322` y `on_buff:325–339` obtienen el resto.
4. En toda la captura H hay 20 ATA 2922, **solo 1 con fuente P**, y ese es P → P a 21:41:51.758 (t 361.212913), no enemigo. **Cero** ATA source P → enemigo y **cero** ATA source G'. No puede haberse aprendido una plantilla por la ruta anterior.
5. `apply_hit:711–719` intenta cualquier ATA aprendido. Sigue vacío.
6. La reacción completa exige `copy_here && ata.size() >= 0x30`, **línea 776**, incluso para entrar a la rama AAR/buff. No se encola reacción.
7. El log solo añade `[their copy is not here]` si falta la copia; no dice `no reaction template`. Ausencia de “+ stagger” no informa de esta causa: **0 líneas +stagger en toda la sesión**.

**Hecho de código + captura; confianza alta:** el mod no produce la reacción sintética en esta sesión. No es un borrado de plantillas al morir; nunca se obtuvieron. El motivo de que la primera pelea pareciera funcionar es compatible con adquisición/ataques naturales previos de esos enemigos. Esa última explicación de la percepción es **inferencia**, no vídeo analizado.

La defensa de variante 54018 es 0,220 en el log: golpes nominales ~13–16 terminan en 3–4 HP. Esto también hace menos visible el daño; no demuestra que ese factor aprendido sea incorrecto.

### 4.2 Adquirir G' no está implementado al recibir su golpe

`enemy_note_guest_blow:1087–1089` rellena `g_engaged/g_at_us`. Son tablas del mod, no el aggro nativo ni `CurrentTarget`. `apply_hit` no llama a `force_target_on_puppet`.

**Hecho observado:** los dos nuevos osos 06BD/BE reciben 10 cambios HP entre 21:47:53 y 21:48:06 antes de seleccionar a P. No hay CTC a G' de ninguno, tampoco tras la muerte 2. En esta sesión las copias nuevas quedan sin una razón nativa demostrada para atacar a G'.

**Ya aplicado:** CheckAIBattleType está hooked (H log 60) y slots=2 (log 63). `aggro_test.cpp:188–204` solo altera una condición cuando el objetivo **ya es G'** y hay atacante en su lista (`:126–166`). No ayuda a un enemigo que todavía no adquirió G'. No reinstalar la solución antigua de rango como si faltara.

**Hipótesis residual:** puede existir además un estado de elegibilidad de G' tras recargar H (dead/hidden/etc.). No hay lectura de Status 273/390/394 durante la segunda pelea; recibir NotifyHandleReviveAck en H y localizar la posición de G' no demuestra todos sus flags. La prueba propuesta registra esos campos. No afirmar que la copia estaba muerta ni escribirlos a ciegas.

### 4.3 Evaluación de CurrentTargetChangedAck 3040

Desensamblado local rehecho, acotado a `0xB68520–0xB6893F`:

- Deserializa 4 + 4 + 8 + 1 + 1 B en 0xB685CE/0xB6861C/0xB68646/0xB68670/0xB6869A.
- Resuelve actor; obtiene comp de objetivo+0x90.
- 0xB687BB–0xB687C4: si cierto flag de control tiene bit 2 y force=0, sale.
- 0xB687C6–0xB687E7: pasa motivo, force y seq a `SetCurrentTarget0x2217290`.
- Formato 18 B: enemigo+0, objetivo+4, seq u64+8, force u8+0x10, motivo u8+0x11.

La rutina existente `aggro_test.cpp:336–370` sirve de demostración **manual**, no se debe llamar tal cual desde cada golpe: usa `g_last_enemy` que apuntó a P en los últimos 30 s, por lo que no seleccionaría necesariamente el enemigo golpeado y depende justo de que H ya haya sido visto.

**Propuesta concreta:** generalizar a `request_target_on_puppet(enemy_key)`, validar el enemigo de ese golpe y G' vivo/presente, preparar 3040 con `force=1,reason=0x18,seq=current+1`, y emitir por la cola normal en hilo cliente. Leer seq protegido **al entregar**, no segundos antes en el hilo red. Nunca `seq+N` grande: complica los contadores IA. No llamar a 0x2217290 desde el worker, no forzar +0xC8 de G'.

**Limitación:** fijar objetivo una vez no garantiza conservarlo ni superar cada condición de ataque. Las investigaciones anteriores observan retarget en <1 s en algunas circunstancias. Primero medir CTC, resultado del mid-hook y DFE dirigido a G'; si cambia inmediatamente, aplicar una asignación breve y restringida en el **mid-hook de entrada ya existente** `on_set_target:284`, solo cuando la IA intercambia P/G' y con G' elegible. No secuestrar NPCs, limpiezas motivo 02/04/09 ni objetivos de misión. Esta segunda fase requiere validación en juego.

## 5. Tirones: medidas y descarte de sospechas

Medidas P3, ventanas expresadas con G + 1 h (su script no añade los 0,6 s aproximados). “Tarde” = ts tras InjectClock menos (renderclock = gameclock − 200 ms) < 0; son Acks que entraron, no el datagrama EnemyPos original.

| Ventana / copia | MoveActorAck | Tarde, con reloj medible | Intervalo entre posiciones que cambian >5 cm, p50/p95 |
| --- | ---: | ---: | ---: |
| Antes, 21:42–21:43:30 /03 | 554 con reloj | 6/554 =1,08 % | 134,904 /270,144 ms (548) |
| Antes, misma /04 | 486 con reloj | 6/486 =1,23 % | 134,180 /287,812 ms (468) |
| Segunda pelea 21:47:45–21:48:28 /21 | 71 | 36/71 =50,70 % | 229,084 /447,841 ms (38) |
| Segunda pelea /22 | 61 | 29/61 =47,54 % | 229,086 /658,554 ms (26) |

Lead mediano 21 = −2,333 ms; 22 = +10,843 ms. **Hecho:** la interpolación queda sin margen muy a menudo en estos dos enemigos. **Inferencia alta:** esto produce su movimiento discontinuo. No es prueba de fallo en todos los enemigos ni de bloqueo de la IA.

| Sospecha | Veredicto |
| --- | --- |
| `g_sampled` viejo / kMaxSpeed | No explica 21/22: claves nativas 06BD/BE nuevas tras reaparición, primer Sampled sin historial. No hay `skipped` en H. Por código 1430 un rechazo tampoco adelanta last.ms, así que el límite se abre con el tiempo. Sí hay deuda de limpieza: remove_locked no borra Sampled; corregir lifecycle en hilo dueño, pero no presentarlo como causa observada |
| `g_last_move` viejo | No demuestra causa: puppet_enemy_move 1322–1345 usa la plantilla pero reemplaza clave, posición, rotación, parent y timestamp; en segunda pelea hay MoveActorAck para ambas. Auditar campos que quedan heredados si se reproduce distinto, sin atribuirlo por edad |
| Pausa/clock | Sí hay varios cambios grandes del offset, pero son esperables con pausas distintas. Hace falta controlar lead/cadencia al entregar, no fijar offset a 0. P3 identifica muchos Acks de enemigo al borde/tarde |
| Duplicados nativos/copia por gracia | No hay restauraciones ni OFF/ON durante reaparición. No apoyado por datos |
| Bucle red | El jugador remoto mejoró; enemigos mantienen cadencia peor y lead insuficiente. Ver P3 para separar tiempo de muestreo/enqueue/inyección, sin atribuir cada hueco a Steam |
| Clasificación de movimiento | **Defecto de código confirmado:** EnemyMove 1345 y EnemyAction 1369 se encolan con `movement=false`; el descarte stale 2350 y la pausa 2363 solo actúan sobre `movement=true`. Los enemigos pueden acumular/reproducir pasado de una pausa que el jugador descarta. No basta para atribuir por sí solo todo el tirón a 21:47; se necesita medir edad de cola |

No llamar “pérdida de red” a pausas de 900 ms: `enemy_host_tick:1441–1448` envía keepalive al estar parado. Se separaron posiciones con cambio >5 cm.

## 6. Cambios propuestos, sin aplicar

### A. Hacer explícita la adquisición tras un golpe — impacto alto, riesgo medio

**Archivos/funciones actuales:** `guest_hits.cpp:620 apply_hit`, `enemy_relay.cpp:1087 enemy_note_guest_blow`, `aggro_test.cpp:322 read_target_seq /336 force_target_on_puppet /284 on_set_target`.

```cpp
// Después de validar enemy, zona, estado, distancia y deduplicación:
enemy_note_guest_blow(enemy);
if (native_target(enemy) != kPuppetPlayer &&
    puppet_alive_visible() && enemy_alive_visible() &&
    cooldown_per_enemy(enemy, 1000ms)) {
    enqueue_target_intent(enemy, kPuppetPlayer, reason=0x18);
}
// En el punto existente de inyección S>C, hilo cliente y mundo válido:
// validar claves aún presentes; SEH para comp+0x170; no retener punteros.
deliver_3040(enemy, kPuppetPlayer, seq_now + 1, force=1, reason=0x18);
```

Es una solicitud local; no cambia protocolo. Registrar si se aplicó y siguiente target. Si vuelve a P inmediatamente, **solo después de prueba** usar lease por clave ≤2 s en el mid-hook existente para adquisiciones 18/1D/1E entre P/G'. Respetar muerte, supresión, salida de zona, objetivo NPC y limpieza. No convertirlo en “todos atacan siempre al invitado”.

### B. Reacciones disponibles sin un golpe previo H — impacto alto, riesgo medio

**Archivo:** `guest_hits.cpp:687–719,774–804`; persistencia actual `:474–499`.

- Añadir resultado explícito: `reaction=missing_template/ata/aar_buff/no_copy`. Evita diagnóstico “daño no entra” cuando la vida sí baja.
- Separar disponibilidad AAR+buff de disponibilidad ATA; una plantilla ATA no debe ser condición de una rama que deliberadamente no la inyecta.
- Las estadísticas persistidas no son plantillas. Para solución automática, usar recetas previamente verificadas en la misma build, **estructuradas**, sin punteros/claves/timestamps viejos, y reescribir víctima, fuente, instance y clocks como ya hace la ruta actual.
- Si no existe receta verificable, conservar daño y adquisición de A; omitir reacción con log explícito. **No inventar** un ATA a partir de ceros ni persistir dumps enteros con campos desconocidos.
- Prueba mínima para demostrar la causalidad hoy, sin nueva receta: H da un solo golpe real a un enemigo seguro; comprobar ATA nativo capturado, template_ready; luego G golpea otro equivalente. Si ahora aparece reacción, se confirma la ruta independiente. Esto es diagnóstico, no un requisito de uso final.

```cpp
if (copy_here && stagger_enabled && !lethal &&
    aar_valid_for_build && buff_valid_for_build) {
    inject_rewritten_aar_and_buff();
} else if (copy_here && ata_valid_for_build) {
    inject_rewritten_ata();
} else {
    count_and_log_missing_reaction_template(enemy, action, frame);
}
```

No cambiar cálculo de HP, créditos o finalización para solucionar una reacción visual.

### C. Sustituir 50 ms como identificador de golpe — impacto medio, riesgo bajo/medio

**Archivo:** `guest_hits.cpp:536–548,623–628,658–675`; `protocol.h:69,149`.

- Distintos fotogramas de un ataque nativo son eventos distintos; el limitador por tiempo de recepción no es deduplicación.
- Solución robusta: añadir un **hit_id monotónico por sesión** a cada DFE y reutilizarlo para sus objetivos; en H deduplicar `(session,hit_id,enemy)` con ventana acotada. Mismo DFE con dos objetivos no se elimina; una retransmisión repetida sí.
- Mantener validación de distancia, zona, propiedad, daño máximo y límite global 15/s con burst 15. No confiar ciegamente en timestamps proporcionados por otro PC.
- Eliminar rechazo 50 ms universal; opcionalmente limitar por objetivo con token bucket suficientemente amplio para multi-impactos reales, **sin** confundir este control con dedup.
- **Cambiar formato exige kVersion 9 → 10** y actualizar codec/tamaños/pruebas de protocolo; no aplicar ese cambio unilateral a un PC.
- Alternativa temporal sin cambiar protocolo: no aplicar 50 ms si cambia`(hash,frame,type)`, con ventana dedup muy breve; **riesgo:** ataques repetidos con el mismo frame/acción no llevan un identificador de instancia. No es equivalente a la solución robusta.

Prueba offline significativa: reproducir los 21 DFE observados con la misma agrupación H; deben pasar 21 eventos distintos dentro del presupuesto y ningún duplicado del mismo hit_id. Mantener ataques legítimos repetidos de una nueva instancia.

### D. Movimiento enemigo: integrar P3 — impacto alto, riesgo bajo/medio

**Archivo:** `remote_puppet.cpp:1322–1345,1348–1369,2344–2393`; `enemy_relay.cpp:1401–1459`.

- Como mínimo EnemyMove debe tener clasificación de movimiento para recibir descarte por edad y bloqueo de pausa. No cambiar indiscriminadamente todas las cargas/estados a movement.
- Separar posición, acción, vida/muerte/carga/remoción si un booleano no expresa la política. Coalescer movimientos viejos por actor conservando el último; no borrar muerte/botín.
- No reproducir al reanudar una secuencia de Acks antiguos simplemente cambiando su timestamp.
- Dimensionar margen de reproducción para la cadencia medida de enemigos con la propuesta P3. El jugador y los enemigos no necesitan idéntica retención.
- Limpiar historial Sampled por **generación local** en el hilo propietario al retirar/recrear actores, si se corrige deuda de lifecycle. No hacer `g_sampled.erase` desde el callback de juego bajo g_mutex: hoy Sampled es del hilo red y sería una carrera.

## 7. Sondas mínimas que faltan

Contadores periódicos; no un log infinito por frame. En una prueba dirigida de 30 s se puede registrar cada golpe.

1. `GuestHitDecision session hit_id copy enemy action frame type local_epoch partner_epoch sent zone_on zone_dry suppress_hidden hp dead paused distance verdict rx_ms due_ms apply_ms`. Siempre motivo para not_ours/dead y límites.
2. `GuestHitApplied ... requested_damage c2s_enqueued c2s_posted hp_before hp_after hp_ack_ms reaction_kind template_bytes`. “Encolado” no significa “aplicado”.
3. `AggroHit enemy old_target requested_target seq_before seq_sent seq_after flagsG(273,276,340,390,394) target_after_100ms target_after_1s`; lecturas con SEH; por clave, sin direcciones como identidad.
4. `EnemyMoveTiming copy sample_ms sample_game_ts receive_ms due_ms inject_ms old_ts fixed_ts game_now lead queue_age paused dropped_reason`. CDPK actual no separa la red de la cola.
5. `ZoneTransition epoch reason hostpos guestpos distance radius sent_count hidden_count restored_count`; `EnemyRemove enemy copy reason generation sample_age`.
6. `ReactionTemplateReady action frame ata_size aar_size buff_size build`, primera vez y contadores de misses.
7. Una vez por 2 s durante reproducción, lista acotada de actores cercanos a G: clave, nativo/copia, variante, posición, suppressHidden; confirma/descarta duplicado específico.

No hooks inline a mitad de función. Un nuevo punto intermedio usa safetyhook::create_mid; lecturas protegidas; ninguna llamada interna del motor desde hilo worker.

## 8. Prueba de juego

1. Guardar copia de partida antes de probar cambios de memoria. Mismos binarios/build en ambos PCs; grabar logs/capturas. H no golpea a nadie.
2. G inicia pelea contra dos enemigos de zona. Éxito de daño: cada hit_id distinto tiene 2703 → 2147 H → 2147 G y HP idéntica. Éxito de A: target_after100 ms=G', CTC/DFE de ataques a G' aunque P esté lejos.
3. Confirmar que reacción no depende de aprendizaje H: `template_ready` por receta validada o reacción explícitamente omitida; si falta receta, ejecutar una vez el control de “H da un golpe” para verificar B.
4. Repetir acciones 73FA3F0A/10 → 11, 7C16DAAF/13 → 14 y 267BE8EF/11 → 13 con dos objetivos. Con C, no perder el segundo frame ni los dos objetivos por batching. Duplicar artificialmente el mismo hit_id solo en un ensayo de protocolo: cero daño doble.
5. Matar H y esperar 10 s en menú. Esperado: aviso de pausa; no remates pendientes aplicados al mundo nuevo; no cola de movimiento antiguo; pausa no se interpreta como falla Steam.
6. Reaparecer H cerca de G. Verificar ON continuo, 0 restored, retirar copias viejas y cargar nuevas generaciones. G inicia la pelea siguiente **antes de que P se acerque**.
7. Medir específicamente nuevas copias 21/22 equivalentes: p50/p95 de movimiento con cambio, lead/tardíos, edad de cola; comparar con la primera pelea y controles P3. Éxito: no ~50 % de samples fuera del búfer, sin ráfagas de pasado.
8. H se queda lejos 20 s mientras G combate; comprobar target G' y ataques reales. Luego G muere/sale/carga: ningún enemigo permanece fijado a una copia inválida.
9. Salir a carga y reentrar; validar que no quedan intents 3040 ni GuestHit de la generación anterior.

**Pendiente solo resoluble en juego:** cuánto dura el target forzado; flags reales de G' tras recarga; seguridad/efecto de una receta de reacción persistente; contribución exacta de edad de cola frente a muestreo a los tirones. Lo observado ya permite separar daño, reacción, adquisición y temporización sin atribuirlos a una única “zona rota”.

