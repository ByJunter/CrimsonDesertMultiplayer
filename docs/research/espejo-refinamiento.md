# Refinado del modo espejo (2026-10-03)

Investigación con dos agentes:
- [movimiento-remoto-cliente.md](movimiento-remoto-cliente.md): cómo mueve el cliente a un actor remoto.
- [broadcast-servidor-movimiento.md](broadcast-servidor-movimiento.md): qué enviaba el servidor del MMO.

Se completa con mediciones en vivo (probe a 30 ms) y con las capturas del sniffer.

## Modelo del cliente para un actor remoto (modo 2, el clon)
- **Solo interpola** las posiciones recibidas: un búfer con 200 ms de retraso de
  render, 30 muestras como máximo y sin extrapolación. **No aplica root motion ni
  física.** Los usan todos los métodos de movimiento: andar, escalar, WallUp, escaleras…
- Las muestras se **descartan** si su marca de tiempo no es más nueva que la anterior,
  o si se movieron 0,1 mm o menos.
- Si pasan **más de 1000 ms** entre muestras aceptadas, el búfer **se reinicia**: el
  siguiente salto se recorre en unos 200 ms y se ve como un "pop".
- Campos que usa: posición, cuaternión y la marca de tiempo (Move `+0xD5`,
  StartAction Ack `+0x160`; ms del reloj del propio cliente). `RotateActorAck` **no
  mueve** al actor.

## Medido en vivo
- **Escalar una pared** sí envía `MoveActorReq` a ~40 Hz, y la física se mueve: funciona.
- **Subir a un saliente o saltar un obstáculo** (vault): durante 0,3–1,5 s no se envía
  `MoveActorReq`, y **la propia física del jugador se congela**. Al terminar salta
  1–3 m de una vez. Es la animación la que mueve solo el modelo visual.
- **Planear** funciona bien en el espejo, según el usuario.

## Lo que hacía el servidor del MMO
- Nunca calculaba posiciones: solo validaba y reenviaba las del cliente que controla el
  actor (`ToNearExceptOneControl`, ~40 Hz).
- Para subir a salientes enviaba además `UpdateWallUpDataAck` (2893, 132 bytes) con la
  ruta del saliente. El cliente solo envía el Req si el global `exe+0x6D76D5C` != 0,
  que vale 0 en un jugador. Su radio de impacto está en estudio.

## Cambios aplicados
1. **Marcas de tiempo:** se suma el retardo en Move `+0xD5`, Rotate `+0x9C` y ahora
   también StartAction Ack `+0x160`.
2. **Relleno de huecos:** cuando un `MoveActorReq` llega tras un hueco de 150–3000 ms
   con un salto de 0,5–15 m, se insertan muestras cada 50 ms con sus marcas de tiempo
   correctas.
   - Recorrido: primero vertical y luego horizontal al subir; al revés al bajar.
   - Se insertan ordenadas en la cola retardada, así que es completamente suave si el
     retardo supera el hueco.
   - Validado sobre la captura 131648: se activa en las 2 subidas (+3,0 m y +2,8 m, 1,2 s)
     y en 5 saltos de obstáculo, y no en la marcha normal.
3. **Movimientos sintetizados desde memoria** cuando el Move se detiene pero la física
   sí se mueve (vehículos, etc.).

4. **Ruta del saliente (`UpdateWallUpDataAck`, 2893).** Prueba del usuario (video del 2026-10-03,
   19:54): en cada subida el clon **desaparecía** ~0,6 s y reaparecía arriba.
   - Medido en memoria (lectura externa a 100 Hz): el clon entra en el método 5 (WallUp) igual
     que el jugador y su transform (`TS+0xB4`) sube suave por la ruta rellenada. La posición
     **no** era el problema.
   - Su `WallUpPath` (`[CC+0xD8]+0x1170…`) valía **(0,0,0)**; el del jugador, el punto del borde.
   - Arreglo: mientras el jugador está en el método 5, el mod lee su `D` y lo inyecta como
     `UpdateWallUpDataAck` con la clave del clon (retardo del espejo − 40 ms, para llegar antes
     del `StartActionAck`). Pasa lo mismo con un jugador remoto real: en el cooperativo habrá
     que enviar estos datos por la red.

5. **Escalada libre por la pared (método 18, PointClimb).** El clon se quedaba colgado en el
   punto de agarre toda la escalada y saltaba ~10 m al final.
   - Medido en vivo: su búfer de red y `TS+0x534` subían, pero el transform, la física y la
     petición de posición (`TS+0x5A0` = 15) no se movían.
   - Causa ([pointclimb-remoto.md](pointclimb-remoto.md)): `PointClimb::Enter` (`0x691400`)
     falla en un actor remoto porque el sensor de puntos de agarre solo corre para el
     personaje local. El resultado se guarda en `método+0x44` y, si es 0, el frame salta la
     interpolación y el `SetPosition`.
   - Arreglo: hook inline en `0x691400`; si falla y el dueño del actor (`u32 actor+0x90`) es
     el usuario del clon, devuelve 1. Solo afecta al clon.
   - Probado (2026-10-03): el clon sube siguiendo la ruta del jugador. Se activó solo en el clon.
     Queda un leve deslizamiento: la animación de escalada no encaja del todo con el avance.

## Ajustables conocidos (no modificados)
| Dirección | Por defecto | Significado |
|---|---|---|
| `exe+0x6CB9548` | 200 | retraso de interpolación (ms) |
| `exe+0x6CB9598` | 30 | muestras máximas en el búfer |
| `exe+0x6CB9728` | 20,0 | distancia de teletransporte |
| `exe+0x6CB9688` | 8,0 | salto de velocidad que se trata como teletransporte |

## Pendiente
- Deslizamiento leve del clon en la escalada libre (animación sin puntos de agarre reales).
- Localizar el clon en memoria (receta clave → actor en `movimiento-remoto-cliente.md`)
  para medir el error de seguimiento.
- Cooperativo real: el emisor no conoce el final de una subida hasta que acaba. Habrá
  que enviar la ruta del saliente o aceptar ~1,5 s de retardo en esas animaciones.
