# P3 — Retraso: mejora del jugador, problema residual en enemigos

Fecha: 7–8 oct 2026. Código de referencia `57bec36bdad634a502eb587f8f899c090ca162c5`.
Juego 1.0.0.2976, PE `0x6AB28F00`, base `0x140000000`. Análisis sin ejecutar el juego ni aplicar cambios.

## Resumen (8 líneas)

1. **Hecho:** con copias, el suelo de tránsito del jugador subía 186 ms en noche1; en noche2 sube 14 ms frente a su referencia sin copias.
2. **Estimación:** el retraso mostrado mediano H→G baja de 598 a 442 ms; después de reaparecer H permanece en 445 ms.
3. Las muestras de jugador evaluables que llegan tarde pasan del 25,4 % al 0 % en esas ventanas.
4. **No está arreglado todo el movimiento:** los enemigos concretos `B01F0021/22` de la segunda pelea tienen 50,7/47,5 % de muestras tardías.
5. El resultado persiste con reloj local observado a ≤250 ms: 50,9/46,8 %.
6. El hilo de red aún muestra carga asociada a enemigos; la separación entre marcas de movimiento antes de la muerte tiene mediana 144 ms, frente al objetivo de 66 ms.
7. No hay captura del transporte CDMP: `EnemyPos` y duración exacta de cada vuelta requieren sondas; aquí se mide su efecto en `MoveActorAck`.
8. Mantener los dos arreglos actuales; corregir clasificación/espera de movimiento enemigo y medir coste por bloque antes de ampliar cambios.

## 1. Entradas, anclajes y reproducción

Raíces abreviadas:

- `H/` = `D:/SteamLibrary/steamapps/common/Crimson Desert/bin64/`.
- `G/` = carpeta con los logs y capturas del invitado, copiados desde su PC.
- `R/` = repositorio Crimson Desert Multiplayer.

| Sesión | Log H | Captura H | Log/captura G |
|---|---|---|---|
| noche1 | `H/cdmp.2.log` | `H/cdmp_packets_20261007-201826.bin` | `G/20261007-noche/cdmp.log`, `cdmp_packets_20261007-191633.bin` |
| noche2 | `H/cdmp.log` | `H/cdmp_packets_20261007-213534.bin` | `G/20261007-noche2/cdmp.log`, `cdmp_packets_20261007-203438.bin` |

El log rotado correcto de noche1 es actualmente **cdmp.2.log**, aunque el informe anterior lo llamaba cdmp.1.log. No se sustituyen archivos.

Script nuevo: `R/build/research/ia2/p3_measure.py`. Resultados: `p3_noche1.json`, `p3_noche2.json`, CSV `p3_noche{1,2}_{hg,gh,enemies}.csv` en esa misma carpeta. Se ejecutó con Python 3.12 ya disponible y `-B`; no necesita paquetes ni instalaciones.

El lector recorre CDPK v2 secuencialmente: cabecera de 12 bytes, registros `<QBBHHHII>` de 24 bytes y `copied` bytes. Ver `src/mod/net/troc_sniffer.cpp:321` y `:331`. No interpreta un registro incompleto: noche2 H termina con cabecera en offset de archivo **131350691**, declara 620 bytes y quedan 92. Se excluye solo ese último registro; las ventanas estudiadas preceden al final. Las otras tres capturas terminan completas.

Se ancla QPC a hora civil mediante el primer `Inject: delivered opcode 2005` y el paquete correspondiente con tamaño completo y usuario sintético `0x9010F001`:

| Captura | Log de entrega | QPC del paquete (ms) | Origen civil de t=0 |
|---|---|---:|---|
| H noche2 | `cdmp.log:136`, 21:36:33.576, 12673 B | 126435090,195 | 21:35:50.545956 |
| G noche2 | `cdmp.log:210`, 20:36:35.827, 12877 B | 5930449,245 | 20:35:03.521340 |

La línea se escribe inmediatamente después de inyectar; su precisión no equivale a sincronización externa de ambos relojes. La pequeña diferencia con otros anclajes de P2 es esperable por latencia de registro.

**Convención de las tablas de este informe:** ventanas en hora H; para agrupar datos de G se usa su hora civil +1 h. La diferencia adicional aproximada estimada de 0,6 s afecta solo cortes de ventana. Para latencia entre PC se estima una constante de desfase por sesión, como en el informe anterior; no se toma la hora civil como un reloj perfectamente sincronizado. P2 presenta la cronología de hechos con la corrección adicional.

## 2. Método y límites de la medición

### 2.1 Tránsito emparejado

`MoveActorReq` **2556**, dirección C>S, actor `0xA0100001`, posición en payload `+0x04..+0x0F`, marca u64 en `+0xD5`; tamaño habitual 514 B. Se empareja con `MoveActorAck` **2697**, S>C, actor `0xA010F001`, mismos 12 bytes de posición; payload 235 B. El desplazamiento configurado es 0 m.

Se conservan solo posiciones **únicas en los Req de la captura del emisor**, y solo la primera entrega coincidente. Así no se atribuye a un Req una posición que reaparece muchas veces durante una parada. Esto es más restrictivo que el recuento del informe anterior; no implica pérdida de red.

Noche2: **2751 pares H→G** entre 3469 Req propios y 2786 Ack de la copia; **10417 pares G→H**, entre 11424 Req y 10511 Ack. Los CSV conservan número de registro, `t` desde primer paquete, marcas y los bytes de posición.

Sea `raw = hora_receptor - hora_emisor` con la corrección gruesa de 1 h. Se calcula `C = (p1(raw H→G) - p1(raw G→H))/2` en la referencia sin copias. Tránsito estimado = `raw-C` para H→G y `raw+C` para G→H.

- Noche1 referencia 20:19:30–20:22:18, `C = -860,6375 ms`.
- Noche2 referencia 21:36:35–21:36:57, `C = -722,891 ms`, 192 pares H→G.
- Supuesto explícito: ambos sentidos tienen el mismo mínimo de tránsito durante esa referencia. **El nivel absoluto estimado depende de este supuesto. El cambio del p1 dentro de una sesión no depende de C.**
- No usar 21:37–21:40 de noche2 como referencia: hay menús/paradas, solo 34 pares H→G y demoras de 0,78–2,12 s. Calibrar con ese intervalo produciría latencias negativas espurias después. El script final ya evita ese error.

### 2.2 Retraso de representación y muestras tardías

Reloj local del receptor obtenido de sus propios Req: Move 2556 `+D5`, StartAction 2223 `+15F` y RotateActor 2828 `+9C` (constantes verificadas en `local_capture.cpp:46`). Se interpola entre muestras separadas ≤500 ms y pendiente 0,85–1,15; si falta interpolación, se extrapola como máximo 2000 ms desde la muestra más próxima **sin cruzar un intervalo de pausa registrado**. El CSV incluye `clock_age_ms` y deja `lead` vacío si no hay reloj estimable.

`lead = marca_Ack - (reloj_local_estimado - 200 ms)`. El retardo de representación de 200 ms está en `remote_puppet.cpp:648` y documentado en `movimiento-remoto-cliente.md`. `lead<0` significa llegada posterior a su punto de representación; no significa necesariamente que se descarte el paquete.

`shown = tránsito_estimado + max(lead,0)`. Es una **estimación de cuándo puede representarse la muestra**, no una medición de fotones/pantalla ni prueba de que la animación efectivamente la dibujó.

Limitaciones: la extrapolación cerca de una pausa puede errar porque el detector tarda en registrarla. Se publica cobertura y una comprobación restringida a reloj de ≤250 ms. No asignar un porcentaje a los tramos sin reloj medible.

## 3. Resultados del jugador remoto

Todos los tiempos son ms. `N/Nreloj` = pares exactos / pares con reloj evaluable. Percentiles lineales sobre las muestras.

| Ventana H→G | N/Nreloj | tránsito p1 / p50 / p95 | tardías | shown p50 / p95 |
|---|---:|---:|---:|---:|
| Noche1 sin copias, 20:19:30–20:22:18 | 1156/713 | 136 / 191 / 299 | 0,42 % | 413 / 423 |
| Noche1 copias, 20:22:35–20:24:45 | 1410/681 | 322 / 496 / 763 | 25,40 % | 598 / 881 |
| Noche1 segunda pelea, 20:25:38–20:26:48 | 637/596 | 248 / 441 / 857 | 25,50 % | 590 / 742 |
| Noche2 sin copias, 21:36:35–21:36:57 | 192/192 | 144 / 193 / 234 | 0 % | 428 / 444 |
| Noche2 copias antes de muerte, 21:42:00–21:44:05 | 398/398 | 158 / 217 / 310 | 0 % | 442 / 684 |
| Noche2 después de reaparecer, 21:45:35–21:48:20 | 533/512 | 165 / 225 / 391 | 0 % | 445 / 468 |

La cola p95 de shown antes de la muerte sigue alta: no afirmar que desaparecieron todas las demoras. La ventana incluye el final de la vida del invitado y cambios de reloj; el desglose por minuto está en JSON.

Control G→H:

| Ventana | N/Nreloj | tránsito p1 / p50 / p95 | tardías | shown p50 / p95 |
|---|---:|---:|---:|---:|
| Noche1 con copias | 1443/947 | 150 / 267 / 386 | 3,17 % | 414 / 439 |
| Noche2 con copias antes | 4280/3319 | 125 / 217 / 309 | 2,53 % | 392 / 406 |
| Noche2 tras reaparecer | 1768/1635 | 110 / 180 / 273 | 0,80 % | 377 / 386 |

**Hecho de alta confianza:** el incremento del suelo H→G con copias se reduce de **186,37 ms** en noche1 a **14,01 ms** en noche2; después de reaparecer es **21,04 ms**. La prueba reproduce el patrón antiguo y confirma la mejora nueva sin depender del desfase absoluto.

**Inferencia alta:** adelantar `send_out()` evita que cada paquete del jugador pague todo el trabajo de enemigos. Está implementado en `CoopLink::update`, `coop.cpp:129`, llamadas `:150` y `:171`. `enemy_host_tick` usa ya una búsqueda de transform, `enemy_relay.cpp:1420`; `actor_lookup.cpp:95` hace un solo `find_actor`. No deshacer ninguno.

No es un experimento A/B controlado: cambian ping, escena, actividad y cantidad de copias. No atribuir porcentajes de mejora individualmente a cada una de las dos modificaciones.

Sensibilidad a reloj más cercano: H→G noche1 con copias 243 muestras y **26,75 % tarde**; noche2 antes 297 muestras y **0 %**; noche2 después 381 y **0 %**. La conclusión principal se mantiene.

## 4. Enemigos: la mejora del jugador no resuelve la segunda pelea

CDPK captura los mensajes internos del juego, **no** datagramas CDMP. No contiene un registro directo de cada `EnemyPos` con llegada/cola/envío. Se mide el Ack que el receptor termina inyectando y su marca **después** de `InjectClock`. La separación de marcas únicas es una aproximación al ritmo de muestreo durante actividad normal, no prueba exacta del período del emisor durante pausa/restamp.

| Ventana | Ack / claves distintas | separación de marcas únicas p50 | reloj evaluable / tardías |
|---|---:|---:|---:|
| Noche1 con copias | 13013 / 59 | 162 ms | 8592 / 16,59 % |
| Noche2 antes de muerte | 9117 / 25 | 144 ms | 8988 / 1,10 % |
| Noche2 intervalo de menús 21:44:08–21:45:29 | 581 / 28 | 114 ms, no interpretar como tick limpio | 236 / 98,31 % |
| Noche2 después de reaparecer | 5561 / 44 | 86 ms | 4326 / 9,62 % |

Las claves distintas son acumuladas dentro de la ventana; **no** representan copias simultáneas. `kPosIntervalMs=66` (`enemy_relay.cpp:55`), pero solo se emite cuando se ha movido >5 cm o toca mantenimiento de 1 s (`:1441`). No confundir ese mantenimiento con pérdida de red.

### 4.1 Los dos enemigos que realmente recibió golpes G después de reaparecer H

Ventana G 20:47:45–20:48:28 (agrupada como 21:47:45–21:48:28):

| Copia | Ack | tardías / porcentaje | lead p50 | intervalo de transiciones >5 cm p50 / p95 |
|---|---:|---:|---:|---:|
| `0xB01F0021` | 71 | 36 / **50,70 %** | **−2,33 ms** | **229 / 448 ms**, 38 transiciones |
| `0xB01F0022` | 61 | 29 / **47,54 %** | **10,84 ms** | **229 / 659 ms**, 26 transiciones |

Referencia primera pelea, G 20:42:00–20:43:30:

| Copia | Ack | tardías | intervalo de transiciones >5 cm p50 / p95 |
|---|---:|---:|---:|
| `B01F0003` | 554 | 6 / 1,08 % | 135 / 270 ms |
| `B01F0004` | 486 | 6 / 1,23 % | 134 / 288 ms |

Con reloj observado a ≤250 ms: copias21/22 **50,94 % de 53** y **46,81 % de 47**. No depende de extrapolar durante dos segundos. La comparación no garantiza igual velocidad/actividad de los enemigos, pero sí demuestra menor margen temporal de las muestras recibidas en esa pelea.

Ejemplo reproducible: copia21, registro G **376016**, `t=762,225772 s`, payload235, marca **687539**, posición bytes `0e6e20c610d228445cb710c5`, lead +73,29 ms. Más tarde registro **405837**, `t=802,458310 s`, marca727581, posición `d33d20c66dd22844f4da0fc5`, lead −110,53 ms. Ambos constan en CSV y `focused_enemies` del JSON; el segundo usa una muestra de reloj a 1101,53 ms, por lo que la comprobación estricta se basa en las otras muestras próximas.

### 4.2 Mecanismos y grados de certeza

- **Hecho código:** `puppet_enemy_move`, `remote_puppet.cpp:1322–1345`, espera **todo delay_ms (200 ms)**; el jugador tiene `kHoldMs=100` en `:647`, `move_hold_for` en `:664`. Al enemigo le quedan 100 ms menos de margen para entregar la muestra. Esto permite proponer una mejora precisa, pero no prueba que explique por sí solo todas las pausas.
- **Hecho código:** ese movimiento se encola con `movement=false` en `:1345`. Las acciones de enemigo también en `:1369`. Por ello no pasan los filtros de antigüedad (`puppet_take_injection:2350`) ni de pausa (`:2363`) reservados a `movement=true`. Debe corregirse la clasificación sin mezclar purga del compañero con purga de enemigos; ver P2/P5.
- **Hecho código:** el consumidor solo se ejecuta durante un paquete real `OnRecvPacket`, máximo64 por llamada (`troc_sniffer.cpp:225`). CDPK carece de una bandera que permita separar universalmente paquete real e inyectado. No doy una cifra de espera de esa puerta sin una sonda adecuada.
- **Inferencia alta:** muestreo espaciado, espera200 y oportunidades de inyección insuficientes agotan el margen de copias21/22. Su `lead` alrededor de cero hace esperable la falta de muestra futura.
- **Hipótesis por medir:** reparto del coste entre lectura de transform, locks y resto de update; no atribuir automáticamente los milisegundos a VirtualQuery. No hay perfil por bloque.

## 5. Bucle de red: qué permiten afirmar los logs

`run_network`, `mod.cpp:314` toma `now` antes de `coop->update`; `:364–365` emite cada ≥5000 ms medidos con ese valor. La hora de la línea se toma después del trabajo. Por tanto `(intervalo entre Net)-5000` mezcla exceso de período y variación del tiempo de trabajo. Puede ser negativo. **No es la duración exacta de una vuelta ni permite obtener su p95.**

| Ventana | exceso medio Net H / G | RTT sesión H | ping Steam H |
|---|---:|---:|---:|
| Noche1 sin copias | 12,38 ms / consultar JSON | 59,73 ms | referencia JSON |
| Noche1 con copias | 105,79 ms / consultar JSON | 164,70 ms | ~29 ms |
| Noche2 sin copias, 21:37–21:40:40 | 23,84 / 22,64 ms | 79,03 ms | 39,02 ms |
| Noche2 copias antes | **74,78 / 14,21 ms** | **149,45 ms** | **32,21 ms** |
| Noche2 después | **33,59 / 13,34 ms** | 83,56 ms | 32,55 ms |

Anclajes noche2: H `cdmp.log:1039` a `:1573` para antes; `:1728` a `:2017` para después. G `:1134`–`:1514` y `:1752`–`:2101`. Hay 23 y32 intervalos H evaluados respectivamente.

**Inferencia alta:** persiste carga del hilo H cuando procesa enemigos. El RTT de sesión muy superior al ping y la cadencia de muestras lo apoyan. **No demostrado:** cuánto tarda exactamente `enemy_host_tick` ni qué instrucción domina.

## 6. Arreglo propuesto, sin aplicar

### A. Prioridad inmediata: margen y clasificación de EnemyPos

Archivo `src/mod/net/remote_puppet.cpp`, `puppet_enemy_move:1322` y cola `Pending:239`; integrar con P2/P5 para no hacer dos refactors incompatibles.

Pseudocódigo de intención (no sustituye un diff compilable):

```cpp
// Conservar shift de timestamp y validación existentes.
auto ack = move_ack(m);
enqueue_enemy_motion(copy_key, world_epoch,
    make_packet(kOpMoveActorAck, *ack),
    steady_ms() + move_hold_for(delay_ms)); // máximo100, no200
// Pending distingue dueño y clase:
// companion_motion / enemy_motion / actor_load / actor_state / actor_remove.
// La purga del compañero no elimina enemigos y viceversa.
// El consumo rechaza epoch antiguo, pausa/carga y movimiento demasiado viejo.
```

No limitarse a cambiar el booleano `false` a `true`: `purge_movement()` (`:291`) hoy borra todos los marcados, por lo que una retirada del compañero podría borrar movimientos de copias aún válidas. Mantener orden Load→acción inicial→Move; no dar a acciones cualquier espera100 sin revisar sus marcas y orden de acción.

Riesgo: entregar antes expone más al jitter y cambia relación con `StartAction`; comprobar acciones de giro/ataque, no solo desplazamiento. No bajar `coop delay` global ni `kClientRenderDelayMs` para ocultar el síntoma. No retirar SEH/VirtualQuery.

### B. Medir y acotar trabajo H antes de elegir hilo/presupuesto

`CoopLink::update:129–171`, `enemy_host_tick:1399`, `run_network`:

```text
NetLoop: epoch, vueltas, loop_us p50/p95/max,
 recv_us, zone_us, enemy_tick_us, reliable_us, sample_us,
 send_first_us, send_last_us, sampled_actors, failed_reads,
 outgoing_age_us p50/p95/max
EnemyPosTrace: epoch, copy, native, ts_game, sampled_qpc,
 sent_qpc, recv_qpc, queued_due, injected_qpc, ts_before, ts_after,
 local_clock, paused, world_ready, drop_reason
```

Agregar contadores acotados por ventana y trazas seleccionables para dos claves; no escribir una línea por muestra de48 enemigos permanentemente. Relojes QPC entre máquinas requieren calibración o secuencia de intercambio; los intervalos dentro de cada PC sí son directos.

Si `sample_us` domina, siguiente cambio condicionado a medición: presupuesto de lecturas por vuelta y recorrido justo de claves, o productor que publique snapshots; no aumentar hilos que toquen memoria del juego sin resolver lifetime, locks y epoch. Esa decisión aún es hipótesis.

### Qué no tocar

- Mantener los dos `send_out()` y `actor_world_transform` actuales.
- Mantener límites de seguridad, validación de claves y lecturas protegidas.
- No cambiar protocolo para corregir el hold local. Si se añaden epoch/seq al **wire**, incrementar `kVersion`; una sonda local no requiere hacerlo.
- No confundir parada estática de1s con paquetes perdidos ni restamp con tick nuevo del emisor.

## 7. Prueba mínima para cerrar P3

1. Dos PC, misma build, sin menús durante referencias: ambos caminan60s sin copias; después60s con24 y48 copias activas.
2. G inicia pelea con dos enemigos; registrar sus claves y activar trazas solo para esas copias.
3. H muere, espera menú10s y reaparece. G vuelve a golpearlos antes de que H se acerque; repetir tras una carga de zona.
4. Ejecutar el análisis de capturas y comparar ventanas con misma actividad. Criterios: subida p1 H→G≤20ms; shown mediano≤450ms bajo la misma calibración; tardías<10% **por cada enemigo activo**, no solo promedio de todos.
5. Objetivo de muestreo: mediana≤80ms y p95 documentado durante movimiento continuo; distinguir keepalive de1s. `EnemyPosTrace` debe explicar toda pérdida de margen y no contener entregas de epoch anterior.
6. `NetLoop` debe identificar duración real y bloque dominante. Sin esa sonda no declarar resuelto el coste del bucle basándose solo en líneas Net.

**Estado de cierre:** mejora principal del jugador confirmada en estas capturas; P3 no se cierra globalmente mientras fallen el margen de los enemigos y la entrega durante pausa/carga. Dependencias: P2 para IA/reacción y P5 para gate de ciclo de vida.
