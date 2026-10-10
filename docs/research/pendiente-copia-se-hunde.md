# La copia del otro jugador se hunde en el terreno (2026-10-04)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`, SHA256 `57DA440D…E1E7`). Las direcciones
son RVA (`exe+…`). Investigación de solo lectura: capturas del sniffer, logs del mod y análisis
estático. No se ejecutó el juego ni `cdmp_peer`.

Etiquetas: **[verificado]** = leído en el código del juego o del mod; **[captura]** = medido en
capturas o logs; **[inferido]** = deducción.

## Resumen
- **Es un artefacto de la prueba en un solo PC.** La causa es el desplazamiento lateral de la copia
  (`[coop] offset_m`). No es un fallo del método de mensajes.
  - La copia recibe la posición **del emisor** con X desplazada (`X + offset`), pero **con la Y del
    emisor**. Si el terreno en `X + offset` está más alto, la copia queda enterrada; si está más
    bajo, flota [verificado, `remote_puppet.cpp` `move_ack`/`shift_x`].
  - Un actor en modo 2 **no se ajusta al suelo**: su Y es exactamente la del `MoveActorAck`. Medido:
    en 48 muestras la Y real de la copia (TransformSync) coincidió con la del Ack a ±1 cm, también con
    la copia 0,3 m por debajo del suelo [captura].
- **Por qué antes no pasaba:** el espejo se refinó con `mirror_offset_m=0` (modo estela), y los
  primeros días del cooperativo también usaban 0. El desplazamiento llegó después: 2,5 m (eco, commit
  `8506b54`), luego 12, 4 y ahora 10 m [captura, logs]. El error vertical crece con el desplazamiento:

  | Desplazamiento | Mediana \|ΔY\| | p90 \|ΔY\| | Enterrada > 0,3 m | Flotando > 0,3 m |
  |---|---|---|---|---|
  | 0 m | 0,00 | 0,05 | 0 % | 1 % |
  | 2,5 m | 0,07 | 0,24 | 5 % | 3 % |
  | 4 m | 0,35 | 1,02 | 19 % | 37 % |
  | 12 m | 0,52 | 1,69 | 23 % | 40 % |

- **Con dos PCs no debería pasar.** El kit usa `offset_m=0`, y el desplazamiento de eco (2,5 m) solo se
  activa si el otro es `cdmp_peer`. La Y que llega es la del otro jugador, apoyado en **el mismo
  terreno** por su propia física.
- **Recomendación:**
  - para pruebas de aspecto o movimiento en un PC, usar el **modo estela** (sin código): copia sobre
    tu propio camino, con retardo;
  - mantener desplazamientos grandes solo en las pruebas de golpes (pieza 4), aceptando el hundimiento;
  - **no** corregir la Y con llamadas al motor: el riesgo de cierre no compensa para un artefacto de
    pruebas.

---

## 1. Qué posiciones recibe la copia
- **Jugador remoto** (`0xA010F001`): `MoveActorAck` = `MoveActorReq` del emisor (posición de mundo
  `+0x04`), con X desplazada en `+0x04` y `+0x4C`; **Y sin tocar** [verificado, `move_ack`].
  - `StartActionAck` (`+0x8F`, `+0xD7`) y `UpdateWallUpDataAck` (puntos) se desplazan igual: solo en X.
  - La carga (`ActorLoadDataAck`) también: `info.pos.x + g_offset_x` [verificado, `puppet_spawn`].
- **Copias de enemigos** (`0xB01F0000+n`): posición leída de la memoria del anfitrión (TransformSync)
  a ~15 Hz. `puppet_enemy_move` la convierte en un `MoveActorAck` que pasa por el mismo `shift_x`.
  Por tanto **también se desplazan solo en X** [verificado, `enemy_relay.cpp` y `remote_puppet.cpp`].
- El desplazamiento es en **X de mundo**, no lateral al jugador. Si caminas en paralelo al eje X, la
  copia va por delante o por detrás de ti en tu propia línea. Si caminas en perpendicular, va por una
  ladera paralela [inferido].

## 2. Qué hace el cliente con un actor en modo 2
- **[verificado]** (`movimiento-remoto-cliente.md`):
  - `0xAB0540` escribe la posición desde el búfer de red, por interpolación lineal (`0x11B2420`);
  - no hay física propia, ni root motion, ni extrapolación;
  - la petición sale con tipo `0xC` (`SetPosition` `0x9FCA60`).
- **[verificado]** `0x9FF500`, que consume la petición en TransformSync:
  - los tipos de actor 1, 3–6, 8 y 9 (personajes) leen la posición de su física con `0x369E220`
    sobre `[CC+0x140]` y la copian al transform si difiere (tablas de salto `0xA00E68`, `0xA00E84`,
    `0xA00EA0` y `0xA00EBC`);
  - el tipo 7 va por otra rama;
  - el jugador remoto y los enemigos pasan, pues, por la **misma** rama.
- **[captura] La copia del jugador sigue la Y de la red al centímetro.** En la sesión
  `cdmp_packets_20261003-214215` (desplazamiento 12 m, `minimap_icon=0`), el pin del mapa lleva la
  posición **dibujada** de la copia (`puppet_position()` = TS) una vez por segundo.
  - Resultado: `TS.y − Ack.y = 0,00 ± 0,01 m` en 48 muestras, caminando entre Y 558,7 y 561,1.
  - En las 3 muestras con estimación de suelo: −0,29, −0,30 y −0,03 m, es decir, **enterrada** y sin
    corrección (`pins.py`).
- **Conclusión:** la copia del jugador no tiene ajuste al suelo: es una marioneta posicional, igual
  que un jugador remoto del MMO. Choca con otros personajes porque tiene cuerpo físico, pero ese
  cuerpo se coloca donde manda la red.

## 3. Cómo se midió el error frente al terreno
- **Nube de suelo:** todas las posiciones de `MoveActorReq` del propio jugador sin padre (no montado),
  sacadas de la misma captura.
- **Suelo en un punto `(x, z)`:** la mediana de las Y de la nube en un radio de 0,6 m. Se descartan
  los puntos con varios niveles (p90 − p10 > 1,5 m) [captura, `ground.py` y `agg.py`].
- **Control:**
  - la Y de la copia comparada en la posición **sin desplazar** da ΔY ≈ 0 en todas las sesiones
    (p5–p95 dentro de ±0,07 m);
  - con desplazamiento 0, la copia da mediana 0,00 y p90 0,05 m en 53.173 muestras.
- **Límites:**
  - solo se mide donde el jugador pisó en algún momento (del 10 al 40 % de las muestras);
  - los saltos y escaladas del emisor inflan un poco la cola "flotando".

**Cronología del desplazamiento** [captura, logs `Puppet offset set to …` y `ground.py`]:

| Sesiones | Desplazamiento | Origen |
|---|---|---|
| 03-10, 12:21–12:45 | 2,5 m | `dev mirror_offset_m` por defecto (primer espejo) |
| 03-10, 12:59–20:03 | **0 m** | espejo refinado y primer cooperativo ("se ve perfecto") |
| 03-10, 20:59 | 2,5 m | `8506b54`: con eco y `offset_m=0` se usa `kEchoOffset = 2.5` (empujaba al jugador) |
| 03-10, 21:42–23:07 | 12 m | `cdmp.ini.prueba-mapa` y `prueba-caballo` |
| 04-10, 06:43–11:46 | 4 m | pruebas de enemigos y golpes |
| hoy (`cdmp.ini`) | **10 m** | `prueba-pieza4b` |

## 4. Copias de enemigos
- Llevan el mismo desplazamiento en X con la Y del anfitrión, así que **también deberían hundirse o
  flotar** [verificado].
- **Pero** se midió algo distinto [captura, `copy_vs_ack.py` y `spawn_window.py`].
  - El log `Enemy relay: copy … exists: at (…)` da su posición TS 2 s después de cargarla.
  - En 39 casos con suelo conocido: mediana `|copia − suelo|` = **0,06 m**, mientras que
    `|Ack − suelo|` = 0,31 m.
  - Ejemplo: `0xB01F0006` (sesión 095647), quieta, con 3 Acks a Y 600,99 y la copia a **601,9**: sobre
    el suelo (601,92).
  - En otros casos sí sigue al Ack (por ejemplo, Ack +1,15 sobre el suelo y la copia +1,14).
- **[inferido]** Algo ajusta al suelo las copias de NPC al menos en parte: al aparecer, y en algunos
  casos también moviéndose. A la copia del jugador no se lo hace.
  - El mecanismo no está localizado. Candidatos:
    1. colocación en el suelo al crear el actor (`SPAWN_ON_GROUND`), combinada con que las muestras de
       red aún no se aplican (S1/`T ≤ ts[0]`);
    2. un cuerpo físico distinto para el tipo 3 (`hknpCharacterRigidBody` con gravedad frente a un
       proxy movido por posición);
    3. métodos de movimiento de IA.
  - Queda como pregunta abierta. No cambia la conclusión: con dos PCs los enemigos llegan sin
    desplazamiento.

## 5. Un PC frente a dos PCs

| | Un PC (eco con `cdmp_peer`) | Dos PCs |
|---|---|---|
| Posición recibida | La tuya, desplazada en X (2,5 / 4 / 10 / 12 m) | La del otro jugador, **sin desplazar** (`offset_m=0` en el kit) |
| Terreno bajo la copia | **Otro** punto del terreno | El **mismo** punto, el mismo mapa (terreno físico idéntico) |
| Y | Errónea en ±(pendiente × desplazamiento) | Correcta: la calculó la física del otro PC |
| Hundimiento esperado | Sí, en laderas (tabla del resumen) | No en terreno estático |

**Restos posibles con dos PCs** [inferido]:
- **Plataformas móviles** (ascensores, barcos, gimmicks):
  - `clear_parent` borra el padre salvo el caballo, así que llegan como posiciones de mundo;
  - si el objeto está en otro estado en tu mundo, la copia flota o se mete en él.
- **Estado del mundo distinto** entre PCs: objetos destruidos, puertas, estructuras de misión.
- **Relleno de huecos en subidas** (`fill_gap_before`): recorre "primero vertical y luego
  horizontal" en línea recta y puede atravesar el borde del saliente durante menos de 1 s.
- La interpolación lineal entre muestras cada 25 ms recorta las crestas del terreno: solo
  centímetros.

## 6. Cómo lo hacía el MMO
- **El servidor no calcula alturas:** valida (`TS::CanMove` `0x21A1580`, movimiento en el
  field/sector `0x2AF8090`) y reenvía la posición del cliente que controla
  (`ToNearExceptOneControl`) [verificado, `broadcast-servidor-movimiento.md`].
- **Los Ack no traen nada de suelo.** `MoveActorAck` no tiene ningún campo de suelo ni de navmesh; en
  modo 2 el cliente solo usa la posición, el quaternion y la marca de tiempo (`+0xD4` y `+0xDD` no se
  usan) [verificado].
- **Por tanto, un jugador remoto se ve sobre el suelo solo porque su propio cliente lo apoyó en el
  terreno con su física, y todos comparten mapa.** El receptor no lanza rayos para un jugador remoto
  (medido: TS = Ack) [captura e inferido].
- **NPC:**
  - en esta versión de un jugador los simula el cliente (modo 3) y los informa con `MoveActorReq`
    (C>S), unos 15.000–32.000 por sesión con clave `0xB01…` [captura];
  - la corrección de copias de enemigos del §4 apunta a que el cliente sí tiene algún ajuste para
    ellos.

---

## 7. Enfoques posibles

| # | Enfoque | Qué hace | Esfuerzo | Riesgo | Valor |
|---|---|---|---|---|---|
| A | **Aceptarlo y elegir el desplazamiento según la prueba** | 0 o estela para ver movimiento; 2,5 m como compromiso (p90 0,24 m); ≥ 4 m solo para golpes | Nada | Nulo | Alto |
| B | **Modo estela en eco** | Sin código: `offset_m=0.01` (≠ 0 evita el 2,5 forzado), con `delay_ms` de 1500–3000. La copia recorre tu propio camino con retardo: Y exacta, escaladas en la pared real. Con código: en eco, sustituir `kEchoOffset` por esto y "aparcar" la copia si su objetivo cae a menos de ~1,5 m de ti | 0 / bajo | Bajo: solo hay que decidir qué muestras se inyectan | Alto para un PC |
| C | **Corregir la Y en el receptor con un rayo hacia abajo** | ΔY = suelo(x+off, z) − suelo(x, z) sumado a Move, StartAction y WallUp, solo en eco | Alto | **Medio-alto** | Bajo |
| D | **Cambiar el modo de control** (bit 1 de `[[CC+0x60]+0x10]` → modo 3) | Simulación local con física | Medio | Alto | No sirve: en modo 3 el búfer de red no se consume y la copia dejaría de seguir al emisor (`movimiento-remoto-cliente.md` §5) |
| E | **Mapa de alturas con tu propio recorrido** | Lo que hace `ground.py`, en vivo | Medio | Bajo | Bajo: solo cubre del 10 al 40 % de las posiciones de la copia y daría saltos al entrar y salir de la cobertura |

Detalle del enfoque C:
- Existe un método **probado en vivo en esta misma build** (CrimsonDesertTelemetry, MIT,
  `docs/PHYSICS_QUERY_RESEARCH.md`):
  - captura una consulta natural de `TtWorldCastRay` (`0x42B0B50`) o `TtWorldCastShape`
    (`0x42B0C50`) cerca del jugador;
  - reenvía una copia con otro segmento, en el mismo hilo y dentro de la llamada original;
  - su control de suelo dio Y = pies del jugador a 2e-6.
- Riesgos:
  - es una llamada a motor reconstruida a partir de estructuras de pila;
  - se limita a 20 m del jugador y 50 m de longitud;
  - el autor la limita a 12 consultas por proceso por seguridad;
  - un cierre se reporta a Pearl Abyss.
- Solo serviría para el eco: con dos PCs no hace falta.

## 8. Plan recomendado
1. **Documentar el artefacto.** En `PRUEBAS-COOP.md` y en los comentarios de los `.ini` de prueba:
   "con `offset_m` ≠ 0 la copia se hunde o flota en laderas; con dos PCs no".
2. **Prueba P1 (estela, sin código)** para confirmar que con desplazamiento casi 0 no hay hundimiento.
3. **Si se quiere un eco cómodo, implementar B con código** (bajo riesgo, sin tocar memoria del juego):
   - en eco, desplazar a la copia **en el tiempo, no en el espacio**;
   - no inyectar muestras cuyo destino quede a menos de ~1,5 m de la posición actual del jugador
     local, para que no te empuje al pararte;
   - reanudar cuando te alejes.
4. **Mantener `offset_m` ≥ 4 solo** en las pruebas de pieza 4 o de golpes (hace falta separar la copia
   del original) y avisar en el `.ini` de que la copia puede hundirse.
5. **Aplazar C:** solo si una prueba de un PC necesitara de verdad la Y correcta con desplazamiento.
6. **En la primera prueba con dos PCs:** comprobar que no hay hundimiento en terreno estático (P4) y
   anotar los casos de plataformas móviles.

## 9. Pruebas en vivo (un PC, `cdmp_peer --mode relay`)
- Mantener el sniffer grabando, como hoy.
- Copia de las partidas en `backups/` antes de cada prueba, como en las anteriores: el mod inyecta
  mensajes al cliente.
- `minimap_icon=0` y `map_pin=1` hacen que el pin lleve la posición **dibujada** de la copia una vez
  por segundo. Así se mide sin código con `pins.py`.

| # | Configuración | Recorrido | Predicción |
|---|---|---|---|
| P1 | `offset_m=0.01`, `delay_ms=1500`, `enemy_relay=0`, `guest_hits=0`, `minimap_icon=0` | Ladera y colina, ida y vuelta; subir un saliente | Sin hundimiento visible. `ground.py`: \|ΔY\| p90 ≤ 0,1 m. `pins.py`: TS − Ack = 0,00 ± 0,01. Las escaladas encajan con la pared. Al pararte, la copia llega hasta ti y te empuja |
| P2 | Igual, con `offset_m=10` | El mismo recorrido | La copia se hunde donde el terreno en X+10 está más alto y flota donde está más bajo: \|ΔY\| mediana ≈ 0,4–0,5 m, p90 ≈ 1,2–1,6 m. `pins.py`: TS − Ack sigue en 0,00 (no se ajusta al suelo). Al rodear una colina se alternan hundida y flotando |
| P3 | `offset_m=4`, `enemy_relay=1`, Ctrl+F7 (copias automáticas) en un campamento en pendiente | Quieto; luego pelear | Algunas copias de enemigos sobre el suelo aunque su Ack no lo esté (§4) y otras siguiendo al Ack. `copy_vs_ack2.py` da una mezcla "dentro y fuera" parecida a 92/76 |
| P4 | Dos PCs reales (Steam), `offset_m=0` | Andar juntos por laderas | Sin hundimiento en terreno estático. Posibles errores solo en plataformas móviles o en objetos con otro estado |

Lectura de resultados:
```
build\research\venv\Scripts\python.exe build\research\p_ground\agg.py
build\research\venv\Scripts\python.exe build\research\p_ground\pins.py
```

## 10. Preguntas abiertas
- **Qué ajusta al suelo las copias de enemigos** (§4) y por qué no a la del jugador.
  - Prueba: leer en vivo, a 30 Hz, `TS+0xB4`, la física `[[CC+0x140]+0x2C0]+0x90` y el búfer
    `mc+0x3D0` de una copia de enemigo y de la del jugador.
  - Si es un modo del cuerpo físico, ¿se podría activar en la copia del jugador? Haría falta escribir
    en memoria del juego: copia de seguridad antes.
- ¿Sirve la consulta de navegación por actor (`comps+0x1A8`, `0x21B8C60`, la que usa WallUp) como
  consulta de altura sin reconstruir consultas de Havok?
- ¿Qué hace la copia sobre plataformas móviles con dos PCs? (`clear_parent` quita el padre).

## Apéndice A: scripts (`build/research/p_ground/`)

| Fichero | Qué hace |
|---|---|
| `survey.py` | Cuenta los `MoveActorReq` y `MoveActorAck` por clase de clave en cada captura |
| `ground.py` | Y de la copia (Ack) frente al suelo en su XZ, por sesión, con el control sin desplazar. `OWN_ONLY=1` usa solo tu recorrido. Salidas: `ground_all.txt`, `ground_own.txt` |
| `agg.py` | Tabla del error vertical por desplazamiento (`agg.txt`) |
| `pins.py` | Posición dibujada de la copia filtrada en el pin del mapa (2824, `+14`) y en el `ApplyBuffAck` del tambaleo (2506, `+0x29`), frente a Ack y suelo (`pins.txt`) |
| `copy_vs_ack.py`, `copy_vs_ack2.py`, `spawn_window.py` | Posición TS de las copias de enemigos (log `exists: at`) frente a sus Ack y el suelo |

Todos usan `build/research/pieza4/pk.py`.

## Apéndice B: direcciones (build 2976)

| RVA | Qué es | Fuente |
|---|---|---|
| `0xAB0540` | `vf2` de los métodos de movimiento en modo 2: posición desde la red | `movimiento-remoto-cliente.md` |
| `0x11B1FD0` / `0x11B2420` | Insertar y leer en el búfer de interpolación de posición | ídem |
| `0x88E8E0` | Modo de control (2 = remoto si `actor+0x90 != 0`) | ídem |
| `0x9FCA60` | `TransformSync::SetPosition(tipo)` | ídem |
| `0x9FF500` | Consumo de la petición en TS; rama de personaje con `0x369E220([CC+0x140])` = lectura de la posición física | este informe |
| `0xA00E68`, `0xA00E84`, `0xA00EA0`, `0xA00EBC` | Tablas de salto por tipo de actor (3..9) en `0x9FF500`: el tipo 7 va aparte | este informe |
| `0x42B0B50` / `0x42B0C50` | `TtWorldCastRay` / `TtWorldCastShape` (consultas de Havok) | CrimsonDesertTelemetry (MIT), mismo SHA256 del exe |
| `0x428D080` | `TtCastRay` | ídem |
| `0x42820E0` | Entrada del tick de movimiento (contexto de World Builder) | ídem |
| `0x21B8C60` | Consulta de navegación por actor (`comps+0x1A8`) | `broadcast-servidor-movimiento.md` |
| `0x21A1580` / `0x2AF8090` | `TS::CanMove` y movimiento en el field del servidor (no calculan altura) | ídem |
