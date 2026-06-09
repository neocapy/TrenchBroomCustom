# trenchbroom-client

Typed, blocking Python client for the TrenchBroom HTTP control API
(this fork's `/documents`, `/nodes`, `/edit`, … routes — see
`HTTP_API.txt` at the repo root for the wire-level reference).

- Python ≥ 3.10, **zero dependencies** (stdlib `urllib` transport)
- Fully typed (`py.typed`), frozen dataclasses for all response shapes
- Blocking calls; errors raise exceptions

## Installation

From another uv project:

```bash
uv add --editable /path/to/TrenchBroomCustom/http-clients/python/trenchbroom-client
```

This records a path source in your `pyproject.toml`; `--editable` means
changes in this repo are picked up without reinstalling. Plain pip works
too: `pip install -e <same path>`.

## Quick start

```python
from trenchbroom_client import TrenchBroomClient

tb = TrenchBroomClient()              # http://127.0.0.1:28196
doc = tb.active_document()            # raises if nothing is open

with doc.edit() as e:                 # one atomic batch = one undo step
    floor = e.brush_box(([0, 0, -16], [512, 512, 0]), material="base/floor")
    pillar = e.brush_cylinder(([224, 224, 0], [288, 288, 128]), sides=12)
    e.entity("light", position=[256, 256, 96], properties={"intensity": "300"})

print(floor.handle)                   # real server handle, valid after commit
doc.save()
```

## Core type aliases

```python
Vec3 = tuple[float, float, float]     # returned vectors
Vec2 = tuple[float, float]
VecLike = Sequence[float]             # accepted vectors: list, tuple, numpy array...
Handle = int                          # uint64 node/document id
NodeType = Literal["world", "layer", "group", "entity", "brush", "patch"]
HandleLike = Handle | Ref             # Ref only valid inside the same EditBatch
FaceRefLike = FaceSel | tuple[HandleLike, int]   # (brush, positional face index)
BoundsLike = Bounds | tuple[VecLike, VecLike]    # (min, max) shorthand accepted
```

No vector class is imposed: every parameter typed `VecLike` accepts any
indexable sequence of 3 floats, and all returned vectors are plain
`Vec3` tuples. For arbitrary transforms, `transform_matrix` takes 16
numbers, row-major 4×4 — with numpy: `e.transform_matrix(m.flatten())`.

## Response dataclasses

All frozen; optional fields are `None` when the server omits them.

```python
@dataclass(frozen=True)
class Bounds:
    min: Vec3
    max: Vec3
    # Bounds.of(min: VecLike, max: VecLike) -> Bounds

@dataclass(frozen=True)
class LayerInfo:
    handle: Handle
    name: str

@dataclass(frozen=True)
class DocumentInfo:
    handle: Handle
    active: bool
    name: str
    path: str | None                  # None if never saved
    map_format: str
    world_bounds: Bounds
    modified: bool
    layers: tuple[LayerInfo, ...]
    node_count: int

@dataclass(frozen=True)
class NodeRef:
    handle: Handle
    type: NodeType

@dataclass(frozen=True)
class NodeSummary:
    handle: Handle
    type: NodeType
    bounds: Bounds
    selected: bool
    visible: bool
    locked: bool
    parent: Handle | None             # None on world
    layer: Handle | None              # containing layer; None on world/layer
    classname: str | None             # world + entity
    material: str | None              # brush, only if all faces share one

@dataclass(frozen=True)
class NodeDetail(NodeSummary):        # from get_full() / node()
    properties: dict[str, str] | None # world + entity
    origin: Vec3 | None               # entity
    point_entity: bool | None         # entity
    faces: tuple[BrushFaceInfo, ...] | None   # brush
    children: tuple[Handle, ...] | None       # any node with children

@dataclass(frozen=True)
class BrushFaceInfo:
    index: int
    points: tuple[Vec3, Vec3, Vec3]
    normal: Vec3
    material: str
    offset: Vec2
    scale: Vec2
    rotation: float

@dataclass(frozen=True)
class FaceSel:
    brush: Handle
    face: int                         # positional face index

@dataclass(frozen=True)
class Selection:
    nodes: tuple[Handle, ...]
    faces: tuple[FaceSel, ...]

@dataclass(frozen=True)
class RayHit:
    point: Vec3
    distance: float
    handle: Handle
    normal: Vec3 | None               # present iff a brush face was hit
    face: int | None                  # ditto
    material: str | None              # ditto

@dataclass(frozen=True)
class MaterialCollection:
    name: str
    materials: tuple[str, ...]

@dataclass(frozen=True)
class EntityClass:
    classname: str
    type: Literal["point", "brush"]
    description: str | None
    bounds: Bounds | None             # point classes only

@dataclass(frozen=True)
class OpResult:                       # one per op in an /edit batch
    ok: bool
    handle: Handle | None             # brush, entity
    handles: tuple[Handle, ...] | None  # csg, clip
    error: str | None
    skipped: bool                     # not run: an earlier op failed (atomic mode)

@dataclass(frozen=True)
class HistoryResult:
    ok: bool                          # False = nothing to undo/redo
    name: str | None                  # undo-step name
```

## Connecting

```python
class TrenchBroomClient:
    def __init__(self, base_url: str = "http://127.0.0.1:28196",
                 timeout: float = 30.0) -> None
    def documents(self) -> list[Document]        # all open docs, active first
    def active_document(self) -> Document        # raises TrenchBroomError if none
```

A `Document` wraps one document handle and carries it into every call.
`doc.info: DocumentInfo` is a snapshot from creation time;
`doc.refresh() -> DocumentInfo` re-fetches it (modified flag, layers,
node count). `doc.handle: Handle` is the raw id.

## Reading

```python
class Document:
    def nodes(self, type: NodeType | None = None) -> list[NodeRef]
        # every node handle in the document (whole tree), optionally filtered

    def get(self, handles: Iterable[Handle]) -> list[NodeSummary]
        # stale handles silently dropped; use validate() to know which

    def get_full(self, handles: Iterable[Handle]) -> list[NodeDetail]

    def node(self, handle: Handle) -> NodeDetail
        # raises TrenchBroomError if the handle is stale

    def selection(self) -> Selection

    def validate(self, handles: Iterable[Handle]) -> tuple[list[Handle], list[Handle]]
        # -> (valid, invalid)

    def materials(self) -> list[MaterialCollection]

    def entity_classes(self) -> list[EntityClass]

    def contains(self, points: Sequence[VecLike]) -> list[list[Handle]]
        # results[i] = handles of nodes containing points[i]

    def raycast(self, origin: VecLike, direction: VecLike,
                max_distance: float | None = None,
                ignore: Iterable[Handle] | None = None) -> list[RayHit]
        # ALL hits of one ray, sorted near -> far; [] = miss.
        # direction may be any nonzero length.

    def raycast_many(self, rays: Sequence[tuple[VecLike, VecLike]],
                     max_distance: float | None = None,
                     ignore: Iterable[Handle] | None = None) -> list[list[RayHit]]
        # rays are (origin, direction); results[i] corresponds to rays[i]
```

Raycasts hit entry faces of visible entity/brush/patch nodes only. A
`RayHit` carries `normal`/`face`/`material` iff a brush face was hit.

## Editing

```python
def edit(self, on_error: Literal["fail", "continue"] = "fail") -> EditBatch
```

`EditBatch` queues ops and sends them in one request — one server
round-trip and **one undo step**. As a context manager it commits on
clean exit (and discards if your code raises); or call
`batch.commit() -> list[OpResult]` explicitly.

Every op method returns a `Ref`:

```python
class Ref:
    result: OpResult                  # raises TrenchBroomError before commit
    handle: Handle                    # the single handle produced (brush, entity)
    handles: tuple[Handle, ...]       # all handles produced (csg, clip)
```

Within the same batch, a `Ref` is accepted anywhere a `HandleLike` is —
it serializes as a backward reference, so you can create a brush and
carve with it in one atomic batch. After commit it resolves to the real
server handles.

```python
with doc.edit() as e:
    room = e.brush_box(([0, 0, 0], [512, 512, 256]), material="base/wall")
    cut  = e.brush_box(([64, 64, 32], [448, 448, 224]))
    e.csg_subtract([cut])             # hollow the room; cut is consumed
```

### Op methods

All return `Ref`. `as_` overrides the auto-generated wire ref name
(cosmetic; useful when debugging server logs).

```python
class EditBatch:
    def select(self, nodes: Iterable[HandleLike] | None = None,
               faces: Iterable[FaceRefLike] | None = None,
               mode: Literal["replace", "add", "remove"] = "replace") -> Ref

    def brush_box(self, bounds: BoundsLike, material: str | None = None,
                  layer: HandleLike | None = None, as_: str | None = None) -> Ref
        # material default: current material; layer default: current layer
        # layer may also be a group handle (parents the brush there)

    def brush_cylinder(self, bounds: BoundsLike,
                       axis: Literal["x", "y", "z"] = "z", sides: int = 16,
                       material: str | None = None,
                       layer: HandleLike | None = None,
                       as_: str | None = None) -> Ref
        # sides: 3..256

    def brush_points(self, points: Sequence[VecLike],
                     material: str | None = None,
                     layer: HandleLike | None = None,
                     as_: str | None = None) -> Ref
        # convex hull of >= 4 points

    def entity(self, classname: str, position: VecLike | None = None,
               properties: dict[str, str] | None = None,
               brushes: Iterable[HandleLike] | None = None,
               as_: str | None = None) -> Ref
        # position sets the "origin" property; pass brushes to create a
        # brush entity wrapping them

    def set_props(self, handle: HandleLike,
                  set: dict[str, str] | None = None,
                  remove: Iterable[str] | None = None) -> Ref
        # handle: entity node, or the world handle for worldspawn

    def paint(self, faces: Iterable[FaceRefLike],     # non-empty
              material: str | None = None,
              x_offset: float | None = None, y_offset: float | None = None,
              rotation: float | None = None,
              x_scale: float | None = None, y_scale: float | None = None) -> Ref
        # None = leave that attribute unchanged

    def translate(self, offset: VecLike,
                  handles: Iterable[HandleLike] | None = None) -> Ref

    def rotate(self, center: VecLike, axis: VecLike, angle: float,
               handles: Iterable[HandleLike] | None = None,
               degrees: bool = False) -> Ref
        # angle in radians unless degrees=True

    def scale(self, center: VecLike, factors: VecLike,
              handles: Iterable[HandleLike] | None = None) -> Ref

    def transform_matrix(self, matrix: Sequence[float],
                         handles: Iterable[HandleLike] | None = None) -> Ref
        # 16 numbers, row-major 4x4

    def delete(self, handles: Iterable[HandleLike]) -> Ref
        # world/layer handles -> error

    def csg(self, operation: Literal["subtract", "convexMerge",
                                     "intersect", "hollow"],
            handles: Iterable[HandleLike] | None = None,
            as_: str | None = None) -> Ref
    # convenience wrappers, same handles/as_ parameters:
    def csg_subtract(self, cutters=None, as_=None) -> Ref
    def csg_convex_merge(self, handles=None, as_=None) -> Ref
    def csg_intersect(self, handles=None, as_=None) -> Ref
    def csg_hollow(self, handles=None, as_=None) -> Ref

    def clip(self, point: VecLike | None = None,
             normal: VecLike | None = None,
             points: Sequence[VecLike] | None = None,   # exactly 3
             keep: Literal["front", "back", "both"] = "front",
             handles: Iterable[HandleLike] | None = None,
             as_: str | None = None) -> Ref
        # give point+normal OR points; "front" keeps the side the normal
        # points into; for 3 points the normal is cross(p2-p1, p3-p1).
        # Brushes wholly on a discarded side are removed.

    def commit(self) -> list[OpResult]
    results: list[OpResult] | None    # set after commit
```

**Selection semantics:** ops with an optional `handles` parameter act
on the current selection when it's omitted. Supplying `handles` to
`translate`/`rotate`/`scale`/`csg_*`/`clip` *replaces* the selection
with those nodes (an observable side effect). Ops see selection changes
made by earlier ops in the same batch.

### Error modes

Atomic (default): the first failing op rolls back the **entire** batch;
the server returns 422 and the client raises `EditFailed`, whose
`.results: list[OpResult]` gives per-op outcomes (every handle in them
is void — the transaction was rolled back).

`doc.edit(on_error="continue")`: failed ops are skipped, surviving ops
commit, no exception — inspect `batch.results` / each `Ref.result`.

## History / IO

```python
def undo(self) -> HistoryResult       # ok=False: nothing to undo
def redo(self) -> HistoryResult
def save(self) -> str                 # saved path; raises ApiError (422)
                                      # if never saved to disk
```

## Exceptions

```python
TrenchBroomError(Exception)           # base for everything below
├── ConnectionFailed                  # server unreachable
└── ApiError                          # non-2xx response
    │   .status: int                  # 400 malformed; 404 unknown doc;
    │                                 # 409 doc missing / modal tool active;
    │                                 # 422 operation ran but failed
    │   .message: str                 # server's error string
    │   .body: object                 # parsed JSON body, if any
    └── EditFailed                    # atomic /edit rollback (422)
            .results: list[OpResult]
```

## Handle lifetime gotchas

These come from the server, not the client — see "Semantics that bite"
in `HTTP_API.txt`:

- Node handles are **session-scoped**; reloading a map remints them
  (document handles survive). Never persist them.
- `delete` kills a handle; undoing the delete resurrects the *same*
  handle. `csg`/`clip` produce *new* handles and kill their inputs;
  `translate`/`set_props`/`paint` keep handles.
- Face indices are positional per brush and are invalidated by any
  geometry change (clip, csg, vertex edit). Re-read the brush
  (`doc.get_full`) or raycast before painting reshaped geometry; fresh
  boxes are predictable.
- `csg_subtract`'s arguments are the **subtrahend**: they are carved
  out of every brush they touch, then deleted. To cut a hole, pass only
  the cutter brush(es).
