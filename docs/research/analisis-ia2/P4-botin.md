# P4 — Botín del invitado: auditoría del servidor, rechazo y cadáveres

Fecha: 7 oct 2026. Código auditado: HEAD `57bec36bdad634a502eb587f8f899c090ca162c5`. Juego: **1.0.0.2976**, timestamp PE **0x6AB28F00**, base **0x140000000**; comprobados de nuevo en el ejecutable. Las direcciones de este informe son **RVA**, salvo cuando se dice offset de archivo. Investigación sin ejecutar el juego ni aplicar cambios al código.

## Resumen (8 puntos)

1. **Hecho:** 3052 sí tiene payload `u32 cadáver | u8 retirar | u8 mostrar resultado`, 6 bytes. `(0,0)` coincide con el temporizador natural de saqueo.
2. **Hecho:** el primer byte fuerza retirar el cadáver; el segundo añade **UseRandomBoxAck (2304)**. Ninguno selecciona mascota/jugador ni habilita por sí solo la entrega.
3. **Hecho nuevo:** los 28 rechazos de noche 1 identifican **condición 0 de InteractionInfo**, no la lista propia del CharacterInfo/action chart: lo dicen los campos ignorados del FailAck.
4. **No cerrado:** la hoja concreta de esa condición que falla. El error demuestra que la evaluación B sobre el gemelo bloquea; no demuestra que sea Fall, un atributo o la variante.
5. **Hecho nuevo noche 2:** se crearon siete gemelos, pero ningún 3052 ni FailAck 3000; por tanto, el rescate actual no fue ejercitado.
6. **Hecho nuevo noche 2:** el servidor del invitado retiró los siete gemelos juntos, motivo **0x19**, entre t=549,649577 y 549,649778; el mod borró sus siete derechos de botín. Conservar la copia cliente no conserva un gemelo que desapareció del servidor.
7. **Hecho de código:** la protección actual cubre Remove del anfitrión y zona OFF, pero no fin de gemelo, reset de compañero, expiración de 120 s, límite de 32 o reutilización de clave.
8. Recomendación: conservar `(0,0)`, registrar diagnóstico completo y ensayar un solo rescate correlacionado. No cambiar motivo de spawn ni regenerar gemelos automáticamente sin resolver la posibilidad de doble recompensa.

## 1. Fuentes y reproducibilidad

Fuentes primarias leídas: `README.md`, `docs/ROADMAP.md`; `src/mod/net/loot_twin.cpp`, `enemy_relay.cpp`, `coop.cpp`, `troc_sniffer.cpp`; ejecutable local; las dos capturas del invitado:

- G1: `cdmp_packets_20261007-191633.bin` (7 oct, sesión noche).
- G2: `cdmp_packets_20261007-203438.bin` (7 oct, sesión noche2).
- Logs correspondientes `cdmp.log` del invitado de esas sesiones.

El informe anterior `docs/research/botin-invitado.md` orientó direcciones, pero los formatos, las ramas decisivas y los recuentos aquí se contrastaron de nuevo. Los scripts nuevos `build/research/ia2/p4_dis.py` y `p4_packets.py` no escriben cachés ni tocan el juego. Ejecutar con `build/research/venv/Scripts/python.exe -B`.

Ejemplos reproducibles:

```text
p4_dis.py 2A116B0 260 2B81410 600
p4_dis.py 24051B0 37A 21206C0 291
p4_dis.py 1067B826 E1 2449220 A1
p4_packets.py <G1.bin> <G2.bin>
```

`p4_dis.py` usa mmap en lectura y rangos pedidos, máximo 64 KiB por rango; no busca patrones en todo el exe. El tiempo t es relativo al primer registro binario. Para G2, el primer spawn correlaciona t=461,048846 con log 20:42:44.570: cero aproximado **20:35:03.521154**. Las horas H de tablas G2 son la conversión estimada G+1:00:00,6, no un evento H medido independiente. G1 conserva la calibración t+19:17:02,57 del informe anterior; el dato firme de los rechazos es t y bytes.

## 2. Formato y semántica de LootingDropFromDeadBodyReq (3052)

### 2.1 Deserialización: hecho, confianza alta

`0x2A11753..0x2A11766`: lector de 4 bytes a `[rbp-0x80]` (clave).
`0x2A117A1..0x2A117AF`: lector de 1 byte a `[rbp+0x68]` (a).
`0x2A117D2..0x2A117E0`: lector de 1 byte a `[rbp+0x60]` (b).
`0x2A117EB..0x2A11805`: exige haber consumido exactamente el payload.

En `0x2A1180A`, `4c8b3b` carga `r15=[contexto/sesión]`: quien saquea procede del contexto, no del paquete. En `0x2A11828`, `e8830f0700` llama a `0x2A827B0` para buscar la clave del cadáver. En `0x2A11839..5C` obtiene `[actor+0x68]+0xC8` (LootDrop) y llama a `0x2B81410` con:

```cpp
LootDeadBody(cadaver.LootDrop, &error, actorDelContexto, a, b);
```

Bytes decisivos:

```text
2A11841 0fb64560       movzx eax, byte [rbp+0x60]
2A11845 88442420       mov [rsp+0x20], al       ; quinto argumento b
2A11849 440fb64d68     movzx r9d, byte [rbp+0x68] ; cuarto argumento a
2A1184E 4d8bc7         mov r8, r15
2A11855 488b89c8000000 mov rcx,[rcx+0xC8]
2A1185C e8affb1600     call 2B81410
```

### 2.2 Qué hacen a y b: hecho, confianza alta

La entrega se intenta **antes** de bifurcar por estos flags: llamada `0x2B81676 -> 0x2B85870`; error se comprueba en `0x2B8167B..7F`. Esta última entra en `0x2B829A0` por `0x2B85A7D` para procesar el botín. `(0,0)` no significa «no dar objetos».

| Campo | Rama comprobada | Efecto |
|---|---|---|
| a, payload +4 | `0x2B817AC` comprueba CharacterInfo+0x14E; `0x2B817B5` prueba a | Si cualquiera es distinto de cero, `0x2B81814 -> 0x2068340`, motivo 0x56: retira cadáver. Si ambos cero, `0x2B817F4 -> 0x29F2890`: refresco dirigido al jugador. |
| b, payload +5 | `0x2B81819` (`80bdd001000000`) | Si cero salta a final; si no, construye resumen de objetos y llama `0x2B8195E -> 0x29FCFF0`. Este escritor pone opcode **0x0900=2304** en `0x29FD17B` (`6641c7070009`): **UseRandomBoxAck**, nombre verificado en `docs/research/data/troc_messages_2976.csv:315`. |

El temporizador natural `0x2C31569..78` hace exactamente:

```text
c644242000  mov byte [rsp+0x20],0
4533c9      xor r9d,r9d
4c8bc6      mov r8,rsi
...         call 2B81410
```

**Valores recomendados para el rescate: `(a,b)=(0,0)`.** Poner `(0,1)` solo activa un resumen de tipo caja aleatoria; poner a=1 agrega retirada. Ninguno corrige un error de destinatario, un cadáver ausente o una tabla sin recompensa. a=0 tampoco promete conservar cadáver si CharacterInfo+0x14E exige retirada.

La resolución de mascota/montura está en `0x2B814BC..0x2B8162C`: tipos de actor 4, 5 o 6 hacen resolución de dueño mediante su componente +0x118 y clave +0x18. El receptor efectivo pasa en r12 y el actor original en r15. **No hay campo de mascota en el payload.** En el rescate automático de un FailAck causado por mascota, 3052 se ejecutará como actor de la sesión corriente; aún falta verificar en juego qué actor concreto es y la respuesta del servidor.

El payload esperado para el gemelo antiguo B01008DD sería `dd 08 10 b0 00 00`; ejemplo de formato, NO instrucción de repetir sobre una clave de una partida distinta.

## 3. Qué sabemos realmente de 0x2569C661

### 3.1 Rechazos medidos otra vez

| G1 t | Objetivo real | Interacción | Cantidad | Payload completo del primer FailAck (23 B) |
|---|---|---|---:|---|
| 692,580222..753,803368 | B01008DD, variante 54006, copia B01F0064 | F428C, mascota B0100004 | 22 | `040010b0 dd0810b0 8c420f00 61c66925 00000000 00 01 00` |
| 736,718771..739,280821 | B0100937, variante 1000456, copia B01F006A | F4249, jugador A0100001 | 6 | `010010a0 370910b0 49420f00 61c66925 00000000 00 01 00` |

Horas G aproximadas de primer rechazo: 19:28:35.150 y 19:29:19.289; H aproximadamente una hora más. No hubo 3052 en G1.

### 3.2 Hallazgo: el FailAck ya contiene índice y procedencia

Formato ampliado de **InteractionPreemptionStepFailAck (3000)**:

| Offset payload | Tipo | Significado comprobado |
|---:|---|---|
| +0 | u32 | actor que intentó interactuar |
| +4 | u32 | objetivo |
| +8 | u32 | clave pública de interacción |
| +12 | u32 | error |
| +16 | u32 | índice de la condición fallida |
| +20 | u8 | 1 = condiciones propias del objetivo; 0 = condiciones de InteractionInfo |
| +21 | u8 | escritor lo fija a 1 en la ruta observada |
| +22 | u8 | `resultado.tipo > 2` en la ruta observada |

Prueba estática independiente:

- `0x21208C9` llama a 24051B0 para la lista propia del actor; fallo: `0x2120923 897508` guarda índice y `0x2120926 c6450c01` guarda procedencia 1.
- `0x2120909` llama para InteractionInfo+0x48; fallo: `0x212092C 897d08`, `0x212092F c6450c00`, procedencia 0.
- `0x1067B826` crea el resultado; `0x1067B83D` lo pasa a 2B36EF0. La evaluación lo recibe como r15 en `0x2B37074`; si falla, copia error en `0x2B37092`.
- `0x1067B8A4..AE` copia resultado+8 y +0xC a estructura de FailAck; `0x1067B8F1..FD` la entrega a 29E7D10.
- `0x29E7E0A` escribe quien (4 B), después `0x29E7E13 -> 0x2449220` escribe campos de estructura +0,+4,+8,+12 (4 B cada uno), +16,+17,+18 (1 B cada uno).

**Conclusión — hecho, alta confianza:** en los 28 fallos, índice=0 y origen=0. La rama responsable es la primera condición de **InteractionInfo**. La hipótesis de que una condición específica de CharacterInfo o del nodo dinámico produjo directamente estos 28 errores queda descartada. Esos datos podrían afectar el estado evaluado, pero no fueron la lista que rechazó la interacción.

### 3.3 Qué evalúa 24051B0 y qué falta

`0x24051E6` comprueba entrada+0x18; `0x2405223` comprueba entrada+0x19. En la rama +0x19=0, si la evaluación A sobre quien interactúa se cumple se genera error Owner; si B sobre objetivo se cumple se genera Target. B se obtiene en `0x2405444`, se construye contexto del objetivo en `0x240544D..57`, y se ejecuta vf+8/+0x10 en `0x2405487/48C`; `0x24054FA` carga el global 0x6CF6D2C del error Target.

No basta leer las referencias visibles a conditioninfo (InteractionDefault/InteractionDefault_Always): InteractionInfo también contiene **árboles inline de condiciones**. En los datos extraídos `build/research/ids/out/gd/interactioninfo.staticinfobody`, los registros F428C y F4249 contienen nodos cuyos tipos se verifican por factoría y RTTI:

- 0x10 = ConditionData_CheckDead, vtable 0x5951988.
- 0x18 = ConditionData_HasLootItem, vtable 0x59506E8.
- 0x1E = ConditionData_CheckActionAttribute, vtable 0x595D838.
- 0x19 = ConditionData_CheckHasImportantItem y 0xB4 = ConditionData_CheckCatched aparecen además en F4249.

La factoría `0x223BB20` lee tipo u16; tabla de destinos en 0x224140C. El nodo lógico `0x24405A0` lee un tipo u8; 0=OR, 1=AND, 2=NOT, 3=hoja, comprobado por constructores/RTTI. **No presento como decodificada la expresión completa ni la hoja causante:** no se terminó el recorrido de deserialización de cada lista de InteractionInfo y no tenemos su estado de servidor durante el rechazo. Afirmar «es Fall» con estos datos sería inventar.

Ejemplo útil para una sonda: CheckDead, evaluación 0x224F0C0, en la ruta sin parámetro especial y actor distinto de gimmick, comprueba `Status+0x273 == 0` en `0x224F175` (`4038b973020000`) y `0x224F17C` (`0f94c0`). Permite contrastar la muerte del servidor con la del cliente.

**Hipótesis pendientes:** estado de acción/atributos del gemelo nacido y muerto enseguida, o estado LootDrop que produce el resultado booleano de B. Confianza media en el conjunto; insuficiente para atribuir una causa a 54006/1000456. El error es común a distintas variantes y no acredita una prohibición fija de esas variantes.

### 3.4 ¿Se puede arreglar al crear el gemelo?

En HEAD, `loot_on_copy_died`, líneas 244–248, emite `u32 variante | u32 0 | vec3 | u8 33`. Noche 2 confirma siete payloads de 21 B y siete Kill de 8 B, sin necesidad de cambiar campos.

`0x2C3F106..11B` pasa el último byte al constructor del descriptor de spawn; 0x28 se normaliza a 0x21. `0x2C3F133` fija transformación; `0x2C3F198..1A2` fija CharacterInfo encontrado por variante; `0x2C3F1A7..1CB` establece referencias al creador/usuario. **No aparece un campo «ignorar condición de saqueo» en 2115.** No recomiendo cambiar 33 a otro motivo al azar ni cambiar el segundo u32 sin decodificarlo.

Dejar el gemelo oculto vivo 250–1000 ms no garantiza que su acción avance: su carga se filtra antes del cliente y no hay simulación de cliente del gemelo. Es una prueba, no un arreglo demostrado. Si la sonda identifica un atributo concreto, probar primero un gemelo nativo de control y el mismo spawn/muerte con ese estado, sin tocar tablas de juego.

## 4. Noche 2: los cadáveres también se pierden por otra vía

Siete gemelos observados, ninguno saqueado en la captura. La variante 1000456 reaparece como gemelo B01009CD: no hubo intento que permita concluir si seguía bloqueada.

| Copia | Variante | Gemelo | t spawn | t retirada real G | Vida del gemelo desde spawn |
|---|---:|---|---:|---:|---:|
| B01F0015 | 54011 | B0100900 | 461,048846 | 549,649778 | 88,601 s |
| B01F0014 | 54038 | B0100915 | 469,998427 | 549,649683 | 79,651 s |
| B01F000B | 54011 | B0100917 | 471,858528 | 549,649577 | 77,791 s |
| B01F0005 | 54021 | B0100930 | 477,279797 | 549,649651 | 72,370 s |
| B01F0004 | 54039 | B010098C | 498,902945 | 549,649618 | 50,747 s |
| B01F000F | 1000456 | B01009CD | 504,926341 | 549,649746 | 44,723 s |
| B01F0006 | 54023 | B01009CE | 505,315458 | 549,649715 | 44,334 s |

La retirada G ocurre aproximadamente a **20:44:13.171**, equivalente H **21:44:13.771**, con incertidumbre de ajuste de reloj. Un ejemplo completo: opcode 2516, payload 10 B `04 00000000 19 170910b0`; inmediatamente se inyectan 2897 `04 0b001fb0` y 2822 `0b001fb0 010010a0` (t=549,649601 y 549,649609).

A las **20:44:14.472 G** (`cdmp.log:1551` y siguientes), el relevo retira copias B01F0015/B01F000B/B01F0014, etc. Cuando decide conservarlas, sus alias ya no están.

**Causa mecánica demostrada:** `loot_filter_s2c:399–409` ve la retirada real del gemelo, borra g_twin_of/g_twins; `:473–475` llama no_loot_here. `loot_keeps_copy:338–342` pasa a falso. La rama posterior de enemy_on_remove/drop no puede conservar esos cadáveres con botín. No es el timeout de 120 s: los gemelos tenían entre 44 y 89 s.

**No demostrado:** por qué el servidor emitió esas retiradas. El 0x19 en un lote amplio es hecho; identificarlo como descarga de región, reaparición o recolector requiere decodificar el enum/callsite o medir servidor. La proximidad temporal a la muerte del anfitrión no prueba causalidad.

### 4.1 Cobertura y huecos actuales

| Ruta | Comportamiento HEAD y evidencia |
|---|---|
| Host Remove | `enemy_on_remove:1984–1994` conserva si loot_keeps_copy y menos de 32 cadáveres. |
| Zona OFF | `enemy_drop_copies_here:1960–1979` conserva los mismos cadáveres, descarta cargas pendientes. |
| Tiempo | `enemy_kept_corpses_tick:1998–2004`: retira al cumplir 120 s desde petición de retirada o al perder alias. |
| Gemelo retirado por servidor G | `loot_filter_s2c:399–409`: cancela botín; caso demostrado G2. |
| Reconexión/cambio compañero | `CoopLink::forget_partner:361–378 -> enemy_reset_partner:1927`: loot_reset y retirada de todas las copias, aunque tuvieran botín. Correcto para evitar mezclar sesiones; no prometer preservación. |
| Reutilización de clave | `enemy_on_spawn:1844` elimina cadáver conservado antes de validar el nuevo paquete. Host solo reserva una clave retirada 10 s (`:108`, `alloc_copy_key_locked:340–347`), conservación G=120 s. Es hueco potencial; el asignador avanza por un rango grande, no prueba que ocurriera esta noche. |
| Pending huérfano | `loot_forget_copy:345–349` borra alias/hit/done pero no invalida Pending.copy. Si se retira definitivamente una copia pendiente, su gemelo tardío aún puede volver a asociarse a la clave vieja. Riesgo comprobable de lógica, no observado en G2. |
| Paquetes agrupados | El filtro de retirada exige tamaño exacto de un actor (`:403`, 10 B payload). Si aparece un lote multi-key con gemelo, lo deja pasar sin cancelar alias. G2 usó individuales; cobertura incompleta. |

## 5. Propuesta concreta, NO aplicada

### 5.1 Cambio inmediato: diagnóstico y ciclo de vida seguro (riesgo bajo)

`src/mod/net/loot_twin.cpp`, Twin, loot_filter_s2c:420–478, loot_forget_copy:345 y loot_tick:268. No cambiar el formato de red del mod; kVersion no cambia por añadir logs locales.

```cpp
// En FailAck ANTES de reescribir target:
if (payload_size == 23 && is_known_twin(target)) {
    log_deferred("Loot reject: twin={} copy={} who={} interaction={:08X} "
                 "error={:08X} condition_index={} actor_conditions={} tail={:02X}/{:02X}",
                 T,C,u32(p),u32(p+8),u32(p+12),u32(p+16),p[20],p[21],p[22]);
}
// Mantener el rescate de HEAD: seis bytes T|0|0, una vez.
// Registrar el bool de encolado, después el encolado real/post de 3052, no llamarlo éxito de saqueo.

// Al retirar un gemelo, capturar razón antes de borrar:
log_deferred("Loot twin gone: twin={} copy={} reason={:02X} age_ms={} dead={} "
             "rescue_sent={} looted_ack={} source=local_server_remove", ...);

// loot_forget_copy, bajo g_lock:
for (Pending& p : g_pending) if (p.copy == copy) p.loot = false;
unalias_locked(copy); g_hit_ms.erase(copy); g_done.erase(copy);
```

El invalidado de Pending evita revivir un alias de una copia terminada; el gemelo tardío conserva la ruta actual «ocultar y matar, sin botín». Registrar siempre terminaciones con cota por segundo, separadas del contador g_logged que deja de narrar tras 80 eventos. Evitar log I/O en el hilo del juego; encolar en g_lines y drenar en loot_tick.

`enemy_relay.cpp:1834 enemy_on_spawn`: mover la eliminación de cadáver conservado de :1844 **después** de validar tamaño, rango de clave y contenido. `remove_copy_now:1822`: motivo explícito (`host_remove`, `zone_off`, `loot_consumed`, `twin_gone`, `keep_timeout`, `key_reuse`, `partner_reset`) y edad. No cambiar todavía asignación/generaciones por este hallazgo; si se rediseña identidad y cambia formato, subir kVersion.

### 5.2 Rescate: conservar parámetros y probar, no sustituir por flags distintos

Mantener el `(0,0)` de `loot_filter_s2c:462–472`. Para la primera prueba, agregar una sonda en entrada/salida de la función **completa** 2B81410 y filtrar solo gemelos propios registrados, o mid-hooks en los callsites indicados con safetyhook::create_mid. Guardar en búfer acotado:

```text
Loot3052 begin: twin=T actor_session=K actor_type=N a=0 b=0 dead_status273=X
Loot3052 end: twin=T error=E returned_owner=K2 elapsed_us=N
Loot3052 inventory: last_twin=T AddItem=... [correlación temporal; el Ack no nombra T]
Loot3052 completed: twin=T RemoveLootableAck received
```

SEH/VirtualQuery en cada cadena/payload; no llamar al evaluador una segunda vez para observarlo ni retener punteros entre ticks. Capturar errores Nak cuyo request sea 3052 y el efecto inventario; «sniffer_inject devolvió true» solo significa encolado.

Si tras 3 s el servidor no confirma, conservar el desbloqueo del aviso actual, pero etiquetar **resultado desconocido**, no «saqueado». AddItem puede preceder al Ack o llevar botín parcial. No regenerar un segundo gemelo ni cambiar a=1/b=1 como reintento automático.

### 5.3 Pérdida del gemelo: decisión de producto pendiente, riesgo medio/alto

Conservar un modelo cliente no restituye un cadáver de servidor. Para remediar las retiradas 0x19 hay dos caminos que necesitan prueba:

- Dar botín con 3052 al morir el gemelo, cuando se confirma su muerte: evita esperar al saqueo manual, pero cambia la experiencia a reparto automático y no es válido activarlo antes de validar entrega/duplicados.
- Proteger la vida del gemelo en servidor o recuperar tras cambio de región: exige identificar el motivo 0x19 y demostrar que no queda un actor persistente; no parchear el recolector global ni recrear gemelos con recompensa ya parcialmente entregada.

La propuesta aplicable ahora es medir primero el retiro real y presentar «botín expirado» correctamente. No hay evidencia suficiente para recomendar una escritura de servidor que garantice persistencia sin riesgo de partida.

## 6. Sonda mínima que decide la causa del rechazo

1. Mantener `condition_index`/origen del FailAck, ya disponibles sin nuevos hooks.
2. Solo ante rechazo de gemelo propio, capturar la siguiente ejecución de la condición 0 mediante create_mid en **0x2405444** (B), **0x240548F** (resultado AL) y/o retorno de 24051B0. En 0x2405444: RDI=entrada, R13=objetivo, R14=actor que interactúa; leer claves en +0x60 con SEH. En 0x240548F: AL=resultado de B, antes de que se pierda.
3. Registrar solo 1 muestra por gemelo/interacción: clave del gemelo, variante conservada en Twin (añadir ese campo), origen/index, entrada+0x10/+0x18/+0x19, vtable RVA del nodo B, estado Status+0x273, acción y atributos, tamaño/lista y elegibilidad del LootDrop. La estructura LootDrop contiene punteros y contadores: imponer límites, nunca volcados recursivos sin límite.
4. Para aislar hojas, hook de entrada/salida de los evaluadores **completos** CheckDead (0x224F0C0), HasLootItem (0x2251CF0, entrada trampolín; inspeccionar su salto antes de instalar), CheckActionAttribute (0x2252AB0). Anidar contexto TLS de la condición B y registrar retorno solo para gemelos auditados. No instalar inline en 0x240548F.
5. Comparar un gemelo 54006, uno 1000456 y un control 1000331, misma distancia y sin mascota; repetir con mascota. Si una hoja difiere de un cadáver nativo saqueable, se sabe qué estado debe arreglarse; si todas son iguales, revisar composición/flags/contexto, no variar spawn al azar.

No hay identidad entre PC por puntero: todos los logs usan claves de actor y RVA de código; los punteros locales solo sirven para leer esa ejecución y se descartan.

## 7. Prueba en juego (después de implementar los cambios)

No se ejecutó ninguna de estas pruebas durante este análisis. Antes de crear gemelos/objetos, copia de la partida según las reglas del proyecto (CONTRIBUTING.md). Build 2976 y mismo mod en ambos PC, registro binario activado.

1. **Control sin mascota:** en G guardar inventario inicial; golpear una copia normal y que H la mate a menos de 30 m. Comprobar `asking ... twin`, load, Kill y Dead del gemelo. Saquear a mano. Éxito = AddItem nativo y RemoveLootable del gemelo traducido a copia; inventario cambia una sola vez. No basta el mensaje visual.
2. **Rechazo reproducible:** repetir con 54006 y 1000456; confirmar error/condición índice 0/origen 0 si reaparece. Debe salir exactamente un 3052 con 6 B y últimos bytes 00 00. Éxito de rescate = servidor entrega objetos/retira derecho, sin nuevo gemelo; si falla, log del error y aviso desaparece a 3 s.
3. **Mascota:** habilitar mascota, dejar dos cadáveres: uno rechazado y uno de control. Confirmar que después de rescate o cancelación deja de reintentar el primero y alcanza el segundo. Contar AddItem para evitar duplicados entre mascota y 3052.
4. **Reaparición H con botín pendiente:** G golpea, H remata, nadie saquea; matar y reaparecer H. Registrar por separado Remove del host y Remove del servidor G. Si este último no existe, la copia con alias debe sobrevivir zona OFF hasta saqueo/120 s. Si existe motivo 0x19, esta build todavía pierde el botín; capturar `Loot twin gone` y no llamarlo problema resuelto.
5. **Salida y retorno de región G:** mismo montaje, G se aleja y vuelve; observar si se repite 0x19 y el servidor recarga la misma clave/variante. Decide la política de persistencia; no generar recompensas de nuevo durante esta prueba.
6. **Dos cadáveres iguales y retirada durante Pending:** confirmar cada Pending queda asociado una sola vez; tras retirada definitiva, un load tardío se oculta/mata sin alias. En reconexión no heredar derechos del compañero anterior.
7. **Persistencia:** solo después de confirmar una entrega correcta, guardar/salir/cargar con copia previa. Confirmar objetos una sola vez y ausencia de gemelo vivo/cadáver extra persistente.

No tocar: tablas de juego, reglas globales de interacción, motivo de muerte, AddItem recibido del host, exclusiones de jefes/únicos, ni inventario por escritura directa. El objetivo es botín calculado por el servidor local con terminación verificable, sin aumentar riesgo de crash o de duplicación.
