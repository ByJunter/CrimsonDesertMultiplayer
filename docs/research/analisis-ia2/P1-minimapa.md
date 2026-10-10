# P1 — Copias sin icono en el minimapa

Análisis de solo lectura del 7 oct 2026. HEAD de referencia `57bec36bdad634a502eb587f8f899c090ca162c5`; juego 1.0.0.2976, PE `0x6AB28F00`, base `0x140000000`. Las direcciones siguientes son RVA y no son portables a otra build. No se aplicó ningún arreglo, no se ejecutó el juego. Se crearon únicamente este informe y archivos nuevos `build/research/ia2/p1_*`.

## Resumen (9 líneas)

1. **Hecho, confianza alta:** las 397 consultas contabilizadas hasta G 20:48:55.314 fueron bloqueadas por `*b != 0` en `puppet_minimap.cpp:58`; ninguna entregó icono.
2. **Hecho:** `+0x10A` ya es 27 durante la prueba. Cambiar otra vez el offset o el ancho de `+0x340` no arregla este HEAD.
3. **Hecho estático:** `*b` significa «ocultar este actor en el minimapa», no «modelo invisible»: reúne restricciones de IA, secuenciador y Status distintas.
4. **Hecho:** `CharacterInfo+0x1E8` solo pone `noIcon`; NO explica `*b=1`.
5. **Hecho nuevo:** la sonda `TS+3DC` lee `comps+0x1A0`; el minimapa lee **`comps+0xB0`**. El cero de la sonda no descarta esa rama.
6. **Hecho:** 68/68 snapshots de copias tienen `Status+390=+394=+340=0`; faltan `+341`, el bit de IA y el secuenciador correcto al instante de decisión.
7. **Propuesta:** para copias vivas y visualmente habilitadas, responder 27 antes de llamar al original; conservar veto de muerte, flags físicos y, inicialmente, `+340/+341`.
8. **Hecho:** el recálculo depende de `comp+34 & 6`; una respuesta sin icono puede dejarlo inactivo. Usar refresh del juego, no escribir ese bit a mano.
9. **Límite:** se ha demostrado el veto erróneo del mod, pero no qué rama del motor activó `*b` en estas 397 llamadas. La sonda exacta del §7 lo resuelve en una prueba breve.

## 1. Material y reproducibilidad

Leídos `README.md`, `docs/ROADMAP.md`, `src/mod/net/puppet_minimap.cpp`, las partes relevantes de `enemy_relay.cpp`, `troc_sniffer.cpp`, y las investigaciones `minimapa-icono-enemigo.md` (incluido §9), `mapa-minimapa.md`, `copias-enemigos-faltan-pose-minimapa.md`, `copias-grupo-invisibles.md` y `copia-objetivo-sin-ataques.md`.

Evidencia propia:

- `build/research/ia2/p1_disasm.py`: lector PE que verifica timestamp/base y desensambla únicamente rangos explícitos de hasta 0x4000 bytes; no escanea los 384 MB, no importa módulos con cachés.
- `build/research/ia2/p1_disasm_evidence.txt`: salida guardada de los rangos de decisión, aplicación, tick, refresh y visibilidad.
- `build/research/ia2/p1_capture.py`: lectura secuencial de registros de captura, sin cargar la captura entera en memoria.
- Python utilizado: `build/research/venv/Scripts/python.exe -B`, capstone 5.0.7. Emite un aviso de ubicación de Python base, pero ejecuta correctamente.

Ejemplo reproducible desde la raíz: `build/research/venv/Scripts/python.exe -B build/research/ia2/p1_disasm.py 9E8D10:9E9120 9E9FDB:9EA044`. No genera cachés.

## 2. Línea de tiempo y evidencia real

G = Shadow (invitado), log del invitado `cdmp.log` (7 oct, sesión noche2); H ≈ G + 1 h 00 min 00,6 s según la alineación estimada de relojes. La columna H es conversión aproximada, no una segunda observación independiente del icono.

| G | H aproximada | Evidencia |
|---|---|---|
| 20:34:24.254, línea 13 | 21:34:24.854 | Configuración +0x10A; tabla todavía no disponible, devuelve 65535. Este estado inicial no persiste. |
| 20:34:38.568, líneas 59–60 | 21:34:39.168 | Hook instalado; icono del compañero 23. |
| 20:40:54.129 | 21:40:54.729 | Primera copia B01F0001 encolada, 8866 B, cuatro acciones. |
| 20:40:55.161, línea 754 | 21:40:55.761 | `asked 2`, `given 0`, `hidden 2`; tabla +10A=27. |
| 20:40:56.125, línea 760 | 21:40:56.725 | B01F0001 existe, estado10, login1, s390=s394=s340=0. |
| 20:41:55.182, línea 1045 | 21:41:55.782 | `asked 14`, `given 0`, `hidden 14`. |
| 20:42:55.201, línea 1348 | 21:42:55.801 | `asked 124`, `given 0`, `hidden 124`. |
| 20:43:55.219, línea 1501 | 21:43:55.819 | 202/0/202. |
| 20:44:55.238, línea 1671 | 21:44:55.838 | 239/0/239. |
| 20:46:55.277, línea 1912 | 21:46:55.877 | 255/0/255. |
| 20:47:55.297, línea 2041 | 21:47:55.897 | 316/0/316. |
| 20:48:55.314, línea 2197 | 21:48:55.914 | 397/0/397; `last answer 65535`; +10A=27, +10C=24, +110=32, +112=22, +114=28. |

Las 68 líneas `Enemy relay: copy ... exists` de toda la sesión corresponden a 68 claves distintas; **todas** muestran `Status +390 0x0 +394 0x0 +340 0 | TS+3DC 0`. Esto describe instantes de la sonda, no todas las decisiones ni todo el ciclo de vida.

Captura G `cdmp_packets_20261007-203438.bin`, t relativo al primer registro:

| t (s) | Op / tamaño payload | Clave | Bytes iniciales |
|---|---|---|---|
| 350.627872 | 2005 ActorLoadDataAck / 8866 B | B01F0001 | `A0 22 03 01 00 1F B0 00 00 00 00 FF FF FF FF FF FF FF FF FF 01 F0 10 90 8A 43 0F 00 D4 42 0F 00` |
| 350.819775 | 2697 MoveActorAck / 235 B | B01F0001 | `01 00 1F B0 CB 3E 20 C6 C4 CA 28 44 56 8A 0F C5` |
| 350.945359 | 2697 / 235 B | B01F0001 | `01 00 1F B0 25 9F 20 C6 8B B2 28 44 FD EE 0E C5` |
| 380.878594 | 2005 / 9020 B | B01F0002 | `3A 23 03 02 00 1F B0 ...` |
| 461.247227 | 2530 NotifyHandleDeadAck / 19 B | B01F0015 | `15 00 1F B0 01 F0 10 A0 10 00 01 00 00 00 00 00 00 00 0A` |

Totales del lector, registros S>C de claves B01F: **70 cargas de 70 claves, 17.181 movimientos, 7 muertes**. El minimapa se decide localmente: ningún paquete incluye la salida `*b`, los flags del componente ni el motivo de ocultación. La captura no permite recuperar retrospectivamente ese dato. Tampoco se deben dividir las 397 consultas de un corte de log entre todas las cargas de la sesión y presentar el resultado como «consultas por copia»: falta el histograma por clave y no coinciden los intervalos.

## 3. Causa demostrada y límites

### 3.1 El veto exacto del mod [hecho, alta]

`puppet_minimap.cpp:119` llama primero al original; líneas 131–133 cuentan la consulta y conservan el resultado si `keep_game_icon(comp,b)` devuelve true. Dentro de esta función, línea 58, la primera rama es `if (*b != 0) why=0;`. Las condiciones de Status muerto/+340 son ramas `else if`, por lo que `hidden 397` **no demuestra que esos campos sean cero en las 397 llamadas**: solo demuestra qué condición ganó.

Con `given=0xFFFF`, `enemy=0x10A` y `hidden=397`, la escritura de `*icon=27` en líneas 137–140 es inalcanzable en todas las consultas contadas. La tabla funciona. HEAD lee `Status+0x340` como **uint8_t** en línea61; repetir la corrección del informe anterior no modifica este resultado.

### 3.2 Qué escribe *b [hecho estático, alta]

`0x9E89C0` llama a `0x9E8D10(comp, localPlayer, localActor2, icon, &noIcon, a, b, &relation)`. El camino común de ocultación está en:

```text
009E9FE7 48 8B 85 10 08 00 00  mov rax,[rbp+810]  ; argumento b
009E9FEE C6 00 01              mov byte [rax],1
009E9FF1 48 8B 85 00 08 00 00  mov rax,[rbp+800]  ; noIcon
009E9FF8 C6 00 01              mov byte [rax],1
009EA00D 48 8B 89 28 06 00 00  mov rcx,[UIEvents+628]
009EA014 E8 47 84 9E FF        call 003D2460      ; HideFlagMinimapActor(actorKey)
```

Es una salida **de política de minimapa con efecto lateral en UI**, no una lectura del alfa. Se llega por:

| Condición | Evidencia estática | ¿Prueba que el modelo es invisible? |
|---|---|---|
| Actor no exceptuado por dueño/tipo y falta bit `0x04000000` en `*(u64*)(*(u64*)(*(u64*)(comps+58)+60)+38)` | 9E8E33–9E8E4D → 9E8FAA; 9E905A–9E905F → oculto | No. La decisión aquí solo afecta al minimapa. |
| La rama de facción bajo `Status+4A0 & 1` invalida elegibilidad | 9E8E60–9E8FAF | No. |
| `(u32 Status+390 | u32 Status+394) >= 0x80` para tipos aplicables | 9E903B–9E9054 | Son flags físicos relevantes, pero no coincide con todos los casos de alfa0. |
| `u8 (*(u64*)(comps+B0)+3DC) != 0` | 9E8FDA/9E8FE1; 9E9065/9E906A | No se puede equiparar sin más al alfa. |
| `u8 Status+341 != 0` | 9E9078/9E907F | Esta lectura demuestra veto de minimapa; su equivalencia a «modelo invisible» no está demostrada aquí. |

El bit de IA está leído como qword en `9E8E33` y enmascarado en ebx; basta conservar ese ancho/cadena en la sonda. No confundirlo con el tipo de orden situado en `[AI+50]+40`.

**CharacterInfo+1E8 no activa b.** `9E9705` compara con1; si `Sequencer+2F4==-1`, `9E9720` salta a `9E9FDB`, donde solo pone noIcon=1. La salida va a `9EA027`, sin pasar por `9E9FE7`. Explica que no se promocione el subicono, pero no el contador `hidden` de noche2.

**No es correcto afirmar que hace falta aggro para el punto rojo.** El postproceso de 9E89C0 puede promocionar el subicono normal27 cuando la relación es hostil y noIcon=0. El aggro sirve para otro subicono; véase la evidencia de `minimapa-icono-enemigo.md:175` en adelante.

### 3.3 La sonda lee otro componente [hecho nuevo, alta]

`enemy_relay.cpp:200–201`:

```cpp
const auto ts = *(uintptr_t*)(comps + 0x1A0);
if (ts) f.ts3dc = *(uint8_t*)(ts + 0x3DC);
```

El minimapa usa `comps+0xB0` en 9E8FDA, inequívocamente `48 8B 88 B0 00 00 00`, y lee su +3DC. Por tanto `TS+3DC 0` no descarta el secuenciador. Conservar ambas sondas con nombres distintos. El problema no es que el desplazamiento +3DC sea incorrecto universalmente: pertenece a **dos estructuras distintas**.

### 3.4 Lo todavía no demostrado

- **Inferencia, alta:** b describe una restricción que nuestras copias no deberían heredar como veto universal; en la prueba se ven modelos moviéndose, los snapshots no muestran flags físicos, y la versión anterior dibujaba un rombo al forzar icono.
- **Hipótesis, media:** ausencia del bit de IA o estado del secuenciador tras quitar identidad de spawn. Son candidatos concretos, no la causa raíz establecida de las 397 llamadas.
- **Hipótesis abierta:** +341 es el veto dominante. No fue registrado. Conservarlo inicialmente reduce riesgo, pero si domina, la primera propuesta conservadora seguirá sin mostrar el punto hasta decidir con el alfa si debe ignorarse también.
- No hay evidencia suficiente para decir «los 70 modelos eran visibles siempre», «+341 significa alfa0», ni «noIcon activa b».

## 4. Cómo distinguir ausencia de icono de ocultación física

La ruta `Status vf80` entra por `0x6293A0 → 0x8FF5920`. Parte del código de .xtls sigue legible; se citan solo instrucciones coherentes concretas, no se interpreta el código virtualizado circundante.

```text
08FF5A1D mov eax,[Status+394]
08FF5A23 or  eax,[Status+390]
08FF5A29 test al,8
08FF5A2B je  08FF5A37
08FF5A2D vmovss xmm0,[constante]  ; transparencia especial si máscara0x8
...
08FF5A37–08FF5A49: flags>0 ? 0.0f : 1.0f
08FF5A4F mov rax,[Status+8]      ; actor
08FF5A53 mov rcx,[rax+68]        ; componentes
08FF5A57 mov rax,[rcx+40]        ; CharacterControl
08FF5A5B mov rcx,[rax+E8]        ; objeto de modelo; UNA indirección
08FF5A62 vmovss [rcx+100],xmm0   ; alfa objetivo
08FF5A6E call 00A5A330           ; aplicar con fundido
```

Conclusiones:

- La máscara **0x2** usada por `watch_hidden()` (`enemy_relay.cpp:1261`) es un caso relevante; no es el único. No escribir «bit2» sin máscara: 0x2 es índice1.
- Para máscara combinada no cero y sin0x8, la ruta establece alfa0. El minimapa nativo solo usa el umbral >=0x80: puede ocultarse físicamente con0x2 sin caer por esa comparación.
- La señal directa útil es `float (*(u64*)(*(u64*)(comps+40)+E8)+100)`, lectura protegida y finita. Es el alfa objetivo escrito por esta ruta, no prueba de que un píxel esté renderizado: recursos, culling y fundido siguen existiendo.
- Política conservadora inicial: solo forzar icono si login completado, no muerto, modelo disponible con alfa positivo y finito, y máscara combinada0. Las transparencias especiales con0x8 se dejan al juego hasta prueba específica.
- Una copia fuera de cámara puede seguir llevando icono: no confundir culling/frustum con invisible de juego.

## 5. Por qué hay pocas consultas y cómo refrescar

**Hecho estático:** en el tick 9E7C60, `9E7DFD: F6 47 34 06` prueba `comp+34 & 6`; si cero, se salta el bloque de decisión/aplicación. No es exclusivamente el bit1: el bit2 (máscara4) también permite entrar.

En `9EA6F0`:

- `9EA940: 80 60 34 FD` borra máscara2 al devolver FFFF en la rama de retirada pendiente (precedida por prueba de máscara8 en flags anteriores).
- Existe **otra** retirada de un icono existente: `9EB172: 80 60 34 ED`, que borra máscaras2 y0x10, y llama al mecanismo de desregistro `4FF390`.
- Cuando antes no había icono y el resultado nuevo es válido, `9EAF77: 80 48 34 02` pone máscara2; `9EAF86` registra el componente mediante `4FF1F0`, y se emite CreateMinimapActor.
- En `9E7E19` se consulta una configuración global; un camino llama a9E89C0 directamente y otro reutiliza la caché comp+38..3D. En este segundo modo se solicita actualización posterior mediante una cola TLS y plazo. Tener icono no garantiza llamar al hook literalmente cada frame.

**Corrección al diagnóstico previo:** no todo FFFF recorre9EA940, ni prueba por sí solo bloqueo permanente. Lo demostrado es el circuito de exclusión del tick cuando `&6` queda0 y el desregistro. La falta de snapshots de comp+34/35 impide adjudicarlo a cada copia concreta.

Refresh validado:

- `0x50A1B0(actor)` obtiene `comps+160`, exige la guarda propia de TransformSync y, si pasa, llama a9E8930.
- `9E8930 → 9C61DD0`; este cuerpo inicializa icon/sub=FFFF, a=b=0, llama a9E89C0, escribe la caché +38/+3A/+3C/+3D/+40 y llama a9EA6F0. Actualiza tanto UI como caché.
- El propio manejador físico, al final `8FF5BE4–8FF5BF9`, llama al virtual actor+108 y, si da true, a50A1B0. Hay un camino natural de refresh al cambiar visibilidad física.

**Propuesta:** para cambios nuestros de configuración o para una copia que pasa de no preparada a preparada, encolar la **clave**, resolver actor de nuevo y ejecutar50A1B0 en un contexto de juego seguro que ya permita modificar UI/componentes. No llamarlo desde `puppet_minimap_tick()` actual: su contrato es cualquier hilo (`puppet_minimap.h:31`) y lo llama el worker (`mod.cpp:470`). No guardar punteros de actor/componentes entre frames, no disparar el refresh recursivamente desde9E89C0 y no hacer OR manual de comp+34: faltaría registro/caché/UI.

Primera prueba de riesgo mínimo: arrancar con el hook corregido antes de crear copias. Aprovechar las transiciones naturales de login/flags y comprobar que producen consulta. Si una copia oculta reaparece sin refresh natural, añadir la cola acotada de refresh por transición en un punto de hilo de juego confirmado, no un temporizador que invoque funciones del motor desde worker. Esta extensión necesita integración y verificación del contexto, no está lista para llamarse desde cualquier sitio.

## 6. Arreglo propuesto listo para implementar (NO aplicado)

### 6.1 Parche mínimo de política

Archivo `src/mod/net/puppet_minimap.cpp`, `keep_game_icon` líneas52–68 y `minimap_icon` líneas115–145. Mantener la rama del compañero desde146, los iconos nativos, offsets y validaciones de build.

Separar lectura SEH trivial de política y registro; no introducir destructores C++ dentro de una función con __try. El siguiente pseudodiff define el cambio de flujo exacto:

```diff
-bool keep_game_icon(const void* comp, const uint8_t* b)
+CopyState read_copy_state_seh(const void* comp)
+// Dentro de __try: actor=*(comp+8), comps=*(actor+68), st=*(comps+20),
+// cc=*(comps+40), model=cc ? *(cc+E8) : 0.
+// Devuelve valid, dead=u8(st+273), noIcon=u8(st+340), mapHide=u8(st+341),
+// flags=u32(st+390)|u32(st+394), login=u8(actor+CF),
+// state=u16(actor+5E), alpha=model?float(model+100):NaN.
+// Sin st/model o excepción: valid=false, no forzar.
 
 void minimap_icon(comp, icon, sub, a, b) {
     key = actor_key_of(comp);
+    if (is_enemy_copy(key) && g_enemy_offset.load() > 0) {
+        s = read_copy_state_seh(comp);
+        // Vivo y mostrado por el motor; no usar *b como condición.
+        bool ready = s.valid && s.state==0x10 && s.login!=0;
+        bool visible = ready && s.flags==0 && finite(s.alpha) && s.alpha>0;
+        bool allowed = visible && !s.dead && !s.noIcon && !s.mapHide;
+        if (allowed) {
+            iconKey = icon_from_table(g_enemy_offset.load());
+            if (iconKey != 0xFFFF && safe_write_outputs(iconKey,0xFFFF,0,0)) {
+                ++overrideSuccess;
+                return;  // ANTES del original: no encola HideFlagMinimapActor
+            }
+        }
+        if (s.valid && (s.flags!=0 || (finite(s.alpha) && s.alpha<=0))) {
+            if (safe_write_outputs(0xFFFF,0xFFFF,0,1)) return;
+            // Ocultación física: nunca fabricar punto27.
+        }
+        // Muerto, flags340/341, carga incompleta, tabla no lista o lectura
+        // fallida: decisión original, sin override posterior.
+        g_hook.call<void>(comp,icon,sub,a,b);
+        record_original_result_and_snapshot(...);
+        return;
+    }
     // Para NPC nativo: original + note_npc_icon(), como hoy.
-    // Eliminar bloque que intenta forzar después de llamar al original
-    // y la clasificación universal `if (*b != 0) why=hidden`.
     // Para compañero/no copia: código existente.
 }
```

`safe_write_outputs` protege las cuatro escrituras con SEH y solo devuelve true si todas se realizaron; aumentar contador de éxito **después** de confirmar éxito. Hoy `g_enemy_given` incrementa incluso si el __try de escritura falla (`:143`), lo cual sería un falso éxito de diagnóstico.

Este parche omite la decisión original para copias vivas aptas y les asigna el icono configurado. Por ello reemplaza también posibles iconos especiales de un jefe vivo de estas copias; es coherente con pedir punto rojo para todas, pero si se quieren distinguir jefes/élites, esa política debe ampliarse explícitamente con CharacterInfo. No cambiar los NPC nativos.

**Precaución concreta sobre +341:** se conserva como veto cautelar aunque no hay prueba de que represente alfa0. Si la sonda encuentra que **solo** mapHide bloquea a una copia visible (alpha>0, flags0, alive, ready) y la captura visual confirma el modelo, permitir `allowed` sin `!s.mapHide` para esas copias será el segundo cambio mínimo. No afirmar que esta segunda relajación ya está validada. Eliminar indiscriminadamente b y todos los flags a la vez impediría distinguir qué se está corrigiendo.

### 6.2 Refresh y diagnóstico

- `puppet_minimap_set_enemy_offset` línea213: al cambiar, marcar una generación de configuración; no ejecutar código de juego aquí.
- La extensión de refresh debe consumir esa generación y las transiciones de readiness de copias en un punto de juego confirmado, una vez por clave/generación, con resolución nueva y control de retiro. Reinicio/carga nueva evita necesitarla para la primera comprobación.
- `enemy_relay.cpp:182–202`: añadir campo `seq3dc` leído de comps+B0; mantener `ts3dc` de1A0. Añadir s341 y alpha al informe tardío en `report():209–226`; es diagnóstico auxiliar, no sustituye la medición dentro del hook.
- No cambiar el protocolo; estos cambios son locales, por lo que no requieren kVersion. Si se añadiese información al protocolo en otra propuesta, sí requiere incremento.

### 6.3 Riesgos y qué no tocar

| Riesgo | Control |
|---|---|
| Icono de modelo aún sin cargar o realmente invisible | estado/login, Status completo, modelo/alfa finito; fallo de lectura no fuerza. |
| Filtrar demasiado durante login y perder primer icono | Medir transición + refresh natural; si falta, cola en hilo seguro. |
| Flood de HideFlag y parpadeo por original+override | Responder antes del original solo para copia elegible; no corregir únicamente al retorno. |
| Cadáver convertido en punto enemigo vivo | `dead` impide override; el juego conserva decisión de cadáver/botín. |
| Iconos de transparencia especial no mostrados | Veto conservador flags!=0; ampliar solo tras prueba específica. |
| Crash o carrera al refrescar desde worker | No llamar desde puppet_minimap_tick; clave resuelta en hilo seguro y SEH/VirtualQuery en toda lectura. |
| Cambiar IA real para obtener un punto | No tocar IA, aggro, secuenciador, Status, alfa ni paquetes. Solo salida de icono y refresh por API del motor. |

No instalar inline hooks en mitades de función. Si se añade una sonda puntual de rama, `safetyhook::create_mid`, validación de bytes/build y activación solo diagnóstica. No reutilizar direcciones de memoria como identidad entre PCs.

## 7. Sonda exacta que cierra la causa dentro del motor

Una línea por clave y cambio de firma, más el primer retorno original; no 60 líneas por segundo por enemigo:

```text
MinimapCopy key=0xB01F0001 phase=orig-result thread=... seq=...
 icon=65535 sub=... a=... b=1 ready=1 state=0x10 login=1
 st273=0 st340=0 st341=0 st390=0 st394=0 alpha=1.000
 ai38=0x... ai26=0 seq3dc=0 seq2f4=-1 ts3dc=0
 comp34=0x.. comp35=0x.. cachedIcon=... cachedB=...
 decision=override|dead|model-hidden|map341|map340|not-ready|fault
```

Obtener `seq3dc` y `seq2f4` de comps+B0, ts3dc de1A0, ai38 de `*(AI+60)+38`. Añadir dueño actor+A0 y actor+94 si se reconstruye la excepción inicial. Registrar por separado cada lectura fallida. No usar las direcciones como identificadores de red; la clave local es suficiente.

**Opción decisiva de diagnóstico de rama** (si los flags no cierran la ambigüedad): mid-hook en9E9FE7 filtrado por `r13->key` B01F y con bytes comprobados, sin modificar registros. En ese punto siguen disponibles actor=r13, comp=rsi y los temporales de elegibilidad `[rsp+20]` y secuenciador `[rsp+30]`. Registrar bajo SEH los flags de Status y la elegibilidad. Para distinguir ramas que llegan simultáneamente, instrumentar únicamente los destinos/condiciones concretos de9E9054/905F/906A/907F con create_mid; nunca un inline en esos offsets. No ejecutar ese diagnóstico permanentemente si basta la lectura de campos.

Una muestra `b=1, ai26=0, seq3dc=0, s341=0, flags0, alpha1` cierra el motivo IA, descontando la excepción de dueño. `ai26=1, eligible=1, seq3dc!=0` cierra secuenciador. `eligible=1, seq3dc=0, s341!=0` cierra +341. La carga CharacterInfo1E8 sola queda descartada como escritora de b por desensamblado.

**Modo diagnóstico separado del arreglo:** la línea `phase=orig-result` requiere una pasada con el override desactivado, antes de activar la política nueva. Una vez que la rama elegible retorna antes del original, ya no existe un resultado original que registrar para esa llamada. No llamar adicionalmente al original en cada tick para obtenerlo: se reintroduciría `HideFlagMinimapActor`. En modo corregido registrar únicamente la lectura de estado, el motivo y la salida realmente entregada. El refresh manual continúa pendiente de confirmar el punto seguro de ejecución; el pseudodiff del icono no resuelve por sí solo esa integración.
## 8. Prueba mínima en juego

1. Respaldar partida según las reglas del proyecto (CONTRIBUTING.md) antes de cualquier futura build de prueba que escriba memoria; aplicar y compilar el cambio. Este análisis no lo hace.
2. Invitar a Shadow, ambos cargados y fuera de túnel; permanecer frente a 2–4 enemigos visibles, antes de combate. Una captura de pantalla de Shadow y una línea diagnóstica por clave.
3. Esperado: +10A=27, `decision=override`, `overrideSuccess>0`; punto rojo pequeño para cada copia apta, sin rombo jefe. Componente tras aplicar: icon27 y máscara2 activada por el juego.
4. Moverse y entrar en combate. El punto sigue posición sin spam de HideFlag por la rama de copia y sin afectar iconos nativos/compañero.
5. Matar una copia: `NotifyHandleDeadAck` para la misma clave; diagnóstico dead1, deja de forzarse27. El cadáver puede conservar el icono de botín decidido por el juego.
6. Salir/entrar en zona o hacer reaparecer anfitrión. Copias retiradas no dejan puntos; nuevas claves vuelven a crearlos. Para una transición alfa0→alfa1, exigir `refresh-request`/`refresh-executed` o consulta natural seguida de override.
7. Si no aparece y reason=map341, obtener screenshot visible y s3411/alpha1/flags0; esto decide la relajación específica del §6.1. Si reason=not-ready, comprobar la transición posterior antes de quitar guardias.
8. Registrar `refresh key=... beforeIcon=FFFF beforeFlags34=... afterIcon=27 afterFlags34=...` desde la ejecución segura, junto con hilo y generación. Éxito: no faltan copias visibles, no puntos en modelos ocultos, no puntos vivos en cadáveres ni en claves eliminadas.

No se puede certificar visualmente este arreglo sin esa prueba; el trabajo estático sí cierra el error de clasificación del mod y especifica exactamente qué observación falta del motor.

