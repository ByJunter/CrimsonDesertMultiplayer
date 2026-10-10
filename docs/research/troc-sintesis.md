# Síntesis: los mensajes internos cliente/servidor (Troc)

Build 1.0.0.2976 (parche 2.03.02). Resume tres investigaciones paralelas del
2026-10-03, con verificación en vivo:
- [troc-rtti.md](troc-rtti.md): taxonomía por RTTI.
- [troc-dispatch.md](troc-dispatch.md): flujo y puntos de intercepción.
- [troc-bdo-y-runtime.md](troc-bdo-y-runtime.md): antecedentes de BDO, restos del MMO
  y mapa en vivo.

Tabla completa de mensajes: [data/troc_messages_2976.csv](data/troc_messages_2976.csv).

## Lo esencial

1. **Es la arquitectura de un MMO, intacta.** Crimson Desert nació como MMORPG
   (precuela de BDO), y el exe enlaza los módulos cliente y servidor de BDO en un
   solo proceso. Hay hilos propios para cada lado (`ServerRunThread`,
   `ClientRunThread`…) y quedan restos completos del MMO:
   - login cifrado, servidor de autenticación y "field server";
   - 116 procedimientos SQL;
   - `NwSyncSession` reales y AES-GCM;
   - imports de Winsock connect/listen/accept.
2. **Los mensajes viajan como bytes, no como objetos.** `pa::TrocTr*` no son paquetes:
   son **1.115 handlers estáticos** (0x90 bytes en `.rdata`), registrados por opcode
   en una tabla hash (`exe+0x6A65BC8`).
   - Tipos: 486 Req (cliente→servidor), 525 Ack y 19 Nak (servidor→cliente), y 85
     temporizadores del servidor.
   - Paquete: `u16 opcode | u8 slot | u16 len | payload`. El marco suma 0x1F bytes
     (cabecera más AES-GCM). **El cifrado está apagado en el modo un jugador.**
3. **El transporte es un loopback en memoria.** `NwVirtualSyncSession` (lado cliente)
   y `NwVirtualAsyncSession` (lado servidor) comparten colas; no hay sockets.
   - Hay **una sesión de servidor por usuario**: `session+0x90` → `ServerUserActor`.
   - El servidor difunde a una lista de sesiones mediante `To*SendTargetFinder`
     (ToNear, ToWorld, ToSessionList…).
4. **El servidor es la autoridad** y simula solo la zona cercana al jugador: 125 de
   128 actores coinciden. Tiene un `ServerUserActor` y 96 huecos de personaje libres.
   - Los actores comparten una clave u32 en `actor+0x60` en ambos lados.
   - Los componentes están reflejados: el servidor tiene su propio HP, inventario y
     posición.
5. **Hay mensajes para otros jugadores**, heredados del MMO: `UpdateVisiblePlayerAck`
   (2135), `OtherEquipItemAck` (2757), `MoveActorAck` (2697), `RemoveActorAck` (2516).
6. **Los opcodes cambian entre builds** (comprobado contra el mod Trinity), así que hay
   que resolverlos en tiempo de ejecución a partir de la tabla o del RTTI.

## Flujo (verificado: 13/13 firmas en vivo, todas en entrada de función y fuera de `.xtls`)

```
CLIENTE                                                       SERVIDOR
PostPacket 0x35C670 ─► NetworkThread ─► NwVirtualSyncSession::Send 0x14380B0
                                         └─► hilo servidor 0x27733F0 ─► OnRecvPacket 0x2773C20
                                             ─► Troc::Find(op,4) 0x143B2C0 ─► ExecuteTroc 0x2774B20 ─► vf2
SendPacket 0x2773870 ─► NwVirtualAsyncSession::Send 0x1437740
   ─► ClientNet::OnRecvPacket 0x35CA00 ─► vf1 (encola) ─► hilo principal 0x35D0C0 ─► 0x35D2A0 ─► vf2
```

## Por qué esto cambia el plan

El repo de referencia intentaba sincronizar memoria a mano. Aquí el propio juego ya
separa cliente y servidor con un protocolo de bytes:

- **Representar al otro jugador:** se puede inyectar en el cliente los mensajes que
  el MMO usaba para eso (`UpdateVisiblePlayerAck`, `MoveActorAck`,
  `OtherEquipItemAck`…) en lugar de secuestrar un NPC.

## Recomendación: próximos pasos, de menor a mayor riesgo

1. **Hito 4a, sniffer pasivo (solo registro).** Hooks inline en
   `ClientNet::PostPacket` (`0x35C670`) y `ClientNet::OnRecvPacket` (`0x35CA00`).
   - El detour copia opcode, tamaño y payload a un búfer sin locks y no modifica nada.
     El hilo del mod los escribe en un log con el nombre resuelto por RTTI.
   - Se juegan 5 minutos (andar, montar, pelear, recoger objetos) y se obtiene un
     catálogo de tráfico real con payloads.
2. **Hito 4b, decodificar los payloads clave**, empezando por `MoveActorReq/Ack`,
   `UpdateVisiblePlayerAck` y `RemoveActorAck`, comparando bytes con el estado leído
   por el probe.
3. **Hito 4c, inyección en el cliente (primera escritura):** fabricar un
   `UpdateVisiblePlayerAck` para que el cliente cree un **segundo personaje real**,
   alimentado por el ghost de `cdmp_peer`.

## Preguntas abiertas
- Esquema de los payloads (lo resuelve el sniffer).
- Qué significa el byte `slot` (¿índice del actor controlado: personaje, montura…?).
- Dónde y cómo se crean las sesiones de servidor. El handler de desconexión está
  mutado por Denuvo.
- Un NPC tenía la X desplazada exactamente 1000 entre servidor y cliente. Puede ser
  otro origen de trozo de mapa.
