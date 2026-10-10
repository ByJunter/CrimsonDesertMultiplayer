# Captura 2 del sniffer v2 (2026-10-03, 14 min de juego)

Fichero: `captures/cdmp_packets_20261003-112627.bin` (152 MB, local). Plantillas
extraídas: `captures/actorload_kliff_{0..3}.bin`.

## Calidad
- 498.482 paquetes en 856 s, **0 descartados y 0 truncados**: los 3.666
  `ActorLoadDataAck` están completos.
- Se leyó en vivo con el juego abierto (acceso compartido).

## Acciones capturadas (nuevas respecto a la captura 1)
Compra y venta en tienda (`BuyStockFromStoreReq`, `SellItemToItemStoreReq`,
`StoreDataListAck`), equipar y desequipar (`EquipItemReq/Ack`, `UnequipItemReq/Ack`,
`MercenaryEquipmentListAck`), 682 intercambios de diálogo, 2 viajes rápidos, combate,
botín y llamar al caballo.

## Arranque: el cliente "se conecta" a su servidor local
El primer paquete C>S tiene el opcode 1001. No está en la tabla de Troc: es el
handshake de conexión. Su payload incluye la cadena ASCII `127.0.0.1`. El servidor
responde `ConnectToServerAck` (1002). Este es el punto de entrada del login del MMO.

## Carga del personaje: dos `ActorLoadDataAck` por cada carga

| Paquete | Tamaño | Claves que contiene | Diferencia entre dos cargas |
|---|---|---|---|
| **Actor jugador** | ~12,8 KB | `0xA0100001` (jugador) ×8 desde `+0x0E`; `0x90100000` (usuario) en `+0x03`, `+0x12`, `+0x1F` | **solo 150 bytes en 35 rangos** (+4 bytes de tamaño) |
| **Cuerpo** | ~18,9 KB | clave propia `0xB01000xx` en `+0x03` y muchas más; `0xA0100001` ×8 desde `+0x1828`; `0x90100000` ×4 | 4.549 bytes, porque **el servidor asigna una clave nueva al cuerpo en cada carga** (`0xB0100065`, luego `0xB010006C`) |

Cabecera observada en el payload (los campos están empaquetados, sin alineación):
`+0x00` 3 bytes (¿tipo y versión?), `+0x03` u32 clave (usuario o actor), y en el actor
jugador un contador u16 en `+0x0B` que cambia entre cargas (`0x31F6` → `0x31FA`).

## Caballo
`CallVehicleMercenaryAck` (100 bytes) contiene:
- `+0x00` clave del mercenario `0xB0100003`;
- después, la clave del dueño `0xA0100001`;
- floats de posición.

Unos 3,75 s más tarde llega el `ActorLoadDataAck` de un actor nuevo (`0xB0100123`).

## Para el hito 4c (clonar a Kliff)
- Plantilla: los dos paquetes de una misma carga (actor y cuerpo), cambiando:
  - `0xA0100001` → clave libre de jugador (por ejemplo `0xA0100002`);
  - `0x90100000` → usuario libre (por ejemplo `0x90100001`);
  - la clave del cuerpo → una `0xB01xxxxx` sin usar;
  - y la posición, para que aparezca cerca.
- Inyección propuesta: en el hilo de red del cliente, desde el propio hook de
  `ClientNet::OnRecvPacket`, llamando a la función original con nuestro paquete.
  Así se mantiene la afinidad de hilo; no se llama desde un hilo nuestro.
- Pendiente: el significado de los 150 bytes que cambian entre cargas, y los campos de
  posición y estado del cuerpo.
