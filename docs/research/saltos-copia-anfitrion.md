# Saltos pequeños de la copia durante el movimiento normal (prueba con dos PC, 5 oct 2026)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Mod `6318627D`, protocolo 6.
Análisis de solo lectura de los registros y las capturas. No se cambió código.

**Datos:**
- Anfitrión: `bin64\cdmp.log` y `cdmp_packets_20261005-204149.bin`.
- Invitado (Shadow): el `cdmp.log` del invitado (5 oct) y `cdmp_packets_20261005-202422.bin`.
- Todas las horas de este documento están en **hora del anfitrión**. Shadow va una hora por detrás.

**Scripts** en `build/research/saltos/`:
- `extract.py` y `extract_q.py` leen el `.bin` en streaming y escriben un CSV de Move, StartAction y Rotate (Req y Ack).
- `load.py`: utilidades comunes.
- `a1.py` y `a7.py`: reloj del juego frente a QPC, y pausas.
- `a2.py` y `a3.py`: el adelanto de las muestras y una réplica del búfer de interpolación (sin excluir nada).
- `a9.py`: lo mismo solo en juego normal, excluyendo pausas y recargas; añade categorías, StartAction y Rotate.
- `a8.py`: huecos del emisor y espera de inyección.
- `a10.py`: retraso de llegada.

Nota sobre el tiempo de las capturas: `t_us` usa el QPC de cada PC. Para emparejar los dos PC uso
cada `MoveActorReq` del emisor con el `MoveActorAck` inyectado en el receptor que tiene **la misma
posición exacta** (en total se emparejaron 32 658 de 33 016 en el invitado).

---

## 0. Resumen

1. **El retardo de 300 ms del mod no amortigua el jitter.** El mod hace dos cosas con esos 300 ms:
   - retiene cada evento 300 ms desde que llega (`enqueue_at(now + delay_ms)`);
   - suma esos mismos 300 ms a su marca de tiempo (`ts_shift = offset + delay_ms`).

   Las dos cosas se anulan. Cuando el paquete entra en el cliente, lo que queda de margen es solo
   el retardo de interpolación del propio juego (200 ms) **menos lo que el paquete llegó tarde**.
   Lo medido en juego normal:
   - el adelanto de la muestra al inyectarla tiene una mediana de **−97 ms** en el invitado y **−105 ms** en el anfitrión;
   - el margen típico que queda es de unos 100 ms.

   Cuando un paquete llega más de ~185 ms tarde respecto al más rápido, el cliente se queda sin
   muestras. Como el juego **no extrapola**, la copia se para y después da un salto hacia delante.
   Ese es el "tirón" pequeño.
2. **El retraso viene de dos fuentes:**
   - **La red de Steam.** Por encima del paquete más rápido, el retraso tiene p50 de unos 56 ms,
     p90 de 105 ms y p99 de 184 ms (invitado). En el anfitrión es p99 de 233 ms.
   - **La inyección.** El mod solo puede inyectar cuando llega un paquete S>C real al hook de
     `OnRecvPacket`, y esos paquetes llegan a ráfagas:
     - la espera media es de 28 a 32 ms;
     - el 10 % del tiempo estamos dentro de huecos de 96 ms o más (129 ms en el anfitrión);
     - los Move se entregan en grupos de 3 a 12.
3. **El efecto medido** en juego normal (réplica exacta del búfer del cliente, sin pausas ni
   recargas):
   - en el invitado, la copia del anfitrión se queda sin muestras el **2,4 %** del tiempo que se mueve: unas 21 paradas de ≥ 33 ms por minuto;
   - en el anfitrión, la copia del invitado, el **8,1 %**.

   A caballo (8 a 13 m/s), una parada de 33 a 100 ms es un salto de **0,3 a 1,3 m**. Andando es de 5 a 30 cm.
4. **Por qué se nota más en el invitado** aunque las cifras sean parecidas o peores en el
   anfitrión (§2.7):
   - el anfitrión **montó mucho más** a caballo (17 290 Move derivados del caballo en el invitado, frente a 6 768) y se movió más rápido;
   - tuvo **más pausas de reloj** (12, algunas cortas de 0,6 a 2 s);
   - el invitado sufrió **5 recargas por desmontar** con saltos de 1 a 7 m;
   - lo que ve el invitado se mira a través del streaming de Shadow, a 50–79 fps.

   El mecanismo es el mismo en las dos direcciones.
5. **Lo que no es la causa:**
   - **Las líneas "physics basis not a rotation"** son de cuando se va **a caballo**
     (21:29–21:35), no nadando. El juego sigue enviando `MoveActorReq` a 67 Hz, así que lo que se
     envía no cambia; solo afecta a los Move sintetizados (que ahí no hacen falta) y al
     `PlayerState`.
   - **Las posiciones de StartAction** coinciden con la trayectoria de los Move (p90 < 0,15 m).
   - **Los gap-fill no son más en el invitado.** El registro entero tiene 14 en el invitado y 13 en
     el anfitrión (no 3), y la mayoría son escaladas cortas y recargas.
6. **Arreglo principal (§4, P1):** separar la retención (`hold`, unos 100 ms o programada por
   marca de tiempo) del desplazamiento de la marca de tiempo (D = 300 ms). Así el margen pasa de
   unos 185 ms a unos 485 ms, **sin añadir latencia**. Hace falta además:
   - recortar los Move a unos 40 Hz (P2), para no superar las 30 muestras del búfer del cliente;
   - una guardia de marcas de tiempo crecientes (P3);
   - no rellenar huecos a través de una recarga (P4).

---

## 1. Por qué los 300 ms de "playout" no dan margen

Repaso de `movimiento-remoto-cliente.md` §2:
- el cliente pinta la copia en `T = reloj_juego − 200 ms`, interpolando un búfer de muestras ordenadas por `ts`;
- no extrapola: si `T` pasa la última muestra, el actor se queda quieto (S6);
- cuando llega una muestra nueva, salta a donde "debería" estar (S2).

Lo que hace el mod (`coop.cpp:378`, `remote_puppet.cpp:1130` y `:1292`):

```
ts_reb  = ts_emisor + offset + 300                 // offset = min(reloj_local_llegada − ts) en 4 s
entrega = llegada + 300 + w                        // w = espera hasta el siguiente S>C real
adelanto al inyectar = ts_reb − reloj_local(entrega) = −(J + w)
          (J = retraso de ese paquete sobre el más rápido de los últimos 4 s)
margen frente al punto de pintado = 200 − J − w
```

Así que los 300 ms solo **retrasan** la copia (unos 500 ms detrás del jugador real) y no absorben
nada. La copia se queda sin muestras cuando `J + w > 200 − Δts`, que es unos 185 ms a 68 Hz.

Comprobación directa (`a2.py`): el adelanto medido de los Move inyectados tiene una mediana de
−81 ms (invitado) y −93 ms (anfitrión), con toda la sesión. Si la retención amortiguara, sería de
unos +200 ms.

Lo mismo vale para la retención de las acciones del caballo (`ride_horse` y `horse_event`).

---

## 2. Medidas

### 2.1 Cadencia del emisor (`a8.py`, `a5.py`)

| | Anfitrión → invitado | Invitado → anfitrión |
|---|---|---|
| `MoveActorReq` enviados | 34 491 | 23 944 |
| Intervalo p50 / p90 / p99 | 14,4 / 19,8 / 114 ms (unos **68 Hz**) | 15,9 / 20,5 / 130 ms |
| Huecos ≥ 150 ms **moviéndose** | 16, todos remates de escalada ("up", con 2–4 StartAction) | 11, igual |
| Huecos ≥ 150 ms quieto | 222 (128 s) | 180 (111 s) |
| Move sintetizados o gap-fill vistos por el receptor | 358 | 414 |
| Pares consecutivos con la **misma ts** | 294 (casi todos montando) | 155 |

No hay huecos de envío al nadar, planear o correr: el juego manda `MoveActorReq` a cada frame. Los
únicos huecos en movimiento son los remates de escalada (165–490 ms, y 1,9–3 s en los más altos).

### 2.2 Retraso de llegada sobre el más rápido (red + inyección), juego normal (`a10.py`)

| Receptor | p50 | p90 | p95 | p99 | p99,9 | > 150 ms | > 200 ms |
|---|---|---|---|---|---|---|---|
| Invitado (copia del anfitrión) | 56 | 105 | 126 | 184 | 283 | 2,4 % | 0,65 % |
| Anfitrión (copia del invitado) | 60 | 117 | 146 | 233 | 1254 | 4,7 % | 1,7 % |

RTT de la sesión: unos 56 ms de media, con un máximo de 95 ms. No hubo pérdidas apreciables.

### 2.3 Espera de inyección: solo en S>C reales (`a8.py`)

| | S>C reales/s | Espera media | 10 % del tiempo en huecos ≥ | 5 % ≥ | Move por ráfaga p50/p90/p99 |
|---|---|---|---|---|---|
| Invitado | 83 | 27,5 ms | 96 ms | 144 ms | 3 / 5 / 11 |
| Anfitrión | 76 | 31,7 ms | 129 ms | 186 ms | 3 / 6 / 12 |

`troc_sniffer.cpp:on_recv_packet` vacía la cola solo cuando llega un paquete real. Esta espera se
suma a J.

### 2.4 Paradas y saltos en la réplica del búfer, juego normal (`a9.py`)

La réplica es exacta: reglas de `setInterpolatePositionData` y `update…`, con un retardo de 200 ms,
30 muestras como máximo y reinicio a los 1000 ms. Usa el reloj de juego real de cada PC, obtenido
de sus propios Req (`ts − t`, constante entre pausas). Se excluyen las pausas de reloj de los dos
PC (−3 s a +15 s) y las recargas de la copia (−3 s a +6 s).

| Copia vista en | Categoría | Moviéndose | Sin muestras | Paradas ≥ 33 ms/min | ≥ 100 ms/min | Adelanto p50 / p5 / p1 |
|---|---|---|---|---|---|---|
| Invitado | a caballo | 1,4 min | 1,4 % | 12,9 | 0 | −108 / −168 / −207 ms |
| Invitado | andando (< 3 m/s) | 1,2 min | 5,1 % | 40,8 | 4,9 | −97 / −191 / −265 |
| Invitado | rápido (> 6 m/s, caída o planeo) | 0,7 min | 0,2–1,7 % | 3,5–13 | 0 | −97 / −160 / −200 |
| Invitado | **total** | 4,1 min | **2,4 %** | 21 | 1,5 | |
| Anfitrión | andando | 1,5 min | 8,6 % | 50 | 10 | −105 / −222 / −297 |
| Anfitrión | a caballo | 0,4 min | 8,6 % | 65 | 13 | −111 / −225 / −305 |
| Anfitrión | **total** | 3,4 min | **8,1 %** | 51 | 10 | |

Sin excluir nada (`a3.py`), los saltos visibles (paso de un frame mayor que el ideal + 0,15 m y
mayor que 2,5 veces el ideal) son:
- **invitado:** 36 eventos; 24 de más de 0,3 m y 5 de más de 1 m. Los mayores son las recargas
  por desmontar: 7,1 m a las 21:34:53, 4,9 m a las 21:31:37 y 1,1 m a las 21:49:55 y a las
  21:32:39. Después vienen paradas yendo rápido o en caída (21:54:12–29, 0,4–0,75 m) y corriendo
  (21:46:31–34, 0,4–0,65 m).
- **anfitrión:** 98 eventos.

### 2.5 Otras fuentes revisadas

- **Posición de StartAction frente a la trayectoria de los Move en su `ts`:** p50 de 0,00–0,03 m
  y p90 por debajo de 0,15 m en todas las categorías. No producen saltos.

  Una primera medida daba 5–19 m al ir a caballo. Era un artefacto: tras una pausa, los rangos de
  `ts` rebasada de épocas distintas se solapan (ver §2.6).
- **RotateActorAck:** aplica la rotación **al instante** (`SetRotation` de tipo 5), por delante
  de la posición interpolada.
  - En juego normal: en el invitado 27 de 235 giran la cara más de 20° respecto a la que se
    muestra, y 2 más de 45°. En el anfitrión, 111 de 312 y 50.
  - Puede verse como un tic de orientación de un frame. Es una hipótesis, de prioridad baja.
- **ts iguales consecutivas** (294 y 155) y **Move reales después de un sintetizado con `ts`
  menor:** el cliente descarta la segunda muestra (unas 60 por lado). El efecto es muy pequeño.
- **"physics basis not a rotation" en el anfitrión:** 204 líneas, alternando con "in world" cada
  60–100 ms.
  - Ocurre entre 21:29 y 21:35 y en 21:32–21:33, siempre **a caballo**: el `parent` del Move es el
    caballo `0xB0100A20`.
  - Entre 21:31:26 y 21:32:50 el anfitrión envió 5 123 Move, unos 67 por segundo, y los únicos
    huecos > 100 ms son una escalada.
  - **No afecta a lo que se envía.** Afecta a `capture_synth` (que ahí no hace falta), al
    `local_pos` (tiene 500 ms de gracia) y al `PlayerState`, que se usa como posición de recarga.
  - La tolerancia de `math::is_orthonormal` (0,02) probablemente es demasiado estricta para la
    base del jinete.
- **Gap-fill:**
  - **invitado:** 14 en total. 5 son de 1,5 a 3 s "down" justo después de "spawn queued", es
    decir, recargas por desmontar (§3, C4). Los otros 9 son remates de escalada de 0,17 a 0,5 s.
  - **anfitrión:** 13 en total, 6 por recarga y 7 por escalada. Los "3" del anfitrión eran solo
    la primera parte del registro.

### 2.6 Pausas de reloj (relación con `pausa-copia-congelada.md`; `a7.py`)

| PC | Pausas de reloj | Más ejemplos |
|---|---|---|
| Anfitrión | 12: entre 0,6 y 32 s | Pausas cortas a las 21:38:53, 21:46:00, 21:46:23 y 21:51:45 (0,6–2 s) |
| Invitado | 11 | Además, 4 tirones de 116–196 ms (Shadow) |

Después de cada pausa, `ClockMapper` salta (39 y 37 saltos de más de 250 ms en los informes cada
5 s). Mientras tanto:
- las muestras llegan ya pasadas, lo que provoca un salto;
- o llegan con la `ts` en el futuro: la copia anda en el sitio o se pierden muestras por desborde.

También aparecen **rangos de `ts` rebasada que se solapan** entre épocas. Es el problema de
"camina en el sitio tras el menú", que se estudia en `pausa-copia-congelada.md`. Aquí queda fuera, salvo que en el
invitado las pausas **del anfitrión** también se ven como saltos de su copia.

Además, el `ClockMapper` se desplaza **1 ms por muestra**, es decir, unos 68 ms/s. Mientras
converge (|objetivo − offset| tiene una media de unos 20 ms, y en el 10 % de los informes es de más
de 50 ms), la copia anda un **±7 %** más rápida o más lenta. Es un efecto de goma, no un salto.

### 2.7 ¿Por qué se ve peor en el invitado?

Con las mismas reglas, la copia del invitado en el anfitrión se queda sin muestras más a menudo.
La diferencia que se percibe sale de:
1. **El tipo de movimiento del anfitrión.**
   - Montó mucho más: 17 290 Move de su caballo en el invitado, frente a 6 768.
   - Corrió, cayó y planeó más rápido.
   - La misma parada de 50 ms es de unos 0,5 m a caballo y de unos 0,1 m andando.
2. **Más pausas de reloj del anfitrión** (cortas y largas) y **5 recargas por desmontar** en el
   invitado, que dejan los saltos más grandes (§3, C4).
3. **El invitado se mira por streaming** (Shadow, a 50–79 fps), que añade su propio judder. No se
   puede medir desde aquí.

---

## 3. Causas, por orden de impacto en el movimiento normal

| # | Causa | Evidencia | Dónde |
|---|---|---|---|
| **C1** | La retención y el desplazamiento de `ts` son el mismo número. El margen real es solo los 200 ms del cliente, ya gastado a medias (adelanto p50 de unos −100 ms). | §1, §2.4 | `coop.cpp:378`; `remote_puppet.cpp` `puppet_event` (`now + delay_ms` y `g_ts_shift`), `ride_horse`, `horse_event`, `fill_gap_before` |
| **C2** | Inyección solo a lomos de los S>C reales: espera media de unos 30 ms, colas de 100–190 ms y entrega en ráfagas. | §2.3 | `troc_sniffer.cpp:on_recv_packet` |
| **C3** | Jitter de Steam: p99 de 184–233 ms sobre el más rápido. El `offset` usa el **mínimo** de 4 s, así que cualquier paquete más lento cuenta como retraso. | §2.2 | `clock_mapper.h` |
| **C4** | Gap-fill **a través de una recarga**: `g_last_move` sobrevive a `puppet_remove` y al nuevo spawn. Rellena 1,5–3 s con muestras "down" que ya vencieron, y la copia recién cargada salta de 1 a 7 m. | §2.4 y log del invitado 20:31:37, 20:32:39, 20:34:53, 20:49:55, 20:52:10 (hora Shadow) | `remote_puppet.cpp:fill_gap_before` / `puppet_remove` |
| **C5** | El `ClockMapper` se desplaza por muestra: ±7 % de velocidad mientras converge, y no hay cota de `ts` creciente por actor. | §2.5, §2.6 | `clock_mapper.h`, `puppet_event` |
| C6 | `ts` iguales del emisor, o un Move real con `ts` menor que uno sintetizado: se descartan muestras. | §2.5 | `local_capture.cpp:capture_synth` y `puppet_event` |
| C7 (hipótesis) | `RotateActorAck` fija la orientación al instante, por delante de la posición. | §2.5 | `rotate_ack` |

---

## 4. Propuestas de arreglo

### P1. Separar la retención (H) del desplazamiento de marca de tiempo (D): la principal

Objetivo: que el margen frente al punto de pintado pase de `200 − J − w` a `D + 200 − max(H, J) − w`
**sin cambiar la latencia** (la copia sigue unos `D + 200 ≈ 500 ms` por detrás).

**Opción recomendada: entrega programada por marca de tiempo.**
- En `CoopLink::on_event`, para eventos con `ts`:
  - `objetivo_local = ts + offset + H`;
  - `hold = clamp(objetivo_local − capture_game_clock_ms(), 0, D)`;
  - llamar a `puppet_event(ev, offset + D, hold)`.
- Un paquete que llega tarde se inyecta enseguida y no se queda retenido 300 ms más.
- Valores: `H ≈ 100 ms` y `D = coop.delay_ms` (300).

**Variante sencilla:** `puppet_event(ev, offset + D, H)` con H fijo en 100. El margen pasa a
`D + 200 − H − J − w` (unos 385 ms).

**Cosas que dependen hoy de la retención de 300 ms y hay que mantener:**
1. **Re-path de StartAction en el remate de escalada** (`fill_gap_before` recorre la cola). Con
   H = 100, las StartAction del congelado ya se habrían entregado.

   Retención condicional: mientras el emisor está congelado (`now − g_last_move_ms ≥
   kMoveSilenceMs`), StartAction y Rotate usan `hold = D`. Así siguen en la cola cuando llega el
   Move que reanuda. Las StartAction posteriores a ese Move ya las corrige `g_recent_gap`.
2. **WallUp** tiene que llegar justo antes de su StartAction (`kLead = 40`). Debe usar la retención
   vigente (la última aplicada a una StartAction o Move, guardada en una variable del hilo de
   eventos) menos 40, no `delay_ms − 40`.
3. **Docking, Equip y Relay** (sin `ts`): pueden seguir con `hold = D`. Llegar después de su acción
   no es un problema; llegar antes sí, y con D ≥ la retención de la acción no ocurre.
4. **Gap-fill**: los `due` de las muestras sintetizadas (`g_last_move_ms + k·50 + delay`) deben
   usar la misma retención. Si ya vencieron, se entregan en orden antes del Move que reanuda, que
   es lo correcto.
5. **Caballo** (`ride_horse` y `horse_event`): la misma retención que el Move del jinete del que
   sale, para que interpolen juntos.
6. **Configuración:** añadir `[coop] hold_ms=100` y mantener `delay_ms=300` como D.

### P2. Recortar los Move a unos 40 Hz en el receptor, o subir el tope del búfer del cliente

Con P1, al inyectar quedan unos `D − H + 200 = 400 ms` de muestras futuras. A 68 Hz son unas 27, y
las StartAction comparten búfer. Eso roza el tope de 30 (`exe+0x6CB9598`, build 2976).

Si se supera, el cliente tira la muestra más antigua. Si todas son futuras, `T ≤ ts[0]` y la copia
se para (S4).

- **Recomendado, en `puppet_event`, caso Move del jugador:** descartar un Move si `ts −
  ts_del_último_Move_encolado < 22 ms`.
  - No descartar nunca un Move cuyo `parent` (`+0x8C`) cambie, ni el primero tras un hueco.
  - Las StartAction no se descartan.
  - `ride_horse` se deriva solo de los Move que se conservan.
  - El documento del cliente ya dice que 25–40 Hz bastan, porque la velocidad se deriva de la
    interpolación.
- **Alternativa** (escribe en la memoria del juego; copia de saves antes): subir
  `GlobalVariableInt exe+0x6CB9598` (valor en `obj+0x38`, build 2976) de 30 a 64. Solo lo usan
  los actores en modo 2, es decir, nuestras copias.

### P3. Guardia de `ts` estrictamente creciente por actor (Move + StartAction + gap-fill)

En `puppet_event`, tras rebasar la `ts`: `if (ts ≤ g_last_injected_ts[key]) ts = g_last_injected_ts[key] + 1`.

- Hay una variable por clave de copia (jugador y caballo).
- Así se evitan los descartes por `ts` iguales del emisor (C6) y por la convergencia del offset.
- Al hacerlo, desplazar también `g_last_move_ts` del relleno.
- **No** aplicarla cuando el offset salta por una pausa (más de 250 ms). Eso lo resuelve el arreglo
  de menús (`pausa-copia-congelada.md`). Por eso conviene coordinar el cambio.

### P4. No rellenar huecos a través de una recarga, ni con muestras ya vencidas

En `puppet_remove` y `puppet_forget`, invalidar la referencia del relleno, por ejemplo con
`g_last_move_ts = 0` y `g_gap_ref_valid = false`.

**No vaciar `g_last_move`:** `puppet_enemy_move` y `puppet_spawn_enemy` lo usan como plantilla.

En `fill_gap_before`:
- salir si la referencia no es válida;
- salir si `g_last_move_ms` es anterior al último `puppet_spawn`;
- saltarse las muestras cuyo `due` ya pasó de largo (`due + 200 < now`), porque se aplicarían de
  golpe.

Esto quita los 5 saltos de 1 a 7 m tras desmontar en el invitado y 6 en el anfitrión. No toca el
problema de la ropa al desmontar, que se estudia en `ropa-al-desmontar.md`.

### P5. ClockMapper: corrección por tiempo, no por muestra (opcional: D adaptativo)

En `clock_mapper.h`:
- limitar la corrección a unos 20 ms por segundo (2 %), en vez de 1 ms por muestra (7 % a 68 Hz);
- añadir tests en `tests/tests.cpp`.

**Opcional, D adaptativo:** la ventana ya guarda las muestras de 4 s.
- `J95 = p95(sample) − min(sample)`;
- `D = clamp(J95 + 60, 150, 400)`, suavizado;
- D solo baja despacio, para no hacer retroceder las `ts`.

Con P1 y D = 300 fijo, el margen (unos 485 ms) ya cubre el p99,9 medido (283 ms en el invitado), así
que esto es secundario. Serviría para **bajar** la latencia cuando la red es buena.

### P6 (baja prioridad)

- **`is_orthonormal`:** subir la tolerancia (por ejemplo a 0,05) o reortonormalizar la base antes
  de rechazarla (`player_reader.cpp:88`). Así el `PlayerState` y la posición de recarga no se
  quedan viejos al ir a caballo.
- **`RotateActorAck`:** probar si el tic de orientación se ve. Si se ve, no inyectar Rotate
  mientras lleguen Move, porque el Move ya trae el quaternion en `+0x10`.
- **No extrapolar desde el mod:** una muestra predicha con `ts` mayor hace que el cliente descarte
  las reales posteriores (`ts ≤ última`), y los fallos de predicción serían saltos. Es mejor dar
  margen (P1).

---

## 5. Plan de pruebas

1. **Validación sin tocar código** (escribe en la memoria; copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` antes):
   - subir `GlobalVariableInt exe+0x6CB9548` (retardo de interpolación del cliente, build 2976)
     de 200 a 400 solo en el invitado;
   - repetir el recorrido de la prueba.

   Si los tirones desaparecen, C1 queda confirmado. A cambio, la copia va 200 ms más tarde.
2. **Prueba unitaria y local:** P3, P4 y P5 tienen lógica pura. Hay que añadir casos a `tests/tests.cpp`:
   - **ClockMapper:** la corrección por tiempo, y que no se salta con jitter de 100 ms;
   - **guardia de `ts`:** se cumple con Move y StartAction intercalados y con `ts` iguales;
   - **`fill_gap_before`:** no hace nada tras un `puppet_remove` seguido de un `puppet_spawn`.
3. **Loopback con `cdmp_peer`.** El mod tiene `dev_mirror_delay_ms`, pero el jitter de Steam no
   se reproduce en loopback. Usar `cdmp_peer` con un retardo aleatorio, si se le añade un modo
   jitter (p50 de 60 ms y cola de 250 ms), para medir con `a9.py` antes y después.
4. **Prueba con los dos PC (Shadow como invitado), con P1 a P4 activos.** Un recorrido de unos 10
   minutos en el anfitrión, sin abrir menús:
   - andar;
   - correr;
   - montar a galope en línea recta y en círculos;
   - 3 remates de escalada;
   - una escalada libre;
   - planear o caer;
   - nadar;
   - desmontar 2 veces.

   Marcar Ctrl+F12 en los dos PC al empezar cada tramo.
5. **Métricas** (los scripts sirven tal cual):
   - `a9.py`:
     - menos del 0,3 % del tiempo sin muestras en todas las categorías;
     - 0 paradas de 100 ms o más;
     - adelanto p1 > −100 ms.
   - `a3.py`: 0 saltos de más de 0,3 m fuera de pausas y recargas.
   - Tras desmontar: ningún "filled a ≥1000 ms gap" y ningún salto de más de 1 m.
   - Escaladas: el log sigue mostrando "re-pathed N queued StartAction positions" con N > 0
     (comprueba que la retención condicional funciona) y no hay vuelta al pie del muro.
   - Revisar que no hay overflow (P2): con la réplica, el número de muestras futuras al inyectar
     debe ser de 25 o menos.
6. **Visual:** capturas con `CopyFromScreen` en el invitado durante el galope (antes y después).
   Mejor aún, una grabación corta por el lado de Shadow, aunque pasa por el streaming.

---

## 6. Límites del análisis

- **El reloj de juego de cada PC** se reconstruye con sus propios Req. Cuando el jugador local
  está quieto mucho rato, no hay referencia. Por eso solo se analizaron con categoría unos 4
  minutos de movimiento por lado, después de excluir pausas y recargas.
- **La réplica asume 60 fps.** En el invitado son 50–79.
- **La categoría** (a caballo, andando…) se deduce de la velocidad y del `parent` de los Move. No
  se distingue nadar de andar.
- **El tic de `RotateActorAck`** no está verificado en el juego.

---

## 6. Regresión tras el protocolo 7 (prueba del 5 oct, 23:21-23:34)

Build del juego **1.0.0.2976** (PE `0x6AB28F00`). Mod `0BD48B66` + cambios locales (= `b22bee2`),
protocolo 7; el `cdmp.asi` del anfitrión y el del kit del invitado tienen el mismo SHA-256
(`43903A80…`), y el `cdmp.ini` del anfitrión es idéntico a `cdmp.ini.prueba-shadow-anfitrion`.
Análisis de solo lectura; no se cambió código.

**Síntoma:** en Shadow (invitado) la copia del anfitrión se vio mucho más "a tirones" que en la
prueba anterior; en el anfitrión, la copia de Shadow se vio normal.

**Datos:**
- Anfitrión: `cdmp.1.log` (23:21-23:31), `cdmp.log` (23:32-23:34), `cdmp_packets_20261005-232134.bin`
  (la de 23:32 casi no tiene copia: 4 Ack inyectados).
- **Los registros de Shadow de esta prueba no han llegado** (en la carpeta compartida de logs solo
  están los de la prueba de las 20:24). Todo lo que sigue sale del lado del anfitrión, del
  código, y de una réplica offline sobre la captura de Shadow de ayer
  (`cdmp_packets_20261005-202422.bin`).
- Scripts en `build/research/regresion7/`: `r1.py` (cadencia y `ts` de lo enviado y lo inyectado),
  `lead.py` + `lead2.py` (adelanto al inyectar y réplica del búfer), `fclock.py` + `pd.py`
  (réplica del `PauseDetector`), `sti.py` + `steps.py` (saltos del reloj en cada
  `ServerTimeInfoAck`), `winmax.py` (fotogramas largos), `bumps.py` (los +1 del re-sellado) y
  `sim.py` (réplica del receptor de Shadow, lógica vieja frente a nueva).

### 6.1 Resumen

1. **Lo que envió el anfitrión está igual de limpio que ayer**, y la red fue algo mejor. No hay nada
   en el emisor ni en Steam que explique un empeoramiento (§6.2).
2. **En el anfitrión, el mismo código de recepción mejoró** frente a ayer: el adelanto mediano de
   las muestras inyectadas pasó de −96 a −55 ms y el tiempo sin muestras de la réplica, del 11,5 % al
   5,0 % (§6.3). Así que el código nuevo no empeora por sí mismo en un PC estable.
3. **El código nuevo sí tiene un fallo que afecta más a Shadow (R1):** el `PauseDetector` decide
   "pausado" con **una sola ventana de 200 ms** en la que el reloj avanza menos de la mitad. Eso
   también ocurre sin menú:
   - con un **fotograma de más de ~105 ms** (el reloj que leemos es el de inicio del fotograma y
     no avanza durante él; §6.4);
   - con un **salto hacia atrás del reloj del cliente** al llegar un `ServerTimeInfoAck` (hallazgo
     nuevo, §6.4): en Shadow, ayer, 8 de 325 saltaron ≥ 100 ms (hasta −502 ms).

   Cada falso positivo retiene todo el movimiento de la copia **≥ 200 ms** y luego lo suelta de
   golpe: con el margen actual de ~100 ms (C1, §1) eso es una parada de 0,1-0,3 s seguida de un
   salto. Ritmo estimado en Shadow: **0,2-0,6 por minuto**.
4. **Otros efectos del código nuevo, menores** (§6.5):
   - R2, los `+1` del re-sellado: convierten el descarte de una muestra con `ts` repetida en un
     tramo de 1 ms;
   - R3, el `ClockMapper` alimentado con el reloj de fotograma: su mínimo coge la lectura más
     vieja. En el anfitrión el balance fue positivo; en Shadow no se puede medir sin su captura.
5. **Réplica offline sobre la captura de Shadow de ayer** (§6.6): la entrega nueva (detector +
   re-sellado) sube el tiempo sin muestras del 4,32 % al 4,62 % y las paradas cortas de 16,8 a
   19,5/min. Es un empeoramiento real pero pequeño: **no explica por sí solo "mucho peor"**.
6. **Conclusión provisional:**
   - el "mucho peor" probablemente sale de Shadow: fotogramas largos o saltos de reloj más
     frecuentes que ayer, sumados a su streaming;
   - lo amplifican dos cosas: el margen base, que sigue siendo de ~100 ms (P1 sin hacer), y R1, que
     convierte cada tirón de Shadow en una retención extra de ≥ 200 ms;
   - **la prueba definitiva es el log de Shadow** (§6.8).

### 6.2 Lo que envió el anfitrión (`r1.py`)

| | Hoy (232134) | Ayer (204149) |
|---|---|---|
| `MoveActorReq` propios / tiempo moviéndose | 8 960 / 135 s (66 Hz) | 34 491 / 603 s (57 Hz) |
| Δts p50 / p90 / p99 / p99,9 | 14 / 15 / 30 / 280 ms | 14 / 20 / 90 / 293 ms |
| Pares consecutivos con la misma `ts` | 19 | 294 |
| `ts` hacia atrás | 0 | 0 |
| Move sintetizados (`capture_synth`) | < 101 (el log solo dice "1 moves synthesised") | — |
| RTT medio / máximo (log `Net:`) | 46 / 65 ms | 56 / 95 ms |
| Cola de envío de Steam | 0 ms siempre | 0 ms |

- El anfitrión se movió poco: 135 s en 10 min. La prueba era de ropa y pausas, con muchos menús en
  los dos lados (el `clock offset` del anfitrión salta de 4178 a 36114, −285 y 14660 ms…).
- Los Move sintetizados ahora llevan el reloj de fotograma (`capture_game_clock_ms`). Solo salen
  tras 150 ms sin `MoveActorReq`, es decir, en remates de escalada, y hoy fueron casi ninguno.
  `capture_horse_synth` (caballo sin jinete) también usa ese reloj, que lo único que cambia es que
  se para en los menús. Ninguno de los dos produce `ts` repetidas ni hacia atrás.

### 6.3 La recepción en el anfitrión, antes y después (`lead.py`, `lead2.py`)

Adelanto = `ts` inyectada − reloj del cliente en ese momento (envolvente de los `MoveActorReq`
propios y de NPC). Réplica del búfer a 60 fps con retardo 200; solo copia en movimiento y con el
reloj estable.

| Receptor | Adelanto p1 / p5 / p50 / p90 | < −200 ms | Sin muestras | Paradas ≥ 6 fot./min |
|---|---|---|---|---|
| Anfitrión, **hoy** (protocolo 7) | −315 / −195 / **−55** / −27 | 4,5 % | **5,0 %** | 10,2 |
| Anfitrión, ayer (protocolo 6) | −373 / −269 / −96 / −51 | 10,4 % | 11,5 % | 11,3 |
| Shadow, ayer (protocolo 6) | −364 / −188 / −85 / −47 | 4,3 % | 4,7 % | 4,7 |

- Hoy la red fue algo mejor (RTT −10 ms), pero eso no explica 41 ms más de margen. La mayor parte
  viene del reloj nuevo del `ClockMapper` (§6.5, R3).
- Re-sellado en el anfitrión: el contador del log llegó a **162** en 10 min.
  - Casi todo son `+1` sueltos sobre `ts` repetidas del emisor (Shadow), con 0,00-0,06 m.
  - Hay tres rachas largas, de 14 a 23 `+1` seguidos, justo tras pausas (23:23:47,9: 23 muestras en
    338 ms y 0,18 m).
  - Inofensivo aquí.
- El detector de pausa del anfitrión no dio falsos positivos claros. Todas las retenciones del log
  duran de 2 a 28 s y son menús, salvo una de 255 ms a las 23:34:43,7, al cerrar la sesión.
  - `puppet_tick` corre cada ~10 ms en el hilo del sniffer, así que cualquier retención de 200 ms o
    más queda en el log.

### 6.4 Dos hechos nuevos sobre el reloj del cliente [estático + captura]

1. **El "reloj de fotograma" que lee `game_clock.cpp` es la hora real al empezar cada
   actualización.**
   - `exe+0xAD1300` (en `0xAD19BE`-`0xAD19E3`) llama a `0x1416AF0`, el reloj real en ms
     (`(ns − [0x6EFBC70]) / 1e6`), y lo guarda en `[0x6C8CD78]`.
   - Si `[0x6C8CD35] == 0`, guarda también `[0x6C8CD68] = ms real` y pone `[0x6C8CD70]` a cero
     (`r12`).
   - Si `[0x6C8CD35] != 0`, en cambio, va sumando a `[0x6C8CD70]` el `dt` del fotograma en µs
     (`dt × 1e6`).
   - `0x1416B80` hace `[cd68] + [cd70] / 1000` (división exacta entre 1000, comprobada).
   - **Consecuencia:** visto desde otro hilo, el valor llega con un retraso de entre 0 y la duración
     del fotograma en curso.
   - El fotograma más largo de cada ventana de 4 s (`winmax.py`) tiene una mediana de **42-47 ms**
     y un p90 de **82-121 ms** en **los dos PC**: Shadow ayer, el anfitrión ayer y hoy, y Shadow hoy
     visto desde las `ts` de su copia.
2. **El reloj del cliente (fotograma + desfase `[0x6D69438]`) sigue a QPC entre dos
   `ServerTimeInfoAck` y salta en cada uno** (`steps.py`):
   - dentro de cada intervalo de 5 s, la deriva es < 10 ms en casi todos;
   - en la llegada del STI, el desfase se recalcula (`servidor − cliente`) y el reloj **salta hacia
     atrás** lo que el servidor perdió en tirones.

   | Captura | STI analizados | Salto ≥ 10 ms | ≥ 50 ms | ≥ 100 ms | Más grandes |
   |---|---|---|---|---|---|
   | Anfitrión hoy | 83 | 6 | 1 | 1 | −228 |
   | Anfitrión ayer | 797 | 40 | 10 | 9 | −1740, −735, −321 |
   | Shadow ayer | 325 | 31 (10 %) | 10 | 8 | −502, −359, −301, −280 |

   - Esto ya pasaba antes y afecta también al punto de pintado del propio cliente: con un salto de
     −X, `T` retrocede X, la copia queda con `T ≤ ts[0]` y se para X ms.
   - Además explica los "tirones" de Shadow de §2.6 (116-196 ms). En `hitch.py`, 4 de 6 pausas del
     reloj en tráfico denso tienen 106-144 ms de pared con solo 14-19 ms de avance.

### 6.5 Mecanismos del código nuevo que pueden dar tirones en Shadow

**R1. `PauseDetector` con una sola ventana de 200 ms** (`inject_clock.h`, llamado en cada S>C real
desde `puppet_take_injection`).
- Marca "pausado" si `dgame·2 < dwall` en una ventana de 200 ms o más.
  - Un fotograma de duración L congela la lectura; con la evaluación cayendo después de ~105 ms
    dentro de él, dispara. La probabilidad es de ~(L − 105)/210 por fotograma largo.
  - Un salto de −X ms en un STI dispara si X > ~105.
- Al dispararse, retiene **todo** el movimiento al menos hasta la siguiente ventana (≥ 200 ms) y lo
  entrega de golpe. Las muestras retenidas ya están detrás de `T`, así que la copia se para y salta.
- En el anfitrión no se vio, porque tuvo 1 salto de STI ≥ 100 ms en 10 min y fotogramas largos sin
  pérdida de reloj.
- En Shadow, con los datos de ayer: unos 8 saltos ≥ 100 ms en 34 min, más los fotogramas largos.
  La réplica (`sim.py`) cuenta **21 retenciones en 34 min**; la de `pd.py` sobre el reloj
  aproximado, 18 cortas.
- Es el único mecanismo nuevo que añade paradas **completas** de 200-400 ms; el resto son de
  pocos ms.

**R2. Re-sellado: `+1` en lugar de descartar.**
- `InjectClock::stamp` sube `ts ≤ última` a `última + 1`. Antes, el cliente descartaba esa muestra
  (S1); ahora la acepta **1 ms después** de la anterior. Si la posición es distinta, la copia
  recorre ese tramo en 1 ms.
- En el anfitrión, hoy, fue el 2 % de los Move de la copia (131 de 6 763) y el 3,4 % de los del
  caballo, con 0,00-0,06 m: no se nota.
- Lo que manda el anfitrión hoy apenas tiene `ts` repetidas (19), así que en Shadow afecta poco. A
  caballo (294 repetidas ayer) se notaría más.
- Tras una pausa, las rachas de 14-23 `+1` meten 0,2-0,5 s de movimiento en ~20 ms.

**R3. `ClockMapper` alimentado con el reloj de fotograma** (`coop.cpp:345`).
- Toma el **mínimo** de 4 s de `reloj_local − ts`, así que se queda con la lectura más vieja: el
  desfase baja tanto como el fotograma más largo de la ventana (~45 ms de mediana).
- El estimador viejo (`ts del último Req + pared desde note_clock`) tenía un sesgo del mismo tipo:
  el retraso del hilo escritor del sniffer, que vacía cada 10 ms y escribe a disco con búferes de
  4 MB. Cuando el jugador local estaba quieto, además se saltaba los saltos de STI.
- En el anfitrión, el balance neto fue **+41 ms de margen** (§6.3).
- En Shadow no se puede medir: la captura no guarda la hora de llegada de cada evento, y la réplica
  da entre −69 y +30 ms según cómo se estime (§6.6). Queda pendiente del log de Shadow.

**R4 (menor).** El retardo de lectura del reloj en el hilo de red también mueve la cota de
re-sellado (`now − 500`, `now + 300`) hasta un fotograma. En juego normal no actúa.

### 6.6 Réplica del receptor de Shadow con la captura de ayer (`sim.py`)

Entradas:
- los Move del anfitrión de ayer (`ts` originales);
- los Ack que se inyectaron en Shadow, emparejados por posición exacta (32 658 de 33 016);
- las llamadas reales a `OnRecvPacket` de Shadow;
- el reloj del cliente de Shadow, reconstruido con QPC y el desfase de cada intervalo de STI.

Cada variante se pasa por la réplica del búfer (60 fps, retardo 200).

| Variante | Sin muestras | Paradas ≥ 2 fot./min | ≥ 6 fot./min | ≥ 12 fot./min |
|---|---|---|---|---|
| Vieja, tal como se capturó | 4,29 % | 16,6 | 5,1 | 2,4 |
| Vieja, entrega re-simulada (control) | 4,32 % | 16,8 | 5,1 | 2,4 |
| Vieja + detector de pausa + re-sellado (R1 + R2) | **4,62 %** | **19,5** | **5,8** | 2,5 |
| Nueva completa (R1 + R2 + R3), según la hora de llegada supuesta | 4,0-24 % | 13-125 | 6,3-29 | 3,6-11 |

- R1 + R2 cuestan unas 3 paradas cortas más por minuto: **es medible, pero no es "mucho peor"**.
- La fila de R3 no sirve para decidir. El mínimo del `ClockMapper` es muy sensible al error en la
  hora de llegada estimada (±½ hueco entre S>C, hasta 50 ms).

### 6.7 Arreglos propuestos, por prioridad

1. **P1 de §4: separar la retención del desplazamiento de `ts`** (`hold ≈ 100`, `D = 300`).
   - Sigue siendo la causa de fondo: con ~100 ms de margen, cualquier perturbación de 100-300 ms
     se ve como parada y salto. Eso incluye una retención de R1, un salto de STI, el sesgo de R3 o
     un tirón de Steam.
   - Con P1 el margen pasa a ~400 ms y absorbe todo lo de esta sección.
2. **`PauseDetector` robusto (arregla R1).**
   - Pedir **2-3 ventanas seguidas** con `dgame < dwall/4`, o una ventana de ≥ 500 ms.
   - Tratar un retroceso del reloj (`game < game_at`) como **salto de reloj**: reanclar sin pausar.
   - Mejor aún, detectar la pausa por "el juego ha dejado de actualizar": el sello `[0x6C8CD78]`,
     que `0xAD1300` escribe en cada actualización, no avanza en 300 ms mientras siguen llegando S>C.
     No le afectan los saltos de STI. [inferido: hay que confirmar en vivo que se congela en los
     menús]
   - Medido con `pd.py`: ventana de 500 ms, o 3 ventanas seguidas, reduce las retenciones cortas
     entre 4 y 8 veces. Un menú real se detecta 0,3-0,5 s más tarde, sin consecuencias, porque la
     cota de re-sellado ya protege.
   - Tests en `tests/tests.cpp`:
     - un fotograma de 150 ms → no pausa;
     - un salto de −150 ms → no pausa;
     - menú de 2 s → pausa;
     - salir del menú → libera en ≤ 1 ventana.
3. **Reloj sin retraso de fotograma, para el `ClockMapper` y el re-sellado (arregla R3 y R4).**
   - `ahora = [cd68] + [cd70]/1000 + [0x6D69438] + clamp(real_ms − [0x6C8CD78], 0, 50)`, con
     `real_ms` calculado como `0x1416AF0`. [inferido: confirmar que `0x1414168A0` es QPC en ns]
   - Alternativa sin RE: en el `ClockMapper`, usar un percentil bajo (p3-p5) de la ventana de 4 s en
     vez del mínimo. Así un solo evento leído al final de un fotograma largo no arrastra el desfase.
4. **Re-sellado: descartar en vez de `+1` cuando el retroceso es pequeño (arregla R2).**
   - Para `MoveActorAck` con `0 ≤ última − ts < 100 ms`, no inyectar la muestra; es lo que hacía el
     cliente.
   - Mantener el `+1` para `StartActionAck`, porque la acción tiene que reproducirse.
   - Mantener la cota y el `+1` para los retrocesos grandes (cambio de reloj o pausa), donde sí
     hacen falta.
5. **Emisor (menor).** En `capture_synth`, usar `ts = max(reloj, g_last_move_ts + 1)`, para que un
   sintetizado nunca quede por detrás del último Move real.

### 6.8 Qué mirar cuando lleguen los registros de Shadow

- `cdmp.log` de Shadow:
  - las líneas `Puppet: this game is paused` y `running again`: **contar las retenciones de menos
    de 1 s** fuera de menús. Más de ~1 por minuto confirma R1 como causa principal;
  - el ritmo del contador `movement timestamps kept within the client's clock`;
  - si aparece `Capture: using the game's own clock` o `disagrees`;
  - el `clock offset` / `target` cada 5 s.
- Captura de Shadow (hora del anfitrión = hora de Shadow + 1 h):
  - `lead.py` + `lead2.py` (adelanto y paradas de la copia del anfitrión; ayer: 4,7 % y 4,7/min);
  - `fclock.py` + `pd.py` y `sti.py` + `steps.py` (fotogramas largos y saltos de STI hoy, frente a
    los de ayer);
  - `bumps.py` (rachas de `+1`).
- Si R1 explica poco y los saltos de STI o los fotogramas largos de Shadow son muchos más que ayer,
  la causa es Shadow, y la solución sigue siendo P1.
