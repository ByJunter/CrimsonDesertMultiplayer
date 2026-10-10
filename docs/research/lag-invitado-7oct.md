# Lag visto desde el invitado (prueba del 7 oct, noche)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Mod 0.1.0, build **`228287A2` +
cambios locales**, protocolo 9, igual en los dos PC. El código que se cita (`coop.cpp`, `mod.cpp`,
`enemy_host_tick`, `reliable.cpp`, `client_session.cpp`, `inject_clock.h`, `clock_mapper.h`) no ha
cambiado entre `228287a` y `HEAD` (`4a71b51`). En `remote_puppet.cpp` solo cambian la flexión de la
cuerda y el reloj de respaldo de `puppet_take_injection`, y ninguna de las dos cosas afecta a esto.

- **Anfitrión** = PC principal (`enemy relay true`, `enemies target copy true`, zona del
  invitado en *dry run* en su propio ini). Log `bin64\cdmp.1.log`, captura `cdmp_packets_20261007-201826`.
- **Invitado** = Shadow (1280×720, `enemy zone (guest) ON`, 150 m / 250 m). Log del invitado
  (`cdmp.log`, 7 oct, noche), captura `cdmp_packets_20261007-191633`.
- Las horas del invitado son las del anfitrión **menos 1 h**. En este documento **todas las horas
  son del anfitrión**.
- Para comparar: sesión de la tarde (build `6B865217`): `cdmp.2.log` + `cdmp_packets_20261007-184300`
  (anfitrión) y el `cdmp.log` de la tarde + `cdmp_packets_20261007-174707` (invitado).

Etiquetas: **[hecho]** = medido en un log o en una captura. **[inferido]** = sale de juntar hechos y el
código. **[hipótesis]** = falta comprobarlo.

---

## 0. Resumen en una frase

En el anfitrión, el hilo de red hace todo el trabajo de las copias de enemigos (≈ 25–48 copias) **entre
el momento en que recoge los movimientos del jugador (`outbox_.swap`) y el momento en que los envía**.
Por eso **cada** movimiento del anfitrión sale entre 150 y 200 ms más tarde, y el bucle va a ~6 Hz. El
invitado sincroniza la reproducción con el paquete más rápido, así que ese retraso se suma tal cual a
lo que ve: el anfitrión le aparece **~520–675 ms** por detrás, frente a **~400 ms** en el sentido
contrario. Además, del **15 al 30 %** de las muestras le llegan tarde, y eso produce los tirones. Los
enemigos usan el mismo camino y encima se muestrean a ~6 Hz en lugar de 15 Hz. Steam, el ancho de
banda y el canal fiable **no** son la causa.

---

## 1. Cómo se ha medido

### 1.1 Líneas del log (cada 5 s)
`Net:` (rtt de sesión, `reliable pending/refused`, Steam: ping, KB/s, `pending`, `queue`, `send
errors`), `co-op:` (`clock offset`, `target`, `events in`, `outbox dropped`), `Overlay:` (presents →
FPS), contadores `Puppet:`.

### 1.2 Deriva del informe `Net:` = duración del bucle de red
En `run_network` (`mod.cpp`), `now` se toma **antes** de `session.tick` y `coop->update`, y el informe
sale en la primera vuelta con `now − last_report ≥ 5000`, tras lo cual `last_report = now`. El intervalo
entre dos líneas `Net:` menos 5000 ms suma "lo que tardó en llegar la vuelta siguiente" más la
diferencia de duración de `update` entre dos vueltas. A lo largo de muchos informes el segundo término
se anula, y la media ≈ **media vuelta del bucle**.

### 1.3 Retraso extremo a extremo (capturas de los dos PC)
Se empareja cada `MoveActorReq` propio del emisor (su captura: C>S, posición en `+0x04`, marca de tiempo
en `+0xD5`, instante QPC de `PostPacket`) con el `MoveActorAck` **inyectado** en el receptor para la copia
`0xA010F001` (los mismos 12 bytes de posición, porque el desplazamiento es 0 m). Se emparejan
**6 340 de 6 437** (anfitrión → invitado) y **8 573 de 8 689** (invitado → anfitrión).
- `transit` = inyectado en el receptor − publicado en el emisor. Incluye el escritor del sniffer, la
  espera en `outbox_`, el bucle de red del emisor, el relay de Steam, el bucle de red del receptor, el
  *hold* de 100 ms y la espera hasta un paquete real (§2.6).
- La diferencia de época entre los QPC de los dos PC (C) es desconocida. Se fija con los minutos
  tranquilos, suponiendo que el camino más rápido tarda lo mismo en los dos sentidos. **El cambio
  del mínimo (p1) de cada sentido no depende de C.**
- `lead` = marca inyectada − (reloj del cliente receptor − 200 ms). El reloj del receptor se aprende
  de sus propios `MoveActorReq`. El cliente pinta los actores remotos 200 ms por detrás de su reloj
  (`kClientRenderDelayMs`). Si `lead < 0`, la muestra llegó **después** de su punto de pintado
  ("tarde"): la copia se queda sin muestra futura (el cliente no extrapola), se para y luego salta.
- `shown` = transit + max(lead, 0) = cuándo pinta realmente la copia esa muestra. Por construcción
  ≈ 400 ms + el tránsito mínimo de la ventana de 4 s del `ClockMapper`.

Los scripts no están en el repositorio (`inject_margin.py`, `inject_gate.py`,
`e2e_delay.py`, `status.py`, `loopgap.py`). Leen el formato `CDPK v2` de `troc_sniffer.cpp`.

---

## 2. Hechos medidos

### 2.1 Steam y ancho de banda: sanos **[hecho]**
| | Anfitrión | Invitado |
|---|---|---|
| Ping de Steam | 24–56 ms (media ≈ 29) | 24–54 ms (media ≈ 29) |
| Salida | 2–69 KB/s (pico 68,9 KB/s a las 20:22:33, envío de ~36 `EnemySpawn`) | 1–28 KB/s |
| `send rate` estimado | 256 KB/s | 256 KB/s |
| `pending` / `queue` | ≤ 4 273 B / ≤ 12 ms (casi siempre 0) | 0–58 B / 0 ms |
| `send errors`, `outbox dropped`, `refused` | 0, 0, 0 | 0, 0, 0 |

Nunca se usa más del 27 % del ritmo de Steam, y su cola no pasa de 12 ms.

### 2.2 Canal fiable: sin bloqueo de cabeza **[hecho + código]**
- `reliable pending` en el anfitrión: 0–9 fragmentos (ventana 128, cola 2 048). En el invitado: 0–14.
  `refused` 0 en los dos.
- El movimiento **no pasa por el canal fiable**. `Move`, `StartAction`, `Rotate`, `EnemyPos` y
  `EnemyAct` son `GameEvent` y salen con `ClientSession::send` directo. En Steam todo va como
  `k_nSteamNetworkingSend_UnreliableNoNagle` (`steam_transport.cpp`). Por el canal fiable solo van
  `Spawn`, `Equip`, los `Relay` "reliable" y los `Enemy*`. Las 48 cargas no pueden retrasar un
  `Move` en la cola.

### 2.3 El bucle de red del anfitrión se vuelve lento con las copias **[hecho]**
| Ventana (hora del anfitrión) | Copias del anfitrión | Media de deriva `Net:` anfitrión | Ídem invitado | rtt de sesión anfitrión / invitado | Ping de Steam |
|---|---|---|---|---|---|
| 20:19:30–20:22:24 (zona encendida, aún sin copias) | 0 | **11 ms** | 17 ms | 59 / 58 ms | 30 ms |
| 20:22:25–20:24:57 | ~25–48 | **105 ms** | 26 ms | **168 / 132 ms** | 29 ms |
| 20:25:10–20:25:37 (pausa del anfitrión y bajada de copias) | pocas | −20 ms | 12 ms | 68 / 53 ms | 30 ms |
| 20:25:38–20:26:48 | ~35–48 | **103 ms** | 16 ms | 134 / 120 ms | 26 ms |
| 20:28:15–20:29:45 | ~33–48 | 56 ms | 14 ms | 149 / 117 ms | 31 ms |

- La deriva media de ~105 ms implica vueltas de ≈ 150–220 ms. El rtt que mide el anfitrión (manda el
  Ping en `session.tick` y solo procesa el Pong en la vuelta siguiente) ≈ una vuelta entera ≈
  160 ms. El que mide el invitado ≈ ping + media vuelta del anfitrión. Las dos cosas cuadran.
- El bucle del invitado se mantiene rápido (12–26 ms).
- Las copias de enemigos del anfitrión empiezan a las 20:22:19 (`Enemy relay: sending enemy`: 36
  envíos en el minuto 20:22). La deriva salta a la vez: +120 ms a las 20:22:28 y +199 ms a las
  20:22:33.
- La sesión de la tarde ya lo mostraba: deriva media de 39 ms en el anfitrión, 48 de 161 informes
  con más de 60 ms, y el rtt de sesión 60–100 ms por encima del ping.

### 2.4 Los enemigos se muestrean a ~6 Hz, no a 15 Hz **[hecho]**
Todas las `EnemyPos` de un mismo tick de `enemy_host_tick` llevan la misma marca. Así se agrupan los
`MoveActorAck` inyectados en el invitado para las copias `0xB01Fxxxx`:

| Hora del anfitrión | Intervalo entre ticks p10 / p50 / p90 | Copias que se mueven por tick p50 / máx. |
|---|---|---|
| 20:21:33–20:22:33 | 65 / 154 / 177 ms | 14 / 27 |
| 20:22:33–20:23:33 | 146 / **162** / 189 ms | 16 / 27 |
| 20:23:33–20:24:33 | 157 / **162** / 230 ms | 16 / 40 |
| 20:24:33–20:25:33 | 78 / 110 / 141 ms | 15 / 28 |
| 20:25:33–20:26:33 | 33 / 141 / 178 ms | 15 / 36 |
| 20:27:33–20:28:33 | 60 / 145 / 189 ms | 11 / 30 |

`kPosIntervalMs = 66`. Con un bucle rápido el intervalo sería ~66–78 ms (`GetTickCount64` va a saltos
de 15,6 ms). El p50 de ~155–165 ms es **una muestra por vuelta del bucle**, otra medida independiente
de que la vuelta dura ~160 ms.

### 2.5 Retraso extremo a extremo, por minuto **[hecho]**
C fijo = −120 507 757 ms (minutos tranquilos 0, 2, 3 y 6). Valores en ms.

| Hora del anfitrión (minuto) | Anfitrión→invitado: transit p5 / p50 / p95 | tarde | **shown p50 / p95** | Invitado→anfitrión: transit p5 / p50 / p95 | tarde | **shown p50 / p95** |
|---|---|---|---|---|---|---|
| 20:18:57 | 148 / 199 / 312 | 0,0 % | **419** / 435 | 163 / 224 / 937 | 18,3 %¹ | **419** / 937 |
| 20:20:57 | 151 / 193 / 302 | 0,4 % | **417** / 439 | 145 / 255 / 574 | 21,7 %¹ | **394** / 574 |
| 20:21:57 | 166 / 398 / 637 | 13,5 % | **545** / 637 | 174 / 271 / 383 | 1,7 % | **412** / 418 |
| 20:22:57 | 371 / 513 / 775 | 27,7 % | **599** / 775 | 166 / 259 / 368 | 3,0 % | **411** / 435 |
| 20:23:57 | 371 / 526 / 774 | 17,5 % | **675** / 900 | 164 / 265 / 392 | 8,6 % | **408** / 507 |
| 20:24:57 | 168 / 312 / 638 | 26,3 % | **426** / 638 | 156 / 237 / 343 | 1,7 % | **396** / 484 |
| 20:25:57 | 338 / 442 / 758 | 15,8 % | **658** / 1018 | 153 / 235 / 323 | 2,6 % | **405** / 617 |
| 20:26:57 | 176 / 325 / 677 | 30,0 % | **438** / 677 | — (anfitrión en el menú) | — | — |
| 20:27:57 | 264 / 425 / 661 | 24,8 % | **518** / 706 | 143 / 230 / 323 | 6,6 % | **393** / 509 |
| 20:28:57 | 333 / 434 / 558 | 2,3 % | **601** / 615 | 144 / 240 / 332 | 0,3 % | **395** / 407 |

¹ Arranque, y pausa de menú en el anfitrión (20:20:15–24), con huecos de paquetes reales en el anfitrión (§2.6).

**Suelo (p1 del valor bruto, no depende de C):**
- Anfitrión→invitado: −620, −618, −607, **−416, −423**, −603, **−436**, …, −560, **−431**. El suelo
  **sube ~190–200 ms** justo en los minutos de bucle lento.
- Invitado→anfitrión: 905, 886, 910, 905, 898, 897, 894, 881, 879. **Plano** (±15 ms).

Conclusión **[hecho]**: el retraso que se añade **está en el sentido anfitrión→invitado y lo sufre
cada paquete** (el mínimo también sube). No es solo variación. El sentido contrario no cambia.

**Retraso efectivo** (cuánto tarda en verse en un PC lo que hace el otro), con copias:
- **El invitado ve al anfitrión: p50 ≈ 520–675 ms** (media de los minutos lentos ≈ 600), **p95 hasta
  ~0,9–1,0 s**, y **15–30 %** de las muestras tarde.
- **El anfitrión ve al invitado: p50 ≈ 395–410 ms**, p95 ≈ 410–620 ms, y **2–9 %** de muestras tarde.
- Sin copias, los dos ≈ 415–420 ms (200 de *playout* + 200 del pintado del cliente + ~15 de tránsito).
- **Asimetría: +120 a +270 ms de retraso constante y entre 3 y 10 veces más muestras tarde en el
  invitado.**

Tarde (ping de 70–80 ms, menos copias): el invitado ve al anfitrión a p50 ≈ 570–590 ms y el anfitrión
al invitado a ≈ 450–465 ms. El suelo del sentido anfitrión→invitado sube ~160 ms en los minutos 6–10
de esa captura. **Es el mismo patrón, solo que más débil.**

### 2.6 El invitado inyecta solo cuando llega un paquete real **[hecho + código]**
`on_recv_packet` (`troc_sniffer.cpp`) solo entrega la cola del puppet **dentro** de un
`OnRecvPacket` real (hasta 64 paquetes por llamada). Medido con la captura:

| | Espera media a una oportunidad de inyección | Movimientos de la copia por lote (p50/p90) |
|---|---|---|
| Invitado 20:21:33–20:25:33 | **75, 122, 67, 119 ms** | 2–6 / 6–13 |
| Anfitrión 20:21:26–20:26:26 | **22, 40, 40, 163 ms²**, 35 ms | 2–8 / 10–12 |

² Pausa de menú del anfitrión a las 20:25:00–08.

- Según el hueco previo al paquete real que la mete, la fracción de muestras del jugador que llegan
  tarde es: invitado 12 % (< 50 ms), 9 % (100–200 ms), **36 %** (200–400 ms) y **66 %** (> 400 ms).
  En el anfitrión: 6 %, 5 %, 18 % y 63 %.
- El tope de 64 se alcanza muy pocas veces (≤ 6 lotes por minuto).
- **[inferido]** El invitado recibe menos paquetes reales porque su servidor ha quitado a los
  nativos de la zona (`Suppress: removed group … natives`, `dropped load …`): su mundo está "callado".
  Es un factor **propio del invitado**, pero secundario: añade ~50–80 ms de media, frente a los
  ~150–300 ms del §2.5.

### 2.7 Reloj: los saltos son pausas de menú **[hecho]**
- `clock offset` en el invitado: 115 060 → 111 895 → 102 196 → 105 6xx (de 20:19:5x a 20:20:3x),
  106 046 → 114 228 (20:25:07), 114 655 → 101 997 (20:26:3x–48), 101 997 → 142 381 (20:27:58) y
  142 828 → 154 823 (20:29:08).
- Todos coinciden con pausas: del anfitrión (`this game is paused` 20:20:15–24, 20:25:00–08,
  20:27:00–55, 20:28:55–29:05) o del invitado (20:19:52–20:20:11, 20:26:39–47 y
  20:27:12–25). Entre pausas, el desfase está quieto (±10 ms).
- El *offset* sigue al *target* (mínimo de 4 s) con ±50–150 ms de retraso en unos 16 informes de 88,
  **igual en los dos PC** (media −5 ms en el invitado y −12 ms en el anfitrión). No explica la
  asimetría.
- El *playout delay* no cambia: `Puppet: N events replayed (ts shift …, delay 200 ms)` siempre dice
  200 ms. El `ts shift` es el desfase + 200.

### 2.8 FPS **[hecho]**
- Invitado (Shadow, 1280×720): **48–62 FPS** durante toda la sesión. No va corto.
- Anfitrión: ~35–38 FPS en el mundo (84–86 en el título). Ya era así por la tarde (35–38 FPS), sin
  depender de las copias. Su `MoveActorReq` va a ese ritmo: una muestra cada ~29 ms (34 Hz) frente
  a ~18 ms (55 Hz) del invitado. Hace al anfitrión un poco menos fluido en el invitado, pero no es
  la causa.

### 2.9 Contadores del puppet **[hecho]**
| | Invitado (final) | Anfitrión (final) |
|---|---|---|
| `movement timestamps kept within the client's clock` | **3 201** | 205 |
| `repeated positions dropped` | **2 312** | 193 |
| `filled a … gap` | 8 | 4 |

En el invitado también cuentan las copias de enemigos (comparten `g_inject_clock`). Los saltos grandes
(p. ej. 191 → 494 entre 20:24:58 y 20:25:08, y +190 por cada 5 s entre 20:27:30 y 20:27:55) coinciden con
pausas del anfitrión, cuando su reloj se para y las muestras se ajustan a [now−500, now+450].

### 2.10 Enemigos en el invitado **[hecho]**
`Em` (movimientos de las copias de enemigos): **10–35 %** tarde por minuto, p50 de *lead* entre 54 y
350 ms. Su margen es menor que el del jugador: `puppet_enemy_move` las encola a `now + delay_ms`
(200 ms) y no a `now + kHoldMs` (100). Con muestras cada ~160 ms y retrasadas como las del anfitrión
(§2.5), se ven a saltos y con retraso.

---

## 3. Causa raíz

### 3.1 Mecanismo principal: el envío del jugador espera al trabajo de los enemigos **[hecho + código]**
`CoopLink::update` (`src/mod/net/coop.cpp`), en este orden:

```
1. out.swap(outbox_)                         ← se recogen los Move/StartAction/Rotate del jugador
2. enemy_set_linked, update_enemy_zone       (suppress_tick, enemy_zone_update)
3. enemy_tick(), loot_tick()
4. while (reliable_pending < 512) send_reliable(EnemySpawn/EnemyRemove/EnemyState…)
5. enemy_host_tick()                         ← por CADA copia enviada: actor_world_position()
                                                + actor_world_rotation() = 2 × find_actor + ~30 safe_read
6. hits_take_events, guest_hits_take_events, time_sync_tick
7. for (ev : out) session_.send_event(ev)    ← solo aquí salen los movimientos del jugador
```

y `run_network` (`mod.cpp`): `recv` (hasta 512) → `session.tick` → `coop->update` → HUD/informe → `recv`…

- **En el anfitrión**, con 25–48 copias, los pasos 2–6 tardan ~150–200 ms en cada vuelta (§2.3, §2.4).
  Todo `Move` del anfitrión espera al menos esos pasos (por eso **sube el suelo**, §2.5) y, de media,
  media vuelta más hasta el siguiente `swap`. Por eso salen a ráfagas de ~5 `Move` cada ~160 ms.
- **En el invitado** el bucle es ligero: no lee memoria por cada copia en el hilo de red, porque
  `enemy_on_event` y `puppet_enemy_move` solo encolan. Sus `Move` salen enseguida.
- **Al recibir en el anfitrión**, los `Move` del invitado se procesan en el `recv` del principio de la
  vuelta, **antes** del trabajo pesado. Esperan entre 0 y una vuelta, pero el más rápido no espera
  nada: el suelo no cambia y el `ClockMapper` del anfitrión no añade retraso. La variación (≤ 160 ms)
  cabe en el margen de 300 ms (200 + 200 − *hold* de 100). Por eso el anfitrión ve bien al invitado.
- **En el invitado**, `ClockMapper` sincroniza la reproducción con el **mínimo** de 4 s de
  (reloj local − marca). Ese mínimo ya incluye los ~150–200 ms del suelo, así que todo lo del
  anfitrión se pinta ~150–200 ms más tarde (**el "lag"**). Además: ráfagas de ~160 ms, el suelo que
  sube y baja con el número de copias (el *offset* lo sigue a 1 ms por muestra) y la espera a
  paquetes reales del §2.6. La suma se come el margen de 300 ms en el **15–30 %** de las muestras
  (**los tirones**).
- **Enemigos**: el mismo camino, más el muestreo a ~6 Hz (§2.4) y un margen de solo 200 ms (§2.10).

### 3.2 Qué parte de los pasos 2–6 cuesta **[hipótesis, alta confianza; falta cronometrarlo]**
- Antes de que hubiera copias, la vuelta duraba ~11 ms aunque la zona ya estaba encendida
  (20:20:31). Con copias, ~160 ms. Bajó a casi 0 a las 20:25:10–37, cuando hubo menos copias. **El
  coste crece con las copias vivas**, no con los envíos: en 20:23 apenas hubo 7 envíos y la vuelta
  seguía en ~160 ms. Lo único del hilo de red que trabaja por cada copia viva es
  `enemy_host_tick` (paso 5).
- Cada copia hace 2 búsquedas completas (`find_actor` dos veces) y ~30 `safe_read`. Cada `safe_read`
  llama a `VirtualQuery` (`pages_readable`, `safe_memory.cpp`). Con ~40 copias son ~1 200–1 500
  `VirtualQuery` por vuelta. Para llegar a 150 ms, cada una tendría que tardar ~100 µs. Es posible en
  un proceso de este tamaño: `VirtualQuery` recorre las PTE de la región para calcular `RegionSize`
  y comparte el bloqueo del espacio de direcciones con las `VirtualAlloc`/`VirtualFree` del juego.
  **No está medido.**
- Otros candidatos, menos probables: esperas en `g_mutex` de `enemy_relay` (lo toman
  `zone_candidates`/`send_enemy` en el hilo del relay y `enemy_observe_*` en el escritor del
  sniffer) y `suppress_tick`. Los dos son por tick y no por copia.

### 3.3 Por qué no se notaba tanto antes **[inferido]**
Con `kZoneMax = 24` (tarde) el coste era la mitad, y el ping de entonces (70–80 ms) enmascaraba
parte del efecto. Con 48 copias (`eea8de8`) la vuelta llega a ~160 ms, y el suelo añadido
(~200 ms) ya es tan grande como el *playout* entero.

---

## 4. Hipótesis descartadas
| Hipótesis | Por qué no |
|---|---|
| El canal fiable se satura con 48 `EnemySpawn` y bloquea al movimiento | El movimiento es no fiable y va aparte (§2.2). `pending` ≤ 14/128, `refused` 0, cola de Steam ≤ 12 ms. |
| Ancho de banda o relay de Steam | Pico de 69 KB/s frente a 256, ping 25–30 ms en los dos sentidos y sin cambios cuando aparece el retraso (§2.1). |
| Shadow va a pocos FPS | 48–62 FPS, y su bucle de red va rápido (§2.3, §2.8). |
| El desfase del reloj oscila | Solo salta en las pausas de menú. Entre pausas está quieto, y igual en los dos PC (§2.7). |
| Crece el *playout delay* | Siempre 200 ms (§2.7). |
| `outbox dropped` / `send errors` | 0 en los dos. |
| Tope de 64 inyecciones por paquete real | Se alcanza ≤ 6 veces por minuto (§2.6). |

## 5. Lo que sigue abierto
1. **Cronometrar** cada paso de `CoopLink::update` en el anfitrión para confirmar el §3.2
   (instrucciones en el §7).
2. Por qué el servidor del invitado envía menos paquetes reales. ¿Son solo los nativos quitados por
   la zona? Se puede comprobar contando los S>C por minuto con la zona encendida y apagada.
3. Los ~35 FPS del anfitrión en el mundo (84 en el título) no dependen de las copias (tarde y noche
   igual). No forman parte de este problema.

---

## 6. Arreglos propuestos (por orden)

### A1. Enviar los movimientos del jugador antes del trabajo de los enemigos — **rápido, riesgo bajo**
**Dónde:** `CoopLink::update`, `src/mod/net/coop.cpp`.
**Cambio:** justo después de `out.swap(outbox_)` y de `enemy_set_linked`, si `connected && g_ready`,
ejecutar ya el bucle `for (auto& ev : out)` (Equip/Relay fiables por su secuencia y el resto con
`send_event`). Moverlo **delante** de `update_enemy_zone`, `enemy_tick`, `loot_tick`, el bucle
fiable de enemigos y `enemy_host_tick`. Mejor aún: sacarlo a una función `flush_outbox(now)` y
llamarla **dos veces**, al principio y al final de `update`, para recoger también lo que capture el
escritor del sniffer mientras se trabaja con los enemigos.
**Cuidado:** hoy, si `!connected || !g_ready`, `out` se descarta. Hay que mantenerlo así, y
`update_enemy_zone` tiene que seguir ejecutándose aunque no haya enlace.
**Efecto esperado:** desaparece el suelo de +150–200 ms (§2.5). El invitado vería al anfitrión a
~400–430 ms, como ahora el anfitrión ve al invitado. Las muestras tarde bajarían hacia el 5–10 %
(quedan la variación de media vuelta y el §2.6).
**Riesgo:** el orden entre los `Relay`/`Equip` del jugador y los `Enemy*` cambia, pero son flujos
independientes (cada uno con su secuencia y su `RelKind`). Ningún receptor relaciona los dos.

### A2. Que `enemy_host_tick` sea barato y acotado — **arregla la causa, riesgo bajo–medio**
**Dónde:** `enemy_host_tick` (`src/mod/net/enemy_relay.cpp`) y `src/mod/game/actor_lookup.cpp` /
`safe_memory.cpp`.
1. **Una sola búsqueda por copia:** una nueva `game::actor_world_transform(key)` que haga
   `find_actor` una vez y lea de un tirón el bloque TransformSync `+0xA4…+0xC4` (rotación `+0xA4`,
   posición `+0xB4`, sector `+0xC0`: 32 bytes) con un único `safe_read`. Pasa de ~30 lecturas
   protegidas a ~13. **Riesgo bajo** (offsets ya usados).
2. **Presupuesto por vuelta:** como mucho N copias por llamada (p. ej. 12, por turnos) o un límite de
   tiempo (p. ej. 4 ms, medido con QPC). Las demás, en la vuelta siguiente. Con A1 hecho, la vuelta
   ya no retrasa al jugador, pero sí el `recv` y los pings.
3. **Mejor aún, sacarlo del hilo de red:** muestrear en el hilo de `enemy_test_loop` (que ya hace
   `auto_tick` cada 500 ms) con su propio temporizador de 66 ms. Las `EnemyPos` se dejan en
   `g_out_events` (con `g_mutex`) y `enemy_host_tick` solo vacía la cola. **Riesgo medio:**
   `g_sampled` pasa a ser de otro hilo, y `enemy_reset_partner` lo borra desde el hilo de red, así
   que debería ir con `g_mutex` o con una bandera.
4. **Menos `VirtualQuery`** (si el cronometraje confirma el §3.2): una caché pequeña por hilo de
   regiones ya validadas (`[BaseAddress, BaseAddress+RegionSize)`, *commit*, sin
   `PAGE_NOACCESS/GUARD`, válida ~250 ms) dentro de `pages_readable`. La copia sigue bajo SEH, así
   que una región liberada dentro de esa ventana se queda en un fallo capturado. Cumple las reglas del
   proyecto (CONTRIBUTING.md: `VirtualQuery`/SEH). **Riesgo:** leer un `PAGE_GUARD` recién creado en esa ventana.
   En el montón de actores no los hay, pero conviene limitar la caché a regiones `MEM_PRIVATE`
   grandes.
5. Menor: `kPosIntervalMs` se compara con `GetTickCount64` (15,6 ms). Conviene usar `now_ms()` (QPC)
   para que los 66 ms sean de verdad 66.

**Efecto esperado:** vuelta del anfitrión de ~160 ms a < 20 ms, enemigos a 15 Hz reales y rtt de
sesión ≈ ping + ~20 ms.

### A3. Más margen para las posiciones de los enemigos — **riesgo bajo**
**Dónde:** `puppet_enemy_move` (`src/mod/net/remote_puppet.cpp`).
**Cambio:** `enqueue_at(..., steady_ms() + move_hold_for(delay_ms), …)` en lugar de
`steady_ms() + delay_ms`. Es la misma regla que los `Move` del jugador: margen de 300 ms en vez de
200.
**Riesgo:** las posiciones llegan al búfer antes que las acciones de la misma copia (que siguen a
`now + delay_ms`). Como el cliente pinta las posiciones por marca de tiempo y empieza las acciones al
recibirlas, no cambia cuándo se ve nada. `InjectClock` deja pasar intacta una acción con una marca
más vieja que el búfer (como ya pasa con el jugador).

### A4. Espera a paquetes reales en el invitado — **medir primero**
- Medir en `puppet_take_injection` cuánto llevan vencidos los movimientos al entregarlos
  (`now − due_ms`) y sacar en el log, cada 5 s, el p50, el p95 y el máximo.
- Si después de A1 y A2 el p95 del invitado sigue por encima de ~100 ms, hay dos opciones:
  - (a) Subir solo en el invitado `[coop] delay` de 200 a 250–300 ms. Cuesta +50–100 ms
    constantes, pero da menos tirones.
  - (b) Que el *hold* de los `Move` dependa de esa espera medida: `kHoldMs` − espera media, con un
    mínimo de 0. La propia espera ya hace de *hold*. **Riesgo:** `fill_gap_before` necesita que
    los `StartAction` sigan en la cola para recolocarlos; con *hold* 0 se recolocan menos.
- Un segundo disparador de inyección (otro hook en el hilo de red del cliente) sería la solución de
  fondo, pero **requiere investigar** y es arriesgado.

### A5. No hace falta (con los datos de hoy)
- Canal separado o no fiable para el movimiento: **ya lo es**.
- Comprimir `EnemySpawn`: el ancho de banda es ≤ 27 % del de Steam y no hay cola. Solo serviría para
  hacer más cortas las ráfagas de carga (opcional).
- Prioridades o carriles de Steam: la cola de Steam está a ~0 ms.

---

## 7. Cómo comprobarlo en la próxima prueba
1. **Instrumentación (antes o junto con A1):** en `CoopLink::update`, medir con QPC cada bloque (zona,
   `enemy_tick+loot_tick`, bucle fiable, `enemy_host_tick` con el número de copias, envío de `out`) y la
   vuelta completa de `run_network`. Cada 5 s, una línea `Net loop: vuelta media/máx …, update …,
   enemy_host_tick … (N copias)`. Guardar también la hora de llegada al outbox de cada evento para
   sacar la **edad en el envío** (p50/p95).
2. Repetir el emparejamiento de §1.3 con las dos capturas. Criterio de éxito con ~48 copias:
   - el p1 del sentido anfitrión→invitado no sube más de 20 ms respecto a los minutos sin copias;
   - `shown` p50 ≤ 450 ms en los dos sentidos;
   - muestras tarde < 10 %;
   - intervalo entre ticks de `EnemyPos` p50 ≤ 80 ms;
   - rtt de sesión ≤ ping + 30 ms.
3. Ver también el `Net:` del anfitrión: la deriva media entre informes tendría que volver a ≤ 15 ms.

---

## 8. Resumen (≤ 15 líneas)
1. Es asimétrico de verdad: con 25–48 copias, el invitado ve al anfitrión a p50 ≈ 520–675 ms (p95
   ~0,9–1 s) y con 15–30 % de muestras tarde. El anfitrión ve al invitado a ≈ 395–410 ms y con 2–9 %.
2. El retraso añadido va **solo en el sentido anfitrión→invitado** y afecta a **todos** los
   paquetes: su suelo (p1) sube ~200 ms cuando hay copias, y el del otro sentido sigue plano.
3. Causa: en `CoopLink::update` del anfitrión, los movimientos del jugador se recogen
   (`outbox_.swap`) **antes** del trabajo de los enemigos y se envían **después**. Ese trabajo
   (sobre todo `enemy_host_tick`: 2 búsquedas y ~30 lecturas con `VirtualQuery` por copia) hace
   que cada vuelta del hilo de red dure ~160 ms.
4. Pruebas: deriva del `Net:` de 11 → ~105 ms, rtt de sesión de 168 ms con ping de 29, y `EnemyPos`
   cada ~160 ms en vez de 66 ms. El bucle del invitado sigue rápido (~15 ms).
5. El invitado sincroniza la reproducción con el paquete más rápido, así que ese suelo se suma tal
   cual a lo que pinta ("lag"). Las ráfagas, el suelo que sube y baja y la espera a paquetes reales
   en el invitado (70–120 ms de media frente a 20–40) se comen el margen de 300 ms ("tirones").
6. Los enemigos usan el mismo camino, a ~6 Hz y con solo 200 ms de margen: 10–35 % tarde.
7. Descartado: canal fiable (el movimiento es no fiable; `pending` ≤ 14, `refused` 0), Steam (ping
   25–30, ≤ 69 de 256 KB/s, cola ≤ 12 ms), FPS de Shadow (48–62), oscilación del reloj (solo en las
   pausas) y *playout* (fijo en 200).
8. Arreglo 1 (rápido): mandar `out` justo después del `swap` y antes del trabajo de los enemigos, y
   repetir el envío al final de `update`.
9. Arreglo 2 (de fondo): `enemy_host_tick` con una sola lectura del transform por copia, un
   presupuesto por vuelta o en otro hilo, y una caché de `VirtualQuery`.
10. Arreglo 3: las `EnemyPos` con el *hold* de 100 ms de los `Move` (+100 ms de margen).
11. Antes de cerrar: cronometrar `update` por bloques y repetir la medición de §1.3 (criterios en §7).
