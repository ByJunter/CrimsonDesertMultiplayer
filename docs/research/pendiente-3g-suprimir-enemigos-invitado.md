# Etapa 3g: el invitado solo pelea con los enemigos del anfitrión

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Todas las direcciones son RVA (`exe+…`) y
solo valen para esta build. Investigación de solo lectura:
- capstone sobre el exe en disco (`cddis.py`, `lin.py`, `horse2/xtrace.py` para `.xtls`);
- las 50 capturas (`bin64\cdmp_packets_20261004-*` y `D:\cdmp-grabaciones`);
- las tablas estáticas ya extraídas en `build/research/ids/out/gd/`;
- el código actual del mod (`enemy_relay.cpp`, `local_capture.cpp`, `troc_sniffer.cpp`).

El juego no se ejecutó y no se tocó el código del mod. Continúa
[enemigos-del-anfitrion.md](enemigos-del-anfitrion.md) §4 y §8 (3g) y tiene en cuenta
[copias-grupo-invisibles.md](copias-grupo-invisibles.md),
[ids-enemigos-verificacion.md](ids-enemigos-verificacion.md) y las dependencias de la pieza 4
([pieza4-botin-reaccion.md](pieza4-botin-reaccion.md) R6,
[pieza4-dano-servidor.md](pieza4-dano-servidor.md) M2).

Marcas:
- **[verificado]**: leído en el desensamblado o en los datos estáticos;
- **[captura]**: medido en las capturas (se da el script);
- **[inferido]**: deducción; la §6 dice con qué prueba se confirma.

Notación: **H** anfitrión, **G** invitado, **nativo** = enemigo del propio mundo de G, **copia** =
copia en G de un enemigo de H (`0xB01F0000+n`), **gemelo** = el nativo de G que corresponde a un
enemigo de H.

Scripts y salidas en `build/research/p_3g/` (§9).

---

## 0. Resumen

1. **Recomendación: descartar en el cliente de G las cargas de los nativos hostiles dentro de la
   "zona de autoridad" de H, y retirar con `RemoveActorAck` inyectado los que ya estén cargados.**
   Es la receta B de `enemigos-del-anfitrion.md` §4.2, ahora con criterios medidos:
   - **Zona:** un disco de **150 m alrededor de la posición de H** (las dos partidas comparten
     coordenadas). Dentro manda H; fuera, cada mundo es el suyo. H solo releva lo que está en esa
     misma zona, así que nadie ve un enemigo dos veces.
   - **Qué se suprime:** tipo 3, dueño 0, **facción hostil por tabla estática** (55 de 135
     facciones; §2.1), origen de aparición en una lista permitida (§2.2), sin jefes ni únicos, sin
     jinetes, y siempre el **grupo entero** (líder + miembros).
   - **Cómo:** en el mismo mid-hook de entrada de `ClientNet::OnRecvPacket` (`0x35CA00`) que ya
     usa el sniffer, saltando la función con un stub `ret` (como `horse_diag`). Los llamadores no
     usan el valor de retorno [verificado].
2. **El servidor de G no se entera de nada, y así debe ser** [captura + verificado]:
   - el cliente no confirma las cargas: lo primero que manda sobre un NPC es un `RegionInOutReq`
     (8 B) a los 0,27 s, en 2.469 de 2.605 cargas (`s1`);
   - el servidor **nunca** reenvía la carga de una clave viva (0 casos en 2.605); cada recarga trae
     una clave nueva;
   - el cliente ya ignora de forma nativa los mensajes sobre claves que no tiene: tras una retirada
     siguen llegando `UnapplyBuffAck` y otros (más de 1.300 en 3 capturas), y el handler de
     `RemoveActorAck` se salta las claves desconocidas (`0xB62F41`) [verificado].
3. **Para el servidor de G, el nativo descartado queda "oculto y congelado":** sigue vivo en su
   servidor, sin IA (la IA la corre el cliente) y sin poder atacar. Esto **resuelve la duda de la
   pieza 4**: el gemelo existe en el servidor de G, así que R6 (muerte espejo para el botín de G) y
   M2 (golpe sombra) siguen siendo posibles [inferido; prueba L3].
4. **Reversible sin escribir nada más** [captura]:
   - el servidor despawnea por distancia (motivo 25) con un pico a **200-225 m** (966 de 2.000
     retiradas con motivo 25 en 4 capturas, `s3`). Al volver, carga claves nuevas y el filtro decide otra vez;
   - una pantalla de carga (motivo 19) lo recarga todo;
   - opcional (experimental): **entrega diferida**. Se guardan las cargas descartadas y se inyectan
     al terminar el co-op si el servidor no las ha retirado (§3.5).
5. **Emparejar "el mismo" enemigo entre mundos** (solo hace falta para R6/M2 y el diagnóstico, no
   para suprimir) [captura, `s5`]:

   | Origen | Identidad | Misma posición exacta | Misma variante |
   |---|---|---|---|
   | Schedule (S) | `(hash, id)` | 70 % (85 % a menos de 15 m) | 85 % |
   | Secuencia (Q) | `(hash, id)` | 96 % | 95 % |
   | Nodo de facción (F) | `(nodo, slot)` + el más cercano | slot de campamento entero | **2 %** |

6. **Lo que no hay que hacer:**
   - bloquear las Req de spawn;
   - matar a los nativos en el servidor de G (botín, contadores y reapariciones irreversibles);
   - congelarlos visibles (colisión, golpes y riesgo de cierre);
   - cambiar relaciones de facción (§4).
7. **El eco de un PC no puede probar 3g de extremo a extremo** (sin nativos no hay copias). Sí puede
   probar:
   - el filtro en **dry-run**;
   - un **modo laboratorio** con zona manual y relevo apagado: supresión, retirada, vuelta,
     entrega diferida y `KillCheatReq` a un gemelo descartado (§6).

---

## 1. Cómo llegan los enemigos al cliente de G y qué pasa si no se crean

### 1.1 Ciclo de vida de un NPC en el protocolo [captura, `s1_lifecycle.py` en 112627, 090931 y 114648]

| Paso | Dirección | Mensajes | Datos |
|---|---|---|---|
| Alta | S>C | `ActorLoadDataAck` de **un solo registro** (tipo 3/6) | 2.605 cargas. **0 recargas de una clave ya viva**: cada alta trae clave nueva |
| Primeras Req del cliente | C>S | `RegionInOutReq` ×5-6 (región de juego en la que entra), luego IA | `RegionInOutReq` es lo primero en 2.469 de 2.605. **No hay mensaje de "cargado" ni de login** |
| Vida | C>S | `ChangeAIStateReq`, `StartActionReq`, `MoveActorReq`, `UpdateAIDestinationReq`, `StartAIFunctionReq`, `DoFrameEventReq`… | Todo lo emite el cliente que controla al NPC: la IA vive en el cliente (`bdo-emuladores-monstruos.md` §4) |
| Baja | S>C | `RemoveActorAck` | Tras la baja el cliente aún manda `RegionInOutReq` (salida), `ChangeAggroRelationValidTickReq` y algún `MoveActorReq` rezagado |
| Después de la baja | S>C | `UnapplyBuffAck` (≥1.300), `VaryDataDefinedStaticStatAck` | El servidor sigue hablando de claves que el cliente ya no tiene, y no pasa nada |

`RegionInOutReq` (3067, 8 B) = `u32 actor | u16 región | u8 entra(1)/sale(0) | u8` [captura,
`s2_regioninout.py`]. Las regiones típicas son `0x0001`, `0x0004`, `0x000D`, `0x2710` y `0x271B`.

**Consecuencia:** si el cliente de G no entrega la carga, el servidor de G no recibe nada de ese NPC:
- ni regiones, ni IA, ni movimiento;
- ni golpes (`DoFrameEventReq` es del cliente), así que **no puede hacer daño al invitado**;
- en el servidor queda vivo, quieto en su punto de carga.

Es lo mismo que le pasa a un NPC lejano antes de que su controlador arranque, solo que para siempre.

### 1.2 El handler de `RemoveActorAck` (`0xB62C70`) [verificado, `p_3g/b62c70.asm`]

- **Formato:** `u8 04 | u32 X | u8 motivo | n × u32 clave`.
  - Si `X ≠ 0`, se busca aparte y se llama a `TS vf(0x150)`.
- **Por cada clave:**
  1. `0x8A67E0(mgr, &out, clave)`;
  2. **si no existe, la salta** (`0xB62FD0`/`0xB62FD4`). Así que `RemoveActorAck` sobre claves
     descartadas es inocuo, y el servidor lo enviará (motivo 25) cuando G se aleje;
  3. salta los actores en estado `+5E == 1`, y al jugador local (tipo 1 con `+0x96`);
  4. `rsi = actor->vf(0x160)` (actor enlazado: montura o vehículo);
  5. `0x2068340(actor, out, motivo, err)`;
  6. si `motivo ≠ 7` y el enlazado no es del jugador local (`[rsi+0x60]` frente a
     `[[mgr+0x58]+0x60]`) y `0x206ADA0(rsi)`: **también se retira el enlazado**, con motivo 6.
- **`0x2068340` → `.xtls 0xE0D9AF0`** [verificado]:
  1. bloquea `actor+0x18`;
  2. si el estado `+5E` es `0x20`/`0x40` (ya saliendo), devuelve `eErrNoUserAlreadyLogouting`
     (`[0x6CDB934]`) y no hace nada;
  3. si no: `+0x98 = err`, `+0x9C = motivo`, `+0x5C = estado previo`, `vf(0x80)`, desbloquea,
     `vf(0x110)(out)`.

  Es una baja **local**: no hay ninguna llamada de red en esta ruta. Una retirada inyectada dos
  veces es inocua.

### 1.3 `OnRecvPacket` se puede saltar entero [verificado]

- `0x35CA00` tiene 4 llamadores (`0x35BB53`, `0x35C0ED`, `0x35C951`, `0x86E55AA`).
- En los dos leídos, tras la llamada no se usa `rax` (`0x35BB58 lea rdx…`, `0x35C0F2 test rbx`).
- La propia función ya vuelve sin hacer nada si el tamaño está fuera de rango (`0x35CA3C → 0x35CCCC`).
- El mid-hook del sniffer está **en la entrada**: la pila aún no se ha tocado. Poner
  `ctx.rip = stub ret` devuelve limpio al llamador, como hace `horse_diag` con `0x85FBA0`.
- **Una llamada = un mensaje Troc**: el descarte no se lleva nada más por delante.

### 1.4 Grupos de campamento [captura, `s9_groups.py`, todas las capturas]

| Hecho | Medida |
|---|---|
| El padre (líder, `actor+0xAC`) se carga **antes** que el miembro | 617 de 617 (0 sin padre); 560 a menos de 0,1 s |
| Padre y miembro de la misma hostilidad | 545 de 549 con padre de tipo 3 |
| Miembros cuyo padre es un carro (tipo 7) o una montura (tipo 5) | 64 + 4 (jinetes: el relevo ya los rechaza) |
| Un líder y sus hijos se retiran en el **mismo** `RemoveActorAck` | nunca (0 de 185): el servidor los retira por separado |

Por `copias-grupo-invisibles.md` §2.2:
- un miembro cuyo padre no existe se destruye (`eErrNoParentsNotExist`);
- un líder al que le falta un hijo se queda en estado 8 para siempre.

**Regla:** la decisión es **por grupo y la toma el líder**:
- se suprime un líder → se suprimen todos los que lo nombran como padre;
- se entrega un líder → se entregan sus hijos.

### 1.5 Armas "on demand" [captura, `s11_weapons.py`]

- Las armas en mano de los NPC son gimmicks de tipo 7 pedidos por el cliente
  (`OnDemandEquipDockingGimmickSpawnReq` = `04 | u32 NPC | u16 0`) **cuando el NPC entra en
  combate**.
- Un nativo descartado nunca llega a pedirlas: no hay armas huérfanas.
- Al retirar un NPC, sus gimmicks **no** salen en el mismo mensaje (en ninguna de las retiradas medidas). A veces
  llegan después con motivo 45, y 160 no se retiran en toda la captura.
- **Retirar con `RemoveActorAck` inyectado a un nativo en combate podría dejar su arma flotando**
  [inferido]. Por eso el barrido solo retira nativos que no estén en combate (§3.3).

---

## 2. Qué suprimir: el predicado

### 2.1 Hostilidad: está en las tablas [verificado en `gamedata`, `s6_faction_relation.py`]

- **Registro de `faction.staticinfo`:** `u32 key | u32 L | nombre | …`. Tras el nombre, `+0x22`
  es un u16 **grupo de relación** y `+0x24` un u16 grupo regional.
- **`factionrelationgroup`** tiene 5 grupos, cada uno con `[enemigos], [amigos]`:

| Grupo | Enemigos | Amigos |
|---|---|---|
| `0x4243` Graymane (el jugador) | **HostileCombat, Monster_Common** | FriendlyCombat, NPC_Common |
| `0x4244` FriendlyCombat | HostileCombat, Monster_Common | Graymane, NPC_Common |
| `0x4245` HostileCombat | Graymane, FriendlyCombat, Monster_Common | NPC_Common |
| `0x4246` NPC_Common | Monster_Common | Graymane, FriendlyCombat, HostileCombat |
| `0x4247` Monster_Common | todos | — |

- **55 de 135 facciones son hostiles al jugador por defecto:** 18 HostileCombat y 37
  Monster_Common. La lista está en `p_3g/tables_2976.txt`.
- Contra las capturas [`s7_rule_eval.py`]:
  - de los 121 NPC que **atacaron** al jugador (`DoFrameEventReq` con el jugador como objetivo),
    **118 son de facción hostil**;
  - los 3 restantes eran de facción 0 ("Wandering Freesword" ×2 y "Stranger"), seguramente
    provocados.
  - Las facciones hostiles vistas: BleedBlade (764 cargas), Bandit (333), Antumbra (255) y
    WolfPursuer (113).
- La relación puede cambiar en partida (`UpdateFactionRelationDataAck` 2597). La tabla es la
  semilla, y se completa en vivo con lo aprendido (§3.2).

### 2.2 Origen de aparición: lista permitida [captura + datos, `s4`/`s7`, `tables_2976.txt`]

El bloque T0 de la carga (`ids-enemigos-verificacion.md` §2) da el origen. Los hashes se resuelven
con `ids/stemhash.pkl`.

| Clase | Ejemplos | Cargas hostiles | Atacaron al jugador | Propuesta |
|---|---|---|---|---|
| **Permitida** (585 hashes): `cd_mNN_mNN_monster_*`, `cd_seq_spawn_docu_npc_*` | `…monster_beacon_01_0003`, `…monster_temple_00_0004_01`, `…docu_npc_small_0001` | 797 | 84 | suprimir |
| **Nodo de facción (F)** | `Node_Her_EmbernestBeacon` | 379 | 24 | suprimir |
| **Sin identidad** | bandidos de camino | 56 | 2 | suprimir (salvo jinetes) |
| **Cautela** (1.170 hashes): `cd_seq_node_*`, `cd_seq_faction_*`, `cd_reoccupation*` | `…hernandcastle_daily_0700` (156), `…node_reinforcement_3p`, `…bleed_moru_slave_*` | 237 | 8 | interruptor aparte (eventos diarios o de facción) |
| **Prohibida**: misiones, jefes, contenidos, minijuegos, "wanted", "memory" (3.024 nombres) | `cd_seq_quest_*`, `cd_seq_boss_*` | **0** hostiles en las capturas | 0 | nunca |

- Con la clase permitida, F y sin identidad se cubren 110 de los 118 atacantes (93 %); con la
  cautela, los 118.
- Ninguna carga hostil vino de un origen de misión en las capturas. Pero las capturas son todas de
  la zona de Hernand: **la lista prohibida es la red de seguridad** para el resto del mapa.
- La lista permitida **se ancla en el nombre**, no en la ruta: los hashes son
  `hashlittle(stem, 0xC5EDE)` y hay stems que solo aparecen como `.road`.

### 2.3 Predicado completo (todo se lee de la carga, sin tocar la memoria)

Offsets de payload (`u16 len | registro`):

```text
suprimir(carga) si co-op activo, enlace sano, zona activa y:
  len+2 == tamaño                    (un solo registro)
  p[2] == 3                          (persona; los tipos 4, 5, 6 y 7 nunca)
  u32[+7] == 0                       (sin dueño)
  clave >> 20 == 0xB01 y no es de red (0xB01F…, 0xB010F00x)
  u32[+0x1C] ∈ facciones_hostiles    (tabla §2.1 ∪ aprendidas)
  u32[+0x18] ∉ variantes_excluidas   (characterinfo con "Boss"/"Unique": 120 + 837)
  origen(T0) ∈ {permitida, F, ninguna} (∪ cautela con interruptor); nunca prohibida
  cola: padre == 0, o padre ∈ suprimidos (miembro de un grupo suprimido)
        y nunca padre de tipo 5/7 (jinete) ni hijos de Hierarchy
  posición (última Transform) a ≤ R_zona del anfitrión
```

- Coste: leer la cabecera, buscar la firma de T0 (`0C 00 | 8×00 | FF×4`, única en 5.332 de 5.332
  cargas) y la última Transform desde el final.
- Unos 8 KB recorridos por carga de NPC (1,4/s de media; picos de 270/s al cargar zona). Es
  asumible en el hilo de red.

---

## 3. Diseño recomendado

### 3.1 La zona de autoridad

- **Centro:** la posición de H en el mundo de G, leída de la copia de H
  (`game::actor_world_position(0xA010F001)`) o de su `PlayerState`.
- **Radio:** `R_zona = 150 m`, con histéresis a 170 m para la desactivación.
  - El servidor de H tiene cargado todo lo que está a menos de ~200 m de H (despawn por distancia
    a 200-225 m, §0.4). Con 150 m, **H tiene seguro su gemelo cargado** y puede relevarlo.
  - Fuera de la zona, G juega su mundo, y H no releva nada que esté fuera.
- **Activa** solo si:
  - el enlace está vivo;
  - la copia de H existe;
  - G está a menos de 300 m de H;
  - y H ha anunciado que releva (`RelKind` nuevo, §3.6).

  Al caer cualquiera de las cuatro, se desactiva en el acto.
- **H releva** (requisito de 3e, `enemigos-del-anfitrion.md` §5):
  - todo enemigo que cumpla el mismo predicado (§2.3) dentro de la zona y a menos de 100 m de la
    marioneta de G;
  - más los comprometidos con la marioneta (aunque salgan de la zona);
  - con el tope de 24, **los comprometidos primero**: un enemigo de H que pega a G nunca puede
    quedarse sin copia.
- **"Tierra de nadie":**
  - los nativos de G a 100-150 m de G dentro de la zona se suprimen, y H aún no los releva. Se ve
    un campamento vacío a lo lejos, que se puebla al acercarse a 100 m (como un LOD);
  - al revés: una copia de H que persigue a G fuera de la zona puede coincidir con un nativo. Es un
    caso de borde aceptado.

### 3.2 Descarte en la llegada (hilo de red de G)

En `on_recv_packet`, **después** de `capture()` (la captura sigue registrando la carga para
diagnóstico) y solo si `!t_injecting`:

1. Si es un `ActorLoadDataAck` que cumple §2.3:
   - `held[clave] = payload` (para la entrega diferida y el emparejamiento);
   - `suppressed.insert(clave)`;
   - si es líder, también sus hijos (`+0xB0`);
   - contadores y `ctx.rip = stub_ret`.
2. Si es un `RemoveActorAck` con claves suprimidas: borrarlas de `held` y `suppressed`, y
   **entregarlo** (el cliente las ignora solo).
3. Los demás S>C sobre claves suprimidas se dejan pasar: el cliente ya los ignora (§1.1).
   - Opcional: guardar el último `VaryHPAck` en `held` para la entrega diferida.
4. **Aprendizaje de hostilidad:** facción ≠ 0 de un nativo que:
   - fija o golpea al jugador de G (`CurrentTargetChangeReq` con objetivo propio, o
     `DoFrameEventReq` con el jugador);
   - o que H marca como comprometido.

   Se añade a `facciones_hostiles` durante la sesión.
5. **Disyuntor:** se apaga la supresión y se registra con un log claro si:
   - una misma identidad se descarta más de 4 veces en 2 minutos;
   - o se descartan más de 60 cargas por minuto.

   La rotación natural es alta: 138 identidades vuelven a cargarse antes de 10 s tras una retirada
   en vida, con motivos 30, 80 o 21 (`s10_respawn.py`). El umbral tiene que tolerarla.

### 3.3 Barrido de lo que ya estaba (hilo trabajador, cada 1 s)

- **Candidatos:** nativos vivos de tipo 3 que cumplan §2.3 con la posición **actual** (lectura
  protegida, `actor_world_position`) dentro de la zona.
  - Se toma el grupo entero: líder y todos sus hijos según su carga, que guardan `g_loads` y
    `g_known`.
- **Solo si el grupo no está en combate:**
  - ningún miembro fijó al jugador de G en los últimos 10 s (`g_aimed` ya lo registra);
  - y, si se lee, `ChangeBattleState` es 0.
  - Si lo está, se espera; tras 20 s dentro de la zona se retira igual (configurable).
- **`RemoveActorAck` inyectado** con todas las claves del grupo en un mensaje: `04 | 0 | 25 | n×clave`.
  - Motivo **25**: el mismo que el despawn por distancia del servidor.
  - Entra por la cola de inyección S>C que ya usa `remote_puppet`.
- **Antes de retirar:** guardar en `held` la carga original **con la posición actual reescrita**
  (las 3 copias, método `spawn_horse`) para la entrega diferida.
- **Qué verá el servidor de G** [captura de bajas naturales, §1.1]: `RegionInOutReq` de salida y
  `ChangeAggroRelationValidTickReq`. Es lo mismo que en un despawn normal.

### 3.4 Al acabar la zona (desconexión, fin del co-op o G se aleja de H)

- **Por defecto: no hacer nada.**
  - Los nativos descartados siguen vivos y quietos en el servidor de G.
  - Vuelven cuando G se aleja más de ~200 m y regresa (el servidor los retira con motivo 25 y los
    vuelve a crear con clave nueva) o tras una pantalla de carga.
  - En la interfaz: "tus enemigos vuelven al cambiar de zona".
- **Las copias de H se retiran en bloque** (`EnemyRemove`, ya existe).

### 3.5 Entrega diferida (opcional, experimental)

- Al desactivar la zona, inyectar las cargas de `held` que el servidor no haya retirado:
  - en el orden en que llegaron (primero los líderes);
  - a ritmo lento (5 por tick).
- **Por qué podría funcionar** [inferido]:
  - el servidor cree que el cliente ya las tiene;
  - el NPC no ha hecho nada desde la carga: no tiene movimiento, IA ni golpes, así que su estado
    en el servidor es casi el de la carga;
  - es una entrega tardía del mismo paquete.
- **Riesgos:**
  - campos de tiempo dentro del registro (CC, buffs) caducados;
  - buffs aplicados o quitados por el servidor mientras tanto;
  - la posición, en los retirados por barrido, se corrige con la reescrita.
- Se decide con la prueba L2. Mientras no pase, queda apagada.

### 3.6 Cambios de protocolo (`kVersion` 5 → 6, junto con 3e)

| Nuevo | Tipo | Formato | Uso |
|---|---|---|---|
| `RelKind::EnemyAuthority = 8` | fiable | `u8 flags (activa, cautela) │ f32 R_zona │ u16 n │ n × u32 facción aprendida` | H anuncia que releva y comparte las facciones que aprendió. G no suprime nada sin este mensaje |
| `EnemySpawn` + identidad | fiable (amplía el existente) | `… │ u8 clase (S/Q/F/-) │ u32 hash o nodo │ u16 id o slot │ u32 variante │ f32 x,y,z de carga` | Diagnóstico y enlace copia ↔ gemelo para R6/M2. Hoy `strip_spawn_identity` borra la identidad de la carga; aquí viaja aparte |

- G usa la identidad para enlazar copia y gemelo:
  - S/Q: el mismo `(hash, id)` con la posición más cercana (≤ 30 m);
  - F: el mismo `(nodo, slot)` y el más cercano;
  - sin identidad: la misma facción y ≤ 15 m.

### 3.7 Lo que hay que cambiar en lo que ya existe

- **Roles:** hoy el relevo es simétrico (Ctrl+F7 en los dos lados).
  - Con 3g: `[net] role=host` releva y no suprime; `role=guest` suprime y **no releva** (sus
    nativos están suprimidos).
  - En el eco (un PC), los dos papeles chocan: §6.
- **Daño espejo** (`coop_damage`, activo por defecto): `apply_remote_damage` empareja por variante
  y punto de aparición entre los `g_enemies` del invitado, **que incluyen las cargas descartadas**
  (el sniffer las ve antes de la decisión).
  - Hoy no se aplicaría porque no están en `g_actors` (no mandan Moves), pero es frágil.
  - **Apagarlo mientras la zona esté activa** y excluir `suppressed` de forma explícita.
- `enemy_relay::g_loads` del invitado también aprenderá cargas suprimidas: no deben elegirse como
  candidatos (rol de invitado = sin relevo).
- **Tablas por build** (generadas por script en `p_3g/tables_2976.txt`, 2.772 líneas):
  - 55 facciones;
  - 957 variantes excluidas;
  - 585 hashes permitidos;
  - 1.170 de cautela.

  Se incrustan como arrays `constexpr` ordenados y solo se usan si la build es la 2976.

---

## 4. Enfoques candidatos

| | Enfoque | Cómo | A favor | En contra | Riesgo | Esfuerzo |
|---|---|---|---|---|---|---|
| **A** | **Descartar en el cliente + barrido** (recomendado) | §3. Mid-hook existente + stub `ret`; `RemoveActorAck` inyectado (formato ya usado con las copias) | Nada en la memoria del juego, nada al servidor. Invisible, sin colisión, sin IA ni daño. El gemelo sigue en el servidor de G (R6/M2). Se deshace solo al cambiar de zona | Los descartados no vuelven hasta recargar la zona (salvo §3.5). Las misiones de "matar X" de G no avanzan con las copias (como en Genshin). Rotación de spawns que vigilar (disyuntor) | **Bajo-medio**: se salta una función entera ya enganchada; el `RemoveActorAck` de un NPC real es la ruta natural del motivo 25 | 2-3 días (+1 la entrega diferida) |
| B | Congelar en el sitio: reescribir `rec+0x12` del nativo a un usuario ajeno (modo 2, sin IA) y ocultarlo (`Status+0x390` bit 2 → alfa 0, `0x1795490`/`0x6293A0`) | Mismo truco que las copias, aplicado al nativo | El gemelo existe también en el cliente de G | Visible, salvo con escrituras o llamadas en un actor vivo. **Oculto no es intangible**: los "originales invisibles" seguían recibiendo y dando golpes (`copias-grupo-invisibles.md` §2.4). El problema de la capa sin acción (contención). Los golpes de G al aire acaban en el gemelo y en el servidor de G | Medio-alto (posible cierre, que se reporta) | 4-6 días |
| C | Matarlos en el servidor de G (`KillCheatReq` 2279 `u32 clave │ u32 opción`, o `VaryStatCheatReq` 2703) | C>S por `sniffer_inject_c2s` (ya usado en X4) | Trivial | **Irreversible:** botín en el suelo, buff 20001, contadores, campamento "limpio" y reaparición a los 30-120 s (`s10`: 20 de 117 muertes reaparecieron en la sesión, 19 de ellas en menos de 2 min y con el jugador a 50-100 m) | Medio (partida de G alterada) | 0,5 días |
| D | Gemelo marioneta: usar el nativo de G como la copia (modo 2) y moverlo con los datos de H | Emparejamiento §3.6 + recetas de 3b | Todo nativo en G: golpes y botín de G sin piezas extra | En nodos de facción la variante coincide en el 2 %. HP distinto en cada servidor. Grupos. 17 % sin identidad | Alto | Semanas |

Descartados de entrada:
- **Bloquear Req de spawn** (`enemigos-del-anfitrion.md` §4.2 A): se llevan objetos y estado guardado.
- **Cambiar relaciones de facción** (inyectar `UpdateFactionRelationDataAck`): global, persistente
  en el cliente, y los nativos seguirían visibles.
- **Solo ocultar** (opción C antigua): siguen atacando.

**Combinación recomendada:** A como base. C solo en su papel de R6 (pieza 4): cuando muere E en H,
`KillCheatReq` al gemelo **suprimido** en G, para que G tenga su botín. Así C deja de ser una
supresión y pasa a ser una consecuencia de una muerte real.

---

## 5. Misiones, jefes y estado del mundo de G

| Tema | Con el enfoque A | Fuente |
|---|---|---|
| NPC de misión | No se tocan: tipo 4 nunca; orígenes `quest`/`boss`/`contents`/`wanted`/`memory` nunca; facción 0 nunca | §2.2 [verificado en datos] |
| Jefes y élites | Variantes "Boss"/"MiddleBoss" (120) y "Unique" (837) excluidas. H tampoco debería relevarlos en la primera versión | `characterinfo` [verificado] |
| Reapariciones en G | No hay muertes en G, así que no arranca ningún temporizador. El campamento sigue intacto al acabar el co-op | `s10` [captura] |
| Campamento "limpio" o nodo conquistado en G | No avanza con las copias (las mata el servidor de H). Se acepta y se dice en la interfaz (precedente: Genshin) | `genshin-coop-referencia.md` §13 |
| Eventos de nodo (`cd_seq_node_*_daily_*`, refuerzos) | Clase de cautela, con interruptor propio: sin él, G ve su evento y el de H | §2.2 |
| Disparadores (`RegionInOutReq`, `SequencerTriggerVolumeInOutReq`, `GamePlayTriggerInOutReq`) | Los nativos descartados no los mandan, igual que un NPC quieto. Los retirados mandan la salida | §1.1 [captura] |
| Guardado de G durante el co-op | El estado de esos NPC en el servidor no cambia | [inferido] |
| Estado "en combate" de G al retirar nativos que le peleaban | No medido: por eso el barrido espera a que no haya combate | Pregunta P2 |

---

## 6. Pruebas en un PC (eco con `cdmp_peer`)

**Preparación común:**
- copia de `%LOCALAPPDATA%\Pearl Abyss\CD\save` en `backups/`;
- ir al campamento de Bleed Bandits de `cd_m11_m04_monster_beacon_01_0003` (cerca de
  (−10.284, 640, −3.383)) o a `Node_Her_EmbernestBeacon` (−10.201, 623, −3.481);
- `[sniffer] record=1` para tener captura;
- con el eco, este PC es H y G a la vez: **la supresión real no se puede probar con el relevo
  encendido** (suprimir los nativos haría desaparecer lo que se copia).

**Modo laboratorio:** `[coop] suppress_lab=1`:
- relevo de enemigos apagado;
- la zona se centra en **tu propio jugador** con una tecla (p. ej. Ctrl+F8), radio 60 m.

| # | Prueba | Escribe en el juego | Resultado previsto |
|---|---|---|---|
| **L0** | Dry-run: el predicado se evalúa y registra `WOULD DROP clave nombre facción origen dist` sin descartar nada, con eco y relevo normales | No | Las líneas coinciden con los Bleed Bandits del campamento. Ningún guardia, animal, civil ni jinete. En una ciudad, 0 |
| **L1** | Laboratorio: activar la zona **lejos** (> 250 m) y caminar hasta el campamento | Descarta paquetes | **Campamento vacío**: ningún bandido visible, ninguna barra de vida, sin ataques. El log del juego (`game_errors`) sin `eErrNoParentsNotExist`/`loginCharacter` (se descarta el grupo entero). La captura muestra cargas sin `RegionInOutReq` detrás. El disyuntor no salta |
| **L1b** | Laboratorio: activar la zona **junto** a un campamento ya cargado y en reposo | Inyecta `RemoveActorAck` | Los bandidos desaparecen (el fundido del motivo 25). En C>S salen `RegionInOutReq` de salida. Sin armas flotando (estaban en reposo) |
| **L1c** | Igual que L1b, pero con el campamento peleando contigo (la espera de 20 s desactivada para medir) | Inyecta | Medir: armas huérfanas, música o estado de combate pegado, y si se puede guardar o viajar después (P2, P7) |
| **L1d** | Con la zona activa, alejarse 300 m y volver | Descarta | Llegan `RemoveActorAck` (25) para las claves descartadas (ignoradas) y, al volver, cargas con **claves nuevas**, descartadas otra vez. Mide la rotación para el disyuntor |
| **L1e** | Desactivar la zona, alejarse 300 m y volver | No | El campamento vuelve normal y se puede pelear con él |
| **L2** | Entrega diferida: zona activa con el campamento descartado; a 80 m, desactivar con `restore` | Inyecta las cargas guardadas | **Previsto:** los bandidos aparecen en su sitio con login normal (`+5E 0x10`, `+CF 1` en el informe de 2 s), atacan y se pueden matar. **Fallo posible:** estado 8 sin login, errores de recurso o del CC. Decide §3.5 |
| **L3** | Gemelo en el servidor: con el campamento descartado, `KillCheatReq` (u Ctrl+Shift+F11 de `piece4_test`) a una clave **descartada** de variante con máscara 2 (`…OneHandAxe_54139`, `…OneHandBow_54121`) | Sí (servidor) | Llegan S>C `VaryHPAck` y `NotifyHandleDeadAck` de una clave que el cliente no tiene (ignorados), `LootDropCharacterRewardListAck` y **una carga de botín (tipo 7) en el punto del gemelo**. Confirma R6/M2 con A |
| **L4** | Eco completo con roles: el mismo PC como anfitrión (relevo con identidad, `EnemyAuthority`) e invitado en dry-run | No | Cada copia anunciada se enlaza en el log con "su gemelo": en eco, el propio original, con la misma identidad, distancia 0 y la misma variante. Comprueba el formato del protocolo v6 |

**Con dos PC (G1, cuando haya prueba real):** H y G en el mismo campamento.
- **Esperado:** G ve solo las copias de H; sus nativos no aparecen.
- Alejarse H 200 m → la zona se apaga (histéresis) y los nativos de G vuelven al recargar.
- Desconexión → copias retiradas y G vuelve a su mundo tras alejarse o tras una pantalla de carga.

---

## 7. Preguntas abiertas

| # | Duda | Cómo se decide |
|---|---|---|
| P1 | ¿El servidor hace algo con un NPC que ningún cliente controla durante mucho tiempo (temporizadores de inactividad)? Los motivos 30, 80 y 21 rotan identidades vivas cerca del jugador | L1/L1d: retiradas de claves descartadas mientras G sigue cerca |
| P2 | ¿Retirar a un nativo en combate deja a G "en combate" en el servidor (aggro, `ChangeBattleState`)? | L1c |
| P3 | ¿Es viable la entrega diferida (registros con campos de tiempo)? | L2 |
| P4 | ¿Algún gestor de spawn del cliente (pools, `AutoSpawnRoadReq`) vuelve a pedir lo que no ve, en bucle? Las Req de spawn son de fondo y no se pudo atribuir (`s8`) | L1/L1d con el disyuntor y contadores por identidad |
| P5 | Cambios dinámicos de relación (`UpdateFactionRelationDataAck`, conquistas de nodo): un nativo de facción "hostil" que ya no lo es en el mundo de G, o al revés | Registrar ese Ack; aprendizaje en vivo (§3.2) |
| P6 | ¿Qué cambia el motivo del `RemoveActorAck` inyectado (25 frente a 21)? Fundido, cadáver | L1b con los dos motivos |
| P7 | Armas "on demand" huérfanas tras una retirada en combate | L1c |
| P8 | Estado distinto de los nodos de facción en H y G (conquistado en uno solo) | G1 |
| P9 | Cubrir otras regiones: las capturas son de Hernand. La lista prohibida protege, pero la permitida puede quedarse corta (más nativos visibles, nunca menos seguridad) | Dry-run en otras regiones |
| P10 | Tipo 6 (lobos, osos) y jinetes: hoy H no los releva. Entran en 3g cuando lo haga 3f | 3f |

---

## 8. Plan por pasos

1. **3g-0 (sin escrituras):**
   - el predicado y las tablas (`tables_2976.txt` → `constexpr`);
   - el **dry-run** en `on_recv_packet`;
   - `EnemyAuthority` y la identidad en `EnemySpawn` (protocolo v6, con 3e).
   - Pruebas L0 y L4.
2. **3g-1 (laboratorio):**
   - el descarte (stub `ret`), el disyuntor, el barrido sin combate y los contadores;
   - el modo laboratorio con zona manual y relevo apagado.
   - Pruebas L1-L1e.
3. **3g-2:**
   - la zona real alrededor de H;
   - roles host/guest (el invitado no releva);
   - el daño espejo apagado mientras la zona esté activa.
   - Prueba G1 cuando haya dos PC.
4. **3g-3 (opcional):** la entrega diferida (L2) y R6 con `KillCheatReq` al gemelo suprimido (L3 y
   pieza 4).

Interruptores en `cdmp.ini`:

```text
[coop] suppress_natives=0|dry|1   ; por defecto dry
       suppress_radius_m=150
       suppress_node_events=0
       suppress_restore=0
       suppress_lab=0
```

---

## 9. Apéndice: direcciones (build 2976) y scripts

| RVA | Qué es |
|---|---|
| `0x35CA00` | `ClientNet::OnRecvPacket` (mid-hook del sniffer en la entrada). Llamadores `0x35BB53`, `0x35C0ED`, `0x35C951`, `0x86E55AA`; en los dos primeros no se usa `rax` tras la llamada. Salida temprana `0x35CA3C → 0x35CCCC` |
| `0xB62C70` | Handler cliente de `RemoveActorAck`: `u8 04 │ u32 X │ u8 motivo │ n×u32`. Búsqueda `0x8A67E0` (clave ausente → salta, `0xB62FD4`). Salta `+5E == 1` y al jugador local. `vf(0x160)` = enlazado, retirado con motivo 6 (`0xB6305C`) si `0x206ADA0` |
| `0x2068340` → `.xtls 0xE0D9AF0` | Baja local del actor: estados `0x20`/`0x40` → `eErrNoUserAlreadyLogouting` (`[0x6CDB934]`). Si no: `+0x98` error, `+0x9C` motivo, `+0x5C` estado previo, `vf(0x80)`, `vf(0x110)`. Sin red |
| `0x8A67E0` | Busca un actor por clave en el gestor `[[0x6D69188]+0x38]` |
| `0x29BF730` / `0x29BF3C0` | Deserializadores de servidor de `KillCheatReq` (2279, 8 B) / `VaryStatCheatReq` (2703, 15 B) |
| Mensajes | `RegionInOutReq` 3067 (8 B: actor, región u16, entra/sale); `ActorLoadDataAck` 2005; `RemoveActorAck` 2516; `UpdateFactionRelationDataAck` 2597; `OnDemandEquipDockingGimmickSpawnReq` 2249 (`04 │ u32 NPC │ …`) |
| Datos | `faction.staticinfo`: `+0x22` (tras el nombre) u16 grupo de relación, `+0x24` u16 grupo regional. `factionrelationgroup`: `0x4243` Graymane… `0x4247` Monster_Common, cada uno con `u8 │ (u32 0 │ u32 n │ n×u16) × 2` = enemigos, amigos |

Scripts en `build/research/p_3g/` (`build/research/venv/Scripts/python.exe`; los de capturas usan
`pieza4/pk.py` y su caché):

| Script | Qué hace | Salida |
|---|---|---|
| `s1_lifecycle.py <sid…>` | C>S de cada NPC, primeras Req tras la carga, mensajes tras la baja, recargas de claves vivas | `s1_out.txt` |
| `s2_regioninout.py <sid>` | Formato de `RegionInOutReq`, `SequencerTriggerVolumeInOutReq`, `GamePlayTriggerInOutReq` y `ReportFromViewerReq` | `s2_out.txt` |
| `s3_spawn_context.py <sid…>` | Contexto de las ráfagas hostiles; **distancia jugador-NPC por motivo de retirada** | `s3_out.txt` |
| `s4_hostile_origin.py all` | Todas las cargas de NPC: facción, variante, origen resuelto y si pelearon | `hostile_rows.tsv`, `s4_out.txt` |
| `s5_claims.py` | Identidad entre "mundos" (capturas): precisión de posición y variante por clase | `s5_out.txt` |
| `s6_faction_relation.py` | Grupos de relación de facción y las 55 hostiles | `faction_relation.tsv`, `s6_out.txt` |
| `s7_rule_eval.py` | Evalúa el predicado estático contra quién atacó al jugador | `s7_out.txt` |
| `s8_spawn_trigger.py all` | Req o Ack de spawn previos a las cargas hostiles (no concluyente) | `s8_out.txt` |
| `s9_groups.py all` | Orden de carga líder → miembros y retiradas de grupo | `s9_out.txt` |
| `s10_respawn.py all` | Reaparición de identidades tras la muerte o la retirada en vida | `s10_out.txt` |
| `s11_weapons.py all` | Armas "on demand" de los NPC hostiles y su retirada | `s11_out.txt` |
| (inline) → `tables_2976.txt` | Facciones hostiles, variantes excluidas, hashes permitidos y de cautela | `tables_2976.txt` |
| — | Desensamblados | `b62c70.asm`, `2068340.asm`, `x_e0d9af0.txt` |
