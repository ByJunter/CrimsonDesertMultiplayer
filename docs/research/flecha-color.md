# Flecha del compañero en otro color (rojo, azul o verde)

Build **1.0.0.2976** (PE `0x6AB28F00`, base `0x140000000`). Las direcciones son RVA (`exe+…`).
Es un análisis de solo lectura: capstone sobre el exe en disco y lectura de los PAZ del juego.
El juego no se ejecutó y no se escribió nada en `D:\SteamLibrary`.

Marcas:
- **[verificado]**: comprobado en el desensamblado o en los archivos de datos extraídos.
- **[inferido]**: deducido; hay que confirmarlo en vivo.
- **[no encontrado]**: se buscó y no apareció.

Scripts y salidas en `build/research/arrow/` (fuera del repo, `build/` está en `.gitignore`):

| Archivo | Qué hace |
|---|---|
| `index.py` → `index.tsv` | Índice de los 2 052 523 ficheros de todos los PAMT |
| `ext.py <patrón>` | Extrae, descifra y descomprime entradas a `out/` |
| `uimt.py` → `uimt_list.txt` | Lista de `UIMapTextureInfo`: índice, clave, offset y nombre |
| `uimt_resolved.txt` | Lo mismo, con el componente de UI de cada entrada resuelto por hash |
| `evmap2.py`, `evusers.py`, `strrva.py` | Tabla de eventos de UI, quién los lanza, RVA de cadenas |
| `out/ui/` | Los 370 `.css`, `.html` y `.thtml` de la UI, ya descifrados |
| `out/arrow.png`, `out/arrow_tints.png` | La flecha original y una vista previa teñida |

Usa los lectores de CrimsonForge (MIT), clonados en `build/research/arrow/crimsonforge`, con `lz4` y
`cryptography` instalados en `build/research/arrow/deps`. El venv compartido no se tocó.

---

## 0. Resumen y recomendación

1. **La "clave" que devuelve el hook es un índice de la tabla estática `UIMapTextureInfo`**,
   con 2027 entradas. No es una clave arbitraria **[verificado]**:
   - el índice 0 es `Actor_Focus` y el 23 es `Actor_Focus_InBush`;
   - coincide con lo que se vio en vivo: 0 da la flecha amarilla y 23 la flecha con contorno.
2. **El color de la flecha no está en la textura ni en la tabla: es una clase CSS.**
   - La textura `cd_icon_map_player` es una flecha **gris neutra** (media RGB 194,194,194).
   - La plantilla `MapIcon_ActorFocus` la tiñe con la clase `common-color-orange-01`
     (`background-color: #ffb74d; background-blend-mode: multiply`) **[verificado]**.
   - Las clases `common-color-red-01`, `-green-01` y `-blue-01` ya existen en `ui/cdcommon.css`
     **[verificado]**.
   - **No hacen falta texturas nuevas.**
3. **Sin tocar archivos no hay una flecha de otro color.**
   - Ninguna de las 2027 entradas usa la flecha con otro color **[verificado]**.
   - Los eventos del minimapa no llevan color: solo opacidad (`float`) **[verificado]**.
   - Cambiar la clase CSS del icono en memoria es posible en teoría, pero no encontré la API del
     motor de UI **[no encontrado]**.
   - Sin archivos, lo único que da otro color es **otro icono**: por ejemplo, el índice **102**
     (`Actor_Character`), un punto cian `#74d4e5` que no gira (§3.3).
4. **Recomendación: un mod de datos mínimo, solo texto y sin binarios.**
   - Se editan las plantillas `ui/minimapicon.thtml` y `ui/worldmapicon.thtml`, más dos líneas
     de CSS.
   - Se reutilizan tres entradas que la partida en solitario no debería usar: **105**
     `Actor_Player_Other`, **104** `Actor_Party_Default` y **111** `TestCharacter`.
   - Cada una se convierte en una flecha que gira, en rojo, azul o verde.
   - El hook ya existente solo cambia el valor devuelto: 105, 104 o 111 en vez de 0.
   - Se instala como **overlay PAZ** con un gestor de mods (CDUMM o DMM); no se toca ningún
     archivo original.
   - Si el amigo no instala el overlay, verá un punto gris: **el fallo es suave**.
   - La receta exacta está en §5.

| Opción | Archivos | Esfuerzo | Riesgo | Resultado |
|---|---|---|---|---|
| A. Índice 102 en el hook (ya) | No | Minutos | Muy bajo | Punto cian sin rotación |
| B. Flecha amarilla + subicono | No | Bajo | Bajo, sin probar | Flecha amarilla con adorno **[inferido]** |
| **C. Overlay con 2 thtml + 2 css** | Sí, overlay | 1–2 h | Bajo | **Flecha roja, azul o verde que gira** |
| D. Cambiar la clase CSS en memoria | No | Alto (RE en vivo) | Medio | Flecha de cualquier color |
| E. Servir el thtml desde el .asi (hook de lectura de archivos) | No en disco | Alto | Medio | Como C, sin instalar overlay |

---

## 1. De dónde sale el icono

### 1.1 `UIMapTextureInfo`: la tabla completa de iconos

- Archivos: `gamedata/uimaptextureinfo.staticinfoheader` y `.staticinfobody`, en el grupo
  `0008/0.paz` **[verificado]**.
  - Cabecera: `u16 count` (= `0x07EB` = 2027) y luego `count × {u32 key, u32 offsetEnBody}`.
  - Cuerpo: cada registro empieza con `u32 key | u32 len | name[len]` y sigue con los campos.
- El `u16` que maneja el juego (`StaticInfoWrapper<UIMapTextureKey, UIMapTextureInfo,
  UIMapTextureInfoManager, unsigned short>`, en el RTTI) es el **índice de la cabecera**, no la
  `key` (`5001`, `5068`…) **[verificado por coincidencia]**:
  - el 0 es `Actor_Focus` y el 23 es `Actor_Focus_InBush`, igual que en la prueba en vivo.
- Campos de la clase (cadenas de reflexión junto a `UIMapTextureInfo` en el exe) **[verificado]**:
  - `_uiTextureName`, `_uiSmallTextureName`, `_uiFilterGroupComponentName`,
    `_uiFilterTextureName`;
  - `_mapIconType`, `_uiMapLayerType`, `_zIndex`, `_tooltipText`, `_isActorType`;
  - `_isAlwayShowMinimap`, `_minimapForceUpdateIcon`, `_isPerspectiveIcon`, `_useChangeScale`,
    `_minScale`, `_maxScale`, `_isSimpleMaterial`, `_filterType`…
  - **Ninguno es de color.**
- Los campos `*Name` se guardan como hash: `hashlittle(nombre, 0xC5EDE)`, el mismo hash que
  usan los PAZ **[verificado]**. Por ejemplo, `0x3A33C566` = `"MapIcon_ActorFocus"`.
- `_uiTextureName` es el **nombre del componente** de la plantilla de UI que se instancia.

Entradas de interés (`uimt_resolved.txt`):

| Índice | Nombre | Componente | Cómo se ve (CSS) |
|---|---|---|---|
| 0 | `Actor_Focus` | `MapIcon_ActorFocus` | Flecha `cd_icon_map_player`, **naranja `#ffb74d`**, gira |
| 23 | `Actor_Focus_InBush` | `MapIcon_ActorBushFocus` | Flecha con contorno `cd_icon_map_player_bush`, sin tinte, gira |
| 4, 5, 6, 7 | `Actor_Player_Kliff`, `_Damian`, `_Oongka`, `_BlackWolf` | `MapIcon_ActorCharacter` + textura pequeña `cd_Icon_map_quest_Character_*` | Punto cian. **Los usa el juego** para los personajes jugables **[inferido]** |
| 102 | `Actor_Character` | `MapIcon_ActorCharacter` | Punto `cd_icon_map_npc`, **cian `#74d4e5`**, no gira |
| 104 | `Actor_Party_Default` | `MapIcon_ActorPartyDefault` | Punto gris `#f2efe7` al 50 % (en el mapa del mundo, al 100 % y gira) |
| 105 | `Actor_Player_Other` | `MapIcon_ActorPlayerOther` | Punto gris `#f2efe7`, opacidad 0,5, no gira |
| 110 | `Actor_Friend` | `MapIcon_ActorFriend` | `cd_icon_map_npc_friend`, blanco |
| 111 | `TestCharacter` | `MapIcon_ActorTestChr` | `cd_Icon_map_TestChr` gris (icono de prueba) |

**No existe ninguna flecha de color aparte de la propia** **[verificado]**:
- `cd_icon_map_player` solo aparece en `.icon-image-player` (minimapa) y en
  `.worldmap-common-focus-actor` / `.default-focus-actor` (mapa del mundo);
- solo la usa `MapIcon_ActorFocus`.

### 1.2 Plantillas y CSS (grupo `0012/4.paz`, cifrados con ChaCha20 y comprimidos con LZ4)

| Archivo | Tamaño descomprimido | Contenido |
|---|---|---|
| `ui/minimapicon.thtml` | 254 578 B | ~700 `<component name="MapIcon_…">` del minimapa |
| `ui/minimapicon.css` | 100 043 B | `.icon-image-*` (`.icon-image-player` en la línea 221) |
| `ui/worldmapicon.thtml` | 353 870 B | Los mismos componentes, versión del mapa del mundo |
| `ui/worldmapicon.css` | 236 122 B | `.worldmap-common-focus-actor`, `.party-default-actor`… |
| `ui/cdcommon.css` | 64 242 B | `.common-color-*` (líneas 202–226) |
| `ui/cd_icon_map_00.xml` | | `<Texture Name="cd_icon_map_player" Filename="UI/texture/cd_icon_map_00.dds" GetRect="341,1,100,100"/>` |
| `ui/cd_icon_map_00.dds` (`0012/3.paz`) | 1 MB, DXT5 1024² | Atlas de iconos del mapa |

La flecha propia en el minimapa (`minimapicon.thtml:1911`) **[verificado]**:

```html
<component name="MapIcon_ActorFocus" css="minimap-icon-container !cpp-none" script="UIGamePlayControlCommon_MiniMapIcon" selector-change-color-icon="#BaseIcon" selector-show-layer-list=".minimap-icon-container">
    <div id="BaseIcon" css="base-icon icon-image-player common-color-orange-01 cpp-rotate-character-angle cpp-non-scaling-icon icon-player-order" script="UIGamePlayControlCommonDefault"></div>
</component>
```

- **La rotación también la decide el CSS**: la clase `cpp-rotate-character-angle` hace que el
  script C++ gire el elemento con el yaw de `UpdateMinimapActor` **[inferido]**.
- La flecha del arbusto (23), que no es el tipo de mapa 5 de `Actor_Focus`, también gira con esa
  clase. Por eso **no depende del tipo de icono** **[inferido]**.
- La vista previa del tinte multiplicativo está en `out/arrow_tints.png`: naranja original, rojo
  `#d14d45`, verde `#31ba7e`, azul `#25459c` (demasiado oscuro sobre el minimapa) y un azul claro
  propuesto, `#5aa9ff`.

---

## 2. Motor de UI

- **No es Coherent Gameface ni un navegador** **[verificado]**:
  - no hay `cohtml`, `Gameface`, V8, QuickJS ni `addEventListener` en el exe;
  - en `bin64` no hay ninguna DLL de UI.
- Es el motor propio de Pearl Abyss, embebido en el exe (cadenas `PAUI`):
  - usa `.html`, `.thtml` (plantillas con `<component>`/`<widget>`/`<override>`) y `.css`
    propios, cifrados en el PAZ;
  - **no hay JavaScript**: el atributo `script="…"` nombra una **clase C++**, por ejemplo
    `pa::uiCommonScript::UIGamePlayControlCommon_MiniMapIcon` (vtable `0x56744E0`, 167 slots;
    bases `…_MapIcon`, `UIBaseScriptObject`, `IUIScriptObject`).
- **Propiedades CSS que entiende** (tabla junto a `CssPropertyType`) **[verificado]**:
  - `background-color`, `background-blend-mode`, `opacity`;
  - `filter-saturate`, `filter-grayscale`, `filter-contrast`, `filter-brightness`,
    `filter-invert`, `filter-blur`, `filter-pa-blur`;
  - `material-name`, `material-param`, `rotate-z`, `scale-*`…
  - **No hay `hue-rotate`**: el tinte se hace siempre con `background-color` + `multiply`.
- Las clases `cpp-*` las ponen y quitan los scripts C++ en tiempo de ejecución, por ejemplo
  `cpp-show`/`cpp-hide`/`cpp-disable` y `.icon-image-horse.friendly-hired.cpp-disable`. Así que
  el motor **sí** cambia clases de un elemento en vivo.
- `selector-change-color-icon` aparece en 733 componentes. Es un selector que el script usa para
  algún cambio de color o estado. No encontré quién lo activa **[no encontrado]**:
  - el id de cadena (`exe+0x6CD58B0`) solo lo usa su inicializador en el código no mutado.

---

## 3. Opción 1: en tiempo de ejecución, sin tocar archivos

### 3.1 Eventos del minimapa: formatos

Los eventos son `UIEventWrap<…>`, con los tipos en el RTTI, y salen de `[[exe+0x6D69190]+0x80]+off`.
- Cada objeto de evento tiene un despacho inmediato, `byte[q+0x1A]`, con oyentes en `q+0x40`.
- Si no, una cola con lock (`exe+0x6D76338`).

Los formatos salen de quien lanza cada evento **[verificado]**, salvo los marcados.
`Tex` = `StaticInfoWrapper<UIMapTextureKey,…,u16>`: el índice de §1.1.

| Off | Evento | Función que lo lanza (RCX=cola) | Argumentos |
|---|---|---|---|
| `+0x620` | `CreateMinimapActor` | `0x5C2010` (desde `0x9EB097`/`0x9EB357`) | `u16 Tex`, `u32 ActorKey`, `staticstringA& nombre`, `bool`, `bool`, `u32 QuestDialogKey = 0`. Plantilla `<Tex, ActorKey, staticstringA&, bool, bool, QuestDialogKey>`. Los dos `bool` son los bytes `a`/`b` del cálculo (`[rsp+0x42]`, `[rsp+0x43]`) **[inferido]** |
| `+0x628` | `HideFlagMinimapActor` | `0x3D2460` (desde `0x9EA00D`) | `u32 ActorKey` |
| `+0x630` | `RemoveMinimapActorByActorKey` | `0x4317A0` | `u16 Tex`, `u32 ActorKey` |
| `+0x638` | `ChangeMinimapActor` | `0x9EC330` | `u16 TexNuevo`, `u16 TexViejo`, `u32 ActorKey`, `staticstringA&`, `bool`, `bool`, `bool` |
| `+0x640` | `UpdateMinimapActor` | `0x9EC150` (tick `0x9E7DBE`) | `u16 Tex`, `u32 ActorKey`, `float3& pos`, `float& yaw` |
| `+0x648` | `UpdateMinimapActorAdditionalIcon` | `0x9EBF90` (`0x9EB551`) | `u16 Tex`, `u16 sub`, `u16 sub2`, `u32 ActorKey` (`sub`/`sub2` = nuevo y anterior **[inferido]**) |
| `+0x658` | `UpdateMinimapActorIconOpacity` | sin llamador en el código no mutado **[no encontrado]** | Plantilla `<Tex, ActorKey, const float&>`: **solo opacidad** |
| `+0x670` | `UpdateMinimapActorDuplcateTargetIcon` | `0x9EBDE0` (tick `0x9E7DF8`, si `comp+0x36 & 1`) | `u32 ActorKey`, `u32 ActorKeyObjetivo`, `bool = 1` |

**Ningún evento lleva color, clase ni filtro** **[verificado]**:
- ningún `UIEventWrap` con `UIMapIconKey` o `ActorKey` tiene un tipo `Color`;
- el único parámetro visual es la opacidad.

"Duplicate target" duplica el icono de **otro actor** (el objetivo); no es una variante de color
**[inferido]**.

Notas:
- `ChangeMinimapActor` (cambiar la clave en caliente) hace desaparecer el icono, como se vio en
  vivo. Tiene sentido: la UI tendría que destruir un componente e instanciar otro de otra plantilla.
- Recrear el actor sigue siendo el camino fiable para cambiar de icono.

### 3.2 Cambiar la clase CSS en memoria (opción D)

La idea:
1. Tras el `CreateMinimapActor` de `0xA010F001`, localizar la instancia del componente.
2. En su `#BaseIcon`, **quitar** `common-color-orange-01` y **añadir** `common-color-red-01`.

**Hay que quitar la clase naranja** **[inferido]**:
- red, green y blue están definidas **antes** que orange en `cdcommon.css`;
- con la misma especificidad gana la última, así que añadir rojo sin quitar naranja seguiría en
  naranja.

Lo que falta **[no encontrado]**:
- la función del motor que añade o quita una clase a un elemento. Los ids de clase se internan
  con `exe+0x12BECB0(out, const char*, 1, 0x2FFFF)`;
- cómo pasar de `ActorKey` al widget en `UIGamePlayControlRootMiniMap` (vtable `0x5674E00`;
  `vf171` = `0xE09FC0` crea el widget, y las llamadas `0x1435C9320(elem, &u8)` parecen
  conmutar estados del elemento).

Haría falta depurar en vivo (x64dbg/CE) con un breakpoint en `0xE09FC0`. Hay que repetirlo en el
mapa del mundo (`UIGamePlayControlCommon_WorldMapIcon`) y en cada recreación de la UI.

**No lo recomiendo como primer paso.**

### 3.3 Otras claves sin tocar archivos (opción A)

- El hook puede devolver **cualquier índice < 2027**, no solo los de `cfg+0xEA…0x16C`.
- La única alternativa de color real sin archivos es un punto:
  - **102** `Actor_Character`: cian `#74d4e5`, el "aliado", que no gira;
  - o 110 `Actor_Friend`.
- Con 105 se ve un punto gris semitransparente: es lo que verá el amigo si no instala el overlay.
- **Validar el rango**: `StaticInfoWrapper` indexa un array, así que un índice ≥ 2027
  probablemente lee fuera de la tabla **[inferido]**. En el hook, aceptar solo una lista blanca:
  `{0, 23, 102, 104, 105, 110, 111}`.

### 3.4 Subicono (opción B, no probada)

- El 3.<sup>er</sup> argumento del cálculo (`*sub`) produce `UpdateMinimapActorAdditionalIcon(Tex,
  sub, …)`.
- Se podría dejar la flecha amarilla (0) y añadir un subicono de otro color como marca.
- No sé cómo se dibuja `sub` en la plantilla, ni si todas las plantillas lo admiten
  **[inferido]**. Se prueba con el hook: `*icon = 0; *sub = 102`.

---

## 4. Opción 2: archivos de datos

### 4.1 Dónde están y cómo se empaquetan

- Iconos: el atlas `ui/cd_icon_map_00.dds` (`0012/3.paz`) y el registro de recortes
  `ui/cd_icon_map_00.xml` (`0012`).
- Definiciones:
  - la tabla `gamedata/uimaptextureinfo.staticinfo*` (`0008`);
  - las plantillas y CSS de §1.2 (`0012/4.paz`).
- Formato PAZ/PAMT (lo implementan CrimsonForge, pycrimson y el unpacker de lazorr410):
  - `meta/0.papgt` es la raíz y guarda el CRC de cada `NNNN/0.pamt`;
  - cada PAMT lista entradas `{offset, comp_size, orig_size, flags}` y el checksum de cada PAZ;
  - `.css`, `.html`, `.thtml` y `.xml` van cifrados con ChaCha20, con la clave derivada del nombre
    de archivo con `hashlittle(…, 0xC5EDE)`;
  - la compresión es LZ4 (tipo 2 en `flags>>16 & 0xF`);
  - la `.dds` lleva cabecera y cuerpo LZ4.
- **El juego comprueba la integridad** de PAPGT y PAMT:
  - el exe contiene `packMeta Crc`, `. file(%u) papgt(%u) pamt(%u)` y `' was corrupted.`
    **[verificado, la cadena]**;
  - un PAZ editado sin recalcular los CRC no carga **[inferido]**;
  - las herramientas de abajo los recalculan.

### 4.2 Herramientas de la comunidad (existen y se usan)

| Herramienta | Licencia | Qué hace |
|---|---|---|
| [CrimsonForge](https://github.com/hzeemr/crimsonforge) (Nexus 446) | MIT | Explorador y editor (incluye CSS/HTML), repack completo con PAMT y PAPGT |
| [pycrimson](https://github.com/LukeFZ/pycrimson) | MIT | Librería Python: PAZ/PAMT, DDS, saves |
| [crimson-desert-unpacker](https://github.com/lazorr410/crimson-desert-unpacker) | MIT | Unpack/repack en C++ y Python |
| [CDUMM](https://github.com/faisalkindi/CrimsonDesert-UltimateModsManager) ([Nexus 207](https://www.nexusmods.com/crimsondesert/mods/207)) | | **Overlay PAZ**: escribe un grupo nuevo y registra su PAMT en PAPGT, sin tocar los originales; recuperación tras parches |
| [Definitive Mod Manager](https://www.nexusmods.com/crimsondesert/mods/633), [JSON Mod Manager](https://www.nexusmods.com/crimsondesert/mods/113) | | Overlays, reemplazo de archivos, parches JSON |

Ya hay mods publicados que editan **exactamente** el CSS y el HTML del minimapa:
[Enhanced Mini Map Icons V3](https://www.nexusmods.com/crimsondesert/mods/3628),
[Enhanced Map Icons](https://www.nexusmods.com/crimsondesert/mods/855),
[Minimap Tweaks](https://www.nexusmods.com/crimsondesert/mods/275) y
[Classic HUD](https://www.nexusmods.com/crimsondesert/mods/2609).

Por eso el motor acepta plantillas y CSS modificados **[inferido por terceros, no probado aquí]**.

### 4.3 Riesgos

- **Integridad**: con un gestor de overlay no se modifica ningún archivo original y los CRC se
  recalculan. Steam "verificar archivos" restaura `meta/0.papgt` y desactiva el overlay; no rompe
  nada.
- **Slots reservados**: CDUMM avisa de que los grupos `0036–0040` están reservados en
  `0.papgt` para packs de idioma. Un overlay en esos números no se monta. Usar el gestor, que lo
  tiene en cuenta.
- **Parches del juego**:
  - un parche reescribe `0.papgt`, y el overlay queda desregistrado hasta reaplicarlo;
  - mientras tanto, el compañero se ve como punto gris (105) o con el icono de prueba (111). Es un
    fallo suave;
  - **riesgo real**: si PA cambia `minimapicon.thtml` y se reaplica un overlay **viejo**, el
    archivo entero viejo pisa al nuevo y podría romper iconos nuevos. Hay que regenerar el parche
    por build, como ya se hace con las firmas.
- **Conflictos**: otros mods de iconos del minimapa reemplazan los mismos dos `.thtml`. Solo
  puede ganar uno, salvo que se fusionen los cambios.
- **Reparto al amigo**:
  - lo mejor es distribuir un **script o parche** (diff de texto) que genere los archivos desde
    su propia copia del juego, no los `.thtml` de PA. Así no se redistribuyen 600 KB de archivos
    del juego y el parche se adapta a su build;
  - si no lo instala, la flecha se ve como punto gris; no hay crash.
- **Crashes**: un `.thtml` mal formado podría colgar la UI, y el crash se envía a PA. Hay que
  editar con un parser o reemplazo exacto, validar el XML antes de empaquetar y probar sin el hook
  activo.
- **Reutilizar entradas (104/105/111)**:
  - si el juego usara alguna para otro actor, ese actor saldría como flecha de color;
  - 105 ("otro jugador") y 111 ("TestCharacter") no tienen sentido en una partida en solitario.
    104 ("party") es la más dudosa;
  - antes de nada, la prueba P-A (§6) registra qué índices produce el juego por sí solo;
  - las tres tienen `_isActorType` y el mismo bloque de flags que la flecha del arbusto, salvo el
    último bool. Las dos primeras tienen el mismo `_uiMapLayerType` (07) que `Actor_Focus`; la 111
    tiene 01 y podría filtrarse distinto en el mapa del mundo **[inferido]**.

---

## 5. Receta recomendada (opción C)

### 5.1 Datos (overlay)

Hay que modificar 4 archivos del grupo `0012`; no hacen falta texturas ni tocar la tabla binaria.

**`ui/minimapicon.css`**: añadir al **final**, para que gane por orden:

```css
/* cdmp coop */
.coop-arrow-red   { background-image: textureid(cd_icon_map_player); background-color: #e0433a; background-blend-mode: multiply; }
.coop-arrow-blue  { background-image: textureid(cd_icon_map_player); background-color: #5aa9ff; background-blend-mode: multiply; }
.coop-arrow-green { background-image: textureid(cd_icon_map_player); background-color: #31ba7e; background-blend-mode: multiply; }
```

Se podrían usar `common-color-red-01`/`green-01`/`blue-01` de `cdcommon.css` y no tocar los
`.css`. Pero el azul original (`#25459c`) queda muy oscuro, y no está claro qué hoja carga
`cdcommon.css` en esta vista **[inferido]**. Con clases propias se controla el color exacto.

**`ui/minimapicon.thtml`**: sustituir el `<div id="BaseIcon">` de tres componentes. Se mantiene
la etiqueta `<component>` tal cual.

| Componente (línea) | `css` nuevo de `#BaseIcon` |
|---|---|
| `MapIcon_ActorPlayerOther` (1917) | `base-icon coop-arrow-red cpp-rotate-character-angle cpp-non-scaling-icon icon-player-order` |
| `MapIcon_ActorPartyDefault` (1967) | `base-icon coop-arrow-blue cpp-rotate-character-angle cpp-non-scaling-icon icon-player-order` |
| `MapIcon_ActorTestChr` (1850) | `base-icon coop-arrow-green cpp-rotate-character-angle cpp-non-scaling-icon icon-player-order` |

En `MapIcon_ActorTestChr`, borrar también el `<div id="StickyIcon">` y la clase
`cpp-change-color-icon`. Así no se reemplaza el color en algún estado.

**`ui/worldmapicon.css`**: al final.

```css
.coop-arrow-world { position: absolute; width: 32px; height: 32px; transform: translate(-50%, -50%); }
/* y las tres .coop-arrow-red/blue/green de arriba */
```

**`ui/worldmapicon.thtml`**: el `#BaseIcon` de `MapIcon_ActorPlayerOther` (1633),
`MapIcon_ActorPartyDefault` (2120) y `MapIcon_ActorTestChr` (2426) pasa a ser:

```
no-pickable coop-arrow-world coop-arrow-<color> cpp-rotate-character-angle cpp-checked-layer
```

- Sin la animación `fliker-player-icon`, para no confundirla con la propia.
- Si se quiere que parpadee, añadir `worldmap-common-focus-actor`.

Empaquetado:
- un overlay con CDUMM o DMM (carpeta de mod con los 4 archivos en `ui/`), o el repack de
  CrimsonForge;
- **antes**: copia de `save` a `backups/`, como dicen las reglas del proyecto (CONTRIBUTING.md). Aunque esto no escribe en
  memoria, la sesión de prueba sí.

### 5.2 Mod (hook existente en `exe+0x9E89C0`)

- Solo cambia el valor de `g_minimap_override_key`: `[coop] minimap_color = red|blue|green|yellow`
  se traduce a `105 | 104 | 111 | 0`.
- Lista blanca de valores: `{0, 23, 102, 104, 105, 110, 111}`.
- Comprobar la build (PE `0x6AB28F00`) antes de usarlos: **los índices son de esta build**. Si PA
  inserta entradas en la tabla, se desplazan.
- Mejor aún, resolverlos al arrancar por nombre o hash del componente:
  - recorrer la tabla en memoria, si se localiza el `UIMapTextureInfoManager` **[no encontrado]**;
  - o fijarlos por build en `offsets.h`.
- Para cambiar de color en caliente, **recrear el actor**: despawn y respawn, o `0x50A1B0(actor)`
  desde el hilo del juego. **No** usar `ChangeMinimapActor`, que lo hace desaparecer.
- El color puede viajar en el protocolo para que cada PC pinte al otro como este elija. Es un
  campo nuevo, así que habría que **subir `kVersion`**. O basta un ajuste local en `cdmp.ini`.

---

## 6. Pruebas en vivo propuestas

| # | Prueba | Cómo | Qué mirar |
|---|---|---|---|
| P-A | ¿El juego usa 104, 105 o 111? | Modo diagnóstico: llamar al original para **todos** los actores y registrar el conjunto de `*icon` distintos durante 10 min de juego (ciudad, combate, caballo, cambio de personaje si se puede) | Si 104/105/111 no aparecen nunca, se pueden reutilizar. Si aparece 104, usar otra entrada |
| P-B | Índice fuera de `cfg`, sin archivos | Hook con 105 y luego 102 | 105: punto gris semitransparente. 102: punto cian. Confirma que el índice es de la tabla completa |
| P-C | Subicono | `*icon = 0; *sub = 102` | ¿Aparece un adorno junto a la flecha? |
| P-D | Overlay | Instalar el overlay de §5.1 y poner el hook en 105, 104 y 111 | Flecha roja, azul o verde que gira con la marioneta en el minimapa y el mapa (M). Captura con `CopyFromScreen` |
| P-E | Fallo suave | Desinstalar el overlay con el hook en 105 | Punto gris; sin crash |

---

## 7. Referencias rápidas (build 2976)

| Qué | Dónde |
|---|---|
| Tabla de iconos | `0008/0.paz`: `gamedata/uimaptextureinfo.staticinfoheader` (16 218 B) y `.staticinfobody` (394 174 B, LZ4); 2027 entradas |
| Hash de nombres | `hashlittle(nombre, 0xC5EDE)` (PAZ, componentes y texturas) |
| Plantillas | `0012/4.paz`: `ui/minimapicon.thtml`/`.css`, `ui/worldmapicon.thtml`/`.css`, `ui/cdcommon.css` |
| Flecha | `ui/cd_icon_map_00.dds` (`0012/3.paz`), rect `341,1,100,100`, gris |
| Lanzadores de eventos | Create `0x5C2010`, Change `0x9EC330`, Remove `0x4317A0`, Update `0x9EC150`, AdditionalIcon `0x9EBF90`, DuplicateTarget `0x9EBDE0`, Hide `0x3D2460` |
| Script C++ del icono | `UIGamePlayControlCommon_MiniMapIcon` vtable `0x56744E0`; `UIGamePlayControlRootMiniMap::vf171` `0xE09FC0` |
| Interning de ids de cadena de la UI | `0x12BECB0(out, str, 1, 0x2FFFF)`; `selector-change-color-icon` → `exe+0x6CD58B0` |
| Tabla de eventos (nombres) | `0x7C7240` (offset → nombre, `evmap2.py`) |
