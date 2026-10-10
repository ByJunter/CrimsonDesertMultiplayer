# P6 — Cuerda: R5 funciona en la prueba disponible; detalles pendientes

Fecha: 7 oct 2026. HEAD `57bec36bdad634a502eb587f8f899c090ca162c5`. Juego 1.0.0.2976, PE `0x6AB28F00`, base `0x140000000`. Análisis de lectura; ninguna propuesta aplicada. Todas las direcciones de código son RVA.

## Resumen (8 puntos)

1. **Hecho, confianza alta:** R5 está instalado en ambos PC y se ejercitó en Shadow, viendo al anfitrión usar la cuerda.
2. Las cuatro muestras activas de la copia dan **|B−objetivo| = 0,09 / 0,05 / 0,07 / 0,02 m**, con 61→583 escrituras; el extremo llega al árbol.
3. El anfitrión solo aporta una muestra de su jugador local: 0,25 m. Su `set 0 times` es normal: no estaba viendo a la copia enganchada.
4. Las muestras de fase 3 tienen el efecto apagado y la flexión activa al tirar, como pretende el código actual. No hay fallo de cuerda registrado.
5. La muestra posterior a soltar conserva un error viejo: los valores no tienen vigencia ni se invalidan. **No demuestra escrituras tras soltar.**
6. El punto local y la escena del 2763 se conservan; la pieza se resuelve por PC. La divergencia entre pieza local y pieza transmitida sigue sin explicación completa.
7. Pendientes: mano/extremo A, vuelo fase 2, varios enganches y sentido G→H; no hay evidencia para modificar su geometría ahora.
8. Propuestas pequeñas: proteger una lectura previa al SEH y mejorar vigencia/cadencia de sondas. Posible discontinuidad al soltar por desactivar R5 antes del Ack, aún no demostrada visualmente.

## 1. Fuentes y método

- `docs/research/arbol-cuerda-5.md`: hipótesis y diseño previo de R5; no se toman sus conclusiones anteriores como prueba de la sesión actual.
- `src/mod/net/remote_puppet.cpp`, referencias de líneas del HEAD indicado.
- H: `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\cdmp.log` y `cdmp_packets_20261007-213534.{bin,log}`.
- G: log del invitado `cdmp.log` (7 oct, sesión noche2) y `cdmp_packets_20261007-203438.{bin,log}`.
- Ejecutable local, rango acotado `0x59D89A..0x59D8CA`, leído con `build/research/ia2/p4_dis.py`.
- Script nuevo `build/research/ia2/p6_packets.py`: lectura secuencial sin caché, solo imprime 2763/2471. Ejecutar con Python `-B`; no requiere descargar nada.

Los tiempos t de capturas son relativos al primer registro de **cada archivo**; no se restan directamente los t de dos PC. Hora H equivalente a una línea G = G+1:00:00,6, aproximación estimada. La calibración G cercana al enganche es t=395,702716 ≈ 20:41:39.224 (registro de resolución de pieza inmediatamente antes de la inyección). Las horas derivadas no sustituyen las originales de log.

## 2. Línea de tiempo y evidencia

| Hora G | Hora H o equivalente aproximada | Evidencia | Interpretación |
|---|---|---|---|
| 20:34:38.563 | 21:35:34.414, instalación propia H | G:54 / H:52, `rope end on the tree: installed` | Hook R5 instalado en ambos; las horas son eventos distintos, no sincronizados |
| 20:41:38.997 | ≈21:41:39.597 | G:929, árbol `090000DA2C4E76AF`, localizado por escena | Recepción del enganche remoto |
| 20:41:39.224 | ≈21:41:39.824 | G:930, pieza `0x8000000001001E39`; captura G t=395,702716 | Pieza resuelta en G, 2763 inyectado |
| 20:41:40.728 | ≈21:41:41.328 | G:934, copy fase 3, gap 0,09, set 61, bends 0 | Cuerda de copia sin tirar todavía |
| ≈20:41:40.727 | **21:41:41.327** | H:973, ours fase 3, gap 0,25; copy fase 7, gap −1, set 0 | Referencia local casi simultánea a la anterior |
| 20:41:43.739 | ≈21:41:44.339 | G:947, copy pulling, gap 0,05, set 236, bends 127; flex f 0,33/v 0,15/on 1 | Tirando |
| 20:41:46.749 | ≈21:41:47.349 | G:960, gap 0,07, set 410, bends 301; f 0,78/v 0,15/on 1 | Tirando |
| 20:41:49.768 | ≈21:41:50.368 | G:973, gap 0,02, set 583, bends 475; f 0,80/v 0,15/on 1 | Alcanzó límite visual 0,8 |
| 20:41:50.475 | ≈21:41:51.075 | G:975, `let go ... action 0x1F4EF471` | El hilo de red baja `g_copy_hooked` |
| ≈20:41:50.877 | ≈21:41:51.477 | G captura t=407,355303, 2471 de 6 B | Suelta inyectada en el cliente |
| 20:41:52.791 | ≈21:41:53.391 | G:983, fase 7, nodos inexistentes, flex 0; gap 0,09, set 625, bends 516 | Ya soltó; gap guardado de antes |

Entre las muestras activas, el contador de colocación crece a aproximadamente 58,1 / 57,8 / 57,3 veces/s. **Es cadencia de llamadas que retornaron**, no FPS certificado ni medida de red. Las 42 escrituras adicionales entre set 583 y 625 caben en los 707 ms anteriores a la línea de suelta (~59/s); no hay evidencia de que ocurran después.

En H no hay otro `rope probe` en la sesión y no hay 2763 de la copia. En G solo hay ese enganche. Buscar `rope`, `tree`, `fault`, `pull` no devuelve errores de los hooks de cuerda ni suelta por watchdog. Esto cubre la sesión disponible, no demuestra ausencia general de fallos.

### 2.1 Paquetes cotejados

`RemoteCatchResultAck` **2763 / 0x0ACB**, payload **97 B**, paquete **102 B**:

| Campo del payload | H t 349,108716 | G t 395,702716 |
|---|---|---|
| +0 actor u32 | `01 00 10 A0` = A0100001 | `01 F0 10 A0` = A010F001 |
| +4 ranura | 00 | 00 |
| +0E escena 16 B | `AF 76 4E 2C DA 00 00 09 0F FE 00 01 00 00 00 00` | idéntica |
| +1E pieza u64 | `9D 0D 00 01 00 00 00 80` = 8000000001000D9D | `39 1E 00 01 00 00 00 80` = 8000000001001E39 |
| +26 punto float3 | `00 00 41 3E 80 A6 FF 40 00 B0 E2 BE` | idéntico |

Punto = **(0,1884765625; 7,9890747070; −0,4427490234)**. También se conserva el quaternion +3E. El paquete G es idéntico al H salvo actor y pieza, conforme a `hook_tree_here` (593–624) y `puppet_resolve_rope_piece` (2062–2094). El nombre abreviado de árbol del log solo muestra los primeros 8 B del SceneId invertidos (`scene_text`, 576–579), no toda su identidad de 16 B.

`ReleaseRemoteCatchResultAck` **2471 / 0x09A7**, payload **6 B**, G t 407,355303:

```text
a7 09 ff 06 00 | 01 f0 10 a0 00 02
cabecera       | actor A010F001, slot 0, reason 2
```

No hay 2471 en captura H; el cliente G lo fabrica al ver la acción de suelta, código 1761–1777 y `rope_release` 582–588. No hace falta atribuir la ausencia de H a pérdida de red.

**Matiz pendiente:** la sonda local H lleva pieza **8000000003001AE3**, distinta de la transmitida **8000000001000D9D**; G lleva **8000000001001E39**. Se repite la divergencia local/transmitida descrita en el informe previo, pero ahora son otros valores. No se demuestra que los pivotes de estos tres cuerpos coincidan. `|B−T|` pequeño solo prueba alineación con **el objetivo que resuelve cada PC**, no que los dos objetivos de mundo coincidan entre sí. Para cerrar altura/offset del tronco falta registrar T de mundo en ambos.

## 3. Contraste de R5/R6 con el código y el ejecutable

**Hecho, alta:** `puppet_install_rope_bend` 2330–2336 instala `create_mid` en 0x59D89A. Se verificaron directamente los bytes actuales:

```text
0059D89A 44 0F B6 C6       movzx r8d,sil
0059D89E C5 F8 28 CE       vmovaps xmm1,xmm6
0059D8A2 48 8B CF          mov rcx,rdi
0059D8A5 E8 F6 11 01 00    call 0x5AEAA0
```

Offset de archivo 0x59CC9A; PE/base coinciden con la build indicada. El hook precede la llamada que construye la cuerda, no se trata de un inline-hook en mitad de función. La comprobación actual incluye 12 bytes (hasta E8); mejorar la firma al incluir los 4 B del desplazamiento de llamada es una defensa opcional, sin relación demostrada con el síntoma.

`rope_end` 2220–2257:

- Solo ranura 0, tipo 0, fase 3, actor propio o copia; para la copia exige `g_copy_hooked` (2224–2234).
- Comprueba nodo B vivo, obtiene objetivo con `0x5A40E0`, mide B de mundo con `0x26D3940` (2235–2249), todo bajo SEH.
- La distancia se mide **antes de escribir en ese fotograma**, no después (2245–2248). La sonda no captura sistemáticamente el primer fotograma: se programa a +1500 ms (1726). Los 2–9 cm actuales son residuo tras fotogramas anteriores de R5, no una prueba de que R5 era innecesario.
- Al actor propio solo lo mide (2250). Para la copia normaliza sectores y llama `0x26D4B40` (2251–2254).
- La fase 3 usa `rope_effect` apagado: condición real `phase == 2` en 2190, aunque los comentarios 546–551 y 2178–2179 todavía dicen fase 2–3. Las sondas activas confirman AC0/ABC/AB8=0.

**Conclusión demostrada:** la corrección de extremo B está actuando y deja un error pequeño durante este enganche, compatible con que en la prueba ya se ve llegar la cuerda. No valida todavía vuelo, extremo A, diferencias entre pivotes o sentidos simultáneos.

### 3.1 Límites reales de las sondas

1. `g_end_gap_copy/ours` (568) solo cambia cuando hay fase 3 y un nodo resoluble. Al soltar se retorna por 2231 sin invalidarlo. La fase 7 puede mostrar una distancia vieja.
2. `g_rope_end_sets` es acumulativo de sesión; no tiene id de enganche ni contador antes/después de suelta.
3. El local se programa solo al flanco `own_on && !own_was` (1919). La reprogramación de 1895 depende únicamente de `g_copy_hooked`, por eso H tiene una sola sonda y no una por segundo como pedía R0'.
4. `g_bends` aumenta cuando `bend_tree` retorna true (2301–2306), pero esta función también retorna true en caminos sin aplicar flexión (2109–2141). Por sí solo no mide flexiones reales. Aquí los campos +4F4=1 y f 0,33→0,80 corroboran aplicación.
5. La sonda no incluye pos(A), manos, T, edad de muestra, puntos CE0 ni padre de B. No decide las mejoras opcionales R5b/R5c del documento anterior.

## 4. Detalles pendientes, clasificación y prioridad

| Detalle | Estado y confianza | Acción propuesta |
|---|---|---|
| Extremo B pegado al cuerpo | Resuelto en este enganche H→G, alta | Conservar R5 |
| Extremo A separado de la mano | No medido | Medir A/manos; R5b solo si se observa error reproducible >0,5 m y no en local |
| Cuerda durante vuelo fase 2 | No hay muestra en fase 2 | Captura corta de vuelo + sonda por transición; no activar R5c sin comparación local |
| Altura de enganche ligeramente distinta | Posible por piezas/pivotes, media-baja | Comparar T de mundo y parte de árbol en ambos; no cambiar punto local a ciegas |
| Flexión del árbol | Funciona visualmente, perfil aproximado por código | 0,15/s con tope 0,8 (544,2292–2296); falta comparativa local durante el tirón |
| Dos jugadores tirando a la vez | No probado; bloqueo explícito | `bend_tree` 2113–2119 omite flexión remota si el jugador local está en **cualquier** árbol. Documentarlo antes de cambiar política de concurrencia |
| Salto justo antes de soltar | Hipótesis, media-baja | El flag se baja al recibir acción, ~402 ms antes del 2471 inyectado; medir continuidad de B en ese intervalo |
| Seguridad de lectura en hook de flexión | Hecho de código, alta | Proteger `*(ctx.rsi+8)` de 2276, actualmente fuera de los SEH de funciones llamadas |

La ventana de suelta existe por código: 1772 baja `g_copy_hooked` al programar 2471 para `due`, pero R5 consulta ese flag inmediatamente en 2234. No se demuestra que produzca un salto: el nodo conserva la transformación local previa, y su padre puede o no moverse durante ese intervalo. No se propone cambiar la sincronización de la suelta sin una muestra que lo justifique.

La lectura de 2276 no está protegida por el SEH de `rope_effect`/`bend_tree`, pues ocurre **antes de entrar** en ellas. Esto es una mejora concreta de robustez; no se atribuye a ella ningún cierre observado.

## 5. Propuesta lista para aplicar (NO aplicada)

Prioridad baja para acabado visual, baja complejidad para seguridad/diagnóstico. Todo en `src/mod/net/remote_puppet.cpp`; sin cambios al protocolo de red.

### 5.1 Lectura del propietario protegida

Cambiar la guardia de `on_actor_update`, línea 2276:

```cpp
// Helper sin objetos con destructor: lectura del juego bajo SEH.
bool control_has_actor(uintptr_t control, uintptr_t actor) noexcept {
    __try {
        return control && actor &&
               *reinterpret_cast<const uintptr_t*>(control + 8) == actor;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// on_actor_update:
if (!control_has_actor(ctx.rsi, copy)) return;
```

No registrar ni bloquear en cada callback. Si se desea contador de fallos, incrementar atómico y publicar agregado desde `puppet_tick`. Mantener `create_mid`, la verificación de build/firma y apagado por excepción de los cuerpos actuales.

### 5.2 Sondas con vigencia y un evento de enganche

Añadir muestra por lado con generación de copia, tiempo steady y estado válido. Publicar como conjunto coherente (por ejemplo snapshot con secuencia; no asumir consistencia de múltiples atómicos independientes). En `rope_end` (2231–2249) invalidar al salir de fase 3; registrar T/A/B/manos solo en el hilo del hook y protegido, sin I/O. Almacenar el primer gap antes de la primera escritura de **cada** enganche y una muestra tras escritura para confirmar el destino. En `puppet_tick` (1894–1920), reprogramar cada 1 s mientras `copy_on || own_on`, no solo copy_on.

Pseudocódigo de intención, no implementación:

```cpp
if (!phase3 || !nodeB_alive || !target_found) publish_invalid(actor_generation, now);
else publish({generation, hook_id, sample_ms, type, phase, target_world,
              a_world, b_world, hand_world, gap_before, gap_after, writes});

if (probe_due && (copy_on || own_on)) next_probe = now + 1000;
// Al soltar: emitir antes/due/after, conservando contador histórico separado de muestra válida.
```

Formato exacto sugerido:

```text
rope probe2 side=copy hook=3 epoch=7 phase=3 valid=1 age=8ms first=0
T=(...) A=(...) B=(...) hands=(...) gapB_pre=0.07 gapB_post=0.00 gapA_hand=0.04
piece=8000000001001E39 wraps=0 sets_hook=236 bends_applied=127
rope release hook=3 recv=... due=... injected=... sets_recv=625 sets_injected=625
```

Contar `bends_applied` solo junto a la llamada de 2143, no tras cualquier retorno true de `bend_tree`. Ninguna dirección de memoria identifica entidades entre PC; para comparar usar SceneId completo, actor lógico y generación.

### 5.3 Qué conservar y riesgos

Conservar el punto local +26, resolución de pieza por SceneId, R5 solo para la copia/ranura 0/fase 3, la suelta 2471 y efecto apagado en fase 3. No pedir la herramienta al servidor para un actor que este no conoce ni tocar listas de rodeo CE0 sin evidencia.

Las nuevas sondas tienen coste por fotograma: limitar cálculo detallado a transiciones/1 Hz, salvo las mediciones mínimas de R5; publicar fuera del hook. R5b/R5c y una futura suelta con flag ligado a inyección requieren prueba visual independiente. El perfil de flexión simultánea necesita política por árbol y análisis de hilos; no retirar la guardia de 2113 como arreglo rápido.

## 6. Prueba mínima en juego

1. Anfitrión engancha el mismo árbol a 8–15 m, mantiene sin tirar 2 s, tira 5 s, suelta con impulso. Invitado observa/graba mano, tramo completo y tronco. Esperado: 2763 de 97 B en ambos; copia fase 3, muestras vigentes gapB_pre pequeño y gapB_post≈0; flexión activa al tirar y error de mano medido.
2. Repetir soltando sin impulso y volver a enganchar 3 veces. Debe haber un 2471 de 6 B por suelta; fase 7 invalida la muestra y el contador de escrituras no crece tras inyección. Sin mensajes `faulted` ni enganche rechazado.
3. Intercambiar papeles (G engancha, H observa). Esta dirección no está probada en noche 2.
4. Grabar especialmente vuelo fase 2 y últimos 500 ms antes de la suelta. Si hay salto, correlacionar `recv/due/injected` y B/T; solo entonces decidir si `g_copy_hooked` visual debe seguir el momento de inyección.
5. Opcional: ambos enganchados primero a árboles diferentes, luego al mismo. Confirmar la limitación actual de flexión remota y que no queda fuerza aplicada tras soltar. No usar este caso para justificar retirar la guardia sin revisar concurrencia.

Criterio de cierre P6: cuerda continua desde la mano al mismo punto de árbol, muestras vigentes en ambos sentidos, tres ciclos de enganche/suelta sin escrituras sobre nodo inválido y sin errores. La sesión actual acredita la llegada al árbol H→G; los demás criterios quedan pendientes de prueba.
