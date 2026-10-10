# Captura 1 del sniffer (2026-10-03, 19 min de juego)

Fichero: `captures/cdmp_packets_20261003-105149.bin`, local y no versionado (112 MB).
Se analiza con `python tools/scripts/analyze_capture.py <bin> summary|phases|show|floats`.

## Resumen
- 356.315 paquetes en 1.164 s: 193.132 C>S (113 tipos) y 163.183 S>C (177 tipos).
- **61.683 descartados**, todos en 3 ráfagas de carga de zona: tras cargar la partida
  (~150 s), tras el viaje rápido (~490 s) y al cerrar. Durante el juego normal no se
  perdió nada.
- Solo se copiaba 1 KB por paquete, así que los grandes llegaron truncados (por ejemplo
  `ActorLoadDataAck`, de 6–19 KB).
- El juego y el overlay funcionaron 20 min sin errores.

## Hallazgos
1. **El cliente es la autoridad del movimiento.**
   - `MoveActorReq` (2556): 44.067 paquetes, unos 38/s, de **3.393 actores distintos**.
     El cliente simula el movimiento y la IA de los actores cercanos y lo comunica al
     servidor. También envía `ChangeAIStateReq`, `UpdateAIDestinationReq` y
     `StartActionReq`.
   - El servidor **no envía nunca** `MoveActorAck`. El handler cliente existe
     (`0xB621F0`), heredado del MMO.
2. **Formato de `MoveActorReq`** (payload de 514 bytes; slot siempre 0):

   | Offset | Contenido |
   |---|---|
   | `+0x00` | u32 clave del actor, la misma que `actor+0x60`. `0xA0100001` = jugador (18.839 paquetes, ~16/s); `0xB01000xx` = NPC y criaturas |
   | `+0x04` | vec3 posición en **coordenadas de mundo** (X, altura, Z) |
   | `+0x10` | cuaternión de rotación (por ejemplo `0, -0.94, 0, 0.33`) |
   | `+0x3C` | 4 floats a 1.0 (¿escala?) |
   | `+0x4C` | segundo vec3 y cuaternión, que a veces coincide con el primero (¿destino o anterior?) |

   La posición de carga `(0, 1000, 0)` también aparece.
3. **`ActorLoadDataAck` (2005) carga actores en el cliente**: 3.995 paquetes de 6–19 KB.
   - El primero es **el propio jugador**: contiene `"Kliff"` en UTF-16, la clave
     `0xA0100001` y la clave de usuario `0x90100000`.
   - Es la plantilla candidata para crear un segundo personaje.
4. **`RemoveActorAck` (2516)**: 10 bytes, por ejemplo
   `04 00 00 00 00 19 24 01 10 b0`. Al final va la clave `0xB0100124`.
5. **Acciones identificables por mensaje:**
   - viaje rápido: `SetMoveFieldAck`, `MoveFieldAck`, `MoveFieldCompleteReq/Ack`;
   - llamar al caballo: `CallVehicleMercenaryAck`;
   - combate: `PostProcessAttackResult_*`, `VaryHPAck`, `ApplyBuffAck`/`UnapplyBuffAck`;
   - botín: `LootDropCharacterRewardListAck`, `RemoveLootableAck`;
   - diálogos: `SetQuestDialogReq/Ack`.
6. `UpdateVisiblePlayerAck` no aparece en un jugador, como era de esperar.
7. Mensajes de mantenimiento de escena: `SyncSceneObjectAck` (43k),
   `UnloadSyncSceneObjectAck` (35k), `UpdateElementalAck` (30k) y
   `UpdateStageStateAck` (13k).

## Consecuencias para el cooperativo
- Para mostrar a otro jugador hacen falta dos cosas en el cliente:
  1. **cargarlo**: un `ActorLoadDataAck` con otra clave, basado en el del jugador;
  2. **moverlo**: un `MoveActorAck` (camino MMO) o escribir su transform directamente.
- El movimiento local del invitado ya existe en bytes (`MoveActorReq` de `0xA0100001`)
  y se puede reenviar tal cual al host.

## Mejoras del sniffer para la captura 2
- Búfer en anillo de bytes de tamaño variable, con paquetes completos de hasta 32 KB.
- Escritor más rápido: hex por tabla y lotes grandes, sin perder paquetes en las cargas.
