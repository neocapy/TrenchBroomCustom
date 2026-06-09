# Grafting an HTTP control API onto TrenchBroom

Plan and supporting research for adding a server that lets an external tool (an
Electron UI over `fetch()`) read and write the live map document while the normal
UI keeps working. This reflects the design as currently decided; superseded
alternatives have been removed. The node-identity foundation (section 7) is now
built and tested; everything else below is still design only. See "Implementation
status" near the end.

## The plan in brief

Add a server component owned by `AppController`, running Qt's `QHttpServer` on the
GUI thread with JSON request and response bodies. Each request locates the active
document (`AppController -> MapWindowManager -> topMapWindow() -> document().map()`),
opens an `mdl::Transaction`, calls the relevant `mdl::Map_*` free functions, and
commits. Those are the same functions the UI calls, so external edits are
undoable, fire the same notifiers, and repaint automatically — no parallel code
path and no renderer hook.

Object identity is a `uint64` id minted on construction and carried on every
`Node`, with a `uint64 -> Node*` index owned by `Map`. Clients hold these ids as
durable, weak-reference handles; the server resolves an id to a transient `Node*`
immediately before each operation. Bulk geometry crosses the wire as `.map` text
(the clipboard format) via `serializeSelectedNodes` and `paste`.

Decided so far:

- Transport: HTTP via `QHttpServer`, JSON bodies, loopback only. Not hand-rolled,
  not raw TCP. (Qt HTTP Server + WebSockets modules are installed.)
- Server on the GUI thread; per-request atomicity is sufficient, no session lock.
- Request/response only; no streaming/push channel.
- Scope: one document at a time. The API targets the single open document and
  returns an error when zero or more than one is open. No document id in the API.
- Identity: node-level `uint64` ids, minted on construction, session-scoped, not
  serialized; a `Map`-owned id index; resolve-just-before-mutate. Ids are global,
  so they stay unique even with several documents open.
- Geometry on the wire: JSON. Creation is constructive (primitives + CSG +
  transform + clip), so raw brush data rarely crosses the wire; `.map` text via
  `paste` / `serialize` is an optional bulk import/export escape hatch.
- Clip exposed as a new non-interactive `clipSelectedBrushes`; every other verb
  already exists as a free function.
- API surface specified in section 13. Reads are individual GET/POST routes (node
  enumeration and detail, selection, handle validation, materials, entity classes,
  raycast, point containment). All creation and mutation go through one batch
  endpoint, `POST /edit`: an array of ops (create brush/entity, paint faces,
  transform, delete, CSG, clip, select, set properties) run in order as a single
  transaction — one undo step — returning an array of results. Undo/redo/save are
  their own endpoints.

## 1. Build and module layout

- Language/build: C++20, CMake (>= 3.25), vcpkg for non-Qt dependencies. The only
  git submodule is `vcpkg`; the `lib/*` libraries are vendored in-tree and
  directly editable.
- Qt: installed via the official Qt installer at
  `/Users/isopod/Repos/Qt/6.11.1/macos` (BUILD.md targets 6.9+, later is fine).
  `find_package(Qt6 COMPONENTS Core Widgets OpenGL OpenGLWidgets Network Svg Test)`
  in `CMakeLists.txt:111`. Note: `Qt6::HttpServer` and `Qt6::WebSockets` are not
  in this install yet — they must be added (see section 10).
- Libraries under `lib/`:
  - `VmLib` (`vm::`) vector/matrix/geometry math.
  - `KdLib` (`kdl::`) utilities: `Result<>` error handling, reflection, ranges,
    `task_manager`.
  - `TbBaseLib` base types (`Color`, `Logger`, `Notifier`, `Result`).
  - `TbMdlLib` (`tb::mdl`) the document model and all editing operations. The
    important one.
  - `TbElLib` (`tb::el`) an embedded expression language (parser + runtime), used
    for entity/model definitions and config string interpolation. An expression
    evaluator, not an editor-automation host; not relevant to this work beyond
    noting it exists.
  - `TbFsLib`, `TbGlLib` (`gl::`), `TbRenderLib` (`render::`), `TbPreferencesLib`,
    `UpdateLib` (`upd::`, the auto-update HTTP client), `TbUiLib` (`tb::ui`) the
    Qt UI plus the document wrapper.
- Apps under `app/`: `TrenchBroom` (the real app, single `Main.cpp`), `CmdTool`
  and `DumpShortcuts` (small aux tools). No existing scripting, plugin, or IPC
  server subsystem; the only network code today is the auto-updater.

## 2. Application object graph and document ownership

`app/TrenchBroom/src/Main.cpp` creates a plain `QApplication`, builds one
`AppController`, then runs `app.exec()`. That loop is the GUI thread event loop
and the only thread where model and widget access is legal.

`ui::AppController` (`QObject`) is the process-wide root. It owns the
`kdl::task_manager`, the game/GL managers, the offscreen `QOpenGLContext`, the
`QNetworkAccessManager` used for updates, the `MapWindowManager`, and the
`ActionManager`. It is the natural owner for the new server, and the server needs
an `AppController&` to reach documents. `AppController` is a local `unique_ptr`
in `main`; there is no global accessor.

`ui::MapWindowManager` owns `std::vector<MapWindow*>` and exposes `topMapWindow()`
and `mapWindows()`. Each `ui::MapWindow` (`QMainWindow`) owns one
`std::unique_ptr<MapDocument>` and exposes `document()`. Multiple documents can be
open at once (except Windows, where `AppController::useSDI` is true).

Access path to the live document:

```
mdl::Map& map =
  app.mapWindowManager().topMapWindow()->document().map();   // top window may be null
```

## 3. The document: mdl::Map and the model/UI split

`mdl::Map` (`lib/TbMdlLib/include/mdl/Map.h`) is the core document state and is
UI-independent. It owns the world node tree (`WorldNode`), the asset managers, the
`EditorContext`, `Grid`, the `CommandProcessor`, the current `Selection`, the node
index, the entity link manager, and the vertex/edge/face handle managers. It also
declares the change notifiers (section 6) and the command-processing entry points
(`execute`, `executeAndStore`, `startTransaction`/`commitTransaction`/...,
`undoCommand`/`redoCommand`).

`ui::MapDocument` (`lib/TbUiLib/include/ui/MapDocument.h`) is a thin Qt-side
wrapper holding `std::unique_ptr<mdl::Map>`. It adds the renderer, autosaver,
point/portal files, a logging hub, and re-broadcasts the map's notifiers. For data
work, go straight to `doc.map()`.

Editing operations are free functions grouped into modules, each taking `Map&`:
`Map_Selection.h`, `Map_Nodes.h`, `Map_Entities.h`, `Map_Brushes.h`,
`Map_Geometry.h`, `Map_Groups.h`, `Map_Layers.h`, `Map_NodeVisibility.h`,
`Map_NodeLocking.h`, `Map_World.h`, `Map_CopyPaste.h`, `Map_Picking.h`. This set
is the API surface to expose.

## 4. The world data model (read side)

Tree rooted at `map.worldNode()`:

```
WorldNode
  LayerNode (default + custom layers)
    EntityNode    (point or brush entity; brush entities contain BrushNodes)
    BrushNode
    PatchNode     (Quake 3 bezier patch)
    GroupNode     (nestable; can be linked across copies)
```

Class hierarchy (relevant to identity in section 7): everything in the tree
derives from `Node`. `BrushNode`, `GroupNode`, `PatchNode` are
`public Node, public Object`; `EntityNode` is `public EntityNodeBase, public
Object`; `EntityNodeBase` and `LayerNode` are `public Node`; `WorldNode` is
`public EntityNodeBase` (worldspawn is itself entity-like). `Object` is a sibling
mixin (it carries `linkId()`), not an ancestor of `Node`.

`mdl::Node` provides parent/children, selection flags, visibility/lock state,
bounds, source-file line numbers, and a lambda visitor:

```cpp
node->accept(kdl::overload(
  [](WorldNode&){...}, [](LayerNode&){...}, [](GroupNode&){...},
  [](EntityNode&){...}, [](BrushNode&){...}, [](PatchNode&){...}));
```

`mdl::Entity` is a `std::vector<EntityProperty>` (key/value) plus protected-key
list and cached `classname`/`origin`/`rotation`. `mdl::Brush` holds
`std::vector<BrushFace>` plus a computed polyhedron; a `BrushFace` is three plane
points + plane + `BrushFaceAttributes` (material, UV offset/scale/rotation,
surface flags) — the literal Quake `.map` face. `mdl::Selection` exposes vectors
of selected nodes/groups/entities/brushes/patches/brushFaces.

Lookups: `Map::findNodes<T>(pattern)` (name/classname/targetname via the node
index); the new id index (section 7) for handle resolution; `mdl::MapFormat`
enumerates the supported formats.

## 5. Mutation: commands, transactions, verbs

Everything that changes the document goes through `mdl::CommandProcessor` (owned
by `Map`): `executeAndStore` for undoable commands, `execute` for non-undoable
ones. The `Map_*` functions build the commands internally. Wrap a set of
operations in `mdl::Transaction{map, "name"}` (RAII; `commit`/`cancel`/`rollback`,
nesting, collation) so they form one undo step. Rule for the API: one transaction
per `/edit` batch — the whole array of ops commits together on success, and by
default cancels (rolls back) on the first failure.

Verb catalog (all take `Map&`, most return `bool`):

- Selection: `selectAllNodes`, `selectNodes`, `selectTouchingNodes`,
  `selectContainedNodes`, `selectNodesWithFilePosition`,
  `selectBrushesWithMaterial`, `selectBrushFaces`, `invertNodeSelection`,
  `deselectAll`, `deselectNodes`, `deselectBrushFaces`.
- Nodes: `parentForNodes`, `addNodes`, `removeNodes`, `removeSelectedNodes`,
  `reparentNodes`, `duplicateSelectedNodes`, `updateNodeContents`.
- Entities: `createPointEntity`, `createBrushEntity`, `setEntityProperty`,
  `renameEntityProperty`, `removeEntityProperty`, `setEntityColorProperty`,
  `updateEntitySpawnflag`, protected/default-property helpers.
- Brushes/UV: `createBrush`, `setBrushFaceAttributes`, `translateUV`, `rotateUV`,
  `shearUV`, `flipUV`, `alignUV`, `justifyUV`, `fitUV`, `autoFitUV`.
- Geometry: `transformSelection`, `translateSelection`, `rotateSelection`,
  `scaleSelection`, `shearSelection`, `flipSelection`, vertex/edge/face transforms,
  `snapVertices`, `csgConvexMerge`, `csgSubtract`, `csgIntersect`, `csgHollow`,
  `extrudeBrushes`.
- Groups/Layers/Visibility/Locking/World/Copy-paste/Picking: as in the
  corresponding `Map_*.h` modules.

Undo/redo and history live on `Map` (`undoCommand`, `redoCommand`,
`setIsCommandCollationEnabled`, repeatable-command stack).

One gap to fill: there is no `Map_*` clip function. The primitive
`Brush::clip(worldBounds, BrushFace)` exists and is UI-independent, but the
per-selection orchestration lives in the UI tool `ClipTool`
(`ClipTool::updateBrushes` / `performClip`, `lib/TbUiLib/src/ClipTool.cpp`). A
non-interactive clip is a small new free function, for example
`clipSelectedBrushes(map, p1, p2, p3, side)`, lifted almost verbatim from
`ClipTool` and built entirely from existing public functions (`BrushFace::create`,
`Brush::clip`, `addNodes`, `removeNodes`, `Transaction`). It returns success or
failure, which is all the API needs. Primitive creation (box, cylinder, ...) is
available via `BrushBuilder` / `BasicShapes` in `mdl`.

## 6. Notifications: why external writes propagate for free

`Map` exposes `Notifier<...>` members (`nodesWereAddedNotifier`,
`nodesWereRemovedNotifier`, `nodesDidChangeNotifier`, `selectionDidChangeNotifier`,
command/transaction lifecycle notifiers, ...). `MapDocument` connects and
re-broadcasts. The renderer, map views, and inspector panels all subscribe.
Because the `Map_*` functions mutate through commands and the command processor
fires the notifiers, an API edit triggers the same refresh as a UI edit — the
server never touches the renderer. These notifiers are also what `Map` uses to
keep the id index correct (section 7).

A push/event channel streaming UI-driven edits back to clients is possible on top
of the notifiers but is out of scope: scripts run when the user is not
concurrently editing, so request/response is sufficient.

## 7. Object identity and handles

Goal: a client library holds durable references to created or queried objects, for
example `items = [createBox(...), createCylinder(...)]`, operates on them across
many requests, and sees a handle go dead when its object's identity is destroyed —
whether by the user in the UI or by another scripted operation.

Design: a `uint64` id on every node, plus a `Map`-owned index.

- Carry `uint64 m_id` on the base `Node`. Base-class placement covers brushes,
  entities, patches, groups, layers, and the world in one shot (all derive from
  `Node`; see section 4), and keeps identity off the `Brush`/`Entity` value types
  so it never touches their equality/reflection or `.map` serialization. Do not
  put it on the `Object` mixin — that would miss layers and the world.
- Mint from a process-global `std::atomic<uint64_t>` counter on construction. A
  default member initializer (`uint64_t m_id = nextNodeId();`) means every
  constructed node, including the clones produced for linked groups, gets a fresh
  unique id automatically, as long as the copy/clone constructor does not
  explicitly copy it. No "clear id on clone" hygiene step is needed. Because the
  counter is process-global, ids are unique across every open document, so a
  handle is unambiguous without a document qualifier.
- `Map` owns a `std::unordered_map<uint64_t, Node*>`, maintained at the same three
  hook points it already uses for the name index (`initializeNodeIndex`,
  `addToNodeIndex`, `removeFromNodeIndex`), plus a `findNodeById(uint64_t)`. The
  index holds only nodes currently attached to the tree.
- Not serialized. The id never reaches `.map`; loading a map mints fresh ids as
  nodes are parsed. Session-scoped by design.

Resolve-just-before-mutate. Because the server runs on the GUI thread, a handler
does `Node* n = map.findNodeById(id);` and uses `n` synchronously within the same
handler, with no event-loop yield in between, so the pointer cannot go stale
mid-request. A miss means the handle is dead — respond stale (HTTP 410).

Weak-reference semantics, including resurrect-on-undo. Many edits keep the node and
only swap its contents (`SwapNodeContentsCommand`, used by transform / property /
face-attr edits and by reparent), so handles survive them. CSG, clip, paste, and
duplicate create new nodes (new ids) and end the old identity. Deleting a node
removes it from the index, so its handle resolves to nothing; undoing the delete
re-adds the same node object (the command owns it; nothing is reconstructed), so
the same id re-registers and the handle comes back. Validity is therefore a
property that can flip false then true again across undo/redo, which the client
wrapper should expect.

Client side. The library wraps each id in a small object. With no push channel,
validity is observed lazily: an op naming a dead handle fails (its `OpResult` is
`{ ok: false, error: "stale" }`, and in the default atomic mode that fails the
whole batch), surfaced as an invalid reference. `POST /handles/validate` takes a
list of ids and returns their live/dead status for pruning a set in one call.

```
items = [map.createBox(...), map.createCylinder(...)]   # handle ids
items[0].translate([0, 0, 16])                          # 200; keeps identity
live = [h for h in items if h.valid]                    # optional batch refresh
```

Selection maps to handles. `getSelection` returns ids; `select(ids)` /
`deselect(ids)` resolve them (dropping stale ones) and call `selectNodes` /
`deselectNodes`. Since selection is the implicit operand for most verbs, the
common pattern is select-by-handle then invoke the verb, all in one transaction.

## 8. Wire format

JSON is the wire format. The client is JSON-native and should not have to speak
Quake `.map` text. JSON is already in-tree two ways: Qt `QJsonDocument` (used by
`QPreferenceStore`) and `rapidjson` (used by `UpdateLib`); Qt JSON is the path of
least resistance.

The important consequence is that geometry creation is constructive, not raw. A
client builds complex shapes by composing operations: create a primitive
(`BrushBuilder` / `BasicShapes`: box, cylinder, ...), then `transform` / CSG /
`clip`. So JSON carries only parameters (bounds, radius, matrices, clip planes),
never full brush representations, which keeps the surface small and avoids
defining a complete brush-soup schema. Where a faithful brush/face read is
genuinely needed, expose it on request: a face is three points + material + UV
attributes, mapped to JSON directly.

`.map` text stays available as an optional escape hatch for bulk import/export,
since the copy/paste module is a ready-made, UI-independent round-trip:
`serializeSelectedNodes(Map&)` writes the selection as `.map` text, and
`paste(Map&, str)` parses `.map` text and adds it through a transaction (parenting
via `parentForNodes`, selecting the result). Useful for moving a prefab in or
handing a selection to another tool, not for the common path.

## 9. Threading and the per-request UI lock

The model, command processor, notifiers, and Qt widgets are single-threaded and
must be touched only on the GUI thread (the `app.exec()` thread). `ui::isMainThread()`
exists for assertions; the established cross-thread idiom is
`QMetaObject::invokeMethod(obj, ..., Qt::QueuedConnection)`.

Run the server on the GUI thread. `QHttpServer` bound to a GUI-thread
`QTcpServer` dispatches route handlers from the GUI event loop, so handlers touch
the model directly with no marshaling. This also gives request atomicity for free:
a handler runs to completion before the event loop dispatches the next input
event, so the user cannot interleave a UI edit into the middle of a request. An
`/edit` batch is one transaction inside one handler — the whole array of ops is
atomic with respect to user input.

Boundary worth naming: this locks the UI for the duration of one handler
(milliseconds), not a whole script. Between requests the UI is live, which suits
the intended usage; per-request atomicity is sufficient, so no whole-session lock
is planned. (It could not be achieved by holding the thread anyway — the server
needs the event loop to receive the next request — but would instead be an
explicit `begin`/`end` request that disables the top window.) Costs of the
GUI-thread model: a slow handler also freezes
rendering (negligible for clip/property edits/normal pastes, a possible hitch for
a huge serialize), and atomicity holds only if handlers avoid nested event loops
or modal dialogs — another reason to drive everything through `mdl::Map_*` rather
than the `MapWindow` methods, some of which pop dialogs.

If handler latency ever becomes a problem, move only socket I/O and JSON
(de)serialization to a worker thread and marshal the model op to the GUI thread
with `Qt::BlockingQueuedConnection`. The edit still runs on the GUI thread, and
you lose the free per-request lock, so only do this if measurements demand it.

## 10. HTTP transport: QHttpServer

Decided: use Qt's `QHttpServer` rather than hand-rolling HTTP or using raw TCP.
It provides routing (`server.route("/path", handler)`), request objects, and JSON
response helpers, and avoids owning request parsing and partial-read buffering. A
raw-TCP/JSON protocol was rejected because `fetch()` only speaks HTTP; raw framing
would force a raw-socket client instead of `fetch()` in the Electron UI.

What this requires (the one dependency action):

- `QtHttpServer` and `QtWebSockets` are not in the current Qt 6.11.1 install. Add
  the "Qt HTTP Server" module via the Qt Maintenance Tool (it pulls in Qt
  WebSockets, which its CMake config depends on). No Qt rebuild.
- Add `HttpServer` to the `find_package(Qt6 COMPONENTS ...)` line in the root
  `CMakeLists.txt`, and link `Qt6::HttpServer` to whichever target holds the
  server (section 12).
- CI: the workflow's Qt install step must include the HttpServer module, or CI
  builds will fail to configure. Same for any other contributor.

Operational notes:

- Bind to `127.0.0.1` only; an editing endpoint is effectively remote control of
  the map. Consider a shared-secret token in a header.
- Electron `fetch('http://127.0.0.1:port')` from a renderer triggers a CORS
  preflight that the page CSP may block. Either call from the Electron main
  process (Node, no CORS) or answer `OPTIONS` and send
  `Access-Control-Allow-Origin: *`; loopback-only makes permissive CORS benign.
- Fallback only if you decide against touching the Qt install/CI: a minimal
  hand-rolled HTTP/1.1 layer over `QTcpServer` (read to `\r\n\r\n`, parse headers,
  read `Content-Length` body, reply with `Content-Length` + `Connection: close`).
  Small because you control the client, but it is the thing `QHttpServer` exists
  to spare you.

## 11. Where to graft the server

Primary: a new server component owned by `AppController`, created in
`AppController::create()` (or just after it in `main`), holding `AppController&` to
reach the active document and driving it through `mdl::Map` and the `Map_*`
functions inside transactions. Decoupled from any particular window, fully reuses
the UI's data path.

Optional complement: the action surface. `ui::ActionManager::actionsMap()` returns
every menu/shortcut action keyed by a preference path; `Action::execute(
ActionExecutionContext{appController, topMapWindow, currentMapViewBase})` runs one,
guarded by `enabled()`. This gives a near-free "invoke any menu command by id"
endpoint, but it is UI-context dependent (many actions need a live `MapViewBase`).
Expose it only if you want menu-command parity in addition to the data API.

## 12. Build wiring

- Put the server sources in `TbUiLib`. It already links `Qt6::Network` PUBLIC and
  has `AUTOMOC TRUE` (`lib/TbUiLib/CMakeLists.txt`), so a `Q_OBJECT` server class
  compiles with moc and reaches the model. Add the new files to `target_sources`
  and the header `FILE_SET`, and add `Qt6::HttpServer` to its `target_link_libraries`.
  (A separate `TbApiLib` is possible but unnecessary.)
- Add `HttpServer` to the root `find_package(Qt6 COMPONENTS ...)`.
- The `uint64` id and the id index are model changes in `TbMdlLib`: a field +
  initializer on `Node`, a counter, the `unordered_map` maintained in `Map`'s
  three index hook points, and `findNodeById`. No new dependency, no command or
  serialization changes.

## 13. API reference (v1)

All routes are HTTP on `127.0.0.1:<port>` with JSON bodies. `GET /documents` lists
every open document with its handle; every other route targets one document through
a required `doc` handle — a `?doc=<handle>` query item on GET, a `"doc": <handle>`
field in the POST body. Omitting `doc` is 409; an unknown handle is 404. All
mutation goes through one batch endpoint (`POST /edit`) that runs its whole array of
ops as a single transaction; reads are individual routes. Routes with no body are
GET; anything taking a body is POST.

### Shared types

```
Vec3    = [number, number, number]          // world units, x y z
Bounds  = { min: Vec3, max: Vec3 }
Handle  = number                            // uint64 id for a node or layer; documents
                                            // have their own handles (see GET /documents).
                                            // Counters start low, within JS safe-integer range
Ref         = string                        // "@name": refers to an earlier op's result within one /edit batch
HandleOrRef = Handle | Ref                  // a real handle (number), or a batch-local "@name" (string)
FaceRef     = { brush: HandleOrRef, face: number }   // positional face index; brush is a real Handle in responses

NodeType = "world" | "layer" | "group" | "entity" | "brush" | "patch"

NodeSummary = {
  handle: Handle, type: NodeType, bounds: Bounds,
  parent?: Handle, layer?: Handle,
  selected: boolean, visible: boolean, locked: boolean,
  classname?: string,        // entities
  material?: string          // brushes; present only if all faces share one material
}

BrushFaceInfo = {
  index: number, points: [Vec3, Vec3, Vec3], normal: Vec3,
  material: string, offset: [number, number], scale: [number, number], rotation: number
}

NodeDetail = NodeSummary & {
  faces?: BrushFaceInfo[],                          // brushes
  origin?: Vec3, pointEntity?: boolean,             // entities
  properties?: { [key: string]: string }, children?: Handle[]
}

RayHit = {
  point: Vec3, distance: number, handle: Handle,   // the node that was hit
  normal?: Vec3,             // face normal, when a brush face was hit
  face?: number,             // face index, when a brush face was hit
  material?: string          // material of that face
}

FaceAttributes = {           // any subset; omitted fields are left unchanged
  material?: string, xOffset?: number, yOffset?: number,
  rotation?: number, xScale?: number, yScale?: number
}

Transform =                  // exactly one of:
    { translate: Vec3 }
  | { rotate: { center: Vec3, axis: Vec3, angle: number } }   // angle in radians
  | { scale:  { center: Vec3, factors: Vec3 } }
  | { matrix: number[] }     // 16 numbers, row-major 4x4
```

Errors: non-2xx with `{ error: string }`. 409 = required `doc` selector missing;
404 = unknown `doc` handle; 410 = a referenced handle is stale; 400 = malformed
request; 422 = the operation ran but failed (reason in `error`).

### Read routes

```
GET  /documents                              // [built] every open document
  -> Document[]   // { handle, active, name, path|null, mapFormat, worldBounds: Bounds,
                  //   modified, layers: { handle, name }[], nodeCount }

GET  /nodes?doc=<handle>&type=<NodeType>     // [built] type optional; whole-tree enumeration
  -> { nodes: { handle: Handle, type: NodeType }[] }

POST /nodes/get                              // [built]
  req { doc: Handle, handles: Handle[], detail?: "summary" | "full" }   // default "summary"
  -> { nodes: (NodeSummary | NodeDetail)[], missing: Handle[] }   // stale handles in `missing`

GET  /selection?doc=<handle>                 // [built]
  -> { nodes: Handle[], faces: FaceRef[] }

POST /handles/validate                       // [built]
  req { doc: Handle, handles: Handle[] }
  -> { valid: Handle[], invalid: Handle[] }

GET  /materials?doc=<handle>                 // [built]
  -> { collections: { name: string, materials: string[] }[] }

GET  /entityclasses?doc=<handle>             // [built]
  -> { classes: { classname: string, type: "point" | "brush",
                  description?: string, bounds?: Bounds }[] }   // bounds: point classes only

POST /raycast                                // [built]
  req { doc: Handle, rays: { origin: Vec3, direction: Vec3, maxDistance?: number,
                ignore?: Handle[] }[] }
  -> { results: RayHit[][] }                // one array per ray, same order; all hits
                                            // along the ray, front to back; [] = no hit.
                                            // Entry faces only: the picker backface-culls,
                                            // so a ray reports where it enters geometry,
                                            // never where it exits.

POST /contains                               // [built]
  req { doc: Handle, points: Vec3[] }
  -> { results: { point: Vec3, handles: Handle[] }[] }   // nodes containing each point
```

### Mutation: POST /edit

All creation and mutation go through one endpoint. The request body is an array
of operations; they run in order as a single transaction, so the whole array is
one undo step. The response is an array of results, one per op, in the same order.

```
POST /edit                      // [built] atomic: any failure rolls back the whole batch
POST /edit?onError=continue     // skip failures, commit the survivors (still one undo step)
  req  { doc: Handle, ops: Op[] }
  ->   OpResult[]               // one per op, same order
```

Atomic by default: on the first failing op the transaction is cancelled and
nothing is applied; the reply is HTTP 422 whose body is still the results array
(the failed op carries `error`, later ops are `skipped`) so you can see what went
wrong, but every handle in that body is void. With `onError=continue` the reply
is 200, each op reports its own `ok`, and the transaction commits with whatever
succeeded.

An op may carry `as: "<name>"` to label its result; a later op in the same batch
then passes `"@name"` anywhere a handle is accepted. References are backward-only.
Real handles are numbers and refs are strings, so the two never collide. This is
what lets one batch build a compound object — create brushes, then wrap them in an
entity — and still be a single undo step. A ref bound to a handle list (from `csg`
or `clip`) is accepted where a single handle is required iff the list has exactly
one element — the common case after a `convexMerge` or a one-sided `clip`.

```
Op = { op: "select",    nodes?: HandleOrRef[], faces?: FaceRef[],
                         mode?: "replace"|"add"|"remove" }          // default replace
   | { op: "brush",      shape: { box: Bounds }
                              | { cylinder: { bounds: Bounds, axis: "x"|"y"|"z", sides: number } }
                              | { points: Vec3[] },                 // convex hull of the points
                         material?: string, layer?: HandleOrRef }
   | { op: "entity",     classname: string, position?: Vec3,
                         properties?: { [key: string]: string },
                         brushes?: HandleOrRef[] }                  // brush entity: wrap these
   | { op: "setProps",   handle: HandleOrRef,
                         set?: { [key: string]: string }, remove?: string[] }
   | { op: "paint",      faces: FaceRef[], attributes: FaceAttributes }
   | { op: "transform",  handles?: HandleOrRef[], transform: Transform }   // default selection
   | { op: "delete",     handles: HandleOrRef[] }
   | { op: "csg",        operation: "subtract"|"convexMerge"|"intersect"|"hollow",
                         handles?: HandleOrRef[] }
   | { op: "clip",       handles?: HandleOrRef[],
                         plane: { points: [Vec3, Vec3, Vec3] } | { point: Vec3, normal: Vec3 },
                         keep: "front"|"back"|"both" }
// every variant also accepts an optional  as: string

OpResult = { ok: true,  handle?: Handle, handles?: Handle[] }       // handle(s) for create / csg / clip
         | { ok: false, error: string, skipped?: true }
```

`as` binds an op's primary result: the new handle for `brush` / `entity`, or the
resulting handle list for `csg` / `clip`. Referencing a list result where a single
handle is required is an error. A `handles?` left out means the current selection,
reflecting any selection changes earlier ops in the same batch have made.

### History and IO (separate endpoints)

These act on the command history or the filesystem, not on document content, so
they are not `/edit` ops:

```
POST /undo  -> { ok: boolean, name?: string }    // [built] all three; req { doc: Handle }
POST /redo  -> { ok: boolean, name?: string }
POST /save  -> { ok: boolean, path: string }     // 422 if the document was never saved
```

Notes:

- Paint-what-you-hit: a `/raycast` result carries `{ handle, face }`, fed straight
  into a `paint` op. A face index is positional within a brush and stays valid only
  until that brush's geometry changes (clip, CSG, vertex edits), so resolve faces
  by raycast or `/nodes/get` immediately before the batch that paints them. Within
  one batch, a `paint` on a brush an earlier op created is reliable only for
  predictable faces (a fresh box); for faces of geometry a prior op reshaped, read
  back in a second round trip.
- New brushes get `material` (or the current material) on every face; repaint
  individual faces with a `paint` op.
- Handles are uint64. The counter starts at 1 (0 is reserved as a null handle), so
  values stay well within the JS safe-integer range; switch to string ids only if
  that ever changes.

## 14. Gotchas and risks

- Thread safety is the dominant constraint. All model and widget access on the GUI
  thread; assert with `ui::isMainThread()`.
- Handle resolution. Resolve ids to `Node*` at the start of each handler and use
  synchronously; never persist a raw `Node*` across requests.
- Notifier reentrancy. Do not start new commands from inside a notifier callback on
  the request path; finish the transaction, then respond.
- Command collation. Similar commands within about a second can collate into one
  undo step; `setIsCommandCollationEnabled(false)` around `/edit` gives
  deterministic one-undo-step-per-batch granularity, so two batches sent close
  together stay separately undoable.
- Modal tools. Reject edits while a modal tool is mid-drag
  (`MapWindow::anyModalToolActive`).
- Multiple documents / no document. `topMapWindow()` can be null (welcome window
  only); handle it.
- Paste structure. `paste` will not place a `WorldNode`/`LayerNode` as a child;
  send brush/entity/group content.
- Performance/security. Large serialize/paste is O(map) and briefly blocks the UI;
  keep the endpoint loopback-only and consider a token.

## 15. Key files

- `lib/TbMdlLib/include/mdl/Map.h` — core document, notifiers, command entry, index
  hook points.
- `lib/TbMdlLib/include/mdl/Map_*.h` — the editing verbs.
- `lib/TbMdlLib/include/mdl/{Node,Object,EntityNodeBase}.h` — node hierarchy for
  the id field.
- `lib/TbMdlLib/include/mdl/{CommandProcessor,Transaction}.h` — undo system.
- `lib/TbMdlLib/include/mdl/Map_CopyPaste.h` (+ `.cpp`) — `.map` text I/O.
- `lib/TbMdlLib/include/mdl/{Entity,Brush,BrushFace,Selection}.h` — data model.
- `lib/TbMdlLib/src/ClipTool.cpp` — clip logic to lift into `clipSelectedBrushes`.
- `lib/TbUiLib/include/ui/{AppController,MapWindowManager,MapWindow,MapDocument}.h`
  — ownership chain.
- `lib/TbUiLib/CMakeLists.txt`, `CMakeLists.txt` — build wiring (add HttpServer).

## Implementation status

Built and verified: the entire v1 surface of section 13. Node identity (section 7),
document identity, the server (sections 10-11), all read routes, the `POST /edit`
batch executor with every op, and `/undo`, `/redo`, `/save`.

The work lives on branch `http-control-api` (pushed to `origin`): node identity +
server scaffold; document handle + `GET /documents`; the document and scene-graph
read routes; the palette and spatial read routes; the mutation surface.

- `mdl::Node` carries a `std::uint64_t` id minted on construction from a
  process-global atomic (`mdl::nextNodeId()`; starts at 1, with 0 reserved as a
  null handle). Clones get fresh ids for free: `clone()` builds through the normal
  constructors and `cloneAttributes` does not copy the id, so there is no
  clone-hygiene step.
- `mdl::NodeIndex` keeps a `uint64 -> Node*` map beside its string trie, so the id
  index rides the three existing index hooks (covering the world, layers, and every
  descendant, with the right add / remove / change / undo timing).
  `Map::findNodeById` exposes the lookup.
- `MapWindow::debugPrintNodeTree` logs the indented node tree with ids, wired to a
  "Debug > Print Node Tree to Console" menu item next to "Print Vertices". It lives
  in the `#ifndef NDEBUG` debug menu; the current build has `NDEBUG` undefined, so
  the item is live.
- Tests: `NodeTest.id` and `Map_findNodeById`. The full TbMdlLib suite passes
  (74170 assertions across 195 cases), and the whole app builds, links, and bundles.
- Each open document carries a stable `std::uint64_t` handle (`ui::nextDocumentId()`,
  its own atomic). It lives on `ui::MapDocument`, which outlives the `mdl::Map` it
  wraps, so the handle survives reload while the map's node ids are reminted. The
  Debug node-tree dump leads with it.
- `ui::ApiServer` (a `QObject` owned by `AppController`) binds a `QHttpServer` to a
  loopback-only `QTcpServer` on `127.0.0.1:28196`, built against `Qt6::HttpServer`.
  It serves `GET /documents` (every open document) and the per-document reads
  `GET /nodes`, `POST /nodes/get`, `GET /selection`, and `POST /handles/validate`.
  Every per-document route takes a required `doc` handle (query item on GET, body
  field on POST); a shared resolver returns 409 if it is missing, 400 if malformed,
  404 if it names no open document. Node serialization (summary / full) covers world,
  layer, group, entity, brush, and patch. Smoke-tested offscreen across every
  resolver error path; the data paths verified by hand against an open map.

The remaining read routes (commit 4):

- `GET /materials` and `GET /entityclasses` enumerate `map.materialManager().
  collections()` and `map.entityDefinitionManager().definitions()` directly.
  Entity classes carry `description` (free and useful for an assist UI) and
  `bounds` for point classes.
- `POST /contains` maps onto `mdl::findNodesContaining`.
- `POST /raycast` uses `mdl::pick` (`PickResult::byDistance`), which routes
  through `EditorContext` — the design fork resolved as: inherit the UI's hover
  semantics, so hidden geometry is not hit. Hits are filtered to
  `mdl::nodeHitType()` (entity | brush | patch) plus the request's `ignore` set
  and `maxDistance`. Each ray returns all hits front to back (`PickResult` keeps
  hits distance-sorted on insertion). Entry hits only: `BrushFace::
  intersectWithRay` backface-culls, so exit faces would need model changes —
  deliberately skipped.
- Verified by hand against a live map with known geometry (a box brush and an
  info_player_start): contains inside/outside, raycast hit ordering past the
  entity bbox onto the brush top face with exact distances and normals,
  maxDistance cutoff, ignore filtering, and every 400/404/409 error path.
  `/materials` returned the correct empty list for a wad-less map; not yet
  exercised against a document with loaded material collections.

The mutation surface (commit 5):

- `POST /edit` runs `{ doc, ops }` (the spec's `req Op[]` plus the required `doc`
  selector) inside one `mdl::Transaction` named "API Edit", with command collation
  disabled around the batch so consecutive batches stay separately undoable, and a
  409 guard when a modal tool is mid-drag. Atomic mode cancels on first failure and
  replies 422 with the results array (later ops marked `skipped`);
  `onError=continue` commits the survivors and replies 200.
- Implementation choices per op: `brush` builds through `BrushBuilder` (cuboid /
  edge-aligned cylinder / convex hull of points) with the game's default face
  attributes and adds via `addNodes`, so the new node's handle is returned directly.
  `entity` constructs the `Entity` by hand (classname, `origin` from `position`,
  properties, plus definition defaults when the game config asks for them), adds it,
  and reparents the given brushes into it. `setProps` and `paint` avoid touching the
  selection entirely: `setProps` swaps entity contents via `updateNodeContents` (and
  works on the world node for worldspawn properties); `paint` uses `applyAndSwap`
  over explicit `BrushFaceHandle`s with `UpdateBrushFaceAttributes`. `transform`,
  `csg`, and `clip` are selection verbs in the model, so an explicit `handles` list
  means "replace the selection with these, then act" — the selection change is
  observable, by design. `delete` takes explicit nodes, deselects them and their
  selected descendants/faces first (the remove command does not), and refuses the
  world and layers.
- `csg subtract` follows the editor's semantics: the operand brushes are the
  *subtrahend*, carved out of every brush they touch, and are themselves removed.
- `clip` maps to the new `mdl::clipSelectedBrushes(map, p1, p2, p3, keepFront,
  keepBack)` in `Map_Geometry`, lifted from `ClipTool` (including its best-matching-
  face attribute copy). "front" is the side of the plane normal: for three points,
  `cross(p2-p1, p3-p1)`; for `{point, normal}` the server builds tangents so the
  given normal is "front". Covered by new `Map_Geometry` unit tests (front / back /
  both / fully-discarded / undo-restores-selection).
- Found and fixed an upstream copy-paste bug while testing: `Map::canRedoCommand()`
  checked `undoCommandName()` instead of `redoCommandName()`, so redo always
  reported unavailable right after an undo. Regression test added in `tst_Map.cpp`.
- Also fixed on the read side: world-node detail now carries worldspawn's
  `classname` and `properties` (the serializer previously treated `WorldNode` as
  having neither).
- Verified end to end against a live map: compound batches (create boxes, convex-
  merge via refs, paint the result, rotate it — one undo step, exact expected
  bounds), clip front/back/both with exact bounds, subtract fragments, entity
  wrapping + setProps + worldspawn props, atomic rollback (422, nothing applied),
  continue mode (200, survivors committed), skipped markers, stale-handle and
  malformed-op errors, undo/redo handle resurrection, and `/save` round-tripping
  everything to disk. The full TbMdlLib suite passes (74196 assertions, 195 cases).

## Open questions and deferrals

The use case is settled: semi-interactive procedural editing assist — select or
draw a region, raycast against existing geometry, emit brushes/entities, paint
faces. The v1 surface is section 13. The rest are deferrals, not blockers:

- Batching is now the core mutation model (`POST /edit`), not a deferral: one array
  of ops, one transaction, one undo step. Read-side fan-out (arrays of rays or
  points) stays on the individual read routes.
- Action-invocation endpoint (any menu command by id): cut from v1.
- Multi-document: done. Each open document has its own handle (`ui::MapDocument`
  id), `GET /documents` lists them (active first), and every route resolves a
  required `doc` handle. Node ids stay per-`Map`, so a node handle is only
  meaningful together with its `doc`.
- `/csg` and `/clip` return the resulting selection; if a later script needs the
  exact added/removed handle sets, that is a small extension.
